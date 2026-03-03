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

#define RUN_INIT_SYSCALL_EPC      0x800015B0u
#define RUN_INIT_SYSCALL_RET_PC   0x800015B4u
#define TLB_DEFER_RETRY_LIMIT     32u
#define TLB_EXL_DROP_DEFER_LIMIT  64u

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
    reset_tlb_defer_state(m);
    if (clear_execve_watch) {
        m->execve_watch_active = false;
        m->execve_entry_pc = 0;
        m->execve_last_pc = 0;
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

static bool map_kseg_alias_block(machine_t *m, uint64_t map_base, uint64_t pa_base)
{
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
static void tlb_map_kuseg_page(machine_t *m, uint64_t kuseg_va, uint64_t pa)
{
    if ((uint32_t)kuseg_va >= 0x80000000u)
        return;  /* safety: only handle user-space addresses */

    uint64_t va_blk = kuseg_va & ~(uint64_t)0xFFFFF;   /* 1 MB-aligned VA */
    uint64_t pa_blk = pa       & ~(uint64_t)0xFFFFF;   /* 1 MB-aligned PA */

    uc_err me = uc_mem_map(m->uc, va_blk, 0x100000, UC_PROT_ALL);
    if (me == UC_ERR_MAP)
        return;  /* already present in Unicorn's region table — OK */
    if (me != UC_ERR_OK) {
        static uint32_t err_log = 0;
        if (err_log++ < 16)
            fprintf(stderr, "[TLB_MAP_FAIL] VA_blk=0x%08" PRIX64 " err=%s\n",
                    va_blk, uc_strerror(me));
        return;
    }

    /* Populate the new VA block from the corresponding SDRAM PA block.
     * Copy 4 KB at a time to avoid a large stack allocation. */
    if (pa_blk < (uint64_t)m->cfg.sdram_size) {
        uint8_t buf[4096];
        for (uint64_t off = 0; off < 0x100000; off += sizeof(buf)) {
            if (uc_mem_read(m->uc, pa_blk + off, buf, sizeof(buf)) != UC_ERR_OK)
                memset(buf, 0, sizeof(buf));
            uc_mem_write(m->uc, va_blk + off, buf, sizeof(buf));
        }
    }

    static uint32_t map_log = 0;
    if (map_log++ < 64)
        fprintf(stderr, "[TLB_MAP] kuseg 1MB VA=0x%08" PRIX64
                " <- SDRAM PA=0x%08" PRIX64 "\n", va_blk, pa_blk);
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

static int read_guest_string(uc_engine *uc, uint64_t va, char *buf, int bufsz);

static bool read_guest_u32(uc_engine *uc, uint64_t va, uint32_t *out)
{
    if (uc_mem_read(uc, va, out, sizeof(*out)) == UC_ERR_OK)
        return true;
    uint64_t pa = 0;
    if (va_to_pa_kseg(va, &pa) &&
        uc_mem_read(uc, pa, out, sizeof(*out)) == UC_ERR_OK)
        return true;
    return false;
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
     * Mark execve_open_exec_ran so ERET_EXECVE_RETRY / TLB_DEFER_SKIP know
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
     * Track most-recent do_execve PC while an execve syscall is in flight.
     * Skip SYSCALL+4 notification addresses so DEFER retries can resume useful
     * in-flight progress instead of rewinding to the syscall site.
     */
    if (m->execve_watch_active) {
        uint32_t pc32 = (uint32_t)address;
        uint32_t notify_pc = (uint32_t)m->pending_syscall_epc + 4u;
        bool is_exception_vector = (pc32 >= 0x80000000u && pc32 < 0x80000200u);
        if (pc32 != notify_pc && !is_exception_vector)
            m->execve_last_pc = address;
    }

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
    uint32_t op  = (insn >> 26) & 0x3Fu;
    uint32_t rs  = (insn >> 21) & 0x1Fu;
    uint32_t rt  = (insn >> 16) & 0x1Fu;
    uint32_t rd  = (insn >> 11) & 0x1Fu;
    uint32_t sel =  insn        & 0x07u;

    /*
     * sys_execve path probes (2.4 kernel):
     *   0x800073dc: error fast-return path (getname failed)
     *   0x800073f4: call do_execve path (getname succeeded)
     *   0x80007400: do_execve return consumed into kmem_cache_free
     */
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
        uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
        uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
        uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
        uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
        uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
        read_guest_string(uc, a0, path, sizeof(path));

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
        m->execve_last_pc         = address;
        m->execve_watch_a0        = a0;
        m->execve_saved_a1        = a1;       /* argv — restored on stale-TLB retry */
        m->execve_saved_a2        = a2;       /* envp — restored on stale-TLB retry */
        m->execve_saved_sp        = sp;       /* $sp  — restored on stale-TLB retry (prevents frame drift) */
        m->execve_open_exec_ran   = false;    /* reset fd-leak flag for this invocation */
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
        cp0_shadow_write(m, rd, sel, val);
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
        /*
         * Work around suspected Unicorn tlbwi(index with P-bit) behavior by
         * rewriting this single instruction to tlbwr for one execution.
         */
        if (insn == 0x42000002u &&
            tlb_trace_window_active(m) &&
            !m->tlbwi_patch_pending) {
            uint32_t replacement = 0x42000006u; /* tlbwr */
            uc_err patch_err = write_mem_best_effort(uc, address, &replacement, 4);
            if (patch_err == UC_ERR_OK) {
                m->tlbwi_patch_pending = true;
                m->tlbwi_patch_addr = address;
                m->tlbwi_patch_orig = insn;
                if (tlbwi_patch_log < 64) {
                    fprintf(stderr,
                            "[TLBWI_PATCH] PC=0x%08" PRIX64
                            " idx=0x%08" PRIX64 " -> tlbwr\n",
                            (uint64_t)(uint32_t)address,
                            (uint64_t)(uint32_t)m->shadow_cp0_index);
                    tlbwi_patch_log++;
                }
            } else if (tlbwi_patch_log < 64) {
                fprintf(stderr,
                        "[TLBWI_PATCH_FAIL] PC=0x%08" PRIX64 " idx=0x%08" PRIX64
                        " err=%s\n",
                        (uint64_t)(uint32_t)address,
                        (uint64_t)(uint32_t)m->shadow_cp0_index,
                        uc_strerror(patch_err));
                tlbwi_patch_log++;
            }
        }
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
            /* Arm the softmmu TLB flush (fix A). */
            m->pending_tlb_flush = true;

            /* Direct kuseg mapping (fix B).
             * shadow_cp0_badvaddr is populated by the MFC0 $8 readback that
             * fires when tlb_trace_window is active (which is guaranteed
             * before create_elf_tables by the run_init_process checkpoint). */
            uint32_t badvaddr = (uint32_t)m->shadow_cp0_badvaddr;
            if (badvaddr != 0u && badvaddr < 0x80000000u) {
                /* Even page: VA = VPN2 (badvaddr & ~0x1FFF), PA from EntryLo0 */
                uint32_t even_va = badvaddr & ~0x1FFFu;
                uint32_t lo0     = (uint32_t)m->shadow_cp0_entrylo0;
                uint32_t lo1     = (uint32_t)m->shadow_cp0_entrylo1;
                /* V bit (valid) = bit 1 of EntryLo */
                if (lo0 & 0x2u) {
                    uint64_t pfn0 = (uint64_t)((lo0 >> 6) & 0xFFFFFu);
                    tlb_map_kuseg_page(m, (uint64_t)even_va, pfn0 << 12);
                }
                /* Odd page: VA = even_va + 4096, PA from EntryLo1 */
                uint32_t odd_va = even_va + 0x1000u;
                if (lo1 & 0x2u) {
                    uint64_t pfn1 = (uint64_t)((lo1 >> 6) & 0xFFFFFu);
                    tlb_map_kuseg_page(m, (uint64_t)odd_va, pfn1 << 12);
                }
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
        if (mfc0_cause_seen_log < 256) {
            fprintf(stderr,
                    "[CAUSE_MFC0] seen PC=0x%08" PRIX64 " pending_excode=%u served=%u cause=0x%08X\n",
                    (uint64_t)(uint32_t)address, m->pending_excode,
                    m->pending_cause_served ? 1u : 0u, m->pending_cause);
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
        if (mfc0_cause_inject_log < 256) {
            fprintf(stderr,
                    "[CAUSE_MFC0] inject PC=0x%08" PRIX64 " cause=0x%08" PRIX64 " rt=%u\n",
                    (uint64_t)(uint32_t)address, (uint64_t)(uint32_t)cause, rt);
            mfc0_cause_inject_log++;
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
        if (mfc0_epc_seen_log < 256) {
            fprintf(stderr,
                    "[EPC_MFC0] seen PC=0x%08" PRIX64 " pending_excode=%u served=%u epc=0x%08" PRIX64 "\n",
                    (uint64_t)(uint32_t)address, m->pending_excode,
                    m->pending_epc_served ? 1u : 0u, (uint64_t)(uint32_t)m->pending_epc);
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
        if (mfc0_epc_inject_log < 256) {
            fprintf(stderr,
                    "[EPC_MFC0] inject PC=0x%08" PRIX64 " epc=0x%08" PRIX64 " rt=%u\n",
                    (uint64_t)(uint32_t)address, (uint64_t)(uint32_t)epc, rt);
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
            m->pending_syscall_nr == 4011u &&
            !m->execve_watch_active) {
            uint32_t val32 = (uint32_t)val;
            uint32_t ret_site = (uint32_t)m->pending_syscall_epc + 4u;
            bool val_is_user = (val32 < 0x80000000u);
            bool val_matches_ret_site = (val32 == ret_site);
            if (!val_is_user && !val_matches_ret_site) {
                static uint32_t mtc0_epc_execve_ignore_log = 0;
                if (mtc0_epc_execve_ignore_log < 64) {
                    uint64_t status_now = 0;
                    uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &status_now);
                    fprintf(stderr,
                            "[MTC0_EPC_EXECVE_IGNORE] PC=0x%08" PRIX64
                            " pending_epc=0x%08" PRIX64 " syscall_epc=0x%08" PRIX64
                            " new_epc=0x%08" PRIX64 " STATUS=0x%08" PRIX64 "\n",
                            (uint64_t)(uint32_t)address,
                            (uint64_t)(uint32_t)m->pending_epc,
                            (uint64_t)(uint32_t)m->pending_syscall_epc,
                            (uint64_t)(uint32_t)val,
                            status_now);
                    mtc0_epc_execve_ignore_log++;
                }
                return; /* let native MTC0 EPC execute; keep synthetic execve state */
            }
        }
        if (m->pending_excode == MIPS_EXCCODE_SYS &&
            mtc0_epc_sys_log < 96) {
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
                    " ra=0x%08" PRIX64 " served_epc=%u epc_written=%u\n",
                    m->pending_excode, (uint64_t)(uint32_t)m->pending_epc,
                    (uint64_t)(uint32_t)val, mode,
                    (uint64_t)(uint32_t)address, status_now,
                    sp, ra, m->pending_epc_served ? 1u : 0u,
                    m->epc_was_written ? 1u : 0u);
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
                m->epc_was_written &&
                syscall_ret_log < 128) {
                uint64_t v0 = 0, a3 = 0;
                uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
                uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
                fprintf(stderr,
                        "[SYSCALL_RET] EPC=0x%08" PRIX64 " nr=%u"
                        " a0=0x%08" PRIX64 " \"%s\""
                        " v0=0x%08" PRIX64 " a3=0x%08" PRIX64
                        " status=0x%08" PRIX64 "\n",
                        (uint64_t)(uint32_t)m->pending_epc,
                        m->pending_syscall_nr,
                        (uint64_t)(uint32_t)m->pending_syscall_a0,
                        m->pending_syscall_a0_str,
                        (uint64_t)(uint32_t)v0,
                        (uint64_t)(uint32_t)a3,
                        status_snapshot);
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

            /*
             * Stale-TLB ERET: do_execve is in flight and a spurious TLB miss
             * at SYSCALL+4 was deferred (see TLB_NESTED_DEFER above).  The
             * kernel's TLB refill handler has now filled the TLB entry for
             * the fault address.  MIPS TLB handlers only clobber $k0/$k1, so
             * all user-visible registers (a0/a1/a2/ra/sp) still hold the
             * values they had when do_execve was first called.
             *
             * Redirect to do_execve's entry point instead of the fault PC
             * (run_init's 0x800015B4 / SYSCALL+4).  This restarts do_execve
             * from the beginning with correct arguments, and this time the
             * TLB entry is present so the stale miss will not repeat.
             *
             * Keep pending_excode=8 and all other syscall tracking active —
             * the execve syscall is still in progress.
             */
            if (m->pending_excode == MIPS_EXCCODE_SYS &&
                m->execve_watch_active &&
                m->execve_entry_pc != 0) {
                static uint32_t eret_execve_retry_log = 0;
                uint64_t resume_pc = m->execve_last_pc != 0 ?
                                     m->execve_last_pc : m->execve_entry_pc;
                bool resume_entry =
                    ((uint32_t)resume_pc == (uint32_t)m->execve_entry_pc);
                uint64_t ra_v  = m->execve_watch_ret_pc;
                uint64_t a0_v  = m->execve_watch_a0;
                uint64_t a1_v  = m->execve_saved_a1;
                uint64_t a2_v  = m->execve_saved_a2;
                if (eret_execve_retry_log < 16) {
                    fprintf(stderr,
                            "[ERET_EXECVE_RETRY] TLB entry filled;"
                            " redirecting ERET 0x%08" PRIX64 " -> 0x%08" PRIX64
                            " mode=%s"
                            " pending_epc=0x%08" PRIX64
                            " a0=0x%08" PRIX64 " a1=0x%08" PRIX64
                            " a2=0x%08" PRIX64 " ra=0x%08" PRIX64 "\n",
                            (uint64_t)(uint32_t)address,
                            (uint64_t)(uint32_t)resume_pc,
                            resume_entry ? "entry" : "last_pc",
                            (uint64_t)(uint32_t)m->pending_epc,
                            (uint64_t)(uint32_t)a0_v,
                            (uint64_t)(uint32_t)a1_v,
                            (uint64_t)(uint32_t)a2_v,
                            (uint64_t)(uint32_t)ra_v);
                    eret_execve_retry_log++;
                }
                uint64_t status = status_snapshot & ~(uint64_t)0x2u;  /* clear EXL */
                uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &status);
                /*
                 * Restore entry registers only when we must rewind all the way to
                 * do_execve() entry; re-applying them while resuming in-flight code
                 * clobbers progress through count()/copy_strings().
                 */
                if (resume_entry) {
                    uc_reg_write(uc, UC_MIPS_REG_RA, &ra_v);
                    uc_reg_write(uc, UC_MIPS_REG_A0, &a0_v);
                    uc_reg_write(uc, UC_MIPS_REG_A1, &a1_v);
                    uc_reg_write(uc, UC_MIPS_REG_A2, &a2_v);
                    /*
                     * fd-leak compensation when we rewind to do_execve() entry.
                     */
                    if (m->execve_open_exec_ran) {
                        uint32_t nr_files = 0;
                        if (uc_mem_read(m->uc, 0x80178104u, &nr_files, sizeof(nr_files)) == UC_ERR_OK
                            && nr_files > 0) {
                            nr_files--;
                            uc_mem_write(m->uc, 0x80178104u, &nr_files, sizeof(nr_files));
                            fprintf(stderr,
                                    "[ENFILE_FIX] ERET_RETRY: closed orphaned open_exec fd;"
                                    " nr_files now %u\n", nr_files);
                        }
                        m->execve_open_exec_ran = false;
                    }
                }
                uc_reg_write(uc, UC_MIPS_REG_PC, &resume_pc);
                /* Re-arm one-shot Cause/EPC injection only when rewinding entry. */
                if (resume_entry) {
                    m->pending_cause_served = false;
                    m->pending_epc_served   = false;
                }
                return;
            }

            /*
             * If no explicit MTC0 EPC was observed, let Unicorn execute the
             * native ERET, but still clear synthetic exception bookkeeping so
             * we don't permanently block future injected interrupts.
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
            bool keep_execve_native_eret =
                (m->pending_excode == MIPS_EXCCODE_SYS &&
                 m->pending_syscall_nr == 4011u &&
                 !m->epc_was_written);
            if (keep_execve_native_eret) {
                static uint32_t eret_native_keep_log = 0;
                if (eret_native_keep_log < 64) {
                    fprintf(stderr,
                            "[ERET_NATIVE_KEEP] preserving syscall state"
                            " nr=%u pending_epc=0x%08" PRIX64
                            " syscall_epc=0x%08" PRIX64
                            " PC=0x%08" PRIX64 " STATUS=0x%08" PRIX64
                            " served_epc=%u served_cause=%u\n",
                            m->pending_syscall_nr,
                            (uint64_t)(uint32_t)m->pending_epc,
                            (uint64_t)(uint32_t)m->pending_syscall_epc,
                            (uint64_t)(uint32_t)address, status_snapshot,
                            m->pending_epc_served ? 1u : 0u,
                            m->pending_cause_served ? 1u : 0u);
                    eret_native_keep_log++;
                }
                return;
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
            if (!m->tlb_trace_window && (uint32_t)pc == RUN_INIT_SYSCALL_RET_PC) {
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
                ((uint32_t)pc == RUN_INIT_SYSCALL_RET_PC) ||
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
             * NULL function-pointer recovery.
             *
             * If PC is in the low kuseg range (< 0x10000) this is almost
             * certainly a NULL indirect call from kernel code (jalr $t3 or
             * similar with a zero call-target).  Unicorn's MIPS BEQZ delay-slot
             * emulation has a known bug: when the beqz is taken, it can execute
             * the instruction two words after the branch (instead of the true
             * delay slot one word after), so a NULL-guard like
             *
             *   beqz t3, epilogue       ; intended: if t3==0 skip call
             *   li   v0, -2             ; delay-slot (skipped by Unicorn bug)
             *   jalr t3                 ; "delay slot" that Unicorn runs instead
             *
             * ends up executing "jalr t3" even when t3==0, landing here.
             *
             * Recovery: read $ra (return address deposited by jalr PC+8) and,
             * if it is a valid kseg0 kernel address, treat the NULL call as a
             * no-op function that returns 0 and redirect PC to $ra.  We also
             * set v0=0 so the caller sees a clean zero return.
             *
             * This is safe because the only side-effect of skipping the call is
             * that the "unknown parameter" handler is not invoked — which is
             * exactly what the kernel intended when it checked for NULL.
             */
            static uint32_t null_ptr_log = 0;
            if ((uint32_t)pc < 0x10000u) {
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
                /* If $ra matches the known parse_one jalr-t3 call-site
                 * (0x80042F8C = jalr_addr+8), the prid_hook intercept at
                 * 0x80042F7C fired too late (basic-block boundary).
                 *
                 * We can't safely redirect to 0x80042F8C (the epilogue) because
                 * the epilogue uses `ld ra,16(sp)` — a 64-bit load — and Unicorn
                 * stores sd/ld with zero-extension in MIPS32 mode, so the reloaded
                 * ra ends up in the kuseg range and generates another TLB miss.
                 *
                 * Instead, reconstruct the full frame unwind manually:
                 *   1. Read the 4-byte saved-ra from the stack (sp+16, little-endian).
                 *   2. Sign-extend it to a kseg0 address.
                 *   3. Restore sp (add 24, undoing parse_one's prologue).
                 *   4. Set v0 = -2 (the "unknown param" return value).
                 *   5. Jump directly to the restored ra (parse_args return site).
                 */
                uint32_t ra32 = (uint32_t)ra;
                if (ra32 == 0x80042F8Cu) {
                    /*
                     * NULL jalr t3 from parse_one at 0x80042F84.
                     * ra=0x80042F8C = jalr+8 (parse_one's epilogue return).
                     *
                     * We cannot safely run parse_one's epilogue (`ld ra,16(sp)`
                     * then `jr ra`) because Unicorn's 64-bit sd/ld zero-extends
                     * addresses stored by jal, making the loaded ra land in
                     * kuseg and causing another TLB miss.
                     *
                     * Instead, manually unwind parse_one's stack frame:
                     *   sp+16 (PA) holds the saved ra (parse_args return site).
                     *   Frame size = 24 bytes (addiu sp,-24 in prologue).
                     *
                     * BUG FIX (v2): read the saved ra from the kseg0 alias VA,
                     * NOT from the raw PA.  The kernel's `sd ra,16(sp)` writes
                     * to mips_sext(sp32)+16 (the sign-extended kseg0 VA that
                     * map_kseg_alias_block maps separately from SDRAM PA=0).
                     * Reading from PA (sp32 & 0x1FFFFFFF) would return whatever
                     * the ELF loader placed there — not the live stack data.
                     */
                    uint32_t saved_ra_lo = 0;
                    uint64_t sp_val = 0;
                    uc_reg_read(uc, UC_MIPS_REG_SP, &sp_val);
                    uint32_t sp32   = (uint32_t)sp_val;
                    uint64_t va_sp  = mips_sext(sp32);   /* kseg0 alias VA */
                    uc_mem_read(uc, va_sp + 16u, &saved_ra_lo, 4u);
                    /* restore frame */
                    uint64_t new_sp = mips_sext(sp32 + 24u);
                    uint64_t new_ra = mips_sext(saved_ra_lo);
                    uint64_t neg2   = (uint64_t)(uint32_t)-2;
                    fprintf(stderr,
                            "[NULL_CALL_RECOVER] parse_one jalr t3 NULL;"
                            " va_sp=0x%016" PRIX64 " saved_ra=0x%08" PRIX32
                            " new_sp=0x%08" PRIX64 "\n",
                            va_sp, saved_ra_lo, (uint64_t)(uint32_t)new_sp);
                    if (saved_ra_lo >= 0x80000000u) {
                        /* Valid kseg0 return site — unwind and continue. */
                        uc_reg_write(uc, UC_MIPS_REG_SP, &new_sp);
                        uc_reg_write(uc, UC_MIPS_REG_RA, &new_ra);
                        uc_reg_write(uc, UC_MIPS_REG_V0, &neg2);
                        uc_reg_write(uc, UC_MIPS_REG_PC, &new_ra);
                        return;   /* skip normal TLB-miss handling */
                    }
                    /* saved_ra was garbage (read failed or kuseg) — fall through
                     * to the general recovery below which redirects to ra. */
                }
                /*
                 * General NULL-call recovery: for any kseg0 ra, treat this as
                 * a null function call that returned 0.  Just redirect PC to ra
                 * (the return address deposited by the faulting jalr) with v0=0.
                 * This handles do_early_param's `jalr v0` when v0==0
                 * (ra=0x802724DC) and any other null indirect calls.
                 */
                if (ra32 >= 0x80000000u) {
                    uint64_t new_pc = mips_sext(ra32);
                    uint64_t zero   = 0;
                    uc_reg_write(uc, UC_MIPS_REG_PC, &new_pc);
                    uc_reg_write(uc, UC_MIPS_REG_V0, &zero);
                    static uint32_t gen_recover_log = 0;
                    if (gen_recover_log < 16u) {
                        fprintf(stderr,
                                "[NULL_CALL_RECOVER_GENERAL] PC=0 ra=0x%08" PRIX32
                                " → redirecting to ra (v0=0)\n", ra32);
                        gen_recover_log++;
                    }
                    return;   /* skip normal TLB-miss handling */
                }
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
                    if (intno != 26u) {
                        static uint32_t tlb_nested_non26_keep_log = 0;
                        bool old_cause_served = m->pending_cause_served;
                        bool old_epc_served = m->pending_epc_served;
                        m->pending_cause_served = false;
                        m->pending_epc_served = false;
                        if (tlb_nested_non26_keep_log < 64) {
                            uint64_t v0 = 0, a3 = 0;
                            uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
                            uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
                            fprintf(stderr,
                                    "[TLB_NESTED_KEEP_NON26] intno=%u PC=0x%08" PRIX64
                                    " STATUS=0x%08" PRIX64
                                    " syscall_epc=0x%08" PRIX64 " pending_epc=0x%08" PRIX64
                                    " v0=0x%08" PRIX64 " a3=0x%08" PRIX64
                                    " served_cause:%u->%u served_epc:%u->%u\n",
                                    intno, (uint64_t)(uint32_t)pc, status,
                                    (uint64_t)(uint32_t)m->pending_syscall_epc,
                                    (uint64_t)(uint32_t)m->pending_epc,
                                    (uint64_t)(uint32_t)v0,
                                    (uint64_t)(uint32_t)a3,
                                    old_cause_served ? 1u : 0u, m->pending_cause_served ? 1u : 0u,
                                    old_epc_served ? 1u : 0u, m->pending_epc_served ? 1u : 0u);
                            tlb_nested_non26_keep_log++;
                        }
                        return;
                    }
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
                    bool syscall_is_execve = (m->pending_syscall_nr == 4011u);
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
                     * Spurious re-delivery guard.
                     *
                     * After the first DEFER fires (tlb_defer_count==1) and
                     * ERET_EXECVE_RETRY redirects PC to do_execve, Unicorn can
                     * re-deliver the same notification-only intno=26 for
                     * run_init's SYSCALL+4 (0x800015B4) before any instruction at
                     * do_execve executes.
                     *
                     * Bound the retries per owner syscall EPC and hard-clear stale
                     * synthetic state when the retry budget is exceeded.
                     */
                    if (m->tlb_defer_count > 0 &&
                        m->execve_entry_pc != 0 &&
                        m->tlb_defer_owner_epc == syscall_epc) {
                        static uint32_t defer_skip_log = 0;
                        static uint32_t defer_abort_log = 0;
                        m->tlb_defer_count++;
                        if (m->tlb_defer_count > TLB_DEFER_RETRY_LIMIT) {
                            if (defer_abort_log < 32) {
                                fprintf(stderr,
                                        "[TLB_DEFER_ABORT] owner_epc=0x%08X pc=0x%08" PRIX64
                                        " retries=%u limit=%u action=clear_syscall\n",
                                        m->tlb_defer_owner_epc,
                                        (uint64_t)(uint32_t)pc,
                                        m->tlb_defer_count,
                                        TLB_DEFER_RETRY_LIMIT);
                                defer_abort_log++;
                            }
                            clear_synthetic_syscall_state(m, true);
                            m->has_saved_exception = false;
                            return;
                        }
                        uint64_t resume_pc = m->execve_last_pc != 0 ?
                                             m->execve_last_pc : m->execve_entry_pc;
                        bool resume_entry = ((uint32_t)resume_pc == (uint32_t)m->execve_entry_pc);
                        if (defer_skip_log < 32) {
                            fprintf(stderr,
                                    "[TLB_DEFER_SKIP] owner_epc=0x%08X retry=%u"
                                    " intno=%u PC=0x%08" PRIX64 " resume=0x%08" PRIX64
                                    " mode=%s\n",
                                    m->tlb_defer_owner_epc,
                                    m->tlb_defer_count,
                                    intno, (uint64_t)(uint32_t)pc,
                                    (uint64_t)(uint32_t)resume_pc,
                                    resume_entry ? "entry" : "last_pc");
                            defer_skip_log++;
                        }
                        if (resume_entry) {
                            /* Restore do_execve call registers only when rewinding to entry.
                             * Replaying the prologue without restoring $sp causes frame drift. */
                            uint64_t ra_v = m->execve_watch_ret_pc;
                            uint64_t a0_v = m->execve_watch_a0;
                            uint64_t a1_v = m->execve_saved_a1;
                            uint64_t a2_v = m->execve_saved_a2;
                            uint64_t sp_v = m->execve_saved_sp;
                            uc_reg_write(uc, UC_MIPS_REG_RA, &ra_v);
                            uc_reg_write(uc, UC_MIPS_REG_A0, &a0_v);
                            uc_reg_write(uc, UC_MIPS_REG_A1, &a1_v);
                            uc_reg_write(uc, UC_MIPS_REG_A2, &a2_v);
                            uc_reg_write(uc, UC_MIPS_REG_SP, &sp_v);
                            /* fd-leak compensation: same rationale as ERET_EXECVE_RETRY. */
                            if (m->execve_open_exec_ran) {
                                uint32_t nr_files = 0;
                                if (uc_mem_read(m->uc, 0x80178104u, &nr_files, sizeof(nr_files)) == UC_ERR_OK
                                    && nr_files > 0) {
                                    nr_files--;
                                    uc_mem_write(m->uc, 0x80178104u, &nr_files, sizeof(nr_files));
                                    fprintf(stderr,
                                            "[ENFILE_FIX] DEFER_SKIP: closed orphaned open_exec fd;"
                                            " nr_files now %u\n", nr_files);
                                }
                                m->execve_open_exec_ran = false;
                            }
                        }
                        /* Resume the best known in-flight do_execve PC. */
                        uc_reg_write(uc, UC_MIPS_REG_PC, &resume_pc);
                        return;
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
                     * Unicorn fires this intno=26 as a notification-only event
                     * (EXL=0, PC still at fault address) without automatically
                     * redirecting to the TLB refill handler.  We must perform the
                     * MIPS exception entry ourselves so the kernel's TLB handler at
                     * 0x80000000 can fill the TLB entry for the fault page.
                     *
                     * After the handler runs and executes ERET, prid_hook's
                     * ERET_EXECVE_RETRY intercept will redirect to do_execve's
                     * entry point with EXL=0 and the original SYSCALL state intact.
                     *
                     * Unicorn's MIPS CPU model already updated CP0_BADVADDR and
                     * CP0_CONTEXT (VPN2 from the fault address) before firing this
                     * hook, so the kernel TLB handler has the correct context even
                     * though we are setting EXL and redirecting manually here.
                     */
                    m->tlb_defer_count++;
                    if (m->tlb_defer_count > TLB_DEFER_RETRY_LIMIT) {
                        static uint32_t defer_entry_abort_log = 0;
                        if (defer_entry_abort_log < 32) {
                            fprintf(stderr,
                                    "[TLB_DEFER_ABORT] owner_epc=0x%08X pc=0x%08" PRIX64
                                    " retries=%u limit=%u action=clear_syscall\n",
                                    m->tlb_defer_owner_epc,
                                    (uint64_t)(uint32_t)pc,
                                    m->tlb_defer_count,
                                    TLB_DEFER_RETRY_LIMIT);
                            defer_entry_abort_log++;
                        }
                        clear_synthetic_syscall_state(m, true);
                        m->has_saved_exception = false;
                        return;
                    }
                    {
                        uint64_t exl_status = status | 0x2u;   /* set EXL=1 */
                        uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &exl_status);
                        uint64_t tlb_vec = mips_sext(0x80000180u);
                        uc_reg_write(uc, UC_MIPS_REG_PC, &tlb_vec);
                        fprintf(stderr,
                                "[TLB_DEFER_ENTRY] intno=%u fault_pc=0x%08" PRIX64
                                " owner_epc=0x%08X retry=%u"
                                " -> EXL=1 PC=0x80000180 (general exception handler)\n",
                                intno, (uint64_t)(uint32_t)pc,
                                m->tlb_defer_owner_epc, m->tlb_defer_count);
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
             * drop it before native TLB handling so MFC0 Cause/EPC reads see
             * native CP0 values (ExcCode 2/3), not synthetic syscall state.
             *
             * We intentionally do NOT save/restore synthetic state here:
             * intno=26/27 delivery timing can occur before we reliably observe
             * a matching nested ERET, and stale saved state can poison later
             * exception returns.
             */
            static uint32_t tlb_passthrough_log_count = 0;
            static uint32_t tlb_nested_drop_log = 0;
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
                if (m->pending_excode == MIPS_EXCCODE_SYS)
                    clear_synthetic_syscall_state(m, true);
                else {
                    m->pending_epc          = 0;
                    m->pending_excode       = 0;
                    m->pending_cause        = 0;
                    m->epc_was_written      = false;
                    m->pending_cause_served = false;
                    m->pending_epc_served   = false;
                    reset_tlb_defer_state(m);
                }
                m->has_saved_exception  = false;
                if (tlb_nested_drop_log < 64) {
                    fprintf(stderr,
                            "[TLB_NESTED_DROP] cleared synthetic excode for intno=%u at PC=0x%08" PRIX64 "\n",
                            intno, (uint64_t)(uint32_t)pc);
                    tlb_nested_drop_log++;
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
            fprintf(stderr,
                    "[SYSCALL_ENTRY_STALE_CLEAR] intno=%u PC=0x%08" PRIX64
                    " STATUS=0x%08" PRIX64
                    " old_excode=%u old_epc=0x%08" PRIX64 "\n",
                    intno, (uint64_t)(uint32_t)pc, status,
                    m->pending_excode, (uint64_t)(uint32_t)m->pending_epc);
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

    {
        uint64_t v0 = 0, a0 = 0, a1 = 0, a2 = 0, a3 = 0;
        uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
        uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
        uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
        uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
        uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
        uint64_t old_a0 = a0, old_a1 = a1, old_a2 = a2;

        if ((uint32_t)v0 == 4011u) {
            uint32_t a0_32 = (uint32_t)a0;
            uint32_t a1_32 = (uint32_t)a1;
            uint32_t a2_32 = (uint32_t)a2;
            /*
             * Do not rewrite valid kernel argv/envp vectors (kseg addresses):
             * run_init_process() in this 2.4 kernel intentionally passes
             * kernel-space init_argv/init_envp pointers.
             *
             * Only apply shim for obviously clobbered tiny pointers.
             */
            bool needs_execve_defaults =
                ((a1_32 != 0u && a1_32 < 0x1000u) ||
                 (a2_32 != 0u && a2_32 < 0x1000u));
            bool needs_execve_filename_shim = (!needs_execve_defaults &&
                                               a0_32 >= 0x80000000u);
            uint32_t sh_a0 = 0, sh_a1 = 0, sh_a2 = 0;
            bool used_defaults = false;
            bool filename_only = false;
            bool have_shim_ptrs = false;
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
                uint64_t na0 = filename_only ? sh_a0 : old_a0;
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
                            used_defaults ? "DEFAULTS" :
                            (filename_only ? "FILENAME" : "COPY"),
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
    bool pending = icu_pending(&m->icu);

    if (!m->tlb_trace_window &&
        m->pending_excode == MIPS_EXCCODE_SYS &&
        (uint32_t)m->pending_epc == RUN_INIT_SYSCALL_EPC) {
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
                    if (m->tlb_exl_drop_defer_count > TLB_EXL_DROP_DEFER_LIMIT) {
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
    { 0x80016ef0u, "do_page_fault (entry)",           false },
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
    const char *name = ((uint32_t)address == 0x80016ef0u) ? "do_page_fault" :
                       ((uint32_t)address == 0x8001a4e0u) ? "handle_tlbl"   :
                       ((uint32_t)address == 0x8001a660u) ? "handle_tlbs"   :
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
    gpio_init(&m->gpio);

    /* Always install the invalid-instruction hook (for MACC) */
    uc_hook hk;
    uc_hook_add(m->uc, &hk, UC_HOOK_INSN_INVALID,
                insn_invalid_hook, m, 1, 0);
    uc_hook_add(m->uc, &hk,
                UC_HOOK_MEM_READ_UNMAPPED |
                UC_HOOK_MEM_WRITE_UNMAPPED |
                UC_HOOK_MEM_FETCH_UNMAPPED,
                mem_unmapped_hook, m, 1, 0);

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

    /* Page-fault probe: log the first few do_page_fault invocations */
    pf_probe_count = 0;
    {
        uint64_t va = mips_sext(0x80016ef0u);
        uc_hook_add(m->uc, &hk, UC_HOOK_CODE, page_fault_probe_hook, m, va, va);
    }
    {
        uint64_t va_l = mips_sext(0x8001a4e0u); /* handle_tlbl */
        uint64_t va_s = mips_sext(0x8001a660u); /* handle_tlbs */
        uc_hook_add(m->uc, &hk, UC_HOOK_CODE, page_fault_probe_hook, m, va_l, va_l);
        uc_hook_add(m->uc, &hk, UC_HOOK_CODE, page_fault_probe_hook, m, va_s, va_s);
    }

    /* ICU ETIME fixup: force-enable ETIME bit in MSYSINT1 after
     * vr41xx_icu_init clears it.  Fires at the jr $ra (0x80275bec). */
    icu_etime_fixup_fired = false;
    {
        uint64_t va = mips_sext(0x80275becu);
        uc_hook_add(m->uc, &hk, UC_HOOK_CODE, icu_etime_fixup_hook, m, va, va);
    }

    /* Kernel direct-boot mode: load ELF and set entry point */
    if (cfg->kernel_path) {
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

    return m;
}

void machine_destroy(machine_t *m)
{
    if (!m) return;
    if (m->uc) uc_close(m->uc);
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

    fprintf(stderr, "[MACHINE] Starting execution at VA 0x%016" PRIX64 "\n",
            m->kernel_entry);

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
            fprintf(stderr, "[PROGRESS] insns=%" PRIu64 "M  PC=0x%08" PRIX64 "\n",
                    m->insn_count / 1000000, (uint64_t)(uint32_t)pc);
        }

        uint32_t step_count = m->post_init_trace_window ? POST_INIT_BATCH_SIZE : BATCH_SIZE;
        uc_err err = uc_emu_start(m->uc, pc, 0, 0, step_count);

        if (err != UC_ERR_OK) {
            uint64_t bad_pc = 0;
            uc_reg_read(m->uc, UC_MIPS_REG_PC, &bad_pc);
            if (err == UC_ERR_WRITE_UNMAPPED) {
                /*
                 * Fallback recovery for UC_ERR_WRITE_UNMAPPED.
                 * Normally the user-space pre-map in machine_create() covers
                 * all kuseg writes; this path handles any gaps (e.g., kernel
                 * data writes above kseg0 that miss our static maps).
                 * No limit — each unique block is only mapped once.
                 */
                uint64_t v0 = 0, a0 = 0, t2 = 0, sp = 0;
                uc_reg_read(m->uc, UC_MIPS_REG_V0, &v0);
                uc_reg_read(m->uc, UC_MIPS_REG_A0, &a0);
                uc_reg_read(m->uc, UC_MIPS_REG_T2, &t2);
                uc_reg_read(m->uc, UC_MIPS_REG_SP, &sp);
                uint64_t candidates[4] = { v0, a0, t2, sp };
                const char *names[4] = { "v0", "a0", "t2", "sp" };
                bool mapped_any = false;

                for (int i = 0; i < 4; i++) {
                    uint64_t va = candidates[i];
                    /* Only attempt kuseg (user-space) addresses */
                    if (va < 0x1000u || va >= 0x80000000u)
                        continue;
                    uint64_t block = va & ~((uint64_t)0xFFFFF);
                    uc_err me = uc_mem_map(m->uc, block, 0x100000,
                                           UC_PROT_READ | UC_PROT_WRITE);
                    if (me == UC_ERR_OK) {
                        /* Fresh mapping — log it */
                        if (!mapped_any) {
                            fprintf(stderr,
                                    "[MACHINE] write-unmapped recovery #%d at PC=0x%08" PRIX64 "\n",
                                    write_unmapped_recoveries + 1,
                                    (uint64_t)(uint32_t)bad_pc);
                        }
                        fprintf(stderr,
                                "[MACHINE]   mapped block 0x%08" PRIX64 " via $%s=0x%08" PRIX64 "\n",
                                (uint64_t)(uint32_t)block, names[i],
                                (uint64_t)(uint32_t)va);
                        mapped_any = true;
                    } else if (me == UC_ERR_MAP) {
                        /* Already mapped — still count as handled */
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
                uc_mem_type fault_type = (uc_mem_type)0;
                if (m->last_unmapped_valid) {
                    badv = m->last_unmapped_addr;
                    fault_type = m->last_unmapped_type;
                } else if (m->shadow_cp0_badvaddr != 0) {
                    badv = m->shadow_cp0_badvaddr;
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
                uint64_t candidates[10] = { bad_pc, badv, at, a0, a1, a2, k0, k1, t2, sp };
                const char *names[10] = { "pc", "badv", "at", "a0", "a1", "a2", "k0", "k1", "t2", "sp" };
                bool mapped_any = false;

                for (int i = 0; i < 10; i++) {
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

    fprintf(stderr, "[MACHINE] Stopped after %" PRIu64 " instructions\n",
            m->insn_count);
}

void machine_stop(machine_t *m)
{
    m->running = false;

    ui_destroy(m);
    uc_emu_stop(m->uc);
}
