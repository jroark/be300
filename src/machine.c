#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "machine.h"
#include "bus.h"
#include "loader.h"
#include "macc.h"
#include "ui.h"

/*
 * In MIPS64 mode, kseg0/kseg1 virtual addresses (0x80000000–0xBFFFFFFF)
 * must be sign-extended to 64 bits for Unicorn.  Plain 0x80000000 is kuseg
 * in 64-bit address space; 0xFFFFFFFF80000000 is the correct kseg0 base.
 */
static inline uint64_t mips_sext(uint32_t va32) {
    return (uint64_t)(int32_t)va32;
}

static inline bool is_kseg_va32(uint32_t va32)
{
    uint32_t seg = va32 & 0xE0000000u;
    return seg == 0x80000000u || seg == 0xA0000000u;
}

static inline bool is_wince_boot_cfg(const machine_config_t *cfg)
{
    return cfg != NULL && cfg->nand_path != NULL;
}

static inline bool is_wince_boot_machine(const machine_t *m)
{
    return m != NULL && is_wince_boot_cfg(&m->cfg);
}

static bool read_guest_u32(uc_engine *uc, uint64_t va, uint32_t *out);

static bool sdram_alias_pa_offset(const machine_t *m, uint64_t addr, uint64_t *off_out)
{
    uint64_t sdram_size = (uint64_t)m->cfg.sdram_size;
    uint64_t off = UINT64_MAX;

    if (addr < sdram_size) {
        off = addr;
    } else if (addr >= UINT64_C(0x0000000080000000) &&
               addr < UINT64_C(0x0000000080000000) + sdram_size) {
        off = addr - UINT64_C(0x0000000080000000);
    } else if (addr >= UINT64_C(0x00000000A0000000) &&
               addr < UINT64_C(0x00000000A0000000) + sdram_size) {
        off = addr - UINT64_C(0x00000000A0000000);
    } else if (addr >= UINT64_C(0xFFFFFFFF80000000) &&
               addr < UINT64_C(0xFFFFFFFF80000000) + sdram_size) {
        off = addr - UINT64_C(0xFFFFFFFF80000000);
    } else if (addr >= UINT64_C(0xFFFFFFFFA0000000) &&
               addr < UINT64_C(0xFFFFFFFFA0000000) + sdram_size) {
        off = addr - UINT64_C(0xFFFFFFFFA0000000);
    } else {
        return false;
    }

    if (off_out)
        *off_out = off;
    return true;
}

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

static void cp0_shadow_write(machine_t *m, uint32_t rd, uint32_t sel, uint64_t val)
{
    if (sel != 0)
        return;
    switch (rd) {
    case 0:  m->shadow_cp0_index    = val; break;
    case 2:  m->shadow_cp0_entrylo0 = val; break;
    case 3:  m->shadow_cp0_entrylo1 = val; break;
    case 4:  m->shadow_cp0_context  = val; break;
    case 5:  m->shadow_cp0_pagemask = val; break;
    case 8:  m->shadow_cp0_badvaddr = val; break;
    case 10: m->shadow_cp0_entryhi  = val; break;
    case 14: m->shadow_cp0_epc      = val; break;
    default: break;
    }
}

static inline bool tlb_trace_window_active(const machine_t *m)
{
    return m->tlb_trace_window;
}

/*
 * vmlinux-pgui-demo (2.4.18) init() execve("/linuxrc") site:
 *   0x8000181c syscall
 *   0x80001820 branch on a3 (post-syscall return check)
 */
#define RUN_INIT_SYSCALL_EPC      0x8000181Cu
#define RUN_INIT_SYSCALL_RET_PC   0x80001820u
#define RUN_INIT_SYSCALL_EPC_26   0x800015B0u
#define RUN_INIT_SYSCALL_RET_PC_26 0x800015B4u
#define INIT_ARGV_KPTR            0x80171244u
#define INIT_ENVP_KPTR            0x8017126Cu
#define EXECVE_SCRATCH_BASE       0x01020000u
#define EXECVE_SCRATCH_END        0x01021000u
#define DO_EXECVE_START_PC        0x8004A9D0u
#define DO_EXECVE_END_PC          0x8004ACA4u
#define TLB_DEFER_RETRY_LIMIT     32u
#define TLB_EXL_DROP_DEFER_LIMIT  64u
#define EXECVE_HANDOFF_STATE_DONE          0u
#define EXECVE_HANDOFF_STATE_ARMED         1u
#define EXECVE_HANDOFF_STATE_USER_FETCH_SEEN 2u
#define USER_HANDOFF_ENTRY_WINDOW_START    0x2AAA8A00u
#define USER_HANDOFF_ENTRY_WINDOW_END      0x2AAA8A40u

/* vmlinux-pgui-demo (2.4.18) fault-path symbols (resolved with objdump). */
#define K24_DO_PAGE_FAULT         0x8000AFA8u
#define K24_HANDLE_TLBL           0x800133E0u
#define K24_NOPAGE_TLBL           0x80013464u
#define K24_HANDLE_TLBL_PTE_LOAD  0x80013410u
#define K24_HANDLE_TLBL_PTE_STORE 0x80013430u
#define K24_HANDLE_TLBS           0x80013560u
#define K24_DO_NO_PAGE            0x800283A4u
#define K24_FILEMAP_NOPAGE        0x8002DAB8u
#define K24_FILEMAP_FIND_GET_PAGE_RET   0x8002DBB4u
#define K24_FILEMAP_PAGE_CACHE_READ_RET 0x8002DBECu
#define K24_FILEMAP_READ_CLUSTER_RET    0x8002DC40u
#define K24_FILEMAP_UPTODATE_CHECK      0x8002DC48u
#define K24_FILEMAP_UPTODATE_BRANCH     0x8002DC54u
#define K24_FILEMAP_READPAGE_RET        0x8002DC94u
#define K24_FILEMAP_LOCKPAGE_CALL       0x8002DC5Cu
#define K24_FILEMAP_MAPPING_CHECK       0x8002DC64u
#define K24_FILEMAP_UPTODATE_RECHECK    0x8002DC7Cu
#define K24_FILEMAP_READPAGE_CALL       0x8002DC8Cu
#define K24_FILEMAP_SKIP_READPAGE       0x8002DCC4u
#define K24_BLOCK_READ_SUBMIT_BH_CALL   0x80042ED8u
#define K24_BLOCK_READ_SUBMIT_BH_RET    0x80042EE0u
#define K24_BLOCK_READ_GET_BLOCK_CALL   0x80042FB0u
#define K24_BLOCK_READ_GET_BLOCK_RET    0x80042FB8u
#define K24_BLOCK_READ_MEMSET_HOLE      0x80042FE8u
#define K24_BLOCK_READ_RET              0x80042EECu
#define K24_EXT2_GET_BLOCK_RET          0x8006E730u

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

static inline bool in_user_handoff_entry_window(uint32_t pc)
{
    return (pc >= USER_HANDOFF_ENTRY_WINDOW_START &&
            pc < USER_HANDOFF_ENTRY_WINDOW_END);
}

static inline bool is_handoff_stale_pc(uint32_t pc, uint32_t handoff_pc)
{
    return (pc == 0x80001850u ||
            pc == 0x80000180u ||
            (handoff_pc != 0u && pc == handoff_pc));
}

static bool read_insn_best_effort(uc_engine *uc, uint64_t address, uint32_t *insn);

static inline bool trace_user_handoff_fault_path_active(const machine_t *m)
{
    if (!m->cfg.trace_user_handoff)
        return false;
    if (!m->execve_user_handoff_active ||
        m->execve_user_handoff_state != EXECVE_HANDOFF_STATE_ARMED)
        return false;
    return ((uint32_t)m->shadow_cp0_badvaddr < 0x80000000u);
}

static uint64_t tlb_pair_bytes_from_pagemask(uint32_t pagemask)
{
    uint64_t pair_bytes = ((uint64_t)(pagemask | 0x1FFFu) + 1u);
    if (pair_bytes < 0x2000u)
        pair_bytes = 0x2000u;
    if ((pair_bytes & 0xFFFu) != 0u)
        pair_bytes = (pair_bytes + 0xFFFu) & ~(uint64_t)0xFFFu;
    if ((pair_bytes & (pair_bytes - 1u)) != 0u) {
        uint64_t p2 = 0x2000u;
        while (p2 < pair_bytes && p2 < (1ull << 31))
            p2 <<= 1;
        pair_bytes = p2;
    }
    return pair_bytes;
}

static uint64_t tlb_leaf_bytes_from_pagemask(uint32_t pagemask)
{
    uint64_t pair_bytes = tlb_pair_bytes_from_pagemask(pagemask);
    if (pair_bytes < 0x2000u)
        return 0x1000u;
    return pair_bytes >> 1;
}

static bool tlb_entry_matches_va(uint32_t va, uint32_t entryhi, uint32_t pagemask)
{
    uint64_t pair_bytes = tlb_pair_bytes_from_pagemask(pagemask);
    uint32_t mask = ~((uint32_t)(pair_bytes - 1u));
    return ((va & mask) == (entryhi & mask));
}

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

#define WINCE_TRACE_VEC_PA_START UINT32_C(0x00000000)
#define WINCE_TRACE_VEC_PA_END   UINT32_C(0x00000400)
#define WINCE_TRACE_CTX_PA_START UINT32_C(0x00002000)
#define WINCE_TRACE_CTX_PA_END   UINT32_C(0x00002400)
#define WINCE_TRACE_CB_PA_START  UINT32_C(0x00051680)
#define WINCE_TRACE_CB_PA_END    UINT32_C(0x00051B00)
#define WINCE_TRACE_OBJ_PA_START UINT32_C(0x0066BFC0)
#define WINCE_TRACE_OBJ_PA_END   UINT32_C(0x00680000)

static inline bool write_value_is_zero(unsigned size, uint64_t value)
{
    if (size >= 8u)
        return value == 0u;
    uint64_t mask = (UINT64_C(1) << (size * 8u)) - 1u;
    return (value & mask) == 0u;
}

static void wince_pa_watch_update(machine_t *m, wince_pa_watch_t *watch,
                                  const char *region, uint32_t pa, uint32_t pc,
                                  unsigned size, uint64_t value)
{
    bool is_zero = write_value_is_zero(size, value);

    watch->writes++;
    if (is_zero)
        watch->zero_writes++;
    else
        watch->saw_nonzero = true;

    if (!watch->first_valid) {
        watch->first_valid = true;
        watch->first_pa = pa;
        watch->first_pc = pc;
        watch->first_size = (uint8_t)size;
        watch->first_value = value;
    }

    watch->last_pa = pa;
    watch->last_pc = pc;
    watch->last_size = (uint8_t)size;
    watch->last_value = value;

    if (m->cfg.log_wince_stall && m->wince_pa_watch_logs < 96u) {
        bool should_log = (watch->writes <= 4u) ||
                          (!is_zero && watch->writes <= 32u);
        if (should_log) {
            fprintf(stderr,
                    "[WINCE_PA_WATCH] region=%s writes=%u zero=%u"
                    " pa=0x%08X pc=0x%08X size=%u val=0x%016" PRIX64 "\n",
                    region, watch->writes, watch->zero_writes,
                    pa, pc, size, value);
            m->wince_pa_watch_logs++;
        }
    }
}

static bool wince_pa_watch_write_hook(uc_engine *uc, uc_mem_type type,
                                      uint64_t address, int size, int64_t value,
                                      void *user_data)
{
    (void)type;
    machine_t *m = user_data;
    if (!m->wince_pa_watch_active)
        return true;
    if (size <= 0 || size > 8)
        return true;
    uint32_t pa = (uint32_t)address & UINT32_C(0x1FFFFFFF);
    if ((uint64_t)pa >= (uint64_t)m->cfg.sdram_size)
        return true;

    uint64_t pc64 = 0;
    uc_reg_read(uc, UC_MIPS_REG_PC, &pc64);
    uint32_t pc = (uint32_t)pc64;
    uint64_t uval = (uint64_t)value;

    if (pa < WINCE_TRACE_VEC_PA_END) {
        wince_pa_watch_update(m, &m->wince_vec_watch, "vectors",
                              pa, pc, (unsigned)size, uval);
    } else if (pa >= WINCE_TRACE_CTX_PA_START && pa < WINCE_TRACE_CTX_PA_END) {
        wince_pa_watch_update(m, &m->wince_ctx_watch, "ctx2200",
                              pa, pc, (unsigned)size, uval);
    } else if (pa >= WINCE_TRACE_CB_PA_START && pa < WINCE_TRACE_CB_PA_END) {
        bool is_zero = write_value_is_zero((unsigned)size, uval);
        m->wince_cb_writes++;
        if (!is_zero)
            m->wince_cb_nonzero = true;
        if (m->cfg.log_wince_stall && m->wince_pa_watch_logs < 192u) {
            fprintf(stderr,
                    "[WINCE_CB_WATCH] writes=%u nonzero=%u"
                    " pa=0x%08X pc=0x%08X size=%u val=0x%016" PRIX64 "\n",
                    m->wince_cb_writes,
                    m->wince_cb_nonzero ? 1u : 0u,
                    pa, pc, (unsigned)size, uval);
            m->wince_pa_watch_logs++;
        }
    } else if (pa >= WINCE_TRACE_OBJ_PA_START && pa < WINCE_TRACE_OBJ_PA_END) {
        bool is_zero = write_value_is_zero((unsigned)size, uval);
        m->wince_obj_writes++;
        if (!is_zero)
            m->wince_obj_nonzero = true;
        if (m->cfg.log_wince_stall && m->wince_pa_watch_logs < 224u) {
            fprintf(stderr,
                    "[WINCE_OBJ_WATCH] writes=%u nonzero=%u"
                    " pa=0x%08X pc=0x%08X size=%u val=0x%016" PRIX64 "\n",
                    m->wince_obj_writes,
                    m->wince_obj_nonzero ? 1u : 0u,
                    pa, pc, (unsigned)size, uval);
            m->wince_pa_watch_logs++;
        }
    }

    return true;
}

static void log_wince_pa_watch_summary(const machine_t *m, const char *reason)
{
    if (!m || !m->wince_pa_watch_active)
        return;

    const wince_pa_watch_t *watches[] = { &m->wince_vec_watch, &m->wince_ctx_watch };
    const char *names[] = { "vectors", "ctx2200" };

    for (int i = 0; i < 2; i++) {
        const wince_pa_watch_t *w = watches[i];
        if (!w->first_valid) {
            fprintf(stderr,
                    "[WINCE_PA_WATCH_SUMMARY] reason=%s region=%s writes=0\n",
                    reason, names[i]);
            continue;
        }
        fprintf(stderr,
                "[WINCE_PA_WATCH_SUMMARY] reason=%s region=%s writes=%u"
                " zero=%u nonzero=%u"
                " first(pc=0x%08X pa=0x%08X size=%u val=0x%016" PRIX64 ")"
                " last(pc=0x%08X pa=0x%08X size=%u val=0x%016" PRIX64 ")\n",
                reason, names[i],
                w->writes, w->zero_writes, w->saw_nonzero ? 1u : 0u,
                w->first_pc, w->first_pa, (unsigned)w->first_size, w->first_value,
                w->last_pc, w->last_pa, (unsigned)w->last_size, w->last_value);
    }
}

static void log_wince_pa_watch_nonzero(const machine_t *m, const char *reason)
{
    if (!m || !m->wince_pa_watch_active)
        return;
    fprintf(stderr,
            "[WINCE_PA_WATCH_NONZERO] reason=%s vectors_nonzero=%u"
            " ctx_nonzero=%u vectors_writes=%u ctx_writes=%u"
            " cb_nonzero=%u cb_writes=%u obj_nonzero=%u obj_writes=%u\n",
            reason,
            m->wince_vec_watch.saw_nonzero ? 1u : 0u,
            m->wince_ctx_watch.saw_nonzero ? 1u : 0u,
            m->wince_vec_watch.writes,
            m->wince_ctx_watch.writes,
            m->wince_cb_nonzero ? 1u : 0u,
            m->wince_cb_writes,
            m->wince_obj_nonzero ? 1u : 0u,
            m->wince_obj_writes);
}

