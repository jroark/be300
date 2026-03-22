#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include "machine.h"
#include "wince_diag.h"
#include "wince_init.h"
#include "probes.h"
#include "mem_alias.h"
#include "tlb_shadow.h"
#include "null_call.h"
#include "bus.h"
#include "loader.h"
#include "macc.h"
#include "ui.h"

/* mips_sext, is_kseg_va32 moved to machine.h */
/* is_wince_boot_cfg / is_wince_boot_machine are in machine.h */

/* CP0 Cause ExcCode values (also defined before Unicorn hooks for local use) */
#define MIPS_EXCCODE_TLBL 2u
#define MIPS_EXCCODE_TLBS 3u
#define MIPS_EXCCODE_SYS  8u

#define VR4131_PCLK_HZ      UINT64_C(166000000)
#define VR4131_CP0_COUNT_HZ (VR4131_PCLK_HZ / 2u)
#define VR4131_RTC_HZ       UINT64_C(32768)

static inline uint32_t machine_cp0_count32(const machine_t *m)
{
    return m->cp0_count_base + (uint32_t)(m->cp0_count_ticks / 2u);
}

static void machine_advance_rtc_from_cp0(machine_t *m)
{
    uint32_t now = machine_cp0_count32(m);
    uint32_t prev = (uint32_t)m->rtc_last_count_tick;
    uint32_t delta = now - prev; /* wraps naturally as Count is 32-bit */

    m->rtc_last_count_tick = now;
    if (delta == 0u)
        return;

    m->rtc_tick_frac_num += (uint64_t)delta * VR4131_RTC_HZ;
    uint64_t rtc_ticks = m->rtc_tick_frac_num / VR4131_CP0_COUNT_HZ;
    m->rtc_tick_frac_num %= VR4131_CP0_COUNT_HZ;
    if (rtc_ticks != 0u)
        rtc_tick(&m->rtc, rtc_ticks);
}

bool read_guest_u32(uc_engine *uc, uint64_t va, uint32_t *out);
static uc_err write_mem_best_effort(uc_engine *uc, uint64_t address,
                                    const void *data, size_t size);

/* sdram_alias_pa_offset moved to mem_alias.c */

static const char *cp0_reg_name(uint32_t rd, uint32_t sel)
{
    if (sel != 0)
        return NULL;
    switch (rd) {
    case 0:  return "Index";
    case 2:  return "EntryLo0";
    case 3:  return "EntryLo1";
    case 4:  return "Context";
    case 5:  return "PageMask";
    case 8:  return "BadVAddr";
    case 10: return "EntryHi";
    case 13: return "Cause";
    case 14: return "EPC";
    default: return NULL;
    }
}

static bool cp0_is_tlb_diag_reg(uint32_t rd, uint32_t sel)
{
    if (sel != 0)
        return false;
    switch (rd) {
    case 0:   /* Index */
    case 2:   /* EntryLo0 */
    case 3:   /* EntryLo1 */
    case 4:   /* Context */
    case 5:   /* PageMask */
    case 8:   /* BadVAddr */
    case 10:  /* EntryHi */
        return true;
    default:
        return false;
    }
}

/* cp0_shadow_write moved to tlb_shadow.c */

static inline uint32_t wince_reverse_entrylo_pfn_fixup(uint32_t canon)
{
    uint32_t canon_pfn = (canon >> 6) & 0xFFFFFu;
    uint32_t flags = canon & 0x3Fu;
    uint32_t raw_pfn = canon_pfn << 2;
    return (raw_pfn << 6) | flags;
}



static inline bool execve_pc_in_do_execve(uint64_t pc)
{
    uint32_t p = (uint32_t)pc;
    return (p >= DO_EXECVE_START_PC && p < DO_EXECVE_END_PC);
}

static inline bool is_init_execve_epc(uint32_t epc)
{
    switch (epc) {
    case 0x8000181Cu: /* /linuxrc from cmdline */
    case 0x8000184Cu: /* /sbin/init */
    case 0x8000186Cu: /* /etc/init */
    case 0x8000188Cu: /* /bin/init */
    case 0x800018ACu: /* /bin/sh */
        return true;
    default:
        return false;
    }
}

static inline bool is_run_init_syscall_epc(uint32_t pc)
{
    return (pc == RUN_INIT_SYSCALL_EPC || pc == RUN_INIT_SYSCALL_EPC_26);
}

static inline bool is_run_init_syscall_ret_pc(uint32_t pc)
{
    return (pc == RUN_INIT_SYSCALL_RET_PC || pc == RUN_INIT_SYSCALL_RET_PC_26);
}

static inline bool in_init_execve_syscall_window(const machine_t *m)
{
    return (m->pending_excode == 8u &&
            m->pending_syscall_nr == 4011u &&
            is_init_execve_epc((uint32_t)m->pending_syscall_epc));
}

/* in_user_handoff_entry_window moved to machine.h */

static inline bool is_handoff_stale_pc(uint32_t pc, uint32_t handoff_pc)
{
    return (pc == 0x80001850u ||
            pc == 0x80000180u ||
            (handoff_pc != 0u && pc == handoff_pc));
}

bool read_insn_best_effort(uc_engine *uc, uint64_t address, uint32_t *insn);

static inline bool trace_user_handoff_fault_path_active(const machine_t *m)
{
    if (!m->cfg.trace_user_handoff)
        return false;
    if (!m->execve_user_handoff_active ||
        m->execve_user_handoff_state != EXECVE_HANDOFF_STATE_ARMED)
        return false;
    return ((uint32_t)m->shadow_cp0_badvaddr < 0x80000000u);
}

/* tlb_pair_bytes_from_pagemask, tlb_leaf_bytes_from_pagemask,
 * tlb_entry_matches_va moved to machine.h */

static void reset_tlb_defer_state(machine_t *m)
{
    m->tlb_defer_count = 0;
    m->tlb_defer_active = false;
    m->tlb_defer_owner_epc = 0;
    m->tlb_exl_drop_defer_count = 0;
}

static void clear_synthetic_syscall_state(machine_t *m, bool clear_execve_watch)
{
    if (clear_execve_watch && m->execve_watch_active &&
        m->pending_syscall_nr == 4011u) {
        static uint32_t clear_execve_log = 0;
        if (clear_execve_log < 32) {
            uint64_t caller_pc = 0;
            uc_reg_read(m->uc, UC_MIPS_REG_PC, &caller_pc);
            fprintf(stderr,
                    "[CLEAR_EXECVE_STATE] PC=0x%08" PRIX64
                    " pend_exc=%u pend_epc=0x%08" PRIX64
                    " nr=%u epc_wr=%u has_saved=%u\n",
                    (uint64_t)(uint32_t)caller_pc,
                    m->pending_excode,
                    (uint64_t)(uint32_t)m->pending_epc,
                    m->pending_syscall_nr,
                    m->epc_was_written ? 1u : 0u,
                    m->has_saved_exception ? 1u : 0u);
            clear_execve_log++;
        }
    }
    m->pending_epc          = 0;
    m->pending_syscall_epc  = 0;
    m->pending_excode       = 0;
    m->pending_cause        = 0;
    m->epc_was_written      = false;
    m->pending_cause_served = false;
    m->pending_epc_served   = false;
    m->pending_syscall_nr   = 0;
    m->pending_syscall_a0   = 0;
    m->pending_syscall_a0_str[0] = '\0';
    m->do_no_page_watch_active = false;
    m->do_no_page_watch_ra = 0;
    m->do_no_page_watch_addr = 0;
    m->do_no_page_watch_pte_ptr = 0;
    m->filemap_nopage_watch_active = false;
    m->filemap_nopage_watch_ra = 0;
    m->filemap_nopage_watch_addr = 0;
    reset_tlb_defer_state(m);
    m->execve_user_entry_valid = false;
    m->execve_user_entry = 0;
    if (clear_execve_watch) {
        m->execve_watch_active = false;
        m->execve_entry_pc = 0;
        m->execve_last_pc = 0;
        m->execve_last_ctx_valid = false;
    }
}

static void arm_execve_user_handoff(machine_t *m, uint64_t user_pc, uint64_t user_sp)
{
    m->execve_user_handoff_active = true;
    m->execve_user_handoff_pc = user_pc;
    m->execve_user_handoff_sp = user_sp;
    m->execve_user_handoff_state = EXECVE_HANDOFF_STATE_ARMED;
    m->execve_user_handoff_done_keep_count = 0;
    m->user_handoff_fault_traced = false;

    /* Quarantine stale re-delivered notifications from the completed syscall. */
    m->pending_cause_served = false;
    m->pending_epc_served = false;
    m->last_unmapped_valid = false;
    m->do_no_page_watch_active = false;
    m->do_no_page_watch_ra = 0;
    m->do_no_page_watch_addr = 0;
    m->do_no_page_watch_pte_ptr = 0;
    m->filemap_nopage_watch_active = false;
    m->filemap_nopage_watch_ra = 0;
    m->filemap_nopage_watch_addr = 0;
    reset_tlb_defer_state(m);
    m->has_saved_exception = false;

    if (m->cfg.trace_user_handoff) {
        fprintf(stderr,
                "[EXECVE_HANDOFF_ARM] user_pc=0x%08" PRIX64
                " user_sp=0x%08" PRIX64 "\n",
                (uint64_t)(uint32_t)user_pc,
                (uint64_t)(uint32_t)user_sp);
    }
}

static void snapshot_execve_gpr_context(machine_t *m, uc_engine *uc, uint64_t pc)
{
    m->execve_last_pc = pc;
    for (int i = 0; i < 32; i++) {
        uint64_t v = 0;
        uc_reg_read(uc, UC_MIPS_REG_0 + i, &v);
        m->execve_last_gpr[i] = v;
    }
    m->execve_last_ctx_valid = true;
}

static void restore_execve_gpr_context(const machine_t *m, uc_engine *uc)
{
    if (!m->execve_last_ctx_valid)
        return;
    for (int i = 1; i < 32; i++) {
        uint64_t v = m->execve_last_gpr[i];
        uc_reg_write(uc, UC_MIPS_REG_0 + i, &v);
    }
}

static void save_pending_exception(machine_t *m)
{
    if (m->has_saved_exception)
        return;
    m->saved_pending_epc          = m->pending_epc;
    m->saved_pending_excode       = m->pending_excode;
    m->saved_pending_cause        = m->pending_cause;
    m->saved_epc_was_written      = m->epc_was_written;
    m->saved_pending_cause_served = m->pending_cause_served;
    m->saved_pending_epc_served   = m->pending_epc_served;
    m->has_saved_exception        = true;
}

static void restore_pending_exception(machine_t *m)
{
    if (!m->has_saved_exception)
        return;
    static uint32_t restore_log_count = 0;
    if (tlb_trace_window_active(m) && restore_log_count < 96) {
        fprintf(stderr,
                "[EXC_RESTORE] from excode=%u/epc=0x%08" PRIX64
                " -> excode=%u/epc=0x%08" PRIX64
                " saved_epc_written=%u\n",
                m->pending_excode, (uint64_t)(uint32_t)m->pending_epc,
                m->saved_pending_excode, (uint64_t)(uint32_t)m->saved_pending_epc,
                m->saved_epc_was_written ? 1u : 0u);
        restore_log_count++;
    }
    m->pending_epc          = m->saved_pending_epc;
    m->pending_excode       = m->saved_pending_excode;
    m->pending_cause        = m->saved_pending_cause;
    m->epc_was_written      = m->saved_epc_was_written;
    m->pending_cause_served = m->saved_pending_cause_served;
    m->pending_epc_served   = m->saved_pending_epc_served;
    m->has_saved_exception  = false;
    m->saved_exception_reason = SAVE_REASON_NONE;
}

static void update_irq_lines(machine_t *m)
{
    /*
     * VR41xx timer path:
     * RTC elapsed-time compare -> RTCINTREG.ELAPSEDTIME -> ICU SYSINT1 bit3.
     */
    if (m->rtc.rtcint & RTCINT_ELAPSEDTIME_INT)
        icu_assert(&m->icu, ICU_SRC1_ETIME);
    else
        icu_deassert(&m->icu, ICU_SRC1_ETIME);

    (void)m;
}

/*
 * Temporary timer shim:
 * linux4.be spins in calibrate_delay waiting for jiffies to change.
 * Until CP0 timer/interrupt delivery is fully emulated, bump jiffies
 * in RAM once per execution batch to keep early boot moving.
 */
static void tick_jiffies_hack(machine_t *m)
{
    static const uint32_t default_jiffies_pa = 0x001CD9E0u;
    uint32_t jiffies_pa = m->has_jiffies_pa ? m->jiffies_pa : default_jiffies_pa;
    uint32_t j = 0;
    if (uc_mem_read(m->uc, jiffies_pa, &j, sizeof(j)) == UC_ERR_OK) {
        j += 1;
        uc_mem_write(m->uc, jiffies_pa, &j, sizeof(j));
    }
}

static bool mem_unmapped_hook(uc_engine *uc, uc_mem_type type,
                              uint64_t address, int size, int64_t value,
                              void *user_data)
{
    (void)uc;
    machine_t *m = user_data;
    m->last_unmapped_addr = address;
    m->last_unmapped_type = type;
    m->last_unmapped_valid = true;
    uint64_t pc = 0, status = 0;
    uc_reg_read(m->uc, UC_MIPS_REG_PC, &pc);
    uc_reg_read(m->uc, UC_MIPS_REG_CP0_STATUS, &status);
    fprintf(stderr,
            "[UNMAPPED] type=%d addr=0x%08" PRIX64 " size=%d value=0x%08" PRIX64
            " PC=0x%08" PRIX64 " STATUS=0x%08" PRIX64
            " pending_excode=%u pending_epc=0x%08" PRIX64 "\n",
            (int)type, (uint64_t)(uint32_t)address, size, (uint64_t)(uint32_t)value,
            (uint64_t)(uint32_t)pc, status,
            m->pending_excode, (uint64_t)(uint32_t)m->pending_epc);
    return false;
}

static bool alias_write_sync_hook(uc_engine *uc, uc_mem_type type,
                                  uint64_t address, int size, int64_t value,
                                  void *user_data)
{
    (void)uc;
    (void)type;
    machine_t *m = user_data;

    if (!m->alias_fallback_sync_active || m->shared_alias_active)
        return true;
    if (m->alias_sync_reentrant)
        return true;
    if (size <= 0 || size > 8)
        return true;

    uint64_t off = 0;
    if (!sdram_alias_pa_offset(m, address, &off))
        return true;
    if (off + (uint64_t)size > (uint64_t)m->cfg.sdram_size)
        return true;

    uint8_t bytes[8];
    uint64_t uval = (uint64_t)value;
    for (int i = 0; i < size; i++)
        bytes[i] = (uint8_t)(uval >> (i * 8));

    uint64_t targets[] = {
        off,
        UINT64_C(0x0000000080000000) + off,
        UINT64_C(0x00000000A0000000) + off,
        UINT64_C(0xFFFFFFFF80000000) + off,
        UINT64_C(0xFFFFFFFFA0000000) + off,
    };

    m->alias_sync_reentrant = true;
    for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        if (targets[i] == address)
            continue;
        uc_mem_write(m->uc, targets[i], bytes, (size_t)size);
    }
    m->alias_sync_reentrant = false;
    return true;
}

/* write_pa_u32_all_aliases, read_pa_u32_all_aliases, alias_coherence_probe
 * moved to mem_alias.c */

/* Convert kseg0/kseg1 VA to physical; return false for non-direct-mapped VA. */

/*
 * Best-effort instruction read for hooks.
 * Unicorn sometimes reports virtual PC while uc_mem_read expects mapped PA.
 */
bool read_insn_best_effort(uc_engine *uc, uint64_t address, uint32_t *insn)
{
    if (uc_mem_read(uc, address, insn, 4) == UC_ERR_OK)
        return true;

    uint64_t pa = 0;
    if (va_to_pa_kseg(address, &pa) && uc_mem_read(uc, pa, insn, 4) == UC_ERR_OK)
        return true;

    return false;
}

/* Best-effort VA inference for load/store ops at PC when BadVAddr is stale. */
static bool infer_mem_access_va_from_pc(machine_t *m, uint64_t pc, uint32_t *va_out)
{
    uint32_t insn = 0;
    if (!va_out || !read_insn_best_effort(m->uc, pc, &insn))
        return false;

    uint32_t op = insn >> 26;
    bool is_mem =
        (op == 0x20u || op == 0x21u || op == 0x22u || op == 0x23u || op == 0x24u ||
         op == 0x25u || op == 0x26u || op == 0x28u || op == 0x29u || op == 0x2Au ||
         op == 0x2Bu || op == 0x30u || op == 0x34u || op == 0x37u || op == 0x38u ||
         op == 0x39u || op == 0x3Fu);
    if (!is_mem)
        return false;

    uint32_t rs = (insn >> 21) & 0x1Fu;
    int32_t imm = (int32_t)(int16_t)(insn & 0xFFFFu);
    uint64_t base = 0;
    uc_reg_read(m->uc, UC_MIPS_REG_0 + (int)rs, &base);
    *va_out = (uint32_t)((uint32_t)base + (uint32_t)imm);
    return true;
}

/*
 * Fallback for TLB load-miss notifications (intno=26) during do_execve:
 * decode simple MIPS load ops, read from guest memory, write to destination
 * register, and advance PC.  Analogous to emulate_store_on_write_unmapped
 * but for reads.  The guest TLB has no entries for kuseg pages, so
 * Unicorn's softmmu keeps re-notifying; this bypasses the TLB entirely.
 */
static bool emulate_load_at_pc(machine_t *m, uint64_t pc)
{
    uint32_t insn = 0;
    if (!read_insn_best_effort(m->uc, pc, &insn))
        return false;

    uint32_t op = insn >> 26;
    /* lb=0x20, lbu=0x24, lh=0x21, lhu=0x25, lw=0x23 */
    if (!(op == 0x20u || op == 0x21u || op == 0x23u ||
          op == 0x24u || op == 0x25u))
        return false;

    uint32_t rs = (insn >> 21) & 0x1Fu;
    uint32_t rt = (insn >> 16) & 0x1Fu;
    int32_t imm = (int32_t)(int16_t)(insn & 0xFFFFu);

    uint64_t base = 0;
    uc_reg_read(m->uc, UC_MIPS_REG_0 + (int)rs, &base);
    uint32_t addr32 = (uint32_t)((uint32_t)base + (uint32_t)imm);
    uint64_t addr64 = (uint64_t)addr32;

    /* Ensure the target block is mapped. */
    uint64_t block32 = (uint64_t)(addr32 & ~0xFFFFFu);
    uint64_t block = addr64 & ~(uint64_t)0xFFFFF;
    if (addr32 >= 0x80000000u && addr32 <= 0xBFFFFFFFu) {
        map_kseg_mirror_block(m, block);
    } else {
        bool mapped = false;
        if (m->cfg.kuseg_hotpath_populate && addr32 < 0x80000000u) {
            mapped = shadow_tlb_populate(m, addr32, false,
                                         "LOAD_EMU_POPULATE",
                                         (uint32_t)pc);
        }
        if (!mapped) {
            uc_mem_map(m->uc, block32, 0x100000, UC_PROT_READ | UC_PROT_WRITE);
            if (block != block32)
                uc_mem_map(m->uc, block, 0x100000, UC_PROT_READ | UC_PROT_WRITE);
        }
    }

    uint64_t val = 0;
    uc_err re = UC_ERR_OK;
    if (op == 0x20u) { /* lb (sign-extend) */
        int8_t v = 0;
        re = uc_mem_read(m->uc, addr64, &v, sizeof(v));
        val = (uint64_t)(int64_t)v;
    } else if (op == 0x24u) { /* lbu */
        uint8_t v = 0;
        re = uc_mem_read(m->uc, addr64, &v, sizeof(v));
        val = (uint64_t)v;
    } else if (op == 0x21u) { /* lh (sign-extend) */
        int16_t v = 0;
        re = uc_mem_read(m->uc, addr64, &v, sizeof(v));
        val = (uint64_t)(int64_t)v;
    } else if (op == 0x25u) { /* lhu */
        uint16_t v = 0;
        re = uc_mem_read(m->uc, addr64, &v, sizeof(v));
        val = (uint64_t)v;
    } else if (op == 0x23u) { /* lw (sign-extend) */
        int32_t v = 0;
        re = uc_mem_read(m->uc, addr64, &v, sizeof(v));
        val = (uint64_t)(int64_t)v;
    }
    if (re != UC_ERR_OK)
        return false;

    uc_reg_write(m->uc, UC_MIPS_REG_0 + (int)rt, &val);
    uint64_t next_pc = pc + 4u;
    uc_reg_write(m->uc, UC_MIPS_REG_PC, &next_pc);

    static uint32_t load_emu_log = 0;
    if (load_emu_log < 256) {
        fprintf(stderr,
                "[LOAD_EMU] op=0x%02X pc=0x%08" PRIX64
                " addr=0x%08X rt=$%u val=0x%08" PRIX64 "\n",
                op, (uint64_t)(uint32_t)pc, addr32, rt,
                (uint64_t)(uint32_t)val);
        load_emu_log++;
    }
    return true;
}

/* Forward declaration */
static void kuseg_store_writeback(machine_t *m, uint32_t kuseg_va,
                                  uint32_t size_bytes);

/*
 * Fallback for UC_ERR_WRITE_UNMAPPED in late execve/user-copy paths:
 * decode simple MIPS store ops and commit the write directly, then advance PC.
 * This avoids livelock when Unicorn repeatedly reports write-unmapped without
 * completing the guest store after we map candidate blocks.
 */
static bool emulate_store_on_write_unmapped(machine_t *m, uint64_t pc)
{
    uint32_t insn = 0;
    if (!read_insn_best_effort(m->uc, pc, &insn))
        return false;

    uint32_t op = insn >> 26;
    if (!(op == 0x28u || op == 0x29u || op == 0x2Bu || op == 0x3Fu))
        return false; /* sb, sh, sw, sd */

    uint32_t rs = (insn >> 21) & 0x1Fu;
    uint32_t rt = (insn >> 16) & 0x1Fu;
    int32_t imm = (int32_t)(int16_t)(insn & 0xFFFFu);

    uint64_t base = 0, src = 0;
    uc_reg_read(m->uc, UC_MIPS_REG_0 + (int)rs, &base);
    uc_reg_read(m->uc, UC_MIPS_REG_0 + (int)rt, &src);

    uint32_t addr32 = (uint32_t)((uint32_t)base + (uint32_t)imm);
    uint64_t addr64 = (uint64_t)addr32;
    uint64_t block32 = (uint64_t)(addr32 & ~0xFFFFFu);
    uint64_t block = addr64 & ~(uint64_t)0xFFFFF;

    if (addr32 >= 0x80000000u && addr32 <= 0xBFFFFFFFu) {
        map_kseg_mirror_block(m, block);
    } else {
        bool mapped = false;
        if (m->cfg.kuseg_hotpath_populate && addr32 < 0x80000000u) {
            mapped = shadow_tlb_populate(m, addr32, false,
                                         "STORE_EMU_POPULATE",
                                         (uint32_t)pc);
        }
        if (!mapped) {
            uc_mem_map(m->uc, block32, 0x100000, UC_PROT_READ | UC_PROT_WRITE);
            if (block != block32)
                uc_mem_map(m->uc, block, 0x100000, UC_PROT_READ | UC_PROT_WRITE);
        }
    }

    uc_err we = UC_ERR_OK;
    if (op == 0x28u) { /* sb */
        uint8_t v = (uint8_t)(src & 0xFFu);
        we = uc_mem_write(m->uc, addr64, &v, sizeof(v));
    } else if (op == 0x29u) { /* sh */
        uint16_t v = (uint16_t)(src & 0xFFFFu);
        we = uc_mem_write(m->uc, addr64, &v, sizeof(v));
    } else if (op == 0x2Bu) { /* sw */
        uint32_t v = (uint32_t)src;
        we = uc_mem_write(m->uc, addr64, &v, sizeof(v));
    } else if (op == 0x3Fu) { /* sd */
        uint64_t v = src;
        we = uc_mem_write(m->uc, addr64, &v, sizeof(v));
    }
    if (we != UC_ERR_OK)
        return false;

    /* Write back to SDRAM PA for kuseg coherence */
    if (addr32 < 0x80000000u) {
        uint32_t sz = 4;
        if (op == 0x28u) sz = 1;       /* sb */
        else if (op == 0x29u) sz = 2;   /* sh */
        else if (op == 0x2Bu) sz = 4;   /* sw */
        else if (op == 0x3Fu) sz = 8;   /* sd */
        kuseg_store_writeback(m, addr32, sz);
    }

    uint64_t next_pc = pc + 4u;
    uc_reg_write(m->uc, UC_MIPS_REG_PC, &next_pc);

    static uint32_t store_emu_log = 0;
    if (store_emu_log < 256) {
        fprintf(stderr,
                "[STORE_EMU] op=0x%02X pc=0x%08" PRIX64
                " addr=0x%08X rt=$%u val=0x%08" PRIX64 "\n",
                op, (uint64_t)(uint32_t)pc, addr32, rt,
                (uint64_t)(uint32_t)src);
        store_emu_log++;
    }
    return true;
}

/*
 * Unicorn sometimes reports UC_ERR_WRITE_UNMAPPED at a nearby non-store PC
 * (e.g. branch/jr around a failing store). Probe a small window around the
 * fault PC and emulate the first decodable store we find.
 */
static bool emulate_store_nearby_on_write_unmapped(machine_t *m, uint64_t bad_pc)
{
    uint64_t probes[4];
    int n = 0;
    probes[n++] = bad_pc;
    if ((uint32_t)bad_pc >= 4u)
        probes[n++] = bad_pc - 4u;
    if ((uint32_t)bad_pc >= 8u)
        probes[n++] = bad_pc - 8u;
    probes[n++] = bad_pc + 4u;

    for (int i = 0; i < n; i++) {
        uint64_t pc = probes[i];
        if (emulate_store_on_write_unmapped(m, pc)) {
            static uint32_t near_log = 0;
            if (near_log < 256) {
                fprintf(stderr,
                        "[STORE_EMU_NEAR] bad_pc=0x%08" PRIX64
                        " emu_pc=0x%08" PRIX64 "\n",
                        (uint64_t)(uint32_t)bad_pc,
                        (uint64_t)(uint32_t)pc);
                near_log++;
            }
            return true;
        }
    }
    return false;
}

/*
 * Decode a store instruction at `pc` and compute the effective target VA.
 * Returns true if `pc` contains a recognized store (sb/sh/sw/sd/swl/swr).
 */
static bool decode_store_target_va(machine_t *m, uint64_t pc, uint32_t *target_va_out)
{
    uint32_t insn = 0;
    if (!read_insn_best_effort(m->uc, pc, &insn))
        return false;
    uint32_t op = insn >> 26;
    /* sb=0x28, sh=0x29, sw=0x2B, swl=0x2A, swr=0x2E, sd=0x3F, sc=0x38 */
    if (!(op == 0x28u || op == 0x29u || op == 0x2Au || op == 0x2Bu ||
          op == 0x2Eu || op == 0x38u || op == 0x3Fu))
        return false;
    uint32_t rs = (insn >> 21) & 0x1Fu;
    int32_t imm = (int32_t)(int16_t)(insn & 0xFFFFu);
    uint64_t base = 0;
    uc_reg_read(m->uc, UC_MIPS_REG_0 + (int)rs, &base);
    *target_va_out = (uint32_t)((uint32_t)base + (uint32_t)imm);
    return true;
}

/*
 * Decode a load instruction at `pc` and compute the effective source VA.
 * Returns true if `pc` contains a recognized load.
 */
static bool decode_load_source_va(machine_t *m, uint64_t pc, uint32_t *source_va_out)
{
    uint32_t insn = 0;
    if (!read_insn_best_effort(m->uc, pc, &insn))
        return false;
    uint32_t op = insn >> 26;
    /* lb=0x20, lh=0x21, lwl=0x22, lw=0x23, lbu=0x24, lhu=0x25, lwr=0x26, ll=0x30 */
    if (!(op == 0x20u || op == 0x21u || op == 0x22u || op == 0x23u ||
          op == 0x24u || op == 0x25u || op == 0x26u || op == 0x30u))
        return false;
    uint32_t rs = (insn >> 21) & 0x1Fu;
    int32_t imm = (int32_t)(int16_t)(insn & 0xFFFFu);
    uint64_t base = 0;
    uc_reg_read(m->uc, UC_MIPS_REG_0 + (int)rs, &base);
    *source_va_out = (uint32_t)((uint32_t)base + (uint32_t)imm);
    return true;
}

/*
 * Populate a kseg2/kseg3 VA leaf directly from the shadow TLB translation.
 * This is the kernel-mapped analogue of shadow_tlb_populate(), used when
 * kernel code faults on high VAs like 0xFFFFxxxx that Unicorn leaves unmapped.
 */
static bool shadow_tlb_populate_kernel_mapped(machine_t *m, uint32_t va,
                                              const char *tag,
                                              uint32_t pc_hint)
{
    uint32_t best_idx = 0xFFFFFFFFu;
    uint32_t best_seq = 0u;
    uint32_t entryhi = 0, lo0 = 0, lo1 = 0, pagemask = 0;
    uint32_t entryhi_vpn2 = 0;
    uint8_t entryhi_asid = 0;
    bool current_asid_valid = false;
    uint8_t current_asid = 0;
    uint64_t page_bytes = 0, pa_page = 0, va_page = 0;
    const char *reason = "init";
    bool odd_page = false;
    uint32_t lo = 0;

    if (!m || !tag || va < 0xC0000000u) {
        return false;
    }

    current_asid_valid = m->shadow_cp0_entryhi_live_valid;
    current_asid = (uint8_t)(m->shadow_cp0_entryhi_live & 0xFFu);

    for (uint32_t i = 0; i < 64u; i++) {
        if (!m->shadow_tlb_valid[i])
            continue;
        if (!tlb_entry_matches_va(va, m->shadow_tlb_entryhi[i],
                                  m->shadow_tlb_pagemask[i]))
            continue;
        if (m->shadow_tlb_seq[i] >= best_seq) {
            best_seq = m->shadow_tlb_seq[i];
            best_idx = i;
        }
    }

    if (best_idx == 0xFFFFFFFFu) {
        reason = "vpn2_mismatch";
        goto skip;
    }

    entryhi = m->shadow_tlb_entryhi[best_idx];
    lo0 = m->shadow_tlb_lo0[best_idx];
    lo1 = m->shadow_tlb_lo1[best_idx];
    pagemask = m->shadow_tlb_pagemask[best_idx];
    entryhi_vpn2 = entryhi & 0xFFFFE000u;
    entryhi_asid = (uint8_t)(entryhi & 0xFFu);

    page_bytes = tlb_leaf_bytes_from_pagemask(pagemask);
    if (page_bytes < 0x1000u) {
        reason = "invalid_page_bytes";
        goto skip;
    }

    odd_page = (((uint64_t)va & page_bytes) != 0u);
    lo = odd_page ? lo1 : lo0;
    if ((lo & 0x2u) == 0u) {
        reason = odd_page ? "lo1_invalid" : "lo0_invalid";
        goto skip;
    }

    {
        bool global_pair = ((lo0 & 0x1u) != 0u) && ((lo1 & 0x1u) != 0u);
        bool asid_ok = global_pair || !current_asid_valid ||
                       (entryhi_asid == current_asid);
        if (!asid_ok) {
            reason = "asid_mismatch";
            goto skip;
        }
        if (!global_pair && !current_asid_valid)
            reason = "asid_unchecked";
    }

    {
        uint64_t pfn = (uint64_t)((lo >> 6) & 0xFFFFFu);
        pa_page = (pfn << 10) & ~(page_bytes - 1u);
        if (pa_page >= (uint64_t)m->cfg.sdram_size) {
            reason = "pa_out_of_range";
            goto skip;
        }
    }

    va_page = (uint64_t)va & ~(page_bytes - 1u);

    {
        uc_err me = uc_mem_map(m->uc, va_page, page_bytes, UC_PROT_ALL);
        bool premap = (me == UC_ERR_MAP);
        if (premap)
            me = uc_mem_protect(m->uc, va_page, page_bytes, UC_PROT_ALL);
        if (me != UC_ERR_OK) {
            reason = "map_fail";
            goto skip;
        }

        uint8_t buf[4096];
        for (uint64_t off = 0; off < page_bytes; off += sizeof(buf)) {
            uint64_t pa_off = pa_page + off;
            bool got_data = false;
            if (pa_off < (uint64_t)m->cfg.sdram_size) {
                uint64_t kseg0_addr = UINT64_C(0x80000000) + pa_off;
                if (uc_mem_read(m->uc, kseg0_addr, buf, sizeof(buf)) == UC_ERR_OK)
                    got_data = true;
            }
            if (!got_data && pa_off < (uint64_t)m->cfg.sdram_size) {
                if (uc_mem_read(m->uc, pa_off, buf, sizeof(buf)) == UC_ERR_OK)
                    got_data = true;
            }
            if (!got_data)
                memset(buf, 0, sizeof(buf));
            uc_mem_write(m->uc, va_page + off, buf, sizeof(buf));
        }

        static uint32_t ok_log = 0;
        if (ok_log++ < 128u) {
            fprintf(stderr,
                    "[%s_POPULATE] va=0x%08X pc=0x%08X pa=0x%08" PRIX64
                    " bytes=0x%08" PRIX64 " src=tlb:%u reason=%s"
                    " entryhi=0x%08X lo0=0x%08X lo1=0x%08X mask=0x%08X"
                    " premap=%u\n",
                    tag, va, pc_hint, pa_page, page_bytes, best_idx, reason,
                    entryhi, lo0, lo1, pagemask, premap ? 1u : 0u);
        }
        return true;
    }

skip:
    {
        static uint32_t skip_log = 0;
        if (skip_log++ < 128u) {
            fprintf(stderr,
                    "[%s_SKIP] va=0x%08X pc=0x%08X reason=%s src=%s%u"
                    " entryhi=0x%08X vpn2=0x%08X lo0=0x%08X lo1=0x%08X"
                    " mask=0x%08X asid=0x%02X cur_asid=%s0x%02X"
                    " ctx=0x%08X badv=0x%08X\n",
                    tag, va, pc_hint, reason,
                    (best_idx == 0xFFFFFFFFu) ? "none:" : "tlb:",
                    (best_idx == 0xFFFFFFFFu) ? 0u : best_idx,
                    entryhi, entryhi_vpn2, lo0, lo1, pagemask,
                    (unsigned)entryhi_asid,
                    current_asid_valid ? "" : "?",
                    (unsigned)current_asid,
                    (uint32_t)m->shadow_cp0_context,
                    (uint32_t)m->shadow_cp0_badvaddr);
        }
    }
    return false;
}

/*
 * Inject a TLBS (TLB store miss) exception for a kernel-mode store to
 * an unmapped kuseg VA. Sets up shadow CP0 state (BadVAddr, EntryHi,
 * Context), saves any existing exception state, and redirects PC to
 * the TLB refill vector.
 *
 * Returns true if the injection was performed.
 */
static bool inject_kernel_kuseg_tlbs(machine_t *m, uint64_t pc, uint32_t kuseg_va)
{
    static uint32_t tlbs_entry_log = 0;
    if (!m->kernel_vm_ready) {
        if (tlbs_entry_log < 32) {
            fprintf(stderr, "[TLBS_ENTRY] SKIP vm_not_ready pc=0x%08X va=0x%08X\n",
                    (uint32_t)pc, kuseg_va);
            tlbs_entry_log++;
        }
        return false;
    }
    if ((uint32_t)pc < 0x80000000u) {
        if (tlbs_entry_log < 32) {
            fprintf(stderr, "[TLBS_ENTRY] SKIP user_pc pc=0x%08X va=0x%08X\n",
                    (uint32_t)pc, kuseg_va);
            tlbs_entry_log++;
        }
        return false;  /* not kernel mode */
    }
    if (kuseg_va >= 0x80000000u) {
        if (tlbs_entry_log < 32) {
            fprintf(stderr, "[TLBS_ENTRY] SKIP not_kuseg pc=0x%08X va=0x%08X\n",
                    (uint32_t)pc, kuseg_va);
            tlbs_entry_log++;
        }
        return false;  /* not kuseg */
    }

    /* Check shadow TLB: if a valid D=1 entry exists, don't inject —
     * the caller should populate and emulate instead. */
    tlb_lookup_result_t r = shadow_tlb_lookup(m, kuseg_va);
    if (r.hit && r.confident) {
        /* Entry exists with valid translation. Check D (dirty) bit. */
        bool odd_page = (((uint64_t)kuseg_va & r.page_bytes) != 0u);
        uint32_t lo = odd_page ? r.lo1 : r.lo0;
        if ((lo & 0x4u) != 0u) {
            /* D=1: page is writable, don't inject — populate instead */
            return false;
        }
        /* D=0: page exists but not writable — inject TLBS (dirty fault)
         * so the kernel can upgrade the PTE to writable. */
    }

    /* Save existing exception state if we're nested (e.g. inside SYSCALL) */
    if (m->pending_excode != 0u)
        save_pending_exception(m);

    /* Set up shadow CP0 for the TLB handler */
    uint32_t asid = (uint32_t)(m->shadow_cp0_entryhi_live_valid
                     ? m->shadow_cp0_entryhi_live
                     : m->shadow_cp0_entryhi) & 0xFFu;
    uint32_t ctx_base = (uint32_t)m->shadow_cp0_context & 0xFF800000u;
    uint32_t ctx_badvpn2 = ((kuseg_va >> 9) & 0x007FFFF0u);
    m->shadow_cp0_badvaddr = (uint64_t)kuseg_va;
    m->shadow_cp0_entryhi = (uint64_t)((kuseg_va & 0xFFFFE000u) | asid);
    m->shadow_cp0_context = (uint64_t)(ctx_base | ctx_badvpn2);

    /* Set pending exception state */
    m->pending_epc          = (uint32_t)pc;
    m->pending_excode       = MIPS_EXCCODE_TLBS;
    m->pending_cause        = (uint32_t)(MIPS_EXCCODE_TLBS << 2);
    m->epc_was_written      = false;
    m->pending_cause_served = false;
    m->pending_epc_served   = false;

    /* Set EXL=1 and choose vector based on previous EXL state */
    uint64_t ex_status = 0;
    uc_reg_read(m->uc, UC_MIPS_REG_CP0_STATUS, &ex_status);
    bool was_exl = (ex_status & 0x2u) != 0u;
    ex_status |= 0x2u;  /* EXL=1 */
    uc_reg_write(m->uc, UC_MIPS_REG_CP0_STATUS, &ex_status);

    /* Vector selection per MIPS spec:
     *   - TLB Refill (no matching entry):  EXL=0 → 0x80000000, EXL=1 → 0x80000180
     *   - TLB Invalid (entry V=0):         ALWAYS 0x80000180 (general exception)
     *   - TLB Modified (entry V=1, D=0):   ALWAYS 0x80000180 (general exception)
     * r.hit indicates a matching TLB entry exists (even if V=0 or D=0). */
    uint64_t vec;
    if (ex_status & 0x00400000u) {
        vec = mips_sext(0xBFC00380u);  /* BEV=1 */
    } else if (was_exl || r.hit) {
        vec = mips_sext(0x80000180u);  /* general exception vector (TLB Invalid/Modified or nested) */
    } else {
        vec = mips_sext(0x80000000u);  /* TLB refill vector (true TLB miss) */
    }
    uc_reg_write(m->uc, UC_MIPS_REG_PC, &vec);

    static uint32_t kernel_tlbs_inject_log = 0;
    if (kernel_tlbs_inject_log < 256) {
        fprintf(stderr,
                "[KERNEL_TLBS_INJECT] pc=0x%08X kuseg_va=0x%08X"
                " entryhi=0x%08X ctx=0x%08X vec=0x%08" PRIX64
                " status=0x%08" PRIX64 " was_exl=%u saved=%u"
                " tlb_hit=%u tlb_reason=%s\n",
                (uint32_t)pc, kuseg_va,
                (uint32_t)m->shadow_cp0_entryhi,
                (uint32_t)m->shadow_cp0_context,
                (uint64_t)(uint32_t)vec,
                (uint64_t)(uint32_t)ex_status,
                was_exl ? 1u : 0u,
                m->has_saved_exception ? 1u : 0u,
                r.hit ? 1u : 0u,
                r.reason ? r.reason : "none");
        kernel_tlbs_inject_log++;
    }
    return true;
}

/*
 * Inject a TLBL (TLB load miss) exception for a kernel-mode read from
 * an unmapped kuseg VA. Same mechanism as inject_kernel_kuseg_tlbs but
 * with TLBL exception code.
 */
static bool inject_kernel_kuseg_tlbl(machine_t *m, uint64_t pc, uint32_t kuseg_va)
{
    if (!m->kernel_vm_ready)
        return false;
    if ((uint32_t)pc < 0x80000000u)
        return false;
    if (kuseg_va >= 0x80000000u)
        return false;

    /* If shadow TLB has a confident entry with V=1, don't inject — populate instead.
     * If entry exists but V=0 (invalid), we still need to inject so do_page_fault runs. */
    tlb_lookup_result_t r = shadow_tlb_lookup(m, kuseg_va);
    if (r.hit && r.confident) {
        bool odd_page = (((uint64_t)kuseg_va & r.page_bytes) != 0u);
        uint32_t lo = odd_page ? r.lo1 : r.lo0;
        if ((lo & 0x2u) != 0u) {
            /* V=1: page is valid, don't inject — populate instead */
            return false;
        }
        /* V=0: entry exists but invalid — inject as TLB Invalid (not Refill) */
    }

    if (m->pending_excode != 0u)
        save_pending_exception(m);

    uint32_t asid = (uint32_t)(m->shadow_cp0_entryhi_live_valid
                     ? m->shadow_cp0_entryhi_live
                     : m->shadow_cp0_entryhi) & 0xFFu;
    uint32_t ctx_base = (uint32_t)m->shadow_cp0_context & 0xFF800000u;
    uint32_t ctx_badvpn2 = ((kuseg_va >> 9) & 0x007FFFF0u);
    m->shadow_cp0_badvaddr = (uint64_t)kuseg_va;
    m->shadow_cp0_entryhi = (uint64_t)((kuseg_va & 0xFFFFE000u) | asid);
    m->shadow_cp0_context = (uint64_t)(ctx_base | ctx_badvpn2);

    m->pending_epc          = (uint32_t)pc;
    m->pending_excode       = MIPS_EXCCODE_TLBL;
    m->pending_cause        = (uint32_t)(MIPS_EXCCODE_TLBL << 2);
    m->epc_was_written      = false;
    m->pending_cause_served = false;
    m->pending_epc_served   = false;

    uint64_t ex_status = 0;
    uc_reg_read(m->uc, UC_MIPS_REG_CP0_STATUS, &ex_status);
    bool was_exl = (ex_status & 0x2u) != 0u;
    ex_status |= 0x2u;
    uc_reg_write(m->uc, UC_MIPS_REG_CP0_STATUS, &ex_status);

    /* Vector selection per MIPS spec:
     *   - TLB Refill (no matching entry):  EXL=0 → 0x80000000, EXL=1 → 0x80000180
     *   - TLB Invalid (entry V=0):         ALWAYS 0x80000180 (general exception)
     */
    uint64_t vec;
    if (ex_status & 0x00400000u) {
        vec = mips_sext(0xBFC00380u);
    } else if (was_exl || r.hit) {
        vec = mips_sext(0x80000180u);
    } else {
        vec = mips_sext(0x80000000u);
    }
    uc_reg_write(m->uc, UC_MIPS_REG_PC, &vec);

    static uint32_t kernel_tlbl_inject_log = 0;
    if (kernel_tlbl_inject_log < 256) {
        fprintf(stderr,
                "[KERNEL_TLBL_INJECT] pc=0x%08X kuseg_va=0x%08X"
                " entryhi=0x%08X ctx=0x%08X vec=0x%08" PRIX64
                " status=0x%08" PRIX64 " was_exl=%u saved=%u"
                " tlb_hit=%u tlb_reason=%s\n",
                (uint32_t)pc, kuseg_va,
                (uint32_t)m->shadow_cp0_entryhi,
                (uint32_t)m->shadow_cp0_context,
                (uint64_t)(uint32_t)vec,
                (uint64_t)(uint32_t)ex_status,
                was_exl ? 1u : 0u,
                m->has_saved_exception ? 1u : 0u,
                r.hit ? 1u : 0u,
                r.reason ? r.reason : "none");
        kernel_tlbl_inject_log++;
    }
    return true;
}

/*
 * After emulating a store to a kuseg VA, write the data back to the
 * corresponding SDRAM PA so that kseg0/kseg1 views remain coherent.
 */
static void kuseg_store_writeback(machine_t *m, uint32_t kuseg_va,
                                  uint32_t size_bytes)
{
    if (kuseg_va >= 0x80000000u)
        return;
    tlb_lookup_result_t r = shadow_tlb_lookup(m, kuseg_va);
    if (!r.hit || !r.confident)
        return;
    uint64_t pa = r.pa_page + ((uint64_t)kuseg_va - r.va_page);
    if (pa >= (uint64_t)m->cfg.sdram_size)
        return;

    uint8_t buf[8];
    if (size_bytes > sizeof(buf))
        size_bytes = sizeof(buf);
    if (uc_mem_read(m->uc, (uint64_t)kuseg_va, buf, size_bytes) != UC_ERR_OK)
        return;

    /* Write to raw PA and kseg0 alias */
    uc_mem_write(m->uc, pa, buf, size_bytes);
    uc_mem_write(m->uc, 0x80000000u + pa, buf, size_bytes);
}

static bool insn_has_delay_slot(uint32_t insn)
{
    uint32_t op = insn >> 26;
    if (op == 0x02u || op == 0x03u)  /* j / jal */
        return true;
    if (op == 0x00u) {
        uint32_t funct = insn & 0x3Fu;
        if (funct == 0x08u || funct == 0x09u)  /* jr / jalr */
            return true;
    }
    if (op == 0x01u ||  /* regimm branches */
        op == 0x04u || op == 0x05u || op == 0x06u || op == 0x07u || /* beq/bne/blez/bgtz */
        op == 0x14u || op == 0x15u || op == 0x16u || op == 0x17u)   /* likely branches */
        return true;
    if (op == 0x10u && ((insn >> 21) & 0x1Fu) == 0x08u) /* bc0* */
        return true;
    if (op == 0x11u && ((insn >> 21) & 0x1Fu) == 0x08u) /* bc1* */
        return true;
    return false;
}

static bool pc_is_delay_slot(uc_engine *uc, uint64_t pc)
{
    if ((uint32_t)pc < 4u)
        return false;
    uint32_t prev = 0;
    if (!read_insn_best_effort(uc, pc - 4u, &prev))
        return false;
    return insn_has_delay_slot(prev);
}


bool read_guest_u32(uc_engine *uc, uint64_t va, uint32_t *out)
{
    uint32_t va32 = (uint32_t)va;
    uint64_t pa = 0;
    /*
     * Prefer direct-mapped kseg translation first; zero-extended kernel VAs
     * (0x80xxxxxx in a 64-bit register) can alias unrelated low regions if
     * read directly as VA.
     */
    if (va_to_pa_kseg(va, &pa) &&
        uc_mem_read(uc, pa, out, sizeof(*out)) == UC_ERR_OK)
        return true;
    if (va32 >= 0x80000000u &&
        uc_mem_read(uc, mips_sext(va32), out, sizeof(*out)) == UC_ERR_OK)
        return true;
    if (uc_mem_read(uc, va, out, sizeof(*out)) == UC_ERR_OK)
        return true;
    return false;
}

static bool page_struct_ptr_to_pa(uc_engine *uc, uint32_t page_ptr,
                                  uint32_t *mem_map_out,
                                  uint32_t *pfn_out,
                                  uint32_t *pa_out)
{
    uint32_t mem_map = 0;
    if (!read_guest_u32(uc, 0x8024108Cu, &mem_map))
        return false;
    if (mem_map_out)
        *mem_map_out = mem_map;
    if (mem_map == 0u || page_ptr < mem_map)
        return false;

    /*
     * Replicate the kernel's inlined do_no_page page->pfn conversion exactly
     * (see do_no_page at 0x8002846c). This avoids guessing struct page layout.
     */
    const uint32_t delta = page_ptr - mem_map;
    uint32_t v1 = (uint32_t)((int32_t)delta >> 2);
    uint32_t v0 = (v1 << 4) + v1;
    v0 += (v0 << 8);
    v0 += (v0 << 16);
    v0 = (uint32_t)(0u - v0);
    v0 <<= 14;

    uint32_t pa_page = (v0 & 0xFFFFF000u) >> 2;
    if (pfn_out) *pfn_out = pa_page >> 12;
    if (pa_out)  *pa_out = pa_page;
    return true;
}

/*
 * Execve pointer shim:
 * 2.4 init code invokes execve() from kernel context with kernel pointers.
 * If addr_limit is USER_DS on this path, copy_from_user-style checks reject
 * those kseg pointers with -EFAULT.  Re-home filename/argv/envp into a stable
 * mapped kuseg scratch page and rewrite A0/A1/A2 to user-space addresses.
 */
static bool prepare_execve_user_ptrs(uc_engine *uc,
                                     uint64_t old_a0, uint64_t old_a1, uint64_t old_a2,
                                     uint32_t *new_a0, uint32_t *new_a1, uint32_t *new_a2)
{
    const uint32_t base = 0x01020000u;
    const uint32_t argv_base = base + 0x00000020u;
    const uint32_t envp_base = base + 0x00000080u;
    const uint32_t str_base  = base + 0x00000200u;
    uint32_t str_cur = str_base;

    char s[192];
    if (read_guest_string(uc, old_a0, s, sizeof(s)) <= 0)
        return false;
    if (uc_mem_write(uc, str_cur, s, strlen(s) + 1) != UC_ERR_OK)
        return false;
    *new_a0 = str_cur;
    str_cur += (uint32_t)strlen(s) + 1u;

    if ((uint32_t)old_a1 != 0) {
        for (int i = 0; i < 15; i++) {
            uint32_t p = 0;
            uint32_t dst = 0;
            if (!read_guest_u32(uc, old_a1 + (uint64_t)(i * 4), &p))
                return false;
            if (p == 0) {
                if (uc_mem_write(uc, argv_base + (uint32_t)(i * 4), &dst, 4) != UC_ERR_OK)
                    return false;
                break;
            }
            if (read_guest_string(uc, p, s, sizeof(s)) <= 0)
                return false;
            dst = str_cur;
            if (uc_mem_write(uc, str_cur, s, strlen(s) + 1) != UC_ERR_OK)
                return false;
            if (uc_mem_write(uc, argv_base + (uint32_t)(i * 4), &dst, 4) != UC_ERR_OK)
                return false;
            str_cur += (uint32_t)strlen(s) + 1u;
        }
        *new_a1 = argv_base;
    } else {
        *new_a1 = 0;
    }

    if ((uint32_t)old_a2 != 0) {
        for (int i = 0; i < 15; i++) {
            uint32_t p = 0;
            uint32_t dst = 0;
            if (!read_guest_u32(uc, old_a2 + (uint64_t)(i * 4), &p))
                return false;
            if (p == 0) {
                if (uc_mem_write(uc, envp_base + (uint32_t)(i * 4), &dst, 4) != UC_ERR_OK)
                    return false;
                break;
            }
            if (read_guest_string(uc, p, s, sizeof(s)) <= 0)
                return false;
            dst = str_cur;
            if (uc_mem_write(uc, str_cur, s, strlen(s) + 1) != UC_ERR_OK)
                return false;
            if (uc_mem_write(uc, envp_base + (uint32_t)(i * 4), &dst, 4) != UC_ERR_OK)
                return false;
            str_cur += (uint32_t)strlen(s) + 1u;
        }
        *new_a2 = envp_base;
    } else {
        *new_a2 = 0;
    }

    return true;
}

/*
 * Fallback execve shim for stale kernel init fallback paths:
 * build argv/envp explicitly when the kernel-side argv/envp pointers have
 * been clobbered by nested exception/retry behavior.
 */
static bool prepare_execve_user_ptrs_defaults(uc_engine *uc,
                                              uint64_t old_a0,
                                              uint32_t *new_a0,
                                              uint32_t *new_a1,
                                              uint32_t *new_a2)
{
    const uint32_t base = 0x01020000u;
    const uint32_t argv_base = base + 0x00000020u;
    const uint32_t envp_base = base + 0x00000080u;
    const uint32_t str_base  = base + 0x00000200u;
    uint32_t str_cur = str_base;

    char filename[192];
    if (read_guest_string(uc, old_a0, filename, sizeof(filename)) <= 0)
        return false;
    if (uc_mem_write(uc, str_cur, filename, strlen(filename) + 1) != UC_ERR_OK)
        return false;
    *new_a0 = str_cur;
    str_cur += (uint32_t)strlen(filename) + 1u;

    const char *argv0 = "init";
    uint32_t argv0_ptr = str_cur;
    if (uc_mem_write(uc, str_cur, argv0, strlen(argv0) + 1) != UC_ERR_OK)
        return false;
    str_cur += (uint32_t)strlen(argv0) + 1u;

    const char *env0 = "HOME=/";
    const char *env1 = "TERM=linux";
    uint32_t env0_ptr = str_cur;
    if (uc_mem_write(uc, str_cur, env0, strlen(env0) + 1) != UC_ERR_OK)
        return false;
    str_cur += (uint32_t)strlen(env0) + 1u;
    uint32_t env1_ptr = str_cur;
    if (uc_mem_write(uc, str_cur, env1, strlen(env1) + 1) != UC_ERR_OK)
        return false;

    uint32_t z = 0;
    if (uc_mem_write(uc, argv_base + 0, &argv0_ptr, 4) != UC_ERR_OK) return false;
    if (uc_mem_write(uc, argv_base + 4, &z, 4) != UC_ERR_OK) return false;
    if (uc_mem_write(uc, envp_base + 0, &env0_ptr, 4) != UC_ERR_OK) return false;
    if (uc_mem_write(uc, envp_base + 4, &env1_ptr, 4) != UC_ERR_OK) return false;
    if (uc_mem_write(uc, envp_base + 8, &z, 4) != UC_ERR_OK) return false;

    *new_a1 = argv_base;
    *new_a2 = envp_base;
    return true;
}

static bool guest_ptr_looks_like_string(uc_engine *uc, uint32_t p)
{
    if (p < 0x1000u)
        return false;
    char s[8];
    return read_guest_string(uc, p, s, sizeof(s)) > 0;
}

static bool execve_vectors_look_valid(uc_engine *uc, uint32_t argv, uint32_t envp)
{
    uint32_t p0 = 0, p1 = 0, e0 = 0;
    if (argv == 0u)
        return false;
    if (!read_guest_u32(uc, argv, &p0) || !guest_ptr_looks_like_string(uc, p0))
        return false;
    if (!read_guest_u32(uc, argv + 4u, &p1))
        return false;
    if (p1 != 0u && !guest_ptr_looks_like_string(uc, p1))
        return false;
    if (envp != 0u) {
        if (!read_guest_u32(uc, envp, &e0))
            return false;
        if (e0 != 0u && !guest_ptr_looks_like_string(uc, e0))
            return false;
    }
    return true;
}

static bool prepare_execve_user_filename(uc_engine *uc,
                                         uint64_t old_a0,
                                         uint32_t *new_a0)
{
    const uint32_t base = 0x01020000u;
    const uint32_t str_base = base + 0x00000200u;
    char filename[192];

    if (read_guest_string(uc, old_a0, filename, sizeof(filename)) <= 0)
        return false;
    if (uc_mem_write(uc, str_base, filename, strlen(filename) + 1) != UC_ERR_OK)
        return false;
    *new_a0 = str_base;
    return true;
}

static uc_err write_mem_best_effort(uc_engine *uc, uint64_t address,
                                    const void *data, size_t size)
{
    uc_err err = uc_mem_write(uc, address, data, size);
    if (err == UC_ERR_OK)
        return err;

    uint64_t pa = 0;
    if (va_to_pa_kseg(address, &pa))
        err = uc_mem_write(uc, pa, data, size);
    return err;
}

/* MIPS_EXCCODE_* defined near top of file */

/* ------------------------------------------------------------------ */
/* Unicorn hooks                                                         */
/* ------------------------------------------------------------------ */

/*
 * Per-instruction trace hook.
 * Only installed when cfg.trace == true (significant performance impact).
 */
static void trace_hook(uc_engine *uc, uint64_t address,
                        uint32_t size, void *user_data)
{
    (void)size;
    machine_t *m = user_data;
    m->insn_count++;

    uint32_t insn = 0;
    if (!read_insn_best_effort(uc, address, &insn))
        insn = 0xFFFFFFFFu;

    /* Read a few key registers for context */
    uint32_t pc = (uint32_t)address;
    uint64_t at=0, v0=0, a0=0, sp=0, ra=0;
    uc_reg_read(uc, UC_MIPS_REG_AT, &at);
    uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
    uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
    uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra);

    fprintf(stderr, "[T] %08X: %08X  at=%016" PRIX64 " v0=%016" PRIX64
                    " a0=%016" PRIX64 " sp=%016" PRIX64 " ra=%016" PRIX64 "\n",
            pc, insn, at, v0, a0, sp, ra);
}

/*
 * PRId intercept hook.
 *
 * Unicorn runs with the NEC VR5432 CPU model (PRId = 0x00000400), but the
 * linux4.be kernel identifies the platform by checking CP0 PRId against the
 * VR4131 value (0x00000C80).  Unicorn does not expose CP0_PRID as writable,
 * so we intercept every `mfc0 $rt, $15` instruction (CP0 reg 15 = PRId):
 *
 *   Encoding: 0x4000_7800 | (rt << 16)
 *   Mask:     0xFFE0_FFFF  (fix all bits except the 5-bit rt field)
 *
 * When detected:
 *   1. Write VR4131_PRID to the destination GPR.
 *   2. Advance PC by 4 to skip the instruction (Unicorn respects PC changes
 *      made inside UC_HOOK_CODE before the instruction executes).
 */
static void prid_hook(uc_engine *uc, uint64_t address,
                      uint32_t size, void *user_data)
{
    (void)size;
    machine_t *m = user_data;

    /* Deferred redirect from ERET_KERNEL_TLB_OK/PGFAULT: the native ERET
     * read a stale CP0_EPC, landing at the wrong PC. Write PC and stop
     * execution so the main loop can apply the redirect reliably. Don't
     * clear the flag — the main loop clears it after applying. */
    if (m->pending_eret_redirect) {
        static uint32_t eret_redir_prid_log = 0;
        if (eret_redir_prid_log < 64) {
            fprintf(stderr,
                    "[ERET_REDIRECT_PRID] at_pc=0x%08" PRIX64
                    " redir_pc=0x%08" PRIX64 "\n",
                    (uint64_t)(uint32_t)address,
                    (uint64_t)(uint32_t)m->pending_eret_redirect_pc);
            eret_redir_prid_log++;
        }
        uc_reg_write(uc, UC_MIPS_REG_PC, &m->pending_eret_redirect_pc);
        uc_emu_stop(uc);
        return;
    }

    /* User-mode instruction fetch: when execution reaches a kuseg VA and the
     * shadow TLB has no entry for that page, the code hasn't been demand-loaded.
     * Inject TLBL so the kernel's page fault handler loads the ELF code page
     * from the ramdisk.  If the shadow TLB has a valid entry, populate the
     * kuseg flat mapping from SDRAM so the instruction bytes are correct.
     *
     * This is needed because kuseg 0x00000000-0x00FFFFFF shares Unicorn's PA
     * region with kseg0 and cannot be unmapped — so instruction fetches don't
     * trigger UC_ERR_FETCH_UNMAPPED for addresses in that range. */
    if (m->kernel_vm_ready && (uint32_t)address < 0x80000000u &&
        m->kuseg_gaps_unmapped) {
        uint32_t upc = (uint32_t)address;
        tlb_lookup_result_t rd = shadow_tlb_lookup(m, upc);
        if (rd.hit && rd.confident) {
            /* TLB entry exists — populate kuseg flat mapping from SDRAM */
            shadow_tlb_populate(m, upc, true,
                                "USER_FETCH_POPULATE", upc);
        } else {
            /* No TLB entry — inject TLBL for code page demand loading */
            uint32_t asid = (uint32_t)(m->shadow_cp0_entryhi_live_valid
                             ? m->shadow_cp0_entryhi_live
                             : m->shadow_cp0_entryhi) & 0xFFu;
            uint32_t ctx_base = (uint32_t)m->shadow_cp0_context & 0xFF800000u;
            uint32_t ctx_badvpn2 = ((upc >> 9) & 0x007FFFF0u);
            m->shadow_cp0_badvaddr = (uint64_t)upc;
            m->shadow_cp0_entryhi = (uint64_t)((upc & 0xFFFFE000u) | asid);
            m->shadow_cp0_context = (uint64_t)(ctx_base | ctx_badvpn2);

            m->pending_epc          = upc;
            m->pending_excode       = MIPS_EXCCODE_TLBL;
            m->pending_cause        = (uint32_t)(MIPS_EXCCODE_TLBL << 2);
            m->epc_was_written      = false;
            m->pending_cause_served = false;
            m->pending_epc_served   = false;

            uint64_t ex_status = 0;
            uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &ex_status);
            bool was_exl = (ex_status & 0x2u) != 0u;
            ex_status |= 0x2u;  /* EXL=1 */
            uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &ex_status);

            uint64_t vec = was_exl ?
                           mips_sext(0x80000180u) :
                           mips_sext(0x80000000u);
            uc_reg_write(uc, UC_MIPS_REG_PC, &vec);

            static uint32_t user_fetch_tlbl_log = 0;
            if (user_fetch_tlbl_log < 128) {
                fprintf(stderr,
                        "[USER_FETCH_TLBL] pc=0x%08X"
                        " entryhi=0x%08X ctx=0x%08X"
                        " vec=0x%08" PRIX64 " status=0x%08" PRIX64
                        " was_exl=%u\n",
                        upc,
                        (uint32_t)m->shadow_cp0_entryhi,
                        (uint32_t)m->shadow_cp0_context,
                        (uint64_t)(uint32_t)vec,
                        (uint64_t)(uint32_t)ex_status,
                        was_exl ? 1u : 0u);
                user_fetch_tlbl_log++;
            }
            uc_emu_stop(uc);
            return;
        }
    }

    m->cp0_count_ticks++;
    m->last_exec_pc = address;

    /* Dead-simple probe: does prid_hook EVER fire at restore_all PCs
     * while /sbin/init SYSCALL state is pending? */
    if ((uint32_t)m->pending_syscall_epc == 0x8000184Cu &&
        m->pending_syscall_nr == 4011u) {
        if ((uint32_t)address == 0x80007C20u ||
            (uint32_t)address == 0x80007C4Cu ||
            (uint32_t)address == 0x800045B8u ||
            (uint32_t)address == 0x80004640u) {
            static uint32_t sbin_init_restore_probe = 0;
            if (sbin_init_restore_probe < 64) {
                fprintf(stderr,
                        "[SBIN_INIT_RESTORE] PC=0x%08X pend_exc=%u"
                        " pend_epc=0x%08" PRIX64 " epc_wr=%u"
                        " has_saved=%u\n",
                        (uint32_t)address, m->pending_excode,
                        (uint64_t)(uint32_t)m->pending_epc,
                        m->epc_was_written ? 1u : 0u,
                        m->has_saved_exception ? 1u : 0u);
                sbin_init_restore_probe++;
            }
        }
        /* Periodic trace: log every 2000th instruction while /sbin/init
         * SYSCALL is pending and has_saved is false (i.e., we're in the
         * SYSCALL's own execution, not a nested TLB exception). */
        if (!m->has_saved_exception && m->pending_excode == 8u) {
            static uint32_t batch5_trace_count = 0;
            static uint32_t batch5_pc_trace_log = 0;
            batch5_trace_count++;
            if ((batch5_trace_count % 2000u) == 1u && batch5_pc_trace_log < 128) {
                uint64_t trace_v0 = 0, trace_sp = 0, trace_ra = 0;
                uc_reg_read(uc, UC_MIPS_REG_V0, &trace_v0);
                uc_reg_read(uc, UC_MIPS_REG_SP, &trace_sp);
                uc_reg_read(uc, UC_MIPS_REG_RA, &trace_ra);
                fprintf(stderr,
                        "[BATCH5_TRACE] #%u PC=0x%08X v0=0x%08" PRIX64
                        " sp=0x%08" PRIX64 " ra=0x%08" PRIX64
                        " pend_exc=%u\n",
                        batch5_pc_trace_log,
                        (uint32_t)address,
                        trace_v0, trace_sp, trace_ra,
                        m->pending_excode);
                batch5_pc_trace_log++;
            }
        }
    }

    /* Targeted probe: log ALL MTC0 CP0_EPC and ERET instructions
     * during the /sbin/init execve window (pending_syscall_epc == 0x8000184C).
     * Also count prid_hook invocations at restore_all PCs. */
    if ((uint32_t)m->pending_syscall_epc == 0x8000184Cu ||
        (m->insn_count >= 159800000ULL && m->insn_count <= 160200000ULL)) {
        uint32_t probe_insn = 0;
        uc_mem_read(uc, address, &probe_insn, 4);
        bool is_mtc0_epc = ((probe_insn & 0xFFE0FFFFu) == 0x40807000u);
        bool is_eret = (probe_insn == 0x42000018u);
        /* Also log if we're at any known restore_all PC */
        bool is_restore_all_pc = ((uint32_t)address == 0x80007C20u ||
                                  (uint32_t)address == 0x80007C4Cu ||
                                  (uint32_t)address == 0x800045B8u ||
                                  (uint32_t)address == 0x80004640u);
        if (is_mtc0_epc || is_eret || is_restore_all_pc) {
            static uint32_t restore_all_probe_log = 0;
            if (restore_all_probe_log < 256) {
                uint64_t probe_status = 0;
                uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &probe_status);
                fprintf(stderr,
                        "[RESTORE_ALL_PROBE] PC=0x%08X insn=0x%08X"
                        " pend_exc=%u pend_epc=0x%08" PRIX64
                        " epc_wr=%u has_saved=%u execve_act=%u"
                        " STATUS=0x%08" PRIX64 " nr=%u syscall_epc=0x%08" PRIX64
                        " insn_count=%" PRIu64 "\n",
                        (uint32_t)address, probe_insn,
                        m->pending_excode,
                        (uint64_t)(uint32_t)m->pending_epc,
                        m->epc_was_written ? 1u : 0u,
                        m->has_saved_exception ? 1u : 0u,
                        m->execve_watch_active ? 1u : 0u,
                        probe_status,
                        m->pending_syscall_nr,
                        (uint64_t)(uint32_t)m->pending_syscall_epc,
                        m->insn_count);
                restore_all_probe_log++;
            }
        }
    }

    static uint32_t mfc0_cause_seen_log = 0;
    static uint32_t mfc0_epc_seen_log = 0;
    static uint32_t mfc0_cause_inject_log = 0;
    static uint32_t mfc0_epc_inject_log = 0;
    static uint32_t mtc0_epc_sys_log = 0;
    static uint32_t mtc0_cp0_log = 0;
    static uint32_t mfc0_cp0_readback_log = 0;
    static uint32_t tlbop_log = 0;
    static uint32_t tlbwi_patch_log = 0;
    static uint32_t do_execve_enter_log = 0;
    static uint32_t do_execve_ret_log = 0;
    static uint32_t open_exec_call_count = 0;

    /*
     * open_exec call-site probe (vmlinux-pgui-demo 2.4.18).
     * JAL open_exec is at 0x8004A9F8, the 10th instruction from do_execve entry.
     * Counting calls here tells us whether open_exec runs during TLB_DEFER_SKIP
     * iterations (fd-leak hypothesis) or only on the final real run.
     */
    if ((uint32_t)address == 0x8004A9F8u) {
        open_exec_call_count++;
        uint64_t a0_path = 0;
        uc_reg_read(uc, UC_MIPS_REG_A0, &a0_path);
        fprintf(stderr,
                "[OPEN_EXEC_CALL] #%u PC=0x%08X a0=0x%08" PRIX64
                " defer_count=%u\n",
                open_exec_call_count, (uint32_t)address,
                (uint64_t)(uint32_t)a0_path,
                m->tlb_defer_count);
    }

    /*
     * open_exec return probe (vmlinux-pgui-demo 2.4.18).
     * JAL open_exec is at 0x8004A9F8; MIPS sets ra = JAL+8 = 0x8004AA00.
     * When execution reaches 0x8004AA00, $v0 holds the struct file* returned
     * by open_exec (positive = success, negative = error code).
     * Mark execve_open_exec_ran so ERET_TLB_PASSTHROUGH / TLB_DEFER_SKIP know
     * to compensate for the leaked nr_files increment before restarting.
     */
    if ((uint32_t)address == 0x8004AA00u) {
        uint64_t v0 = 0;
        uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
        m->execve_open_exec_ran = true;
        static uint32_t open_exec_ret_log = 0;
        if (open_exec_ret_log < 32) {
            fprintf(stderr,
                    "[OPEN_EXEC_RET] #%u v0=0x%08" PRIX64
                    " (file_ptr) defer_count=%u\n",
                    open_exec_call_count, (uint64_t)(uint32_t)v0,
                    m->tlb_defer_count);
            open_exec_ret_log++;
        }
    }

    /*
     * parse_one NULL unknown-param callback intercept (linux4be20040908/vmlinux).
     *
     * parse_one() at 0x80042EF0 loops over the kernel_param table searching for
     * a command-line argument match. When no match is found it falls through to:
     *
     *   80042f7c:  beqz  t3, 80042f8c   ; intended: skip if unknown-cb is NULL
     *   80042f80:  li    v0, -2          ; delay slot: default return value
     *   80042f84:  jalr  t3              ; call the unknown-param handler
     *   80042f88:  move  a1, t4          ; delay slot
     *   80042f8c:  ld    ra, 16(sp)      ; function epilogue
     *   80042f90:  jr    ra
     *
     * t3 = *(sp+40) is the 5th argument — the "unknown param" callback.
     * When parse_args() passes NULL the beqz t3 at 0x80042F7C SHOULD skip the
     * jalr, but Unicorn's MIPS branch-delay-slot emulation has a bug: when the
     * branch IS taken it can execute the instruction two words after the branch
     * (0x80042F84: jalr t3) as the "delay slot" instead of the true delay slot
     * at 0x80042F80 (li v0,-2).  This fires "jalr t3" with t3=0, jumping to
     * VA=0x00000000 and causing an infinite TLB-miss storm.
     *
     * Fix: intercept at the beqz site (0x80042F7C) BEFORE the instruction
     * executes.  When $t3==0, set v0=-2 (what the delay-slot li would have done)
     * and redirect PC directly to the function epilogue at 0x80042F8C, skipping
     * the beqz + delay slot + jalr + delay slot entirely.
     */
    /*
     * do_early_param entry probe — confirm whether do_early_param is reached
     * after jalr t3 from parse_one.
     */
    if ((uint32_t)address == 0x80272450u) {
        static uint32_t dep_entry_log = 0;
        if (dep_entry_log < 8u) {
            uint64_t a0 = 0, a1 = 0, ra_val = 0;
            uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
            uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
            uc_reg_read(uc, UC_MIPS_REG_RA, &ra_val);
            fprintf(stderr,
                    "[DEP_ENTRY] #%u do_early_param a0=0x%08" PRIX64
                    " a1=0x%08" PRIX64 " ra=0x%08" PRIX64 "\n",
                    dep_entry_log,
                    (uint64_t)(uint32_t)a0, (uint64_t)(uint32_t)a1,
                    (uint64_t)(uint32_t)ra_val);
            dep_entry_log++;
        }
    }

    /*
     * parse_one beqz intercept at 0x80042F7C (linux4be20040908/vmlinux).
     *
     * The instruction sequence in parse_one is:
     *   80042f7c: beqz  t3, 80042f8c   ; skip jalr if unknown-cb is NULL
     *   80042f80: li    v0, -2          ; true delay slot
     *   80042f84: jalr  t3              ; call unknown-param callback
     *
     * When t3==0 and the beqz IS taken, Unicorn's MIPS delay-slot emulation
     * has a bug: it executes the instruction at beqz+8 (jalr t3 at 0x80042F84)
     * as the "delay slot" instead of the correct slot at beqz+4 (li v0,-2).
     * This fires jalr t3 with t3=0, jumping to VA=0.
     *
     * Crucially, the prid_hook does NOT fire for 0x80042F84 in this case because
     * Unicorn treats it as an internal delay-slot execution without a code hook.
     *
     * Fix: intercept at the beqz BEFORE it executes.  When t3==0, rewrite t3
     * to the epilogue address (0x80042F8C) and set v0=-2.  This converts:
     *   "beqz taken, wrong delay slot jalr t3(=0) → PC=0"
     * into:
     *   "beqz NOT taken (t3 now non-zero), li v0,-2, jalr t3(=0x80042F8C) → epilogue"
     * The jalr then jumps to parse_one's epilogue which correctly unwinds the frame.
     */
    if ((uint32_t)address == 0x80042F7Cu) {
        uint64_t t3_full = 0;
        uc_reg_read(uc, UC_MIPS_REG_T3, &t3_full);
        if ((uint32_t)t3_full == 0u) {
            /* Redirect: make jalr t3 jump to the epilogue instead of PC=0 */
            uint64_t safe_t3 = mips_sext(0x80042F8Cu);
            uint64_t neg2    = (uint64_t)(uint32_t)-2;
            uc_reg_write(uc, UC_MIPS_REG_T3, &safe_t3);
            uc_reg_write(uc, UC_MIPS_REG_V0, &neg2);
            static uint32_t beqz_fix_log = 0;
            if (beqz_fix_log < 16u) {
                fprintf(stderr,
                        "[BEQZ_T3_FIX] #%u parse_one beqz t3==0; redirect t3→0x%016"
                        PRIX64 " v0=-2\n", beqz_fix_log, safe_t3);
                beqz_fix_log++;
            }
        }
    }

    /* Probe full 64-bit t3 at the jalr t3 site to detect sign-extension issues.
     * Also intercepts when t3==0 to prevent the null jalr before it happens. */
    if ((uint32_t)address == 0x80042F84u) {
        uint64_t t3_full = 0;
        uc_reg_read(uc, UC_MIPS_REG_T3, &t3_full);
        static uint32_t jalr_t3_log = 0;
        /* Always log when t3==0 (null call about to happen), cap otherwise */
        if (jalr_t3_log < 8u || (uint32_t)t3_full == 0u) {
            fprintf(stderr, "[JALR_T3] #%u PC=0x80042F84 t3_full=0x%016" PRIX64
                    " (hi=%s)\n", jalr_t3_log, t3_full,
                    (t3_full >> 32) == 0xFFFFFFFFu ? "FF(OK)" :
                    (t3_full == 0u)                ? "NULL"   : "WRONG");
            jalr_t3_log++;
        }
        /* If t3==0 (null unknown-param callback), set v0=-2 and redirect to
         * parse_one's epilogue at 0x80042F8C.  Because Unicorn still executes
         * the jalr after this hook returns, this redirect may be overridden by
         * the jalr's own PC write; the intr_hook NULL_CALL_RECOVER is the
         * definitive safety net. */
        if ((uint32_t)t3_full == 0u) {
            uint64_t new_pc = mips_sext(0x80042F8Cu);
            uint64_t neg2   = (uint64_t)(uint32_t)-2;
            uc_reg_write(uc, UC_MIPS_REG_PC, &new_pc);
            uc_reg_write(uc, UC_MIPS_REG_V0, &neg2);
        }
        /* If t3 is zero-extended (0x0000000080xxxxxx) instead of sign-extended
         * (0xFFFFFFFF80xxxxxx), the jalr jumps to user-space and causes a TLB
         * miss storm.  Force sign-extension. */
        if ((t3_full >> 32) == 0u && (uint32_t)t3_full >= 0x80000000u) {
            uint64_t fixed_t3 = mips_sext((uint32_t)t3_full);
            uc_reg_write(uc, UC_MIPS_REG_T3, &fixed_t3);
            fprintf(stderr,
                    "[JALR_T3_FIX] zero-extended t3=0x%016" PRIX64
                    " -> sign-extended 0x%016" PRIX64 "\n",
                    t3_full, fixed_t3);
        }
    }

    /*
     * do_early_param NULL fn-pointer intercept (linux4be20040908/vmlinux).
     *
     * do_early_param() at 0x80272450 is the "unknown param" callback passed
     * to parse_args.  It walks the __setup table and for each matching entry
     * calls entry.fn via:
     *
     *   802724d0:  lw  v0, 4(s1)    ; v0 = entry.fn (function pointer)
     *   802724d4:  jalr v0           ; call entry.fn(val)
     *   802724d8:  nop               ; delay slot
     *   802724dc:  addiu a0,s2,-3096 ; next insn (beqz v0 at 802724e0 skips
     *   802724e0:  beqz v0,802724f0  ;  the printk if fn returned 0)
     *
     * Some __setup entries have a NULL fn (e.g., obsolete/empty entries).
     * When v0==0 the jalr jumps to VA=0x00000000 causing a TLB-miss storm.
     *
     * Fix: at 0x802724D4, if v0==0, skip the call by redirecting to
     * 0x802724DC (the instruction after the delay slot) with v0 unchanged
     * (=0), which is treated as "param not handled" by the beqz at 802724e0.
     */
    if ((uint32_t)address == 0x802724D4u) {
        uint64_t v0 = 0;
        uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
        static uint32_t ep_jalrv0_log = 0;
        if (ep_jalrv0_log < 32u) {
            uint64_t s1 = 0, a0 = 0;
            uc_reg_read(uc, UC_MIPS_REG_S1, &s1);
            uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
            fprintf(stderr,
                    "[EP_JALRV0] #%u v0=0x%08" PRIX64 " s1=0x%08" PRIX64
                    " a0=0x%08" PRIX64 "%s\n",
                    ep_jalrv0_log,
                    (uint64_t)(uint32_t)v0, (uint64_t)(uint32_t)s1,
                    (uint64_t)(uint32_t)a0,
                    (uint32_t)v0 == 0u ? " <NULL fn — will skip>" : "");
            ep_jalrv0_log++;
        }
        if ((uint32_t)v0 == 0u) {
            /* NULL fn pointer in __setup table: skip the call.
             * v0 stays 0 → beqz v0 at 802724e0 takes the "skip printk" branch */
            uint64_t new_pc = mips_sext(0x802724DCu);
            uc_reg_write(uc, UC_MIPS_REG_PC, &new_pc);
        }
    }

    /*
     * Keep execve_last_gpr as an ENTRY snapshot for retry restore.
     * Do not overwrite it while do_execve runs; retries must roll back
     * caller/callee-saved state to the original call boundary.
     */

    /* Precise do_execve return probe: capture at function entry, log at caller PC. */
    if (m->execve_watch_active &&
        (uint32_t)address == (uint32_t)m->execve_watch_ret_pc) {
        uint64_t v0 = 0, a3 = 0;
        uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
        uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
        if (do_execve_ret_log < 128) {
            fprintf(stderr,
                    "[DO_EXECVE_RET] pc=0x%08" PRIX64 " path_ptr=0x%08" PRIX64
                    " \"%s\" v0=0x%08" PRIX64 " a3=0x%08" PRIX64
                    " pending_sys_nr=%u open_exec_calls=%u\n",
                    (uint64_t)(uint32_t)address,
                    (uint64_t)(uint32_t)m->execve_watch_a0,
                    m->execve_watch_path,
                    (uint64_t)(uint32_t)v0,
                    (uint64_t)(uint32_t)a3,
                    m->pending_syscall_nr,
                    open_exec_call_count);
            do_execve_ret_log++;
        }
        /* When ENFILE (-23): dump files_stat to diagnose nr_files vs max_files.
         * files_stat.nr_files lives at 0x80178104 (confirmed from get_empty_filp
         * disassembly: addiu s1,0x8018,-32508 → s1=0x80178104; lw v1,0(s1)).
         * The struct is { int nr_files; int nr_free_files; int max_files; }.        */
        if ((int32_t)(uint32_t)v0 == -23) {
            uint32_t fs[3] = {0, 0, 0};
            if (uc_mem_read(m->uc, 0x80178104u, fs, sizeof(fs)) == UC_ERR_OK) {
                fprintf(stderr,
                        "[ENFILE_PROBE] nr_files=%u nr_free=%u max_files=%u"
                        " open_exec_calls=%u\n",
                        fs[0], fs[1], fs[2], open_exec_call_count);
            } else {
                fprintf(stderr, "[ENFILE_PROBE] uc_mem_read(0x80178104) failed\n");
            }
        }
        m->execve_watch_active = false;
    }

    if (m->do_no_page_watch_active &&
        (uint32_t)address == (uint32_t)m->do_no_page_watch_ra) {
        uint64_t v0 = 0, a3 = 0;
        uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
        uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
        uint32_t pte = 0;
        bool have_pte = (m->do_no_page_watch_pte_ptr != 0u) &&
                        read_guest_u32(uc, m->do_no_page_watch_pte_ptr, &pte);
        uint32_t pa_page = (pte & 0xFFFFF000u) >> 2;
        uint8_t pa_bytes[16] = {0};
        uint8_t kseg_bytes[16] = {0};
        uint8_t va_bytes[16] = {0};
        uint8_t pa_off_bytes[16] = {0};
        uc_err pa_err = UC_ERR_READ_UNMAPPED;
        uc_err kseg_err = UC_ERR_READ_UNMAPPED;
        uc_err pa_off_err = UC_ERR_READ_UNMAPPED;
        uint32_t entry_off = m->do_no_page_watch_addr & 0xFFFu;
        uc_err va_err = uc_mem_read(uc, (uint64_t)m->do_no_page_watch_addr,
                                    va_bytes, sizeof(va_bytes));
        if (have_pte && pa_page != 0u) {
            pa_err = uc_mem_read(uc, (uint64_t)pa_page, pa_bytes, sizeof(pa_bytes));
            kseg_err = uc_mem_read(uc, mips_sext(0x80000000u | pa_page),
                                   kseg_bytes, sizeof(kseg_bytes));
            pa_off_err = uc_mem_read(uc, (uint64_t)pa_page + entry_off,
                                     pa_off_bytes, sizeof(pa_off_bytes));
        }

        char pa_hex[16 * 3 + 1];
        char kseg_hex[16 * 3 + 1];
        char va_hex[16 * 3 + 1];
        char pa_off_hex[16 * 3 + 1];
        format_hex_bytes(pa_bytes, sizeof(pa_bytes), pa_hex, sizeof(pa_hex));
        format_hex_bytes(kseg_bytes, sizeof(kseg_bytes), kseg_hex, sizeof(kseg_hex));
        format_hex_bytes(va_bytes, sizeof(va_bytes), va_hex, sizeof(va_hex));
        format_hex_bytes(pa_off_bytes, sizeof(pa_off_bytes), pa_off_hex, sizeof(pa_off_hex));
        static uint32_t do_no_page_ret_log = 0;
        if (do_no_page_ret_log < 128) {
            fprintf(stderr,
                    "[HANDOFF_DO_NO_PAGE_RET] pc=0x%08" PRIX64
                    " ra=0x%08" PRIX64 " fault_addr=0x%08X v0=0x%08" PRIX64
                    " a3=0x%08" PRIX64 " pte_ptr=0x%08" PRIX64 " pte=0x%08X pa_page=0x%08X"
                    " pa_err=%d kseg_err=%d va_err=%d pa=[%s] kseg=[%s] va=[%s]"
                    " off=0x%03X pa_off_err=%d pa_off=[%s]\n",
                    (uint64_t)(uint32_t)address,
                    (uint64_t)(uint32_t)m->do_no_page_watch_ra,
                    m->do_no_page_watch_addr,
                    (uint64_t)(uint32_t)v0,
                    (uint64_t)(uint32_t)a3,
                    (uint64_t)(uint32_t)m->do_no_page_watch_pte_ptr,
                    have_pte ? pte : 0u,
                    pa_page,
                    pa_err, kseg_err, va_err,
                    pa_hex, kseg_hex, va_hex,
                    entry_off, pa_off_err, pa_off_hex);
            do_no_page_ret_log++;
        }
        m->do_no_page_watch_active = false;
        m->do_no_page_watch_ra = 0;
        m->do_no_page_watch_addr = 0;
        m->do_no_page_watch_pte_ptr = 0;
    }

    if (m->filemap_nopage_watch_active &&
        (uint32_t)address == (uint32_t)m->filemap_nopage_watch_ra) {
        uint64_t v0 = 0;
        uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
        uint32_t page_flags = 0;
        uint32_t page_ptr52 = 0;
        uint32_t page_ptr56 = 0;
        uint32_t page_ptr60 = 0;
        bool have_page_flags = ((uint32_t)v0 >= 0x80000000u) &&
                               read_guest_u32(uc, v0 + 24u, &page_flags);
        bool have_page_ptr52 = ((uint32_t)v0 >= 0x80000000u) &&
                               read_guest_u32(uc, v0 + 52u, &page_ptr52);
        bool have_page_ptr56 = ((uint32_t)v0 >= 0x80000000u) &&
                               read_guest_u32(uc, v0 + 56u, &page_ptr56);
        bool have_page_ptr60 = ((uint32_t)v0 >= 0x80000000u) &&
                               read_guest_u32(uc, v0 + 60u, &page_ptr60);
        uint32_t mem_map = 0, pfn = 0, pa_page = 0;
        bool have_pa = ((uint32_t)v0 >= 0x80000000u) &&
                       page_struct_ptr_to_pa(uc, (uint32_t)v0, &mem_map, &pfn, &pa_page);
        uint8_t pa_bytes[16] = {0};
        uint8_t pa_a00_bytes[16] = {0};
        uc_err pa_err = UC_ERR_READ_UNMAPPED;
        uc_err pa_a00_err = UC_ERR_READ_UNMAPPED;
        if (have_pa)
            pa_err = uc_mem_read(uc, (uint64_t)pa_page, pa_bytes, sizeof(pa_bytes));
        if (have_pa)
            pa_a00_err = uc_mem_read(uc, (uint64_t)pa_page + 0xA00u,
                                     pa_a00_bytes, sizeof(pa_a00_bytes));
        char pa_hex[16 * 3 + 1];
        char pa_a00_hex[16 * 3 + 1];
        format_hex_bytes(pa_bytes, sizeof(pa_bytes), pa_hex, sizeof(pa_hex));
        format_hex_bytes(pa_a00_bytes, sizeof(pa_a00_bytes), pa_a00_hex, sizeof(pa_a00_hex));
        static uint32_t filemap_ret_log = 0;
        if (filemap_ret_log < 128) {
            fprintf(stderr,
                    "[HANDOFF_FILEMAP_NOPAGE_RET] pc=0x%08" PRIX64
                    " ra=0x%08" PRIX64 " fault_addr=0x%08X page=0x%08" PRIX64
                    " page_flags=%s0x%08X mem_map=%s0x%08X pfn=%s0x%X pa=%s0x%08X"
                    " page+52=%s0x%08X page+56=%s0x%08X page+60=%s0x%08X"
                    " pa_err=%d pa=[%s] pa@a00_err=%d pa@a00=[%s]\n",
                    (uint64_t)(uint32_t)address,
                    (uint64_t)(uint32_t)m->filemap_nopage_watch_ra,
                    m->filemap_nopage_watch_addr,
                    (uint64_t)(uint32_t)v0,
                    have_page_flags ? "" : "?",
                    page_flags,
                    have_pa ? "" : "?", mem_map,
                    have_pa ? "" : "?", pfn,
                    have_pa ? "" : "?", pa_page,
                    have_page_ptr52 ? "" : "?", page_ptr52,
                    have_page_ptr56 ? "" : "?", page_ptr56,
                    have_page_ptr60 ? "" : "?", page_ptr60,
                    pa_err, pa_hex, pa_a00_err, pa_a00_hex);
            filemap_ret_log++;
        }
        m->filemap_nopage_watch_active = false;
        m->filemap_nopage_watch_ra = 0;
        m->filemap_nopage_watch_addr = 0;
    }

    bool trace_handoff_fault_path = trace_user_handoff_fault_path_active(m);
    bool trace_execve_filemap = m->cfg.trace_user_handoff &&
                                m->execve_watch_active &&
                                m->pending_syscall_nr == 4011u;
    if (trace_handoff_fault_path || trace_execve_filemap) {
        uint32_t pc32 = (uint32_t)address;
        uint32_t badv = (uint32_t)m->shadow_cp0_badvaddr;

        if (pc32 == K24_HANDLE_TLBL || pc32 == K24_NOPAGE_TLBL ||
            pc32 == K24_HANDLE_TLBS || pc32 == K24_DO_PAGE_FAULT) {
            static uint32_t handoff_fault_path_log = 0;
            if (handoff_fault_path_log < 192) {
                fprintf(stderr,
                        "[HANDOFF_FAULT_PATH] pc=0x%08X badv=0x%08X"
                        " entryhi=0x%08" PRIX64 " lo0=0x%08" PRIX64
                        " lo1=0x%08" PRIX64 " mask=0x%08" PRIX64 "\n",
                        pc32, badv,
                        (uint64_t)(uint32_t)m->shadow_cp0_entryhi,
                        (uint64_t)(uint32_t)m->shadow_cp0_entrylo0,
                        (uint64_t)(uint32_t)m->shadow_cp0_entrylo1,
                        (uint64_t)(uint32_t)m->shadow_cp0_pagemask);
                handoff_fault_path_log++;
            }
        }

        if (pc32 == K24_HANDLE_TLBL_PTE_LOAD || pc32 == K24_HANDLE_TLBL_PTE_STORE) {
            uint64_t k1 = 0, k0 = 0;
            uint32_t pte = 0;
            bool have_pte = false;
            uc_reg_read(uc, UC_MIPS_REG_K1, &k1);
            uc_reg_read(uc, UC_MIPS_REG_K0, &k0);
            if ((uint32_t)k1 != 0u)
                have_pte = read_guest_u32(uc, k1, &pte);
            static uint32_t handoff_tlbl_pte_log = 0;
            if (handoff_tlbl_pte_log < 192) {
                fprintf(stderr,
                        "[HANDOFF_TLBL_PTE] pc=0x%08X badv=0x%08X"
                        " pte_ptr=0x%08" PRIX64 " k0=0x%08" PRIX64
                        " mem_pte=%s0x%08X\n",
                        pc32, badv,
                        (uint64_t)(uint32_t)k1,
                        (uint64_t)(uint32_t)k0,
                        have_pte ? "" : "?",
                        pte);
                handoff_tlbl_pte_log++;
            }
        }

        if (pc32 == K24_FILEMAP_FIND_GET_PAGE_RET) {
            uint64_t v0 = 0, s1 = 0, s2 = 0, s5 = 0;
            uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
            uc_reg_read(uc, UC_MIPS_REG_S1, &s1);
            uc_reg_read(uc, UC_MIPS_REG_S2, &s2);
            uc_reg_read(uc, UC_MIPS_REG_S5, &s5);
            uint32_t mem_map = 0, pfn = 0, pa_page = 0;
            bool have_pa = ((uint32_t)v0 >= 0x80000000u) &&
                           page_struct_ptr_to_pa(uc, (uint32_t)v0, &mem_map, &pfn, &pa_page);
            uint32_t page_mapping = 0, page_index = 0, page_flags = 0;
            uint32_t page_ptr52 = 0, page_ptr56 = 0, page_ptr60 = 0;
            bool have_page_mapping = ((uint32_t)v0 >= 0x80000000u) &&
                                     read_guest_u32(uc, v0 + 8u, &page_mapping);
            bool have_page_index = ((uint32_t)v0 >= 0x80000000u) &&
                                   read_guest_u32(uc, v0 + 12u, &page_index);
            bool have_page_flags = ((uint32_t)v0 >= 0x80000000u) &&
                                   read_guest_u32(uc, v0 + 24u, &page_flags);
            bool have_page_ptr52 = ((uint32_t)v0 >= 0x80000000u) &&
                                   read_guest_u32(uc, v0 + 52u, &page_ptr52);
            bool have_page_ptr56 = ((uint32_t)v0 >= 0x80000000u) &&
                                   read_guest_u32(uc, v0 + 56u, &page_ptr56);
            bool have_page_ptr60 = ((uint32_t)v0 >= 0x80000000u) &&
                                   read_guest_u32(uc, v0 + 60u, &page_ptr60);
            uint32_t bh_blocknr = 0, bh_state = 0, bh_data = 0, bh_page = 0, bh_size_word = 0;
            bool have_bh_blocknr = (page_ptr52 >= 0x80000000u) &&
                                   read_guest_u32(uc, page_ptr52 + 4u, &bh_blocknr);
            bool have_bh_state = (page_ptr52 >= 0x80000000u) &&
                                 read_guest_u32(uc, page_ptr52 + 24u, &bh_state);
            bool have_bh_data = (page_ptr52 >= 0x80000000u) &&
                                read_guest_u32(uc, page_ptr52 + 52u, &bh_data);
            bool have_bh_page = (page_ptr52 >= 0x80000000u) &&
                                read_guest_u32(uc, page_ptr52 + 56u, &bh_page);
            bool have_bh_size = (page_ptr52 >= 0x80000000u) &&
                                read_guest_u32(uc, page_ptr52 + 8u, &bh_size_word);
            uint8_t bh_data_bytes[16] = {0};
            uc_err bh_data_err = UC_ERR_READ_UNMAPPED;
            if (have_bh_data && bh_data >= 0x80000000u)
                bh_data_err = uc_mem_read(uc, (uint64_t)bh_data, bh_data_bytes, sizeof(bh_data_bytes));
            uint8_t pa_bytes[16] = {0};
            uint8_t pa_400[16] = {0}, pa_800[16] = {0}, pa_a00[16] = {0};
            uc_err pa_err = UC_ERR_READ_UNMAPPED;
            uc_err pa_400_err = UC_ERR_READ_UNMAPPED;
            uc_err pa_800_err = UC_ERR_READ_UNMAPPED;
            uc_err pa_a00_err = UC_ERR_READ_UNMAPPED;
            if (have_pa)
                pa_err = uc_mem_read(uc, (uint64_t)pa_page, pa_bytes, sizeof(pa_bytes));
            if (have_pa)
                pa_400_err = uc_mem_read(uc, (uint64_t)pa_page + 0x400u, pa_400, sizeof(pa_400));
            if (have_pa)
                pa_800_err = uc_mem_read(uc, (uint64_t)pa_page + 0x800u, pa_800, sizeof(pa_800));
            if (have_pa)
                pa_a00_err = uc_mem_read(uc, (uint64_t)pa_page + 0xA00u, pa_a00, sizeof(pa_a00));
            char pa_hex[16 * 3 + 1];
            char pa_400_hex[16 * 3 + 1];
            char pa_800_hex[16 * 3 + 1];
            char pa_a00_hex[16 * 3 + 1];
            char bh_data_hex[16 * 3 + 1];
            format_hex_bytes(pa_bytes, sizeof(pa_bytes), pa_hex, sizeof(pa_hex));
            format_hex_bytes(pa_400, sizeof(pa_400), pa_400_hex, sizeof(pa_400_hex));
            format_hex_bytes(pa_800, sizeof(pa_800), pa_800_hex, sizeof(pa_800_hex));
            format_hex_bytes(pa_a00, sizeof(pa_a00), pa_a00_hex, sizeof(pa_a00_hex));
            format_hex_bytes(bh_data_bytes, sizeof(bh_data_bytes), bh_data_hex, sizeof(bh_data_hex));
            static uint32_t handoff_find_page_log = 0;
            if (handoff_find_page_log < 128) {
                fprintf(stderr,
                        "[HANDOFF_FIND_GET_PAGE_RET] pc=0x%08X badv=0x%08X"
                        " page=%s0x%08" PRIX64
                        " idx=0x%08" PRIX64 " end=0x%08" PRIX64 " map_arg=0x%08" PRIX64
                        " page.map=%s0x%08X page.idx=%s0x%08X page.flags=%s0x%08X"
                        " page+52=%s0x%08X page+56=%s0x%08X page+60=%s0x%08X"
                        " bh.block=%s0x%08X bh.size=%s0x%X bh.state=%s0x%08X"
                        " bh.data=%s0x%08X bh.page=%s0x%08X bh_data_err=%d bh_data=[%s]"
                        " mem_map=%s0x%08X pfn=%s0x%X pa=%s0x%08X pa_err=%d pa=[%s]"
                        " pa400_err=%d pa400=[%s] pa800_err=%d pa800=[%s]"
                        " paa00_err=%d paa00=[%s]\n",
                        pc32, badv,
                        ((uint32_t)v0 >= 0x80000000u) ? "" : "?",
                        (uint64_t)(uint32_t)v0,
                        (uint64_t)(uint32_t)s1,
                        (uint64_t)(uint32_t)s2,
                        (uint64_t)(uint32_t)s5,
                        have_page_mapping ? "" : "?", page_mapping,
                        have_page_index ? "" : "?", page_index,
                        have_page_flags ? "" : "?", page_flags,
                        have_page_ptr52 ? "" : "?", page_ptr52,
                        have_page_ptr56 ? "" : "?", page_ptr56,
                        have_page_ptr60 ? "" : "?", page_ptr60,
                        have_bh_blocknr ? "" : "?", bh_blocknr,
                        have_bh_size ? "" : "?", (bh_size_word & 0xFFFFu),
                        have_bh_state ? "" : "?", bh_state,
                        have_bh_data ? "" : "?", bh_data,
                        have_bh_page ? "" : "?", bh_page,
                        bh_data_err, bh_data_hex,
                        have_pa ? "" : "?", mem_map,
                        have_pa ? "" : "?", pfn,
                        have_pa ? "" : "?", pa_page,
                        pa_err, pa_hex,
                        pa_400_err, pa_400_hex,
                        pa_800_err, pa_800_hex,
                        pa_a00_err, pa_a00_hex);
                handoff_find_page_log++;
            }
        }

        if (pc32 == K24_FILEMAP_UPTODATE_CHECK &&
            m->filemap_nopage_watch_active &&
            m->filemap_nopage_watch_addr == 0x2AAA8000u) {
            uint64_t s0 = 0, s1 = 0, s5 = 0;
            uc_reg_read(uc, UC_MIPS_REG_S0, &s0); /* struct page* */
            uc_reg_read(uc, UC_MIPS_REG_S1, &s1); /* page index */
            uc_reg_read(uc, UC_MIPS_REG_S5, &s5); /* mapping */
            if ((uint32_t)s0 >= 0x80000000u) {
                uint32_t flags = 0;
                uint32_t mem_map = 0, pfn = 0, pa_page = 0;
                bool have_flags = read_guest_u32(uc, s0 + 24u, &flags);
                bool have_pa = page_struct_ptr_to_pa(uc, (uint32_t)s0,
                                                     &mem_map, &pfn, &pa_page);
                uint8_t pa0[16] = {0}, paa00[16] = {0};
                uc_err pa0_err = UC_ERR_READ_UNMAPPED;
                uc_err paa00_err = UC_ERR_READ_UNMAPPED;
                if (have_pa) {
                    pa0_err = uc_mem_read(uc, (uint64_t)pa_page, pa0, sizeof(pa0));
                    paa00_err = uc_mem_read(uc, (uint64_t)pa_page + 0xA00u,
                                            paa00, sizeof(paa00));
                }
                bool zero0 = true, zeroa00 = true;
                for (size_t i = 0; i < sizeof(pa0); i++) {
                    if (pa0[i] != 0u) {
                        zero0 = false;
                        break;
                    }
                }
                for (size_t i = 0; i < sizeof(paa00); i++) {
                    if (paa00[i] != 0u) {
                        zeroa00 = false;
                        break;
                    }
                }
                static uint32_t handoff_uptodate_fix_log = 0;
                if (have_flags && have_pa &&
                    (flags & (1u << 3)) != 0u &&
                    pa0_err == UC_ERR_OK && paa00_err == UC_ERR_OK &&
                    zero0 && zeroa00) {
                    uint32_t new_flags = flags & ~(1u << 3); /* clear PG_uptodate */
                    uc_err werr = write_mem_best_effort(uc, s0 + 24u,
                                                        &new_flags, sizeof(new_flags));
                    if (handoff_uptodate_fix_log < 64) {
                        fprintf(stderr,
                                "[HANDOFF_UPTODATE_CLEAR] pc=0x%08X page=0x%08X"
                                " map=0x%08X idx=0x%08X mem_map=0x%08X pfn=0x%X pa=0x%08X"
                                " flags=0x%08X->0x%08X werr=%d\n",
                                pc32, (uint32_t)s0, (uint32_t)s5, (uint32_t)s1,
                                mem_map, pfn, pa_page, flags, new_flags, werr);
                        handoff_uptodate_fix_log++;
                    }
                }
            }
        }

        if (pc32 == K24_FILEMAP_UPTODATE_BRANCH &&
            m->filemap_nopage_watch_active &&
            m->filemap_nopage_watch_addr == 0x2AAA8000u) {
            uint64_t s0 = 0;
            uc_reg_read(uc, UC_MIPS_REG_S0, &s0);
            uint32_t mem_map = 0, pfn = 0, pa_page = 0;
            bool have_pa = ((uint32_t)s0 >= 0x80000000u) &&
                           page_struct_ptr_to_pa(uc, (uint32_t)s0,
                                                 &mem_map, &pfn, &pa_page);
            uint8_t pa0[16] = {0}, paa00[16] = {0};
            uc_err pa0_err = UC_ERR_READ_UNMAPPED;
            uc_err paa00_err = UC_ERR_READ_UNMAPPED;
            if (have_pa) {
                pa0_err = uc_mem_read(uc, (uint64_t)pa_page, pa0, sizeof(pa0));
                paa00_err = uc_mem_read(uc, (uint64_t)pa_page + 0xA00u,
                                        paa00, sizeof(paa00));
            }
            bool zero0 = true, zeroa00 = true;
            for (size_t i = 0; i < sizeof(pa0); i++) {
                if (pa0[i] != 0u) { zero0 = false; break; }
            }
            for (size_t i = 0; i < sizeof(paa00); i++) {
                if (paa00[i] != 0u) { zeroa00 = false; break; }
            }
            if (have_pa && pa0_err == UC_ERR_OK && paa00_err == UC_ERR_OK &&
                zero0 && zeroa00) {
                uint64_t force_not_uptodate = 0;
                uc_reg_write(uc, UC_MIPS_REG_V0, &force_not_uptodate);
                static uint32_t handoff_uptodate_branch_force_log = 0;
                if (handoff_uptodate_branch_force_log < 64) {
                    fprintf(stderr,
                            "[HANDOFF_UPTODATE_BRANCH_FORCE] pc=0x%08X page=0x%08X"
                            " pfn=0x%X pa=0x%08X v0->0\n",
                            pc32, (uint32_t)s0, pfn, pa_page);
                    handoff_uptodate_branch_force_log++;
                }
            }
        }

        if (pc32 == K24_FILEMAP_UPTODATE_RECHECK &&
            m->filemap_nopage_watch_active &&
            m->filemap_nopage_watch_addr == 0x2AAA8000u) {
            uint64_t s0 = 0;
            uc_reg_read(uc, UC_MIPS_REG_S0, &s0);
            uint32_t mem_map = 0, pfn = 0, pa_page = 0;
            bool have_pa = ((uint32_t)s0 >= 0x80000000u) &&
                           page_struct_ptr_to_pa(uc, (uint32_t)s0,
                                                 &mem_map, &pfn, &pa_page);
            uint8_t pa0[16] = {0}, paa00[16] = {0};
            uc_err pa0_err = UC_ERR_READ_UNMAPPED;
            uc_err paa00_err = UC_ERR_READ_UNMAPPED;
            if (have_pa) {
                pa0_err = uc_mem_read(uc, (uint64_t)pa_page, pa0, sizeof(pa0));
                paa00_err = uc_mem_read(uc, (uint64_t)pa_page + 0xA00u,
                                        paa00, sizeof(paa00));
            }
            bool zero0 = true, zeroa00 = true;
            for (size_t i = 0; i < sizeof(pa0); i++) {
                if (pa0[i] != 0u) { zero0 = false; break; }
            }
            for (size_t i = 0; i < sizeof(paa00); i++) {
                if (paa00[i] != 0u) { zeroa00 = false; break; }
            }
            if (have_pa && pa0_err == UC_ERR_OK && paa00_err == UC_ERR_OK &&
                zero0 && zeroa00) {
                uint32_t flags = 0;
                if (read_guest_u32(uc, s0 + 24u, &flags)) {
                    uint32_t new_flags = flags & ~(1u << 3);
                    (void)write_mem_best_effort(uc, s0 + 24u,
                                                &new_flags, sizeof(new_flags));
                }
                uint64_t force_not_uptodate = 0;
                uc_reg_write(uc, UC_MIPS_REG_V0, &force_not_uptodate);
                static uint32_t handoff_uptodate_recheck_force_log = 0;
                if (handoff_uptodate_recheck_force_log < 64) {
                    fprintf(stderr,
                            "[HANDOFF_UPTODATE_RECHECK_FORCE] pc=0x%08X page=0x%08X"
                            " pfn=0x%X pa=0x%08X v0->0\n",
                            pc32, (uint32_t)s0, pfn, pa_page);
                    handoff_uptodate_recheck_force_log++;
                }
            }
        }

        if (pc32 == K24_FILEMAP_READPAGE_RET &&
            m->filemap_nopage_watch_active &&
            m->filemap_nopage_watch_addr == 0x2AAA8000u) {
            uint64_t v0 = 0, s0 = 0, s1 = 0, s5 = 0;
            uc_reg_read(uc, UC_MIPS_REG_V0, &v0); /* readpage() return */
            uc_reg_read(uc, UC_MIPS_REG_S0, &s0); /* struct page* */
            uc_reg_read(uc, UC_MIPS_REG_S1, &s1); /* idx */
            uc_reg_read(uc, UC_MIPS_REG_S5, &s5); /* mapping */
            uint32_t flags = 0;
            bool have_flags = ((uint32_t)s0 >= 0x80000000u) &&
                              read_guest_u32(uc, s0 + 24u, &flags);
            uint32_t mem_map = 0, pfn = 0, pa_page = 0;
            bool have_pa = ((uint32_t)s0 >= 0x80000000u) &&
                           page_struct_ptr_to_pa(uc, (uint32_t)s0,
                                                 &mem_map, &pfn, &pa_page);
            uint8_t pa0[16] = {0}, paa00[16] = {0};
            uc_err pa0_err = UC_ERR_READ_UNMAPPED;
            uc_err paa00_err = UC_ERR_READ_UNMAPPED;
            if (have_pa) {
                pa0_err = uc_mem_read(uc, (uint64_t)pa_page, pa0, sizeof(pa0));
                paa00_err = uc_mem_read(uc, (uint64_t)pa_page + 0xA00u,
                                        paa00, sizeof(paa00));
            }
            char pa0_hex[16 * 3 + 1];
            char paa00_hex[16 * 3 + 1];
            format_hex_bytes(pa0, sizeof(pa0), pa0_hex, sizeof(pa0_hex));
            format_hex_bytes(paa00, sizeof(paa00), paa00_hex, sizeof(paa00_hex));
            static uint32_t handoff_readpage_ret_log = 0;
            if (handoff_readpage_ret_log < 64) {
                fprintf(stderr,
                        "[HANDOFF_READPAGE_RET] pc=0x%08X ret=%d page=0x%08X"
                        " map=0x%08X idx=0x%08X flags=%s0x%08X"
                        " mem_map=%s0x%08X pfn=%s0x%X pa=%s0x%08X"
                        " pa0_err=%d pa0=[%s] paa00_err=%d paa00=[%s]\n",
                        pc32, (int32_t)(uint32_t)v0, (uint32_t)s0,
                        (uint32_t)s5, (uint32_t)s1,
                        have_flags ? "" : "?", flags,
                        have_pa ? "" : "?", mem_map,
                        have_pa ? "" : "?", pfn,
                        have_pa ? "" : "?", pa_page,
                        pa0_err, pa0_hex, paa00_err, paa00_hex);
                handoff_readpage_ret_log++;
            }
        }

        if ((pc32 == K24_FILEMAP_LOCKPAGE_CALL ||
             pc32 == K24_FILEMAP_MAPPING_CHECK ||
             pc32 == K24_FILEMAP_UPTODATE_RECHECK ||
             pc32 == K24_FILEMAP_READPAGE_CALL ||
             pc32 == K24_FILEMAP_SKIP_READPAGE) &&
            m->filemap_nopage_watch_active &&
            m->filemap_nopage_watch_addr == 0x2AAA8000u) {
            uint64_t s0 = 0, s1 = 0, s5 = 0, v0 = 0;
            uc_reg_read(uc, UC_MIPS_REG_S0, &s0);
            uc_reg_read(uc, UC_MIPS_REG_S1, &s1);
            uc_reg_read(uc, UC_MIPS_REG_S5, &s5);
            uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
            uint32_t page_flags = 0, page_mapping = 0;
            bool have_flags = ((uint32_t)s0 >= 0x80000000u) &&
                              read_guest_u32(uc, s0 + 24u, &page_flags);
            bool have_mapping = ((uint32_t)s0 >= 0x80000000u) &&
                                read_guest_u32(uc, s0 + 8u, &page_mapping);
            static uint32_t handoff_readpage_path_log = 0;
            if (handoff_readpage_path_log < 128) {
                fprintf(stderr,
                        "[HANDOFF_READPAGE_PATH] pc=0x%08X page=0x%08X map=0x%08X idx=0x%08X"
                        " v0=0x%08X page.map=%s0x%08X page.flags=%s0x%08X\n",
                        pc32, (uint32_t)s0, (uint32_t)s5, (uint32_t)s1,
                        (uint32_t)v0,
                        have_mapping ? "" : "?", page_mapping,
                        have_flags ? "" : "?", page_flags);
                handoff_readpage_path_log++;
            }
        }

        if (pc32 == K24_FILEMAP_PAGE_CACHE_READ_RET ||
            pc32 == K24_FILEMAP_READ_CLUSTER_RET) {
            uint64_t v0 = 0, s1 = 0, s2 = 0, s5 = 0;
            uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
            uc_reg_read(uc, UC_MIPS_REG_S1, &s1);
            uc_reg_read(uc, UC_MIPS_REG_S2, &s2);
            uc_reg_read(uc, UC_MIPS_REG_S5, &s5);
            const char *tag = (pc32 == K24_FILEMAP_PAGE_CACHE_READ_RET)
                            ? "HANDOFF_PAGE_CACHE_READ_RET"
                            : "HANDOFF_READ_CLUSTER_RET";
            static uint32_t handoff_cache_read_log = 0;
            if (handoff_cache_read_log < 128) {
                fprintf(stderr,
                        "[%s] pc=0x%08X badv=0x%08X ret=0x%08" PRIX64
                        " idx=0x%08" PRIX64 " end=0x%08" PRIX64
                        " mapping=0x%08" PRIX64 "\n",
                        tag, pc32, badv, (uint64_t)(uint32_t)v0,
                        (uint64_t)(uint32_t)s1,
                        (uint64_t)(uint32_t)s2,
                        (uint64_t)(uint32_t)s5);
                handoff_cache_read_log++;
            }
        }

        if ((pc32 == K24_BLOCK_READ_SUBMIT_BH_CALL ||
             pc32 == K24_BLOCK_READ_SUBMIT_BH_RET ||
             pc32 == K24_BLOCK_READ_GET_BLOCK_CALL ||
             pc32 == K24_BLOCK_READ_GET_BLOCK_RET ||
             pc32 == K24_BLOCK_READ_MEMSET_HOLE ||
             pc32 == K24_BLOCK_READ_RET) &&
            m->filemap_nopage_watch_active &&
            m->filemap_nopage_watch_addr == 0x2AAA8000u) {
            uint64_t a0 = 0, a1 = 0, a2 = 0, a3 = 0, v0 = 0, v1 = 0;
            uint64_t s0 = 0, s1 = 0, s3 = 0, s5 = 0, s8 = 0;
            uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
            uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
            uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
            uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
            uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
            uc_reg_read(uc, UC_MIPS_REG_V1, &v1);
            uc_reg_read(uc, UC_MIPS_REG_S0, &s0);
            uc_reg_read(uc, UC_MIPS_REG_S1, &s1);
            uc_reg_read(uc, UC_MIPS_REG_S3, &s3);
            uc_reg_read(uc, UC_MIPS_REG_S5, &s5);
            uc_reg_read(uc, UC_MIPS_REG_S8, &s8);

            uint32_t bh_ptr = 0;
            if (pc32 == K24_BLOCK_READ_SUBMIT_BH_CALL ||
                pc32 == K24_BLOCK_READ_SUBMIT_BH_RET) {
                bh_ptr = (uint32_t)a1;
            } else {
                bh_ptr = (uint32_t)s0;
            }

            uint32_t bh_blocknr = 0, bh_state = 0, bh_data = 0;
            uint32_t bh_page = 0, bh_size_word = 0, bh_endio = 0;
            bool have_bh_blocknr = (bh_ptr >= 0x80000000u) &&
                                   read_guest_u32(uc, (uint64_t)bh_ptr + 4u, &bh_blocknr);
            bool have_bh_size = (bh_ptr >= 0x80000000u) &&
                                read_guest_u32(uc, (uint64_t)bh_ptr + 8u, &bh_size_word);
            bool have_bh_state = (bh_ptr >= 0x80000000u) &&
                                 read_guest_u32(uc, (uint64_t)bh_ptr + 24u, &bh_state);
            bool have_bh_data = (bh_ptr >= 0x80000000u) &&
                                read_guest_u32(uc, (uint64_t)bh_ptr + 52u, &bh_data);
            bool have_bh_page = (bh_ptr >= 0x80000000u) &&
                                read_guest_u32(uc, (uint64_t)bh_ptr + 56u, &bh_page);
            bool have_bh_endio = (bh_ptr >= 0x80000000u) &&
                                 read_guest_u32(uc, (uint64_t)bh_ptr + 60u, &bh_endio);

            uint8_t bh_data_bytes[16] = {0};
            uc_err bh_data_err = UC_ERR_READ_UNMAPPED;
            if (have_bh_data && bh_data >= 0x80000000u)
                bh_data_err = uc_mem_read(uc, (uint64_t)bh_data, bh_data_bytes, sizeof(bh_data_bytes));

            uint32_t mem_map = 0, pfn = 0, pa_page = 0;
            bool have_pa = ((uint32_t)s3 >= 0x80000000u) &&
                           page_struct_ptr_to_pa(uc, (uint32_t)s3, &mem_map, &pfn, &pa_page);
            uint8_t pa0[16] = {0}, paa00[16] = {0};
            uc_err pa0_err = UC_ERR_READ_UNMAPPED;
            uc_err paa00_err = UC_ERR_READ_UNMAPPED;
            if (have_pa) {
                pa0_err = uc_mem_read(uc, (uint64_t)pa_page, pa0, sizeof(pa0));
                paa00_err = uc_mem_read(uc, (uint64_t)pa_page + 0xA00u,
                                        paa00, sizeof(paa00));
            }

            char bh_data_hex[16 * 3 + 1];
            char pa0_hex[16 * 3 + 1];
            char paa00_hex[16 * 3 + 1];
            format_hex_bytes(bh_data_bytes, sizeof(bh_data_bytes), bh_data_hex, sizeof(bh_data_hex));
            format_hex_bytes(pa0, sizeof(pa0), pa0_hex, sizeof(pa0_hex));
            format_hex_bytes(paa00, sizeof(paa00), paa00_hex, sizeof(paa00_hex));

            const char *tag = "HANDOFF_BLOCK_READ";
            if (pc32 == K24_BLOCK_READ_SUBMIT_BH_CALL)
                tag = "HANDOFF_SUBMIT_BH_CALL";
            else if (pc32 == K24_BLOCK_READ_SUBMIT_BH_RET)
                tag = "HANDOFF_SUBMIT_BH_RET";
            else if (pc32 == K24_BLOCK_READ_GET_BLOCK_CALL)
                tag = "HANDOFF_GET_BLOCK_CALL";
            else if (pc32 == K24_BLOCK_READ_GET_BLOCK_RET)
                tag = "HANDOFF_GET_BLOCK_RET";
            else if (pc32 == K24_BLOCK_READ_MEMSET_HOLE)
                tag = "HANDOFF_BLOCK_HOLE_ZERO";
            else if (pc32 == K24_BLOCK_READ_RET)
                tag = "HANDOFF_BLOCK_READ_RET";

            static uint32_t handoff_block_io_log = 0;
            if (handoff_block_io_log < 256) {
                fprintf(stderr,
                        "[%s] pc=0x%08X page=0x%08X idx=0x%08X map=0x%08X"
                        " a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X v0=0x%08X v1=0x%08X"
                        " s8=0x%08X bh=%s0x%08X"
                        " bh.block=%s0x%08X bh.size=%s0x%X bh.state=%s0x%08X"
                        " bh.data=%s0x%08X bh.page=%s0x%08X bh.endio=%s0x%08X"
                        " bh_data_err=%d bh_data=[%s]"
                        " mem_map=%s0x%08X pfn=%s0x%X pa=%s0x%08X"
                        " pa0_err=%d pa0=[%s] paa00_err=%d paa00=[%s]\n",
                        tag, pc32,
                        (uint32_t)s3, (uint32_t)s1, (uint32_t)s5,
                        (uint32_t)a0, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3,
                        (uint32_t)v0, (uint32_t)v1, (uint32_t)s8,
                        (bh_ptr >= 0x80000000u) ? "" : "?",
                        bh_ptr,
                        have_bh_blocknr ? "" : "?", bh_blocknr,
                        have_bh_size ? "" : "?", (bh_size_word & 0xFFFFu),
                        have_bh_state ? "" : "?", bh_state,
                        have_bh_data ? "" : "?", bh_data,
                        have_bh_page ? "" : "?", bh_page,
                        have_bh_endio ? "" : "?", bh_endio,
                        bh_data_err, bh_data_hex,
                        have_pa ? "" : "?", mem_map,
                        have_pa ? "" : "?", pfn,
                        have_pa ? "" : "?", pa_page,
                        pa0_err, pa0_hex, paa00_err, paa00_hex);
                handoff_block_io_log++;
            }
        }

        if (pc32 == K24_EXT2_GET_BLOCK_RET &&
            m->filemap_nopage_watch_active &&
            m->filemap_nopage_watch_addr == 0x2AAA8000u) {
            uint64_t sp = 0, s4 = 0, s6 = 0, s8 = 0;
            uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
            uc_reg_read(uc, UC_MIPS_REG_S4, &s4); /* inode */
            uc_reg_read(uc, UC_MIPS_REG_S6, &s6); /* depth */
            uc_reg_read(uc, UC_MIPS_REG_S8, &s8); /* iblock */

            uint32_t ret_slot = 0, bh_ptr = 0, create = 0;
            bool have_ret_slot = read_guest_u32(uc, sp + 88u, &ret_slot);
            bool have_bh_ptr = read_guest_u32(uc, sp + 144u, &bh_ptr);
            bool have_create = read_guest_u32(uc, sp + 148u, &create);

            uint32_t bh_blocknr = 0, bh_state = 0, bh_data = 0, bh_page = 0;
            uint32_t bh_size_word = 0, bh_endio = 0;
            bool have_bh_blocknr = (bh_ptr >= 0x80000000u) &&
                                   read_guest_u32(uc, (uint64_t)bh_ptr + 4u, &bh_blocknr);
            bool have_bh_size = (bh_ptr >= 0x80000000u) &&
                                read_guest_u32(uc, (uint64_t)bh_ptr + 8u, &bh_size_word);
            bool have_bh_state = (bh_ptr >= 0x80000000u) &&
                                 read_guest_u32(uc, (uint64_t)bh_ptr + 24u, &bh_state);
            bool have_bh_data = (bh_ptr >= 0x80000000u) &&
                                read_guest_u32(uc, (uint64_t)bh_ptr + 52u, &bh_data);
            bool have_bh_page = (bh_ptr >= 0x80000000u) &&
                                read_guest_u32(uc, (uint64_t)bh_ptr + 56u, &bh_page);
            bool have_bh_endio = (bh_ptr >= 0x80000000u) &&
                                 read_guest_u32(uc, (uint64_t)bh_ptr + 60u, &bh_endio);

            uint8_t bh_data_bytes[16] = {0};
            uc_err bh_data_err = UC_ERR_READ_UNMAPPED;
            if (have_bh_data && bh_data >= 0x80000000u)
                bh_data_err = uc_mem_read(uc, (uint64_t)bh_data, bh_data_bytes, sizeof(bh_data_bytes));
            char bh_data_hex[16 * 3 + 1];
            format_hex_bytes(bh_data_bytes, sizeof(bh_data_bytes), bh_data_hex, sizeof(bh_data_hex));

            static uint32_t handoff_ext2_get_block_log = 0;
            if (handoff_ext2_get_block_log < 192) {
                fprintf(stderr,
                        "[HANDOFF_EXT2_GET_BLOCK_RET] pc=0x%08X inode=%s0x%08X"
                        " iblock=0x%08X depth=%u ret=%s%d create=%s%u bh=%s0x%08X"
                        " bh.block=%s0x%08X bh.size=%s0x%X bh.state=%s0x%08X"
                        " bh.data=%s0x%08X bh.page=%s0x%08X bh.endio=%s0x%08X"
                        " bh_data_err=%d bh_data=[%s]\n",
                        pc32,
                        ((uint32_t)s4 >= 0x80000000u) ? "" : "?",
                        (uint32_t)s4,
                        (uint32_t)s8, (uint32_t)s6,
                        have_ret_slot ? "" : "?", (int32_t)ret_slot,
                        have_create ? "" : "?", create,
                        have_bh_ptr ? "" : "?", bh_ptr,
                        have_bh_blocknr ? "" : "?", bh_blocknr,
                        have_bh_size ? "" : "?", (bh_size_word & 0xFFFFu),
                        have_bh_state ? "" : "?", bh_state,
                        have_bh_data ? "" : "?", bh_data,
                        have_bh_page ? "" : "?", bh_page,
                        have_bh_endio ? "" : "?", bh_endio,
                        bh_data_err, bh_data_hex);
                handoff_ext2_get_block_log++;
            }
        }

        if (pc32 == K24_DO_NO_PAGE) {
            uint64_t ra = 0, a2 = 0, a3 = 0, sp = 0;
            uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
            uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
            uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
            uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
            uint32_t pte_ptr = 0;
            (void)read_guest_u32(uc, sp + 16u, &pte_ptr);
            m->do_no_page_watch_active = true;
            m->do_no_page_watch_ra = ra;
            m->do_no_page_watch_addr = (uint32_t)a2;
            m->do_no_page_watch_pte_ptr = mips_sext(pte_ptr);
            static uint32_t do_no_page_entry_log = 0;
            if (do_no_page_entry_log < 128) {
                fprintf(stderr,
                        "[HANDOFF_DO_NO_PAGE_IN] pc=0x%08X badv=0x%08X"
                        " addr=0x%08" PRIX64 " write=%u ra=0x%08" PRIX64
                        " sp=0x%08" PRIX64 " pte_ptr=0x%08X\n",
                        pc32, badv,
                        (uint64_t)(uint32_t)a2,
                        ((uint32_t)a3 != 0u) ? 1u : 0u,
                        (uint64_t)(uint32_t)ra,
                        (uint64_t)(uint32_t)sp,
                        pte_ptr);
                do_no_page_entry_log++;
            }
        } else if (pc32 == K24_FILEMAP_NOPAGE) {
            uint64_t ra = 0, a1 = 0;
            uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
            uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
            m->filemap_nopage_watch_active = true;
            m->filemap_nopage_watch_ra = ra;
            m->filemap_nopage_watch_addr = (uint32_t)a1;
            static uint32_t filemap_entry_log = 0;
            if (filemap_entry_log < 128) {
                fprintf(stderr,
                        "[HANDOFF_FILEMAP_NOPAGE_IN] pc=0x%08X badv=0x%08X"
                        " addr=0x%08" PRIX64 " ra=0x%08" PRIX64 "\n",
                        pc32, badv,
                        (uint64_t)(uint32_t)a1,
                        (uint64_t)(uint32_t)ra);
                filemap_entry_log++;
            }
        }
    }

    /* Per-instruction exception vector probes for TLB storm debugging. */
    if (tlb_trace_window_active(m)) {
        if (((uint32_t)address & 0xFFFFFF80u) == 0x80000000u) {
            if (m->refill_insn_count < 1024) {
                fprintf(stderr, "[VEC_PROBE_PER_INSN] refill PC=0x%08" PRIX64 "\n", address);
                m->refill_insn_count++;
            }
        } else if (((uint32_t)address & 0xFFFFFF00u) == 0x80000100u) {
            if (m->general_insn_count < 1024) {
                fprintf(stderr, "[VEC_PROBE_PER_INSN] general PC=0x%08" PRIX64 "\n", address);
                m->general_insn_count++;
            }
        }
    }

    /* Flush Unicorn's softmmu TLB after the previous TLBWI/TLBWR executed.
     * prid_hook fires BEFORE each instruction, so we arm the flag when the
     * TLBWI instruction is seen, then flush at the very next instruction
     * (after TLBWI has actually run and updated the hardware TLB).
     * This evicts any stale read-only (D=0) softmmu entry so the retry store
     * at the faulting PC can fill fresh from the updated hardware TLB. */
    /* Restore GPR after PFN-corrected MTC0 executed. */
    if (m->pending_gpr_restore) {
        uc_reg_write(uc, m->pending_gpr_reg, &m->pending_gpr_val);
        m->pending_gpr_restore = false;
    }

    if (m->pending_tlb_flush) {
        uc_ctl_flush_tlb(uc);
        m->pending_tlb_flush = false;
        static uint32_t tlb_flush_log = 0;
        if (tlb_flush_log++ < 64)
            fprintf(stderr, "[TLB_FLUSH] softmmu flushed at PC=0x%08" PRIX64 "\n",
                    (uint64_t)(uint32_t)address);
    }

    /* Restore a previous one-instruction tlbwi->tlbwr runtime patch. */
    if (m->tlbwi_patch_pending && address != m->tlbwi_patch_addr) {
        write_mem_best_effort(uc, m->tlbwi_patch_addr, &m->tlbwi_patch_orig, 4);
        m->tlbwi_patch_pending = false;
    }

    /* Restore a previous ERET NOP-patch (ERET was overwritten with NOP to
     * prevent the native ERET from reading stale CP0_EPC). */
    if (m->eret_nop_patch_pending && address != m->eret_nop_patch_addr) {
        write_mem_best_effort(uc, m->eret_nop_patch_addr, &m->eret_nop_patch_orig, 4);
        m->eret_nop_patch_pending = false;
    }

    /* Clear EXL that was set by the KERNEL_RI_SPURIOUS handler to suppress
     * the deferred RI exception from a native ERET.  Now that we're executing
     * at the correct address, restore EXL=0 so the kernel continues normally. */
    if (m->eret_ri_exl_fixup) {
        uint64_t fixup_status = 0;
        uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &fixup_status);
        fixup_status &= ~(uint64_t)0x2u;  /* clear EXL */
        uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &fixup_status);
        m->eret_ri_exl_fixup = false;
    }

    /*
     * MFC0 readback helper:
     * UC_HOOK_CODE fires before instruction execution. For native MFC0 reads
     * that we don't intercept, capture the read value from $rt on the next
     * instruction.
     */
    if (m->cp0_readback_pending) {
        if ((uint32_t)address == (uint32_t)m->cp0_readback_next_pc) {
            uint64_t val = 0;
            uc_reg_read(uc, UC_MIPS_REG_0 + (int)m->cp0_readback_rt, &val);
            cp0_shadow_write(m, m->cp0_readback_rd, m->cp0_readback_sel, val);
            if (m->cp0_readback_rd == 10u && m->cp0_readback_sel == 0u) {
                m->shadow_cp0_entryhi_live = val;
                m->shadow_cp0_entryhi_live_valid = true;
            }
            const char *name = cp0_reg_name(m->cp0_readback_rd, m->cp0_readback_sel);
            uint32_t mfc0_pc = (uint32_t)(m->cp0_readback_next_pc - 4u);
            bool wince_ctx_row_readback =
                is_wince_boot_machine(m) &&
                mfc0_pc >= 0x80079814u && mfc0_pc <= 0x80079820u;
            if (name != NULL &&
                cp0_is_tlb_diag_reg(m->cp0_readback_rd, m->cp0_readback_sel) &&
                (tlb_trace_window_active(m) || wince_ctx_row_readback) &&
                mfc0_cp0_readback_log < 512) {
                fprintf(stderr,
                        "[MFC0_CP0] rd=%s rt=$%u val=0x%08" PRIX64
                        " mfc0_pc=0x%08" PRIX64 " next_pc=0x%08" PRIX64
                        " shadow=[idx=0x%08X lo0=0x%08X lo1=0x%08X"
                        " hi=0x%08X mask=0x%08X ctx=0x%08X]\n",
                        name, (unsigned)m->cp0_readback_rt,
                        (uint64_t)(uint32_t)val,
                        (uint64_t)mfc0_pc,
                        (uint64_t)(uint32_t)address,
                        (uint32_t)m->shadow_cp0_index,
                        (uint32_t)m->shadow_cp0_entrylo0,
                        (uint32_t)m->shadow_cp0_entrylo1,
                        (uint32_t)m->shadow_cp0_entryhi,
                        (uint32_t)m->shadow_cp0_pagemask,
                        (uint32_t)m->shadow_cp0_context);
                mfc0_cp0_readback_log++;
            }
        }
        m->cp0_readback_pending = false;
    }

    uint32_t insn = 0;
    if (!read_insn_best_effort(uc, address, &insn))
        return;

    /* Deferred TLB refill handler patch: check on first entry to refill
     * vector (0x80000000).  This fires AFTER trap_init has installed the
     * handler, unlike the kernel_vm_ready check which may fire too early. */
    if ((uint32_t)address == 0x80000000u && !m->tlb_refill_patched &&
        !is_wince_boot_machine(m)) {
        m->tlb_refill_patched = true;
        uint32_t srl_insn = 0;
        uc_mem_read(uc, 0x80000020u, &srl_insn, 4);
        if (srl_insn == 0x001AD0C2u) {  /* srl k0, k0, 3 (R4000 8-byte PTE) */
            uint32_t patched = 0x001AD042u;  /* srl k0, k0, 1 (VR41xx 4-byte PTE) */
            uc_mem_write(uc, 0x80000020u, &patched, 4);
            uc_ctl_flush_tb(uc);
            fprintf(stderr,
                    "[TLB_REFILL_PATCH] patched srl shift at 0x80000020:"
                    " 0x%08X -> 0x%08X (4-byte PTE fix)\n",
                    srl_insn, patched);
        } else {
            fprintf(stderr,
                    "[TLB_REFILL_CHECK] insn@0x80000020=0x%08X (no patch needed)\n",
                    srl_insn);
        }
    }

    if (is_wince_boot_machine(m))
        m->wince_code_hook_seq++;

    if (is_wince_boot_machine(m)) {
        uint64_t reg_sp = 0, reg_ra = 0;
        uint32_t sp32 = 0, ra32 = 0;
        uc_reg_read(uc, UC_MIPS_REG_SP, &reg_sp);
        uc_reg_read(uc, UC_MIPS_REG_RA, &reg_ra);
        sp32 = (uint32_t)reg_sp;
        ra32 = (uint32_t)reg_ra;

        if (m->cfg.wince_collapse_double_helper_entry &&
            (uint32_t)address == UINT32_C(0x80096790) &&
            insn == UINT32_C(0x27BDFFD8) &&
            m->wince_prev_hook_valid &&
            m->wince_prev_hook_pc == UINT32_C(0x80096790) &&
            m->wince_prev_hook_ra == ra32 &&
            m->wince_prev_hook_seq + 1u == m->wince_code_hook_seq &&
            m->wince_prev_hook_sp == sp32 + UINT32_C(0x28)) {
            static uint32_t wince_helper_collapse_logs = 0;
            uint64_t a3 = 0;
            uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
            if (m->cfg.log_wince_stall && wince_helper_collapse_logs < 64u) {
                fprintf(stderr,
                        "[WINCE_HELPER_COLLAPSE] hook_seq=%llu prev_hook_seq=%llu"
                        " pc=0x%08X prev_sp=0x%08X sp=0x%08X ra=0x%08X"
                        " a3=0x%08X action=%s\n",
                        (unsigned long long)m->wince_code_hook_seq,
                        (unsigned long long)m->wince_prev_hook_seq,
                        (uint32_t)address,
                        m->wince_prev_hook_sp, sp32, ra32,
                        (uint32_t)a3,
                        ((uint32_t)a3 == 0u) ? "early_return" : "resume_96794");
                wince_helper_collapse_logs++;
            }
            m->wince_prev_hook_seq = m->wince_code_hook_seq;
            m->wince_prev_hook_pc = (uint32_t)address;
            m->wince_prev_hook_sp = sp32;
            m->wince_prev_hook_ra = ra32;
            m->wince_prev_hook_valid = true;
            if ((uint32_t)a3 == 0u && ra32 >= UINT32_C(0x80000000)) {
                uint64_t restore_sp = mips_sext(sp32 + UINT32_C(0x28));
                uint64_t next_pc = mips_sext(ra32);
                uc_reg_write(uc, UC_MIPS_REG_SP, &restore_sp);
                uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
            } else {
                uint64_t next_pc = UINT64_C(0x80096794);
                uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
            }
            return;
        }

        m->wince_prev_hook_seq = m->wince_code_hook_seq;
        m->wince_prev_hook_pc = (uint32_t)address;
        m->wince_prev_hook_sp = sp32;
        m->wince_prev_hook_ra = ra32;
        m->wince_prev_hook_valid = true;
    }

    if (maybe_skip_wince_bootmode_delay_call(m, uc, (uint32_t)address, insn))
        return;
    if (maybe_emulate_wince_objptr_init_call(m, uc, (uint32_t)address, insn))
        return;
    if (is_wince_boot_machine(m) && !m->wince_nk_epoch_reset_done) {
        uint32_t pc32 = (uint32_t)address;
        if (pc32 >= WINCE_NK_TRACE_BASE && pc32 < WINCE_NK_TRACE_END) {
            m->wince_nk_epoch_reset_done = true;
            seed_wince_probe_deferred(m, pc32);
            uint32_t old_step = m->rtc.etime_read_step;
            m->rtc.etime_read_step = 0;
            if (m->cfg.log_wince_stall) {
                fprintf(stderr,
                        "[WINCE_RTC_MODE] nk_entry_disable_read_assist pc=0x%08X"
                        " etime=0x%012" PRIX64 " step:%u->%u\n",
                        pc32,
                        (uint64_t)(m->rtc.etime & UINT64_C(0xFFFFFFFFFFFF)),
                        old_step, m->rtc.etime_read_step);
            }
        }
    }

    uint32_t op  = (insn >> 26) & 0x3Fu;
    uint32_t rs  = (insn >> 21) & 0x1Fu;
    uint32_t rt  = (insn >> 16) & 0x1Fu;
    uint32_t rd  = (insn >> 11) & 0x1Fu;
    uint32_t sel =  insn        & 0x07u;

    wince_div_call_trace_step(m, uc, (uint32_t)address, insn);
    wince_div_stack_watch_step(m, uc, (uint32_t)address, insn);
    wince_div_hist_record(m, uc, (uint32_t)address, insn);
    maybe_record_wince_ctrl_event(m, uc, (uint32_t)address,
                                  insn, op, rs, rt, rd, sel);
    maybe_probe_wince_ctx_path(m, uc, (uint32_t)address,
                               insn, op, rs, rt, rd, sel);
    maybe_probe_wince_bootctx_gate(m, uc, (uint32_t)address);
    maybe_probe_wince_live_call_targets(m, uc, (uint32_t)address);
    maybe_probe_wince_gate_path_hits(m, uc, (uint32_t)address);
    maybe_probe_wince_bootmode(m, uc, (uint32_t)address, insn);
    maybe_probe_wince_obj_dispatch(m, uc, (uint32_t)address, insn);
    maybe_probe_wince_bootmode_sp_flow(m, uc, (uint32_t)address, insn);
    maybe_probe_wince_spl_memcpy(m, uc, (uint32_t)address, insn);
    maybe_probe_wince_producer_pcs(m, uc, (uint32_t)address, insn);
    maybe_probe_wince_77FE4_internals(m, uc, (uint32_t)address, insn);
    maybe_probe_wince_delay_call_frame(m, uc, (uint32_t)address, insn);
    maybe_probe_wince_dispatcher_corridor(m, uc, (uint32_t)address, insn);
    maybe_probe_wince_ctx_launch(m, uc, (uint32_t)address, insn);
    maybe_probe_wince_ctx_save_writer(m, uc, (uint32_t)address, insn);
    maybe_probe_wince_obj_late_writer(m, uc, (uint32_t)address, insn);

    if (is_wince_boot_machine(m) &&
        m->cfg.wince_vr4131_tlbr_roundtrip &&
        (uint32_t)address == 0x80079824u) {
        uint64_t a0 = 0, t0 = 0, t1 = 0, t3 = 0;
        uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
        uc_reg_read(uc, UC_MIPS_REG_T0, &t0);
        uc_reg_read(uc, UC_MIPS_REG_T1, &t1);
        uc_reg_read(uc, UC_MIPS_REG_T3, &t3);

        uint32_t row[4] = {0, 0, 0, 0};
        bool row_ok[4] = {false, false, false, false};
        uint32_t row_base = (uint32_t)a0;
        if ((row_base & 0xFFFFF000u) == 0xA0002000u) {
            row_ok[0] = read_guest_u32(uc, row_base + 0u, &row[0]);
            row_ok[1] = read_guest_u32(uc, row_base + 4u, &row[1]);
            row_ok[2] = read_guest_u32(uc, row_base + 8u, &row[2]);
            row_ok[3] = read_guest_u32(uc, row_base + 12u, &row[3]);
        }

        uint32_t old_t0 = (uint32_t)t0;
        uint32_t old_t1 = (uint32_t)t1;
        uint32_t old_t3 = (uint32_t)t3;
        uint32_t new_t0 = old_t0;
        uint32_t new_t1 = old_t1;
        uint32_t new_t3 = old_t3;

        if (row_ok[0]) {
            uint32_t raw_t0 = wince_reverse_entrylo_pfn_fixup(old_t0);
            if (raw_t0 == row[0] && row[0] != old_t0)
                new_t0 = row[0];
        }
        if (row_ok[1]) {
            uint32_t raw_t1 = wince_reverse_entrylo_pfn_fixup(old_t1);
            if (raw_t1 == row[1] && row[1] != old_t1)
                new_t1 = row[1];
        }
        if (row_ok[3] && old_t3 == 0u && row[3] != 0u)
            new_t3 = row[3];

        if (new_t0 != old_t0)
            uc_reg_write(uc, UC_MIPS_REG_T0, &new_t0);
        if (new_t1 != old_t1)
            uc_reg_write(uc, UC_MIPS_REG_T1, &new_t1);
        if (new_t3 != old_t3)
            uc_reg_write(uc, UC_MIPS_REG_T3, &new_t3);

        if ((new_t0 != old_t0 || new_t1 != old_t1 || new_t3 != old_t3) &&
            m->cfg.log_wince_stall) {
            static uint32_t roundtrip_fix_logs = 0;
            if (roundtrip_fix_logs < 128u) {
                uint32_t row_idx = (row_base >= 0xA0002000u) ?
                    ((row_base - 0xA0002000u) >> 4) : 0u;
                fprintf(stderr,
                        "[WINCE_TLBR_ROUNDTRIP_FIX] pc=0x%08X row=%u base=0x%08X"
                        " t0:0x%08X->0x%08X t1:0x%08X->0x%08X"
                        " t3:0x%08X->0x%08X row=[%s0x%08X %s0x%08X %s0x%08X %s0x%08X]\n",
                        (uint32_t)address, row_idx, row_base,
                        old_t0, new_t0, old_t1, new_t1, old_t3, new_t3,
                        row_ok[0] ? "" : "ERR:", row[0],
                        row_ok[1] ? "" : "ERR:", row[1],
                        row_ok[2] ? "" : "ERR:", row[2],
                        row_ok[3] ? "" : "ERR:", row[3]);
                roundtrip_fix_logs++;
            }
        }
    }

    /* WinCE NK last-PC ring: keep last 256 PCs for postmortem.
     * Only active when --log-wince-stall is set and PC is in NK range. */
    if (is_wince_boot_machine(m) && m->cfg.log_wince_stall) {
        static uint32_t nk_pc_ring[256];
        static uint32_t nk_pc_ring_head = 0;
        static bool nk_pc_ring_active = false;
        uint32_t pc32 = (uint32_t)address;

        if (pc32 >= 0x80060000u && pc32 < 0x80F00000u) {
            nk_pc_ring_active = true;
            nk_pc_ring[nk_pc_ring_head & 0xFF] = pc32;
            nk_pc_ring_head++;
        }
        /* Provide a way for the postmortem to access the ring */
        (void)nk_pc_ring_active; /* suppress unused warning */
    }

    /*
     * WinCE SPL uses COP0 WAIT (0x42000023) in early boot.
     * Unicorn's MIPS core traps this path and lands at PC=0.
     * Explicit ISA-gap shim: treat WAIT as a low-power hint (NOP) so
     * execution advances.
     */
    if (is_wince_boot_machine(m) && insn == 0x42000023u) {
        uint64_t next_pc = address + 4u;
        static uint32_t wince_wait_skip_log = 0;
        if (wince_wait_skip_log < 32u) {
            fprintf(stderr,
                    "[WINCE_ISA_GAP_WAIT] PC=0x%08" PRIX64 " -> 0x%08" PRIX64 "\n",
                    (uint64_t)(uint32_t)address, (uint64_t)(uint32_t)next_pc);
            wince_wait_skip_log++;
        }
        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
        return;
    }

    if (m->execve_user_handoff_active &&
        (m->execve_user_handoff_state == EXECVE_HANDOFF_STATE_ARMED ||
         m->execve_user_handoff_state == EXECVE_HANDOFF_STATE_USER_FETCH_SEEN) &&
        (uint32_t)address < 0x80000000u) {
        bool first_fetch = (m->execve_user_handoff_state == EXECVE_HANDOFF_STATE_ARMED);
        const char *next_state =
            first_fetch ? "USER_FETCH_SEEN" :
            ((insn != 0u) ? "DONE" : "USER_FETCH_SEEN");
        if (m->cfg.trace_user_handoff) {
            fprintf(stderr,
                    "[EXECVE_HANDOFF_FETCH] pc=0x%08" PRIX64
                    " insn=0x%08X%s -> %s\n",
                    (uint64_t)(uint32_t)address, insn,
                    (insn == 0u) ? " (zero)" : "",
                    next_state);
        }
        if (insn != 0u) {
            /* User fetch observed: stale exception redelivery from the completed
             * syscall must no longer be visible in the handoff fast-path. */
            m->pending_epc = 0;
            m->pending_excode = 0;
            m->pending_cause = 0;
            m->epc_was_written = false;
            m->pending_cause_served = false;
            m->pending_epc_served = false;
            m->has_saved_exception = false;
            m->execve_user_handoff_pc = (uint32_t)address;
            (void)trace_user_handoff_entry_probe(m, "RETIRE",
                                                 (uint32_t)address,
                                                 (uint32_t)address,
                                                 m->shadow_cp0_badvaddr, false);
            if (first_fetch) {
                m->execve_user_handoff_state = EXECVE_HANDOFF_STATE_USER_FETCH_SEEN;
            } else {
                m->execve_user_handoff_state = EXECVE_HANDOFF_STATE_DONE;
                m->execve_user_handoff_active = false;
                m->execve_user_handoff_done_keep_count = 0;
                /* Disable quarantine — user execution is established */
                m->execve_user_handoff_pc = 0;
            }
        }
    }
    if (!m->execve_user_handoff_active &&
        m->execve_user_handoff_state == EXECVE_HANDOFF_STATE_DONE &&
        m->execve_user_handoff_pc != 0u &&
        (uint32_t)address < 0x80000000u &&
        insn != 0u) {
        (void)trace_user_handoff_entry_probe(m, "RETIRE",
                                             (uint32_t)address,
                                             (uint32_t)address,
                                             m->shadow_cp0_badvaddr, false);
        /* Track forward progress while DONE quarantine is active. */
        if ((uint32_t)m->execve_user_handoff_pc != (uint32_t)address) {
            m->execve_user_handoff_pc = (uint32_t)address;
        }
        /* Keep DONE quarantine anchor until a subsequent syscall path reset.
         * Disarming here allows stale intno=12/26 callbacks at 0x80001850 to
         * re-surface immediately after first user progress. */
        if (!in_user_handoff_entry_window((uint32_t)address) &&
            m->cfg.trace_user_handoff) {
            static uint32_t handoff_done_retain_log = 0;
            if (handoff_done_retain_log < 64) {
                fprintf(stderr,
                        "[EXECVE_USER_HANDOFF_DONE_RETAIN] pc=0x%08" PRIX64
                        " anchor=0x%08" PRIX64 " done_keep=%u\n",
                        (uint64_t)(uint32_t)address,
                        (uint64_t)(uint32_t)m->execve_user_handoff_pc,
                        m->execve_user_handoff_done_keep_count);
                handoff_done_retain_log++;
            }
        }
    }

    /*
     * sys_execve path probes (2.4 kernel):
     *   0x800073b4: jal getname (a0 already loaded from stack arg area)
     *   0x800073bc: getname return in v0 (before move s0,v0)
     *   0x800073cc: sltiu classify getname result (ERR_PTR check)
     *   0x800073d4: beqz branch to do_execve/error-return
     *   0x800073dc: error fast-return path (getname failed)
     *   0x800073f4: call do_execve path (getname succeeded)
     *   0x80007400: do_execve return consumed into kmem_cache_free
     */
    if ((uint32_t)address == 0x800073B4u || (uint32_t)address == 0x800073BCu) {
        static uint32_t sys_execve_getname_log = 0;
        if (sys_execve_getname_log < 128) {
            uint64_t sp = 0, a0 = 0, v0 = 0;
            uint32_t arg72 = 0, arg76 = 0, arg80 = 0;
            uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
            uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
            uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
            (void)read_guest_u32(uc, (uint32_t)sp + 72u, &arg72);
            (void)read_guest_u32(uc, (uint32_t)sp + 76u, &arg76);
            (void)read_guest_u32(uc, (uint32_t)sp + 80u, &arg80);
            if ((uint32_t)address == 0x800073B4u) {
                char a0s[128] = "<unreadable>";
                read_guest_string(uc, a0, a0s, sizeof(a0s));
                fprintf(stderr,
                        "[SYS_EXECVE_GETNAME_IN] PC=0x%08" PRIX64
                        " sp=0x%08" PRIX64 " a0=0x%08" PRIX64 " \"%s\""
                        " stk72=0x%08X stk76=0x%08X stk80=0x%08X\n",
                        (uint64_t)(uint32_t)address, (uint64_t)(uint32_t)sp,
                        (uint64_t)(uint32_t)a0, a0s, arg72, arg76, arg80);
            } else {
                fprintf(stderr,
                        "[SYS_EXECVE_GETNAME_RET] PC=0x%08" PRIX64
                        " sp=0x%08" PRIX64 " v0=0x%08" PRIX64
                        " stk72=0x%08X stk76=0x%08X stk80=0x%08X\n",
                        (uint64_t)(uint32_t)address, (uint64_t)(uint32_t)sp,
                        (uint64_t)(uint32_t)v0, arg72, arg76, arg80);
            }
            sys_execve_getname_log++;
        }
    }
    if ((uint32_t)address == 0x800073CCu || (uint32_t)address == 0x800073D4u) {
        static uint32_t sys_execve_gate_log = 0;
        if (sys_execve_gate_log < 128) {
            uint64_t v0 = 0, s0 = 0, s1 = 0, a0 = 0;
            uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
            uc_reg_read(uc, UC_MIPS_REG_S0, &s0);
            uc_reg_read(uc, UC_MIPS_REG_S1, &s1);
            uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
            if ((uint32_t)address == 0x800073CCu) {
                fprintf(stderr,
                        "[SYS_EXECVE_GATE] pre_sltiu PC=0x%08" PRIX64
                        " s0=0x%08" PRIX64 " v0=0x%08" PRIX64
                        " a0=0x%08" PRIX64 "\n",
                        (uint64_t)(uint32_t)address,
                        (uint64_t)(uint32_t)s0,
                        (uint64_t)(uint32_t)v0,
                        (uint64_t)(uint32_t)a0);
            } else {
                fprintf(stderr,
                        "[SYS_EXECVE_GATE] pre_beqz PC=0x%08" PRIX64
                        " v0=0x%08" PRIX64 " s0=0x%08" PRIX64
                        " s1=0x%08" PRIX64 " a0=0x%08" PRIX64 "\n",
                        (uint64_t)(uint32_t)address,
                        (uint64_t)(uint32_t)v0,
                        (uint64_t)(uint32_t)s0,
                        (uint64_t)(uint32_t)s1,
                        (uint64_t)(uint32_t)a0);
            }
            sys_execve_gate_log++;
        }
    }
    if ((uint32_t)address == 0x800073DCu ||
        (uint32_t)address == 0x800073F4u ||
        (uint32_t)address == 0x80007400u) {
        static uint32_t sys_execve_path_log = 0;
        if (sys_execve_path_log < 128) {
            const char *tag = ((uint32_t)address == 0x800073DCu) ? "err_return" :
                              ((uint32_t)address == 0x800073F4u) ? "call_do_execve" :
                                                                    "post_do_execve";
            uint64_t v0 = 0, a0 = 0, a1 = 0, a2 = 0, a3 = 0, s0 = 0, s1 = 0, ra = 0;
            uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
            uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
            uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
            uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
            uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
            uc_reg_read(uc, UC_MIPS_REG_S0, &s0);
            uc_reg_read(uc, UC_MIPS_REG_S1, &s1);
            uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
            fprintf(stderr,
                    "[SYS_EXECVE_PATH] %s PC=0x%08" PRIX64
                    " v0=0x%08" PRIX64 " s0=0x%08" PRIX64 " s1=0x%08" PRIX64
                    " a0=0x%08" PRIX64 " a1=0x%08" PRIX64
                    " a2=0x%08" PRIX64 " a3=0x%08" PRIX64
                    " ra=0x%08" PRIX64 "\n",
                    tag, (uint64_t)(uint32_t)address,
                    (uint64_t)(uint32_t)v0,
                    (uint64_t)(uint32_t)s0,
                    (uint64_t)(uint32_t)s1,
                    (uint64_t)(uint32_t)a0,
                    (uint64_t)(uint32_t)a1,
                    (uint64_t)(uint32_t)a2,
                    (uint64_t)(uint32_t)a3,
                    (uint64_t)(uint32_t)ra);
            sys_execve_path_log++;
        }
    }
    /*
     * Capture do_execve entry for both known kernels:
     *   2.4: 0x8004a9d0
     *   2.6: 0x80080cb0
     */
    if ((uint32_t)address == 0x8004A9D0u || (uint32_t)address == 0x80080CB0u) {
        uint64_t ra = 0, a0 = 0, a1 = 0, a2 = 0, sp = 0;
        char path[128] = "<unreadable>";
        static uint32_t syscall_handoff_clear_log = 0;
        uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
        uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
        uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
        uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
        uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
        read_guest_string(uc, a0, path, sizeof(path));

        /*
         * do_execve() entry marker.
         *
         * Keep synthetic syscall state intact here; it must live until the
         * syscall return path (MTC0 EPC / ERET / fallback retire) completes.
         * Clearing it at entry allows intno=26/27 at SYSCALL+4 to corrupt
         * init's execve return bookkeeping.
         */
        if (m->pending_excode == MIPS_EXCCODE_SYS &&
            m->pending_syscall_nr == 4011u) {
            if (syscall_handoff_clear_log < 64) {
                fprintf(stderr,
                        "[SYSCALL_HANDOFF_SEEN] pc=0x%08" PRIX64
                        " syscall_epc=0x%08" PRIX64
                        " pending_epc=0x%08" PRIX64
                        " path=\"%s\"\n",
                        (uint64_t)(uint32_t)address,
                        (uint64_t)(uint32_t)m->pending_syscall_epc,
                        (uint64_t)(uint32_t)m->pending_epc,
                        path);
                syscall_handoff_clear_log++;
            }
        }

        if (m->execve_watch_active && do_execve_enter_log < 128) {
            fprintf(stderr,
                    "[DO_EXECVE_WATCH_OVERRUN] old_ret=0x%08" PRIX64
                    " old_path=\"%s\" new_ret=0x%08" PRIX64 " new_path=\"%s\"\n",
                    (uint64_t)(uint32_t)m->execve_watch_ret_pc,
                    m->execve_watch_path,
                    (uint64_t)(uint32_t)ra,
                    path);
            do_execve_enter_log++;
        }

        m->execve_watch_active    = true;
        m->execve_watch_ret_pc    = ra;
        m->execve_entry_pc        = address;  /* saved for stale-TLB retry redirect */
        m->execve_watch_a0        = a0;
        m->execve_saved_a1        = a1;       /* argv — restored on stale-TLB retry */
        m->execve_saved_a2        = a2;       /* envp — restored on stale-TLB retry */
        m->execve_saved_sp        = sp;       /* $sp  — restored on stale-TLB retry (prevents frame drift) */
        m->execve_open_exec_ran   = false;    /* reset fd-leak flag for this invocation */
        snapshot_execve_gpr_context(m, uc, address);
        strncpy(m->execve_watch_path, path, sizeof(m->execve_watch_path) - 1);
        m->execve_watch_path[sizeof(m->execve_watch_path) - 1] = '\0';

        /* Unmap kuseg gap regions so kernel writes to user-space VAs during
         * do_execve trigger WRITE_UNMAPPED → TLBS injection, allowing the
         * kernel's page fault handler to allocate and populate user pages.
         * Without this, the pre-mapped flat kuseg region absorbs writes at
         * VA=PA (wrong), and user-space code/data is never loaded from the
         * ELF binary on the ramdisk. */
        if (m->kernel_vm_ready && !m->kuseg_gaps_unmapped) {
            m->kuseg_gaps_unmapped = true;
            static const struct { uint64_t base; uint64_t size; } kuseg_gaps[] = {
                { 0x01000000u, 0x09000000u },
                { 0x0B000000u, 0x04000000u },
                { 0x0F001000u, 0x0EFFF000u },
                { 0x20000000u, 0x60000000u },
            };
            for (int gi = 0; gi < 4; gi++) {
                uc_err uerr = uc_mem_unmap(m->uc,
                                           kuseg_gaps[gi].base,
                                           kuseg_gaps[gi].size);
                fprintf(stderr,
                        "[KUSEG_UNMAP_EXECVE] 0x%08" PRIX64
                        "-0x%08" PRIX64 ": %s\n",
                        kuseg_gaps[gi].base,
                        kuseg_gaps[gi].base + kuseg_gaps[gi].size - 1,
                        uc_strerror(uerr));
            }
            /* Flush Unicorn's JIT translation buffer and softmmu TLB
             * so stale cached translations for the old kuseg mappings
             * are invalidated.  Without this, the JIT still directs
             * kuseg accesses to the now-unmapped backing memory. */
            uc_ctl_flush_tb(m->uc);
            uc_ctl_flush_tlb(m->uc);
            fprintf(stderr, "[KUSEG_UNMAP_EXECVE] flushed TB+TLB\n");
        }

        if (do_execve_enter_log < 128) {
            fprintf(stderr,
                    "[DO_EXECVE_ENTER] pc=0x%08" PRIX64 " ra=0x%08" PRIX64
                    " path_ptr=0x%08" PRIX64 " \"%s\""
                    " a1=0x%08" PRIX64 " a2=0x%08" PRIX64 "\n",
                    (uint64_t)(uint32_t)address,
                    (uint64_t)(uint32_t)ra,
                    (uint64_t)(uint32_t)a0,
                    path,
                    (uint64_t)(uint32_t)a1,
                    (uint64_t)(uint32_t)a2);
            do_execve_enter_log++;
        }
    }

    /* Simulate CP0 Count register ($9).
     * Unicorn does not auto-increment CP0 Count, so MFC0 rt,$9 always
     * returns 0.  WinCE (and some Linux paths) use Count-based busy-wait
     * loops that spin forever without this.  We derive a monotonic count
     * from insn_count, scaling by 2 to approximate "one count tick per
     * two instructions" (roughly matching a single-issue pipeline). */
    if (op == 0x10u && rs == 0u && rd == 9u && sel == 0u) {
        uint64_t count_val = (uint64_t)machine_cp0_count32(m);
        uc_reg_write(uc, UC_MIPS_REG_0 + (int)rt, &count_val);
        uint64_t next_pc = address + 4u;
        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
        return;
    }

    /* Always capture MFC0 BadVAddr (rd=8) via deferred readback so that
     * shadow_cp0_badvaddr is current when TLBWI fires.  This is needed for
     * the tlb_map_kuseg_page call even before tlb_trace_window is active. */
    if (op == 0x10u && rs == 0u && rd == 8u && sel == 0u) {
        m->cp0_readback_pending = true;
        m->cp0_readback_rt  = (uint8_t)rt;
        m->cp0_readback_rd  = 8u;
        m->cp0_readback_sel = 0u;
        m->cp0_readback_next_pc = address + 4u;
    }

    /* Always capture MFC0 EntryHi to keep a current ASID hint. */
    if (op == 0x10u && rs == 0u && rd == 10u && sel == 0u) {
        m->cp0_readback_pending = true;
        m->cp0_readback_rt  = (uint8_t)rt;
        m->cp0_readback_rd  = 10u;
        m->cp0_readback_sel = 0u;
        m->cp0_readback_next_pc = address + 4u;
    }

    /* Queue readback for other TLB diagnostic registers when the debug window
     * is active (avoid log saturation from early-boot MFC0 reads). */
    if (tlb_trace_window_active(m) && op == 0x10u && rs == 0u) {
        const char *name = cp0_reg_name(rd, sel);
        if (name != NULL && cp0_is_tlb_diag_reg(rd, sel) &&
            !(rd == 8u && sel == 0u)) {  /* BadVAddr already queued above */
            m->cp0_readback_pending = true;
            m->cp0_readback_rt = (uint8_t)rt;
            m->cp0_readback_rd = (uint8_t)rd;
            m->cp0_readback_sel = (uint8_t)sel;
            m->cp0_readback_next_pc = address + 4u;
        }
    }

    /*
     * Workaround: when writing CP0 Index (rd=0), clear bit31 (P/probe-fail)
     * in the source GPR so tlbwi uses a concrete index. Linux often carries
     * Index=0x8000001F through tlbp-miss paths; hardware ignores P for tlbwi.
     */
    if (op == 0x10u && rs == 4u && rd == 0u && sel == 0u) {
        uint64_t idx_val = 0;
        uc_reg_read(uc, UC_MIPS_REG_0 + (int)rt, &idx_val);
        if (idx_val & 0x80000000u) {
            uint64_t fixed = idx_val & 0x7FFFFFFFu;
            uc_reg_write(uc, UC_MIPS_REG_0 + (int)rt, &fixed);
            static uint32_t idx_fix_log = 0;
            if (tlb_trace_window_active(m) && idx_fix_log < 128) {
                fprintf(stderr,
                        "[INDEX_FIX] rt=$%u old=0x%08" PRIX64 " new=0x%08" PRIX64
                        " PC=0x%08" PRIX64 "\n",
                        rt, (uint64_t)(uint32_t)idx_val,
                        (uint64_t)(uint32_t)fixed,
                        (uint64_t)(uint32_t)address);
                idx_fix_log++;
            }
        }
    }

    /* Track guest CP0 writes and keep a shadow copy for TLB diagnostics. */
    if (op == 0x10u && rs == 4u) {
        uint64_t val = 0;
        uc_reg_read(uc, UC_MIPS_REG_0 + (int)rt, &val);
        if (sel == 0u && rd == 11u) {
            m->cp0_compare_shadow = (uint32_t)val;
            m->cp0_compare_shadow_valid = true;
        }
        /*
         * Execve scratch TLB rescue:
         * refill handlers can occasionally compute EntryLo0/1 == 0 for the
         * synthetic argv/envp scratch page (0x01020000), which traps us in a
         * refill loop and never lets sys_execve retire.  When that happens
         * during an in-flight execve syscall, synthesize a concrete RWV+G PTE
         * for the scratch page pair.
         */
        if (sel == 0u &&
            (rd == 2u || rd == 3u) &&
            (uint32_t)val == 0u &&
            m->pending_excode == MIPS_EXCCODE_SYS &&
            m->pending_syscall_nr == 4011u) {
            uint32_t badv = (uint32_t)m->shadow_cp0_badvaddr;
            if (badv >= EXECVE_SCRATCH_BASE && badv < EXECVE_SCRATCH_END) {
                uint32_t even_va = badv & ~0x1FFFu;
                uint32_t pa = even_va + ((rd == 3u) ? 0x1000u : 0u);
                uint64_t synth_lo = (((uint64_t)(pa >> 10) & 0xFFFFFu) << 6) | 0x1Fu;
                uc_reg_write(uc, UC_MIPS_REG_0 + (int)rt, &synth_lo);
                val = synth_lo;
                static uint32_t execve_tlb_synth_log = 0;
                if (execve_tlb_synth_log < 64) {
                    fprintf(stderr,
                            "[EXECVE_TLB_SYNTH] rd=%s rt=$%u badv=0x%08X"
                            " lo=0x%08" PRIX64 " pa=0x%08X\n",
                            cp0_reg_name(rd, sel), rt, badv,
                            (uint64_t)(uint32_t)val, pa);
                    execve_tlb_synth_log++;
                }
            }
        }
        cp0_shadow_write(m, rd, sel, val);

        /* VR41xx → MIPS32 PFN correction for EntryLo0/EntryLo1.
         * The VR41xx kernel stores PFN so that PA = PFN << 10, but
         * Unicorn's MIPS32 hardware TLB computes PA = PFN << 12.
         * Shift PFN right by 2 in the GPR before the native MTC0
         * executes, so the hardware TLB gets the correct PA.
         * The shadow copy retains the original VR41xx value. */
        if (sel == 0u && (rd == 2u || rd == 3u) && rt != 0u) {
            bool skip_fixup = false;
            uint32_t ctx_row_word = 0;
            bool ctx_row_ok = false;
            if (is_wince_boot_machine(m) &&
                ((uint32_t)address == 0x80079684u || (uint32_t)address == 0x8007968Cu)) {
                uint32_t idx = (uint32_t)m->shadow_cp0_index & 0x3Fu;
                uint32_t row_va = 0xA0002000u + (idx * 16u) + ((rd == 3u) ? 4u : 0u);
                ctx_row_ok = (uc_mem_read(uc, row_va, &ctx_row_word, sizeof(ctx_row_word)) == UC_ERR_OK);
                if (rd == 2u &&
                    m->wince_ctx_entrylo0_last_valid &&
                    m->wince_ctx_entrylo0_last_pc == 0x80079368u &&
                    m->wince_ctx_entrylo0_last_val == (uint32_t)val)
                    skip_fixup = true;
                if (rd == 3u &&
                    m->wince_ctx_entrylo1_last_valid &&
                    m->wince_ctx_entrylo1_last_pc == 0x80079370u &&
                    m->wince_ctx_entrylo1_last_val == (uint32_t)val)
                    skip_fixup = true;
            }
            uint32_t orig_pfn = ((uint32_t)val >> 6) & 0xFFFFFu;
            uint32_t flags = (uint32_t)val & 0x3Fu;
            uint64_t corrected = (uint64_t)(((orig_pfn >> 2) << 6) | flags);
            static uint32_t mtc0_pfn_fixup_log = 0;
            bool wince_ctx_restore_fixup =
                is_wince_boot_machine(m) &&
                (uint32_t)address >= 0x80079684u &&
                (uint32_t)address <= 0x8007968Cu;
            if ((wince_ctx_restore_fixup || tlb_trace_window_active(m)) &&
                mtc0_pfn_fixup_log < 128) {
                fprintf(stderr,
                        "[MTC0_PFN_FIXUP] rd=%s rt=$%u pc=0x%08" PRIX64
                        " orig=0x%08" PRIX64 " corrected=0x%08" PRIX64
                        " orig_pfn=0x%05X corrected_pfn=0x%05X flags=0x%02X"
                        " skip=%u ctx_row=%s0x%08X"
                        " last_ctx=[lo0:%u@0x%08X=0x%08X lo1:%u@0x%08X=0x%08X]\n",
                        cp0_reg_name(rd, sel), rt,
                        (uint64_t)(uint32_t)address,
                        (uint64_t)(uint32_t)val,
                        (uint64_t)(uint32_t)corrected,
                        orig_pfn, (orig_pfn >> 2), flags,
                        skip_fixup ? 1u : 0u,
                        ctx_row_ok ? "" : "ERR:", ctx_row_word,
                        m->wince_ctx_entrylo0_last_valid ? 1u : 0u,
                        m->wince_ctx_entrylo0_last_pc,
                        m->wince_ctx_entrylo0_last_val,
                        m->wince_ctx_entrylo1_last_valid ? 1u : 0u,
                        m->wince_ctx_entrylo1_last_pc,
                        m->wince_ctx_entrylo1_last_val);
                mtc0_pfn_fixup_log++;
            }
            if (skip_fixup)
                goto skip_entrylo_fixup;
            /* Save original GPR value, write corrected, restore after MTC0 */
            m->pending_gpr_restore = true;
            m->pending_gpr_reg = UC_MIPS_REG_0 + (int)rt;
            m->pending_gpr_val = val;
            uc_reg_write(uc, UC_MIPS_REG_0 + (int)rt, &corrected);
skip_entrylo_fixup:
            ;
        }

        const char *name = cp0_reg_name(rd, sel);
        if (name != NULL &&
            cp0_is_tlb_diag_reg(rd, sel) &&
            tlb_trace_window_active(m) &&
            mtc0_cp0_log < 512) {
            uint64_t status_now = 0;
            uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &status_now);
            fprintf(stderr,
                    "[MTC0_CP0] rd=%s rt=$%u val=0x%08" PRIX64
                    " PC=0x%08" PRIX64 " STATUS=0x%08" PRIX64
                    " pending_excode=%u\n",
                    name, rt, (uint64_t)(uint32_t)val,
                    (uint64_t)(uint32_t)address, status_now,
                    m->pending_excode);
            mtc0_cp0_log++;
        }
    }

    /* Observe TLB management instructions with current CP0 shadow state. */
    if (insn == 0x42000002u || insn == 0x42000006u ||
        insn == 0x42000008u || insn == 0x42000001u) {
        if (tlb_trace_window_active(m) && tlbop_log < 512) {
            const char *opname = (insn == 0x42000002u) ? "tlbwi" :
                                 (insn == 0x42000006u) ? "tlbwr" :
                                 (insn == 0x42000008u) ? "tlbp"  : "tlbr";
            uint64_t status_now = 0;
            uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &status_now);
            fprintf(stderr,
                    "[TLB_OP] %s PC=0x%08" PRIX64 " STATUS=0x%08" PRIX64
                    " idx=0x%08" PRIX64 " hi=0x%08" PRIX64
                    " lo0=0x%08" PRIX64 " lo1=0x%08" PRIX64
                    " mask=0x%08" PRIX64 " ctx=0x%08" PRIX64
                    " badv=0x%08" PRIX64 " pending_excode=%u\n",
                    opname, (uint64_t)(uint32_t)address, status_now,
                    (uint64_t)(uint32_t)m->shadow_cp0_index,
                    (uint64_t)(uint32_t)m->shadow_cp0_entryhi,
                    (uint64_t)(uint32_t)m->shadow_cp0_entrylo0,
                    (uint64_t)(uint32_t)m->shadow_cp0_entrylo1,
                    (uint64_t)(uint32_t)m->shadow_cp0_pagemask,
                    (uint64_t)(uint32_t)m->shadow_cp0_context,
                    (uint64_t)(uint32_t)m->shadow_cp0_badvaddr,
                    m->pending_excode);
            tlbop_log++;
        }

        /*
         * TLBWI/TLBWR dual fix for kuseg TLB storm at PC=0x800015B4.
         *
         * Root cause A — stale softmmu TLB: Unicorn's QEMU softmmu caches a
         * read-only (D=0) TLB entry for the kuseg VA.  After handle_tlbs sets
         * D=1 and runs TLBWI, the stale softmmu entry remains, so the retry
         * store still fails.  Fix: arm pending_tlb_flush so uc_ctl_flush_tlb()
         * is called at the next instruction (after TLBWI has actually executed
         * and updated the hardware TLB).  This forces QEMU to re-fill the
         * softmmu from the hardware TLB on the next access.
         *
         * Root cause B — Unicorn flat VA mapping: some Unicorn builds bypass
         * the MIPS hardware TLB for kuseg and look up the flat Unicorn region
         * table directly by VA.  Fix: directly map the faulting kuseg VA block
         * in Unicorn's region table using the PFN from EntryLo0/EntryLo1 and
         * populate it from the corresponding SDRAM PA block.
         *
         * Both fixes are applied together; each is harmless if the other is
         * the actual root cause.
         */
        if (insn == 0x42000002u || insn == 0x42000006u) {
            shadow_tlb_record_write(m, insn, (uint32_t)address);

            /* Once we see any TLB write from kernel code, the kernel's
             * exception vectors are installed and TLB refill is functional.
             * Enable kernel-mode TLBS/TLBL injection for kuseg accesses. */
            if (!m->kernel_vm_ready && !is_wince_boot_machine(m)) {
                m->kernel_vm_ready = true;
                fprintf(stderr,
                        "[KERNEL_VM_READY] armed at PC=0x%08X (first TLBW%c)\n",
                        (uint32_t)address,
                        (insn == 0x42000002u) ? 'I' : 'R');

                /*
                 * Patch the TLB refill handler at 0x80000000 if the kernel
                 * installed except_vec0_r4000 (8-byte PTE shift) instead of
                 * except_vec0_nevada (4-byte PTE shift).
                 *
                 * The kernel's traps.c installs except_vec0_nevada only for
                 * CPU_NEVADA, but the VR4131 is identified as CPU_VR41XX.
                 * The R4000 handler uses "srl k0, k0, 3" (0x001AD0C2) for
                 * the Context→PTE offset, which assumes 8-byte PTEs.  The
                 * VR41xx family uses 4-byte PTEs and needs "srl k0, k0, 1"
                 * (0x001AD042).  With the wrong shift, every TLB refill
                 * reads from offset+8 in the PTE table (wrong entry).
                 */
                uint32_t refill_srl = 0;
                uc_mem_read(uc, 0x80000020u, &refill_srl, 4);
                fprintf(stderr,
                        "[TLB_REFILL_CHECK] insn@0x80000020=0x%08X"
                        " (need 0x001AD0C2 for R4000 handler)\n",
                        refill_srl);
                if (refill_srl == 0x001AD0C2u) {
                    uint32_t patched = 0x001AD042u;  /* srl k0, k0, 1 */
                    uc_mem_write(uc, 0x80000020u, &patched, 4);
                    uc_ctl_flush_tb(uc);
                    fprintf(stderr,
                            "[TLB_REFILL_PATCH] patched srl shift at 0x80000020:"
                            " 0x%08X -> 0x%08X (4-byte PTE fix)\n",
                            refill_srl, patched);
                }
            }

            uint32_t badvaddr = (uint32_t)m->shadow_cp0_badvaddr;
            uint32_t entryhi = (uint32_t)m->shadow_cp0_entryhi;
            uint32_t entryhi_vpn2 = entryhi & 0xFFFFE000u;

            /* Arm the softmmu TLB flush so the hardware TLB entry (now
             * with corrected MIPS32 PFN from MTC0 intercept) takes effect. */
            m->pending_tlb_flush = true;
            uint32_t kuseg_va = (entryhi_vpn2 != 0u && entryhi_vpn2 < 0x80000000u)
                              ? entryhi_vpn2 : badvaddr;

            static uint32_t tlbwi_diag_log = 0;
            if (tlbwi_diag_log < 128) {
                fprintf(stderr,
                        "[TLBWI_DIAG] badvaddr=0x%08X lo0=0x%08" PRIX64
                        " lo1=0x%08" PRIX64 " entryhi=0x%08X"
                        " mask=0x%08" PRIX64 " chosen=0x%08X"
                        " PC=0x%08" PRIX64 "\n",
                        badvaddr,
                        (uint64_t)(uint32_t)m->shadow_cp0_entrylo0,
                        (uint64_t)(uint32_t)m->shadow_cp0_entrylo1,
                        entryhi,
                        (uint64_t)(uint32_t)m->shadow_cp0_pagemask,
                        kuseg_va,
                        (uint64_t)(uint32_t)address);
                tlbwi_diag_log++;
            }

            if (kuseg_va != 0u && kuseg_va < 0x80000000u) {
                shadow_tlb_populate(m, kuseg_va, true, "TLBWI_POPULATE",
                                    (uint32_t)address);
            }
        }
    }

    /* MFC0 $rt, $15, 0  — PRId (CP0 register 15, sel 0)
     * Encoding: 000100 00000 rt 01111 00000 000000
     *           op=0x10 rs=0  rt    rd=15  sel=0  */
    if ((insn & 0xFFE0FFFFu) == 0x40007800u) {
        uint32_t rt = (insn >> 16) & 0x1F;
        uint64_t prid = VR4131_PRID;
        uc_reg_write(uc, UC_MIPS_REG_0 + (int)rt, &prid);
        uint64_t next_pc = address + 4;
        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
        return;
    }

    if (is_wince_boot_machine(m) && (insn & 0xFFE0FFFFu) == 0x40008000u) {
        uint32_t rt = (insn >> 16) & 0x1F;
        uint64_t config = VR4131_CONFIG;
        uc_reg_write(uc, UC_MIPS_REG_0 + (int)rt, &config);
        uint64_t next_pc = address + 4;
        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
        return;
    }

    if (is_wince_boot_machine(m) && (insn & 0xFFE0FFFFu) == 0x40003000u) {
        uint32_t rt = (insn >> 16) & 0x1F;
        uint64_t wired = VR4131_WIRED;
        uc_reg_write(uc, UC_MIPS_REG_0 + (int)rt, &wired);
        uint64_t next_pc = address + 4;
        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
        return;
    }

    if (is_wince_boot_machine(m) && (insn & 0xFFE0FFFFu) == 0x40005800u) {
        uint32_t rt = (insn >> 16) & 0x1F;
        uint64_t compare = m->cp0_compare_shadow_valid
            ? (uint64_t)m->cp0_compare_shadow
            : (uint64_t)(machine_cp0_count32(m) + UINT32_C(0x4000));
        uc_reg_write(uc, UC_MIPS_REG_0 + (int)rt, &compare);
        uint64_t next_pc = address + 4;
        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
        return;
    }

    /*
     * MFC0 $rt, $13, 0  — Cause register (CP0 register 13, sel 0)
     * Encoding: 000100 00000 rt 01101 00000 000000
     *           op=0x10 rs=0  rt    rd=13  sel=0
     * Mask:     0xFFE0FFFF
     * Match:    0x40006800
     *
     * Unicorn 2.1.4 does not set CP0_Cause.ExcCode when routing a SYSCALL
     * exception.  The except_vec3_r4000 exception vector reads Cause to
     * dispatch to the right handler.  When a SYSCALL exception is pending
     * (m->pending_excode != 0), return a synthesised Cause value with the
     * correct ExcCode in bits[6:2].
     */
    if ((insn & 0xFFE0FFFFu) == 0x40006800u) {
        if (mfc0_cause_seen_log < 256 || in_init_execve_syscall_window(m)) {
            fprintf(stderr,
                    "[CAUSE_MFC0] seen PC=0x%08" PRIX64
                    " pending_excode=%u served=%u cause=0x%08X"
                    " epc_written=%u sys_nr=%u syscall_epc=0x%08" PRIX64 "\n",
                    (uint64_t)(uint32_t)address, m->pending_excode,
                    m->pending_cause_served ? 1u : 0u, m->pending_cause,
                    m->epc_was_written ? 1u : 0u, m->pending_syscall_nr,
                    (uint64_t)(uint32_t)m->pending_syscall_epc);
            if (mfc0_cause_seen_log < 256)
                mfc0_cause_seen_log++;
        }
    }
    bool should_inject_cause = false;
    if (m->pending_excode == 1u) {
        /*
         * External interrupt injection:
         * keep IP2 visible on all Cause reads while the synthetic IRQ is
         * in flight. This is less address-fragile across kernel versions
         * (2.4 vs 2.6 have different handler PCs).
         */
        should_inject_cause = true;
    } else if (m->pending_excode != 0) {
        /* SYSCALL and other synthetic exceptions: one-shot Cause inject. */
        should_inject_cause = !m->pending_cause_served;
    }
    if ((insn & 0xFFE0FFFFu) == 0x40006800u && should_inject_cause) {
        uint32_t rt = (insn >> 16) & 0x1F;
        /* Serve the injected Cause once (exception dispatch).  After this, let
         * QEMU's real Cause register pass through so that synchronous nested
         * exceptions (TLB invalid → page fault, ExcCode=3) are dispatched to
         * do_page_fault rather than a wrong syscall handler.
         * Asynchronous interrupts (CP0 timer, intno=27) reset pending_cause_served
         * in intr_hook before arriving here, so they still get the injected value. */
        uint64_t cause = m->pending_cause;
        uc_reg_write(uc, UC_MIPS_REG_0 + (int)rt, &cause);
        if (m->pending_excode != 1u) {
            m->pending_cause_served = true;
        }
        if (mfc0_cause_inject_log < 256 || in_init_execve_syscall_window(m)) {
            fprintf(stderr,
                    "[CAUSE_MFC0] inject PC=0x%08" PRIX64
                    " cause=0x%08" PRIX64 " rt=%u"
                    " sys_nr=%u syscall_epc=0x%08" PRIX64 "\n",
                    (uint64_t)(uint32_t)address, (uint64_t)(uint32_t)cause, rt,
                    m->pending_syscall_nr,
                    (uint64_t)(uint32_t)m->pending_syscall_epc);
            if (mfc0_cause_inject_log < 256)
                mfc0_cause_inject_log++;
        }
        uint64_t next_pc = address + 4;
        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
        return;
    }
    if ((insn & 0xFFE0FFFFu) == 0x40006800u &&
        in_init_execve_syscall_window(m)) {
        static uint32_t mfc0_cause_skip_log = 0;
        if (mfc0_cause_skip_log < 128) {
            fprintf(stderr,
                    "[CAUSE_MFC0] skip PC=0x%08" PRIX64
                    " served=%u pending_excode=%u sys_nr=%u syscall_epc=0x%08" PRIX64 "\n",
                    (uint64_t)(uint32_t)address,
                    m->pending_cause_served ? 1u : 0u,
                    m->pending_excode, m->pending_syscall_nr,
                    (uint64_t)(uint32_t)m->pending_syscall_epc);
            mfc0_cause_skip_log++;
        }
    }

    /*
     * Synthetic user TLB refill support:
     * when machine_run injects a user-mode TLBL/TLBS exception from
     * UC_ERR_READ_UNMAPPED, provide BadVAddr/EntryHi values expected by
     * Linux refill handlers that start at 0x80000000.
     */
    if ((insn & 0xFFE0FFFFu) == 0x40004000u &&
        (m->pending_excode == MIPS_EXCCODE_TLBL ||
         m->pending_excode == MIPS_EXCCODE_TLBS)) {
        uint32_t rt = (insn >> 16) & 0x1Fu;
        uint64_t badv = (uint64_t)(uint32_t)m->shadow_cp0_badvaddr;
        uc_reg_write(uc, UC_MIPS_REG_0 + (int)rt, &badv);
        static uint32_t badv_inject_log = 0;
        if (badv_inject_log < 128) {
            fprintf(stderr,
                    "[BADVADDR_MFC0] inject PC=0x%08" PRIX64
                    " badv=0x%08" PRIX64 " rt=%u excode=%u\n",
                    (uint64_t)(uint32_t)address, badv, rt, m->pending_excode);
            badv_inject_log++;
        }
        uint64_t next_pc = address + 4;
        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
        return;
    }

    if ((insn & 0xFFE0FFFFu) == 0x40002000u &&
        (m->pending_excode == MIPS_EXCCODE_TLBL ||
         m->pending_excode == MIPS_EXCCODE_TLBS)) {
        uint32_t rt = (insn >> 16) & 0x1Fu;
        uint32_t ctx32 = (uint32_t)m->shadow_cp0_context;
        /*
         * Force Context address into kseg0 if PTEBase points to kuseg.
         *
         * The kernel may set PTEBase=0 (via mtc0 zero, $4), making
         * Context produce kuseg addresses (e.g. 0x00155540).  On real
         * MIPS, the TLB refill handler reads PTEs from this address,
         * which goes through the TLB (wired entries cover the page table).
         * In Unicorn, kuseg is a flat PA region — separate from kseg0.
         * The kernel writes PTEs to kseg0 (0x80155540), so the PA copy
         * at 0x00155540 becomes stale.  Forcing Context into kseg0
         * makes the refill handler read from the coherent kseg0 region.
         */
        if (ctx32 < 0x80000000u && ctx32 < (uint32_t)m->cfg.sdram_size)
            ctx32 |= 0x80000000u;
        uint64_t ctx = (uint64_t)ctx32;
        uc_reg_write(uc, UC_MIPS_REG_0 + (int)rt, &ctx);
        static uint32_t context_inject_log = 0;
        if (context_inject_log < 128) {
            fprintf(stderr,
                    "[CONTEXT_MFC0] inject PC=0x%08" PRIX64
                    " ctx=0x%08" PRIX64 " rt=%u excode=%u\n",
                    (uint64_t)(uint32_t)address, ctx, rt, m->pending_excode);
            context_inject_log++;
        }
        uint64_t next_pc = address + 4;
        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
        return;
    }

    if ((insn & 0xFFE0FFFFu) == 0x40005000u &&
        (m->pending_excode == MIPS_EXCCODE_TLBL ||
         m->pending_excode == MIPS_EXCCODE_TLBS)) {
        uint32_t rt = (insn >> 16) & 0x1Fu;
        uint64_t hi = (uint64_t)(uint32_t)m->shadow_cp0_entryhi;
        uc_reg_write(uc, UC_MIPS_REG_0 + (int)rt, &hi);
        static uint32_t entryhi_inject_log = 0;
        if (entryhi_inject_log < 128) {
            fprintf(stderr,
                    "[ENTRYHI_MFC0] inject PC=0x%08" PRIX64
                    " entryhi=0x%08" PRIX64 " rt=%u excode=%u\n",
                    (uint64_t)(uint32_t)address, hi, rt, m->pending_excode);
            entryhi_inject_log++;
        }
        uint64_t next_pc = address + 4;
        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
        return;
    }

    /*
     * MFC0 $rt, $14, 0  — EPC register (CP0 register 14, sel 0)
     * Encoding: 000100 00000 rt 01110 00000 000000
     *           op=0x10 rs=0  rt    rd=14  sel=0
     * Mask:     0xFFE0FFFF
     * Match:    0x40007000
     *
     * Return the EPC saved during intr_hook so the exception handler
     * can correctly restore the return address before ERET.
     * Only intercept when we are inside our injected exception context
     * (pending_excode != 0).
     */
    if ((insn & 0xFFE0FFFFu) == 0x40007000u) {
        if (mfc0_epc_seen_log < 256 || in_init_execve_syscall_window(m)) {
            fprintf(stderr,
                    "[EPC_MFC0] seen PC=0x%08" PRIX64
                    " pending_excode=%u served=%u epc=0x%08" PRIX64
                    " syscall_epc=0x%08" PRIX64 " sys_nr=%u\n",
                    (uint64_t)(uint32_t)address, m->pending_excode,
                    m->pending_epc_served ? 1u : 0u, (uint64_t)(uint32_t)m->pending_epc,
                    (uint64_t)(uint32_t)m->pending_syscall_epc,
                    m->pending_syscall_nr);
            if (mfc0_epc_seen_log < 256)
                mfc0_epc_seen_log++;
        }
    }
    if ((insn & 0xFFE0FFFFu) == 0x40007000u &&
        m->pending_excode != 0 && !m->pending_epc_served) {
        uint32_t rt = (insn >> 16) & 0x1F;
        /* Serve the injected EPC once (SAVE_SOME frame build).  Subsequent MFC0
         * EPC reads (e.g. from nested exception handlers) get QEMU's real EPC so
         * that the correct return address is saved for each nested exception. */
        uint64_t epc = m->pending_epc;
        uc_reg_write(uc, UC_MIPS_REG_0 + (int)rt, &epc);
        m->pending_epc_served = true;
        if (mfc0_epc_inject_log < 256 || in_init_execve_syscall_window(m)) {
            fprintf(stderr,
                    "[EPC_MFC0] inject PC=0x%08" PRIX64 " epc=0x%08" PRIX64
                    " rt=%u sys_nr=%u syscall_epc=0x%08" PRIX64 "\n",
                    (uint64_t)(uint32_t)address, (uint64_t)(uint32_t)epc, rt,
                    m->pending_syscall_nr,
                    (uint64_t)(uint32_t)m->pending_syscall_epc);
            if (mfc0_epc_inject_log < 256)
                mfc0_epc_inject_log++;
        }
        uint64_t next_pc = address + 4;
        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
        return;
    }

    /*
     * MTC0 $rt, $14, 0  — write EPC (CP0 register 14, sel 0)
     * Encoding: 000100 00100 rt 01110 00000 000000
     *           op=0x10 rs=4  rt    rd=14  sel=0
     * Match:    0x40807000 (rs=4 = MTC0)
     *
     * The syscall exit path writes the adjusted return address back to EPC
     * via MTC0 before ERET.  We capture that write so ERET returns correctly.
     * Only intercept when we are inside our injected exception context.
     * Outside that context, Unicorn's QEMU executes the real MTC0 EPC
     * instruction, keeping the CPU's internal CP0_EPC register consistent.
     */
    if ((insn & 0xFFE0FFFFu) == 0x40807000u &&
        m->pending_excode == 0 && m->pending_epc == 0) {
        /* Diagnostic: MTC0 CP0_EPC with no pending exception context */
        uint32_t rt_diag = (insn >> 16) & 0x1F;
        uint64_t val_diag = 0;
        uc_reg_read(uc, UC_MIPS_REG_0 + (int)rt_diag, &val_diag);
        static uint32_t mtc0_epc_lost_log = 0;
        if (mtc0_epc_lost_log < 64) {
            fprintf(stderr,
                    "[MTC0_EPC_EXCODE0] val=0x%08" PRIX64 " PC=0x%08" PRIX64
                    " execve_active=%u has_saved=%u saved_excode=%u"
                    " nr=%u epc_written=%u\n",
                    (uint64_t)(uint32_t)val_diag, (uint64_t)(uint32_t)address,
                    m->execve_watch_active ? 1u : 0u,
                    m->has_saved_exception ? 1u : 0u,
                    m->saved_pending_excode,
                    m->pending_syscall_nr,
                    m->epc_was_written ? 1u : 0u);
            mtc0_epc_lost_log++;
        }
        /* Don't intercept — let native MTC0 execute */
    }
    /*
     * Intercept MTC0 EPC when we have a pending exception context.
     * This includes: SYSCALL (excode=8), TLBL (excode=2), TLBS (excode=3),
     * and hardware interrupts (excode=0 with pending_epc != 0).
     * For IRQ returns, pending_excode=0 but pending_epc holds the
     * interrupted PC.  Without interception, the native ERET reads stale
     * CP0_EPC (which we can't write).
     */
    if ((insn & 0xFFE0FFFFu) == 0x40807000u &&
        (m->pending_excode != 0 || m->pending_epc != 0)) {
        uint32_t rt = (insn >> 16) & 0x1F;
        uint64_t val = 0;
        uc_reg_read(uc, UC_MIPS_REG_0 + (int)rt, &val);
        if (m->pending_excode == MIPS_EXCCODE_SYS &&
            (uint32_t)val == (uint32_t)m->pending_syscall_epc) {
            static uint32_t mtc0_epc_plus4_fix_log = 0;
            uint64_t fixed = (uint32_t)val + 4u;
            if (mtc0_epc_plus4_fix_log < 64) {
                fprintf(stderr,
                        "[MTC0_EPC_FIXUP_PLUS4] old=0x%08" PRIX64
                        " fixed=0x%08" PRIX64 " sys_nr=%u syscall_epc=0x%08" PRIX64
                        " pc=0x%08" PRIX64 "\n",
                        (uint64_t)(uint32_t)val, (uint64_t)(uint32_t)fixed,
                        m->pending_syscall_nr,
                        (uint64_t)(uint32_t)m->pending_syscall_epc,
                        (uint64_t)(uint32_t)address);
                mtc0_epc_plus4_fix_log++;
            }
            val = fixed;
        }
        if (m->pending_excode == MIPS_EXCCODE_SYS &&
            (mtc0_epc_sys_log < 96 || in_init_execve_syscall_window(m))) {
            uint64_t sp = 0, ra = 0, status_now = 0;
            uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
            uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
            uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &status_now);
            const char *mode = (val & 0xFFFFFFFF80000000ull) == 0xFFFFFFFF80000000ull
                               ? "kseg" : "kuseg";
            fprintf(stderr,
                    "[MTC0_EPC] pending_excode=%u old_epc=0x%08" PRIX64
                    " new_epc=0x%08" PRIX64 " (%s) PC=0x%08" PRIX64
                    " STATUS=0x%08" PRIX64 " sp=0x%08" PRIX64
                    " ra=0x%08" PRIX64 " served_epc=%u epc_written=%u"
                    " sys_nr=%u syscall_epc=0x%08" PRIX64 "\n",
                    m->pending_excode, (uint64_t)(uint32_t)m->pending_epc,
                    (uint64_t)(uint32_t)val, mode,
                    (uint64_t)(uint32_t)address, status_now,
                    sp, ra, m->pending_epc_served ? 1u : 0u,
                    m->epc_was_written ? 1u : 0u,
                    m->pending_syscall_nr,
                    (uint64_t)(uint32_t)m->pending_syscall_epc);
            if (mtc0_epc_sys_log < 96)
                mtc0_epc_sys_log++;
        }
        m->pending_epc      = val;   /* capture return address written by exit path */
        m->epc_was_written  = true;  /* arm the ERET intercept for the next ERET    */
        /* Track execve user entry separately: if this MTC0 writes a
         * kuseg address during an active execve, save it so that the
         * ERET_EXECVE_NATIVE path (which fires when a timer interrupt
         * resets epc_was_written via ERET_PASSTHROUGH + restore) can
         * still correctly redirect to user space. */
        if (m->execve_watch_active &&
            m->pending_syscall_nr == 4011u &&
            (uint32_t)val < 0x80000000u) {
            m->execve_user_entry = (uint32_t)val;
            m->execve_user_entry_valid = true;
        }
        uint64_t next_pc = address + 4;
        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
        return;
    }

    /*
     * VR4131 power management: STANDBY / SUSPEND / HIBERNATE.
     * Encodings: 0x42000021 / 0x42000022 / 0x42000023
     *
     * On real hardware, these halt the CPU until an interrupt fires.
     * In the emulator we treat them as NOPs: the CPU "wakes up" immediately
     * and continues at the next instruction.  The idle function will loop
     * back and re-check interrupt sources on the next scheduler pass.
     */
    if (insn == UINT32_C(0x42000021) ||   /* standby */
        insn == UINT32_C(0x42000022) ||   /* suspend */
        insn == UINT32_C(0x42000023)) {   /* hibernate */
        static uint32_t pm_log = 0;
        if (pm_log < 8) {
            const char *name = (insn == UINT32_C(0x42000021)) ? "STANDBY" :
                               (insn == UINT32_C(0x42000022)) ? "SUSPEND" :
                                                                "HIBERNATE";
            fprintf(stderr,
                    "[VR4131_PM] %s at PC=0x%08" PRIX64 " — NOP'd\n",
                    name, address);
            pm_log++;
        }
        /* Skip past the instruction: advance PC by 4 */
        uint64_t next_pc = address + 4;
        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
        return;
    }

    /*
     * ERET  — return from exception.
     * Encoding: 0x42000018
     *
     * In normal MIPS SOFTMMU, ERET clears Status.EXL and jumps to EPC.
     * When we are managing the exception state manually (pending_epc != 0),
     * we perform the ERET ourselves:
     *   1. Clear EXL in Status.
     *   2. Set PC = pending_epc.
     *   3. Clear pending state.
     */
    /*
     * ERET intercept: only fire when the kernel's exit path has already
     * written the return address to EPC via MTC0 EPC (epc_was_written=true).
     *
     * Without this guard, the intercept would also fire for nested ERET
     * instructions from TLB refill handlers (0x80000000) that run during
     * user-space access faults inside the syscall/IRQ handler body.  Those
     * intermediate ERETs must be left to Unicorn's native ERET logic, which
     * reads the real CP0 EPC (set by the hardware TLB-miss exception entry)
     * and returns to the faulting instruction correctly.
     */
    if (insn == 0x42000018u) {
        if (m->execve_watch_active || in_init_execve_syscall_window(m)) {
            uint64_t eret_diag_status = 0;
            uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &eret_diag_status);
            static uint32_t eret_trace_log = 0;
            if (eret_trace_log < 256) {
                fprintf(stderr,
                        "[ERET_TRACE] PC=0x%08" PRIX64 " STATUS=0x%08" PRIX64
                        " pend_exc=%u pend_epc=0x%08" PRIX64
                        " epc_wr=%u has_saved=%u execve_act=%u nr=%u\n",
                        (uint64_t)(uint32_t)address, eret_diag_status,
                        m->pending_excode, (uint64_t)(uint32_t)m->pending_epc,
                        m->epc_was_written ? 1u : 0u,
                        m->has_saved_exception ? 1u : 0u,
                        m->execve_watch_active ? 1u : 0u,
                        m->pending_syscall_nr);
                eret_trace_log++;
            }
        }
        if (m->pending_excode == 0 && m->has_saved_exception) {
            if (m->saved_exception_reason == SAVE_REASON_KERNEL_RI) {
                static uint32_t eret_ri_restore_log = 0;
                if (eret_ri_restore_log < 64) {
                    fprintf(stderr,
                            "[ERET_RI_RESTORE] PC=0x%08" PRIX64
                            " restoring saved_excode=%u saved_epc=0x%08" PRIX64 "\n",
                            (uint64_t)(uint32_t)address,
                            m->saved_pending_excode,
                            (uint64_t)(uint32_t)m->saved_pending_epc);
                    eret_ri_restore_log++;
                }
            }
            restore_pending_exception(m);
            return;
        }
        if (m->pending_excode == 0 && !m->has_saved_exception) {
            /* Diagnostic: ERET during execve with all state lost */
            uint64_t eret_status = 0;
            uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &eret_status);
            static uint32_t eret_lost_log = 0;
            if (eret_lost_log < 64) {
                fprintf(stderr,
                        "[ERET_STATE_LOST] PC=0x%08" PRIX64 " STATUS=0x%08" PRIX64
                        " epc_written=%u nr=%u execve_user_valid=%u"
                        " execve_user=0x%08X\n",
                        (uint64_t)(uint32_t)address, eret_status,
                        m->epc_was_written ? 1u : 0u,
                        m->pending_syscall_nr,
                        m->execve_user_entry_valid ? 1u : 0u,
                        m->execve_user_entry);
                eret_lost_log++;
            }
            /* Fall through to native ERET */
        }
        if (m->pending_excode != 0) {
            static uint32_t eret_pending_log = 0;
            static uint32_t syscall_ret_log = 0;
            uint64_t status_snapshot = 0;
            uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &status_snapshot);
            if (m->pending_excode == MIPS_EXCCODE_SYS &&
                (m->epc_was_written || in_init_execve_syscall_window(m)) &&
                syscall_ret_log < 256) {
                uint64_t v0 = 0, a3 = 0;
                uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
                uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
                fprintf(stderr,
                        "[SYSCALL_RET] EPC=0x%08" PRIX64 " nr=%u"
                        " a0=0x%08" PRIX64 " \"%s\""
                        " v0=0x%08" PRIX64 " a3=0x%08" PRIX64
                        " status=0x%08" PRIX64
                        " epc_written=%u syscall_epc=0x%08" PRIX64 "\n",
                        (uint64_t)(uint32_t)m->pending_epc,
                        m->pending_syscall_nr,
                        (uint64_t)(uint32_t)m->pending_syscall_a0,
                        m->pending_syscall_a0_str,
                        (uint64_t)(uint32_t)v0,
                        (uint64_t)(uint32_t)a3,
                        status_snapshot,
                        m->epc_was_written ? 1u : 0u,
                        (uint64_t)(uint32_t)m->pending_syscall_epc);
                syscall_ret_log++;
            }
            if (eret_pending_log < 96) {
                uint64_t v0 = 0, a3 = 0;
                uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
                uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
                fprintf(stderr,
                        "[ERET] pending_excode=%u epc_written=%u pending_epc=0x%08" PRIX64
                        " PC=0x%08" PRIX64 " STATUS=0x%08" PRIX64
                        " v0=0x%08" PRIX64 " a3=0x%08" PRIX64
                        " served_epc=%u served_cause=%u\n",
                        m->pending_excode, m->epc_was_written ? 1u : 0u,
                        (uint64_t)(uint32_t)m->pending_epc, (uint64_t)(uint32_t)address,
                        status_snapshot, (uint64_t)(uint32_t)v0,
                        (uint64_t)(uint32_t)a3,
                        m->pending_epc_served ? 1u : 0u,
                        m->pending_cause_served ? 1u : 0u);
                eret_pending_log++;
            }
            /*
             * ERET from kernel-mode TLBS/TLBL injection.
             *
             * When we injected a TLBS or TLBL exception from the WRITE/READ
             * UNMAPPED handler, the kernel's TLB refill handler has run.
             *
             * The MIPS fast TLB refill at 0x80000000 loads the PTE from the
             * page table. Two cases:
             *
             * (a) PTE valid: TLBWI installed a valid mapping → shadow TLB has
             *     a confident entry → return to faulting instruction for retry.
             *
             * (b) PTE is zero (page not yet allocated): TLBWR wrote invalid
             *     entries (lo=0) → shadow TLB has no confident entry.
             *     On real hardware, the retry would TLB-miss again, and since
             *     EXL=1, the second miss goes to 0x80000180 (general exception
             *     vector) which calls do_page_fault → allocate → TLBWI.
             *     We simulate this by redirecting to 0x80000180 with EXL=1
             *     and the original fault info preserved.
             */
            if ((m->pending_excode == MIPS_EXCCODE_TLBS ||
                 m->pending_excode == MIPS_EXCCODE_TLBL) &&
                !m->epc_was_written) {
                uint32_t fault_va = (uint32_t)m->shadow_cp0_badvaddr;
                tlb_lookup_result_t eret_tlb = shadow_tlb_lookup(m, fault_va);
                bool tlb_valid = eret_tlb.hit && eret_tlb.confident;

                /* For TLBS, also check D (dirty/writable) bit */
                if (tlb_valid && m->pending_excode == MIPS_EXCCODE_TLBS) {
                    bool odd = (((uint64_t)fault_va & eret_tlb.page_bytes) != 0u);
                    uint32_t lo = odd ? eret_tlb.lo1 : eret_tlb.lo0;
                    if ((lo & 0x4u) == 0u)
                        tlb_valid = false;  /* D=0, need do_page_fault to upgrade */
                }

                if (tlb_valid) {
                    /* Case (a): TLB entry is valid, return to faulting insn.
                     * Also populate the kuseg flat mapping from SDRAM so the
                     * instruction/data at the VA is accessible. */
                    shadow_tlb_populate(m, fault_va, true,
                                        "ERET_KTLB_POPULATE",
                                        (uint32_t)address);

                    uint64_t fault_pc = m->pending_epc;

                    /* Clear EXL (the ERET we're skipping would have cleared it). */
                    uint64_t status = status_snapshot & ~(uint64_t)0x2u;
                    uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &status);

                    /* Skip the ERET by writing PC directly to the fault address.
                     * This avoids NOP-patching and uc_emu_stop entirely: the
                     * ERET instruction does NOT execute, so there's no stale
                     * CP0_EPC read.  This follows the same pattern as the
                     * standby/suspend/hibernate skip at machine.c:3854. */
                    uint64_t fault_pc_sext = mips_sext((uint32_t)fault_pc);
                    uc_reg_write(uc, UC_MIPS_REG_PC, &fault_pc_sext);

                    static uint32_t eret_ktlb_ok_log = 0;
                    if (eret_ktlb_ok_log < 128) {
                        fprintf(stderr,
                                "[ERET_KERNEL_TLB_OK] excode=%u fault_pc=0x%08X"
                                " fault_va=0x%08X pa=0x%08" PRIX64
                                " status=0x%08" PRIX64 " has_saved=%u (PC-skip)\n",
                                m->pending_excode, (uint32_t)fault_pc,
                                fault_va, eret_tlb.pa_page,
                                (uint64_t)(uint32_t)status,
                                m->has_saved_exception ? 1u : 0u);
                        eret_ktlb_ok_log++;
                    }
                    /* Diagnostic: dump pgd→pte chain for the code page 0x2AAA8000
                     * to understand why the PTE gets zeroed. */
                    if ((fault_va & 0xFFFFF000u) == 0x2AAA8000u) {
                        static uint32_t pte_diag_log = 0;
                        if (pte_diag_log < 32) {
                            uint32_t pgd_ptr = 0;
                            uc_mem_read(uc, 0x80260CD8u, &pgd_ptr, 4);
                            uint32_t pgd_idx = (0x2AAA8000u >> 22);
                            uint32_t pte_page = 0;
                            uc_mem_read(uc, (uint64_t)pgd_ptr + pgd_idx * 4, &pte_page, 4);
                            /* PTE offset from Context: (Context >> 1) & 0xFF8
                             * Context for VA 0x2AAA8000: BadVPN2 = 0x15554
                             * Context[22:4] = 0x15554, Context = 0x155540
                             * (Context >> 1) & 0xFF8 = (0x0AAAA0) & 0xFF8 = 0xAA0 */
                            uint32_t pte_off = 0xAA0u;
                            uint32_t pte0 = 0, pte1 = 0;
                            uc_mem_read(uc, (uint64_t)pte_page + pte_off, &pte0, 4);
                            uc_mem_read(uc, (uint64_t)pte_page + pte_off + 4, &pte1, 4);
                            /* Also read from PA alias for comparison */
                            uint32_t pte_page_pa = pte_page & 0x1FFFFFFFu;
                            uint32_t pte0_pa = 0, pte1_pa = 0;
                            uc_mem_read(uc, (uint64_t)pte_page_pa + pte_off, &pte0_pa, 4);
                            uc_mem_read(uc, (uint64_t)pte_page_pa + pte_off + 4, &pte1_pa, 4);
                            fprintf(stderr,
                                    "[PTE_DIAG] pgd_ptr=0x%08X pgd[%u]=0x%08X"
                                    " pte@kseg0[0x%08X+0x%X]=0x%08X/0x%08X"
                                    " pte@pa[0x%08X+0x%X]=0x%08X/0x%08X\n",
                                    pgd_ptr, pgd_idx, pte_page,
                                    pte_page, pte_off, pte0, pte1,
                                    pte_page_pa, pte_off, pte0_pa, pte1_pa);
                            /* Dump the TLB refill handler at 0x80000000 */
                            if (pte_diag_log == 0) {
                                uint32_t hdl[24] = {0};
                                uc_mem_read(uc, 0x80000000u, hdl, sizeof(hdl));
                                fprintf(stderr, "[REFILL_HANDLER_DUMP]");
                                for (int i = 0; i < 24; i++)
                                    fprintf(stderr, " %08X", hdl[i]);
                                fprintf(stderr, "\n");
                            }
                            pte_diag_log++;
                        }
                    }

                    m->pending_epc          = 0;
                    m->pending_excode       = 0;
                    m->pending_cause        = 0;
                    m->epc_was_written      = false;
                    m->pending_cause_served = false;
                    m->pending_epc_served   = false;
                    restore_pending_exception(m);
                    /* No uc_emu_stop needed: PC was redirected to fault_pc
                     * and the ERET is skipped.  Execution continues within
                     * the same batch at the faulting instruction. */
                    return;
                } else {
                    /* Case (b): PTE was zero/invalid — redirect to general
                     * exception vector (0x80000180) to trigger do_page_fault.
                     * Keep EXL=1 and preserve pending TLBS/TLBL state.
                     *
                     * Skip the ERET by writing PC directly to the general
                     * exception vector.  This avoids NOP-patching (which
                     * fails because the decoded ERET in the current TB
                     * still executes, reading stale CP0_EPC).  Same pattern
                     * as the TLB_OK PC-skip fix (commit 675a5f75). */
                    uint64_t gen_vec = mips_sext(0x80000180u);
                    uc_reg_write(uc, UC_MIPS_REG_PC, &gen_vec);
                    /* Re-arm MFC0 intercepts for the general handler */
                    m->pending_cause_served = false;
                    m->pending_epc_served   = false;

                    static uint32_t eret_ktlb_pgfault_log = 0;
                    if (eret_ktlb_pgfault_log < 128) {
                        fprintf(stderr,
                                "[ERET_KERNEL_TLB_PGFAULT] excode=%u fault_pc=0x%08X"
                                " fault_va=0x%08X status=0x%08" PRIX64
                                " -> 0x80000180 (do_page_fault) (PC-skip)\n",
                                m->pending_excode,
                                (uint32_t)m->pending_epc,
                                fault_va,
                                status_snapshot);
                        eret_ktlb_pgfault_log++;
                    }
                    return;
                }
            }

            /*
             * Intercept the ERET when we can reliably determine the return address:
             *  (a) epc_was_written=true: restore_all or start_thread wrote EPC via MTC0 —
             *      always intercept regardless of execve state.
             *  (b) pending_excode==SYS and execve NOT in flight: the syscall body has
             *      returned normally and we should deliver to SYSCALL+4.
             *      For execve (nr=4011), require explicit EPC write to avoid
             *      premature retirement at notification-only SYSCALL+4 events.
             *
             * Do NOT intercept when execve_watch_active is true and epc_was_written is
             * false: that means a nested TLB handler is executing its ERET while
             * do_execve is still running.  Intercepting here would send PC back to
             * SYSCALL+4 prematurely (before do_execve completes), causing init() to
             * see a spurious successful execve.  Pass these through to ERET_NATIVE so
             * the kernel's TLB refill ERET can return to the faulting instruction
             * inside do_execve and let it continue running.
             */
            bool syscall_is_execve =
                (m->pending_excode == MIPS_EXCCODE_SYS &&
                 m->pending_syscall_nr == 4011u);
            if (m->epc_was_written ||
                (m->pending_excode == MIPS_EXCCODE_SYS &&
                 !m->execve_watch_active &&
                 !syscall_is_execve)) {
                uint64_t status = status_snapshot & ~(uint64_t)0x2u;   /* clear EXL */
                bool execve_handoff_ctx =
                    (m->pending_excode == MIPS_EXCCODE_SYS &&
                     m->pending_syscall_nr == 4011u &&
                     is_init_execve_epc((uint32_t)m->pending_syscall_epc));
                if (execve_handoff_ctx) {
                    /* Quarantine stale post-syscall IRQ delivery while entering
                     * first user instructions. Kernel will re-enable as needed. */
                    status &= ~(uint64_t)0xFF01u; /* IM[7:0]=0, IE=0 */
                }
                uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &status);

                uint64_t epc = m->pending_epc;
                /* If it's a syscall and the kernel didn't write EPC, return to PC+4 */
                if (!m->epc_was_written && m->pending_excode == MIPS_EXCCODE_SYS) {
                    epc += 4;
                }
                uc_reg_write(uc, UC_MIPS_REG_PC, &epc);

                if (eret_pending_log < 96) {
                    fprintf(stderr, "[ERET_MANUAL] returning to EPC=0x%08" PRIX64 "\n", epc);
                }

                if ((uint32_t)epc < 0x80000000u && execve_handoff_ctx) {
                    uint64_t sp_val = 0;
                    uc_reg_read(uc, UC_MIPS_REG_SP, &sp_val);
                    arm_execve_user_handoff(m, epc, sp_val);

                    /* Keep execution in the same uc_emu_start batch so kuseg
                     * softmmu state remains warm across the handoff seam. */
                    uint32_t probe = 0;
                    uc_err re = uc_mem_read(uc, (uint32_t)epc, &probe, 4);
                    fprintf(stderr,
                            "[ERET_MANUAL_KUSEG] target=0x%08X sp=0x%08" PRIX64
                            " status=0x%08" PRIX64
                            " insn@target=0x%08X read_err=%d\n",
                            (uint32_t)epc, sp_val, status, probe, re);

                    if (probe == 0u) {
                        bool populated = shadow_tlb_populate(m, (uint32_t)epc, true,
                                                             "ERET_MANUAL_KUSEG",
                                                             (uint32_t)address);
                        re = uc_mem_read(uc, (uint32_t)epc, &probe, 4);
                        fprintf(stderr,
                                "[ERET_MANUAL_KUSEG] after populate target=0x%08X"
                                " insn@target=0x%08X read_err=%d\n",
                                (uint32_t)epc, probe, re);

                        if (probe == 0u) {
                            uint32_t alias_pa = (uint32_t)epc & 0x1FFFFFFFu;
                            uint32_t alias_probe = 0;
                            uc_err alias_re = uc_mem_read(uc, alias_pa, &alias_probe, 4);
                            static uint32_t alias_log = 0;
                            if (alias_log < 256) {
                                fprintf(stderr,
                                        "[ERET_MANUAL_KUSEG_ALIAS] target=0x%08X"
                                        " alias_pa=0x%08X alias_insn=0x%08X alias_err=%d"
                                        " populated=%u\n",
                                        (uint32_t)epc, alias_pa, alias_probe, alias_re,
                                        populated ? 1u : 0u);
                                alias_log++;
                            }
                            if (alias_re == UC_ERR_OK && alias_probe != 0u) {
                                tlb_map_kuseg_page(m, (uint32_t)epc,
                                                   (uint64_t)alias_pa, 0x1000u);
                                re = uc_mem_read(uc, (uint32_t)epc, &probe, 4);
                                fprintf(stderr,
                                        "[ERET_MANUAL_KUSEG_ALIAS] after alias map"
                                        " target=0x%08X insn@target=0x%08X read_err=%d\n",
                                        (uint32_t)epc, probe, re);
                            }
                        }

                        /*
                         * Code page is still zero after all populate attempts.
                         * On real hardware, the instruction fetch would TLB-miss
                         * and trigger TLBL → do_page_fault → filemap_nopage →
                         * reads the code from the ramdisk.
                         *
                         * Inject TLBL to make the kernel load the code page.
                         */
                        if (probe == 0u && m->kernel_vm_ready) {
                            uint32_t user_pc = (uint32_t)epc;
                            fprintf(stderr,
                                    "[ERET_KUSEG_TLBL_INJECT] target=0x%08X"
                                    " code_is_zero -> injecting TLBL\n",
                                    user_pc);

                            /* Save the current SYS exception state */
                            save_pending_exception(m);

                            /* Set up shadow CP0 for TLBL */
                            uint32_t asid = (uint32_t)(
                                m->shadow_cp0_entryhi_live_valid
                                    ? m->shadow_cp0_entryhi_live
                                    : m->shadow_cp0_entryhi) & 0xFFu;
                            uint32_t ctx_base = (uint32_t)m->shadow_cp0_context
                                                & 0xFF800000u;
                            uint32_t ctx_badvpn2 = ((user_pc >> 9) & 0x007FFFF0u);
                            m->shadow_cp0_badvaddr = (uint64_t)user_pc;
                            m->shadow_cp0_entryhi = (uint64_t)(
                                (user_pc & 0xFFFFE000u) | asid);
                            m->shadow_cp0_context = (uint64_t)(
                                ctx_base | ctx_badvpn2);

                            m->pending_epc          = user_pc;
                            m->pending_excode       = MIPS_EXCCODE_TLBL;
                            m->pending_cause        = (uint32_t)(
                                MIPS_EXCCODE_TLBL << 2);
                            m->epc_was_written      = false;
                            m->pending_cause_served = false;
                            m->pending_epc_served   = false;

                            /* Set EXL=1 (clear IE to prevent IRQ during refill) */
                            uint64_t tlbl_status = status | 0x2u;
                            uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS,
                                         &tlbl_status);

                            uint64_t vec = (tlbl_status & 0x00400000u)
                                ? mips_sext(0xBFC00380u)
                                : mips_sext(0x80000000u);
                            uc_reg_write(uc, UC_MIPS_REG_PC, &vec);
                            return;
                        }
                    }
                    (void)trace_user_handoff_entry_probe(m, "ERET",
                                                         (uint32_t)epc,
                                                         (uint32_t)epc,
                                                         m->shadow_cp0_badvaddr,
                                                         false);
                }

                if (m->pending_excode == MIPS_EXCCODE_SYS)
                    clear_synthetic_syscall_state(m, true);
                else {
                    m->pending_epc          = 0;
                    m->pending_excode       = 0;
                    m->pending_cause        = 0;
                    m->epc_was_written      = false;
                    m->pending_cause_served = false;
                    m->pending_epc_served   = false;
                }
                restore_pending_exception(m);
                return;
            }

            /* ERET_TLB_PASSTHROUGH: transparent nested-exception return
             * during do_execve.
             *
             * On real MIPS: TLB miss -> handler fills TLB -> ERET ->
             * faulting insn retries and succeeds.  The handler only
             * clobbers $k0/$k1.
             *
             * We replicate this: read QEMU's native CP0 EPC (set to the
             * faulting insn address at TLB miss time), clear EXL, jump
             * there.  All synthetic SYSCALL state is preserved so the
             * eventual syscall return ERET (with epc_was_written=true)
             * still works correctly.
             *
             * Previous approach (ERET_EXECVE_RETRY) restarted do_execve
             * from its entry point, which corrupted kernel state:
             * search_binary_handler() calls set_fs(USER_DS) during
             * partial execution, and the restart could not undo that —
             * causing all kseg0 pointer accesses to fail access_ok()
             * with EFAULT.
             */
            if (m->pending_excode == MIPS_EXCCODE_SYS &&
                m->execve_watch_active &&
                !m->epc_was_written) {
                /*
                 * Check if the syscall's MTC0 CP0_EPC already wrote a user
                 * entry point but epc_was_written was reset by a timer
                 * interrupt's ERET_PASSTHROUGH + restore_pending_exception.
                 * In that case, this is the ACTUAL syscall return ERET,
                 * not a nested exception ERET.  Redirect to user space.
                 */
                if (m->execve_user_entry_valid &&
                    (uint32_t)(status_snapshot & 0x2u) == 0u) {
                    /* EXL=0 confirms this is post-restore_all (the MTC0
                     * CP0_STATUS already cleared EXL).  Nested exception
                     * ERETshave EXL=1 and are handled by ERET_PASSTHROUGH
                     * above. */
                    uint64_t user_entry = mips_sext(m->execve_user_entry);
                    uint64_t status = status_snapshot & ~(uint64_t)0x2u;
                    /* Quarantine IRQ delivery during first user instructions */
                    status &= ~(uint64_t)0xFF01u;
                    uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &status);
                    uc_reg_write(uc, UC_MIPS_REG_PC, &user_entry);

                    static uint32_t eret_execve_user_log = 0;
                    if (eret_execve_user_log < 64) {
                        fprintf(stderr,
                                "[ERET_EXECVE_USER_ENTRY] user_entry=0x%08X"
                                " nr=%u PC=0x%08" PRIX64 " status=0x%08" PRIX64 "\n",
                                m->execve_user_entry,
                                m->pending_syscall_nr,
                                (uint64_t)(uint32_t)address,
                                (uint64_t)(uint32_t)status);
                        eret_execve_user_log++;
                    }

                    uint64_t sp_val = 0;
                    uc_reg_read(uc, UC_MIPS_REG_SP, &sp_val);
                    arm_execve_user_handoff(m, user_entry, sp_val);

                    /* Probe and populate user code page if needed */
                    uint32_t probe = 0;
                    uc_err re = uc_mem_read(uc, m->execve_user_entry, &probe, 4);
                    if (probe == 0u) {
                        shadow_tlb_populate(m, m->execve_user_entry, true,
                                            "ERET_EXECVE_USER",
                                            (uint32_t)address);
                        re = uc_mem_read(uc, m->execve_user_entry, &probe, 4);
                    }
                    if (probe == 0u && m->kernel_vm_ready) {
                        fprintf(stderr,
                                "[ERET_EXECVE_USER_TLBL] target=0x%08X"
                                " code_is_zero -> injecting TLBL\n",
                                m->execve_user_entry);
                        inject_kernel_kuseg_tlbl(m, (uint64_t)m->execve_user_entry,
                                                  m->execve_user_entry);
                    }

                    m->execve_user_entry_valid = false;
                    clear_synthetic_syscall_state(m, true);
                    m->has_saved_exception = false;
                    return;
                }

                /* Nested ERET during do_execve (timer IRQ, TLB refill, etc.)
                 * Let Unicorn execute the ERET natively — it reads CP0_EPC
                 * (set by the exception entry) and returns to the interrupted
                 * instruction correctly.  Preserve SYS pending state. */
                static uint32_t eret_tlb_pass_log = 0;
                if (eret_tlb_pass_log < 64) {
                    fprintf(stderr,
                            "[ERET_EXECVE_NATIVE] pending_epc=0x%08" PRIX64
                            " nr=%u PC=0x%08" PRIX64 "\n",
                            (uint64_t)(uint32_t)m->pending_epc,
                            m->pending_syscall_nr,
                            (uint64_t)(uint32_t)address);
                    eret_tlb_pass_log++;
                }
                return;
            }

            /*
             * Catch-all for injected TLB exceptions (TLBL/TLBS):
             * If no specific handler matched and we have a pending injected
             * exception with pending_epc set, skip the ERET and redirect
             * to pending_epc.  This prevents native ERET from reading
             * stale CP0_EPC (which we can't write).
             */
            if ((m->pending_excode == MIPS_EXCCODE_TLBL ||
                 m->pending_excode == MIPS_EXCCODE_TLBS) &&
                m->pending_epc != 0) {
                uint64_t fault_pc_sext = mips_sext((uint32_t)m->pending_epc);
                uint64_t clr_status = status_snapshot & ~(uint64_t)0x2u;
                uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &clr_status);
                uc_reg_write(uc, UC_MIPS_REG_PC, &fault_pc_sext);
                static uint32_t eret_tlb_catchall_log = 0;
                if (eret_tlb_catchall_log < 128) {
                    fprintf(stderr,
                            "[ERET_TLB_CATCHALL] excode=%u fault_pc=0x%08X"
                            " PC=0x%08" PRIX64 " (PC-skip)\n",
                            m->pending_excode,
                            (uint32_t)m->pending_epc,
                            (uint64_t)(uint32_t)address);
                    eret_tlb_catchall_log++;
                }
                m->pending_epc          = 0;
                m->pending_excode       = 0;
                m->pending_cause        = 0;
                m->epc_was_written      = false;
                m->pending_cause_served = false;
                m->pending_epc_served   = false;
                restore_pending_exception(m);
                return;
            }

            /*
             * If no explicit MTC0 EPC was observed, let Unicorn execute the
             * native ERET.
             *
             * Execve + deferred TLB miss corner case (2.6 run_init path):
             * when we forced one TLB refill entry from SYSCALL+4, the nested
             * refill handler ERET must return to guest code while preserving
             * pending syscall bookkeeping. Clearing pending_excode here causes
             * intno=26/27 floods at SYSCALL+4 to lose syscall context and
             * livelock in unmapped-recovery loops.
             */
            static uint32_t eret_native_log = 0;
            if (tlb_trace_window_active(m) && eret_native_log < 128) {
                fprintf(stderr,
                        "[ERET_NATIVE] pending_excode=%u pending_epc=0x%08" PRIX64
                        " syscall_epc=0x%08" PRIX64
                        " PC=0x%08" PRIX64 " STATUS=0x%08" PRIX64
                        " has_saved=%u\n",
                        m->pending_excode, (uint64_t)(uint32_t)m->pending_epc,
                        (uint64_t)(uint32_t)m->pending_syscall_epc,
                        (uint64_t)(uint32_t)address, status_snapshot,
                        m->has_saved_exception ? 1u : 0u);
                eret_native_log++;
            }
            bool keep_syscall_state =
                (m->pending_excode == MIPS_EXCCODE_SYS &&
                 m->pending_syscall_nr == 4011u &&
                 !m->epc_was_written &&
                 m->tlb_defer_active);
            if (keep_syscall_state) {
                static uint32_t eret_native_keep_log = 0;
                if (eret_native_keep_log < 64) {
                    fprintf(stderr,
                            "[ERET_NATIVE_KEEP_SYS] nr=%u pending_epc=0x%08" PRIX64
                            " owner_epc=0x%08X defer_count=%u\n",
                            m->pending_syscall_nr,
                            (uint64_t)(uint32_t)m->pending_epc,
                            m->tlb_defer_owner_epc,
                            m->tlb_defer_count);
                    eret_native_keep_log++;
                }
            } else if (m->pending_excode == MIPS_EXCCODE_SYS) {
                clear_synthetic_syscall_state(m, true);
            } else {
                m->pending_epc          = 0;
                m->pending_excode       = 0;
                m->pending_cause        = 0;
                m->epc_was_written      = false;
                m->pending_cause_served = false;
                m->pending_epc_served   = false;
            }
            restore_pending_exception(m);
        }
    }
}


/*
 * Interrupt / CPU-exception hook.
 *
 * UC_HOOK_INTR fires for all Unicorn-level interrupts and CPU exceptions.
 * For MIPS SOFTMMU this includes hardware IRQ delivery AND CPU exceptions
 * such as SYSCALL (exception code 8 / EXCP_SYSCALL in QEMU).
 *
 * Strategy: on MIPS SYSCALL (intno == 8) Unicorn raises UC_ERR_EXCEPTION
 * instead of transparently routing to the guest exception vector.  We handle
 * it here by performing the MIPS exception-entry sequence ourselves:
 *   1. Capture the current PC (EPC — the address of the syscall instruction).
 *   2. Save EPC in a machine-local field so downstream MFC0/EPC intercepts
 *      can return it to the guest.
 *   3. Set CP0 Status.EXL = 1.
 *   4. Redirect PC to the general exception vector (0x80000180, BEV=0).
 *
 * Cause.ExcCode cannot be written via the Unicorn 2.1.4 API.  The
 * except_vec3_r4000 handler at 0x80000180 reads Cause with `mfc0 $k1,Cause`
 * (the very first instruction) and uses ExcCode to index exception_handlers[].
 * We intercept that specific read in prid_hook() below and inject ExcCode=8
 * (SYSCALL) when we are in an active SYSCALL entry.
 */

/*
 * Route a user-space exception to the kernel general exception vector.
 * Sets EXL, redirects PC to 0x80000180, and sets up synthetic EPC/Cause
 * so the kernel exception handler can dispatch it.
 */
static void route_user_exception_to_kernel(machine_t *m, uc_engine *uc,
                                           uint32_t intno,
                                           uint64_t user_pc, uint64_t status)
{
    /* Map Unicorn intno to MIPS ExcCode and exception vector. */
    uint32_t exccode;
    uint64_t exc_vec;
    bool was_exl = (status & 0x2u) != 0;

    switch (intno) {
    case 26u:  /* TLBL */
        exccode = MIPS_EXCCODE_TLBL;
        /* TLB refill vector (0x80000000) if EXL was 0; else general (0x80000180) */
        exc_vec = mips_sext(was_exl ? 0x80000180u : 0x80000000u);
        /* Set up shadow CP0 for TLB handler */
        {
            uint32_t upc = (uint32_t)user_pc;
            uint32_t asid = (uint32_t)(m->shadow_cp0_entryhi_live_valid
                             ? m->shadow_cp0_entryhi_live
                             : m->shadow_cp0_entryhi) & 0xFFu;
            uint32_t ctx_base = (uint32_t)m->shadow_cp0_context & 0xFF800000u;
            uint32_t ctx_badvpn2 = ((upc >> 9) & 0x007FFFF0u);
            m->shadow_cp0_badvaddr = (uint64_t)upc;
            m->shadow_cp0_entryhi = (uint64_t)((upc & 0xFFFFE000u) | asid);
            m->shadow_cp0_context = (uint64_t)(ctx_base | ctx_badvpn2);
        }
        break;
    case 27u:  /* TLBS */
        exccode = MIPS_EXCCODE_TLBS;
        exc_vec = mips_sext(was_exl ? 0x80000180u : 0x80000000u);
        {
            uint32_t upc = (uint32_t)user_pc;
            uint32_t asid = (uint32_t)(m->shadow_cp0_entryhi_live_valid
                             ? m->shadow_cp0_entryhi_live
                             : m->shadow_cp0_entryhi) & 0xFFu;
            uint32_t ctx_base = (uint32_t)m->shadow_cp0_context & 0xFF800000u;
            uint32_t ctx_badvpn2 = ((upc >> 9) & 0x007FFFF0u);
            m->shadow_cp0_badvaddr = (uint64_t)upc;
            m->shadow_cp0_entryhi = (uint64_t)((upc & 0xFFFFE000u) | asid);
            m->shadow_cp0_context = (uint64_t)(ctx_base | ctx_badvpn2);
        }
        break;
    default:   /* intno=12: RI or CpU */
        exccode = 10u;  /* RI (Reserved Instruction) */
        exc_vec = mips_sext(0x80000180u);
        break;
    }

    uint64_t exc_status = status | 0x2u;  /* set EXL */
    uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &exc_status);
    uc_reg_write(uc, UC_MIPS_REG_PC, &exc_vec);

    m->pending_epc = user_pc;
    m->pending_excode = exccode;
    m->pending_cause = (uint32_t)(exccode << 2);
    m->epc_was_written = false;
    m->pending_cause_served = false;
    m->pending_epc_served = false;

    static uint32_t user_exc_log = 0;
    if (user_exc_log < 64) {
        fprintf(stderr,
                "[USER_EXCEPTION] intno=%u exccode=%u pc=0x%08" PRIX64
                " -> vec=0x%08" PRIX64 "\n",
                intno, exccode, (uint64_t)(uint32_t)user_pc,
                (uint64_t)(uint32_t)exc_vec);
        user_exc_log++;
    }
}

static void intr_hook(uc_engine *uc, uint32_t intno, void *user_data)
{
    machine_t *m = user_data;

    /* Deferred redirect from ERET_KERNEL_TLB_OK/PGFAULT: the native ERET
     * read a stale CP0_EPC we can't write, landing at the wrong PC.
     * Override PC and return before any state-corrupting handlers fire. */
    if (m->pending_eret_redirect) {
        static uint32_t eret_redir_log = 0;
        if (eret_redir_log < 64) {
            fprintf(stderr,
                    "[ERET_REDIRECT_INTR] intno=%u redir_pc=0x%08" PRIX64 "\n",
                    intno, (uint64_t)(uint32_t)m->pending_eret_redirect_pc);
            eret_redir_log++;
        }
        uc_reg_write(uc, UC_MIPS_REG_PC, &m->pending_eret_redirect_pc);
        uc_emu_stop(uc);
        return;
    }

    static uint32_t intr_log_count[64];
    static uint32_t syscall_entry_log_count = 0;
    static uint32_t intr_log_count_27_detail = 0;
    static uint32_t execve_args_log = 0;

    uint64_t pc = 0, status = 0;
    uc_reg_read(uc, UC_MIPS_REG_PC,         &pc);
    uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &status);

    if (handle_null_call_interrupt(m, uc, intno, pc, status))
        return;

    /*
     * WinCE boot mode: skip all Linux-specific exception intercepts.
     * Let Unicorn's MIPS CPU handle exceptions natively (TLB miss,
     * syscall, etc.).  Only log the first few for diagnostics.
     */
    if (handle_wince_interrupt_passthrough(m, intno, pc, status)) {
        return;
    }

    /*
     * Post-execve stale callback quarantine:
     * keep the handoff FSM deterministic (ARMED -> USER_FETCH_SEEN -> DONE),
     * and in DONE only quarantine intno 12/26 at known stale PCs.
     */
    uint32_t pc32 = (uint32_t)pc;
    uint32_t handoff_pc32 = (uint32_t)m->execve_user_handoff_pc;
    uint8_t handoff_state = m->execve_user_handoff_state;
    bool handoff_active_state =
        (m->execve_user_handoff_active ||
         handoff_state == EXECVE_HANDOFF_STATE_USER_FETCH_SEEN ||
         handoff_state == EXECVE_HANDOFF_STATE_DONE);
    bool stale_pc_hit = is_handoff_stale_pc(pc32, handoff_pc32);
    bool stale_intno_ok =
        (handoff_state == EXECVE_HANDOFF_STATE_DONE)
            ? (intno == 12u || intno == 26u)
            : (intno == 12u || intno == 26u || intno == 27u);
    bool handoff_stale_clearable =
        (m->pending_excode == 0u ||
         m->pending_excode == MIPS_EXCCODE_SYS ||
         m->pending_excode == 1u);
    if (stale_pc_hit &&
        stale_intno_ok &&
        handoff_pc32 != 0u &&
        handoff_active_state &&
        handoff_stale_clearable) {
        if (handoff_state == EXECVE_HANDOFF_STATE_DONE &&
            intno == 12u &&
            handoff_pc32 < 0x80000000u) {
            tlb_lookup_result_t handoff_lookup = shadow_tlb_lookup(m, handoff_pc32);
            if (!handoff_lookup.confident) {
                uint32_t asid = (uint32_t)(m->shadow_cp0_entryhi_live_valid
                                 ? m->shadow_cp0_entryhi_live
                                 : m->shadow_cp0_entryhi) & 0xFFu;
                uint32_t ctx_base = (uint32_t)m->shadow_cp0_context & 0xFF800000u;
                uint32_t ctx_badvpn2 = ((handoff_pc32 >> 9) & 0x007FFFF0u);
                m->shadow_cp0_badvaddr = (uint64_t)handoff_pc32;
                m->shadow_cp0_entryhi = (uint64_t)((handoff_pc32 & 0xFFFFE000u) | asid);
                m->shadow_cp0_context = (uint64_t)(ctx_base | ctx_badvpn2);

                m->pending_epc          = handoff_pc32;
                m->pending_excode       = MIPS_EXCCODE_TLBL;
                m->pending_cause        = (uint32_t)(MIPS_EXCCODE_TLBL << 2);
                m->epc_was_written      = false;
                m->pending_cause_served = false;
                m->pending_epc_served   = false;

                uint64_t ex_status = status | 0x2u;  /* EXL=1 */
                uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &ex_status);
                uint64_t vec = (ex_status & 0x00400000u) ?
                               mips_sext(0xBFC00380u) :
                               mips_sext(0x80000180u);
                uc_reg_write(uc, UC_MIPS_REG_PC, &vec);

                static uint32_t handoff_itlbl_inject_log = 0;
                if (handoff_itlbl_inject_log < 96) {
                    fprintf(stderr,
                            "[EXECVE_HANDOFF_ITLBL_INJECT] intno=%u stale_pc=0x%08" PRIX64
                            " user_pc=0x%08X reason=%s lo0=0x%08X lo1=0x%08X"
                            " entryhi=0x%08X vec=0x%08" PRIX64 " status=0x%08" PRIX64 "\n",
                            intno,
                            (uint64_t)(uint32_t)pc,
                            handoff_pc32,
                            handoff_lookup.reason ? handoff_lookup.reason : "unknown",
                            handoff_lookup.lo0,
                            handoff_lookup.lo1,
                            handoff_lookup.entryhi,
                            (uint64_t)(uint32_t)vec,
                            (uint64_t)(uint32_t)ex_status);
                    handoff_itlbl_inject_log++;
                }
                return;
            }
        }

        uint64_t restore_pc = (uint64_t)handoff_pc32;
        uint64_t user_status = ((uint64_t)status & ~(uint64_t)0x1Au) | 0x10u;
        user_status &= ~(uint64_t)0xFF01u; /* IM[7:0]=0, IE=0 */
        uc_reg_write(uc, UC_MIPS_REG_PC, &restore_pc);
        uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &user_status);

        m->pending_epc = 0;
        m->pending_excode = 0;
        m->pending_cause = 0;
        m->epc_was_written = false;
        m->pending_cause_served = false;
        m->pending_epc_served = false;
        m->has_saved_exception = false;

        if (handoff_state == EXECVE_HANDOFF_STATE_USER_FETCH_SEEN) {
            m->execve_user_handoff_state = EXECVE_HANDOFF_STATE_DONE;
            m->execve_user_handoff_active = false;
            m->execve_user_handoff_done_keep_count = 0;

            /*
             * Disable the handoff quarantine: clear handoff_pc so
             * handoff_irq_quarantine_active() returns false.
             *
             * The quarantine was needed during the initial kernel→user
             * transition to catch stale intno=26 at the kernel return PC.
             * Now that user code is executing (USER_FETCH_SEEN), further
             * TLB misses during user execution must go through the normal
             * kernel exception handler (0x80000000/0x80000180) so the
             * kernel can demand-page new code/data pages from the ramdisk.
             *
             * Without this, the quarantine in DONE state intercepts ALL
             * intno=26 events (via the loop_sig check in
             * handoff_irq_quarantine_active) and handles them via load
             * emulation instead of the kernel's TLB handler — preventing
             * new pages from being allocated.
             */
            m->execve_user_handoff_pc = 0;
            static uint32_t handoff_done_log = 0;
            if (handoff_done_log < 4) {
                fprintf(stderr,
                        "[EXECVE_HANDOFF_DONE] user fetches established,"
                        " quarantine disabled\n");
                handoff_done_log++;
            }
        } else if (handoff_state == EXECVE_HANDOFF_STATE_DONE) {
            m->execve_user_handoff_done_keep_count++;
            bool entry_probe_valid = trace_user_handoff_entry_probe(
                m, "DONE_STALE", (uint32_t)restore_pc, (uint32_t)restore_pc, pc, false);
            if (entry_probe_valid && in_user_handoff_entry_window((uint32_t)restore_pc)) {
                bool emu_progress = emulate_load_at_pc(m, restore_pc);
                if (emu_progress) {
                    uint64_t emu_pc = restore_pc;
                    uc_reg_read(uc, UC_MIPS_REG_PC, &emu_pc);
                    if ((uint32_t)emu_pc < 0x80000000u)
                        m->execve_user_handoff_pc = (uint32_t)emu_pc;
                    static uint32_t handoff_load_emu_log = 0;
                    if (handoff_load_emu_log < 128) {
                        fprintf(stderr,
                                "[EXECVE_USER_HANDOFF_DONE_LOAD_EMU] intno=%u stale_pc=0x%08" PRIX64
                                " emu_pc=0x%08" PRIX64 "\n",
                                intno,
                                (uint64_t)(uint32_t)pc,
                                (uint64_t)(uint32_t)emu_pc);
                        handoff_load_emu_log++;
                    }
                } else {
                    uint32_t handoff_insn = 0xFFFFFFFFu;
                    if (read_insn_best_effort(uc, restore_pc, &handoff_insn) &&
                        handoff_insn == 0u) {
                        uint64_t next_pc = restore_pc + 4u;
                        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
                        if ((uint32_t)next_pc < 0x80000000u)
                            m->execve_user_handoff_pc = (uint32_t)next_pc;
                        static uint32_t handoff_skip_nop_log = 0;
                        if (handoff_skip_nop_log < 64) {
                            fprintf(stderr,
                                    "[EXECVE_USER_HANDOFF_DONE_SKIP_NOP] intno=%u"
                                    " stale_pc=0x%08" PRIX64 " pc=0x%08" PRIX64
                                    " -> next=0x%08" PRIX64 "\n",
                                    intno,
                                    (uint64_t)(uint32_t)pc,
                                    (uint64_t)(uint32_t)restore_pc,
                                    (uint64_t)(uint32_t)next_pc);
                            handoff_skip_nop_log++;
                        }
                    } else if (intno == 12u && handoff_insn != 0xFFFFFFFFu &&
                               handoff_insn != 0u) {
                        /*
                         * Real instruction that emulate_load_at_pc can't handle
                         * and isn't a zero-word gap.  Route to the kernel
                         * exception handler — this is the same intno=12 event
                         * that the normal user-exception path handles; the
                         * quarantine was just swallowing it.
                         */
                        static uint32_t stuck_log = 0;
                        if (stuck_log < 16) {
                            fprintf(stderr,
                                    "[HANDOFF_STUCK_INSN] pc=0x%08X insn=0x%08X op=%u\n",
                                    (uint32_t)restore_pc, handoff_insn,
                                    handoff_insn >> 26);
                            stuck_log++;
                        }

                        /* Re-read status since we overwrote it with user_status above */
                        uint64_t cur_status;
                        uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &cur_status);
                        route_user_exception_to_kernel(m, uc, intno,
                                                      restore_pc, cur_status);

                        /* Disable handoff FSM — we're done */
                        m->execve_user_handoff_state = 0;
                        m->execve_user_handoff_active = false;
                        return;
                    }
                }
            }
        }

        static uint32_t handoff_stale_log = 0;
        if (handoff_stale_log < 192) {
            fprintf(stderr,
                    "[EXECVE_USER_HANDOFF_QUARANTINE] intno=%u stale_pc=0x%08" PRIX64
                    " -> user_pc=0x%08" PRIX64 " state=%u excode=%u done_keep=%u\n",
                    intno, (uint64_t)(uint32_t)pc,
                    (uint64_t)(uint32_t)restore_pc,
                    (unsigned)handoff_state,
                    m->pending_excode,
                    m->execve_user_handoff_done_keep_count);
            handoff_stale_log++;
        }
        if (handoff_state == EXECVE_HANDOFF_STATE_DONE &&
            intno == 12u &&
            m->execve_user_handoff_done_keep_count >= 4u &&
            (m->execve_user_handoff_done_keep_count & 0xFu) == 0u) {
            static uint32_t handoff_stale_restart_log = 0;
            if (handoff_stale_restart_log < 64) {
                fprintf(stderr,
                        "[EXECVE_USER_HANDOFF_STALE_RESTART] stale_pc=0x%08" PRIX64
                        " user_pc=0x%08" PRIX64 " done_keep=%u\n",
                        (uint64_t)(uint32_t)pc,
                        (uint64_t)(uint32_t)restore_pc,
                        m->execve_user_handoff_done_keep_count);
                handoff_stale_restart_log++;
            }
            uc_emu_stop(uc);
        }
        return;
    }

    /*
     * User-mode exception at a kernel-mode PC (UM=1, PC >= 0x80000000).
     * This is an impossible state on real MIPS — user mode can't access
     * kseg0/kseg1.  It occurs when a native ERET reads stale CP0_EPC
     * (which we can't write) and jumps to a kernel address while in user
     * mode.  Redirect PC back to the last known user instruction.
     */
    if ((uint32_t)pc >= 0x80000000u && (status & 0x10u) && !(status & 0x2u)) {
        /* UM=1, EXL=0, PC in kseg — user-at-kernel address error */
        uint32_t last_user = (uint32_t)m->last_exec_pc;
        if (last_user < 0x80000000u && last_user != 0u) {
            /* Redirect to the last user PC as a TLBL so the kernel
             * re-executes the faulting instruction correctly. */
            route_user_exception_to_kernel(m, uc, 26u, (uint64_t)last_user, status);
            static uint32_t user_at_kernel_log = 0;
            if (user_at_kernel_log < 64) {
                fprintf(stderr,
                        "[USER_AT_KERNEL_PC] intno=%u pc=0x%08X status=0x%08"
                        PRIX64 " -> redirect to last_user=0x%08X\n",
                        intno, (uint32_t)pc, status, last_user);
                user_at_kernel_log++;
            }
            return;
        }
    }

    /*
     * intno=12 (RI) at a kernel-mode PC while pending_eret_redirect is active.
     *
     * Root cause: ERET_KERNEL_TLB_OK NOP-patches the ERET instruction and
     * calls uc_emu_stop, but Unicorn's TCG has already decoded the ERET.
     * The native ERET executes before uc_emu_stop takes effect, reading a
     * stale CP0_EPC (e.g. SYSCALL+4 from the original exception entry).
     * Unicorn lands at the wrong PC and raises intno=12 for the instruction
     * there.  The uc_emu_stop then fires, ending the batch.  The main loop
     * applies pending_eret_redirect to correct the PC.
     *
     * Fix: just return without modifying any synthetic state.  The batch
     * will end (uc_emu_stop is already pending), and pending_eret_redirect
     * will correct the PC for the next batch.
     */
    if ((uint32_t)pc >= 0x80000000u && intno == 12u &&
        m->pending_eret_redirect) {
        static uint32_t kernel_ri_eret_redir_log = 0;
        if (kernel_ri_eret_redir_log < 64) {
            fprintf(stderr,
                    "[KERNEL_RI_ERET_REDIR] intno=%u pc=0x%08X"
                    " redir_pc=0x%08" PRIX64 " pend_exc=%u"
                    " last_exec_pc=0x%08X — suppressing (uc_emu_stop pending)\n",
                    intno, (uint32_t)pc,
                    (uint64_t)(uint32_t)m->pending_eret_redirect_pc,
                    m->pending_excode,
                    (uint32_t)m->last_exec_pc);
            kernel_ri_eret_redir_log++;
        }
        return;  /* uc_emu_stop will end batch; main loop applies redirect */
    }

    /*
     * intno=12 (RI) at a kernel-mode PC during a pending synthetic exception
     * WITHOUT pending_eret_redirect.  This happens when the native ERET
     * executes (from a stale decoded TB) and lands at a stale CP0_EPC,
     * but the batch didn't go through ERET_KERNEL_TLB_OK's uc_emu_stop path.
     *
     * The registers at this point reflect mid-do_execve state (ra/sp/v0
     * are inside the kernel), not the syscall return context.  The instruction
     * at the reported PC is valid (e.g. beqz) — the RI is spurious.
     *
     * Fix: force PC back to the faulting instruction (last_exec_pc + 4,
     * i.e. the instruction after the NOP'd ERET) or stop the batch so the
     * main loop can sort things out.  For now, just suppress and stop.
     */
    if ((uint32_t)pc >= 0x80000000u && intno == 12u &&
        m->pending_excode != 0) {
        uint32_t ri_insn_at_last = 0;
        uc_mem_read(uc, (uint32_t)m->last_exec_pc, &ri_insn_at_last, 4);
        static uint32_t kernel_ri_spurious_log = 0;
        if (kernel_ri_spurious_log < 64) {
            uint64_t ri_ra = 0, ri_sp = 0, ri_v0 = 0;
            uc_reg_read(uc, UC_MIPS_REG_RA, &ri_ra);
            uc_reg_read(uc, UC_MIPS_REG_SP, &ri_sp);
            uc_reg_read(uc, UC_MIPS_REG_V0, &ri_v0);
            fprintf(stderr,
                    "[KERNEL_RI_SPURIOUS] intno=%u pc=0x%08X pend_exc=%u"
                    " pend_epc=0x%08X last_exec=0x%08X insn@last=0x%08X"
                    " ra=0x%08X sp=0x%08X v0=0x%08X eret_redir=%u"
                    " nop_patch_pending=%u nop_patch_addr=0x%08X\n",
                    intno, (uint32_t)pc,
                    m->pending_excode,
                    (uint32_t)m->pending_epc,
                    (uint32_t)m->last_exec_pc,
                    ri_insn_at_last,
                    (uint32_t)ri_ra, (uint32_t)ri_sp, (uint32_t)ri_v0,
                    m->pending_eret_redirect ? 1u : 0u,
                    m->eret_nop_patch_pending ? 1u : 0u,
                    (uint32_t)m->eret_nop_patch_addr);
            kernel_ri_spurious_log++;
        }
        /* If nop_patch_pending is true, this RI is a deferred exception from
         * a native ERET that executed despite the NOP-patch (already-decoded TB).
         *
         * The RI persists across batches because Unicorn's internal exception
         * state is not cleared by PC writes alone.  To break the cycle:
         *  1. Set EXL=1 to suppress further exception delivery.
         *  2. Redirect PC to the correct fault address.
         *  3. Set pending_eret_redirect so the main loop applies the
         *     redirect AFTER this batch (which uc_emu_stop will end).
         *  4. A flag (eret_ri_exl_fixup) tells the next prid_hook to
         *     clear EXL once execution resumes at the correct address. */
        if (m->eret_nop_patch_pending) {
            uint64_t redir = m->pending_eret_redirect_pc;
            uc_reg_write(uc, UC_MIPS_REG_PC, &redir);

            /* Set EXL=1 to suppress exception re-delivery */
            uint64_t ri_status = 0;
            uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &ri_status);
            ri_status |= 0x2u;  /* set EXL */
            uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &ri_status);

            /* Re-arm the redirect for the main loop */
            m->pending_eret_redirect = true;
            /* pending_eret_redirect_pc still has the correct value */

            m->eret_ri_exl_fixup = true;
        }
        uc_emu_stop(uc);
        return;
    }

    /*
     * User-space exception at a kuseg PC: route to kernel exception handler.
     * intno=12: RI (Reserved Instruction) or CpU (Coprocessor Unusable).
     * intno=26: TLBL (TLB load miss) — user data load from unmapped page.
     * intno=27: TLBS (TLB store miss) — user data store to unmapped page.
     * Set EPC = current PC, EXL = 1, redirect to general exception vector.
     */
    if ((uint32_t)pc < 0x80000000u &&
        (intno == 12u || intno == 26u || intno == 27u)) {
        route_user_exception_to_kernel(m, uc, intno, pc, status);
        return;
    }

    /*
     * Unicorn 2.1.4 MIPS fires intno=17 for the SYSCALL instruction.
     * When the hook fires, Unicorn has already advanced PC to SYSCALL+4;
     * the real EPC (address of the syscall instruction) is PC-4.
     *
     * Log early interrupt events during development; only act when the event
     * looks like a SYSCALL (intno 17 on Unicorn 2.1.x, intno 26 on some
     * older Unicorn builds).
     */
    bool likely_syscall = false;
    if (intno == 17u) {
        uint32_t maybe_sys = 0;
        if (pc >= 4u && read_insn_best_effort(uc, pc - 4u, &maybe_sys) &&
            maybe_sys == 0x0000000Cu)
            likely_syscall = true;
    }

    if (!likely_syscall) {
        uint32_t idx = intno < 64u ? intno : 63u;
        if (intr_log_count[idx] < 8u) {
            fprintf(stderr, "[INTR] intno=%u PC=0x%016" PRIX64
                            " STATUS=0x%016" PRIX64 "\n", intno, pc, status);
            intr_log_count[idx]++;
            if (intr_log_count[idx] == 8u)
                fprintf(stderr, "[INTR] intno=%u further logs suppressed\n", intno);
        }
        if (intno == 27u && intr_log_count_27_detail < 32u) {
            if (!m->tlb_trace_window && is_run_init_syscall_ret_pc((uint32_t)pc)) {
                m->tlb_trace_window = true;
                fprintf(stderr,
                        "[TLB_TRACE] activated by first TLBS at PC=0x%08" PRIX64 "\n",
                        (uint64_t)(uint32_t)pc);
            }
            fprintf(stderr,
                    "[INTR27] STATUS=0x%08" PRIX64 " PC=0x%08" PRIX64
                    " pending_excode=%u pending_epc=0x%08" PRIX64
                    " pending_cause=0x%08X epc_written=%u served_epc=%u served_cause=%u\n",
                    status, (uint64_t)(uint32_t)pc,
                    m->pending_excode, (uint64_t)(uint32_t)m->pending_epc,
                    m->pending_cause, m->epc_was_written ? 1u : 0u,
                    m->pending_epc_served ? 1u : 0u,
                    m->pending_cause_served ? 1u : 0u);
            intr_log_count_27_detail++;
        }
        if (intno == 26u || intno == 27u) {
            /*
             * Log BadVAddr (CP0 r8) and EntryHi for intno=26/27 at SYSCALL+4
             * to determine whether the fault is an instruction-fetch miss on
             * the SYSCALL itself or a data miss during early do_execve.
             */
            static uint32_t tlb_badvaddr_log = 0;
            bool focus_fault =
                is_run_init_syscall_ret_pc((uint32_t)pc) ||
                ((uint32_t)pc == 0x80000000u) ||
                ((uint32_t)pc == 0x80000180u);
            if (focus_fault && tlb_badvaddr_log < 96u) {
                /* BadVAddr not accessible via Unicorn API; use shadow + last_unmapped. */
                uint64_t k0 = 0, k1 = 0;
                uc_reg_read(uc, UC_MIPS_REG_K0, &k0);
                uc_reg_read(uc, UC_MIPS_REG_K1, &k1);
                fprintf(stderr,
                        "[TLB_FAULT] intno=%u PC=0x%08" PRIX64
                        " shadow_badvaddr=0x%08" PRIX64 " last_unmapped=0x%08" PRIX64
                        " shadow_entryhi=0x%08" PRIX64
                        " k0=0x%08" PRIX64 " k1=0x%08" PRIX64
                        " STATUS=0x%08" PRIX64 " pending_excode=%u\n",
                        intno, (uint64_t)(uint32_t)pc,
                        (uint64_t)(uint32_t)m->shadow_cp0_badvaddr,
                        m->last_unmapped_valid ? (uint64_t)(uint32_t)m->last_unmapped_addr : 0xDEADu,
                        (uint64_t)(uint32_t)m->shadow_cp0_entryhi,
                        (uint64_t)(uint32_t)k0, (uint64_t)(uint32_t)k1,
                        status, m->pending_excode);
                tlb_badvaddr_log++;
            }
            /*
             * Guard against misclassifying a syscall-site interrupt as nested
             * TLB traffic. We have observed intno=26 at PC=syscall+4 with
             * (PC-4)==SYSCALL encoding; clearing synthetic syscall state there
             * corrupts return-path bookkeeping and can crash at 0x80000180.
             */
            uint32_t prev_insn = 0;
            bool at_syscall_site = (pc >= 4u &&
                                    read_insn_best_effort(uc, pc - 4u, &prev_insn) &&
                                    prev_insn == 0x0000000Cu);
            static uint32_t tlb_nested_keep_log = 0;
            static uint32_t tlb_nested_defer_log = 0;
            if (at_syscall_site && m->pending_excode == MIPS_EXCCODE_SYS) {
                /*
                 * Some Unicorn builds surface intno=26/27 at SYSCALL+4 with EXL=0.
                 * This can fire during do_execve (the beqz-a3 at SYSCALL+4 is a TLB
                 * notification target) or after the syscall has truly returned.
                 *
                 * Retire if EITHER of the following holds:
                 *  (a) pc == pending_epc+4   — pending_epc tracks the syscall EPC and
                 *      is mutated to the kuseg user entry by MTC0 EPC (start_thread).
                 *      Before MTC0: pending_epc == SYSCALL VA, so pending_epc+4 matches
                 *      SYSCALL+4 exactly — correct retirement.
                 *      After MTC0:  pending_epc == kuseg entry, so pending_epc+4 is a
                 *      kuseg address that won't match the kseg SYSCALL+4 PC — DEFER
                 *      stays active until the real ERET fires.
                 *  (b) epc_was_written==true  — start_thread() wrote the user entry via
                 *      MTC0 EPC; any EXL=0 SYSCALL+4 event at that point means the
                 *      syscall body has completed.  (pending_epc is kuseg here so (a)
                 *      would be false; this is belt-and-suspenders for corner cases.)
                 *
                 * Do NOT use pending_syscall_epc+4 here: that would fire prematurely
                 * while do_execve is still running (in-flight TLB miss at SYSCALL+4).
                 */
                if ((status & 0x2u) == 0u) {
                    /*
                     * at_ret_site: we are at the instruction immediately after the
                     * SYSCALL that started this exception.  pending_epc may have been
                     * mutated by the MTC0 EPC intercept to the new user-space entry
                     * point; pending_syscall_epc holds the original (frozen) SYSCALL VA.
                     *
                     * IMPORTANT: only use pending_epc (not pending_syscall_epc) for the
                     * at_ret_site test.  When MTC0 EPC has NOT fired yet (epc_was_written
                     * =false), pending_epc == pending_syscall_epc, so the tests are
                     * equivalent.  When MTC0 HAS fired, pending_epc is the user entry
                     * point (kuseg), so pending_epc+4 is a kuseg address that will NOT
                     * match the kseg SYSCALL+4 PC — keeping DEFER active until the real
                     * ERET fires.  Using pending_syscall_epc+4 here would trigger a
                     * premature retirement while do_execve is still running (observed:
                     * the beqz-a3 at SYSCALL+4 catches an in-progress TLB notification).
                     */
                    bool at_ret_site = ((uint32_t)pc == (uint32_t)m->pending_epc + 4u);
                    bool syscall_is_execve = (m->pending_syscall_nr == 4011u);
                    /*
                     * stale_post_mtc0: kernel has called start_thread() (wrote user
                     * entry via MTC0 EPC) so any EXL=0 SYSCALL+4 event is definitively
                     * after the syscall completed.  Belt-and-suspenders: pending_epc is
                     * already a kuseg address at this point so at_ret_site would be false
                     * anyway; this catches corner cases where the two addresses coincide.
                     */
                    bool stale_post_mtc0 = m->epc_was_written;
                    /*
                     * Do NOT retire while do_execve is in flight.
                     *
                     * Unicorn sometimes fires intno=26/27 at SYSCALL+4 very early in
                     * do_execve execution (before the function has returned).  Without
                     * this guard, at_ret_site would be TRUE (pending_epc==SYSCALL VA,
                     * pending_epc+4==SYSCALL+4==PC) and we'd prematurely clear the
                     * SYSCALL exception state.  The result is that init() sees a
                     * spurious v0=0 / a3=<garbage> return and continues to the next
                     * execve path, eventually hitting "No init found." panic.
                     *
                     * While execve_watch_active is set, defer this retirement.
                     * Also defer for syscall nr=4011 even if watch is inactive:
                     * some kernels don't hit our do_execve watch hook address, and
                     * retiring on at_ret_site alone causes false success/failure data.
                     *
                     * In deferred mode:
                     *   - Native TLB handling continues (we return without clearing).
                     *   - The nested TLB handler's ERET is also guarded (see above)
                     *     so it passes through natively and returns to the faulting
                     *     instruction inside do_execve rather than to SYSCALL+4.
                     *   - stale_post_mtc0 still fires correctly once start_thread or
                     *     restore_all writes EPC via MTC0.
                     */
                    bool retire_at_ret_site =
                        (at_ret_site && !m->execve_watch_active && !syscall_is_execve);
                    if (retire_at_ret_site || stale_post_mtc0) {
                        static uint32_t tlb_nested_retire_log = 0;
                        static uint32_t syscall_ret_fallback_log = 0;
                        uint64_t v0 = 0, a3 = 0;
                        uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
                        uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
                        if (tlb_nested_retire_log < 64) {
                            fprintf(stderr,
                                    "[TLB_NESTED_RETIRE] intno=%u PC=0x%08" PRIX64
                                    " STATUS=0x%08" PRIX64
                                    " syscall_epc=0x%08" PRIX64 " pending_epc=0x%08" PRIX64
                                    " at_ret=%u post_mtc0=%u"
                                    " v0=0x%08" PRIX64 " a3=0x%08" PRIX64 "\n",
                                    intno, (uint64_t)(uint32_t)pc, status,
                                    (uint64_t)(uint32_t)m->pending_syscall_epc,
                                    (uint64_t)(uint32_t)m->pending_epc,
                                    at_ret_site ? 1u : 0u, stale_post_mtc0 ? 1u : 0u,
                                    (uint64_t)(uint32_t)v0,
                                    (uint64_t)(uint32_t)a3);
                            tlb_nested_retire_log++;
                        }
                        if (syscall_ret_fallback_log < 64) {
                            fprintf(stderr,
                                    "[SYSCALL_RET_FALLBACK] syscall_epc=0x%08" PRIX64
                                    " pending_epc=0x%08" PRIX64
                                    " pc=0x%08" PRIX64 " nr=%u"
                                    " a0=0x%08" PRIX64 " \"%s\""
                                    " v0=0x%08" PRIX64 " a3=0x%08" PRIX64
                                    " status=0x%08" PRIX64
                                    " at_ret=%u post_mtc0=%u\n",
                                    (uint64_t)(uint32_t)m->pending_syscall_epc,
                                    (uint64_t)(uint32_t)m->pending_epc,
                                    (uint64_t)(uint32_t)pc,
                                    m->pending_syscall_nr,
                                    (uint64_t)(uint32_t)m->pending_syscall_a0,
                                    m->pending_syscall_a0_str,
                                    (uint64_t)(uint32_t)v0,
                                    (uint64_t)(uint32_t)a3,
                                    status,
                                    at_ret_site ? 1u : 0u, stale_post_mtc0 ? 1u : 0u);
                            syscall_ret_fallback_log++;
                        }
                        clear_synthetic_syscall_state(m, true);
                        m->has_saved_exception  = false;
                        return;
                    }
                    if (syscall_is_execve && at_ret_site &&
                        !m->execve_watch_active && !stale_post_mtc0) {
                        /*
                         * Post-do_execve intno=26/27 at SYSCALL+4 is typically a
                         * stale notification. Re-entering 0x80000180 here
                         * re-runs the syscall decoder with garbage v0 and
                         * corrupts init() fallback registers (a1/a2).
                         *
                         * Ignore this edge notification and keep the current
                         * synthetic syscall bookkeeping intact so the real
                         * return path can retire normally.
                         */
                        uint64_t v0 = 0, a3 = 0;
                        uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
                        uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
                        bool execve_handoff_ctx =
                            ((uint32_t)v0 >= 0x70000000u && (uint32_t)v0 < 0x80000000u &&
                             (uint32_t)a3 >= 0x00010000u && (uint32_t)a3 < 0x80000000u);
                        if (intno == 27u && execve_handoff_ctx) {
                            static uint32_t execve_ctx_defer_log = 0;
                            if (execve_ctx_defer_log < 64) {
                                fprintf(stderr,
                                        "[EXECVE_CTX_AT_RET] intno=%u pc=0x%08" PRIX64
                                        " syscall_epc=0x%08" PRIX64
                                        " pending_epc=0x%08" PRIX64
                                        " v0=0x%08" PRIX64 " a3=0x%08" PRIX64
                                        " action=defer_tlb_entry\n",
                                        intno, (uint64_t)(uint32_t)pc,
                                        (uint64_t)(uint32_t)m->pending_syscall_epc,
                                        (uint64_t)(uint32_t)m->pending_epc,
                                        (uint64_t)(uint32_t)v0,
                                        (uint64_t)(uint32_t)a3);
                                execve_ctx_defer_log++;
                            }
                        } else if (intno == 26u || intno == 27u) {
                            static uint32_t tlb_nested_drop_stale_log = 0;
                            if (tlb_nested_drop_stale_log < 128) {
                                fprintf(stderr,
                                        "[TLB_NESTED_DROP_KEEP] intno=%u PC=0x%08" PRIX64
                                        " STATUS=0x%08" PRIX64
                                        " syscall_epc=0x%08" PRIX64
                                        " pending_epc=0x%08" PRIX64
                                        " v0=0x%08" PRIX64 " a3=0x%08" PRIX64
                                        " epc_written=%u\n",
                                        intno, (uint64_t)(uint32_t)pc, status,
                                        (uint64_t)(uint32_t)m->pending_syscall_epc,
                                        (uint64_t)(uint32_t)m->pending_epc,
                                        (uint64_t)(uint32_t)v0,
                                        (uint64_t)(uint32_t)a3,
                                        m->epc_was_written ? 1u : 0u);
                                tlb_nested_drop_stale_log++;
                            }
                            return;
                        }
                    }
                    uint32_t syscall_epc = (uint32_t)pc - 4u;
                    if (!m->tlb_defer_active || m->tlb_defer_owner_epc != syscall_epc) {
                        static uint32_t defer_owner_reset_log = 0;
                        if (m->tlb_defer_active && defer_owner_reset_log < 32) {
                            fprintf(stderr,
                                    "[TLB_DEFER_OWNER_RESET] old_epc=0x%08X new_epc=0x%08X"
                                    " count=%u\n",
                                    m->tlb_defer_owner_epc, syscall_epc, m->tlb_defer_count);
                            defer_owner_reset_log++;
                        }
                        m->tlb_defer_active = true;
                        m->tlb_defer_owner_epc = syscall_epc;
                        m->tlb_defer_count = 0;
                    }
                    /*
                     * After the first explicit DEFER_ENTRY, subsequent intno=26/27
                     * notifications at the same SYSCALL+4 site are often stale
                     * re-deliveries while do_execve is still running. Re-entering
                     * the refill vector repeatedly here can livelock execve.
                     */
                    if (m->tlb_defer_count > 0 &&
                        m->execve_watch_active &&
                        m->tlb_defer_owner_epc == syscall_epc) {
                        static uint32_t tlb_defer_drop_log = 0;
                        static uint32_t tlb_defer_retry_log = 0;

                        /* When kuseg gaps are unmapped, inject TLBS/TLBL for
                         * real TLB misses instead of emulating or skipping. */
                        if (m->kuseg_gaps_unmapped) {
                            uint64_t real_pc = m->last_exec_pc;
                            uint32_t target_va = 0;
                            bool injected = false;
                            if (intno == 27u &&
                                decode_store_target_va(m, real_pc, &target_va) &&
                                target_va < 0x80000000u) {
                                tlb_lookup_result_t wr = shadow_tlb_lookup(m, target_va);
                                bool has_writable = wr.hit && wr.confident &&
                                    ((((((uint64_t)target_va & wr.page_bytes) != 0u)
                                       ? wr.lo1 : wr.lo0) & 0x4u) != 0u);
                                if (!has_writable) {
                                    injected = inject_kernel_kuseg_tlbs(m, real_pc, target_va);
                                } else {
                                    shadow_tlb_populate(m, target_va, true,
                                                        "DEFER2_STORE_POP",
                                                        (uint32_t)real_pc);
                                    uc_reg_write(uc, UC_MIPS_REG_PC, &real_pc);
                                    injected = true;
                                }
                            } else if (intno == 26u &&
                                       decode_load_source_va(m, real_pc, &target_va) &&
                                       target_va < 0x80000000u) {
                                tlb_lookup_result_t rd = shadow_tlb_lookup(m, target_va);
                                if (rd.hit && rd.confident) {
                                    shadow_tlb_populate(m, target_va, true,
                                                        "DEFER2_LOAD_POP",
                                                        (uint32_t)real_pc);
                                    uc_reg_write(uc, UC_MIPS_REG_PC, &real_pc);
                                    injected = true;
                                } else {
                                    injected = inject_kernel_kuseg_tlbl(m, real_pc, target_va);
                                }
                            }
                            if (injected) {
                                static uint32_t defer2_inject_log = 0;
                                if (defer2_inject_log < 256) {
                                    fprintf(stderr,
                                            "[TLB_DEFER2_INJECT] intno=%u"
                                            " real_pc=0x%08" PRIX64
                                            " target_va=0x%08X count=%u\n",
                                            intno, (uint64_t)(uint32_t)real_pc,
                                            target_va, m->tlb_defer_count);
                                    defer2_inject_log++;
                                }
                                return;
                            }
                        }

                        if (intno == 26u) {
                            uint64_t real_pc = m->last_exec_pc;
                            if (emulate_load_at_pc(m, real_pc)) {
                                if (tlb_defer_drop_log < 64) {
                                    fprintf(stderr,
                                            "[TLB_DEFER_LOAD_EMU] intno=%u"
                                            " notif_pc=0x%08" PRIX64
                                            " real_pc=0x%08" PRIX64
                                            " owner_epc=0x%08X count=%u\n",
                                            intno, (uint64_t)(uint32_t)pc,
                                            (uint64_t)(uint32_t)real_pc,
                                            m->tlb_defer_owner_epc,
                                            m->tlb_defer_count);
                                    tlb_defer_drop_log++;
                                }
                                return;
                            }
                            uc_reg_write(uc, UC_MIPS_REG_PC, &real_pc);
                            if (tlb_defer_drop_log < 64) {
                                fprintf(stderr,
                                        "[TLB_DEFER_DROP] intno=%u PC=0x%08" PRIX64
                                        " real_pc=0x%08" PRIX64
                                        " owner_epc=0x%08X retry=%u\n",
                                        intno, (uint64_t)(uint32_t)pc,
                                        (uint64_t)(uint32_t)real_pc,
                                        m->tlb_defer_owner_epc,
                                        m->tlb_defer_count);
                                tlb_defer_drop_log++;
                            }
                            return;
                        }
                        if (tlb_defer_retry_log < 64) {
                            uint64_t v0 = 0, a3 = 0;
                            uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
                            uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
                            fprintf(stderr,
                                    "[TLB_DEFER_RETRY] intno=%u PC=0x%08" PRIX64
                                    " owner_epc=0x%08X retry=%u"
                                    " v0=0x%08" PRIX64 " a3=0x%08" PRIX64 "\n",
                                    intno, (uint64_t)(uint32_t)pc,
                                    m->tlb_defer_owner_epc, m->tlb_defer_count,
                                    (uint64_t)(uint32_t)v0,
                                    (uint64_t)(uint32_t)a3);
                            tlb_defer_retry_log++;
                        }
                    }

                    if (tlb_nested_defer_log < 64) {
                        uint64_t v0 = 0, a3 = 0;
                        uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
                        uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
                        fprintf(stderr,
                                "[TLB_NESTED_DEFER] intno=%u PC=0x%08" PRIX64
                                " STATUS=0x%08" PRIX64
                                " syscall_epc=0x%08" PRIX64 " pending_epc=0x%08" PRIX64
                                " v0=0x%08" PRIX64 " a3=0x%08" PRIX64
                                " execve_active=%u entry=0x%08" PRIX64 "\n",
                                intno, (uint64_t)(uint32_t)pc, status,
                                (uint64_t)(uint32_t)m->pending_syscall_epc,
                                (uint64_t)(uint32_t)m->pending_epc,
                                (uint64_t)(uint32_t)v0, (uint64_t)(uint32_t)a3,
                                m->execve_watch_active ? 1u : 0u,
                                (uint64_t)(uint32_t)m->execve_entry_pc);
                        tlb_nested_defer_log++;
                    }
                    /*
                     * Unicorn fires intno=26/27 as a notification-only event
                     * (EXL=0, PC set to SYSCALL+4) while execution is actually
                     * deep inside do_execve.
                     *
                     * When kuseg gap regions are unmapped, these are REAL TLB
                     * misses: the kernel store/load to a kuseg VA failed because
                     * the flat mapping is gone.  Inject TLBS/TLBL so the kernel's
                     * TLB refill handler runs (page fault → allocate → TLBWI).
                     *
                     * When kuseg is still pre-mapped, these are stale notifications;
                     * restore PC to last_exec_pc and continue.
                     */
                    m->tlb_defer_count++;
                    {
                        uint64_t real_pc = m->last_exec_pc;
                        if (m->kuseg_gaps_unmapped &&
                            (uint32_t)real_pc >= 0x80000000u) {
                            /* Decode the real faulting instruction to get kuseg VA */
                            uint32_t target_va = 0;
                            bool injected = false;
                            if (intno == 27u) {
                                /* Store miss → TLBS */
                                if (decode_store_target_va(m, real_pc, &target_va) &&
                                    target_va < 0x80000000u) {
                                    tlb_lookup_result_t wr = shadow_tlb_lookup(m, target_va);
                                    bool has_writable = false;
                                    if (wr.hit && wr.confident) {
                                        bool odd = (((uint64_t)target_va & wr.page_bytes) != 0u);
                                        uint32_t lo = odd ? wr.lo1 : wr.lo0;
                                        has_writable = ((lo & 0x4u) != 0u);
                                    }
                                    if (!has_writable) {
                                        injected = inject_kernel_kuseg_tlbs(m, real_pc, target_va);
                                    } else {
                                        shadow_tlb_populate(m, target_va, true,
                                                            "DEFER_STORE_POP",
                                                            (uint32_t)real_pc);
                                        uc_reg_write(uc, UC_MIPS_REG_PC, &real_pc);
                                        injected = true;  /* not really injected, but handled */
                                    }
                                }
                            } else {
                                /* Load miss → TLBL */
                                if (decode_load_source_va(m, real_pc, &target_va) &&
                                    target_va < 0x80000000u) {
                                    tlb_lookup_result_t rd = shadow_tlb_lookup(m, target_va);
                                    if (rd.hit && rd.confident) {
                                        shadow_tlb_populate(m, target_va, true,
                                                            "DEFER_LOAD_POP",
                                                            (uint32_t)real_pc);
                                        uc_reg_write(uc, UC_MIPS_REG_PC, &real_pc);
                                        injected = true;
                                    } else {
                                        injected = inject_kernel_kuseg_tlbl(m, real_pc, target_va);
                                    }
                                }
                            }
                            if (injected) {
                                static uint32_t defer_inject_log = 0;
                                if (defer_inject_log < 256) {
                                    fprintf(stderr,
                                            "[TLB_DEFER_INJECT] intno=%u real_pc=0x%08" PRIX64
                                            " target_va=0x%08X count=%u\n",
                                            intno, (uint64_t)(uint32_t)real_pc,
                                            target_va, m->tlb_defer_count);
                                    defer_inject_log++;
                                }
                                return;
                            } else {
                                /* Decode succeeded but injection failed, or decode failed */
                                static uint32_t defer_noinject_log = 0;
                                if (defer_noinject_log < 128) {
                                    uint32_t dbg_insn = 0;
                                    uc_mem_read(uc, real_pc, &dbg_insn, 4);
                                    fprintf(stderr,
                                            "[TLB_DEFER_NOINJECT] intno=%u real_pc=0x%08" PRIX64
                                            " target_va=0x%08X insn=0x%08X"
                                            " kuseg_unmap=%u count=%u\n",
                                            intno, (uint64_t)(uint32_t)real_pc,
                                            target_va, dbg_insn,
                                            m->kuseg_gaps_unmapped ? 1u : 0u,
                                            m->tlb_defer_count);
                                    defer_noinject_log++;
                                }
                            }
                        } else {
                            /* kuseg_gaps_unmapped is false or real_pc is user mode */
                            static uint32_t defer_nogap_log = 0;
                            if (defer_nogap_log < 64) {
                                fprintf(stderr,
                                        "[TLB_DEFER_NOGAP] intno=%u real_pc=0x%08" PRIX64
                                        " kuseg_unmap=%u count=%u\n",
                                        intno, (uint64_t)(uint32_t)real_pc,
                                        m->kuseg_gaps_unmapped ? 1u : 0u,
                                        m->tlb_defer_count);
                                defer_nogap_log++;
                            }
                        }
                        /* Fall back to skip (stale notification or decode failure) */
                        uc_reg_write(uc, UC_MIPS_REG_PC, &real_pc);
                        static uint32_t tlb_defer_skip_log = 0;
                        if (tlb_defer_skip_log < 64) {
                            fprintf(stderr,
                                    "[TLB_DEFER_SKIP] intno=%u notif_pc=0x%08" PRIX64
                                    " real_pc=0x%08" PRIX64
                                    " owner_epc=0x%08X count=%u\n",
                                    intno, (uint64_t)(uint32_t)pc,
                                    (uint64_t)(uint32_t)real_pc,
                                    m->tlb_defer_owner_epc, m->tlb_defer_count);
                            tlb_defer_skip_log++;
                        }
                    }
                    return;
                }
                bool old_cause_served = m->pending_cause_served;
                bool old_epc_served = m->pending_epc_served;
                /*
                 * Nested TLB/aux interrupts can bounce through the vector while a
                 * syscall is still in flight. Re-arm one-shot Cause/EPC injection
                 * so the resumed exception path still decodes as SYSCALL.
                 */
                m->pending_cause_served = false;
                m->pending_epc_served = false;
                if (tlb_nested_keep_log < 64) {
                    fprintf(stderr,
                            "[TLB_NESTED_KEEP] kept syscall excode for intno=%u"
                            " at PC=0x%08" PRIX64 " (prev=0x%08X STATUS=0x%08" PRIX64
                            " served_cause:%u->%u served_epc:%u->%u)\n",
                            intno, (uint64_t)(uint32_t)pc, prev_insn, status,
                            old_cause_served ? 1u : 0u, m->pending_cause_served ? 1u : 0u,
                            old_epc_served ? 1u : 0u, m->pending_epc_served ? 1u : 0u);
                    tlb_nested_keep_log++;
                }
                return;
            }
            /*
             * Let Unicorn/QEMU deliver TLBL/TLBS natively.
             * Manual reinjection here caused a hard failure path:
             *   intno=26/27 -> forced vector -> UC_ERR_READ_UNMAPPED @ 0x80000000.
             *
             * If a synthetic exception (notably SYSCALL) is currently pending,
             * save it before native TLB handling so MFC0 Cause/EPC reads see
             * native CP0 values (ExcCode 2/3), not synthetic syscall state.
             * The saved state is restored on the TLB handler's ERET (in
             * prid_hook).  Both EXL=1 and EXL=0 paths use save/restore so
             * that SYSCALL tracking (execve_watch_active, pending_syscall_nr,
             * etc.) survives across transparent TLB refills.
             */
            static uint32_t tlb_passthrough_log_count = 0;
            static uint32_t tlb_nested_suspend_exl0_log = 0;
            static uint32_t tlb_nested_suspend_log = 0;
            if (m->pending_excode != 0) {
                if ((status & 0x2u) != 0u) {
                    bool save_was_noop = m->has_saved_exception;
                    save_pending_exception(m);
                    if (save_was_noop && m->execve_watch_active) {
                        static uint32_t save_noop_log = 0;
                        if (save_noop_log < 64) {
                            fprintf(stderr,
                                    "[TLB_NESTED_SAVE_NOOP] intno=%u PC=0x%08" PRIX64
                                    " pending_excode=%u saved_excode=%u"
                                    " clearing pending to 0!\n",
                                    intno, (uint64_t)(uint32_t)pc,
                                    m->pending_excode,
                                    m->saved_pending_excode);
                            save_noop_log++;
                        }
                    }
                    m->pending_epc          = 0;
                    m->pending_excode       = 0;
                    m->pending_cause        = 0;
                    m->epc_was_written      = false;
                    m->pending_cause_served = false;
                    m->pending_epc_served   = false;
                    if (tlb_nested_suspend_log < 64) {
                        fprintf(stderr,
                                "[TLB_NESTED_SUSPEND] intno=%u PC=0x%08" PRIX64
                                " STATUS=0x%08" PRIX64 " saved_excode=%u saved_epc=0x%08" PRIX64 "\n",
                                intno, (uint64_t)(uint32_t)pc, status,
                                m->saved_pending_excode, (uint64_t)(uint32_t)m->saved_pending_epc);
                        tlb_nested_suspend_log++;
                    }
                    return;
                }
                /* EXL=0: same save/restore as EXL=1 path above.
                 * Do NOT call clear_synthetic_syscall_state — it destroys
                 * execve tracking and syscall metadata that must survive
                 * across TLB refill.  Do NOT set has_saved_exception=false. */
                {
                    bool save_was_noop_exl0 = m->has_saved_exception;
                    save_pending_exception(m);
                    if (save_was_noop_exl0 && m->execve_watch_active) {
                        static uint32_t save_noop_exl0_log = 0;
                        if (save_noop_exl0_log < 64) {
                            fprintf(stderr,
                                    "[TLB_NESTED_SAVE_NOOP_EXL0] intno=%u PC=0x%08" PRIX64
                                    " pending_excode=%u saved_excode=%u"
                                    " clearing pending to 0!\n",
                                    intno, (uint64_t)(uint32_t)pc,
                                    m->pending_excode,
                                    m->saved_pending_excode);
                            save_noop_exl0_log++;
                        }
                    }
                }
                m->pending_epc          = 0;
                m->pending_excode       = 0;
                m->pending_cause        = 0;
                m->epc_was_written      = false;
                m->pending_cause_served = false;
                m->pending_epc_served   = false;
                if (tlb_nested_suspend_exl0_log < 64) {
                    fprintf(stderr,
                            "[TLB_NESTED_SUSPEND_EXL0] intno=%u PC=0x%08" PRIX64
                            " STATUS=0x%08" PRIX64 " saved_excode=%u saved_epc=0x%08" PRIX64 "\n",
                            intno, (uint64_t)(uint32_t)pc, status,
                            m->saved_pending_excode, (uint64_t)(uint32_t)m->saved_pending_epc);
                    tlb_nested_suspend_exl0_log++;
                }
            }
            if (tlb_trace_window_active(m) && tlb_passthrough_log_count < 96) {
                fprintf(stderr,
                        "[TLB_PASS] intno=%u PC=0x%08" PRIX64 " STATUS=0x%08" PRIX64
                        " pending_excode=%u pending_epc=0x%08" PRIX64 "\n",
                        intno, (uint64_t)(uint32_t)pc, status,
                        m->pending_excode, (uint64_t)(uint32_t)m->pending_epc);
                tlb_passthrough_log_count++;
            }
            return;
        }
        /*
         * Async interrupt (CP0 timer, external IRQ, …).  QEMU has already set
         * EXL=1, Cause, EPC and routed PC to the exception vector.
         *
         * Only reset one-shot serve flags when we are in our injected IRQ
         * context (pending_excode==1).  Doing this during an in-flight
         * SYSCALL (pending_excode==8) can re-arm syscall Cause injection and
         * mis-dispatch nested exceptions (e.g. page faults in execve).
         */
        if (m->pending_excode == 1u) {
            m->pending_cause_served = false;
            m->pending_epc_served   = false;
        }
        return;
    }

    /*
     * SYSCALL: perform manual MIPS exception entry.
     *
     *   EPC  = PC - 4  (address of the syscall instruction itself)
     *   EXL  = 1       (Status bit 1 — enter exception mode)
     *   PC   = 0x80000180  (general exception vector, BEV=0)
     *
     * Cause.ExcCode and EPC are not writable via Unicorn's 2.1.4 API.
     * They are emulated by intercepting the matching MFC0 / MTC0 / ERET
     * instructions inside prid_hook() below.
     */
    if (m->pending_excode != 0 && (status & 0x2u) == 0u) {
        static uint32_t syscall_entry_stale_clear_log = 0;
        if (syscall_entry_stale_clear_log < 64) {
            uint64_t v0 = 0, a3 = 0;
            uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
            uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
            fprintf(stderr,
                    "[SYSCALL_ENTRY_STALE_CLEAR] intno=%u PC=0x%08" PRIX64
                    " STATUS=0x%08" PRIX64
                    " old_excode=%u old_epc=0x%08" PRIX64
                    " old_nr=%u old_a0=0x%08" PRIX64 " \"%s\""
                    " v0=0x%08" PRIX64 " a3=0x%08" PRIX64 "\n",
                    intno, (uint64_t)(uint32_t)pc, status,
                    m->pending_excode, (uint64_t)(uint32_t)m->pending_epc,
                    m->pending_syscall_nr,
                    (uint64_t)(uint32_t)m->pending_syscall_a0,
                    m->pending_syscall_a0_str,
                    (uint64_t)(uint32_t)v0,
                    (uint64_t)(uint32_t)a3);
            syscall_entry_stale_clear_log++;
        }
        clear_synthetic_syscall_state(m, true);
        m->has_saved_exception  = false;
    }

    uint64_t epc = pc - 4u;   /* undo the PC advance Unicorn already applied */
    m->pending_epc          = epc;
    m->pending_syscall_epc  = epc;  /* freeze original site; MTC0 EPC will mutate pending_epc */
    m->pending_excode       = MIPS_EXCCODE_SYS;
    m->pending_cause        = (uint32_t)(MIPS_EXCCODE_SYS << 2);  /* Cause.ExcCode = 8 */
    m->epc_was_written      = false;  /* reset; set by MTC0 EPC intercept on exit path */
    m->pending_cause_served = false;
    m->pending_epc_served   = false;
    m->has_saved_exception  = false;  /* stale nested state must not leak across syscalls */
    reset_tlb_defer_state(m);         /* reset DEFER retry/ownership for this syscall */
    m->execve_user_handoff_active = false;
    m->execve_user_handoff_state = EXECVE_HANDOFF_STATE_DONE;
    m->execve_user_handoff_done_keep_count = 0;
    m->user_handoff_fault_traced = false;

    {
        uint64_t v0 = 0, a0 = 0, a1 = 0, a2 = 0, a3 = 0;
        uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
        uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
        uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
        uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
        uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
        uint64_t old_a0 = a0, old_a1 = a1, old_a2 = a2;

        if ((uint32_t)v0 == 4011u) {
            uint32_t a1_32 = (uint32_t)a1;
            uint32_t a2_32 = (uint32_t)a2;
            /*
             * Do not rewrite valid kernel argv/envp vectors (kseg addresses):
             * run_init_process() in this 2.4 kernel intentionally passes
             * kernel-space init_argv/init_envp pointers.
             *
             * For fallback init paths we can arrive with clobbered argv/envp
             * (e.g., argv pointing at filename bytes). Detect invalid vector
             * layouts and force stable defaults in that case.
             */
            bool needs_execve_defaults =
                ((a1_32 != 0u && a1_32 < 0x1000u) ||
                 (a2_32 != 0u && a2_32 <= 0x1000u) ||
                 !execve_vectors_look_valid(uc, a1_32, a2_32));
            /*
             * Keep kernel-space filename pointers intact for this 2.4 path.
             * run_init_process() invokes execve from kernel context with
             * kernel virtual string addresses ("/sbin/init", etc.).
             * Shimming a0 into user scratch breaks sys_execve's pt_regs path
             * and causes immediate -errno returns.
             */
            bool needs_execve_filename_shim = false;
            uint32_t sh_a0 = 0, sh_a1 = 0, sh_a2 = 0;
            bool used_defaults = false;
            bool filename_only = false;
            bool have_shim_ptrs = false;
            /*
             * Keep valid kernel argv/envp vectors intact (init_argv/init_envp on
             * kseg addresses). Only force scratch defaults when vectors are
             * genuinely invalid/clobbered.
             */
            if (needs_execve_defaults) {
                used_defaults = true;
                have_shim_ptrs = prepare_execve_user_ptrs_defaults(uc, a0, &sh_a0, &sh_a1, &sh_a2);
            } else if (needs_execve_filename_shim) {
                filename_only = true;
                have_shim_ptrs = prepare_execve_user_filename(uc, a0, &sh_a0);
                sh_a1 = a1_32;
                sh_a2 = a2_32;
            }
            if (have_shim_ptrs) {
                /*
                 * Filename-only mode: keep kernel argv/envp vectors intact and
                 * just re-home filename into user scratch for getname().
                 */
                uint64_t na0 = sh_a0;
                uint64_t na1 = sh_a1, na2 = sh_a2;
                uc_reg_write(uc, UC_MIPS_REG_A0, &na0);
                uc_reg_write(uc, UC_MIPS_REG_A1, &na1);
                uc_reg_write(uc, UC_MIPS_REG_A2, &na2);
                a0 = na0;
                a1 = na1;
                a2 = na2;
                static uint32_t execve_shim_log = 0;
                if (execve_shim_log < 32) {
                    fprintf(stderr,
                            "[EXECVE_SHIM_%s] a0:0x%08" PRIX64 "->0x%08" PRIX64
                            " a1:0x%08" PRIX64 "->0x%08" PRIX64
                            " a2:0x%08" PRIX64 "->0x%08" PRIX64 "\n",
                            (used_defaults ? "DEFAULTS" :
                            (filename_only ? "FILENAME" : "COPY")),
                            (uint64_t)(uint32_t)old_a0, (uint64_t)(uint32_t)a0,
                            (uint64_t)(uint32_t)old_a1, (uint64_t)(uint32_t)a1,
                            (uint64_t)(uint32_t)old_a2, (uint64_t)(uint32_t)a2);
                    execve_shim_log++;
                }
            }
        }

        char a0s[128] = "<unreadable>";
        read_guest_string(uc, a0, a0s, sizeof(a0s));

        m->pending_syscall_nr = (uint32_t)v0;
        m->pending_syscall_a0 = a0;
        strncpy(m->pending_syscall_a0_str, a0s, sizeof(m->pending_syscall_a0_str) - 1);
        m->pending_syscall_a0_str[sizeof(m->pending_syscall_a0_str) - 1] = '\0';

        /* Always log open/execve syscalls regardless of count; log others up to 80. */
        bool is_notable = ((uint32_t)v0 == 4005u || /* open  */
                           (uint32_t)v0 == 4041u || /* dup   */
                           (uint32_t)v0 == 4011u);  /* execve */
        if (is_notable || syscall_entry_log_count < 80) {
            fprintf(stderr,
                    "[SYSCALL_INJECT] EPC=0x%08" PRIX64 " nr=%" PRIu64
                    " a0=0x%08" PRIX64 " \"%s\" a1=0x%08" PRIX64
                    " a2=0x%08" PRIX64 " a3=0x%08" PRIX64
                    " cause=0x%08X STATUS=0x%08" PRIX64 "\n",
                    (uint64_t)(uint32_t)epc, (uint64_t)(uint32_t)v0,
                    (uint64_t)(uint32_t)a0, a0s,
                    (uint64_t)(uint32_t)a1, (uint64_t)(uint32_t)a2,
                    (uint64_t)(uint32_t)a3, m->pending_cause,
                    (uint64_t)(uint32_t)status);
            if (!is_notable)
                syscall_entry_log_count++;
        }

        if ((uint32_t)v0 == 4011u && execve_args_log < 48u) {
            uint32_t argp = (uint32_t)a1;
            uint32_t envp = (uint32_t)a2;
            fprintf(stderr,
                    "[EXECVE_ARGS] filename=\"%s\" argv=0x%08X envp=0x%08X\n",
                    a0s, argp, envp);

            for (int i = 0; i < 4; i++) {
                uint32_t p = 0;
                char s[96] = "<unreadable>";
                if (argp != 0 && read_guest_u32(uc, (uint64_t)argp + (uint64_t)(i * 4), &p)) {
                    if (p != 0)
                        read_guest_string(uc, p, s, sizeof(s));
                    else
                        strcpy(s, "<NULL>");
                    fprintf(stderr, "[EXECVE_ARGS] argv[%d]=0x%08X \"%s\"\n", i, p, s);
                    if (p == 0)
                        break;
                } else {
                    fprintf(stderr, "[EXECVE_ARGS] argv[%d]=<unreadable-ptr>\n", i);
                    break;
                }
            }

            for (int i = 0; i < 4; i++) {
                uint32_t p = 0;
                char s[96] = "<unreadable>";
                if (envp != 0 && read_guest_u32(uc, (uint64_t)envp + (uint64_t)(i * 4), &p)) {
                    if (p != 0)
                        read_guest_string(uc, p, s, sizeof(s));
                    else
                        strcpy(s, "<NULL>");
                    fprintf(stderr, "[EXECVE_ARGS] envp[%d]=0x%08X \"%s\"\n", i, p, s);
                    if (p == 0)
                        break;
                } else {
                    fprintf(stderr, "[EXECVE_ARGS] envp[%d]=<unreadable-ptr>\n", i);
                    break;
                }
            }

            execve_args_log++;
        }
    }

    uint64_t new_status = status | 0x2u;   /* set EXL */
    uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &new_status);

    uint64_t vec = mips_sext(0x80000180u);
    uc_reg_write(uc, UC_MIPS_REG_PC, &vec);

    /* (debug) fprintf(stderr, "[INTR] SYSCALL at EPC=0x%016" PRIX64
                    ", routing to exception vector\n", epc); */
}

/* Suppress timer/ICU IRQ injection while execve handoff is still settling.
 * This avoids re-entering 0x80000180 before first userspace progress. */
static bool handoff_irq_quarantine_active(machine_t *m, uint32_t *pc32_out)
{
    if (m->execve_user_handoff_pc == 0u)
        return false;

    if (m->execve_user_handoff_active ||
        m->execve_user_handoff_state == EXECVE_HANDOFF_STATE_USER_FETCH_SEEN)
        return true;

    if (m->execve_user_handoff_state != EXECVE_HANDOFF_STATE_DONE)
        return false;

    uint64_t pc = 0;
    uc_reg_read(m->uc, UC_MIPS_REG_PC, &pc);
    uint32_t pc32 = (uint32_t)pc;
    if (pc32_out)
        *pc32_out = pc32;

    uint32_t handoff_pc = (uint32_t)m->execve_user_handoff_pc;
    uint32_t last_pc = (uint32_t)m->last_exec_pc;
    bool loop_sig =
        (pc32 == handoff_pc || pc32 == 0x80000180u || pc32 == 0x80001850u ||
         last_pc == handoff_pc || last_pc == 0x80000180u || last_pc == 0x80001850u);
    return loop_sig;
}

/*
 * Inject a hardware interrupt (ExcCode=0 / INTR) into the guest CPU when
 * the VR41xx ICU has a pending, enabled interrupt source.
 *
 * Called once per execution batch (before reading PC) so the injected
 * exception vector is picked up as the start address of the next batch.
 *
 * Conditions for injection:
 *   1. No other injected exception is already in flight (pending_excode==0).
 *   2. ICU has at least one unmasked pending source (icu_pending()).
 *   3. CPU is receptive: Status.IE=1, Status.EXL=0, Status.ERL=0.
 *   4. Status.IM2 (bit 10) is set — HW0/INT0 interrupt line is unmasked.
 *
 * When all conditions are met, we perform the manual MIPS exception-entry
 * sequence (same pattern as intr_hook for SYSCALL):
 *   EPC  = current PC (instruction that would have run next)
 *   pending_cause = IP2 set (bit 10), ExcCode=0 (Interrupt)
 *   Status.EXL = 1
 *   PC = 0x80000180 (general exception vector, BEV=0)
 *
 * The exception is then handled by the guest: except_vec3_r4000 reads
 * Cause (returns pending_cause via prid_hook MFC0 intercept), finds
 * ExcCode=0, dispatches to handle_int, which reads the VR41xx ICU to
 * identify the source and calls the appropriate handler (e.g. do_timer
 * for the ETIME tick, which advances jiffies and wakes sleeping tasks).
 */
static void inject_hw_irq_if_pending(machine_t *m)
{
    static uint32_t log_block_pending_excode = 0;
    static uint32_t log_block_status = 0;
    static uint32_t log_block_im2 = 0;
    static uint32_t log_injected = 0;
    static uint32_t log_stale_pending = 0;
    static uint32_t log_handoff_quarantine = 0;
    bool pending = icu_pending(&m->icu);

    if (!m->tlb_trace_window &&
        m->pending_excode == MIPS_EXCCODE_SYS &&
        is_run_init_syscall_epc((uint32_t)m->pending_epc)) {
        m->tlb_trace_window = true;
        fprintf(stderr,
                "[TLB_TRACE] armed near run_init_process pending_epc=0x%08" PRIX64 "\n",
                (uint64_t)(uint32_t)m->pending_epc);
    }

    if (m->pending_excode != 0) {
        /*
         * Synthetic exception state should track EXL=1 while in-flight.
         * If EXL is already clear, the guest has effectively returned from
         * exception but we missed retirement bookkeeping; clear stale state.
         */
        uint64_t status64 = 0;
        uc_reg_read(m->uc, UC_MIPS_REG_CP0_STATUS, &status64);
        uint32_t status = (uint32_t)status64;
        if ((status & 0x2u) == 0) {
            if (m->pending_excode == 1u) {
                if (log_stale_pending < 32) {
                    uint64_t pc = 0;
                    uc_reg_read(m->uc, UC_MIPS_REG_PC, &pc);
                    fprintf(stderr,
                            "[IRQ_GATE] stale pending_excode=%u cleared (EXL=0) PC=0x%08" PRIX64
                            " STATUS=0x%08X pending_epc=0x%08" PRIX64
                            " epc_written=%u served_epc=%u served_cause=%u\n",
                            m->pending_excode, (uint64_t)(uint32_t)pc, status,
                            (uint64_t)(uint32_t)m->pending_epc,
                            m->epc_was_written ? 1u : 0u,
                            m->pending_epc_served ? 1u : 0u,
                            m->pending_cause_served ? 1u : 0u);
                    log_stale_pending++;
                }
                m->pending_epc          = 0;
                m->pending_excode       = 0;
                m->pending_cause        = 0;
                m->epc_was_written      = false;
                m->pending_cause_served = false;
                m->pending_epc_served   = false;
                reset_tlb_defer_state(m);
            } else if (m->pending_excode == MIPS_EXCCODE_SYS) {
                static uint32_t log_syscall_exl_drop = 0;
                static uint32_t log_syscall_ret_fallback = 0;
                static uint32_t log_syscall_exl_drop_abort = 0;
                uint64_t pc = 0;
                uc_reg_read(m->uc, UC_MIPS_REG_PC, &pc);
                uint32_t ret_site = (uint32_t)m->pending_epc + 4u;
                bool at_ret_site = ((uint32_t)pc == ret_site);
                if (!at_ret_site) {
                    m->tlb_exl_drop_defer_count++;
                    if (log_syscall_exl_drop < 64) {
                        fprintf(stderr,
                                "[IRQ_GATE] syscall_exl_drop_defer pending_excode=%u"
                                " PC=0x%08" PRIX64 " STATUS=0x%08X pending_epc=0x%08" PRIX64
                                " ret_site=0x%08X defer_count=%u epc_written=%u"
                                " served_epc=%u served_cause=%u\n",
                                m->pending_excode, (uint64_t)(uint32_t)pc, status,
                                (uint64_t)(uint32_t)m->pending_epc, ret_site,
                                m->tlb_exl_drop_defer_count,
                                m->epc_was_written ? 1u : 0u,
                                m->pending_epc_served ? 1u : 0u,
                                m->pending_cause_served ? 1u : 0u);
                        log_syscall_exl_drop++;
                    }
                    if (m->tlb_exl_drop_defer_count > TLB_EXL_DROP_DEFER_LIMIT &&
                        !m->execve_watch_active) {
                        /* Abort stale syscall state, but NOT during active
                         * execve: do_execve can generate thousands of kuseg
                         * stores (__bzero/memset), each triggering a spurious
                         * TLB notification + IRQ gate deferral.  The syscall
                         * is still actively progressing. */
                        if (log_syscall_exl_drop_abort < 32) {
                            fprintf(stderr,
                                    "[IRQ_GATE] syscall_exl_drop_abort PC=0x%08" PRIX64
                                    " pending_epc=0x%08" PRIX64 " defer_count=%u limit=%u\n",
                                    (uint64_t)(uint32_t)pc,
                                    (uint64_t)(uint32_t)m->pending_epc,
                                    m->tlb_exl_drop_defer_count,
                                    TLB_EXL_DROP_DEFER_LIMIT);
                            log_syscall_exl_drop_abort++;
                        }
                        clear_synthetic_syscall_state(m, true);
                        m->has_saved_exception = false;
                    }
                    return;
                }
                m->tlb_exl_drop_defer_count = 0;
                if (log_syscall_exl_drop < 64) {
                    fprintf(stderr,
                            "[IRQ_GATE] syscall_exl_drop pending_excode=%u PC=0x%08" PRIX64
                            " STATUS=0x%08X pending_epc=0x%08" PRIX64
                            " epc_written=%u served_epc=%u served_cause=%u\n",
                            m->pending_excode, (uint64_t)(uint32_t)pc, status,
                            (uint64_t)(uint32_t)m->pending_epc,
                            m->epc_was_written ? 1u : 0u,
                            m->pending_epc_served ? 1u : 0u,
                            m->pending_cause_served ? 1u : 0u);
                    log_syscall_exl_drop++;
                }
                if (log_syscall_ret_fallback < 64) {
                    uint64_t v0 = 0, a3 = 0;
                    uc_reg_read(m->uc, UC_MIPS_REG_V0, &v0);
                    uc_reg_read(m->uc, UC_MIPS_REG_A3, &a3);
                    /*
                     * Successful execve handoff can reach the post-SYSCALL site
                     * with user context already staged in regs:
                     *   v0 ~= user stack pointer, a3 ~= user entry PC.
                     * In that case, transition to user entry directly instead
                     * of treating this as a plain syscall fallback.
                     */
                    if (m->pending_syscall_nr == 4011u &&
                        (uint32_t)v0 >= 0x70000000u && (uint32_t)v0 < 0x80000000u &&
                        (uint32_t)a3 >= 0x00010000u && (uint32_t)a3 < 0x80000000u) {
                        static uint32_t execve_ctx_seen_irq_log = 0;
                        if (execve_ctx_seen_irq_log < 64) {
                            fprintf(stderr,
                                    "[EXECVE_CTX_IRQ_GATE] pending_epc=0x%08" PRIX64
                                    " pc=0x%08" PRIX64
                                    " v0=0x%08" PRIX64 " a3=0x%08" PRIX64
                                    " action=clear_synthetic_only\n",
                                    (uint64_t)(uint32_t)m->pending_epc,
                                    (uint64_t)(uint32_t)pc,
                                    (uint64_t)(uint32_t)v0,
                                    (uint64_t)(uint32_t)a3);
                            execve_ctx_seen_irq_log++;
                        }
                    }
                    fprintf(stderr,
                            "[SYSCALL_RET_FALLBACK] pending_epc=0x%08" PRIX64
                            " pc=0x%08" PRIX64 " nr=%u"
                            " a0=0x%08" PRIX64 " \"%s\""
                            " v0=0x%08" PRIX64 " a3=0x%08" PRIX64
                            " status=0x%08X\n",
                            (uint64_t)(uint32_t)m->pending_epc,
                            (uint64_t)(uint32_t)pc,
                            m->pending_syscall_nr,
                            (uint64_t)(uint32_t)m->pending_syscall_a0,
                            m->pending_syscall_a0_str,
                            (uint64_t)(uint32_t)v0,
                            (uint64_t)(uint32_t)a3,
                            status);
                    log_syscall_ret_fallback++;
                }
                /*
                 * Treat EXL=0 with a pending synthetic SYSCALL as completed
                 * return-path bookkeeping we failed to retire in the ERET hook.
                 * Clear synthetic state so later IRQ/syscall handling is not
                 * blocked by stale pending_excode=SYS.
                 */
                clear_synthetic_syscall_state(m, true);
                m->has_saved_exception  = false;
            } else {
                if (log_stale_pending < 32) {
                    uint64_t pc = 0;
                    uc_reg_read(m->uc, UC_MIPS_REG_PC, &pc);
                    fprintf(stderr,
                            "[IRQ_GATE] stale non-sys pending_excode=%u cleared (EXL=0)"
                            " PC=0x%08" PRIX64 " STATUS=0x%08X pending_epc=0x%08" PRIX64 "\n",
                            m->pending_excode, (uint64_t)(uint32_t)pc, status,
                            (uint64_t)(uint32_t)m->pending_epc);
                    log_stale_pending++;
                }
                m->pending_epc          = 0;
                m->pending_excode       = 0;
                m->pending_cause        = 0;
                m->epc_was_written      = false;
                m->pending_cause_served = false;
                m->pending_epc_served   = false;
                reset_tlb_defer_state(m);
            }
        }
    }

    uint32_t quarantine_pc = 0;
    if (handoff_irq_quarantine_active(m, &quarantine_pc)) {
        if (log_handoff_quarantine < 64) {
            fprintf(stderr,
                    "[IRQ_GATE] handoff-quarantine PC=0x%08X handoff_pc=0x%08" PRIX64
                    " state=%u active=%u pending=%u SYSINT1=0x%04X MSYSINT1=0x%04X RTCINT=0x%04X\n",
                    quarantine_pc,
                    (uint64_t)(uint32_t)m->execve_user_handoff_pc,
                    (unsigned)m->execve_user_handoff_state,
                    m->execve_user_handoff_active ? 1u : 0u,
                    pending ? 1u : 0u,
                    m->icu.sysint1, m->icu.msysint1, m->rtc.rtcint);
            log_handoff_quarantine++;
        }
        return;
    }

    /* Only inject when no exception is already in flight */
    if (m->pending_excode != 0) {
        if (pending && log_block_pending_excode < 16) {
            uint64_t pc = 0;
            uc_reg_read(m->uc, UC_MIPS_REG_PC, &pc);
            fprintf(stderr,
                    "[IRQ_GATE] blocked: pending_excode=%u pending_cause=0x%08X pending_epc=0x%08" PRIX64
                    " PC=0x%08" PRIX64 " SYSINT1=0x%04X MSYSINT1=0x%04X RTCINT=0x%04X\n",
                    m->pending_excode, m->pending_cause,
                    (uint64_t)(uint32_t)m->pending_epc,
                    (uint64_t)(uint32_t)pc,
                    m->icu.sysint1, m->icu.msysint1, m->rtc.rtcint);
            log_block_pending_excode++;
        }
        return;
    }

    if (!pending)
        return;

    uint64_t status = 0;
    uc_reg_read(m->uc, UC_MIPS_REG_CP0_STATUS, &status);
    uint32_t s32 = (uint32_t)status;

    /* CPU must be receptive: IE=1, EXL=0, ERL=0 (bits [2:0] == 0b001) */
    if ((s32 & 0x7u) != 0x1u) {
        if (log_block_status < 16) {
            uint64_t pc = 0;
            uc_reg_read(m->uc, UC_MIPS_REG_PC, &pc);
            fprintf(stderr,
                    "[IRQ_GATE] blocked: status gate STATUS=0x%08X PC=0x%08" PRIX64
                    " SYSINT1=0x%04X MSYSINT1=0x%04X RTCINT=0x%04X\n",
                    s32, (uint64_t)(uint32_t)pc,
                    m->icu.sysint1, m->icu.msysint1, m->rtc.rtcint);
            log_block_status++;
        }
        return;
    }

    /* INT0/HW0 mapped to IP2 = Status bit 10; check it is not masked */
    if (!(s32 & (1u << 10))) {
        if (log_block_im2 < 16) {
            uint64_t pc = 0;
            uc_reg_read(m->uc, UC_MIPS_REG_PC, &pc);
            fprintf(stderr,
                    "[IRQ_GATE] blocked: IM2 masked STATUS=0x%08X PC=0x%08" PRIX64
                    " SYSINT1=0x%04X MSYSINT1=0x%04X RTCINT=0x%04X\n",
                    s32, (uint64_t)(uint32_t)pc,
                    m->icu.sysint1, m->icu.msysint1, m->rtc.rtcint);
            log_block_im2++;
        }
        return;
    }

    /* Inject: save return address, build Cause, redirect to exception vector */
    uint64_t pc = 0;
    uc_reg_read(m->uc, UC_MIPS_REG_PC, &pc);
    m->pending_epc          = pc;
    m->pending_excode       = 1u;           /* INTR sentinel — any non-zero value */
    m->pending_cause        = (1u << 10);   /* IP2 = HW0 = INT0, ExcCode = 0     */
    m->epc_was_written      = false;        /* reset; set by MTC0 EPC intercept   */
    m->pending_cause_served = false;
    m->pending_epc_served   = false;

    uint64_t new_status = status | 0x2u;  /* set EXL */
    uc_reg_write(m->uc, UC_MIPS_REG_CP0_STATUS, &new_status);

    uint64_t vec = mips_sext(0x80000180u);
    uc_reg_write(m->uc, UC_MIPS_REG_PC, &vec);
    m->irq_injected_count++;

    if (log_injected < 16) {
        fprintf(stderr,
                "[IRQ_GATE] injected: EPC=0x%08" PRIX64 " STATUS=0x%08X"
                " pending_cause=0x%08X SYSINT1=0x%04X MSYSINT1=0x%04X RTCINT=0x%04X\n",
                (uint64_t)(uint32_t)pc, s32, m->pending_cause,
                m->icu.sysint1, m->icu.msysint1, m->rtc.rtcint);
        log_injected++;
    }
}

/*
 * Fallback timer IRQ injection for kernels that never touch VR41xx ICU MMIO.
 *
 * Some 2.4 test kernels appear to rely on CP0 timer-style interrupt cadence
 * during early networking/tasklet bring-up, but do not configure/use the ICU
 * path implemented above. If no ICU MMIO has been observed after early boot,
 * periodically inject an interrupt with IP7 set.
 */
static void inject_fallback_timer_irq_if_needed(machine_t *m)
{
    static uint32_t log_fallback = 0;

    /* Ground-truth WinCE NAND path should not use synthetic fallback IRQs. */
    if (is_wince_boot_machine(m))
        return;

    if (handoff_irq_quarantine_active(m, NULL))
        return;

    if (m->pending_excode != 0)
        return;
    if (m->irq_injected_count > 0)
        return;
    if (m->insn_count < 20000000ull)
        return;

    uint64_t status = 0;
    uc_reg_read(m->uc, UC_MIPS_REG_CP0_STATUS, &status);
    uint32_t s32 = (uint32_t)status;

    /* CPU receptive and IP7 unmasked */
    if ((s32 & 0x7u) != 0x1u)
        return;
    if ((s32 & (1u << 15)) == 0u)
        return;

    /* Rate-limit to one interrupt every 8 execution batches. */
    m->fallback_timer_div++;
    if (m->fallback_timer_div < 8u)
        return;
    m->fallback_timer_div = 0;

    uint64_t pc = 0;
    uc_reg_read(m->uc, UC_MIPS_REG_PC, &pc);
    m->pending_epc          = pc;
    m->pending_excode       = 1u;
    m->pending_cause        = (1u << 15); /* IP7 */
    m->epc_was_written      = false;
    m->pending_cause_served = false;
    m->pending_epc_served   = false;

    uint64_t new_status = status | 0x2u;  /* EXL=1 */
    uc_reg_write(m->uc, UC_MIPS_REG_CP0_STATUS, &new_status);
    uint64_t vec = mips_sext(0x80000180u);
    uc_reg_write(m->uc, UC_MIPS_REG_PC, &vec);

    if (log_fallback < 32) {
        fprintf(stderr,
                "[IRQ_FALLBACK] injected IP7 EPC=0x%08" PRIX64
                " STATUS=0x%08X insns=%" PRIu64 "M\n",
                (uint64_t)(uint32_t)pc, s32, m->insn_count / 1000000ull);
        log_fallback++;
    }
}


void machine_mmio_history_record(machine_t *m, bool is_write, uint32_t pa,
                                 unsigned size, uint64_t value, uint32_t pc)
{
    if (!m || !is_wince_boot_machine(m))
        return;

    maybe_record_wince_delay_touch(m, is_write, true, pa, size, value, pc);

    uint32_t idx = m->mmio_hist_head % WINCE_MMIO_HISTORY_LEN;
    mmio_hist_entry_t *e = &m->mmio_hist[idx];
    e->pa = pa;
    e->pc = pc;
    e->size_bits = (uint16_t)(size * 8u);
    e->is_write = is_write ? 1u : 0u;
    e->reserved = 0;
    e->value = value;

    m->mmio_hist_head = (m->mmio_hist_head + 1u) % WINCE_MMIO_HISTORY_LEN;
    if (m->mmio_hist_count < WINCE_MMIO_HISTORY_LEN)
        m->mmio_hist_count++;
}


/* ------------------------------------------------------------------ */
/* Machine lifecycle                                                     */
/* ------------------------------------------------------------------ */

machine_t *machine_create(const machine_config_t *cfg)
{
    machine_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;

    m->cfg = *cfg;
    bool wince_boot = is_wince_boot_cfg(cfg);
    m->cp0_count_base = wince_boot ? VR4131_COUNT_WARM : 0u;
    m->cp0_compare_shadow = wince_boot ? VR4131_COMPARE_WARM : 0u;
    m->cp0_compare_shadow_valid = wince_boot;
    if (m->cfg.sdram_size == 0)
        m->cfg.sdram_size = 16u * 1024u * 1024u;   /* 16 MB default */

    {
        size_t backing_size = ((size_t)m->cfg.sdram_size + 0xFFFu) & ~(size_t)0xFFFu;
        void *backing = NULL;
        if (posix_memalign(&backing, 0x1000u, backing_size) == 0 && backing != NULL) {
            memset(backing, 0, backing_size);
            m->sdram_backing = (uint8_t *)backing;
            m->sdram_backing_size = backing_size;
        } else {
            fprintf(stderr,
                    "[ALIAS_MODE] SDRAM backing alloc failed"
                    " size=0x%08X -> map+write-sync fallback\n",
                    m->cfg.sdram_size);
            m->sdram_backing = NULL;
            m->sdram_backing_size = 0;
        }
    }

    /*
     * Open the Unicorn engine: MIPS64 little-endian.
     *
     * The linux4.be vmlinux uses 133k+ MIPS-III 64-bit instructions (LD, SD,
     * DADDIU, LDL, LDR, BEQL, BNEL, …).  MIPS32 mode raises Reserved Instruction
     * exceptions for all of these.  MIPS64 with the NEC VR5432 CPU model supports
     * the full MIPS-III 64-bit ISA.
     *
     * The kernel runs in 32-bit address mode (CP0 Status KX/SX/UX = 0), which
     * keeps kseg0/kseg1 segment translation identical to MIPS32.
     */
    uc_err err = uc_open(UC_ARCH_MIPS,
                         UC_MODE_MIPS64 | UC_MODE_LITTLE_ENDIAN,
                         &m->uc);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "[MACHINE] uc_open failed: %s\n", uc_strerror(err));
        free(m->sdram_backing);
        free(m);
        return NULL;
    }

    /*
     * Map physical memory BEFORE setting the CPU model.
     * uc_ctl_set_cpu_model() for MIPS32_4KC creates internal QEMU machine
     * regions (including a boot-ROM stub at/near PA 0) that can overlap with
     * our SDRAM range.  By calling bus_init first we win the region claim.
     */
    bus_init(m);

    /*
     * Pre-map the user-space (kuseg) VA range in Unicorn — gaps only.
     *
     * Unicorn MIPS SOFTMMU treats kuseg virtual addresses (0x00000000–
     * 0x7FFFFFFF) as physical addresses for kernel-mode memory accesses,
     * completely bypassing the MIPS TLB.  When the kernel writes to user-
     * space VAs (e.g., create_elf_tables writing argv/envp/auxv to the user
     * stack at ~0x7FFF7F40), Unicorn checks the physical memory map and
     * returns UC_ERR_WRITE_UNMAPPED if unmapped — without calling the fault
     * hook or routing to the guest TLB-miss handler.
     *
     * Physical memory layout (must not overlap):
     *   0x00000000–0x00FFFFFF  SDRAM (already mapped by bus_init)
     *   0x0A000000–0x0AFFFFFF  VRC4173 companion chip (MMIO)
     *   0x0F000000–0x0F000FFF  Internal I/O (MMIO)
     *   0x1E000000–0x1FFFFFFF  ROM/Flash (already mapped by bus_init)
     *
     * We fill in the "free" kuseg gaps between those regions.  Host OS uses
     * lazy mmap so large holes cost no physical host RAM until touched.
     */
    if (!wince_boot) {
        static const struct { uint32_t base; uint32_t size; const char *name; } gaps[] = {
            { 0x01000000u, 0x09000000u, "0x01000000–0x09FFFFFF" }, /* after SDRAM, before VRC4173 */
            { 0x0B000000u, 0x04000000u, "0x0B000000–0x0EFFFFFF" }, /* after VRC4173, before IO */
            { 0x0F001000u, 0x0EFFF000u, "0x0F001000–0x1DFFFFFF" }, /* after IO, before ROM */
            { 0x20000000u, 0x60000000u, "0x20000000–0x7FFFFFFF" }, /* after ROM, top of kuseg */
        };
        for (int gi = 0; gi < 4; gi++) {
            fprintf(stderr, "[MACHINE] user-space pre-map begin %s\n", gaps[gi].name);
            /* User-space gaps are only used for kernel writes until
             * user space actually runs, so keep them RW to avoid
             * Unicorn generating TBs (and invalidation) on macOS. */
            uc_err uerr = uc_mem_map(m->uc, gaps[gi].base, gaps[gi].size,
                                     UC_PROT_READ | UC_PROT_WRITE);
            if (uerr != UC_ERR_OK)
                fprintf(stderr, "[MACHINE] user-space pre-map %s failed: %s\n",
                        gaps[gi].name, uc_strerror(uerr));
            else
                fprintf(stderr, "[MACHINE] user-space pre-mapped %s\n", gaps[gi].name);
        }
    } else {
        fprintf(stderr,
                "[MACHINE] user-space pre-map disabled for WinCE/NAND boot"
                " (preserve guest TLB faults)\n");
    }

    /*
     * Select the NEC VR5432 CPU model (MIPS IV, NEC VR series).
     * This is the closest available Unicorn MIPS64 model to the VR4131 and
     * supports the full MIPS-III 64-bit instruction set the kernel relies on.
     */
    uc_ctl_set_cpu_model(m->uc, UC_CPU_MIPS64_VR5432);

    /* Set CP0 Status for reset state: BEV=1 (boot vectors), ERL=1 */
    uint64_t status = 0x00400004u;
    uc_reg_write(m->uc, UC_MIPS_REG_CP0_STATUS, &status);
    bcu_init(&m->bcu);
    cmu_init(&m->cmu);
    pmu_init(&m->pmu);
    icu_init(&m->icu);
    siu_init(&m->siu);
    rtc_init(&m->rtc);
    if (wince_boot) {
        /* SPL uses ETIME polling loops; keep only a minimal read-assist. */
        m->rtc.etime_read_step = 1;
    } else {
        /* Preserve pre-warm-seed Linux boot defaults for direct kernel boots. */
        m->cmu.clkmsk = 0xFFFFu;
        m->rtc.etime = 0;
        m->rtc.etime_latched = 0;
    }
    gpio_init(&m->gpio);
    nand_init(&m->nand, NULL, 0);
    init_wince_region_tracks(m);
    init_wince_producer_attrs(m);
    m->wince_region_nz2z_logs = 0;
    m->wince_deferred_seed_done = false;
    m->wince_despec_touch_resume_ctx = false;
    m->wince_despec_touch_stack_frame = false;
    m->wince_despec_touch_cb_globals = false;
    m->wince_despec_touch_pmu_rtc = false;
    m->wince_despec_touch_nand_c38 = false;
    m->wince_despec_touch_cb_tbl = false;
    m->wince_despec_touch_objptr = false;
    m->wince_despec_touch_obj_exec_page = false;
    m->wince_despec_touch_obj = false;
    m->wince_despec_touch_bootctx = false;
    m->wince_despec_touch_bootparam0 = false;
    m->wince_despec_touch_bootparam1 = false;
    m->wince_div_hist_head = 0;
    m->wince_div_hist_count = 0;
    m->wince_div_logs = 0;
    memset(&m->wince_div_call_trace, 0, sizeof(m->wince_div_call_trace));

    /* Always install the invalid-instruction hook (for MACC) */
    uc_hook hk;
    uc_hook_add(m->uc, &hk, UC_HOOK_INSN_INVALID,
                insn_invalid_hook, m, 1, 0);
    uc_hook_add(m->uc, &hk,
                UC_HOOK_MEM_READ_UNMAPPED |
                UC_HOOK_MEM_WRITE_UNMAPPED |
                UC_HOOK_MEM_FETCH_UNMAPPED,
                mem_unmapped_hook, m, 1, 0);
    if (m->alias_fallback_sync_active && !m->shared_alias_active) {
        uc_err hs = uc_hook_add(m->uc, &hk, UC_HOOK_MEM_WRITE,
                                alias_write_sync_hook, m, 1, 0);
        if (hs == UC_ERR_OK) {
            fprintf(stderr, "[ALIAS_MODE] write-sync hook enabled\n");
        } else {
            fprintf(stderr, "[ALIAS_MODE] write-sync hook failed: %s\n", uc_strerror(hs));
        }
    }
    if (is_wince_boot_cfg(cfg)) {
        static const uint64_t watch_bases[] = {
            UINT64_C(0x0000000000000000),
            UINT64_C(0x0000000080000000),
            UINT64_C(0x00000000A0000000),
            UINT64_C(0xFFFFFFFF80000000),
            UINT64_C(0xFFFFFFFFA0000000),
        };
        static const struct {
            uint32_t start;
            uint32_t end; /* inclusive */
            const char *name;
        } watch_ranges[] = {
            { WINCE_TRACE_VEC_PA_START, WINCE_TRACE_CTX_PA_END - 1u, "vec_ctx" },
            { WINCE_TRACE_BOOTCTX_PA_START, WINCE_TRACE_BOOTCTX_PA_END - 1u, "bootctx" },
            { WINCE_TRACE_BOOTPARAM0_PA_START, WINCE_TRACE_BOOTPARAM0_PA_END - 1u, "bootparam0" },
            { WINCE_TRACE_BOOTPARAM1_PA_START, WINCE_TRACE_BOOTPARAM1_PA_END - 1u, "bootparam1" },
            { WINCE_TRACE_CB_PA_START,  WINCE_TRACE_CB_PA_END - 1u,  "cb_tbl" },
            { WINCE_TRACE_OBJPTR_PA_START, WINCE_TRACE_OBJPTR_PA_END - 1u, "obj_ptr" },
            { WINCE_TRACE_OBJ_EXEC_PAGE_PA_START, WINCE_TRACE_OBJ_EXEC_PAGE_PA_END - 1u, "obj_exec_page" },
            { WINCE_TRACE_OBJ_PA_START, WINCE_TRACE_OBJ_PA_END - 1u, "obj" },
            { WINCE_TRACE_GATE_PA_START, WINCE_TRACE_GATE_PA_END - 1u, "gate" },
            { WINCE_TRACE_GATE_SRC_PA_START, WINCE_TRACE_GATE_SRC_PA_END - 1u, "gate_src" },
            { WINCE_TRACE_STACK_PA_START, WINCE_TRACE_STACK_PA_END - 1u, "stack" },
            { WINCE_TRACE_CALLER_FRAME_PA_START, WINCE_TRACE_CALLER_FRAME_PA_END - 1u, "caller_frame" },
            { WINCE_TRACE_RESUME_GLOBAL_PA_START, WINCE_TRACE_RESUME_GLOBAL_PA_END - 1u, "resume_global" },
            { WINCE_TRACE_S0_OBJ_PA_START, WINCE_TRACE_S0_OBJ_PA_END - 1u, "s0_obj" },
        };
        bool watch_ok = true;
        for (unsigned r = 0; r < sizeof(watch_ranges) / sizeof(watch_ranges[0]); r++) {
            uint64_t start_off = watch_ranges[r].start;
            uint64_t end_off = watch_ranges[r].end;
            for (unsigned i = 0; i < sizeof(watch_bases) / sizeof(watch_bases[0]); i++) {
                uc_err hw = uc_hook_add(m->uc, &hk, UC_HOOK_MEM_WRITE,
                                        wince_pa_watch_write_hook, m,
                                        watch_bases[i] + start_off,
                                        watch_bases[i] + end_off);
                if (hw != UC_ERR_OK) {
                    watch_ok = false;
                    fprintf(stderr,
                            "[WINCE_PA_WATCH] hook failed range=%s base=0x%016" PRIX64
                            " start=0x%08" PRIX64 " end=0x%08" PRIX64 ": %s\n",
                            watch_ranges[r].name, watch_bases[i], start_off, end_off,
                            uc_strerror(hw));
                }
            }
        }
        if (watch_ok) {
            m->wince_pa_watch_active = true;
            fprintf(stderr,
                    "[WINCE_PA_WATCH] enabled ranges"
                    " vec=0x%08X-0x%08X ctx=0x%08X-0x%08X"
                    " bootctx=0x%08X-0x%08X bootparam0=0x%08X-0x%08X"
                    " bootparam1=0x%08X-0x%08X"
                    " cb=0x%08X-0x%08X objptr=0x%08X-0x%08X"
                    " obj_exec_page=0x%08X-0x%08X obj=0x%08X-0x%08X gate=0x%08X-0x%08X"
                    " gate_src=0x%08X-0x%08X stack=0x%08X-0x%08X"
                    " caller_frame=0x%08X-0x%08X"
                    " resume_global=0x%08X-0x%08X s0_obj=0x%08X-0x%08X aliases=5\n",
                    WINCE_TRACE_VEC_PA_START, WINCE_TRACE_VEC_PA_END - 1u,
                    WINCE_TRACE_CTX_PA_START, WINCE_TRACE_CTX_PA_END - 1u,
                    WINCE_TRACE_BOOTCTX_PA_START, WINCE_TRACE_BOOTCTX_PA_END - 1u,
                    WINCE_TRACE_BOOTPARAM0_PA_START, WINCE_TRACE_BOOTPARAM0_PA_END - 1u,
                    WINCE_TRACE_BOOTPARAM1_PA_START, WINCE_TRACE_BOOTPARAM1_PA_END - 1u,
                    WINCE_TRACE_CB_PA_START, WINCE_TRACE_CB_PA_END - 1u,
                    WINCE_TRACE_OBJPTR_PA_START, WINCE_TRACE_OBJPTR_PA_END - 1u,
                    WINCE_TRACE_OBJ_EXEC_PAGE_PA_START, WINCE_TRACE_OBJ_EXEC_PAGE_PA_END - 1u,
                    WINCE_TRACE_OBJ_PA_START, WINCE_TRACE_OBJ_PA_END - 1u,
                    WINCE_TRACE_GATE_PA_START, WINCE_TRACE_GATE_PA_END - 1u,
                    WINCE_TRACE_GATE_SRC_PA_START, WINCE_TRACE_GATE_SRC_PA_END - 1u,
                    WINCE_TRACE_STACK_PA_START, WINCE_TRACE_STACK_PA_END - 1u,
                    WINCE_TRACE_CALLER_FRAME_PA_START, WINCE_TRACE_CALLER_FRAME_PA_END - 1u,
                    WINCE_TRACE_RESUME_GLOBAL_PA_START, WINCE_TRACE_RESUME_GLOBAL_PA_END - 1u,
                    WINCE_TRACE_S0_OBJ_PA_START, WINCE_TRACE_S0_OBJ_PA_END - 1u);
        }

        /* Read hook on fptr table 0x80075580..0x8007559F */
        bool fptbl_ok = true;
        for (unsigned i = 0; i < sizeof(watch_bases) / sizeof(watch_bases[0]); i++) {
            uc_err hf = uc_hook_add(m->uc, &hk, UC_HOOK_MEM_READ,
                                    wince_fptbl_read_hook, m,
                                    watch_bases[i] + WINCE_TRACE_FPTBL_PA_START,
                                    watch_bases[i] + WINCE_TRACE_FPTBL_PA_END - 1u);
            if (hf != UC_ERR_OK) {
                fptbl_ok = false;
                fprintf(stderr,
                        "[FPTBL_READ] hook failed base=0x%016" PRIX64 ": %s\n",
                        watch_bases[i], uc_strerror(hf));
            }
        }
        if (fptbl_ok) {
            fprintf(stderr,
                    "[FPTBL_READ] enabled watch on 0x%08X-0x%08X (5 aliases)\n",
                    WINCE_TRACE_FPTBL_PA_START, WINCE_TRACE_FPTBL_PA_END - 1u);
        }

        /* De-speculation read hooks: fire one-shot first-touch diagnostics
         * on reads to all SDRAM despec families (P1 fix). */
        static const struct {
            uint32_t start;
            uint32_t end; /* inclusive */
            const char *name;
        } despec_read_ranges[] = {
            { UINT32_C(0x00002200), UINT32_C(0x000022FF), "resume_ctx" },
            { UINT32_C(0x00001700), UINT32_C(0x000017FF), "stack_frame" },
            { UINT32_C(0x00679400), UINT32_C(0x006795FF), "cb_globals" },
            { WINCE_TRACE_CB_PA_START, WINCE_TRACE_CB_PA_END - 1u, "cb_tbl" },
            { WINCE_TRACE_OBJPTR_PA_START, WINCE_TRACE_OBJPTR_PA_END - 1u, "objptr" },
            { WINCE_TRACE_OBJ_EXEC_PAGE_PA_START, WINCE_TRACE_OBJ_EXEC_PAGE_PA_END - 1u, "obj_exec_page" },
            { WINCE_TRACE_OBJ_PA_START, WINCE_TRACE_OBJ_PA_END - 1u, "obj" },
            { WINCE_TRACE_BOOTCTX_PA_START, WINCE_TRACE_BOOTCTX_PA_END - 1u, "bootctx" },
            { WINCE_TRACE_BOOTPARAM0_PA_START, WINCE_TRACE_BOOTPARAM0_PA_END - 1u, "bootparam0" },
            { WINCE_TRACE_BOOTPARAM1_PA_START, WINCE_TRACE_BOOTPARAM1_PA_END - 1u, "bootparam1" },
        };
        bool despec_read_ok = true;
        for (unsigned r = 0; r < sizeof(despec_read_ranges) / sizeof(despec_read_ranges[0]); r++) {
            uint64_t start_off = despec_read_ranges[r].start;
            uint64_t end_off = despec_read_ranges[r].end;
            for (unsigned i = 0; i < sizeof(watch_bases) / sizeof(watch_bases[0]); i++) {
                uc_err hdr = uc_hook_add(m->uc, &hk, UC_HOOK_MEM_READ,
                                         wince_despec_read_hook, m,
                                         watch_bases[i] + start_off,
                                         watch_bases[i] + end_off);
                if (hdr != UC_ERR_OK) {
                    despec_read_ok = false;
                    fprintf(stderr,
                            "[DESPEC_READ] hook failed range=%s base=0x%016" PRIX64
                            " start=0x%08" PRIX64 " end=0x%08" PRIX64 ": %s\n",
                            despec_read_ranges[r].name, watch_bases[i],
                            start_off, end_off, uc_strerror(hdr));
                }
            }
        }
        if (despec_read_ok) {
            fprintf(stderr,
                    "[DESPEC_READ] enabled read hooks on %zu ranges x 5 aliases\n",
                    sizeof(despec_read_ranges) / sizeof(despec_read_ranges[0]));
        }
    }

    /*
     * Interrupt / CPU-exception hook.
     * Handles MIPS SYSCALL exceptions that Unicorn 2.1.4 does not
     * transparently route to the guest exception vector.
     */
    uc_hook_add(m->uc, &hk, UC_HOOK_INTR, intr_hook, m, 1, 0);

    /*
     * PRId intercept hook — fires for every instruction, checks for
     * MFC0 $rt, $15 and substitutes VR4131_PRID (0x00000C80).
     * This hook is always installed (low per-instruction cost: one read +
     * one compare; only acts on the rare MFC0 PRId instruction).
     */
    uc_hook_add(m->uc, &hk, UC_HOOK_CODE, prid_hook, m, 1, 0);

    /* Trace hook (only when requested — high overhead) */
    if (cfg->trace)
        uc_hook_add(m->uc, &hk, UC_HOOK_CODE, trace_hook, m, 1, 0);

    if (!is_wince_boot_cfg(cfg)) {
        probes_register_hooks(m);
    }

    /* NAND / WinCE boot mode: load B000FF SPL from NAND image */
    if (is_wince_boot_cfg(cfg)) {
        uint32_t entry_va = 0;
        if (loader_load_nand(m, cfg->nand_path, &entry_va) != 0) {
            fprintf(stderr, "[MACHINE] NAND B000FF load failed\n");
            machine_destroy(m);
            return NULL;
        }
        m->kernel_entry = mips_sext(entry_va);
        m->has_jiffies_pa = false;
        m->jiffies_pa = 0;

        /* Initialize NAND controller with the loaded image */
        nand_init(&m->nand, m->nand_data, m->nand_size);

        /*
         * WinCE SPL warm boot state from hardware snapshots.
         */
        apply_wince_warm_profile(m, NULL);

        /* Set up a stack for the SPL in high SDRAM */
        uint64_t sp = mips_sext(0x80F00000u); /* below SPL load area */
        uc_reg_write(m->uc, UC_MIPS_REG_SP, &sp);

        /* Verify entry-point bytes */
        {
            uint32_t pa_entry = entry_va & 0x1FFFFFFFu;
            uint32_t insns[4] = {0};
            uc_mem_read(m->uc, pa_entry, insns, sizeof(insns));
            fprintf(stderr, "[MACHINE] NAND entry PA=0x%08X  insns: %08X %08X %08X %08X\n",
                    pa_entry, insns[0], insns[1], insns[2], insns[3]);
        }

        fprintf(stderr, "[MACHINE] WinCE NAND boot: entry VA=0x%08X (PA=0x%08X)\n",
                entry_va, entry_va & 0x1FFFFFFFu);

    } else if (cfg->kernel_path) {
    /* Kernel direct-boot mode: load ELF and set entry point */
        uint32_t entry_va = 0;
        uint32_t jiffies_pa = 0;
        if (loader_load_elf(m, cfg->kernel_path, &entry_va, &jiffies_pa) != 0) {
            fprintf(stderr, "[MACHINE] Kernel ELF load failed\n");
            machine_destroy(m);
            return NULL;
        }
        m->kernel_entry = mips_sext(entry_va);
        if (jiffies_pa != 0) {
            m->jiffies_pa = jiffies_pa;
            m->has_jiffies_pa = true;
            fprintf(stderr, "[MACHINE] jiffies tick target PA=0x%08X\n", m->jiffies_pa);
        } else {
            m->jiffies_pa = 0;
            m->has_jiffies_pa = false;
        }

        fprintf(stderr,
                "[LINUX_LEGACY_PROFILE] STATUS=0x00400004 COUNT=0x%08X COMPARE_VALID=%u\n",
                m->cp0_count_base, m->cp0_compare_shadow_valid ? 1u : 0u);

        /*
         * MIPS Linux prom_init() calling convention (linux4.be / arch/mips/vr41xx):
         *   $a0 = argc  (0 = no args, or 1 if cmdline provided)
         *   $a1 = argv  (pointer to cmdline string in low SDRAM, or 0)
         *   $a2 = 0     (envp, unused by prom_init)
         *   $a3 = 0     (reserved / memory descriptor ptr — stub 0)
         *
         * The cmdline string (if any) is placed at a well-known low-SDRAM
         * address (0x00000200) so prom_init can find it.
         */
        uint64_t a0 = 0, a1 = 0;
        if (cfg->cmdline && cfg->cmdline[0]) {
            const char *cl = cfg->cmdline;
            uint32_t   cl_pa = 0x00000200u;   /* physical backing in low SDRAM */
            uint32_t   cl_va = 0xA0000200u;   /* kseg1 virtual pointer seen by kernel */
            const char *arg0 = "be300";
            uint32_t arg0_pa = 0x00000240u;
            uint32_t arg0_va = 0xA0000240u;
            uc_mem_write(m->uc, cl_pa, cl, strlen(cl) + 1);
            uc_mem_write(m->uc, arg0_pa, arg0, strlen(arg0) + 1);
            /* Build argv[] in RAM and pass virtual address in $a1. */
            uint32_t argv_pa = 0x000001F0u;
            uint32_t argv_va = 0xA00001F0u;
            uint32_t argv_words[3] = { arg0_va, cl_va, 0 };
            uc_mem_write(m->uc, argv_pa, argv_words, sizeof(argv_words));
            a0 = 2;
            a1 = mips_sext(argv_va);
        }
        uint64_t a2 = 0, a3 = 0;
        uc_reg_write(m->uc, UC_MIPS_REG_A0, &a0);
        uc_reg_write(m->uc, UC_MIPS_REG_A1, &a1);
        uc_reg_write(m->uc, UC_MIPS_REG_A2, &a2);
        uc_reg_write(m->uc, UC_MIPS_REG_A3, &a3);

        /*
         * For direct kernel boot, relax CP0 Status to kernel mode:
         * BEV=0 (normal exception vectors), EXL=0, ERL=0, KSU=00 (kernel).
         * IM bits all enabled so the kernel can configure them itself.
         */
        uint64_t kstatus = 0x0000FF00u; /* IM7-IM0 all set, no EXL/ERL/BEV */
        uc_reg_write(m->uc, UC_MIPS_REG_CP0_STATUS, &kstatus);

        /*
         * Pre-fill BCU CLKSPEEDREG for VR4131-style encoding:
         *   - CLKSP   bits [4:0]  : core clock step
         *   - VTDIV   bits [10:8] : VT clock divisor selector (must be non-zero)
         *   - TDIVMODE bit  [12]  : 0=/2, 1=/4
         *
         * The previous value (0x004A) left VTDIV=0 and linux4.be executed a
         * guarded DIVU with divisor 0, triggering BREAK.
         */
        m->bcu.clkspeedreg = 0x1108u; /* TDIV=/4, VTDIV=1, CLKSP=8 */

        /* Verify entry-point bytes are correctly loaded */
        {
            uint32_t pa_entry = entry_va & 0x1FFFFFFFu;
            uint32_t insns[4] = {0};
            uc_mem_read(m->uc, pa_entry, insns, sizeof(insns));
            fprintf(stderr, "[MACHINE] Entry PA=0x%08X  insns: %08X %08X %08X %08X\n",
                    pa_entry, insns[0], insns[1], insns[2], insns[3]);
        }

    } else {
        /* ROM boot mode: load flat binary at reset vector */
        m->kernel_entry = mips_sext(VA_RESET_VECTOR);

        if (cfg->rom_path) {
            if (loader_load_rom(m, cfg->rom_path) != 0) {
                fprintf(stderr, "[MACHINE] ROM load failed\n");
                machine_destroy(m);
                return NULL;
            }
        }
    }

    /* Optionally preload RAM (both modes) */
    if (cfg->ram_path)
        loader_load_ram(m, cfg->ram_path);

    alias_coherence_probe(m);

    return m;
}

void machine_destroy(machine_t *m)
{
    if (!m) return;
    if (m->uc) uc_close(m->uc);
    free(m->nand_data);
    free(m->sdram_backing);
    free(m);
}

/*
 * Main execution loop.
 *
 * Run in batches of BATCH_SIZE instructions.  Between batches we check
 * for ICU interrupt state and advance the RTC — this is the point where
 * future interrupt injection will be added.
 *
 * Unicorn with MIPS softmmu handles TLB misses, CP0 exceptions, ERET, etc.
 * internally; they are transparent to this loop.
 */
#define BATCH_SIZE 100000u
#define POST_INIT_BATCH_SIZE 1000u

void machine_run(machine_t *m)
{
    m->running    = true;
    m->insn_count = 0;
    m->cp0_count_ticks = 0;
    m->rtc_last_count_tick = (uint64_t)machine_cp0_count32(m);
    m->rtc_tick_frac_num = 0;
    m->post_init_trace_window = false;
    m->post_init_trace_batches = 0;
    int write_unmapped_recoveries = 0;
    uint32_t stall_track_pc = UINT32_MAX;
    uint32_t stall_track_samples = 0;
    bool stall_episode_active = false;
    bool stall_episode_dumped = false;
    uint32_t stall_episode_pc = 0;

    fprintf(stderr, "[MACHINE] Starting execution at VA 0x%016" PRIX64 "\n",
            m->kernel_entry);
    if (is_wince_boot_machine(m) && m->cfg.log_wince_stall) {
        fprintf(stderr,
                "[WINCE_STALL_MODE] diagnostics-only"
                " (no synthetic IRQ kick)\n");
    }

    /* Set PC to the entry point (sign-extended 64-bit VA for MIPS64 mode) */
    uc_reg_write(m->uc, UC_MIPS_REG_PC, &m->kernel_entry);

    if (ui_init(m) < 0) {
        fprintf(stderr, "[MACHINE] Failed to initialize UI\n");
    }

    while (m->running) {
        /* Advance simulated time and update peripheral interrupt state.
         * RTC progression is coupled to emulated CP0 Count progression
         * (target 32.768 kHz crystal equivalent). */

        if (ui_should_quit(m)) {
            m->running = false;

    ui_destroy(m);
            break;
        }
        ui_update(m);

        if (ui_should_quit(m)) {
            m->running = false;

    ui_destroy(m);
            break;
        }
        ui_update(m);
        machine_advance_rtc_from_cp0(m);
        tick_jiffies_hack(m);
        update_irq_lines(m);

        /* Inject a hardware interrupt (INTR, ExcCode=0) if the ICU has a
         * pending, unmasked source and the CPU is receptive.  The injected
         * PC redirect is picked up by the uc_reg_read(PC) below. */
        inject_hw_irq_if_pending(m);
        inject_fallback_timer_irq_if_needed(m);

        uint64_t pc = 0;
        uc_reg_read(m->uc, UC_MIPS_REG_PC, &pc);

        if (m->post_init_trace_window) {
            if (m->post_init_trace_batches < 512u) {
                uint64_t status = 0;
                uc_reg_read(m->uc, UC_MIPS_REG_CP0_STATUS, &status);
                fprintf(stderr,
                        "[POST_INIT] batch=%u pre PC=0x%08" PRIX64
                        " STATUS=0x%08" PRIX64
                        " pending_excode=%u pending_epc=0x%08" PRIX64
                        " pending_cause=0x%08X epc_written=%u served_epc=%u served_cause=%u\n",
                        m->post_init_trace_batches,
                        (uint64_t)(uint32_t)pc, status,
                        m->pending_excode, (uint64_t)(uint32_t)m->pending_epc,
                        m->pending_cause,
                        m->epc_was_written ? 1u : 0u,
                        m->pending_epc_served ? 1u : 0u,
                        m->pending_cause_served ? 1u : 0u);
                m->post_init_trace_batches++;
            } else if (m->post_init_trace_batches == 512u) {
                fprintf(stderr, "[POST_INIT] batch trace suppressed after 512 samples\n");
                m->post_init_trace_batches++;
            }
        }

        /* Periodic PC sample: log PC every 100 batches (~10M insns) to
         * show where execution is spending time when silent. */
        if ((m->insn_count / BATCH_SIZE) % 100 == 0) {
            uint32_t pc32 = (uint32_t)pc;
            fprintf(stderr, "[PROGRESS] insns=%" PRIu64 "M  PC=0x%08" PRIX64
                    " pend_exc=%u pend_epc=0x%08" PRIX64
                    " epc_wr=%u has_saved=%u execve_act=%u"
                    " execve_user_v=%u\n",
                    m->insn_count / 1000000, (uint64_t)pc32,
                    m->pending_excode,
                    (uint64_t)(uint32_t)m->pending_epc,
                    m->epc_was_written ? 1u : 0u,
                    m->has_saved_exception ? 1u : 0u,
                    m->execve_watch_active ? 1u : 0u,
                    m->execve_user_entry_valid ? 1u : 0u);

            bool stall_candidate = is_wince_boot_machine(m) &&
                                   !is_wince_spl_pc(pc32);

            if (stall_candidate && pc32 == stall_track_pc) {
                if (stall_track_samples < UINT32_MAX)
                    stall_track_samples++;
            } else {
                stall_track_pc = pc32;
                stall_track_samples = stall_candidate ? 1u : 0u;
            }

            if (stall_episode_active && pc32 != stall_episode_pc) {
                stall_episode_active = false;
                stall_episode_dumped = false;
                if (m->cfg.log_wince_stall) {
                    fprintf(stderr,
                            "[WINCE_STALL_CLEAR] prev_pc=0x%08X new_pc=0x%08X\n",
                            stall_episode_pc, pc32);
                }
            }

            if (stall_candidate && stall_track_samples >= WINCE_STALL_SAMPLE_COUNT) {
                if (!stall_episode_active || stall_episode_pc != pc32) {
                    stall_episode_active = true;
                    stall_episode_pc = pc32;
                    stall_episode_dumped = false;
                }

                if (m->cfg.log_wince_stall && !stall_episode_dumped) {
                    log_wince_stall_dump(m, pc32);
                    stall_episode_dumped = true;
                }
            }
        }

        /* Apply deferred ERET redirect from injected TLB exception handler.
         * The hook-based redirects (prid_hook/intr_hook) write PC and stop,
         * but the instruction at the stale PC may have already executed.
         * This is the final safety net: override PC before uc_emu_start. */
        if (m->pending_eret_redirect) {
            uc_reg_write(m->uc, UC_MIPS_REG_PC, &m->pending_eret_redirect_pc);
            m->pending_eret_redirect = false;
        }

        uint64_t run_pc = 0;
        uc_reg_read(m->uc, UC_MIPS_REG_PC, &run_pc);
        /* Extended batch trace: all batches while /sbin/init syscall is pending */
        if ((uint32_t)m->pending_syscall_epc == 0x8000184Cu &&
            m->pending_syscall_nr == 4011u) {
            static uint32_t sbin_init_batch_log = 0;
            if (sbin_init_batch_log < 512) {
                uint64_t batch_status = 0;
                uc_reg_read(m->uc, UC_MIPS_REG_CP0_STATUS, &batch_status);
                fprintf(stderr,
                        "[SBIN_INIT_BATCH] #%u PC=0x%08" PRIX64
                        " STATUS=0x%08" PRIX64
                        " pend_exc=%u pend_epc=0x%08" PRIX64
                        " epc_wr=%u has_saved=%u execve_act=%u"
                        " insn=%" PRIu64 "\n",
                        sbin_init_batch_log, (uint64_t)(uint32_t)run_pc,
                        batch_status,
                        m->pending_excode,
                        (uint64_t)(uint32_t)m->pending_epc,
                        m->epc_was_written ? 1u : 0u,
                        m->has_saved_exception ? 1u : 0u,
                        m->execve_watch_active ? 1u : 0u,
                        m->insn_count);
                sbin_init_batch_log++;
            }
        }
        /* Batch-level tracing during execve */
        if (m->execve_watch_active && m->pending_syscall_nr == 4011u) {
            static uint32_t execve_batch_log = 0;
            if (execve_batch_log < 512) {
                uint64_t batch_status = 0;
                uc_reg_read(m->uc, UC_MIPS_REG_CP0_STATUS, &batch_status);
                fprintf(stderr,
                        "[EXECVE_BATCH] #%u PC=0x%08" PRIX64
                        " STATUS=0x%08" PRIX64
                        " pend_exc=%u pend_epc=0x%08" PRIX64
                        " epc_wr=%u has_saved=%u\n",
                        execve_batch_log, (uint64_t)(uint32_t)run_pc,
                        batch_status,
                        m->pending_excode,
                        (uint64_t)(uint32_t)m->pending_epc,
                        m->epc_was_written ? 1u : 0u,
                        m->has_saved_exception ? 1u : 0u);
                execve_batch_log++;
            }
        }
        uint32_t step_count = m->post_init_trace_window ? POST_INIT_BATCH_SIZE : BATCH_SIZE;

        /* Snapshot execve state before batch to detect transitions */
        bool pre_execve_active = m->execve_watch_active;
        uint32_t pre_pending_excode = m->pending_excode;
        bool pre_has_saved = m->has_saved_exception;
        uint32_t pre_pending_syscall_epc = (uint32_t)m->pending_syscall_epc;
        uint32_t pre_pending_syscall_nr = m->pending_syscall_nr;

        uc_err err = uc_emu_start(m->uc, run_pc, 0, 0, step_count);

        /* Post-batch trace: use pre-saved epc so we catch batch #5 even
         * when pending_syscall_nr is cleared mid-batch */
        if (pre_execve_active && pre_pending_syscall_epc == 0x8000184Cu) {
            static uint32_t execve_post_log = 0;
            if (execve_post_log < 512) {
                uint64_t post_pc = 0;
                uc_reg_read(m->uc, UC_MIPS_REG_PC, &post_pc);
                uint64_t post_status = 0;
                uc_reg_read(m->uc, UC_MIPS_REG_CP0_STATUS, &post_status);
                fprintf(stderr,
                        "[EXECVE_POST] #%u post_PC=0x%08" PRIX64
                        " STATUS=0x%08" PRIX64
                        " pend_exc=%u->%u pend_epc=0x%08" PRIX64
                        " epc_wr=%u has_saved=%u->%u execve_act=%u->%u"
                        " syscall_nr=%u->%u syscall_epc=0x%08X->0x%08" PRIX64
                        " err=%d\n",
                        execve_post_log, (uint64_t)(uint32_t)post_pc,
                        post_status,
                        pre_pending_excode, m->pending_excode,
                        (uint64_t)(uint32_t)m->pending_epc,
                        m->epc_was_written ? 1u : 0u,
                        pre_has_saved ? 1u : 0u,
                        m->has_saved_exception ? 1u : 0u,
                        pre_execve_active ? 1u : 0u,
                        m->execve_watch_active ? 1u : 0u,
                        pre_pending_syscall_nr, m->pending_syscall_nr,
                        pre_pending_syscall_epc,
                        (uint64_t)(uint32_t)m->pending_syscall_epc,
                        (int)err);
                execve_post_log++;
            }
        }

        if (err != UC_ERR_OK) {
            uint64_t bad_pc = 0;
            uc_reg_read(m->uc, UC_MIPS_REG_PC, &bad_pc);
            if (err == UC_ERR_WRITE_UNMAPPED) {
                /* Log ALL WRITE_UNMAPPED events with kuseg-range fault addresses
                 * to track padzero/clear_user faults during execve */
                {
                    static uint32_t all_wunmap_log = 0;
                    uint64_t wfault_addr = m->last_unmapped_valid
                        ? m->last_unmapped_addr : 0;
                    if (all_wunmap_log < 128) {
                        fprintf(stderr,
                                "[WRITE_UNMAPPED_ALL] #%u PC=0x%08" PRIX64
                                " faultVA=0x%08" PRIX64
                                " last_valid=%u pend_exc=%u has_saved=%u"
                                " kuseg_unmap=%u vm_ready=%u"
                                " syscall_epc=0x%08" PRIX64 "\n",
                                all_wunmap_log,
                                (uint64_t)(uint32_t)bad_pc,
                                wfault_addr,
                                m->last_unmapped_valid ? 1u : 0u,
                                m->pending_excode,
                                m->has_saved_exception ? 1u : 0u,
                                m->kuseg_gaps_unmapped ? 1u : 0u,
                                m->kernel_vm_ready ? 1u : 0u,
                                (uint64_t)(uint32_t)m->pending_syscall_epc);
                        all_wunmap_log++;
                    }
                }
                /*
                 * Recovery for write faults mirrors the read-fault strategy:
                 * consult last fault VA + broad register candidates and map
                 * either kuseg aliases or kseg mirrors, then retry.
                 */
                uint64_t badv = 0, at = 0, v0 = 0, a0 = 0, a1 = 0, a2 = 0, a3 = 0;
                uint64_t k0 = 0, k1 = 0, t2 = 0, sp = 0;
                uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0;
                uint64_t gp = 0, fp = 0, ra = 0;
                uc_mem_type fault_type = (uc_mem_type)0;
                if (m->last_unmapped_valid) {
                    badv = m->last_unmapped_addr;
                    fault_type = m->last_unmapped_type;
                } else if (m->shadow_cp0_badvaddr != 0) {
                    badv = m->shadow_cp0_badvaddr;
                }
                m->last_unmapped_valid = false;

                /*
                 * Kernel-mode TLBS injection for kuseg writes.
                 *
                 * When kernel code stores to a kuseg VA and the shadow TLB has
                 * no valid writable entry, inject a TLBS exception so the
                 * kernel's page fault handler runs (allocates a page, sets up
                 * PTEs, TLBWI, ERET → retry).
                 *
                 * If the shadow TLB DOES have a valid D=1 entry, populate the
                 * Unicorn flat mapping from SDRAM, then fall through to emulate
                 * the store.
                 */
                if ((uint32_t)bad_pc >= 0x80000000u) {
                    uint32_t store_va = 0;
                    if (decode_store_target_va(m, bad_pc, &store_va) &&
                        store_va < 0x80000000u) {
                        if (m->kernel_vm_ready) {
                            tlb_lookup_result_t wr = shadow_tlb_lookup(m, store_va);
                            bool has_writable = false;
                            if (wr.hit && wr.confident) {
                                bool odd = (((uint64_t)store_va & wr.page_bytes) != 0u);
                                uint32_t lo = odd ? wr.lo1 : wr.lo0;
                                has_writable = ((lo & 0x4u) != 0u);  /* D bit */
                            }
                            if (!has_writable) {
                                /* No TLB entry or D=0 → inject TLBS */
                                if (inject_kernel_kuseg_tlbs(m, bad_pc, store_va)) {
                                    write_unmapped_recoveries++;
                                    continue;
                                }
                            } else {
                                /* Valid D=1 entry → populate from SDRAM and emulate */
                                shadow_tlb_populate(m, store_va, true,
                                                    "KSTORE_POPULATE",
                                                    (uint32_t)bad_pc);
                            }
                        }
                    } else if (decode_store_target_va(m, bad_pc, &store_va) &&
                               store_va >= 0xC0000000u &&
                               (m->kernel_vm_ready || is_wince_boot_machine(m))) {
                        if (shadow_tlb_populate_kernel_mapped(m, store_va,
                                                              "KSTORE_KMAPPED",
                                                              (uint32_t)bad_pc)) {
                            write_unmapped_recoveries++;
                            continue;
                        }
                    }
                }

                /*
                 * Some Unicorn builds repeatedly report WRITE_UNMAPPED on
                 * the same store without completing it even after mapping.
                 * Decode/commit simple stores directly to break the livelock.
                 */
                {
                    static uint32_t wunmap_emu_log = 0;
                    uint32_t emu_store_va = 0;
                    bool emu_decoded = decode_store_target_va(m, bad_pc, &emu_store_va);
                    if (wunmap_emu_log < 128 && m->kuseg_gaps_unmapped) {
                        uint32_t emu_insn = 0;
                        read_insn_best_effort(m->uc, bad_pc, &emu_insn);
                        fprintf(stderr,
                                "[WUNMAP_FALLBACK] PC=0x%08" PRIX64
                                " badv=0x%08" PRIX64
                                " decoded=%u store_va=0x%08X insn=0x%08X\n",
                                (uint64_t)(uint32_t)bad_pc, badv,
                                emu_decoded ? 1u : 0u, emu_store_va, emu_insn);
                        wunmap_emu_log++;
                    }
                }
                if (emulate_store_nearby_on_write_unmapped(m, bad_pc)) {
                    /* After emulating the store, write back to SDRAM for
                     * kuseg coherence if the store target was kuseg. */
                    uint32_t wb_va = 0;
                    if (decode_store_target_va(m, bad_pc, &wb_va) &&
                        wb_va < 0x80000000u) {
                        uint32_t insn = 0;
                        read_insn_best_effort(m->uc, bad_pc, &insn);
                        uint32_t op = insn >> 26;
                        uint32_t sz = 4;
                        if (op == 0x28u) sz = 1;       /* sb */
                        else if (op == 0x29u) sz = 2;   /* sh */
                        else if (op == 0x2Bu) sz = 4;   /* sw */
                        else if (op == 0x3Fu) sz = 8;   /* sd */
                        kuseg_store_writeback(m, wb_va, sz);
                    }
                    write_unmapped_recoveries++;
                    continue;
                }

                uc_reg_read(m->uc, UC_MIPS_REG_AT, &at);
                uc_reg_read(m->uc, UC_MIPS_REG_V0, &v0);
                uc_reg_read(m->uc, UC_MIPS_REG_A0, &a0);
                uc_reg_read(m->uc, UC_MIPS_REG_A1, &a1);
                uc_reg_read(m->uc, UC_MIPS_REG_A2, &a2);
                uc_reg_read(m->uc, UC_MIPS_REG_A3, &a3);
                uc_reg_read(m->uc, UC_MIPS_REG_K0, &k0);
                uc_reg_read(m->uc, UC_MIPS_REG_K1, &k1);
                uc_reg_read(m->uc, UC_MIPS_REG_T2, &t2);
                uc_reg_read(m->uc, UC_MIPS_REG_SP, &sp);
                uc_reg_read(m->uc, UC_MIPS_REG_S0, &s0);
                uc_reg_read(m->uc, UC_MIPS_REG_S1, &s1);
                uc_reg_read(m->uc, UC_MIPS_REG_S2, &s2);
                uc_reg_read(m->uc, UC_MIPS_REG_S3, &s3);
                uc_reg_read(m->uc, UC_MIPS_REG_S4, &s4);
                uc_reg_read(m->uc, UC_MIPS_REG_S5, &s5);
                uc_reg_read(m->uc, UC_MIPS_REG_S6, &s6);
                uc_reg_read(m->uc, UC_MIPS_REG_S7, &s7);
                uc_reg_read(m->uc, UC_MIPS_REG_GP, &gp);
                uc_reg_read(m->uc, UC_MIPS_REG_FP, &fp);
                uc_reg_read(m->uc, UC_MIPS_REG_RA, &ra);
                uint64_t candidates[] = {
                    bad_pc, badv, at, v0, a0, a1, a2, a3, k0, k1, t2, sp,
                    s0, s1, s2, s3, s4, s5, s6, s7, gp, fp, ra
                };
                const char *names[] = {
                    "pc", "badv", "at", "v0", "a0", "a1", "a2", "a3", "k0", "k1", "t2", "sp",
                    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "gp", "fp", "ra"
                };
                bool mapped_any = false;

                for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++) {
                    uint64_t va = candidates[i];
                    uint32_t va32 = (uint32_t)va;
                    if (va32 < 0x1000u)
                        continue;
                    uint64_t block = va & ~((uint64_t)0xFFFFF);
                    uint64_t block32 = (uint64_t)(va32 & ~0xFFFFFu);
                    bool mapped = false;

                    if (va32 >= 0x80000000u && va32 <= 0xBFFFFFFFu) {
                        mapped = map_kseg_mirror_block(m, block);
                    } else if (va32 < 0x80000000u) {
                        /* When kernel VM is ready and kuseg gaps are unmapped,
                         * do NOT remap kuseg addresses — they must go through
                         * TLB/page-fault injection for demand paging. */
                        if (m->kernel_vm_ready && m->kuseg_gaps_unmapped)
                            continue;
                        uc_err me32 = uc_mem_map(m->uc, block32, 0x100000,
                                                 UC_PROT_READ | UC_PROT_WRITE);
                        if (me32 == UC_ERR_OK || me32 == UC_ERR_MAP)
                            mapped = true;

                        if (block != block32) {
                            uc_err meraw = uc_mem_map(m->uc, block, 0x100000,
                                                      UC_PROT_READ | UC_PROT_WRITE);
                            if (meraw == UC_ERR_OK || meraw == UC_ERR_MAP)
                                mapped = true;
                        }
                    }

                    if (mapped) {
                        if (!mapped_any) {
                            uint32_t bad_insn = 0xFFFFFFFFu;
                            read_insn_best_effort(m->uc, bad_pc, &bad_insn);
                            fprintf(stderr,
                                    "[MACHINE] write-unmapped recovery #%d at PC=0x%08" PRIX64
                                    " badv=0x%08" PRIX64 " type=%d insn=0x%08X\n",
                                    write_unmapped_recoveries + 1,
                                    (uint64_t)(uint32_t)bad_pc,
                                    (uint64_t)(uint32_t)badv,
                                    (int)fault_type,
                                    bad_insn);
                        }
                        fprintf(stderr,
                                "[MACHINE]   mapped block 0x%08" PRIX64 " via $%s=0x%08" PRIX64 "\n",
                                (uint64_t)(uint32_t)block32, names[i],
                                (uint64_t)(uint32_t)va);
                        mapped_any = true;
                    }
                }

                if (mapped_any) {
                    write_unmapped_recoveries++;
                    continue;
                }
            }
            if (err == UC_ERR_READ_UNMAPPED) {
                /*
                 * Recovery for occasional unmapped reads while entering the
                 * exception vector path. Try mapping likely candidate blocks
                 * derived from registers and retry execution.
                 */
                uint64_t badv = 0, at = 0, a0 = 0, a1 = 0, a2 = 0, k0 = 0, k1 = 0, t2 = 0, sp = 0;
                uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0;
                uint64_t gp = 0, fp = 0, ra = 0;
                uc_mem_type fault_type = (uc_mem_type)0;
                uint32_t decoded_load_va = 0;
                bool decoded_load_valid = false;
                if (m->last_unmapped_valid) {
                    badv = m->last_unmapped_addr;
                    fault_type = m->last_unmapped_type;
                } else if (m->shadow_cp0_badvaddr != 0) {
                    badv = m->shadow_cp0_badvaddr;
                }
                if ((uint32_t)bad_pc >= 0x80000000u) {
                    decoded_load_valid = decode_load_source_va(m, bad_pc, &decoded_load_va);
                    if (decoded_load_valid && badv == 0u)
                        badv = decoded_load_va;
                }
                /*
                 * Kernel-mode TLBL injection for kuseg reads.
                 * When kernel code reads from a kuseg VA and there's no
                 * shadow TLB entry, inject TLBL so the kernel's page fault
                 * handler populates the page from the ramdisk/filesystem.
                 * If a valid TLB entry exists, populate from SDRAM and retry.
                 */
                if ((uint32_t)bad_pc >= 0x80000000u) {
                    uint32_t load_va = 0;
                    if (decoded_load_valid)
                        load_va = decoded_load_va;
                    if (decoded_load_valid && load_va < 0x80000000u && m->kernel_vm_ready) {
                        tlb_lookup_result_t rd = shadow_tlb_lookup(m, load_va);
                        if (rd.hit && rd.confident) {
                            /* Populate from SDRAM and retry */
                            shadow_tlb_populate(m, load_va, true,
                                                "KLOAD_POPULATE",
                                                (uint32_t)bad_pc);
                            m->last_unmapped_valid = false;
                            continue;
                        } else {
                            /* No entry → inject TLBL */
                            if (inject_kernel_kuseg_tlbl(m, bad_pc, load_va)) {
                                m->last_unmapped_valid = false;
                                continue;
                            }
                        }
                    } else if (decoded_load_valid && load_va >= 0xC0000000u &&
                               (m->kernel_vm_ready || is_wince_boot_machine(m))) {
                        if (shadow_tlb_populate_kernel_mapped(m, load_va,
                                                              "KLOAD_KMAPPED",
                                                              (uint32_t)bad_pc)) {
                            m->last_unmapped_valid = false;
                            continue;
                        }
                    }
                }

                if ((uint32_t)bad_pc < 0x80000000u &&
                    m->pending_excode == 0u) {
                    uint32_t fault_va = (uint32_t)((badv != 0u) ? badv : bad_pc);
                    uint32_t inferred_fault_va = 0;
                    bool inferred_fault = false;
                    if (!m->last_unmapped_valid &&
                        infer_mem_access_va_from_pc(m, bad_pc, &inferred_fault_va)) {
                        fault_va = inferred_fault_va;
                        inferred_fault = true;
                    }
                    if (m->cfg.trace_user_handoff &&
                        m->execve_user_handoff_pc != 0u) {
                        static uint32_t handoff_user_tlbl_diag_log = 0;
                        if (handoff_user_tlbl_diag_log < 64) {
                            uint32_t insn = 0xFFFFFFFFu;
                            (void)read_insn_best_effort(m->uc, bad_pc, &insn);
                            uint64_t at = 0, v0 = 0, v1 = 0, a0 = 0, a1 = 0, a2 = 0, a3 = 0;
                            uint64_t gp = 0, sp = 0, t9 = 0, ra = 0;
                            uc_reg_read(m->uc, UC_MIPS_REG_AT, &at);
                            uc_reg_read(m->uc, UC_MIPS_REG_V0, &v0);
                            uc_reg_read(m->uc, UC_MIPS_REG_V1, &v1);
                            uc_reg_read(m->uc, UC_MIPS_REG_A0, &a0);
                            uc_reg_read(m->uc, UC_MIPS_REG_A1, &a1);
                            uc_reg_read(m->uc, UC_MIPS_REG_A2, &a2);
                            uc_reg_read(m->uc, UC_MIPS_REG_A3, &a3);
                            uc_reg_read(m->uc, UC_MIPS_REG_GP, &gp);
                            uc_reg_read(m->uc, UC_MIPS_REG_SP, &sp);
                            uc_reg_read(m->uc, UC_MIPS_REG_T9, &t9);
                            uc_reg_read(m->uc, UC_MIPS_REG_RA, &ra);
                            fprintf(stderr,
                                    "[USER_TLBL_DIAG] pc=0x%08" PRIX64
                                    " insn=0x%08X badv_in=0x%08" PRIX64
                                    " fault_va=0x%08X last_unmapped_valid=%u"
                                    " last_unmapped=0x%08" PRIX64 " type=%d"
                                    " inferred=%u inferred_va=0x%08X"
                                    " handoff_pc=0x%08" PRIX64 " state=%u"
                                    " at=0x%08" PRIX64 " v0=0x%08" PRIX64
                                    " v1=0x%08" PRIX64 " a0=0x%08" PRIX64
                                    " a1=0x%08" PRIX64 " a2=0x%08" PRIX64
                                    " a3=0x%08" PRIX64 " gp=0x%08" PRIX64
                                    " sp=0x%08" PRIX64 " t9=0x%08" PRIX64
                                    " ra=0x%08" PRIX64 "\n",
                                    (uint64_t)(uint32_t)bad_pc, insn,
                                    (uint64_t)(uint32_t)badv, fault_va,
                                    m->last_unmapped_valid ? 1u : 0u,
                                    m->last_unmapped_valid ?
                                    (uint64_t)(uint32_t)m->last_unmapped_addr : 0u,
                                    (int)fault_type,
                                    inferred_fault ? 1u : 0u,
                                    inferred_fault_va,
                                    (uint64_t)(uint32_t)m->execve_user_handoff_pc,
                                    (unsigned)m->execve_user_handoff_state,
                                    (uint64_t)(uint32_t)at, (uint64_t)(uint32_t)v0,
                                    (uint64_t)(uint32_t)v1, (uint64_t)(uint32_t)a0,
                                    (uint64_t)(uint32_t)a1, (uint64_t)(uint32_t)a2,
                                    (uint64_t)(uint32_t)a3, (uint64_t)(uint32_t)gp,
                                    (uint64_t)(uint32_t)sp, (uint64_t)(uint32_t)t9,
                                    (uint64_t)(uint32_t)ra);
                            handoff_user_tlbl_diag_log++;
                        }
                    }
                    if (m->execve_user_handoff_active &&
                        m->execve_user_handoff_state == EXECVE_HANDOFF_STATE_ARMED) {
                        trace_user_handoff_fault_once(m, (uint32_t)bad_pc, fault_va, badv);
                    }
                    bool handoff_entry_window =
                        in_user_handoff_entry_window((uint32_t)bad_pc) &&
                        (m->execve_user_handoff_active ||
                         m->execve_user_handoff_state == EXECVE_HANDOFF_STATE_USER_FETCH_SEEN ||
                         m->execve_user_handoff_state == EXECVE_HANDOFF_STATE_DONE);
                    if (handoff_entry_window) {
                        bool entry_bytes_valid = trace_user_handoff_entry_probe(
                            m, "FAULT_RETRY", (uint32_t)bad_pc, fault_va, badv, false);
                        if (entry_bytes_valid && emulate_load_at_pc(m, bad_pc)) {
                            uint64_t emu_pc = bad_pc;
                            uc_reg_read(m->uc, UC_MIPS_REG_PC, &emu_pc);
                            if ((uint32_t)emu_pc < 0x80000000u)
                                m->execve_user_handoff_pc = (uint32_t)emu_pc;
                            static uint32_t handoff_fault_retry_log = 0;
                            if (handoff_fault_retry_log < 96) {
                                fprintf(stderr,
                                        "[USER_HANDOFF_FAULT_RETRY_LOAD] pc=0x%08" PRIX64
                                        " fault_va=0x%08X -> emu_pc=0x%08" PRIX64 "\n",
                                        (uint64_t)(uint32_t)bad_pc,
                                        fault_va,
                                        (uint64_t)(uint32_t)emu_pc);
                                handoff_fault_retry_log++;
                            }
                            m->last_unmapped_valid = false;
                            continue;
                        }
                    }
                    uint32_t asid = (uint32_t)(m->shadow_cp0_entryhi_live_valid
                                     ? m->shadow_cp0_entryhi_live
                                     : m->shadow_cp0_entryhi) & 0xFFu;
                    uint32_t ctx_base = (uint32_t)m->shadow_cp0_context & 0xFF800000u;
                    uint32_t ctx_badvpn2 = ((fault_va >> 9) & 0x007FFFF0u);
                    m->shadow_cp0_badvaddr = (uint64_t)fault_va;
                    m->shadow_cp0_entryhi = (uint64_t)((fault_va & 0xFFFFE000u) | asid);
                    m->shadow_cp0_context = (uint64_t)(ctx_base | ctx_badvpn2);

                    m->pending_epc          = (uint32_t)bad_pc;
                    m->pending_excode       = MIPS_EXCCODE_TLBL;
                    m->pending_cause        = (uint32_t)(MIPS_EXCCODE_TLBL << 2);
                    m->epc_was_written      = false;
                    m->pending_cause_served = false;
                    m->pending_epc_served   = false;

                    uint64_t ex_status = 0;
                    uc_reg_read(m->uc, UC_MIPS_REG_CP0_STATUS, &ex_status);
                    ex_status |= 0x2u;  /* EXL=1 */
                    uc_reg_write(m->uc, UC_MIPS_REG_CP0_STATUS, &ex_status);

                    uint64_t vec = (ex_status & 0x00400000u) ?
                                   mips_sext(0xBFC00380u) :
                                   mips_sext(0x80000180u);
                    uc_reg_write(m->uc, UC_MIPS_REG_PC, &vec);

                    static uint32_t user_tlbl_inject_log = 0;
                    if (user_tlbl_inject_log < 128) {
                        fprintf(stderr,
                                "[USER_TLBL_INJECT] pc=0x%016" PRIX64
                                " badv=0x%016" PRIX64
                                " pc32=0x%08" PRIX64 " badv32=0x%08X"
                                " entryhi=0x%08X ctx=0x%08X vec=0x%08" PRIX64
                                " status=0x%08" PRIX64 "\n",
                                (uint64_t)bad_pc,
                                (uint64_t)badv,
                                (uint64_t)(uint32_t)bad_pc,
                                fault_va,
                                (uint32_t)m->shadow_cp0_entryhi,
                                (uint32_t)m->shadow_cp0_context,
                                (uint64_t)(uint32_t)vec,
                                (uint64_t)(uint32_t)ex_status);
                        user_tlbl_inject_log++;
                    }
                    m->last_unmapped_valid = false;
                    continue;
                }
                m->last_unmapped_valid = false;
                uc_reg_read(m->uc, UC_MIPS_REG_AT, &at);
                uc_reg_read(m->uc, UC_MIPS_REG_A0, &a0);
                uc_reg_read(m->uc, UC_MIPS_REG_A1, &a1);
                uc_reg_read(m->uc, UC_MIPS_REG_A2, &a2);
                uc_reg_read(m->uc, UC_MIPS_REG_K0, &k0);
                uc_reg_read(m->uc, UC_MIPS_REG_K1, &k1);
                uc_reg_read(m->uc, UC_MIPS_REG_T2, &t2);
                uc_reg_read(m->uc, UC_MIPS_REG_SP, &sp);
                uc_reg_read(m->uc, UC_MIPS_REG_S0, &s0);
                uc_reg_read(m->uc, UC_MIPS_REG_S1, &s1);
                uc_reg_read(m->uc, UC_MIPS_REG_S2, &s2);
                uc_reg_read(m->uc, UC_MIPS_REG_S3, &s3);
                uc_reg_read(m->uc, UC_MIPS_REG_S4, &s4);
                uc_reg_read(m->uc, UC_MIPS_REG_S5, &s5);
                uc_reg_read(m->uc, UC_MIPS_REG_S6, &s6);
                uc_reg_read(m->uc, UC_MIPS_REG_S7, &s7);
                uc_reg_read(m->uc, UC_MIPS_REG_GP, &gp);
                uc_reg_read(m->uc, UC_MIPS_REG_FP, &fp);
                uc_reg_read(m->uc, UC_MIPS_REG_RA, &ra);
                uint64_t candidates[] = {
                    bad_pc, badv, at, a0, a1, a2, k0, k1, t2, sp,
                    s0, s1, s2, s3, s4, s5, s6, s7, gp, fp, ra
                };
                const char *names[] = {
                    "pc", "badv", "at", "a0", "a1", "a2", "k0", "k1", "t2", "sp",
                    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "gp", "fp", "ra"
                };
                bool mapped_any = false;

                for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++) {
                    uint64_t va = candidates[i];
                    uint32_t va32 = (uint32_t)va;
                    if (va32 < 0x1000u)
                        continue;
                    uint64_t block = va & ~((uint64_t)0xFFFFF);
                    uint64_t block32 = (uint64_t)(va32 & ~0xFFFFFu);
                    bool mapped = false;

                    if (va32 >= 0x80000000u && va32 <= 0xBFFFFFFFu) {
                        mapped = map_kseg_mirror_block(m, block);
                    } else if (va32 < 0x80000000u) {
                        /* When kernel VM is ready and kuseg gaps are unmapped,
                         * do NOT remap kuseg addresses — they must go through
                         * TLB/page-fault injection for demand paging. */
                        if (m->kernel_vm_ready && m->kuseg_gaps_unmapped)
                            continue;
                        /*
                         * User-space VA alias handling in Unicorn MIPS64:
                         * faults may surface with either raw 64-bit VA (e.g.
                         * 0xFFFFFFFF0102xxxx) or canonical 32-bit form
                         * (0x000000000102xxxx).  Map both 1MB aliases.
                         */
                        uc_err me32 = uc_mem_map(m->uc, block32, 0x100000,
                                                 UC_PROT_READ | UC_PROT_WRITE);
                        if (me32 == UC_ERR_OK || me32 == UC_ERR_MAP)
                            mapped = true;

                        if (block != block32) {
                            uc_err meraw = uc_mem_map(m->uc, block, 0x100000,
                                                      UC_PROT_READ | UC_PROT_WRITE);
                            if (meraw == UC_ERR_OK || meraw == UC_ERR_MAP)
                                mapped = true;
                        }
                    }

                    if (mapped) {
                        if (!mapped_any) {
                            uint32_t bad_insn = 0xFFFFFFFFu;
                            read_insn_best_effort(m->uc, bad_pc, &bad_insn);
                            fprintf(stderr,
                                    "[MACHINE] read-unmapped recovery at PC=0x%08" PRIX64
                                    " badv=0x%08" PRIX64
                                    " type=%d insn=0x%08X\n",
                                    (uint64_t)(uint32_t)bad_pc,
                                    (uint64_t)(uint32_t)badv,
                                    (int)fault_type,
                                    bad_insn);
                        }
                        fprintf(stderr,
                                "[MACHINE]   mapped block 0x%08" PRIX64 " via $%s=0x%08" PRIX64 "\n",
                                (uint64_t)(uint32_t)block32, names[i], (uint64_t)(uint32_t)va);
                        mapped_any = true;
                    }
                }

                if (mapped_any)
                    continue;
            }

            /*
             * UC_ERR_FETCH_UNMAPPED: instruction fetch from unmapped memory.
             * For user-mode VAs (< 0x80000000), this is a TLBL on code fetch.
             * The faulting VA is bad_pc itself (the PC we tried to fetch from).
             *
             * Two cases:
             * (a) kernel_vm_ready and shadow TLB has a valid entry: populate
             *     the kuseg flat mapping from SDRAM and retry.
             * (b) No TLB entry: inject TLBL so the kernel's page fault handler
             *     demand-loads the code page from the ELF/ramdisk.
             */
            if (err == UC_ERR_FETCH_UNMAPPED) {
                uint32_t fetch_va = (uint32_t)bad_pc;
                if (m->kernel_vm_ready && fetch_va < 0x80000000u) {
                    tlb_lookup_result_t rd = shadow_tlb_lookup(m, fetch_va);
                    if (rd.hit && rd.confident) {
                        /* Valid TLB entry — populate kuseg from SDRAM and retry */
                        shadow_tlb_populate(m, fetch_va, true,
                                            "FETCH_POPULATE",
                                            fetch_va);
                        m->last_unmapped_valid = false;
                        static uint32_t fetch_pop_log = 0;
                        if (fetch_pop_log < 128) {
                            fprintf(stderr,
                                    "[FETCH_POPULATE] va=0x%08X pa=0x%08" PRIX64 "\n",
                                    fetch_va, rd.pa_page);
                            fetch_pop_log++;
                        }
                        continue;
                    }
                    /* No TLB entry → inject TLBL for the code page */
                    uint32_t asid = (uint32_t)(m->shadow_cp0_entryhi_live_valid
                                     ? m->shadow_cp0_entryhi_live
                                     : m->shadow_cp0_entryhi) & 0xFFu;
                    uint32_t ctx_base = (uint32_t)m->shadow_cp0_context & 0xFF800000u;
                    uint32_t ctx_badvpn2 = ((fetch_va >> 9) & 0x007FFFF0u);
                    m->shadow_cp0_badvaddr = (uint64_t)fetch_va;
                    m->shadow_cp0_entryhi = (uint64_t)((fetch_va & 0xFFFFE000u) | asid);
                    m->shadow_cp0_context = (uint64_t)(ctx_base | ctx_badvpn2);

                    m->pending_epc          = fetch_va;
                    m->pending_excode       = MIPS_EXCCODE_TLBL;
                    m->pending_cause        = (uint32_t)(MIPS_EXCCODE_TLBL << 2);
                    m->epc_was_written      = false;
                    m->pending_cause_served = false;
                    m->pending_epc_served   = false;

                    uint64_t ex_status = 0;
                    uc_reg_read(m->uc, UC_MIPS_REG_CP0_STATUS, &ex_status);
                    bool was_exl = (ex_status & 0x2u) != 0u;
                    ex_status |= 0x2u;  /* EXL=1 */
                    uc_reg_write(m->uc, UC_MIPS_REG_CP0_STATUS, &ex_status);

                    /* TLB refill vector (EXL was 0) or general vector (EXL was 1) */
                    uint64_t vec = was_exl ?
                                   mips_sext(0x80000180u) :
                                   mips_sext(0x80000000u);
                    uc_reg_write(m->uc, UC_MIPS_REG_PC, &vec);

                    static uint32_t fetch_tlbl_log = 0;
                    if (fetch_tlbl_log < 128) {
                        fprintf(stderr,
                                "[FETCH_TLBL_INJECT] fetch_va=0x%08X"
                                " entryhi=0x%08X ctx=0x%08X"
                                " vec=0x%08" PRIX64 " status=0x%08" PRIX64
                                " was_exl=%u\n",
                                fetch_va,
                                (uint32_t)m->shadow_cp0_entryhi,
                                (uint32_t)m->shadow_cp0_context,
                                (uint64_t)(uint32_t)vec,
                                (uint64_t)(uint32_t)ex_status,
                                was_exl ? 1u : 0u);
                        fetch_tlbl_log++;
                    }
                    m->last_unmapped_valid = false;
                    continue;
                }
                /* Non-user or pre-VM: log and fall through to generic handler */
                static uint32_t fetch_unmapped_log = 0;
                if (fetch_unmapped_log < 16) {
                    fprintf(stderr,
                            "[FETCH_UNMAPPED] pc=0x%08" PRIX64
                            " kernel_vm_ready=%u\n",
                            (uint64_t)(uint32_t)bad_pc,
                            m->kernel_vm_ready ? 1u : 0u);
                    fetch_unmapped_log++;
                }
            }

            /*
             * UC_ERR_READ_UNALIGNED: a guest load instruction tried to read
             * from a misaligned address (e.g., lw from addr & 3 != 0).
             * On real MIPS hardware this would cause an Address Error exception
             * (IntCode=4).  Unicorn raises UC_ERR_READ_UNALIGNED instead of
             * routing to the exception vector.
             *
             * Recovery: clear the synthetic exception state (the unaligned
             * access probably came from our injected SYSCALL path), advance
             * PC by 4 past the faulting instruction, and continue.  This
             * lets the kernel try the next execve path instead of crashing.
             */
            if (err == UC_ERR_READ_UNALIGNED) {
                static uint32_t unaligned_log = 0;
                if (unaligned_log < 8) {
                    fprintf(stderr,
                            "[MACHINE] UC_ERR_READ_UNALIGNED at PC=0x%016" PRIX64
                            " — skipping faulting insn, clearing synthetic state\n",
                            bad_pc);
                    unaligned_log++;
                }
                /* Clear synthetic exception bookkeeping so subsequent
                 * SYSCALL injections start fresh. */
                clear_synthetic_syscall_state(m, true);
                m->has_saved_exception  = false;
                /* Advance past the faulting instruction. */
                uint64_t skip_pc = bad_pc + 4u;
                uc_reg_write(m->uc, UC_MIPS_REG_PC, &skip_pc);
                /* Clear EXL so the kernel doesn't stay stuck in exception mode. */
                uint64_t cur_status = 0;
                uc_reg_read(m->uc, UC_MIPS_REG_CP0_STATUS, &cur_status);
                cur_status &= ~(uint64_t)0x2u;
                uc_reg_write(m->uc, UC_MIPS_REG_CP0_STATUS, &cur_status);
                continue;
            }

            uint64_t status = 0;
            uint32_t insn = 0;
            uc_reg_read(m->uc, UC_MIPS_REG_CP0_STATUS, &status);
            if (!read_insn_best_effort(m->uc, bad_pc, &insn))
                insn = 0xFFFFFFFFu;
            fprintf(stderr,
                    "[MACHINE] uc_emu_start error at PC=0x%016" PRIX64 ": %s\n",
                    bad_pc, uc_strerror(err));
            fprintf(stderr,
                    "[MACHINE] pending_excode=%u pending_cause=0x%08X pending_epc=0x%016" PRIX64 "\n",
                    m->pending_excode, m->pending_cause, m->pending_epc);
            /* Dump all 32 GPRs to help identify the faulting address */
            static const char *gpr_names[] = {
                "zero","at","v0","v1","a0","a1","a2","a3",
                "t0","t1","t2","t3","t4","t5","t6","t7",
                "s0","s1","s2","s3","s4","s5","s6","s7",
                "t8","t9","k0","k1","gp","sp","fp","ra"
            };
            for (int r = 0; r < 32; r++) {
                uint64_t val = 0;
                uc_reg_read(m->uc, UC_MIPS_REG_0 + r, &val);
                fprintf(stderr, "  $%-4s = 0x%016" PRIX64 "\n", gpr_names[r], val);
            }
            fprintf(stderr,
                    "[MACHINE] STATUS=0x%016" PRIX64 " INSN=0x%08X\n",
                    status, insn);
            m->running = false;

    ui_destroy(m);
            break;
        }

        if (!m->cfg.trace)
            m->insn_count += step_count;
    }

    ui_destroy(m);
    fprintf(stderr, "[MACHINE] Stopped after %" PRIu64 " instructions\n",
            m->insn_count);
}

void machine_stop(machine_t *m)
{
    if (!m)
        return;
    m->running = false;
    uc_emu_stop(m->uc);
}
