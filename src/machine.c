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

/* CP0 Cause ExcCode values */
#define MIPS_EXCCODE_TLBL 2u
#define MIPS_EXCCODE_TLBS 3u
#define MIPS_EXCCODE_SYS  8u

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
    m->cp0_count_ticks++;
    m->last_exec_pc = address;
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
            if (name != NULL &&
                cp0_is_tlb_diag_reg(m->cp0_readback_rd, m->cp0_readback_sel) &&
                tlb_trace_window_active(m) &&
                mfc0_cp0_readback_log < 512) {
                fprintf(stderr,
                        "[MFC0_CP0] rd=%s rt=$%u val=0x%08" PRIX64
                        " mfc0_pc=0x%08" PRIX64 " next_pc=0x%08" PRIX64 "\n",
                        name, (unsigned)m->cp0_readback_rt,
                        (uint64_t)(uint32_t)val,
                        (uint64_t)(uint32_t)(m->cp0_readback_next_pc - 4u),
                        (uint64_t)(uint32_t)address);
                mfc0_cp0_readback_log++;
            }
        }
        m->cp0_readback_pending = false;
    }

    uint32_t insn = 0;
    if (!read_insn_best_effort(uc, address, &insn))
        return;

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
            uint32_t orig_pfn = ((uint32_t)val >> 6) & 0xFFFFFu;
            uint32_t flags = (uint32_t)val & 0x3Fu;
            uint64_t corrected = (uint64_t)(((orig_pfn >> 2) << 6) | flags);
            /* Save original GPR value, write corrected, restore after MTC0 */
            m->pending_gpr_restore = true;
            m->pending_gpr_reg = UC_MIPS_REG_0 + (int)rt;
            m->pending_gpr_val = val;
            uc_reg_write(uc, UC_MIPS_REG_0 + (int)rt, &corrected);
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
        uint64_t ctx = (uint64_t)(uint32_t)m->shadow_cp0_context;
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
    if ((insn & 0xFFE0FFFFu) == 0x40807000u && m->pending_excode != 0) {
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
        if (m->pending_excode == 0 && m->has_saved_exception) {
            restore_pending_exception(m);
            return;
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
                uint64_t fault_pc = m->tlb_defer_fault_pc;
                uint64_t status = status_snapshot & ~(uint64_t)0x2u;  /* clear EXL */
                uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &status);
                uc_reg_write(uc, UC_MIPS_REG_PC, &fault_pc);
                /* Do NOT re-arm pending_cause_served / pending_epc_served.
                 * TLB refill handlers don't read Cause/EPC. */
                static uint32_t eret_tlb_pass_log = 0;
                if (eret_tlb_pass_log < 64) {
                    fprintf(stderr,
                            "[ERET_TLB_PASSTHROUGH] fault_pc=0x%08" PRIX64
                            " pending_epc=0x%08" PRIX64 " nr=%u\n",
                            (uint64_t)(uint32_t)fault_pc,
                            (uint64_t)(uint32_t)m->pending_epc,
                            m->pending_syscall_nr);
                    eret_tlb_pass_log++;
                }
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

static void intr_hook(uc_engine *uc, uint32_t intno, void *user_data)
{
    machine_t *m = user_data;
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
     * User-space exception at a kuseg PC: route to kernel exception handler.
     * intno=12 can be RI (Reserved Instruction) or CpU (Coprocessor Unusable).
     * Set EPC = current PC, EXL = 1, redirect to general exception vector.
     */
    if ((uint32_t)pc < 0x80000000u && intno == 12u) {
        uint64_t user_epc = pc;
        uint64_t exc_status = status | 0x2u;  /* set EXL */
        uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &exc_status);
        /* Store EPC for the kernel handler */
        uint64_t exc_vec = 0x80000180u;
        uc_reg_write(uc, UC_MIPS_REG_PC, &exc_vec);
        static uint32_t user_exc_log = 0;
        if (user_exc_log < 64) {
            fprintf(stderr,
                    "[USER_EXCEPTION] intno=%u pc=0x%08" PRIX64
                    " -> exception vector 0x80000180\n",
                    intno, (uint64_t)(uint32_t)pc);
            user_exc_log++;
        }
        /* Set synthetic exception state so the kernel can read EPC/Cause */
        m->pending_epc = user_epc;
        m->pending_excode = 10u;  /* RI (Reserved Instruction) — approximate */
        m->pending_cause = (uint32_t)(10u << 2);
        m->epc_was_written = false;
        m->pending_cause_served = false;
        m->pending_epc_served = false;
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
                        /*
                         * Repeated intno=26 notifications at the same owner_epc are
                         * usually stale duplicates after we already forced one refill
                         * entry; dropping those avoids SYSCALL+4 livelock.
                         *
                         * Keep intno=27 (store-side) on the retry path so the kernel
                         * still gets a chance to establish writable mappings.
                         */
                        if (intno == 26u) {
                            /*
                             * TLB load-miss notification during do_execve.
                             * The guest TLB has no entry for the kuseg page,
                             * so Unicorn would re-notify forever if we just
                             * restored PC.  Emulate the load instruction
                             * directly (bypassing the guest TLB), advancing
                             * past it so execution continues.
                             */
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
                            /* If load emulation fails, fall through to
                             * TLB_DEFER_SKIP (restore PC only). */
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
                     * deep inside do_execve.  The notification PC is in kseg0
                     * (which never uses TLB), so there is no real TLB miss to
                     * handle.  Running the TLB refill handler would fill a
                     * garbage entry (wrong BadVAddr) and loop forever.
                     *
                     * Fix: restore PC to the last instruction actually executed
                     * (tracked by prid_hook) and let execution continue.  The
                     * synthetic SYSCALL state is preserved.
                     */
                    m->tlb_defer_count++;
                    {
                        uint64_t real_pc = m->last_exec_pc;
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
                    save_pending_exception(m);
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
                save_pending_exception(m);
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
    {
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
    m->wince_region_nz2z_logs = 0;
    m->wince_deferred_seed_done = false;
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
                    " cb=0x%08X-0x%08X objptr=0x%08X-0x%08X obj=0x%08X-0x%08X gate=0x%08X-0x%08X"
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
            fprintf(stderr, "[PROGRESS] insns=%" PRIu64 "M  PC=0x%08" PRIX64 "\n",
                    m->insn_count / 1000000, (uint64_t)pc32);

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

        uint64_t run_pc = 0;
        uc_reg_read(m->uc, UC_MIPS_REG_PC, &run_pc);
        uint32_t step_count = m->post_init_trace_window ? POST_INIT_BATCH_SIZE : BATCH_SIZE;
        uc_err err = uc_emu_start(m->uc, run_pc, 0, 0, step_count);

        if (err != UC_ERR_OK) {
            uint64_t bad_pc = 0;
            uc_reg_read(m->uc, UC_MIPS_REG_PC, &bad_pc);
            if (err == UC_ERR_WRITE_UNMAPPED) {
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
                 * Some Unicorn builds repeatedly report WRITE_UNMAPPED on
                 * the same store without completing it even after mapping.
                 * Decode/commit simple stores directly to break the livelock.
                 */
                if (emulate_store_nearby_on_write_unmapped(m, bad_pc)) {
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
                if (m->last_unmapped_valid) {
                    badv = m->last_unmapped_addr;
                    fault_type = m->last_unmapped_type;
                } else if (m->shadow_cp0_badvaddr != 0) {
                    badv = m->shadow_cp0_badvaddr;
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