#define WINCE_NK_TRACE_BASE UINT32_C(0x80020000)
#define WINCE_NK_TRACE_END  UINT32_C(0x80F00000)

static inline bool is_wince_nk_pc32(uint32_t pc32)
{
    return pc32 >= WINCE_NK_TRACE_BASE && pc32 < WINCE_NK_TRACE_END;
}

static void wince_ctrl_hist_record(machine_t *m,
                                   uint8_t kind,
                                   uint32_t pc,
                                   uint32_t target,
                                   uint32_t value,
                                   uint8_t reg_idx,
                                   uint32_t ra,
                                   uint32_t sp,
                                   uint32_t a0)
{
    uint32_t idx = m->wince_ctrl_hist_head % WINCE_CTRL_HISTORY_LEN;
    wince_ctrl_hist_entry_t *e = &m->wince_ctrl_hist[idx];
    e->kind = kind;
    e->pc = pc;
    e->target = target;
    e->value = value;
    e->reg_idx = reg_idx;
    e->ra = ra;
    e->sp = sp;
    e->a0 = a0;
    e->reserved = 0;

    m->wince_ctrl_hist_head = (m->wince_ctrl_hist_head + 1u) % WINCE_CTRL_HISTORY_LEN;
    if (m->wince_ctrl_hist_count < WINCE_CTRL_HISTORY_LEN)
        m->wince_ctrl_hist_count++;
}

static void maybe_record_wince_ctrl_event(machine_t *m, uc_engine *uc,
                                          uint32_t pc32, uint32_t insn,
                                          uint32_t op, uint32_t rs, uint32_t rt,
                                          uint32_t rd, uint32_t sel)
{
    if (!is_wince_boot_machine(m) || !m->cfg.log_wince_stall)
        return;
    if (!is_wince_nk_pc32(pc32))
        return;

    uint8_t kind = 0;
    uint8_t reg_idx = 0;
    uint32_t target = 0;
    uint32_t value = 0;
    uint32_t funct = insn & 0x3Fu;

    if (op == 0x03u) { /* JAL */
        kind = 1u;
        target = ((pc32 + 4u) & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2);
    } else if (op == 0u && funct == 0x09u) { /* JALR */
        uint64_t t = 0;
        uc_reg_read(uc, UC_MIPS_REG_0 + (int)rs, &t);
        kind = 2u;
        target = (uint32_t)t;
    } else if (op == 0u && funct == 0x08u && rs == 31u) { /* JR $ra */
        uint64_t ra64 = 0;
        uc_reg_read(uc, UC_MIPS_REG_RA, &ra64);
        kind = 3u;
        target = (uint32_t)ra64;
    } else if (op == 0x10u && rs == 0x04u && rd == 12u && sel == 0u) { /* MTC0 Status */
        uint64_t v = 0;
        uc_reg_read(uc, UC_MIPS_REG_0 + (int)rt, &v);
        kind = 4u;
        reg_idx = (uint8_t)rt;
        value = (uint32_t)v;
        target = (uint32_t)v;
    } else {
        return;
    }

    uint64_t ra64 = 0, sp64 = 0, a064 = 0;
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra64);
    uc_reg_read(uc, UC_MIPS_REG_SP, &sp64);
    uc_reg_read(uc, UC_MIPS_REG_A0, &a064);
    wince_ctrl_hist_record(m, kind, pc32, target, value, reg_idx,
                           (uint32_t)ra64, (uint32_t)sp64, (uint32_t)a064);
}

static void log_wince_ctrl_hist_summary(const machine_t *m,
                                        const char *reason,
                                        uint32_t max_lines)
{
    if (!m || m->wince_ctrl_hist_count == 0)
        return;

    uint32_t emit = m->wince_ctrl_hist_count;
    if (emit > max_lines)
        emit = max_lines;
    uint32_t dropped = m->wince_ctrl_hist_count - emit;
    uint32_t start = (m->wince_ctrl_hist_head + WINCE_CTRL_HISTORY_LEN - emit)
                   % WINCE_CTRL_HISTORY_LEN;

    fprintf(stderr,
            "[WINCE_CTRL_SUMMARY] reason=%s entries=%u emitted=%u dropped=%u\n",
            reason, m->wince_ctrl_hist_count, emit, dropped);
    for (uint32_t i = 0; i < emit; i++) {
        uint32_t idx = (start + i) % WINCE_CTRL_HISTORY_LEN;
        const wince_ctrl_hist_entry_t *e = &m->wince_ctrl_hist[idx];
        const char *kind =
            (e->kind == 1u) ? "JAL" :
            (e->kind == 2u) ? "JALR" :
            (e->kind == 3u) ? "JR_RA" :
            (e->kind == 4u) ? "MTC0_STATUS" : "UNK";
        if (e->kind == 4u) {
            fprintf(stderr,
                    "[WINCE_CTRL] %02u kind=%s pc=0x%08X status=0x%08X"
                    " rt=$%u ra=0x%08X sp=0x%08X a0=0x%08X\n",
                    i, kind, e->pc, e->value, (unsigned)e->reg_idx,
                    e->ra, e->sp, e->a0);
        } else {
            fprintf(stderr,
                    "[WINCE_CTRL] %02u kind=%s pc=0x%08X target=0x%08X"
                    " ra=0x%08X sp=0x%08X a0=0x%08X\n",
                    i, kind, e->pc, e->target, e->ra, e->sp, e->a0);
        }
    }
}

static bool pc_in_wince_ctx_range(uint32_t pc32)
{
    if (pc32 >= 0x80079580u && pc32 <= 0x800798A0u)
        return true;
    if (pc32 >= 0x80032530u && pc32 <= 0x80032790u)
        return true;
    return false;
}

static const char *wince_ctx_key_tag(uint32_t pc32)
{
    switch (pc32) {
    case 0x80079640u: return "ALL_call_5e70";
    case 0x80079648u: return "ALL_call_5fec";
    case 0x80079650u: return "ALL_call_7aaac";
    case 0x80079658u: return "ALL_call_7a65c";
    case 0x80079660u: return "ALL_pre_ctx_call";
    case 0x80079844u: return "ALL_ctx_fn_entry";
    case 0x80079890u: return "ALL_ctx_fn_return";
    case 0x80079668u: return "ALL_after_ctx_fn";
    case 0x80079714u: return "ALL_mtc0_status";
    case 0x80032530u: return "NET_call_16f20";
    case 0x80032538u: return "NET_call_17008";
    case 0x80032540u: return "NET_call_17388";
    case 0x80032548u: return "NET_call_1354C";
    case 0x80032550u: return "NET_pre_ctx_call";
    case 0x80032734u: return "NET_ctx_fn_entry";
    case 0x80032780u: return "NET_ctx_fn_return";
    case 0x80032558u: return "NET_after_ctx_fn";
    case 0x800326C4u: return "NET_mtc0_status";
    default: return NULL;
    }
}

static const char *wince_ctx_phase_tag(uint32_t pc32)
{
    switch (pc32) {
    case 0x80079640u: return "ALL_call_5e70";
    case 0x80079648u: return "ALL_call_5fec";
    case 0x80079650u: return "ALL_call_7aaac";
    case 0x80079658u: return "ALL_call_7a65c";
    case 0x80079660u: return "ALL_pre_ctx_call";
    case 0x80079844u: return "ALL_ctx_fn_entry";
    case 0x80079890u: return "ALL_ctx_fn_return";
    case 0x80079668u: return "ALL_post_ctx_call";
    case 0x80079714u: return "ALL_mtc0_status";
    case 0x80032530u: return "NET_call_16f20";
    case 0x80032538u: return "NET_call_17008";
    case 0x80032540u: return "NET_call_17388";
    case 0x80032548u: return "NET_call_1354C";
    case 0x80032550u: return "NET_pre_ctx_call";
    case 0x80032734u: return "NET_ctx_fn_entry";
    case 0x80032780u: return "NET_ctx_fn_return";
    case 0x80032558u: return "NET_post_ctx_call";
    case 0x800326C4u: return "NET_mtc0_status";
    default: return NULL;
    }
}

static bool wince_ctx_branch_track_slot(uint32_t pc32, uint16_t **count_out)
{
    enum { MAX_BRANCH_SITES = 16 };
    static struct {
        uint32_t pc;
        uint16_t count;
    } sites[MAX_BRANCH_SITES];

    int found = -1;
    int empty = -1;
    for (int i = 0; i < MAX_BRANCH_SITES; i++) {
        if (sites[i].count != 0u && sites[i].pc == pc32) {
            found = i;
            break;
        }
        if (sites[i].count == 0u && empty < 0)
            empty = i;
    }
    if (found < 0) {
        if (empty < 0)
            return false;
        sites[empty].pc = pc32;
        sites[empty].count = 0u;
        found = empty;
    }
    if (count_out)
        *count_out = &sites[found].count;
    return true;
}

static void log_wince_words32(uc_engine *uc, const char *tag,
                              uint32_t base, unsigned words);

static bool ptr_in_sdram(const machine_t *m, uint32_t ptr32)
{
    return ((ptr32 & UINT32_C(0x1FFFFFFF)) < m->cfg.sdram_size);
}

static void log_wince_ctx_snapshot(machine_t *m, uc_engine *uc,
                                   const char *phase, uint32_t pc32)
{
    uint64_t a0 = 0, a1 = 0, t0 = 0, t1 = 0, s0 = 0, s1 = 0, v0 = 0, v1 = 0;
    uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
    uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
    uc_reg_read(uc, UC_MIPS_REG_T0, &t0);
    uc_reg_read(uc, UC_MIPS_REG_T1, &t1);
    uc_reg_read(uc, UC_MIPS_REG_S0, &s0);
    uc_reg_read(uc, UC_MIPS_REG_S1, &s1);
    uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
    uc_reg_read(uc, UC_MIPS_REG_V1, &v1);

    uint32_t a0p = (uint32_t)a0;
    uint32_t a1p = (uint32_t)a1;
    uint32_t t0p = (uint32_t)t0;
    uint32_t t1p = (uint32_t)t1;

    fprintf(stderr,
            "[WINCE_CTX_SNAPSHOT] phase=%s pc=0x%08X"
            " dst(a0)=0x%08X src(a1)=0x%08X alt_t0=0x%08X alt_t1=0x%08X"
            " s0=0x%08X s1=0x%08X v0=0x%08X v1=0x%08X\n",
            phase, pc32, a0p, a1p, t0p, t1p,
            (uint32_t)s0, (uint32_t)s1, (uint32_t)v0, (uint32_t)v1);

    if (ptr_in_sdram(m, a0p))
        log_wince_words32(uc, "dst_a0", a0p, 8u);
    if (ptr_in_sdram(m, a1p))
        log_wince_words32(uc, "src_a1", a1p, 8u);
    if (t0p != a0p && ptr_in_sdram(m, t0p))
        log_wince_words32(uc, "alt_t0", t0p, 8u);
    if (t1p != a1p && ptr_in_sdram(m, t1p))
        log_wince_words32(uc, "alt_t1", t1p, 8u);
    log_wince_words32(uc, "ctx_tbl", 0xA0051680u, 8u);
}

static void log_wince_words32(uc_engine *uc, const char *tag,
                              uint32_t base, unsigned words)
{
    if (words == 0u)
        return;
    if (words > 8u)
        words = 8u;

    fprintf(stderr, "[WINCE_CTX_MEM] %s base=0x%08X", tag, base);
    for (unsigned i = 0; i < words; i++) {
        uint32_t w = 0;
        if (read_guest_u32(uc, (uint64_t)base + (uint64_t)(i * 4u), &w)) {
            fprintf(stderr, " w%u=0x%08X", i, w);
        } else {
            fprintf(stderr, " w%u=????", i);
        }
    }
    fprintf(stderr, "\n");
}

static bool decode_branch_decision(uc_engine *uc, uint32_t pc32, uint32_t insn,
                                   uint32_t op, uint32_t rs, uint32_t rt,
                                   bool *taken_out, uint32_t *target_out,
                                   uint32_t *fallthrough_out,
                                   uint32_t *rs_val_out, uint32_t *rt_val_out)
{
    int32_t imm = (int16_t)(insn & 0xFFFFu);
    uint32_t fallthrough = pc32 + 4u;
    uint32_t target = fallthrough + (uint32_t)(imm << 2);
    uint64_t rs64 = 0, rt64 = 0;
    bool known = true;
    bool taken = false;

    uc_reg_read(uc, UC_MIPS_REG_0 + (int)rs, &rs64);
    uc_reg_read(uc, UC_MIPS_REG_0 + (int)rt, &rt64);
    uint32_t rs_val = (uint32_t)rs64;
    uint32_t rt_val = (uint32_t)rt64;

    switch (op) {
    case 0x04u: /* BEQ */
        taken = (rs_val == rt_val);
        break;
    case 0x05u: /* BNE */
        taken = (rs_val != rt_val);
        break;
    case 0x06u: /* BLEZ */
        taken = ((int32_t)rs_val <= 0);
        break;
    case 0x07u: /* BGTZ */
        taken = ((int32_t)rs_val > 0);
        break;
    case 0x01u: /* REGIMM */
        switch (rt) {
        case 0x00u: /* BLTZ */
        case 0x10u: /* BLTZAL */
            taken = ((int32_t)rs_val < 0);
            break;
        case 0x01u: /* BGEZ */
        case 0x11u: /* BGEZAL */
            taken = ((int32_t)rs_val >= 0);
            break;
        default:
            known = false;
            break;
        }
        break;
    default:
        known = false;
        break;
    }

    if (!known)
        return false;
    if (taken_out) *taken_out = taken;
    if (target_out) *target_out = target;
    if (fallthrough_out) *fallthrough_out = fallthrough;
    if (rs_val_out) *rs_val_out = rs_val;
    if (rt_val_out) *rt_val_out = rt_val;
    return true;
}

static void maybe_probe_wince_ctx_path(machine_t *m, uc_engine *uc,
                                       uint32_t pc32, uint32_t insn,
                                       uint32_t op, uint32_t rs,
                                       uint32_t rt, uint32_t rd, uint32_t sel)
{
    if (!is_wince_boot_machine(m) || !m->cfg.log_wince_stall)
        return;
    if (!pc_in_wince_ctx_range(pc32))
        return;
    if (m->wince_ctx_probe_logs >= 1024u)
        return;

    const char *tag = wince_ctx_key_tag(pc32);
    bool is_key = (tag != NULL);
    bool is_branch = (op == 0x01u || op == 0x04u || op == 0x05u ||
                      op == 0x06u || op == 0x07u);
    bool is_mtc0_status = (op == 0x10u && rs == 0x04u && rd == 12u && sel == 0u);

    if (!is_key && !is_branch && !is_mtc0_status)
        return;

    if (is_branch) {
        uint16_t *site_count = NULL;
        if (!wince_ctx_branch_track_slot(pc32, &site_count))
            return;
        if (site_count == NULL || *site_count >= 8u)
            return;

        bool taken = false;
        uint32_t target = 0, fallthrough = 0, rs_val = 0, rt_val = 0;
        if (decode_branch_decision(uc, pc32, insn, op, rs, rt,
                                   &taken, &target, &fallthrough,
                                   &rs_val, &rt_val)) {
            fprintf(stderr,
                    "[WINCE_CTX_BRANCH] pc=0x%08X insn=0x%08X"
                    " rs=$%u:0x%08X rt=$%u:0x%08X taken=%u"
                    " target=0x%08X fallthrough=0x%08X\n",
                    pc32, insn, rs, rs_val, rt, rt_val,
                    taken ? 1u : 0u, target, fallthrough);
            m->wince_ctx_probe_logs++;
            (*site_count)++;
        }
        return;
    }

    if (is_key || is_mtc0_status) {
        uint64_t ra = 0, sp = 0, a0 = 0, a1 = 0, a2 = 0, a3 = 0;
        uint64_t t0 = 0, t1 = 0, v0 = 0, v1 = 0, s0 = 0, s1 = 0;
        uint64_t status = 0;
        uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
        uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
        uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
        uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
        uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
        uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
        uc_reg_read(uc, UC_MIPS_REG_T0, &t0);
        uc_reg_read(uc, UC_MIPS_REG_T1, &t1);
        uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
        uc_reg_read(uc, UC_MIPS_REG_V1, &v1);
        uc_reg_read(uc, UC_MIPS_REG_S0, &s0);
        uc_reg_read(uc, UC_MIPS_REG_S1, &s1);
        uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &status);

        const char *name = tag ? tag : "MTC0_STATUS";
        fprintf(stderr,
                "[WINCE_CTX_PROBE] tag=%s pc=0x%08X insn=0x%08X"
                " ra=0x%08X sp=0x%08X"
                " a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X"
                " t0=0x%08X t1=0x%08X s0=0x%08X s1=0x%08X"
                " v0=0x%08X v1=0x%08X status=0x%08X\n",
                name, pc32, insn,
                (uint32_t)ra, (uint32_t)sp,
                (uint32_t)a0, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3,
                (uint32_t)t0, (uint32_t)t1, (uint32_t)s0, (uint32_t)s1,
                (uint32_t)v0, (uint32_t)v1, (uint32_t)status);
        m->wince_ctx_probe_logs++;

        const char *phase = wince_ctx_phase_tag(pc32);
        if (phase != NULL) {
            log_wince_ctx_snapshot(m, uc, phase, pc32);
            m->wince_ctx_probe_logs += 4u;
        }

        if (m->wince_ctx_probe_logs >= 1024u)
            return;
        if (((uint32_t)a0 & 0x1FFFFFFFu) < m->cfg.sdram_size)
            log_wince_words32(uc, "a0_ptr", (uint32_t)a0, 8u);
        if (((uint32_t)t0 & 0x1FFFFFFFu) < m->cfg.sdram_size &&
            ((uint32_t)t0 != (uint32_t)a0))
            log_wince_words32(uc, "t0_ptr", (uint32_t)t0, 8u);
        log_wince_words32(uc, "ctx2200", 0xA0002200u, 8u);
        log_wince_words32(uc, "vec0000", 0xA0000000u, 8u);
        m->wince_ctx_probe_logs += 4u;
    }
}

static void alias_coherence_probe(machine_t *m)
{
    if (m->shared_alias_active) {
        if (m->cfg.sdram_size < 0x200u) {
            fprintf(stderr, "[ALIAS_COHERENCE] fail mode=shared reason=sdram_too_small\n");
            return;
        }

        uint32_t probe_pa32 = (m->cfg.sdram_size - 0x100u) & ~UINT32_C(0x3);
        uint64_t pa = probe_pa32;
        uint64_t kseg0 = UINT64_C(0x0000000080000000) + probe_pa32;
        uint64_t kseg1 = UINT64_C(0x00000000A0000000) + probe_pa32;
        uint32_t orig = 0, pa_val = 0, kseg0_val = 0, kseg1_val = 0;
        const uint32_t pattern = UINT32_C(0xA55A3CC3);

        uc_err e_orig = uc_mem_read(m->uc, pa, &orig, sizeof(orig));
        uc_err e_w = uc_mem_write(m->uc, kseg1, &pattern, sizeof(pattern));
        uc_err e_pa = uc_mem_read(m->uc, pa, &pa_val, sizeof(pa_val));
        uc_err e_k0 = uc_mem_read(m->uc, kseg0, &kseg0_val, sizeof(kseg0_val));
        uc_err e_k1 = uc_mem_read(m->uc, kseg1, &kseg1_val, sizeof(kseg1_val));
        if (e_orig == UC_ERR_OK) {
            uc_mem_write(m->uc, pa, &orig, sizeof(orig));
            uc_mem_write(m->uc, kseg0, &orig, sizeof(orig));
            uc_mem_write(m->uc, kseg1, &orig, sizeof(orig));
        }

        bool pass = (e_orig == UC_ERR_OK &&
                     e_w == UC_ERR_OK &&
                     e_pa == UC_ERR_OK &&
                     e_k0 == UC_ERR_OK &&
                     e_k1 == UC_ERR_OK &&
                     pa_val == pattern &&
                     kseg0_val == pattern &&
                     kseg1_val == pattern);
        fprintf(stderr,
                "[ALIAS_COHERENCE] %s mode=shared probe_pa=0x%08X"
                " pa=0x%08X k0=0x%08X k1=0x%08X"
                " errs=(orig:%d wr:%d pa:%d k0:%d k1:%d)\n",
                pass ? "pass" : "fail",
                probe_pa32, pa_val, kseg0_val, kseg1_val,
                (int)e_orig, (int)e_w, (int)e_pa, (int)e_k0, (int)e_k1);
        return;
    }

    if (m->alias_fallback_sync_active) {
        fprintf(stderr,
                "[ALIAS_COHERENCE] fallback-write-sync active"
                " mode=fallback shared=0\n");
        return;
    }

    fprintf(stderr, "[ALIAS_COHERENCE] fail mode=none reason=no_alias_strategy\n");
}

static void format_hex_bytes(const uint8_t *buf, size_t len,
                             char *out, size_t out_sz);

static bool map_kseg_alias_block(machine_t *m, uint64_t map_base, uint64_t pa_base)
{
    if (m->shared_alias_active &&
        m->sdram_backing != NULL &&
        pa_base < (uint64_t)m->cfg.sdram_size &&
        pa_base + UINT64_C(0x100000) <= (uint64_t)m->cfg.sdram_size) {
        uc_err pe = uc_mem_map_ptr(m->uc, map_base, 0x100000, UC_PROT_ALL,
                                   m->sdram_backing + (size_t)pa_base);
        if (pe == UC_ERR_OK || pe == UC_ERR_MAP)
            return true;
        static uint32_t shared_alias_map_fail = 0;
        if (shared_alias_map_fail < 16) {
            fprintf(stderr,
                    "[ALIAS_MODE] shared map_kseg_alias_block failed"
                    " map=0x%08" PRIX64 " pa=0x%08" PRIX64 " err=%s\n",
                    map_base, pa_base, uc_strerror(pe));
            shared_alias_map_fail++;
        }
    }

    uc_err me = uc_mem_map(m->uc, map_base, 0x100000, UC_PROT_ALL);
    if (me != UC_ERR_OK && me != UC_ERR_MAP)
        return false;

    if (me == UC_ERR_OK) {
        uint8_t buf[4096];
        for (uint64_t off = 0; off < 0x100000; off += sizeof(buf)) {
            if (uc_mem_read(m->uc, pa_base + off, buf, sizeof(buf)) != UC_ERR_OK)
                memset(buf, 0, sizeof(buf));
            if (uc_mem_write(m->uc, map_base + off, buf, sizeof(buf)) != UC_ERR_OK)
                break;
        }
    }
    return true;
}

static bool map_kseg_mirror_block(machine_t *m, uint64_t va_block)
{
    uint32_t va32 = (uint32_t)va_block;
    if (va32 < 0x80000000u || va32 > 0xBFFFFFFFu)
        return false;

    uint64_t map_base = (uint64_t)(int64_t)(int32_t)va32;
    uint64_t map_base_zero = (uint64_t)va32;
    uint64_t pa_base = (uint64_t)(va32 & 0x1FFFFFFFu);
    bool mapped_any = false;

    if (map_kseg_alias_block(m, map_base, pa_base))
        mapped_any = true;

    /*
     * Some Unicorn paths surface zero-extended 0x8000xxxx VAs in MIPS64 mode.
     * Mirror both aliases so exception-vector fetches can resolve either form.
     */
    if (map_base_zero != map_base && map_kseg_alias_block(m, map_base_zero, pa_base))
        mapped_any = true;

    if (!mapped_any)
        return false;
    return true;
}

/*
 * tlb_map_kuseg_page — directly map a physical SDRAM page into kuseg virtual
 * address space in Unicorn's host region table.
 *
 * Background: Unicorn's MIPS64 softmmu TLB may not honour TLBWI for kuseg
 * addresses, causing an infinite TLB-store-miss storm at PC=0x800015B4.
 * Two root causes are addressed together:
 *
 *  (a) Stale softmmu TLB entry: the softmmu cached a read-only (D=0) entry
 *      that isn't evicted when handle_tlbs upgrades the PTE to D=1 + TLBWI.
 *      Fixed by uc_ctl_flush_tlb() after TLBWI/TLBWR (see pending_tlb_flush).
 *
 *  (b) Unicorn kuseg bypass: some Unicorn builds skip the MIPS hardware TLB
 *      for kuseg and use the flat Unicorn region table keyed by VA directly.
 *      Fixed by pre-mapping the faulting VA block in Unicorn's region table
 *      with a copy of the corresponding SDRAM content.
 *
 * Both fixes are applied together; whichever is relevant for the current
 * Unicorn build will unblock execution.
 */
static void tlb_map_kuseg_page(machine_t *m, uint64_t kuseg_va, uint64_t pa,
                               uint64_t page_bytes)
{
    if (page_bytes < 0x1000u)
        page_bytes = 0x1000u;
    if ((page_bytes & 0xFFFu) != 0u)
        page_bytes = (page_bytes + 0xFFFu) & ~(uint64_t)0xFFFu;

    if ((uint32_t)kuseg_va >= 0x80000000u)
        return;  /* safety: only handle user-space addresses */

    uint64_t va_pg = kuseg_va & ~(page_bytes - 1u);
    uint64_t pa_pg = pa       & ~(page_bytes - 1u);

    uc_err me = uc_mem_map(m->uc, va_pg, page_bytes, UC_PROT_ALL);
    bool premap = (me == UC_ERR_MAP);
    if (premap) {
        /* Pre-mapped VA window: still refresh it from SDRAM PA content. */
        me = UC_ERR_OK;
        uc_err pe = uc_mem_protect(m->uc, va_pg, page_bytes, UC_PROT_ALL);
        if (pe != UC_ERR_OK) {
            static uint32_t prot_err_log = 0;
            if (prot_err_log++ < 16)
                fprintf(stderr,
                        "[TLB_MAP_PROT_FAIL] VA_pg=0x%08" PRIX64
                        " bytes=0x%08" PRIX64 " err=%s\n",
                        va_pg, page_bytes, uc_strerror(pe));
        }
    }
    if (me != UC_ERR_OK) {
        static uint32_t err_log = 0;
        if (err_log++ < 16)
            fprintf(stderr,
                    "[TLB_MAP_FAIL] VA_pg=0x%08" PRIX64
                    " bytes=0x%08" PRIX64 " err=%s\n",
                    va_pg, page_bytes, uc_strerror(me));
        return;
    }

    /* Populate exactly the translated TLB window.
     * Read from kseg0 alias (0x80000000 + PA) first, because kernel writes
     * (ramdisk load, page cache I/O) go to kseg0 alias regions which are
     * separate Unicorn memory from the PA region at 0x00000000.
     * Fall back to raw PA if the kseg0 alias block isn't mapped yet.
     *
     * We write to THREE destinations:
     *   1. The kuseg VA flat region (for direct softmmu hits before TLBWI)
     *   2. The raw PA region (for softmmu hits after TLB-translated access)
     *   3. The kseg0 alias is already authoritative — no write needed.
     * This ensures correct data regardless of whether the softmmu resolves
     * via the flat VA region or through the MIPS hardware TLB to PA. */
    uint8_t buf[4096];
    for (uint64_t off = 0; off < page_bytes; off += sizeof(buf)) {
        uint64_t pa_off = pa_pg + off;
        bool got_data = false;

        /* Try kseg0 alias first (has kernel runtime writes) */
        if (pa_off < (uint64_t)m->cfg.sdram_size) {
            uint64_t kseg0_addr = 0x80000000u + pa_off;
            if (uc_mem_read(m->uc, kseg0_addr, buf, sizeof(buf)) == UC_ERR_OK)
                got_data = true;
        }
        /* Fallback to raw PA (has ELF loader content) */
        if (!got_data && pa_off < (uint64_t)m->cfg.sdram_size) {
            if (uc_mem_read(m->uc, pa_off, buf, sizeof(buf)) == UC_ERR_OK)
                got_data = true;
        }
        if (!got_data)
            memset(buf, 0, sizeof(buf));

        /* Write to kuseg VA flat region */
        uc_mem_write(m->uc, va_pg + off, buf, sizeof(buf));

        /* Also sync raw PA so TLB-translated accesses see correct data */
        if (got_data && pa_off < (uint64_t)m->cfg.sdram_size)
            uc_mem_write(m->uc, pa_off, buf, sizeof(buf));
    }

    static uint32_t map_log = 0;
    if (map_log++ < 64)
        fprintf(stderr,
                "[TLB_MAP] kuseg VA=0x%08" PRIX64
                " <- SDRAM PA=0x%08" PRIX64
                " bytes=0x%08" PRIX64 " premap=%u (via kseg0)\n",
                va_pg, pa_pg, page_bytes, premap ? 1u : 0u);

    if (m->cfg.trace_user_handoff &&
        m->execve_user_handoff_active &&
        m->execve_user_handoff_state == EXECVE_HANDOFF_STATE_ARMED) {
        static uint32_t mapcheck_log = 0;
        uint32_t handoff_pc = (uint32_t)m->execve_user_handoff_pc;
        uint64_t map_end = va_pg + page_bytes;
        if (mapcheck_log < 256 &&
            (uint64_t)handoff_pc >= va_pg &&
            (uint64_t)handoff_pc + 16u <= map_end) {
            uint64_t off = (uint64_t)handoff_pc - va_pg;
            uint64_t pa_target = pa_pg + off;
            uint8_t va_bytes[16] = {0};
            uint8_t pa_bytes[16] = {0};
            uc_err va_err = uc_mem_read(m->uc, (uint64_t)handoff_pc, va_bytes, sizeof(va_bytes));
            uc_err pa_err = uc_mem_read(m->uc, pa_target, pa_bytes, sizeof(pa_bytes));
            char va_hex[16 * 3 + 1];
            char pa_hex[16 * 3 + 1];
            format_hex_bytes(va_bytes, sizeof(va_bytes), va_hex, sizeof(va_hex));
            format_hex_bytes(pa_bytes, sizeof(pa_bytes), pa_hex, sizeof(pa_hex));
            bool same = (va_err == UC_ERR_OK && pa_err == UC_ERR_OK &&
                         memcmp(va_bytes, pa_bytes, sizeof(va_bytes)) == 0);
            fprintf(stderr,
                    "[USER_HANDOFF_MAPCHECK] pc=0x%08X va_page=0x%08" PRIX64
                    " pa_page=0x%08" PRIX64 " off=0x%04" PRIX64
                    " va_err=%d pa_err=%d same=%u va=[%s] pa=[%s]\n",
                    handoff_pc, va_pg, pa_pg, off,
                    va_err, pa_err, same ? 1u : 0u, va_hex, pa_hex);
            mapcheck_log++;
        }
    }

    /* Ensure any stale softmmu negative translation for this VA is dropped
     * before retrying user-mode fetch/load. */
    uc_ctl_flush_tlb(m->uc);
}

typedef struct {
    bool hit;
    bool confident;
    bool pair_valid;
    const char *reason;
    uint32_t query_va;
    uint32_t query_vpn2;
    uint32_t entryhi;
    uint32_t entryhi_vpn2;
    uint8_t entryhi_asid;
    uint8_t current_asid;
    bool current_asid_valid;
    uint32_t pagemask;
    uint32_t lo0;
    uint32_t lo1;
    uint64_t va_page;
    uint64_t pa_page;
    uint64_t page_bytes;
    uint64_t page_offset;
    uint64_t pair_va_page;
    uint64_t pair_pa_page;
    uint64_t pair_page_bytes;
    uint32_t source_idx;
} tlb_lookup_result_t;

static void shadow_tlb_record_write(machine_t *m, uint32_t tlb_insn, uint32_t pc)
{
    uint32_t idx = 0;
    if (tlb_insn == 0x42000002u) {
        idx = ((uint32_t)m->shadow_cp0_index) & 0x3Fu;  /* tlbwi uses Index */
    } else if (tlb_insn == 0x42000006u) {
        idx = m->shadow_tlb_wr_cursor++ & 0x3Fu;         /* tlbwr index unknown */
    } else {
        return;
    }

    m->shadow_tlb_valid[idx] = true;
    m->shadow_tlb_entryhi[idx] = (uint32_t)m->shadow_cp0_entryhi;
    m->shadow_tlb_lo0[idx] = (uint32_t)m->shadow_cp0_entrylo0;
    m->shadow_tlb_lo1[idx] = (uint32_t)m->shadow_cp0_entrylo1;
    m->shadow_tlb_pagemask[idx] = (uint32_t)m->shadow_cp0_pagemask;
    m->shadow_tlb_seq[idx] = ++m->shadow_tlb_seq_next;

    static uint32_t shadow_tlb_log = 0;
    if (shadow_tlb_log++ < 256) {
        fprintf(stderr,
                "[SHADOW_TLB_WRITE] op=%s idx=%u pc=0x%08X hi=0x%08X"
                " lo0=0x%08X lo1=0x%08X mask=0x%08X seq=%u\n",
                (tlb_insn == 0x42000002u) ? "tlbwi" : "tlbwr",
                idx, pc, m->shadow_tlb_entryhi[idx], m->shadow_tlb_lo0[idx],
                m->shadow_tlb_lo1[idx], m->shadow_tlb_pagemask[idx],
                m->shadow_tlb_seq[idx]);
    }
}

static tlb_lookup_result_t shadow_tlb_lookup(machine_t *m, uint32_t va)
{
    tlb_lookup_result_t r;
    memset(&r, 0, sizeof(r));
    r.reason = "init";
    r.query_va = va;
    r.query_vpn2 = va & ~0x1FFFu;
    r.source_idx = 0xFFFFFFFFu;
    r.current_asid_valid = m->shadow_cp0_entryhi_live_valid;
    r.current_asid = (uint8_t)(m->shadow_cp0_entryhi_live & 0xFFu);

    if (va >= 0x80000000u) {
        r.reason = "non_kuseg";
        return r;
    }

    /* Search most recent matching entry in the shadow TLB table first. */
    uint32_t best_idx = 0xFFFFFFFFu;
    uint32_t best_seq = 0u;
    for (uint32_t i = 0; i < 64u; i++) {
        if (!m->shadow_tlb_valid[i])
            continue;
        uint32_t hi = m->shadow_tlb_entryhi[i];
        uint32_t pm = m->shadow_tlb_pagemask[i];
        if (!tlb_entry_matches_va(va, hi, pm))
            continue;
        if (m->shadow_tlb_seq[i] >= best_seq) {
            best_seq = m->shadow_tlb_seq[i];
            best_idx = i;
        }
    }

    if (best_idx != 0xFFFFFFFFu) {
        r.source_idx = best_idx;
        r.entryhi = m->shadow_tlb_entryhi[best_idx];
        r.entryhi_vpn2 = r.entryhi & 0xFFFFE000u;
        r.entryhi_asid = (uint8_t)(r.entryhi & 0xFFu);
        r.lo0 = m->shadow_tlb_lo0[best_idx];
        r.lo1 = m->shadow_tlb_lo1[best_idx];
        r.pagemask = m->shadow_tlb_pagemask[best_idx];
    } else {
        /* Fallback to the single live CP0 snapshot when no table match exists. */
        r.entryhi = (uint32_t)m->shadow_cp0_entryhi;
        r.entryhi_vpn2 = r.entryhi & 0xFFFFE000u;
        r.entryhi_asid = (uint8_t)(r.entryhi & 0xFFu);
        r.lo0 = (uint32_t)m->shadow_cp0_entrylo0;
        r.lo1 = (uint32_t)m->shadow_cp0_entrylo1;
        r.pagemask = (uint32_t)m->shadow_cp0_pagemask;
        if (!tlb_entry_matches_va(va, r.entryhi, r.pagemask)) {
            r.reason = "vpn2_mismatch";
            return r;
        }
    }
    r.hit = true;

    r.page_bytes = tlb_leaf_bytes_from_pagemask(r.pagemask);
    if (r.page_bytes < 0x1000u) {
        r.reason = "invalid_page_bytes";
        return r;
    }

    bool odd_page = (((uint64_t)va & r.page_bytes) != 0u);
    uint32_t lo = odd_page ? r.lo1 : r.lo0;
    if ((lo & 0x2u) == 0u) {
        r.reason = odd_page ? "lo1_invalid" : "lo0_invalid";
        return r;
    }

    bool global_pair = ((r.lo0 & 0x1u) != 0u) && ((r.lo1 & 0x1u) != 0u);
    bool asid_ok = global_pair || !r.current_asid_valid ||
                   (r.entryhi_asid == r.current_asid);
    if (!asid_ok) {
        r.reason = "asid_mismatch";
        return r;
    }
    if (!global_pair && !r.current_asid_valid)
        r.reason = "asid_unchecked";

    uint64_t pfn = (uint64_t)((lo >> 6) & 0xFFFFFu);
    /* VR41xx software PTE encoding keeps PFN shifted by +2 bits. */
    uint64_t pa = (pfn << 10) & ~(r.page_bytes - 1u);
    if (pa >= (uint64_t)m->cfg.sdram_size) {
        r.reason = "pa_out_of_range";
        return r;
    }

    r.va_page = (uint64_t)va & ~(r.page_bytes - 1u);
    r.pa_page = pa;
    r.page_offset = (uint64_t)va - r.va_page;
    r.confident = true;
    if (r.reason == NULL || strcmp(r.reason, "init") == 0)
        r.reason = "ok";

    uint32_t pair_lo = odd_page ? r.lo0 : r.lo1;
    if ((pair_lo & 0x2u) != 0u) {
        uint64_t pair_pfn = (uint64_t)((pair_lo >> 6) & 0xFFFFFu);
        uint64_t pair_pa = (pair_pfn << 10) & ~(r.page_bytes - 1u);
        if (pair_pa < (uint64_t)m->cfg.sdram_size) {
            r.pair_valid = true;
            r.pair_va_page = odd_page ? (r.va_page - r.page_bytes) :
                                        (r.va_page + r.page_bytes);
            r.pair_pa_page = pair_pa;
            r.pair_page_bytes = r.page_bytes;
        }
    }

    return r;
}

static void format_hex_bytes(const uint8_t *buf, size_t len,
                             char *out, size_t out_sz)
{
    size_t pos = 0;
    if (out_sz == 0)
        return;
    out[0] = '\0';
    for (size_t i = 0; i < len; i++) {
        if (pos + 4 >= out_sz)
            break;
        int n = snprintf(out + pos, out_sz - pos, "%02X", buf[i]);
        if (n <= 0)
            break;
        pos += (size_t)n;
        if (i + 1 < len && pos + 2 < out_sz)
            out[pos++] = ' ';
    }
    if (pos < out_sz)
        out[pos] = '\0';
}

static bool trace_user_handoff_entry_probe(machine_t *m, const char *tag,
                                           uint32_t pc, uint32_t va,
                                           uint64_t raw_badv, bool first_fault_only)
{
    if (!in_user_handoff_entry_window(pc))
        return false;
    if (first_fault_only && m->cfg.trace_user_handoff && m->user_handoff_fault_traced)
        return false;
    if (first_fault_only && m->cfg.trace_user_handoff)
        m->user_handoff_fault_traced = true;

    tlb_lookup_result_t r = shadow_tlb_lookup(m, va);
    uint64_t pa_base = 0;
    uint64_t pa_target = 0;
    uint64_t offset = 0;
    uint8_t va_bytes[16] = {0};
    uint8_t pa_bytes[16] = {0};
    uc_err va_err = uc_mem_read(m->uc, (uint64_t)va, va_bytes, sizeof(va_bytes));
    uc_err pa_err = UC_ERR_READ_UNMAPPED;

    if (r.hit && r.confident) {
        offset = (uint64_t)va - r.va_page;
        pa_base = r.pa_page;
        pa_target = pa_base + offset;
        pa_err = uc_mem_read(m->uc, pa_target, pa_bytes, sizeof(pa_bytes));
    } else {
        pa_target = (uint64_t)(va & 0x1FFFFFFFu);
        pa_err = uc_mem_read(m->uc, pa_target, pa_bytes, sizeof(pa_bytes));
    }

    uint32_t insn = 0;
    bool insn_ok = read_insn_best_effort(m->uc, va, &insn);
    bool bytes_same = (va_err == UC_ERR_OK &&
                       pa_err == UC_ERR_OK &&
                       memcmp(va_bytes, pa_bytes, sizeof(va_bytes)) == 0);
    bool bytes_valid = false;
    if (va_err == UC_ERR_OK) {
        for (size_t i = 0; i < sizeof(va_bytes); i++) {
            if (va_bytes[i] != 0u) {
                bytes_valid = true;
                break;
            }
        }
    }

    char va_hex[16 * 3 + 1];
    char pa_hex[16 * 3 + 1];
    format_hex_bytes(va_bytes, sizeof(va_bytes), va_hex, sizeof(va_hex));
    format_hex_bytes(pa_bytes, sizeof(pa_bytes), pa_hex, sizeof(pa_hex));

    static uint32_t probe_log = 0;
    if (m->cfg.trace_user_handoff && probe_log < 192) {
        fprintf(stderr,
                "[USER_HANDOFF_PROBE_%s] pc=0x%08X va=0x%08X badv=0x%08" PRIX64
                " hit=%u confident=%u reason=%s src=%s%u\n",
                tag, pc, va, (uint64_t)(uint32_t)raw_badv,
                r.hit ? 1u : 0u, r.confident ? 1u : 0u, r.reason,
                (r.source_idx == 0xFFFFFFFFu) ? "live:" : "tlb:",
                (r.source_idx == 0xFFFFFFFFu) ? 0u : r.source_idx);
        fprintf(stderr,
                "[USER_HANDOFF_PROBE_%s] entryhi=0x%08X lo0=0x%08X lo1=0x%08X"
                " mask=0x%08X page=0x%08" PRIX64 " va_page=0x%08" PRIX64
                " pa_base=0x%08" PRIX64 " off=0x%04" PRIX64 " pa=0x%08" PRIX64 "\n",
                tag, r.entryhi, r.lo0, r.lo1, r.pagemask, r.page_bytes,
                r.va_page, pa_base, offset, pa_target);
        fprintf(stderr,
                "[USER_HANDOFF_PROBE_%s] insn_ok=%u insn=0x%08X va_err=%d pa_err=%d"
                " bytes_same=%u bytes_valid=%u va=[%s] pa=[%s]\n",
                tag, insn_ok ? 1u : 0u, insn,
                va_err, pa_err, bytes_same ? 1u : 0u, bytes_valid ? 1u : 0u,
                va_hex, pa_hex);
        probe_log++;
    }
    return bytes_valid;
}

static void trace_user_handoff_fault_once(machine_t *m, uint32_t fault_pc,
                                          uint32_t fault_va, uint64_t raw_badv)
{
    (void)trace_user_handoff_entry_probe(m, "FAULT", fault_pc, fault_va,
                                         raw_badv, true);
}

static bool shadow_tlb_populate(machine_t *m, uint32_t va, bool include_pair,
                                const char *tag, uint32_t pc_hint)
{
    tlb_lookup_result_t r = shadow_tlb_lookup(m, va);
    if (!r.hit || !r.confident) {
        static uint32_t skip_log = 0;
        if (skip_log++ < 256) {
            fprintf(stderr,
                    "[%s_SKIP] va=0x%08X pc=0x%08X hit=%u confident=%u reason=%s"
                    " entryhi=0x%08X vpn2=0x%08X lo0=0x%08X lo1=0x%08X"
                    " mask=0x%08X asid=0x%02X cur_asid=%s0x%02X src=%s%u\n",
                    tag, va, pc_hint,
                    r.hit ? 1u : 0u, r.confident ? 1u : 0u, r.reason,
                    r.entryhi, r.entryhi_vpn2, r.lo0, r.lo1, r.pagemask,
                    (unsigned)r.entryhi_asid,
                    r.current_asid_valid ? "" : "?",
                    (unsigned)r.current_asid,
                    (r.source_idx == 0xFFFFFFFFu) ? "live:" : "tlb:",
                    (r.source_idx == 0xFFFFFFFFu) ? 0u : r.source_idx);
        }
        return false;
    }

    tlb_map_kuseg_page(m, r.va_page, r.pa_page, r.page_bytes);
    if (include_pair && r.pair_valid)
        tlb_map_kuseg_page(m, r.pair_va_page, r.pair_pa_page, r.pair_page_bytes);

    static uint32_t ok_log = 0;
    if (ok_log++ < 256) {
        fprintf(stderr,
                "[%s_POPULATE] va=0x%08X pc=0x%08X pa=0x%08" PRIX64
                " bytes=0x%08" PRIX64
                " pair=%u confident=1 src=%s%u reason=%s\n",
                tag, va, pc_hint, r.pa_page, r.page_bytes,
                r.pair_valid ? 1u : 0u,
                (r.source_idx == 0xFFFFFFFFu) ? "live:" : "tlb:",
                (r.source_idx == 0xFFFFFFFFu) ? 0u : r.source_idx,
                r.reason);
    }
    return true;
}

/* Convert kseg0/kseg1 VA to physical; return false for non-direct-mapped VA. */
static bool va_to_pa_kseg(uint64_t va, uint64_t *pa_out)
{
    uint32_t va32 = (uint32_t)va;
    if ((va32 >= 0x80000000u && va32 <= 0xBFFFFFFFu) ||
        ((uint64_t)(int64_t)(int32_t)va32) == va) {
        *pa_out = (uint64_t)(va32 & 0x1FFFFFFFu);
        return true;
    }
    return false;
}

/*
 * Best-effort instruction read for hooks.
 * Unicorn sometimes reports virtual PC while uc_mem_read expects mapped PA.
 */
static bool read_insn_best_effort(uc_engine *uc, uint64_t address, uint32_t *insn)
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

static int read_guest_string(uc_engine *uc, uint64_t va, char *buf, int bufsz);

static bool read_guest_u32(uc_engine *uc, uint64_t va, uint32_t *out)
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

    if (is_wince_boot_machine(m) && !m->wince_nk_epoch_reset_done) {
        uint32_t pc32 = (uint32_t)address;
        if (pc32 >= WINCE_NK_TRACE_BASE && pc32 < WINCE_NK_TRACE_END) {
            m->wince_nk_epoch_reset_done = true;
            m->rtc.etime = 0;
            m->rtc.etime_latched = 0;
            m->rtc.etime_reads = 0;
            m->rtc.etime_read_step = 1;
            if (m->cfg.log_wince_stall) {
                fprintf(stderr,
                        "[WINCE_RTC_MODE] nk_epoch_reset pc=0x%08X"
                        " etime=0 step=%u\n",
                        pc32, m->rtc.etime_read_step);
            }
        }
    }

    uint32_t op  = (insn >> 26) & 0x3Fu;
    uint32_t rs  = (insn >> 21) & 0x1Fu;
    uint32_t rt  = (insn >> 16) & 0x1Fu;
    uint32_t rd  = (insn >> 11) & 0x1Fu;
    uint32_t sel =  insn        & 0x07u;

    maybe_record_wince_ctrl_event(m, uc, (uint32_t)address,
                                  insn, op, rs, rt, rd, sel);
    maybe_probe_wince_ctx_path(m, uc, (uint32_t)address,
                               insn, op, rs, rt, rd, sel);

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
     * Treat it as a low-power hint (NOP) so execution advances.
     */
    if (is_wince_boot_machine(m) && insn == 0x42000023u) {
        uint64_t next_pc = address + 4u;
        static uint32_t wince_wait_skip_log = 0;
        if (wince_wait_skip_log < 32u) {
            fprintf(stderr,
                    "[WINCE_WAIT_SKIP] PC=0x%08" PRIX64 " -> 0x%08" PRIX64 "\n",
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
        uint64_t count_val = (uint64_t)(uint32_t)(m->insn_count / 2u);
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

#define WINCE_NULL_RECOVER_CAP 64u

static inline bool is_null_call_class_intno(uint32_t intno)
{
    return intno == 20u || intno == 26u || intno == 27u;
}

static void clear_pending_exception_state(machine_t *m)
{
    m->pending_epc          = 0;
    m->pending_excode       = 0;
    m->pending_cause        = 0;
    m->epc_was_written      = false;
    m->pending_cause_served = false;
    m->pending_epc_served   = false;
}

static bool handle_linux_null_call_interrupt(machine_t *m, uc_engine *uc,
                                             uint32_t intno, uint64_t pc)
{
    (void)m;
    if ((uint32_t)pc != 0u || !is_null_call_class_intno(intno))
        return false;

    uint64_t ra = 0, t9 = 0, gp = 0, sp = 0;
    uint64_t a0 = 0, a1 = 0, a2 = 0, v0 = 0;
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
    uc_reg_read(uc, UC_MIPS_REG_T9, &t9);
    uc_reg_read(uc, UC_MIPS_REG_GP, &gp);
    uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
    uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
    uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
    uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
    uc_reg_read(uc, UC_MIPS_REG_V0, &v0);

    static uint32_t null_ptr_log = 0;
    if (null_ptr_log < 8u) {
        fprintf(stderr,
                "[NULL_CALL] PC=0x%08" PRIX64 " ra=0x%08" PRIX64
                " t9=0x%08" PRIX64 " gp=0x%08" PRIX64 " sp=0x%08" PRIX64
                " a0=0x%08" PRIX64 " a1=0x%08" PRIX64
                " a2=0x%08" PRIX64 " v0=0x%08" PRIX64 "\n",
                (uint64_t)(uint32_t)pc,
                (uint64_t)(uint32_t)ra, (uint64_t)(uint32_t)t9,
                (uint64_t)(uint32_t)gp, (uint64_t)(uint32_t)sp,
                (uint64_t)(uint32_t)a0, (uint64_t)(uint32_t)a1,
                (uint64_t)(uint32_t)a2, (uint64_t)(uint32_t)v0);
        null_ptr_log++;
    }

    uint32_t ra32 = (uint32_t)ra;
    if (ra32 == 0x80042F8Cu) {
        uint32_t saved_ra_lo = 0;
        uint64_t sp_val = 0;
        uc_reg_read(uc, UC_MIPS_REG_SP, &sp_val);
        uint32_t sp32   = (uint32_t)sp_val;
        uint64_t va_sp  = mips_sext(sp32);
        uc_mem_read(uc, va_sp + 16u, &saved_ra_lo, 4u);
        uint64_t new_sp = mips_sext(sp32 + 24u);
        uint64_t new_ra = mips_sext(saved_ra_lo);
        uint64_t neg2   = (uint64_t)(uint32_t)-2;
        fprintf(stderr,
                "[NULL_CALL_RECOVER] parse_one jalr t3 NULL;"
                " va_sp=0x%016" PRIX64 " saved_ra=0x%08" PRIX32
                " new_sp=0x%08" PRIX64 "\n",
                va_sp, saved_ra_lo, (uint64_t)(uint32_t)new_sp);
        if (saved_ra_lo >= 0x80000000u) {
            uc_reg_write(uc, UC_MIPS_REG_SP, &new_sp);
            uc_reg_write(uc, UC_MIPS_REG_RA, &new_ra);
            uc_reg_write(uc, UC_MIPS_REG_V0, &neg2);
            uc_reg_write(uc, UC_MIPS_REG_PC, &new_ra);
            return true;
        }
    }

    if (ra32 >= 0x80000000u) {
        uint64_t new_pc = mips_sext(ra32);
        uint64_t zero   = 0;
        uc_reg_write(uc, UC_MIPS_REG_PC, &new_pc);
        uc_reg_write(uc, UC_MIPS_REG_V0, &zero);
        static uint32_t gen_recover_log = 0;
        if (gen_recover_log < 16u) {
            fprintf(stderr,
                    "[NULL_CALL_RECOVER_GENERAL] PC=0 ra=0x%08" PRIX32
                    " -> redirecting to ra (v0=0)\n", ra32);
            gen_recover_log++;
        }
        return true;
    }

    return false;
}

static bool handle_wince_null_call_interrupt(machine_t *m, uc_engine *uc,
                                             uint32_t intno, uint64_t pc,
                                             uint64_t status)
{
    if ((uint32_t)pc != 0u || !is_null_call_class_intno(intno))
        return false;

    uint64_t ra = 0;
    uint64_t v0 = 0;
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
    uc_reg_read(uc, UC_MIPS_REG_V0, &v0);

    /*
     * SPL handoff site:
     *   0xA0F024F8: jalr a1   ; returns stage entry in v0
     *   0xA0F02500: move t0,v0
     *   0xA0F02504: jr t0
     *
     * If v0 is unexpectedly 0, jr t0 jumps to 0. Recover by
     * redirecting to the stage entry block at 0xA0F0250C.
     */
    if ((uint32_t)ra == 0xA0F02500u) {
        uint32_t stage32 = 0xA0F0250Cu;
        uint32_t saved_stage32 = 0;
        uc_err saved_stage_err = uc_mem_read(uc, mips_sext(0xA00024FCu),
                                             &saved_stage32, sizeof(saved_stage32));
        if (saved_stage_err != UC_ERR_OK)
            saved_stage_err = uc_mem_read(uc, UINT64_C(0x000024FC),
                                          &saved_stage32, sizeof(saved_stage32));
        if (saved_stage_err == UC_ERR_OK && is_kseg_va32(saved_stage32))
            stage32 = saved_stage32;
        uint64_t stage = mips_sext(stage32);
        uc_reg_write(uc, UC_MIPS_REG_V0, &stage);
        uc_reg_write(uc, UC_MIPS_REG_T0, &stage);
        uc_reg_write(uc, UC_MIPS_REG_PC, &stage);
        m->wince_null_last_ra = 0;
        m->wince_null_last_intno = 0;
        m->wince_null_consecutive = 0;

        static uint32_t wince_spl_recover_log = 0;
        if (wince_spl_recover_log < 32u) {
            fprintf(stderr,
                    "[WINCE_NULL_RECOVER] intno=%u pc=0x%08" PRIX64
                    " ra=0x%08" PRIX64 " v0=0x%08" PRIX64
                    " saved_stage=0x%08" PRIX32 " saved_stage_err=%d"
                    " -> stage=0x%08" PRIX32 "\n",
                    intno,
                    (uint64_t)(uint32_t)pc,
                    (uint64_t)(uint32_t)ra,
                    (uint64_t)(uint32_t)v0,
                    saved_stage32,
                    (int)saved_stage_err,
                    stage32);
            wince_spl_recover_log++;
        }
        return true;
    }

    uint32_t ra32 = (uint32_t)ra;
    if (ra32 >= 0x80000000u) {
        if (m->wince_null_last_ra == ra32 &&
            m->wince_null_last_intno == intno) {
            if (m->wince_null_consecutive < UINT32_MAX)
                m->wince_null_consecutive++;
        } else {
            m->wince_null_last_ra = ra32;
            m->wince_null_last_intno = intno;
            m->wince_null_consecutive = 1u;
        }

        if (m->wince_null_consecutive == 2u) {
            static uint32_t wince_null_burst_log = 0;
            if (wince_null_burst_log < 64u) {
                fprintf(stderr,
                        "[WINCE_NULL_BURST] intno=%u ra=0x%08X"
                        " pending_excode=%u pending_epc=0x%08" PRIX64
                        " pending_cause=0x%08X STATUS=0x%08" PRIX64 "\n",
                        intno, ra32, m->pending_excode,
                        (uint64_t)(uint32_t)m->pending_epc,
                        m->pending_cause,
                        (uint64_t)(uint32_t)status);
                wince_null_burst_log++;
            }
        }

        if (m->wince_null_consecutive > WINCE_NULL_RECOVER_CAP) {
            fprintf(stderr,
                    "[WINCE_NULL_BAILOUT] intno=%u ra=0x%08X count=%u cap=%u"
                    " STATUS=0x%08" PRIX64 "\n",
                    intno, ra32, m->wince_null_consecutive,
                    WINCE_NULL_RECOVER_CAP, (uint64_t)(uint32_t)status);
            log_wince_pa_watch_nonzero(m, "NULL_BAILOUT");
            machine_stop(m);
            uc_emu_stop(uc);
            return true;
        }

        uint64_t next_pc = mips_sext(ra32);
        uint64_t zero = 0;
        uint64_t new_status = status & ~(uint64_t)0x2u; /* clear EXL only */
        clear_pending_exception_state(m);
        uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &new_status);
        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
        uc_reg_write(uc, UC_MIPS_REG_V0, &zero);

        static uint32_t wince_general_recover_log = 0;
        if (wince_general_recover_log < 128u) {
            fprintf(stderr,
                    "[WINCE_NULL_RECOVER] intno=%u ra=0x%08X count=%u"
                    " STATUS=0x%08" PRIX64 " -> PC=0x%08X\n",
                    intno, ra32, m->wince_null_consecutive,
                    (uint64_t)(uint32_t)status, ra32);
            wince_general_recover_log++;
        }

        /* One-shot code dump around the null-call site for NK addresses.
         * JALR is at RA-8, delay slot at RA-4, return at RA. */
        static bool null_call_code_dumped = false;
        if (!null_call_code_dumped &&
            !(ra32 >= UINT32_C(0x80F00000) && ra32 < UINT32_C(0x80F10000))) {
            null_call_code_dumped = true;
            uint32_t dump_base = (ra32 - 64u) & ~UINT32_C(0x3);
            fprintf(stderr, "[WINCE_NULL_CODE] dumping 128 bytes around RA=0x%08X:\n", ra32);
            for (uint32_t off = 0; off < 128; off += 64) {
                uint8_t chunk[64];
                uint64_t pa = (uint64_t)((dump_base + off) & 0x1FFFFFFFu);
                uc_err merr = uc_mem_read(uc, pa, chunk, 64);
                if (merr != UC_ERR_OK) {
                    fprintf(stderr, "[WINCE_NULL_CODE] chunk at 0x%08X: READ FAILED\n",
                            dump_base + off);
                    continue;
                }
                fprintf(stderr, "[WINCE_NULL_CODE] base=0x%08X off=0x%02X hex=",
                        dump_base, off);
                for (int b = 0; b < 64; b++)
                    fprintf(stderr, "%02X", chunk[b]);
                fprintf(stderr, "\n");
            }
            /* Also dump GPRs for context */
            uint64_t t9v = 0, a0v = 0, a1v = 0, s0v = 0;
            uc_reg_read(uc, UC_MIPS_REG_T9, &t9v);
            uc_reg_read(uc, UC_MIPS_REG_A0, &a0v);
            uc_reg_read(uc, UC_MIPS_REG_A1, &a1v);
            uc_reg_read(uc, UC_MIPS_REG_S0, &s0v);
            fprintf(stderr,
                    "[WINCE_NULL_CODE] t9=0x%08X a0=0x%08X a1=0x%08X s0=0x%08X\n",
                    (uint32_t)t9v, (uint32_t)a0v, (uint32_t)a1v, (uint32_t)s0v);

            /* Dump the JAL target function (RA-8 is the JAL, extract target) */
            uint32_t jal_insn = 0;
            uint64_t jal_pa = (uint64_t)((ra32 - 8u) & 0x1FFFFFFFu);
            if (uc_mem_read(uc, jal_pa, &jal_insn, 4) == UC_ERR_OK) {
                uint32_t jal_op = jal_insn >> 26;
                if (jal_op == 3u) { /* JAL */
                    uint32_t jal_target = ((jal_insn & 0x03FFFFFFu) << 2) |
                                          (ra32 & 0xF0000000u);
                    fprintf(stderr, "[WINCE_NULL_CODE] JAL target=0x%08X, dumping:\n",
                            jal_target);
                    uint32_t tgt_pa = jal_target & 0x1FFFFFFFu;
                    for (uint32_t off = 0; off < 128; off += 64) {
                        uint8_t tc[64];
                        if (uc_mem_read(uc, (uint64_t)(tgt_pa + off), tc, 64) == UC_ERR_OK) {
                            fprintf(stderr, "[WINCE_NULL_CODE] tgt=0x%08X off=0x%02X hex=",
                                    jal_target, off);
                            for (int b = 0; b < 64; b++)
                                fprintf(stderr, "%02X", tc[b]);
                            fprintf(stderr, "\n");
                        }
                    }
                }
            }
        }
        return true;
    }

    fprintf(stderr, "[WINCE_INTR] NULL call detected (ra=0x%08" PRIX64
            ") last_exec_pc=0x%08" PRIX64 " — stopping\n",
            (uint64_t)(uint32_t)ra,
            (uint64_t)(uint32_t)m->last_exec_pc);
    log_wince_pa_watch_nonzero(m, "NULL_POSTMORTEM");
    log_wince_pa_watch_summary(m, "NULL_POSTMORTEM");
    log_wince_ctrl_hist_summary(m, "NULL_POSTMORTEM", 64u);

    /* Dump code around last known execution PC for post-mortem analysis */
    {
        uint32_t lpc = (uint32_t)m->last_exec_pc;
        uint32_t dump_base = (lpc >= 32u) ? (lpc - 32u) & ~UINT32_C(0x3) : 0u;
        uint32_t dump_pa = dump_base & UINT32_C(0x1FFFFFFF);
        fprintf(stderr, "[WINCE_NULL_POSTMORTEM] last_exec_pc=0x%08X dump:\n", lpc);
        for (uint32_t off = 0; off < 128; off += 64) {
            uint8_t chunk[64];
            if (uc_mem_read(uc, (uint64_t)(dump_pa + off), chunk, 64) == UC_ERR_OK) {
                fprintf(stderr, "[WINCE_NULL_POSTMORTEM] base=0x%08X off=0x%02X hex=",
                        dump_base, off);
                for (int b = 0; b < 64; b++)
                    fprintf(stderr, "%02X", chunk[b]);
                fprintf(stderr, "\n");
            }
        }
        /* Dump exception vectors and context block */
        struct { uint32_t pa; const char *name; } mem_regions[] = {
            { 0x00000000, "exception_vectors" },
            { 0x00000100, "exception_vectors+0x100" },
            { 0x00000180, "exception_vectors+0x180" },
            { 0x00002200, "context_block" },
            { 0x80079580, "ctx_caller_0x80079580" },
            { 0x800795C0, "ctx_caller_0x800795C0" },
            { 0x80079600, "ctx_caller_0x80079600" },
            { 0x80079640, "ctx_path_0x80079640" },
            { 0x80079680, "ctx_path_0x80079680" },
            { 0x80079700, "ctx_path_0x80079700" },
            { 0x80079780, "ctx_path_0x80079780" },
            { 0x80079840, "ctx_path_0x80079840" },
            { 0x80079880, "ctx_path_0x80079880" },
            { 0x80077DC0, "ctx_caller_0x80077DC0" },
            { 0x80077E28, "ctx_caller_0x80077E28" },
            { 0x800781C0, "ctx_caller_0x800781C0" },
            { 0x80078200, "ctx_caller_0x80078200" },
            { 0x80078BA0, "ctx_caller_0x80078BC0" },
            { 0x80077FC0, "ctx_caller_0x80077FE4" },
            { 0x80079A80, "ctx_caller_0x80079AC4" },
            { 0x80079DD0, "ctx_caller_0x80079DF8" },
            { 0x8007AF80, "ctx_caller_0x8007AFA8" },
            { 0x800A7D40, "ctx_caller_0x800A7D64" },
            { 0x80660000, "ctx_objptr_0x80660000" },
            { 0x8067BFC0, "ctx_obj_0x8067BFC0" },
            { 0x8007A650, "ctx_callee_0x8007A65C" },
            { 0x8007AAA0, "ctx_callee_0x8007AAAC" },
            { 0x800A5E60, "ctx_callee_0x800A5E70" },
            { 0x800A5FE0, "ctx_callee_0x800A5FEC" },
            { 0x800A7640, "ctx_callee_0x800A7650" },
            { 0x800A7DC0, "ctx_callee_0x800A7DCC" },
            { 0xA0051680, "ctx_table_0xA0051680" },
            { 0x007E9000, "ctx_src_0x007E9000" },
            { 0xA0003800, "ctx_stack_0xA0003800" },
        };
        size_t region_count = sizeof(mem_regions) / sizeof(mem_regions[0]);
        for (size_t r = 0; r < region_count; r++) {
            size_t dump_len = 64u;
            if (mem_regions[r].pa == 0x8007A650u ||
                mem_regions[r].pa == 0x8007AAA0u ||
                mem_regions[r].pa == 0x800A5E60u ||
                mem_regions[r].pa == 0x800A5FE0u ||
                mem_regions[r].pa == 0x800A7640u ||
                mem_regions[r].pa == 0x800A7DC0u ||
                mem_regions[r].pa == 0x80078BA0u ||
                mem_regions[r].pa == 0x80077FC0u ||
                mem_regions[r].pa == 0x80077DC0u ||
                mem_regions[r].pa == 0x80077E28u ||
                mem_regions[r].pa == 0x800781C0u ||
                mem_regions[r].pa == 0x80078200u ||
                mem_regions[r].pa == 0x80079A80u ||
                mem_regions[r].pa == 0x80079DD0u ||
                mem_regions[r].pa == 0x8007AF80u ||
                mem_regions[r].pa == 0x800A7D40u) {
                dump_len = 512u;
            } else if (mem_regions[r].pa >= 0x80000000u) {
                dump_len = 128u;
            }
            uint8_t mc[512];
            if (uc_mem_read(uc, (uint64_t)mem_regions[r].pa, mc, dump_len) == UC_ERR_OK) {
                fprintf(stderr, "[WINCE_NULL_POSTMORTEM] %s PA=0x%08X hex=",
                        mem_regions[r].name, mem_regions[r].pa);
                for (size_t b = 0; b < dump_len; b++)
                    fprintf(stderr, "%02X", mc[b]);
                fprintf(stderr, "\n");
            }
        }

        /* Dump all GPRs */
        static const int gpr_ids[32] = {
            UC_MIPS_REG_ZERO, UC_MIPS_REG_AT, UC_MIPS_REG_V0, UC_MIPS_REG_V1,
            UC_MIPS_REG_A0,   UC_MIPS_REG_A1, UC_MIPS_REG_A2, UC_MIPS_REG_A3,
            UC_MIPS_REG_T0,   UC_MIPS_REG_T1, UC_MIPS_REG_T2, UC_MIPS_REG_T3,
            UC_MIPS_REG_T4,   UC_MIPS_REG_T5, UC_MIPS_REG_T6, UC_MIPS_REG_T7,
            UC_MIPS_REG_S0,   UC_MIPS_REG_S1, UC_MIPS_REG_S2, UC_MIPS_REG_S3,
            UC_MIPS_REG_S4,   UC_MIPS_REG_S5, UC_MIPS_REG_S6, UC_MIPS_REG_S7,
            UC_MIPS_REG_T8,   UC_MIPS_REG_T9, UC_MIPS_REG_K0, UC_MIPS_REG_K1,
            UC_MIPS_REG_GP,   UC_MIPS_REG_SP, UC_MIPS_REG_FP, UC_MIPS_REG_RA
        };
        static const char *gpr_names[32] = {
            "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
            "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
            "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
            "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra"
        };
        for (int i = 0; i < 32; i++) {
            uint64_t gv = 0;
            uc_reg_read(uc, gpr_ids[i], &gv);
            fprintf(stderr, "[WINCE_NULL_POSTMORTEM] $%-4s = 0x%08X\n",
                    gpr_names[i], (uint32_t)gv);
        }
        uint64_t cp0_status_v = 0, cp0_cause_v = 0;
        uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &cp0_status_v);
        fprintf(stderr, "[WINCE_NULL_POSTMORTEM] STATUS=0x%08X CAUSE=0x%08X EPC=0x%08X\n",
                (uint32_t)cp0_status_v, m->pending_cause, (uint32_t)m->pending_epc);
    }

    machine_stop(m);
    uc_emu_stop(uc);
    return true;
}

static bool handle_null_call_interrupt(machine_t *m, uc_engine *uc,
                                       uint32_t intno, uint64_t pc,
                                       uint64_t status)
{
    if ((uint32_t)pc != 0u || !is_null_call_class_intno(intno))
        return false;
    if (is_wince_boot_machine(m))
        return handle_wince_null_call_interrupt(m, uc, intno, pc, status);
    return handle_linux_null_call_interrupt(m, uc, intno, pc);
}

static bool handle_wince_interrupt_passthrough(machine_t *m, uint32_t intno,
                                               uint64_t pc, uint64_t status)
{
    if (!is_wince_boot_machine(m))
        return false;

    m->wince_null_last_ra = 0;
    m->wince_null_last_intno = 0;
    m->wince_null_consecutive = 0;

    static uint32_t wince_intr_log = 0;
    if (wince_intr_log < 32u) {
        fprintf(stderr, "[WINCE_INTR] intno=%u PC=0x%08" PRIX64
                " STATUS=0x%08" PRIX64 "\n",
                intno, (uint64_t)(uint32_t)pc, (uint64_t)(uint32_t)status);
        wince_intr_log++;
    }
    return true;
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

/*
 * Invalid instruction hook — handles VR4131 MACC instructions
 * (opcode 0x1C / SPECIAL2) which are not part of standard MIPS32.
 */
static bool insn_invalid_hook(uc_engine *uc, void *user_data)
{
    (void)user_data;
    uint64_t pc = 0;
    uc_reg_read(uc, UC_MIPS_REG_PC, &pc);

    uint32_t insn = 0;
    if (!read_insn_best_effort(uc, pc, &insn)) {
        fprintf(stderr, "[CPU] Cannot read insn at PC=0x%016" PRIX64 "\n", pc);
        return false;
    }

    /* Try MACC decode first */
    if (macc_execute(uc, insn))
        return true;

    /* Unknown instruction — log and stop */
    fprintf(stderr, "[CPU] Illegal instruction PC=0x%016" PRIX64 " insn=0x%08X\n",
            pc, insn);
    return false;
}

/* ------------------------------------------------------------------ */
/* One-shot checkpoint logging                                           */
/* ------------------------------------------------------------------ */

/*
 * Checkpoint table: VA of well-known kernel functions / branch sites.
 * This table intentionally mixes symbols from multiple known test kernels
 * (linux4be 2.6.8.1 and older 2.4.x images) so the same binary can probe
 * whichever kernel is currently booted.
 *
 * Flags:
 *   print_a0_str: read $a0 as a VA→string and print alongside the name.
 */
static const struct {
    uint32_t va;
    const char *name;
    bool print_a0_str;
} checkpoint_table[] = {
    /* ICU / timer init */
    { 0x80275b20u, "vr41xx_icu_init",                 false },
    /* High-level function entries */
    { 0x80001558u, "rest_init",                       false },
    { 0x80001580u, "do_pre_smp_initcalls",            false },
    { 0x80001770u, "init (kernel thread, 2.4)",       false },
    { 0x800015d0u, "init (kernel thread)",            false },
    { 0x80272918u, "do_basic_setup",                  false },
    { 0x802727d0u, "do_initcalls",                    false },
    /* Fine-grained probes inside init() around the sys_access branch */
    { 0x80001614u, "init: JAL sys_access(/init?)",    false },
    { 0x8000161cu, "init: BNE (sys_access result)",   false },
    { 0x80001624u, "init: B   (skip prepare_ns)",     false },
    { 0x80001630u, "init: JAL prepare_namespace",     false },
    { 0x80273470u, "prepare_namespace (entry)",       false },
    { 0x80001638u, "init: JAL free_initmem",          false },
    /* exec / page-fault path */
    { 0x80001690u, "init: JAL run_init_process [execute_command]", false },
    { 0x8000169cu, "init: JAL run_init_process [/sbin/init]",      false },
    { 0x80001598u, "run_init_process (entry)",        true  },
    { 0x80007394u, "sys_execve (entry, 2.4)",         true  },
    { 0x80040730u, "call_usermodehelper (entry)",    true  },
    { 0x80040530u, "____call_usermodehelper (entry)",  true  },
    { 0x8000dcc8u, "sys_execve (entry)",              true  },
    { 0x8004a9d0u, "do_execve (entry, 2.4)",          true  },
    { 0x8004a774u, "search_binary_handler (2.4)",     false },
    { 0x80017a40u, "panic (2.4)",                     true  },
    { 0x80080cb0u, "do_execve (entry)",               true  },
    { K24_DO_PAGE_FAULT, "do_page_fault (entry, 2.4)", false },
    { 0x80016ef0u,       "do_page_fault (entry, alt)", false },
    { 0x800a7378u, "create_elf_tables (entry)",       false },
    /* Post-inet_init initcalls (last two in table) */
    { 0x80286440u, "af_unix_init",                    false },
    { 0x802864d8u, "packet_init",                     false },
    /* Inside inet_init — call sites (JAL instructions) */
    { 0x80285f58u, "inet_init: JAL sock_register",    false },
    { 0x80286038u, "inet_init: JAL arp_init",         false },
    { 0x80286040u, "inet_init: JAL ip_init",          false },
    { 0x80286050u, "inet_init: JAL tcp_init",         false },
    /* Sub-function entries */
    { 0x80285458u, "ip_init",                         false },
    { 0x802854c0u, "tcp_init",                        false },
    { 0x80285a10u, "arp_init",                        false },
    { 0x80284fb8u, "ip_rt_init",                      false },
    { 0x80162dd0u, "ipfrag_init",                     false },
    { 0x80150378u, "neigh_table_init",                false },
    { 0x8027fd98u, "alloc_large_system_hash",         false },
    { 0x80286208u, "fib_hash_init",                   false },
    /* inet_register_protosw loop site */
    { 0x80286020u, "inet_init: JAL inet_register_protosw (loop)", false },
};
#define CHECKPOINT_COUNT ((int)(sizeof(checkpoint_table)/sizeof(checkpoint_table[0])))

static bool checkpoint_fired[CHECKPOINT_COUNT];

/*
 * Read a NUL-terminated string from guest VA into a host buffer.
 * Returns the number of bytes read (excluding NUL), or 0 on failure.
 */
static int read_guest_string(uc_engine *uc, uint64_t va, char *buf, int bufsz)
{
    /* Try direct PA (kseg0/kseg1) */
    uint64_t pa = 0;
    if (va_to_pa_kseg(va, &pa)) {
        int i;
        for (i = 0; i < bufsz - 1; i++) {
            uint8_t c = 0;
            if (uc_mem_read(uc, pa + i, &c, 1) != UC_ERR_OK) break;
            buf[i] = (char)c;
            if (c == 0) { buf[i] = 0; return i; }
        }
        buf[i] = 0;
        return i;
    }
    /*
     * Fallback: try reading directly at VA.  Useful for user pointers when
     * Unicorn already has the corresponding pages mapped.
     */
    {
        int i;
        for (i = 0; i < bufsz - 1; i++) {
            uint8_t c = 0;
            if (uc_mem_read(uc, va + i, &c, 1) != UC_ERR_OK) break;
            buf[i] = (char)c;
            if (c == 0) { buf[i] = 0; return i; }
        }
        buf[i] = 0;
        if (i > 0)
            return i;
    }
    buf[0] = 0;
    return 0;
}

static void checkpoint_hook(uc_engine *uc, uint64_t address,
                            uint32_t size, void *user_data)
{
    (void)size; (void)address;
    int idx = (int)(uintptr_t)user_data;
    if (checkpoint_fired[idx]) return;
    checkpoint_fired[idx] = true;

    if (checkpoint_table[idx].print_a0_str) {
        uint64_t a0 = 0;
        uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
        char str[128] = "<unreadable>";
        read_guest_string(uc, a0, str, sizeof(str));
        fprintf(stderr, "[CHECKPOINT] >>> %s  a0=0x%08" PRIX64 " \"%s\" <<<\n",
                checkpoint_table[idx].name, a0, str);
    } else {
        fprintf(stderr, "[CHECKPOINT] >>> %s <<<\n", checkpoint_table[idx].name);
    }
}

/*
 * Arm a focused late-boot trace window when init starts execing /sbin/init.
 * This gives per-batch visibility into whether uc_emu_start keeps returning
 * (slow loop) or wedges inside a single batch right after run_init_process.
 */
static void run_init_entry_trace_hook(uc_engine *uc, uint64_t address,
                                      uint32_t size, void *user_data)
{
    (void)uc; (void)address; (void)size;
    machine_t *m = user_data;
    m->post_init_trace_window = true;
    m->post_init_trace_batches = 0;
    m->tlb_trace_window = true;
    fprintf(stderr, "[TLB_TRACE] armed and activated by run_init_process entry\n");
}

/*
 * do_initcalls tracer: fires at the JALR site (0x80272874) inside
 * do_initcalls for every initcall invocation.  Reads $v0 ($2) which
 * holds the function pointer just before the JALR executes.
 * Limited to 64 fires to avoid log flooding.
 */
static int initcall_trace_count = 0;
static void initcall_trace_hook(uc_engine *uc, uint64_t address,
                                uint32_t size, void *user_data)
{
    (void)size; (void)address; (void)user_data;
    if (initcall_trace_count >= 64) return;
    initcall_trace_count++;
    uint64_t fn = 0;
    uc_reg_read(uc, UC_MIPS_REG_V0, &fn);
    fprintf(stderr, "[INITCALL] #%02d  fn=0x%08" PRIX64 "\n",
            initcall_trace_count, (uint64_t)(uint32_t)fn);
}

/* ------------------------------------------------------------------ */
/* RCU / wait_for_completion diagnostic probes                          */
/* ------------------------------------------------------------------ */

/*
 * Multi-fire probes for the RCU grace-period path.
 * Each fires up to RCU_PROBE_LIMIT times, printing a short line so we
 * can see whether these functions are reached while init is blocked in
 * wait_for_completion.
 *
 * Addresses for linux4be20040908/vmlinux (confirmed via nm):
 *   0x801ab590  wait_for_completion   — init should block here in synchronize_kernel
 *   0x800379a8  do_timer              — called from timer interrupt handler
 *   0x800428b8  rcu_check_callbacks   — called from scheduler_tick (via do_timer)
 *   0x80042780  rcu_process_callbacks — RCU tasklet; fires the call_rcu callbacks
 *   0x8000eb30  timer_interrupt       — top-level timer interrupt handler
 */
#define RCU_PROBE_LIMIT 8

static int rcu_probe_wait_count    = 0;
static int rcu_probe_dotimer_count = 0;
static int rcu_probe_rcucheck_count= 0;
static int rcu_probe_rcuproc_count = 0;
static int rcu_probe_timerint_count= 0;

static void rcu_probe_hook(uc_engine *uc, uint64_t address,
                           uint32_t size, void *user_data)
{
    (void)uc; (void)size;
    int idx = (int)(uintptr_t)user_data;
    int *cnt;
    const char *tag;
    switch (idx) {
        case 0: cnt = &rcu_probe_wait_count;     tag = "wait_for_completion";   break;
        case 1: cnt = &rcu_probe_dotimer_count;  tag = "do_timer";              break;
        case 2: cnt = &rcu_probe_rcucheck_count; tag = "rcu_check_callbacks";   break;
        case 3: cnt = &rcu_probe_rcuproc_count;  tag = "rcu_process_callbacks"; break;
        case 4: cnt = &rcu_probe_timerint_count; tag = "timer_interrupt";       break;
        default: return;
    }
    if (*cnt >= RCU_PROBE_LIMIT) return;
    (*cnt)++;
    fprintf(stderr, "[RCU_PROBE] #%d %s  PC=0x%08" PRIX64 "\n",
            *cnt, tag, (uint64_t)(uint32_t)address);
}

/* run_init_process execve syscall site probes (2.4 kernel). */
static void init_execve_site_probe_hook(uc_engine *uc, uint64_t address,
                                        uint32_t size, void *user_data)
{
    (void)size;
    machine_t *m = user_data;
    uint32_t pc = (uint32_t)address;
    int idx = -1;
    const char *site = NULL;
    switch (pc) {
    case RUN_INIT_SYSCALL_EPC:    idx = 0; site = "run_init SYSCALL"; break;
    case RUN_INIT_SYSCALL_RET_PC: idx = 1; site = "run_init RET";     break;
    default: return;
    }

    static uint32_t counts[2];
    if (counts[idx] >= 1024u)
        return;
    counts[idx]++;

    uint64_t v0 = 0, a3 = 0, a0 = 0, a1 = 0, a2 = 0, status = 0;
    uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
    uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
    uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
    uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
    uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
    uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &status);
    fprintf(stderr,
            "[INIT_EXECVE_SITE] #%u %s PC=0x%08X"
            " v0=0x%08" PRIX64 " a3=0x%08" PRIX64
            " a0=0x%08" PRIX64 " a1=0x%08" PRIX64 " a2=0x%08" PRIX64
            " STATUS=0x%08" PRIX64 " pending_excode=%u\n",
            counts[idx], site, pc,
            (uint64_t)(uint32_t)v0, (uint64_t)(uint32_t)a3,
            (uint64_t)(uint32_t)a0, (uint64_t)(uint32_t)a1,
            (uint64_t)(uint32_t)a2, status, m->pending_excode);
}

/* ------------------------------------------------------------------ */
/* vmlinux-pgui-demo (2.4.18) diagnostic probes                        */
/* ------------------------------------------------------------------ */

#define PGUI_PROBE_COUNT 26
static bool pgui_probe_fired[PGUI_PROBE_COUNT];

static void pgui_probe_hook(uc_engine *uc, uint64_t address,
                            uint32_t size, void *user_data)
{
    (void)size;
    int idx = (int)(uintptr_t)user_data;
    if (idx < 0 || idx >= PGUI_PROBE_COUNT) return;
    if (pgui_probe_fired[idx]) return;
    pgui_probe_fired[idx] = true;

    static const char *names[] = {
        "start_kernel",              /* 0 */
        "prom_init",                 /* 1 */
        "prepare_namespace",         /* 2 — also reads initrd_start */
        "rd_init",                   /* 3 */
        "blk_dev_init",              /* 4 */
        "initrd_load",               /* 5 */
        "mount_root:entry",          /* 6 */
        "do_linuxrc",                /* 7 */
        "pns_no_initrd_path",        /* 8 — fires if initrd_start==0 in prepare_namespace */
        "free_initmem",              /* 9 */
        "post_mount_root",           /* 10 — reads ROOT_DEV, real_root_dev, mount_initrd */
        "change_root",               /* 11 */
        "mount_root:alloc_vfsmnt",   /* 12 — have superblock, allocating vfsmnt */
        "mount_root:graft_tree",     /* 13 — grafting new mount into namespace */
        "mount_root:set_fs_root",    /* 14 — inline set_fs_root (sw vfsmnt to current->fs) */
        "mount_root:no_super_loop",  /* 15 — get_super NULL, entering fs-type loop */
        "mount_root:read_super",     /* 16 — calling read_super in fs-type loop */
        "mount_root:blkdev_err",     /* 17 — blkdev_get failed error path */
        "mount_root:return",         /* 18 — normal return (jr ra) */
        "ext2_read_super",           /* 19 */
        "sys_execve",                /* 20 */
        "init:open_devnull",         /* 21 — syscall for open("/dev/console") */
        "init:execve_cmd",           /* 22 — syscall for execve(execute_command) */
        "init:execve_sbin_init",     /* 23 — syscall for execve("/sbin/init") */
        "init:execve_sbin_sh",       /* 24 — syscall for execve("/bin/sh") */
        "handle_sys",                /* 25 — MIPS syscall dispatch entry */
    };
    const char *name = names[idx];

    if (idx == 2) {
        /* Read initrd_start (PA 0x00258cb8) to see if it was set */
        uint32_t initrd_start_val = 0, initrd_end_val = 0;
        uc_mem_read(uc, 0x00258cb8u, &initrd_start_val, 4);
        uc_mem_read(uc, 0x00258cbcu, &initrd_end_val, 4);
        fprintf(stderr, "[PGUI24] %s  initrd_start=0x%08X  initrd_end=0x%08X\n",
                name, initrd_start_val, initrd_end_val);
    } else if (idx == 10) {
        /* After mount_root: read ROOT_DEV(0x002420a0), real_root_dev(0x00238040),
         * mount_initrd(0x0017cd4c) — all PAs = VA & 0x1fffffff */
        uint16_t root_dev = 0;
        uint32_t real_root = 0, mnt_initrd = 0;
        uc_mem_read(uc, 0x002420a0u, &root_dev,   2);
        uc_mem_read(uc, 0x00238040u, &real_root,  4);
        uc_mem_read(uc, 0x0017cd4cu, &mnt_initrd, 4);
        fprintf(stderr, "[PGUI24] %s  ROOT_DEV=0x%04X  real_root_dev=0x%08X  mount_initrd=%u\n",
                name, root_dev, real_root, mnt_initrd);
    } else if (idx == 14) {
        /* mount_root inlined set_fs_root: s2=vfsmnt, s1=current->fs */
        uint64_t s1 = 0, s2 = 0, s0 = 0;
        uc_reg_read(uc, UC_MIPS_REG_S1, &s1);
        uc_reg_read(uc, UC_MIPS_REG_S2, &s2);
        uc_reg_read(uc, UC_MIPS_REG_S0, &s0);
        fprintf(stderr, "[PGUI24] %s  s1(fs)=0x%08" PRIX64 " s2(vfsmnt)=0x%08" PRIX64
                        " s0(dentry)=0x%08" PRIX64 " PC=0x%08" PRIX64 "\n",
                name, (uint64_t)(uint32_t)s1, (uint64_t)(uint32_t)s2,
                (uint64_t)(uint32_t)s0, (uint64_t)(uint32_t)address);
    } else if (idx >= 21 && idx <= 24) {
        /* init() SYSCALL instructions: read v0 (nr) and a0 (first arg) */
        uint64_t v0 = 0, a0 = 0, a3 = 0;
        uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
        uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
        uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
        char s[96] = "<unreadable>";
        read_guest_string(uc, a0, s, sizeof(s));
        fprintf(stderr, "[PGUI24] %s  PC=0x%08" PRIX64 " nr=%u a0=0x%08" PRIX64 " \"%s\" a3=0x%08" PRIX64 "\n",
                name, (uint64_t)(uint32_t)address,
                (uint32_t)v0, (uint64_t)(uint32_t)a0, s, (uint64_t)(uint32_t)a3);
    } else if (idx == 20) {
        /* sys_execve: a0 is pt_regs pointer; try reading regs[4] (a0 = filename) */
        uint64_t a0 = 0;
        uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
        /* pt_regs->regs[4] is at offset 16 (4 regs * 4 bytes before a0 in saved regs) */
        uint32_t fname_ptr = 0;
        /* regs[4] is $a0 in saved pt_regs. On MIPS, pt_regs starts with regs[0..31]
         * so regs[4] is at offset 4*4=16 from pt_regs base. */
        uc_mem_read(uc, (uint64_t)(uint32_t)a0 + 16u, &fname_ptr, 4);
        char s[96] = "<unreadable>";
        read_guest_string(uc, fname_ptr, s, sizeof(s));
        fprintf(stderr, "[PGUI24] %s  PT_regs=0x%08" PRIX64 " fname_ptr=0x%08X \"%s\"\n",
                name, (uint64_t)(uint32_t)a0, fname_ptr, s);
    } else {
        fprintf(stderr, "[PGUI24] %s  PC=0x%08" PRIX64 "\n",
                name, (uint64_t)(uint32_t)address);
    }
}

/* ------------------------------------------------------------------ */
/* IRQ path probes                                                      */
/* ------------------------------------------------------------------ */

#define IRQ_PROBE_LIMIT 24

static int irq_probe_counts[8];

static void irq_probe_hook(uc_engine *uc, uint64_t address,
                           uint32_t size, void *user_data)
{
    (void)size;
    int idx = (int)(uintptr_t)user_data;
    const char *tag = NULL;
    switch (idx) {
        case 0: tag = "vr41xx_handle_interrupt"; break;
        case 1: tag = "irq_dispatch";            break;
        case 2: tag = "do_IRQ";                  break;
        case 3: tag = "timer_interrupt";         break;
        case 4: tag = "vr41xx_timer_ack";        break;
        case 5: tag = "ll_timer_interrupt";      break;
        case 6: tag = "local_timer_interrupt";   break;
        case 7: tag = "ll_local_timer_interrupt"; break;
        default: return;
    }
    if (irq_probe_counts[idx] >= IRQ_PROBE_LIMIT)
        return;
    irq_probe_counts[idx]++;

    uint64_t a0 = 0;
    uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
    fprintf(stderr, "[IRQ_PROBE] #%d %s  PC=0x%08" PRIX64 " a0=0x%08" PRIX64 "\n",
            irq_probe_counts[idx], tag, (uint64_t)(uint32_t)address,
            (uint64_t)(uint32_t)a0);
}

/* ------------------------------------------------------------------ */
/* Exception vector probes                                            */
/* ------------------------------------------------------------------ */

#define VEC_PROBE_LIMIT 512
static int vec_probe_counts[8];

static void exception_vector_probe_hook(uc_engine *uc, uint64_t address,
                                        uint32_t size, void *user_data)
{
    (void)size;
    machine_t *m = user_data;
    uint32_t va = (uint32_t)address;
    int idx = -1;
    const char *tag = NULL;
    if (va == 0x80000000u)      { idx = 0; tag = "refill"; }
    else if (va == 0x80000080u) { idx = 1; tag = "xtlb_refill"; }
    else if (va == 0x80000180u) { idx = 2; tag = "general"; }
    else if (va == 0x80000048u) { idx = 3; tag = "refill_tlbwr"; }
    else if (va == 0x8000004Cu) { idx = 4; tag = "refill_eret"; }
    else if (va == 0x00000000u) { idx = 5; tag = "bev1_refill"; }
    else if (va == 0x00000180u) { idx = 6; tag = "bev1_general"; }
    else if (va == 0x80000008u) { idx = 7; tag = "nested_refill"; }
    if (idx < 0)
        return;
    if (!tlb_trace_window_active(m))
        return;
    if (va == 0x80000180u && m->pending_excode == 1u)
        return; /* Skip normal timer IRQ traffic once late tracing is armed. */
    if (vec_probe_counts[idx] >= VEC_PROBE_LIMIT)
        return;
    vec_probe_counts[idx]++;

    uint64_t status = 0, sp = 0, k0 = 0, k1 = 0;
    uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &status);
    uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
    uc_reg_read(uc, UC_MIPS_REG_K0, &k0);
    uc_reg_read(uc, UC_MIPS_REG_K1, &k1);
    fprintf(stderr,
            "[VEC_PROBE] #%d %s PC=0x%08X STATUS=0x%08" PRIX64
            " sp=0x%08" PRIX64 " k0=0x%08" PRIX64 " k1=0x%08" PRIX64
            " pending_excode=%u hi=0x%08" PRIX64 " lo0=0x%08" PRIX64
            " lo1=0x%08" PRIX64 " badv=0x%08" PRIX64 "\n",
            vec_probe_counts[idx], tag, va, status,
            (uint64_t)(uint32_t)sp, (uint64_t)(uint32_t)k0,
            (uint64_t)(uint32_t)k1, m->pending_excode,
            (uint64_t)(uint32_t)m->shadow_cp0_entryhi,
            (uint64_t)(uint32_t)m->shadow_cp0_entrylo0,
            (uint64_t)(uint32_t)m->shadow_cp0_entrylo1,
            (uint64_t)(uint32_t)m->shadow_cp0_badvaddr);
}

/* ------------------------------------------------------------------ */
/* Page-fault probes                                                   */
/* ------------------------------------------------------------------ */

#define PF_PROBE_LIMIT 64
static int pf_probe_count = 0;

static void page_fault_probe_hook(uc_engine *uc, uint64_t address,
                                  uint32_t size, void *user_data)
{
    (void)size;
    machine_t *m = user_data;
    if (pf_probe_count >= PF_PROBE_LIMIT)
        return;
    pf_probe_count++;
    uint64_t regs = 0, write_flag = 0, badv_arg = 0;
    uc_reg_read(uc, UC_MIPS_REG_A0, &regs);
    uc_reg_read(uc, UC_MIPS_REG_A1, &write_flag);
    uc_reg_read(uc, UC_MIPS_REG_A2, &badv_arg);
    const char *name = ((uint32_t)address == K24_DO_PAGE_FAULT) ? "do_page_fault_2.4" :
                       ((uint32_t)address == 0x80016ef0u) ? "do_page_fault_alt" :
                       ((uint32_t)address == K24_HANDLE_TLBL) ? "handle_tlbl_2.4" :
                       ((uint32_t)address == K24_NOPAGE_TLBL) ? "nopage_tlbl_2.4" :
                       ((uint32_t)address == K24_HANDLE_TLBS) ? "handle_tlbs_2.4" :
                       ((uint32_t)address == 0x8001a4e0u) ? "handle_tlbl_alt"   :
                       ((uint32_t)address == 0x8001a660u) ? "handle_tlbs_alt"   :
                       ((uint32_t)address == 0x800111a0u) ? "handle_sys"    :
                       ((uint32_t)address == 0x00000000u) ? "entry_zero"    : "unknown_fault";
    fprintf(stderr,
            "[PF_PROBE] #%d %s PC=0x%08" PRIX64 " regs=0x%08" PRIX64 " write=%u"
            " arg_badv=0x%08" PRIX64 " shadow_hi=0x%08" PRIX64 " shadow_badv=0x%08" PRIX64
            " pending_excode=%u\n",
            pf_probe_count, name, (uint64_t)(uint32_t)address,
            (uint64_t)(uint32_t)regs, (unsigned)((write_flag & 1u) != 0),
            (uint64_t)(uint32_t)badv_arg,
            (uint64_t)(uint32_t)m->shadow_cp0_entryhi,
            (uint64_t)(uint32_t)m->shadow_cp0_badvaddr,
            m->pending_excode);
}

/* ------------------------------------------------------------------ */
/* ICU MSYSINT1 ETIME fixup                                              */
/* ------------------------------------------------------------------ */

/*
 * vr41xx_icu_init (called from arch_init_irq → vr41xx_init_IRQ) correctly
 * sets icu1_base = 0xAF000080 in kernel data, but then writes 0x0000 to
 * MSYSINT1REG — disabling ALL SYSINT1 interrupt sources, including ETIME
 * (bit 3).
 *
 * The timer was registered via setup_irq(11, …) during time_init(), but
 * at that point icu1_base may have been 0, so the enable_irq write was
 * silently lost.  Subsequent read-modify-write sequences on MSYSINT1REG
 * do not re-enable ETIME because it was never in the initial mask.
 *
 * Fix: intercept the `jr $ra` return instruction of vr41xx_icu_init at
 * 0x80275bec (confirmed by objdump).  At that point the function body has
 * already run (msysint1 = 0), so we force bit 3 (ETIME) back on before
 * control returns to the caller.  The one-shot guard prevents repeated
 * fires (both the normal and error paths share this return site).
 */
static bool icu_etime_fixup_fired = false;

static void icu_etime_fixup_hook(uc_engine *uc, uint64_t address,
                                  uint32_t size, void *user_data)
{
    (void)uc; (void)address; (void)size;
    if (icu_etime_fixup_fired) return;
    icu_etime_fixup_fired = true;
    machine_t *m = user_data;
    m->icu.msysint1 |= ICU_SRC1_ETIME;
    fprintf(stderr,
            "[ICU_FIXUP] vr41xx_icu_init returning; forced ETIME in MSYSINT1=0x%04X\n",
            m->icu.msysint1);
}

void machine_mmio_history_record(machine_t *m, bool is_write, uint32_t pa,
                                 unsigned size, uint64_t value, uint32_t pc)
{
    if (!m || !m->cfg.log_wince_stall)
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

#define WINCE_STALL_SAMPLE_COUNT 3u
#define WINCE_SPL_VA_BASE UINT32_C(0x80F00000)
#define WINCE_SPL_VA_END  UINT32_C(0x80F10000)

static inline bool is_wince_spl_pc(uint32_t pc32)
{
    return pc32 >= WINCE_SPL_VA_BASE && pc32 < WINCE_SPL_VA_END;
}

static void log_wince_stall_dump(machine_t *m, uint32_t pc32)
{
    uint32_t insn = 0xFFFFFFFFu;
    uint64_t cp0_status = 0;
    uint64_t cp0_cause = (uint64_t)m->pending_cause;
    uint64_t cp0_epc = m->shadow_cp0_epc ? m->shadow_cp0_epc : m->pending_epc;
    uint64_t cp0_badv = m->shadow_cp0_badvaddr;

    (void)read_insn_best_effort(m->uc, (uint64_t)(int32_t)pc32, &insn);
    uc_reg_read(m->uc, UC_MIPS_REG_CP0_STATUS, &cp0_status);

    fprintf(stderr,
            "[WINCE_STALL] PC=0x%08X INSN=0x%08X"
            " STATUS=0x%08" PRIX64 " CAUSE=0x%08" PRIX64
            " EPC=0x%08" PRIX64 " BADV=0x%08" PRIX64 "\n",
            pc32, insn,
            (uint64_t)(uint32_t)cp0_status,
            (uint64_t)(uint32_t)cp0_cause,
            (uint64_t)(uint32_t)cp0_epc,
            (uint64_t)(uint32_t)cp0_badv);
    fprintf(stderr,
            "[WINCE_STALL] ICU sys1=0x%04X m1=0x%04X sys2=0x%04X m2=0x%04X\n",
            m->icu.sysint1, m->icu.msysint1, m->icu.sysint2, m->icu.msysint2);
    fprintf(stderr,
            "[WINCE_STALL] RTC etime=0x%012" PRIX64 " ecmp=0x%012" PRIX64
            " rtcint=0x%04X\n",
            m->rtc.etime & UINT64_C(0xFFFFFFFFFFFF),
            m->rtc.ecmp & UINT64_C(0xFFFFFFFFFFFF),
            m->rtc.rtcint);
    fprintf(stderr,
            "[WINCE_STALL] SIU LSR=0x%02X IER=0x%02X IIR=0x%02X"
            " LCR=0x%02X MCR=0x%02X\n",
            m->siu.lsr, m->siu.ier, m->siu.iir, m->siu.lcr, m->siu.mcr);
    log_wince_pa_watch_summary(m, "STALL");
    log_wince_ctrl_hist_summary(m, "STALL", 24u);

    if (m->mmio_hist_count == 0) {
        fprintf(stderr, "[WINCE_STALL_MMIO] (empty)\n");
    } else {
        uint32_t oldest = (m->mmio_hist_head + WINCE_MMIO_HISTORY_LEN - m->mmio_hist_count)
                        % WINCE_MMIO_HISTORY_LEN;
        for (uint32_t i = 0; i < m->mmio_hist_count; i++) {
            uint32_t idx = (oldest + i) % WINCE_MMIO_HISTORY_LEN;
            const mmio_hist_entry_t *e = &m->mmio_hist[idx];
            fprintf(stderr,
                    "[WINCE_STALL_MMIO] %02u %c%u PA=0x%08X PC=0x%08X VAL=0x%08" PRIX64 "\n",
                    i,
                    e->is_write ? 'W' : 'R',
                    e->size_bits,
                    e->pa,
                    e->pc,
                    e->value);
        }
    }

    /* GPR dump: read all 32 general-purpose registers */
    {
        static const int gpr_ids[32] = {
            UC_MIPS_REG_ZERO, UC_MIPS_REG_AT, UC_MIPS_REG_V0, UC_MIPS_REG_V1,
            UC_MIPS_REG_A0,   UC_MIPS_REG_A1, UC_MIPS_REG_A2, UC_MIPS_REG_A3,
            UC_MIPS_REG_T0,   UC_MIPS_REG_T1, UC_MIPS_REG_T2, UC_MIPS_REG_T3,
            UC_MIPS_REG_T4,   UC_MIPS_REG_T5, UC_MIPS_REG_T6, UC_MIPS_REG_T7,
            UC_MIPS_REG_S0,   UC_MIPS_REG_S1, UC_MIPS_REG_S2, UC_MIPS_REG_S3,
            UC_MIPS_REG_S4,   UC_MIPS_REG_S5, UC_MIPS_REG_S6, UC_MIPS_REG_S7,
            UC_MIPS_REG_T8,   UC_MIPS_REG_T9, UC_MIPS_REG_K0, UC_MIPS_REG_K1,
            UC_MIPS_REG_GP,   UC_MIPS_REG_SP, UC_MIPS_REG_FP, UC_MIPS_REG_RA
        };
        static const char *gpr_names[32] = {
            "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
            "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
            "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
            "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra"
        };
        uint64_t gpr_val;
        for (int i = 0; i < 32; i++) {
            gpr_val = 0;
            uc_reg_read(m->uc, gpr_ids[i], &gpr_val);
            fprintf(stderr, "[WINCE_STALL_GPR] $%-4s = 0x%08X\n",
                    gpr_names[i], (uint32_t)gpr_val);
        }
    }

    /* Code dump: 256 bytes (64 instructions) around the stall PC.
     * Try VA first (sign-extended kseg0/kseg1), then fall back to PA. */
    {
        uint32_t dump_base = pc32 & ~UINT32_C(0xFF);
        uint32_t dump_pa_base = dump_base & UINT32_C(0x1FFFFFFF);
        for (uint32_t off = 0; off < 256; off += 64) {
            uint8_t chunk[64];
            uint64_t va = (uint64_t)(int32_t)(dump_base + off);
            uint64_t pa = (uint64_t)(dump_pa_base + off);
            uc_err merr = uc_mem_read(m->uc, va, chunk, 64);
            if (merr != UC_ERR_OK)
                merr = uc_mem_read(m->uc, pa, chunk, 64);
            if (merr != UC_ERR_OK) {
                fprintf(stderr, "[WINCE_STALL_MEM] chunk at 0x%08X: READ FAILED\n",
                        dump_pa_base + off);
                continue;
            }
            fprintf(stderr, "[WINCE_STALL_MEM] base=0x%08X off=0x%02X hex=",
                    dump_base, off);
            for (int b = 0; b < 64; b++)
                fprintf(stderr, "%02X", chunk[b]);
            fprintf(stderr, "\n");
        }
    }
}

/* ------------------------------------------------------------------ */
/* Machine lifecycle                                                     */
/* ------------------------------------------------------------------ */

machine_t *machine_create(const machine_config_t *cfg)
{
    machine_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;

    m->cfg = *cfg;
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
    if (is_wince_boot_cfg(cfg))
        m->rtc.etime_read_step = 64;
    gpio_init(&m->gpio);
    nand_init(&m->nand, NULL, 0);

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
    if (is_wince_boot_cfg(cfg) && cfg->log_wince_stall) {
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
            { WINCE_TRACE_CB_PA_START,  WINCE_TRACE_CB_PA_END - 1u,  "cb_tbl" },
            { WINCE_TRACE_OBJ_PA_START, WINCE_TRACE_OBJ_PA_END - 1u, "obj" },
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
                    " cb=0x%08X-0x%08X obj=0x%08X-0x%08X aliases=5\n",
                    WINCE_TRACE_VEC_PA_START, WINCE_TRACE_VEC_PA_END - 1u,
                    WINCE_TRACE_CTX_PA_START, WINCE_TRACE_CTX_PA_END - 1u,
                    WINCE_TRACE_CB_PA_START, WINCE_TRACE_CB_PA_END - 1u,
                    WINCE_TRACE_OBJ_PA_START, WINCE_TRACE_OBJ_PA_END - 1u);
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
        /* One-shot checkpoint hooks: fire once when each named function is entered */
        memset(checkpoint_fired, 0, sizeof(checkpoint_fired));
        for (int i = 0; i < CHECKPOINT_COUNT; i++) {
            uint64_t va = mips_sext(checkpoint_table[i].va);
            uc_hook_add(m->uc, &hk, UC_HOOK_CODE, checkpoint_hook,
                        (void *)(uintptr_t)i, va, va);
        }
        {
            uint64_t va = mips_sext(0x80001598u); /* run_init_process */
            uc_hook_add(m->uc, &hk, UC_HOOK_CODE, run_init_entry_trace_hook, m, va, va);
        }
        /* run_init_process execve site probes (real failing path). */
        {
            static const uint32_t execve_sites[] = {
                RUN_INIT_SYSCALL_EPC,
                RUN_INIT_SYSCALL_RET_PC,
            };
            for (int i = 0; i < 2; i++) {
                uint64_t va = mips_sext(execve_sites[i]);
                uc_hook_add(m->uc, &hk, UC_HOOK_CODE, init_execve_site_probe_hook, m, va, va);
            }
        }

        /* vmlinux-pgui-demo (2.4.18) one-shot probes */
        memset(pgui_probe_fired, 0, sizeof(pgui_probe_fired));
        {
            static const struct { uint32_t va; int idx; } pgui_probes[] = {
                { 0x8015873cu,  0 },  /* start_kernel       */
                { 0x801651a8u,  1 },  /* prom_init          */
                { 0x800015d0u,  2 },  /* prepare_namespace  */
                { 0x80161234u,  3 },  /* rd_init            */
                { 0x80160fc4u,  4 },  /* blk_dev_init       */
                { 0x80161c5cu,  5 },  /* initrd_load        */
                { 0x8015dd30u,  6 },  /* mount_root:entry   */
                { 0x800014dcu,  7 },  /* do_linuxrc         */
                { 0x80001764u,  8 },  /* pns_no_initrd_path (branch when initrd_start==0) */
                { 0x8000a90cu,  9 },  /* free_initmem       */
                { 0x80001634u, 10 },  /* post_mount_root (reads ROOT_DEV, real_root_dev) */
                { 0x8015ecbcu, 11 },  /* change_root        */
                /* mount_root internals */
                { 0x8015deb8u, 12 },  /* mount_root:alloc_vfsmnt (got superblock) */
                { 0x8015df74u, 13 },  /* mount_root:graft_tree                    */
                { 0x8015dfe8u, 14 },  /* mount_root:set_fs_root (sw s2,20(s1))   */
                { 0x8015e328u, 15 },  /* mount_root:no_super_loop (fs-type loop)  */
                { 0x8015e3d4u, 16 },  /* mount_root:read_super in fs-type loop    */
                { 0x8015e460u, 17 },  /* mount_root:blkdev_get failed             */
                { 0x8015e280u, 18 },  /* mount_root:return (jr ra)                */
                /* filesystem */
                { 0x80071928u, 19 },  /* ext2_read_super                          */
                /* execve path */
                { 0x80007394u, 20 },  /* sys_execve                               */
                /* init() SYSCALL instructions (probe fires before syscall executes) */
                { 0x800017a4u, 21 },  /* init:open("/dev/console")                */
                { 0x8000181cu, 22 },  /* init:execve(execute_command)             */
                { 0x8000184cu, 23 },  /* init:execve("/sbin/init")                */
                { 0x800018acu, 24 },  /* init:execve("/bin/sh") — last attempt    */
                { 0x80007ac0u, 25 },  /* handle_sys (MIPS syscall dispatch)       */
            };
            for (int i = 0; i < PGUI_PROBE_COUNT; i++) {
                uint64_t va = mips_sext(pgui_probes[i].va);
                uc_hook_add(m->uc, &hk, UC_HOOK_CODE, pgui_probe_hook,
                            (void *)(uintptr_t)pgui_probes[i].idx, va, va);
            }
        }

        /* do_initcalls tracer: logs which function pointer is called at JALR site */
        initcall_trace_count = 0;
        {
            uint64_t jalr_va = mips_sext(0x80272874u);
            uc_hook_add(m->uc, &hk, UC_HOOK_CODE, initcall_trace_hook, NULL,
                        jalr_va, jalr_va);
        }

        /* RCU / wait_for_completion diagnostic probes (multi-fire, up to 8 each) */
        rcu_probe_wait_count     = 0;
        rcu_probe_dotimer_count  = 0;
        rcu_probe_rcucheck_count = 0;
        rcu_probe_rcuproc_count  = 0;
        rcu_probe_timerint_count = 0;
        {
            static const struct { uint32_t va; int idx; } rcu_probes[] = {
                { 0x801ab590u, 0 },  /* wait_for_completion   */
                { 0x800379a8u, 1 },  /* do_timer               */
                { 0x800428b8u, 2 },  /* rcu_check_callbacks    */
                { 0x80042780u, 3 },  /* rcu_process_callbacks  */
                { 0x8000eb30u, 4 },  /* timer_interrupt        */
            };
            for (int i = 0; i < 5; i++) {
                uint64_t va = mips_sext(rcu_probes[i].va);
                uc_hook_add(m->uc, &hk, UC_HOOK_CODE, rcu_probe_hook,
                            (void *)(uintptr_t)rcu_probes[i].idx, va, va);
            }
        }

        /* IRQ dispatch path probes (multi-fire, up to 24 each) */
        memset(irq_probe_counts, 0, sizeof(irq_probe_counts));
        {
            static const struct { uint32_t va; int idx; } irq_probes[] = {
                { 0x800076c0u, 0 },  /* vr41xx_handle_interrupt */
                { 0x80007508u, 1 },  /* irq_dispatch            */
                { 0x80009c30u, 2 },  /* do_IRQ                  */
                { 0x8000eb30u, 3 },  /* timer_interrupt         */
                { 0x80007a98u, 4 },  /* vr41xx_timer_ack        */
                { 0x8000ed88u, 5 },  /* ll_timer_interrupt      */
                { 0x8000ea38u, 6 },  /* local_timer_interrupt   */
                { 0x8000ee18u, 7 },  /* ll_local_timer_interrupt*/
            };
            for (int i = 0; i < 8; i++) {
                uint64_t va = mips_sext(irq_probes[i].va);
                uc_hook_add(m->uc, &hk, UC_HOOK_CODE, irq_probe_hook,
                            (void *)(uintptr_t)irq_probes[i].idx, va, va);
            }
        }

        /* Syscall dispatch and return probes. */
        {
            uint64_t va_sys = mips_sext(0x800111a0u); /* handle_sys */
            uc_hook_add(m->uc, &hk, UC_HOOK_CODE, page_fault_probe_hook, m, va_sys, va_sys);
        }
        {
            uint64_t va_zero = 0; /* User entry point 0? */
            uc_hook_add(m->uc, &hk, UC_HOOK_CODE, page_fault_probe_hook, m, va_zero, va_zero);
        }

        /* Exception vector probes (TLB refill + general exception vectors). */
        memset(vec_probe_counts, 0, sizeof(vec_probe_counts));
        {
            static const uint32_t vecs[] = {
                0x80000000u, /* refill vector */
                0x80000080u, /* xtlb refill   */
                0x80000180u, /* general       */
                0x80000048u, /* refill_tlbwr  */
                0x8000004Cu, /* refill_eret   */
                0x00000000u, /* BEV=1 refill  */
                0x00000180u, /* BEV=1 general */
                0x80000008u, /* nested refill?*/
            };
            for (int i = 0; i < 8; i++) {
                uint64_t va = mips_sext(vecs[i]);
                uc_hook_add(m->uc, &hk, UC_HOOK_CODE, exception_vector_probe_hook,
                            m, va, va);
            }
        }

        /* Page-fault probe: log the first few fault-path entries for 2.4/2.6 kernels. */
        pf_probe_count = 0;
        {
            static const uint32_t fault_sites[] = {
                K24_DO_PAGE_FAULT,
                K24_HANDLE_TLBL,
                K24_NOPAGE_TLBL,
                K24_HANDLE_TLBS,
                0x80016ef0u, /* alt do_page_fault */
                0x8001a4e0u, /* alt handle_tlbl   */
                0x8001a660u, /* alt handle_tlbs   */
            };
            for (size_t i = 0; i < (sizeof(fault_sites) / sizeof(fault_sites[0])); i++) {
                uint64_t va = mips_sext(fault_sites[i]);
                uc_hook_add(m->uc, &hk, UC_HOOK_CODE, page_fault_probe_hook, m, va, va);
            }
        }

        /* ICU ETIME fixup: force-enable ETIME bit in MSYSINT1 after
         * vr41xx_icu_init clears it.  Fires at the jr $ra (0x80275bec). */
        icu_etime_fixup_fired = false;
        {
            uint64_t va = mips_sext(0x80275becu);
            uc_hook_add(m->uc, &hk, UC_HOOK_CODE, icu_etime_fixup_hook, m, va, va);
        }
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
         * WinCE SPL boot state: kernel mode, BEV=1 (boot vectors),
         * ERL=1 (reset state).  The SPL will reconfigure CP0 itself.
         */
        uint64_t wince_status = 0x00400004u; /* BEV=1, ERL=1 */
        uc_reg_write(m->uc, UC_MIPS_REG_CP0_STATUS, &wince_status);

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
         * The VR4131 RTC runs at ~32.768 kHz; 33 ticks ≈ 1 ms per batch. */

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
        rtc_tick(&m->rtc, 33);
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
