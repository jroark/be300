/*
 * wince_boot.c — WinCE cold-boot support for the BE-300 emulator.
 *
 * Phase 3 cleanup: all warm-boot, seed, replay, snapshot, and fault-site
 * diagnostics have been removed.  Only the cold-boot essentials remain.
 */

#include "wince_boot.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cop0.h"
#include "cpu.h"
#include "cpu_mips.h"
#include "devices.h"
#include "machine.h"
#include "memory.h"
#include "mips_cpu_types.h"

extern bool single_step;

static machine_t *g_active_wince_machine = NULL;
static const char *wince_gpr_names[] = MIPS_REGISTER_NAMES;
static const char *format_word_or_unknown(char *buf, size_t buf_size, bool ok,
    uint32_t value);
static void dump_section3_descriptor_window(machine_t *m, const char *tag);
static void dump_section3_descriptor_at(machine_t *m, const char *tag,
    uint32_t desc_ptr);
static void dump_section3_wrap_window(machine_t *m, const char *tag,
    uint32_t wrap_va);
static void dump_section3_retobj_window(machine_t *m, const char *tag,
    uint32_t obj_va);
static void dump_section3_context_head(machine_t *m, const char *tag,
    uint32_t pc32);

#define WINCE_COLD_LATE_PROBE_LOGGED UINT32_C(0x00200000)
#define WINCE_CALLBACK_SLOT_BASE_VA  UINT32_C(0x01FE6544)
#define WINCE_CALLBACK_SLOT_CANDIDATE_PA UINT32_C(0x00FFB544)
#define WINCE_CALLBACK_SLOT_TRACE_BYTES 0x40u
#define WINCE_CALLBACK_OBJ_CANDIDATE_VA UINT32_C(0x80FFFEA8)
#define WINCE_CALLBACK_OBJ_CANDIDATE_PA UINT32_C(0x00FFFEA8)
#define WINCE_CALLBACK_OBJ_TRACE_BYTES 0x100u
#define WINCE_HOT_USER_L2_TABLE_VA UINT32_C(0x80FFC1C8)
#define WINCE_HOT_USER_L2_TABLE_PA UINT32_C(0x00FFC1C8)
#define WINCE_HOT_USER_L2_TRACE_BYTES 0x40u
#define WINCE_HOT_USER_L2_PAGE_PA UINT32_C(0x00FFC000)
#define WINCE_HOT_USER_L2_PAGE_BYTES 0x1000u
#define WINCE_PATH_PROBE_77820      UINT64_C(0x0000000000000001)
#define WINCE_PATH_PROBE_79488      UINT64_C(0x0000000000000002)
#define WINCE_PATH_PROBE_794C8      UINT64_C(0x0000000000000004)
#define WINCE_PATH_PROBE_795D8      UINT64_C(0x0000000000000008)
#define WINCE_PATH_PROBE_79634      UINT64_C(0x0000000000000010)
#define WINCE_PATH_PROBE_79730      UINT64_C(0x0000000000000020)
#define WINCE_PATH_PROBE_76FBC      UINT64_C(0x0000000000000040)
#define WINCE_PATH_PROBE_76FE0      UINT64_C(0x0000000000000080)
#define WINCE_PATH_PROBE_7767C      UINT64_C(0x0000000000000100)
#define WINCE_PATH_PROBE_776F0      UINT64_C(0x0000000000000200)
#define WINCE_PATH_PROBE_77770      UINT64_C(0x0000000000000400)
#define WINCE_PATH_PROBE_79430      UINT64_C(0x0000000000000800)
#define WINCE_PATH_PROBE_79460      UINT64_C(0x0000000000001000)
#define WINCE_PATH_PROBE_79510      UINT64_C(0x0000000000002000)
#define WINCE_PATH_PROBE_772F0      UINT64_C(0x0000000000004000)
#define WINCE_PATH_PROBE_7742C      UINT64_C(0x0000000000008000)
#define WINCE_PATH_PROBE_7757C      UINT64_C(0x0000000000010000)
#define WINCE_PATH_PROBE_77664      UINT64_C(0x0000000000020000)
#define WINCE_PATH_PROBE_77738      UINT64_C(0x0000000000040000)
#define WINCE_PATH_PROBE_77260      UINT64_C(0x0000000000080000)
#define WINCE_PATH_PROBE_76B50      UINT64_C(0x0000000000100000)
#define WINCE_PATH_PROBE_76BA0      UINT64_C(0x0000000000200000)
#define WINCE_PATH_PROBE_76C60      UINT64_C(0x0000000000400000)
#define WINCE_PATH_PROBE_76CBC      UINT64_C(0x0000000000800000)
#define WINCE_PATH_PROBE_76E68      UINT64_C(0x0000000001000000)
#define WINCE_PATH_PROBE_77344      UINT64_C(0x0000000002000000)
#define WINCE_PATH_PROBE_775B0      UINT64_C(0x0000000004000000)
#define WINCE_PATH_PROBE_775B8      UINT64_C(0x0000000008000000)
#define WINCE_PATH_PROBE_7794C      UINT64_C(0x0000000010000000)
#define WINCE_PATH_PROBE_7796C      UINT64_C(0x0000000020000000)
#define WINCE_PATH_PROBE_77A14      UINT64_C(0x0000000040000000)
#define WINCE_PATH_PROBE_7B398      UINT64_C(0x0000000080000000)
#define WINCE_PATH_PROBE_7B57C      UINT64_C(0x0000000100000000)
#define WINCE_PATH_PROBE_947C8      UINT64_C(0x0000000200000000)
#define WINCE_PATH_PROBE_8B21C      UINT64_C(0x0000000400000000)
#define WINCE_PATH_PROBE_8B528      UINT64_C(0x0000000800000000)
#define WINCE_PATH_PROBE_7A3FC      UINT64_C(0x0000001000000000)
#define WINCE_PATH_PROBE_79898      UINT64_C(0x0000002000000000)
#define WINCE_PATH_PROBE_79910      UINT64_C(0x0000004000000000)
#define WINCE_PATH_PROBE_79990      UINT64_C(0x0000008000000000)
#define WINCE_FB_REPORT_MAX         10u
#define WINCE_FB_PC_RING_LIMIT      32u
#define WINCE_PPSH_FLOW_LOG_MAX     128u
#define WINCE_PPSH_SEQ_MAX          32u
#define WINCE_PPSH_SEQ_READ_LOG_MAX 6u
#define WINCE_SERIAL_EXC_LOG_MAX    24u
#define WINCE_SERIAL_CORR_LOG_MAX   24u
#define WINCE_SYSTEMPATCH_CTX_MAX   12u
#define WINCE_SYSTEMPATCH_THREAD_CTX_MAX 32u
#define WINCE_HOT_FAULT_PROBE_MAX   12u
#define WINCE_ROMHDR_NMODS_OFF      0x10u
#define WINCE_ROMHDR_RAMSTART_OFF   0x14u
#define WINCE_ROMHDR_RAMFREE_OFF    0x18u
#define WINCE_ROMHDR_RAMEND_OFF     0x1Cu
#define WINCE_ROMHDR_COPYENTRIES_OFF 0x20u
#define WINCE_ROMHDR_COPYOFFSET_OFF 0x24u
#define WINCE_ROMHDR_NUMFILES_OFF   0x30u
#define WINCE_ROMHDR_MODTABLE_OFF   0x54u
#define WINCE_TOCENTRY_SIZE         0x20u
#define WINCE_FILEENTRY_SIZE        0x1Cu

static void dump_recent_pc_ring(machine_t *m, const char *tag, uint32_t limit);
static void maybe_arm_fb_watch(machine_t *m, struct cpu *cpu,
    const char *reason);
static void maybe_track_fb_runtime_changes(machine_t *m, struct cpu *cpu);
static void maybe_dump_toc_summary(machine_t *m, uint32_t ptoc);
static bool try_discover_ptoc(machine_t *m, uint32_t *ptoc_out);
static void wince_fb_write_observer(struct vfb_data *fb, struct cpu *cpu,
    void *opaque, uint64_t relative_addr, size_t len);
static void dump_va_window(machine_t *m, const char *label, uint32_t va,
    uint32_t size);
static void dump_code_window(machine_t *m, uint32_t pc, size_t before_words,
    size_t after_words);
static bool load_utf16_ascii(machine_t *m, struct cpu *cpu, uint32_t va,
    char *ascii, size_t ascii_len);
static void maybe_log_ppsh_debug_message(machine_t *m, struct cpu *cpu,
    const char *source, uint32_t str_va, const char *ascii);
static void maybe_flush_ppsh_serial_line(machine_t *m);
static void maybe_sample_systempatch_thread_context(machine_t *m,
    struct cpu *cpu);
static void maybe_dump_ppsh_helper_context(machine_t *m, struct cpu *cpu,
    uint16_t cmd);
static void maybe_dump_ppsh_flag_context(machine_t *m, struct cpu *cpu,
    uint32_t old_flag, uint32_t new_flag);
static void maybe_note_ppsh_exact_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32);
static void log_ppsh_timeout_state(machine_t *m, const char *tag);
static void reset_serial_exception_record(
    wince_serial_exception_record_t *rec);
static void maybe_commit_serial_exception(machine_t *m, const char *reason);
static void maybe_record_serial_exception_line(machine_t *m,
    const char *line);
static void maybe_log_serial_exception_correlation(machine_t *m,
    struct cpu *cpu, uint32_t exccode, uint32_t fault_vaddr,
    const char *phase);
static void maybe_log_systempatch_context(machine_t *m, const char *reason);
static void maybe_note_exception_hot_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32);
static void maybe_note_hot_l2_alloc_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32);
static void maybe_note_callback_slot_pc(machine_t *m, struct cpu *cpu,
    uint32_t pc32);
static void maybe_log_hot_page_verdict(machine_t *m, struct cpu *cpu,
    uint32_t probe_va, uint32_t fault_vaddr, uint32_t exccode,
    const char *tag);
static void log_l2_table_state(machine_t *m, const char *tag,
    uint32_t table_va, uint32_t focus_vaddr);
static void log_hot_user_l2_state(machine_t *m, const char *tag);
static void log_alloc_leaf_summary(machine_t *m, const char *label,
    uint32_t leaf_va);
static void log_alloc_scan_state(machine_t *m, struct cpu *cpu,
    const char *label, uint32_t base_va, uint32_t type_tag,
    uint32_t start_idx, uint32_t start_slot, uint32_t request_count,
    bool processed_ok, uint32_t processed, bool result_ok, uint32_t result);
static void log_section0_focus_window(machine_t *m, const char *tag,
    uint32_t table_va);
static void maybe_note_section3_queue_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32);
static void maybe_note_section3_worker_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32);
static void maybe_note_section3_type4_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32);
static void maybe_note_section3_type4_gate_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32);
static void maybe_note_section3_type4_state_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32);
static void note_type4_order_event(machine_t *m, struct cpu *cpu,
    uint16_t *slot, const char *tag);
static void maybe_log_section3_pool_write(machine_t *m, struct cpu *cpu,
    uint64_t paddr, size_t len, uint64_t val);
static void maybe_log_section3_ctor_field_write(machine_t *m, struct cpu *cpu,
    uint64_t paddr, size_t len, uint64_t val);
static void maybe_log_section3_focus_obj_write(machine_t *m, struct cpu *cpu,
    uint64_t paddr, size_t len, uint64_t val);
static uint32_t load_pa_word(machine_t *m, uint32_t pa);
static bool load_va_word(machine_t *m, uint32_t va, uint32_t *out);

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static machine_t *wince_boot_from_gx(struct machine *gxm)
{
    if (!g_active_wince_machine)
        return NULL;
    if (g_active_wince_machine->gxe_machine != gxm)
        return NULL;
    return g_active_wince_machine;
}

static uint64_t va32_to_mips64(uint32_t va)
{
    return (uint64_t)(int64_t)(int32_t)va;
}

static uint32_t canonicalize_nk_pc(uint32_t pc32)
{
    if ((pc32 & 0xE0000000u) == 0x80000000u
        || (pc32 & 0xE0000000u) == 0xA0000000u) {
        return (pc32 & 0x1FFFFFFFu) | 0x80000000u;
    }
    return pc32;
}

static bool same_4k_page(uint32_t a, uint32_t b)
{
    return (a & ~UINT32_C(0xFFF)) == (b & ~UINT32_C(0xFFF));
}

static unsigned hot_page_verdict_severity(const wince_hot_page_verdict_t *v)
{
    if (!v || !v->seen)
        return 0u;
    if (v->section_val == 0u || v->l2_val == 0u)
        return 4u;
    if (!v->selected_valid)
        return 3u;
    if (v->fault_va == v->probe_va)
        return 2u;
    return 1u;
}

static void note_type4_order_event(machine_t *m, struct cpu *cpu,
    uint16_t *slot, const char *tag)
{
    uint32_t handle = 0;
    bool handle_ok = false;
    char handle_buf[16];

    if (!m || !cpu || !slot || !tag)
        return;
    if (*slot != 0u)
        return;
    if (m->wince.type4_wrap_watch_va == 0u
        || m->wince.type4_payload_watch_va == 0u)
        return;

    if (m->wince.type4_order_next != UINT16_MAX)
        m->wince.type4_order_next++;
    *slot = m->wince.type4_order_next;

    handle_ok = load_va_word(m, m->wince.type4_wrap_watch_va + 0x08u, &handle);
    fprintf(stderr,
        "[WINCE_TYPE4_ORDER] seq=%u tag=%s pc=0x%08X ra=0x%08X sp=0x%08X"
        " wrap=0x%08X handle=%s payload=0x%08X"
        " sec0=0x%08X sec3=0x%08X\n",
        (unsigned)*slot,
        tag,
        canonicalize_nk_pc((uint32_t)cpu->pc),
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
        m->wince.type4_wrap_watch_va,
        format_word_or_unknown(handle_buf, sizeof(handle_buf), handle_ok, handle),
        m->wince.type4_payload_watch_va,
        load_pa_word(m, 0x18C0u),
        load_pa_word(m, 0x18CCu));
}

static uint32_t count_active_sections(machine_t *m)
{
    uint32_t count = 0;
    unsigned i;

    if (!m)
        return 0;

    for (i = 0; i < 64; i++) {
        uint32_t entry = m->wince.section_table_shadow[i];

        if (entry != 0 && entry != 0x8008BC18u)
            count++;
    }

    return count;
}

static bool ppsh_trace_enabled(machine_t *m)
{
    if (!m || !m->wince.active || !m->wince.cold_boot_copy_done)
        return false;
    if (m->cfg.enable_ppsh || !m->wince.ppsh_trace_armed)
        return false;
    return true;
}

static bool ppsh_flow_log(machine_t *m, const char *fmt, ...)
{
    va_list ap;

    if (!m)
        return false;
    if (m->wince.ppsh_flow_diag_count >= WINCE_PPSH_FLOW_LOG_MAX)
        return false;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    m->wince.ppsh_flow_diag_count++;
    return true;
}

static uint32_t load_pa_word(machine_t *m, uint32_t pa)
{
    unsigned char *host = memory_paddr_to_hostaddr(m->cpu->mem, pa, MEM_READ);
    if (!host)
        return 0;
    return (uint32_t)host[0]
         | ((uint32_t)host[1] << 8)
         | ((uint32_t)host[2] << 16)
         | ((uint32_t)host[3] << 24);
}

static bool load_va_word(machine_t *m, uint32_t va, uint32_t *out)
{
    unsigned char buf[4] = { 0, 0, 0, 0 };

    if (!m || !m->cpu || !out)
        return false;
    if (!m->cpu->memory_rw(m->cpu, m->cpu->mem, va32_to_mips64(va), buf, 4,
        MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
        return false;

    *out = (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
    return true;
}

static bool load_va_half(machine_t *m, uint32_t va, uint16_t *out)
{
    unsigned char buf[2] = { 0, 0 };

    if (!m || !m->cpu || !out)
        return false;
    if (!m->cpu->memory_rw(m->cpu, m->cpu->mem, va32_to_mips64(va), buf, 2,
        MEM_READ, CACHE_DATA | NO_EXCEPTIONS))
        return false;

    *out = (uint16_t)buf[0]
         | ((uint16_t)buf[1] << 8);
    return true;
}

static bool load_vrc_latch_word(uint32_t pa, uint32_t *out)
{
    return be300_vrc4173_latch_read_u32(pa, out);
}

static void dump_live_tlb(machine_t *m)
{
    struct mips_coproc *cp;
    int ntlb;
    int i;

    if (!m || !m->cpu || !m->cpu->cd.mips.coproc[0])
        return;

    cp = m->cpu->cd.mips.coproc[0];
    ntlb = m->cpu->cd.mips.cpu_type.nr_of_tlb_entries;

    fprintf(stderr,
        "[WINCE_TLB] dump entries=%d wired=%u entryhi=0x%08X pagemask=0x%08X\n",
        ntlb,
        (unsigned)cp->reg[COP0_WIRED],
        (uint32_t)cp->reg[COP0_ENTRYHI],
        (uint32_t)cp->reg[COP0_PAGEMASK]);

    for (i = 0; i < ntlb; i++) {
        uint32_t hi = (uint32_t)cp->tlbs[i].hi;
        uint32_t lo0 = (uint32_t)cp->tlbs[i].lo0;
        uint32_t lo1 = (uint32_t)cp->tlbs[i].lo1;
        uint32_t mask = (uint32_t)cp->tlbs[i].mask;

        if (hi == 0 && lo0 == 0 && lo1 == 0 && mask == 0)
            continue;

        fprintf(stderr,
            "[WINCE_TLB]   [%2d] hi=0x%08X lo0=0x%08X lo1=0x%08X"
            " mask=0x%08X\n",
            i, hi, lo0, lo1, mask);
    }
}

static void dump_tlb_match_for_va(machine_t *m, uint32_t va,
    const char *label)
{
    struct mips_coproc *cp;
    int pageshift;
    int ntlb;
    int i;

    if (!m || !m->cpu || !m->cpu->cd.mips.coproc[0])
        return;

    cp = m->cpu->cd.mips.coproc[0];
    ntlb = m->cpu->cd.mips.cpu_type.nr_of_tlb_entries;
    pageshift = (m->cpu->cd.mips.cpu_type.rev == MIPS_R4100) ? 10 : 12;

    for (i = 0; i < ntlb; i++) {
        uint64_t hi = cp->tlbs[i].hi;
        uint64_t lo[2] = { cp->tlbs[i].lo0, cp->tlbs[i].lo1 };
        uint64_t mask = cp->tlbs[i].mask;
        uint64_t match_mask = mask
            | (uint64_t)((m->cpu->cd.mips.cpu_type.rev == MIPS_R4100)
                ? 0x07ffu : 0x1fffu);
        uint64_t page_size = (match_mask + 1u) >> 1;
        uint32_t vbase = ((uint32_t)hi & (uint32_t)ENTRYHI_VPN2_MASK);
        int page;

        vbase &= ~(uint32_t)match_mask;

        for (page = 0; page < 2; page++) {
            uint32_t page_vbase = vbase + (uint32_t)((uint64_t)page
                * page_size);
            uint64_t page_lo = lo[page];

            if (va < page_vbase
                || (uint64_t)va >= (uint64_t)page_vbase + page_size) {
                continue;
            }

            if (!(page_lo & ENTRYLO_V)) {
                fprintf(stderr,
                    "[WINCE_TLB] match %-14s va=0x%08X idx=%d page=%d"
                    " vbase=0x%08X psize=0x%08X valid=0 global=%d"
                    " asid=0x%02X lo=0x%08X\n",
                    label ? label : "-",
                    va, i, page,
                    page_vbase,
                    (uint32_t)page_size,
                    (int)(((uint32_t)hi & TLB_G) ? 1 : 0),
                    (unsigned)((uint32_t)hi & ENTRYHI_ASID),
                    (uint32_t)page_lo);
                return;
            }

            {
                uint64_t pbase = (page_lo & ENTRYLO_PFN_MASK)
                    >> ENTRYLO_PFN_SHIFT;
                uint64_t paddr = (pbase << pageshift)
                    & ~(match_mask >> 1);

                paddr += (uint64_t)(va - page_vbase);
                fprintf(stderr,
                    "[WINCE_TLB] match %-14s va=0x%08X idx=%d page=%d"
                    " vbase=0x%08X pbase=0x%08X paddr=0x%08X"
                    " psize=0x%08X valid=1 global=%d asid=0x%02X"
                    " lo=0x%08X\n",
                    label ? label : "-",
                    va, i, page,
                    page_vbase,
                    (uint32_t)(paddr - (va - page_vbase)),
                    (uint32_t)paddr,
                    (uint32_t)page_size,
                    (int)(((uint32_t)hi & TLB_G) ? 1 : 0),
                    (unsigned)((uint32_t)hi & ENTRYHI_ASID),
                    (uint32_t)page_lo);
            }
            return;
        }
    }

    fprintf(stderr,
        "[WINCE_TLB] match %-14s va=0x%08X status=NO_MATCH\n",
        label ? label : "-", va);
}

static void dump_va_peek(machine_t *m, const char *label, uint32_t va)
{
    uint32_t word = 0;
    bool ok;

    ok = load_va_word(m, va, &word);
    fprintf(stderr,
        "[WINCE_TLB] peek  %-14s va=0x%08X ok=%d value=0x%08X\n",
        label ? label : "-", va, ok ? 1 : 0, word);
}

static void invalidate_all(machine_t *m)
{
    m->cpu->invalidate_translation_caches(m->cpu, 0, INVALIDATE_ALL);
}

static bool block_matches(const uint32_t *a, const uint32_t *b, size_t words)
{
    return memcmp(a, b, words * sizeof(uint32_t)) == 0;
}

static bool block_has_nonzero(const uint32_t *a, size_t words)
{
    size_t i;

    for (i = 0; i < words; i++) {
        if (a[i] != 0)
            return true;
    }
    return false;
}

static void read_block(machine_t *m, uint32_t pa, uint32_t *out, size_t words)
{
    size_t i;

    for (i = 0; i < words; i++)
        out[i] = load_pa_word(m, pa + (uint32_t)(i * 4u));
}

static bool range_overlaps(uint64_t base_a, uint64_t size_a, uint64_t base_b,
    uint64_t size_b)
{
    uint64_t end_a;
    uint64_t end_b;

    if (size_a == 0 || size_b == 0)
        return false;

    end_a = base_a + size_a;
    end_b = base_b + size_b;
    return base_a < end_b && base_b < end_a;
}

static uint32_t table_va_to_pa(uint32_t table_va)
{
    return table_va & UINT32_C(0x1FFFFFFF);
}

static uint32_t load_table_word(machine_t *m, uint32_t table_va,
    uint32_t offset)
{
    if (table_va == 0)
        return 0;
    return load_pa_word(m, table_va_to_pa(table_va) + offset);
}

static bool entrylo_to_pa_4k(uint32_t entrylo, uint32_t va, uint32_t *out_pa)
{
    uint32_t pfn;

    if (!out_pa || !(entrylo & ENTRYLO_V))
        return false;

    pfn = (entrylo & ENTRYLO_PFN_MASK) >> ENTRYLO_PFN_SHIFT;
    *out_pa = ((pfn << 10) & ~UINT32_C(0x0FFF)) | (va & UINT32_C(0x0FFF));
    return true;
}

static bool callback_obj_offset_is_interesting(uint64_t off, size_t len)
{
    static const struct {
        uint32_t off;
        uint32_t len;
    } ranges[] = {
        { 0x08u, 0x0Cu },
        { 0x24u, 0x04u },
        { 0x54u, 0x10u },
        { 0x80u, 0x0Cu },
        { 0xBCu, 0x10u },
    };
    size_t i;

    for (i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
        if (range_overlaps(off, (uint64_t)len, ranges[i].off, ranges[i].len))
            return true;
    }

    return false;
}

static void maybe_log_callback_slot_state(machine_t *m, struct cpu *cpu,
    const char *reason, uint32_t entrylo)
{
    uint32_t base_pa;
    uint32_t flag;
    uint32_t ptr;
    uint32_t aux;
    uint32_t arg;
    uint32_t cb260 = 0;
    bool cb260_ok = false;

    if (!m || !cpu)
        return;
    if (!entrylo_to_pa_4k(entrylo, WINCE_CALLBACK_SLOT_BASE_VA, &base_pa))
        return;
    if (m->wince.callback_slot_diag_count >= 24u
        && m->wince.callback_slot_watch_pa == base_pa) {
        return;
    }

    flag = load_pa_word(m, base_pa + 0x00u);
    ptr = load_pa_word(m, base_pa + 0x04u);
    aux = load_pa_word(m, base_pa + 0x08u);
    arg = load_pa_word(m, base_pa + 0x0Cu);
    if (ptr != 0)
        cb260_ok = load_va_word(m, ptr + 0x260u, &cb260);

    m->wince.callback_slot_watch_armed = true;
    m->wince.callback_slot_watch_pa = base_pa;
    m->wince.callback_slot_diag_count++;

    fprintf(stderr,
        "[WINCE_CB_SLOT] reason=%s base_va=0x%08X base_pa=0x%08X"
        " entrylo=0x%08X flag=0x%08X ptr=0x%08X aux=0x%08X arg=0x%08X"
        " ptr260=%s0x%08X pc=0x%08X ra=0x%08X\n",
        reason ? reason : "?",
        WINCE_CALLBACK_SLOT_BASE_VA,
        base_pa,
        entrylo,
        flag,
        ptr,
        aux,
        arg,
        cb260_ok ? "" : "?",
        cb260,
        (uint32_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
}

static void log_callback_slot_write(machine_t *m, struct cpu *cpu,
    uint32_t base_pa, uint64_t paddr, size_t len, uint64_t val,
    const char *source)
{
    if (!m || !cpu || !source)
        return;

    m->wince.callback_slot_write_count++;
    if (m->wince.callback_slot_write_count > 32u)
        return;

    fprintf(stderr,
        "[WINCE_CB_WRITE] #%u source=%s base_pa=0x%08X off=0x%02" PRIx64
        " len=%zu val=0x%08llX flag=0x%08X ptr=0x%08X aux=0x%08X"
        " arg=0x%08X pc=0x%08" PRIx64 " ra=0x%08X\n",
        (unsigned)m->wince.callback_slot_write_count,
        source,
        base_pa,
        paddr - base_pa,
        len,
        (unsigned long long)val,
        load_pa_word(m, base_pa + 0x00u),
        load_pa_word(m, base_pa + 0x04u),
        load_pa_word(m, base_pa + 0x08u),
        load_pa_word(m, base_pa + 0x0Cu),
        (uint64_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);

    {
        static const char * const word_name[4] = { "flag", "ptr", "aux", "arg" };
        uint64_t start = paddr - base_pa;
        uint64_t end = start + (uint64_t)len;
        for (unsigned w = 0; w < 4u; w++) {
            uint64_t ws = (uint64_t)(w * 4u);
            uint64_t we = ws + 4u;
            if (start >= we || end <= ws)
                continue;
            uint32_t cur = load_pa_word(m, base_pa + (uint32_t)ws);
            uint8_t bit = (uint8_t)(1u << w);
            if (cur != 0 && !(m->wince.cb_slot_first_seen & bit)) {
                m->wince.cb_slot_first_seen |= bit;
                m->wince.cb_slot_first_pc[w] = (uint32_t)cpu->pc;
                m->wince.cb_slot_first_ra[w] = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA];
                m->wince.cb_slot_first_val[w] = cur;
                m->wince.cb_slot_first_instr[w] = (uint64_t)cpu->ninstrs;
                fprintf(stderr,
                    "[WINCE_CB_FIRST] word=%s pc=0x%08X ra=0x%08X val=0x%08X"
                    " base_pa=0x%08X instr=%llu\n",
                    word_name[w],
                    (uint32_t)cpu->pc,
                    (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
                    cur,
                    base_pa,
                    (unsigned long long)cpu->ninstrs);
            } else if (cur == 0 && (m->wince.cb_slot_first_seen & bit)
                       && !(m->wince.cb_slot_zero_seen & bit)) {
                m->wince.cb_slot_zero_seen |= bit;
                m->wince.cb_slot_zero_pc[w] = (uint32_t)cpu->pc;
                m->wince.cb_slot_zero_ra[w] = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA];
                fprintf(stderr,
                    "[WINCE_CB_ZERO] word=%s pc=0x%08X ra=0x%08X"
                    " base_pa=0x%08X instr=%llu\n",
                    word_name[w],
                    (uint32_t)cpu->pc,
                    (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
                    base_pa,
                    (unsigned long long)cpu->ninstrs);
            }
        }
    }
}

static void maybe_log_callback_object_state(machine_t *m, struct cpu *cpu,
    const char *reason, uint32_t obj_va)
{
    uint32_t self = 0;
    uint32_t link = 0;
    uint32_t slot = 0;
    uint32_t state = 0;
    uint32_t flags = 0;
    uint32_t buf54 = 0;
    uint32_t buf58 = 0;
    uint32_t buf5c = 0;
    uint32_t cb = 0;
    uint32_t gate80 = 0;
    uint32_t gate84 = 0;
    uint32_t gate88 = 0;
    uint32_t listbc = 0;
    uint32_t modec8 = 0;
    bool self_ok;
    bool link_ok;
    bool slot_ok;
    bool state_ok;
    bool flags_ok;
    bool buf54_ok;
    bool buf58_ok;
    bool buf5c_ok;
    bool cb_ok;
    bool gate80_ok;
    bool gate84_ok;
    bool gate88_ok;
    bool listbc_ok;
    bool modec8_ok;
    char self_buf[16];
    char link_buf[16];
    char slot_buf[16];
    char state_buf[16];
    char flags_buf[16];
    char buf54_buf[16];
    char buf58_buf[16];
    char buf5c_buf[16];
    char cb_buf[16];
    char gate80_buf[16];
    char gate84_buf[16];
    char gate88_buf[16];
    char listbc_buf[16];
    char modec8_buf[16];

    if (!m || !cpu || obj_va == 0)
        return;
    if (m->wince.callback_obj_diag_count >= 48u)
        return;

    self_ok = load_va_word(m, obj_va + 0x00u, &self);
    link_ok = load_va_word(m, obj_va + 0x04u, &link);
    slot_ok = load_va_word(m, obj_va + 0x08u, &slot);
    state_ok = load_va_word(m, obj_va + 0x0Cu, &state);
    flags_ok = load_va_word(m, obj_va + 0x10u, &flags);
    buf54_ok = load_va_word(m, obj_va + 0x54u, &buf54);
    buf58_ok = load_va_word(m, obj_va + 0x58u, &buf58);
    buf5c_ok = load_va_word(m, obj_va + 0x5Cu, &buf5c);
    cb_ok = load_va_word(m, obj_va + 0x60u, &cb);
    gate80_ok = load_va_word(m, obj_va + 0x80u, &gate80);
    gate84_ok = load_va_word(m, obj_va + 0x84u, &gate84);
    gate88_ok = load_va_word(m, obj_va + 0x88u, &gate88);
    listbc_ok = load_va_word(m, obj_va + 0xBCu, &listbc);
    modec8_ok = load_va_word(m, obj_va + 0xC8u, &modec8);

    fprintf(stderr,
        "[WINCE_CB_OBJ] reason=%s obj=0x%08X self=%s slot=%s state=%s"
        " flags=%s link=%s buf54=%s buf58=%s buf5c=%s cb=%s"
        " gate80=%s gate84=%s gate88=%s listbc=%s modec8=%s"
        " pc=0x%08X ra=0x%08X\n",
        reason ? reason : "?",
        obj_va,
        format_word_or_unknown(self_buf, sizeof(self_buf), self_ok, self),
        format_word_or_unknown(slot_buf, sizeof(slot_buf), slot_ok, slot),
        format_word_or_unknown(state_buf, sizeof(state_buf), state_ok, state),
        format_word_or_unknown(flags_buf, sizeof(flags_buf), flags_ok, flags),
        format_word_or_unknown(link_buf, sizeof(link_buf), link_ok, link),
        format_word_or_unknown(buf54_buf, sizeof(buf54_buf), buf54_ok, buf54),
        format_word_or_unknown(buf58_buf, sizeof(buf58_buf), buf58_ok, buf58),
        format_word_or_unknown(buf5c_buf, sizeof(buf5c_buf), buf5c_ok, buf5c),
        format_word_or_unknown(cb_buf, sizeof(cb_buf), cb_ok, cb),
        format_word_or_unknown(gate80_buf, sizeof(gate80_buf), gate80_ok, gate80),
        format_word_or_unknown(gate84_buf, sizeof(gate84_buf), gate84_ok, gate84),
        format_word_or_unknown(gate88_buf, sizeof(gate88_buf), gate88_ok, gate88),
        format_word_or_unknown(listbc_buf, sizeof(listbc_buf), listbc_ok, listbc),
        format_word_or_unknown(modec8_buf, sizeof(modec8_buf), modec8_ok, modec8),
        (uint32_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
    m->wince.callback_obj_diag_count++;
}

static void log_callback_object_write(machine_t *m, struct cpu *cpu,
    uint64_t paddr, size_t len, uint64_t val)
{
    uint64_t off;

    if (!m || !cpu || paddr < WINCE_CALLBACK_OBJ_CANDIDATE_PA)
        return;

    off = paddr - WINCE_CALLBACK_OBJ_CANDIDATE_PA;
    if (!callback_obj_offset_is_interesting(off, len))
        return;

    m->wince.callback_obj_write_count++;
    if (m->wince.callback_obj_write_count > 48u)
        return;

    fprintf(stderr,
        "[WINCE_CB_OBJW] #%u off=0x%02" PRIx64 " len=%zu val=0x%08llX"
        " slot=0x%08X state=0x%08X flags=0x%08X cb=0x%08X"
        " gate84=0x%08X gate88=0x%08X modec8=0x%08X"
        " pc=0x%08" PRIx64 " ra=0x%08X\n",
        (unsigned)m->wince.callback_obj_write_count,
        off,
        len,
        (unsigned long long)val,
        load_pa_word(m, WINCE_CALLBACK_OBJ_CANDIDATE_PA + 0x08u),
        load_pa_word(m, WINCE_CALLBACK_OBJ_CANDIDATE_PA + 0x0Cu),
        load_pa_word(m, WINCE_CALLBACK_OBJ_CANDIDATE_PA + 0x10u),
        load_pa_word(m, WINCE_CALLBACK_OBJ_CANDIDATE_PA + 0x60u),
        load_pa_word(m, WINCE_CALLBACK_OBJ_CANDIDATE_PA + 0x84u),
        load_pa_word(m, WINCE_CALLBACK_OBJ_CANDIDATE_PA + 0x88u),
        load_pa_word(m, WINCE_CALLBACK_OBJ_CANDIDATE_PA + 0xC8u),
        (uint64_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
}

static void log_hot_user_l2_state(machine_t *m, const char *tag)
{
    static const uint32_t hot_vas[] = {
        UINT32_C(0x03FE7884),
        UINT32_C(0x03FE924C),
        UINT32_C(0x03FEB4AC),
    };
    size_t i;

    if (!m)
        return;

    for (i = 0; i < sizeof(hot_vas) / sizeof(hot_vas[0]); i++) {
        uint32_t va = hot_vas[i];
        uint32_t pte_off = (va >> 10) & 0x38u;
        uint32_t lo0 = load_table_word(m, WINCE_HOT_USER_L2_TABLE_VA,
            pte_off + 12u);
        uint32_t lo1 = load_table_word(m, WINCE_HOT_USER_L2_TABLE_VA,
            pte_off + 16u);

        fprintf(stderr,
            "[WINCE_HOT_L2] tag=%s va=0x%08X pte_off=0x%02X"
            " lo0=0x%08X lo1=0x%08X v0=%u v1=%u\n",
            tag ? tag : "-",
            va,
            pte_off,
            lo0,
            lo1,
            (lo0 & ENTRYLO_V) ? 1u : 0u,
            (lo1 & ENTRYLO_V) ? 1u : 0u);
    }
}

static void log_alloc_leaf_summary(machine_t *m, const char *label,
    uint32_t leaf_va)
{
    char slots[17];
    uint16_t tag = 0;
    uint16_t active = 0;
    uint16_t empty = 0;
    uint16_t sentinels = 0;
    bool tag_ok;
    size_t i;

    if (!m || leaf_va < UINT32_C(0x80000000) || leaf_va >= UINT32_C(0x81000000))
        return;

    memset(slots, 0, sizeof(slots));
    tag_ok = load_va_half(m, leaf_va + 0x06u, &tag);
    for (i = 0; i < 16u; i++) {
        uint32_t slot = 0;
        bool slot_ok = load_va_word(m, leaf_va + 0x0Cu + (uint32_t)(i * 4u),
            &slot);

        if (!slot_ok) {
            slots[i] = '?';
            continue;
        }
        if (slot == 0u) {
            slots[i] = '.';
            empty++;
        } else if (slot == UINT32_C(0xFFFFFFC0)) {
            slots[i] = 'S';
            sentinels++;
        } else {
            slots[i] = 'X';
            active++;
        }
    }
    slots[16] = '\0';

    fprintf(stderr,
        "[WINCE_L2_LEAF] label=%s leaf=0x%08X key=%s%d slots=%s"
        " active=%u empty=%u sentinels=%u\n",
        label ? label : "-",
        leaf_va,
        tag_ok ? "" : "?",
        tag_ok ? (int)tag : 0,
        slots,
        (unsigned)active,
        (unsigned)empty,
        (unsigned)sentinels);
}

static void log_alloc_scan_state(machine_t *m, struct cpu *cpu,
    const char *label, uint32_t base_va, uint32_t type_tag,
    uint32_t start_idx, uint32_t start_slot, uint32_t request_count,
    bool processed_ok, uint32_t processed, bool result_ok, uint32_t result)
{
    uint32_t entry0 = 0;
    uint32_t entry1 = 0;
    bool entry0_ok;
    bool entry1_ok;
    char entry0_buf[16];
    char entry1_buf[16];
    char processed_buf[16];
    char result_buf[16];

    if (!m || !cpu)
        return;

    entry0_ok = load_va_word(m, base_va + start_idx * 4u, &entry0);
    entry1_ok = load_va_word(m, base_va + (start_idx + 1u) * 4u, &entry1);

    fprintf(stderr,
        "[WINCE_L2_ALLOC] label=%s pc=0x%08X ra=0x%08X sp=0x%08X"
        " base=0x%08X type=%u start_idx=%u start_slot=%u count=%u"
        " processed=%s result=%s entry0=%s entry1=%s\n",
        label ? label : "-",
        canonicalize_nk_pc((uint32_t)cpu->pc),
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
        base_va,
        type_tag,
        start_idx,
        start_slot,
        request_count,
        format_word_or_unknown(processed_buf, sizeof(processed_buf),
            processed_ok, processed),
        format_word_or_unknown(result_buf, sizeof(result_buf),
            result_ok, result),
        format_word_or_unknown(entry0_buf, sizeof(entry0_buf), entry0_ok, entry0),
        format_word_or_unknown(entry1_buf, sizeof(entry1_buf), entry1_ok, entry1));

    if (entry0_ok && entry0 > 1u)
        log_alloc_leaf_summary(m, "entry0", entry0);
    if (entry1_ok && entry1 > 1u)
        log_alloc_leaf_summary(m, "entry1", entry1);
}

static bool load_va_bytes(machine_t *m, uint32_t va, unsigned char *buf,
    size_t len)
{
    if (!m || !m->cpu || !buf || len == 0)
        return false;

    return m->cpu->memory_rw(m->cpu, m->cpu->mem, va32_to_mips64(va), buf, len,
        MEM_READ, CACHE_DATA | NO_EXCEPTIONS) != 0;
}

static bool load_guest_c_string(machine_t *m, uint32_t va_raw, char *out,
    size_t out_len)
{
    uint32_t va;
    size_t i;
    size_t start = 0;

    if (!m || !out || out_len == 0)
        return false;

    memset(out, 0, out_len);
    va = va_raw & ~UINT32_C(1);

    for (i = 0; i + 1 < out_len; i++) {
        unsigned char ch = 0;

        if (!load_va_bytes(m, va + (uint32_t)i, &ch, 1))
            break;
        out[i] = (char)ch;
        if (ch == '\0')
            break;
    }

    out[out_len - 1] = '\0';
    while (out[start] != '\0'
        && !isprint((unsigned char)out[start])) {
        start++;
    }
    if (start != 0)
        memmove(out, out + start, out_len - start);

    return out[0] != '\0';
}

static bool load_utf16_ascii(machine_t *m, struct cpu *cpu, uint32_t va,
    char *ascii, size_t ascii_len)
{
    unsigned char buf[256];
    size_t i;
    size_t j;

    if (!m || !cpu || !ascii || ascii_len == 0 || va == 0)
        return false;

    memset(buf, 0, sizeof(buf));
    memset(ascii, 0, ascii_len);
    cpu->memory_rw(cpu, cpu->mem, va32_to_mips64(va),
        buf, sizeof(buf), MEM_READ, CACHE_DATA | NO_EXCEPTIONS);

    for (i = 0, j = 0; i + 1 < sizeof(buf) && j + 1 < ascii_len; i += 2) {
        uint16_t wc = (uint16_t)buf[i] | ((uint16_t)buf[i + 1] << 8);

        if (wc == 0)
            break;
        ascii[j++] = (wc < 0x80) ? (char)wc : '?';
    }
    ascii[j] = '\0';
    return ascii[0] != '\0';
}

static const char *classify_va_space(uint32_t va)
{
    if (va == 0)
        return "null";
    if (va < 0x80000000u)
        return "user";
    if (va < 0xA0000000u)
        return "kseg0";
    if (va < 0xC0000000u)
        return "kseg1";
    if (va < 0xE0000000u)
        return "sseg";
    return "kseg2";
}

static bool parse_hex_field(const char *line, const char *key, uint32_t *out)
{
    const char *p;
    unsigned value;

    if (!line || !key || !out)
        return false;

    p = strstr(line, key);
    if (!p)
        return false;

    p += strlen(key);
    if (sscanf(p, "%x", &value) != 1)
        return false;

    *out = (uint32_t)value;
    return true;
}

static bool parse_exception_code(const char *line, uint32_t *out)
{
    const char *p;
    unsigned value;

    if (!line || !out)
        return false;

    p = strstr(line, "Exception ");
    if (!p)
        return false;
    p += 10;

    if (sscanf(p, "%x", &value) != 1)
        return false;

    *out = (uint32_t)value;
    return true;
}

static bool serial_exception_line_is_focus(const char *ascii)
{
    return ascii != NULL
        && (strstr(ascii, "Exception ") != NULL
            || strstr(ascii, "AKY=") != NULL
            || strstr(ascii, "Process '") != NULL);
}

static bool serial_exception_record_equal(
    const wince_serial_exception_record_t *a,
    const wince_serial_exception_record_t *b)
{
    if (!a || !b)
        return false;

    return a->valid == b->valid
        && a->code_valid == b->code_valid
        && a->thread_valid == b->thread_valid
        && a->proc_valid == b->proc_valid
        && a->aky_valid == b->aky_valid
        && a->pc_valid == b->pc_valid
        && a->ra_valid == b->ra_valid
        && a->bva_valid == b->bva_valid
        && a->code == b->code
        && a->thread == b->thread
        && a->proc == b->proc
        && a->aky == b->aky
        && a->pc == b->pc
        && a->ra == b->ra
        && a->bva == b->bva
        && strcmp(a->process_name, b->process_name) == 0;
}

static void reset_serial_exception_record(
    wince_serial_exception_record_t *rec)
{
    if (!rec)
        return;
    memset(rec, 0, sizeof(*rec));
}

static bool ppsh_debug_message_is_focus(const char *ascii)
{
    return ascii != NULL
        && (strstr(ascii, "PPSH Ctrl Err") != NULL
            || strstr(ascii, "NoPPFS") != NULL
            || strstr(ascii, "CtrlAddr=") != NULL
            || strstr(ascii, "PPFS:Time Outs") != NULL);
}

static void maybe_log_ppsh_debug_message(machine_t *m, struct cpu *cpu,
    const char *source, uint32_t str_va, const char *ascii)
{
    uint32_t callsite;
    uint32_t fb_events;

    if (!m || !cpu || !ascii || !ppsh_debug_message_is_focus(ascii))
        return;
    if (m->wince.ppsh_debug_msg_count >= 32)
        return;

    m->wince.ppsh_debug_msg_count++;
    fb_events = (uint32_t)m->wince.fb_watch_report_count
        + (uint32_t)m->wince.fb_write_diag_count;
    callsite = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA];
    if (callsite >= 8u)
        callsite -= 8u;

    fprintf(stderr,
        "[PPSH_MSG] #%u src=%s str=0x%08X callsite=0x%08X"
        " pc=0x%08X ra=0x%08X sp=0x%08X"
        " a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X"
        " seqs=%u last_cmd=0x%04X polls=%u flags=%u/%u"
        " sections=%u fb=%u msg=\"%s\"\n",
        (unsigned)m->wince.ppsh_debug_msg_count,
        source ? source : "?",
        str_va,
        callsite,
        (uint32_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
        (unsigned)m->wince.ppsh_cmd_seq_count,
        (unsigned)m->wince.ppsh_seq_cmd,
        (unsigned)m->wince.ppsh_poll_episode_count,
        (unsigned)m->wince.ppsh_flag_set_count,
        (unsigned)m->wince.ppsh_flag_clear_count,
        (unsigned)count_active_sections(m),
        (unsigned)fb_events,
        ascii);

    if (callsite != 0 && callsite != m->wince.ppsh_last_debug_callsite
        && m->wince.ppsh_debug_dump_count < 8) {
        m->wince.ppsh_last_debug_callsite = callsite;
        m->wince.ppsh_debug_dump_count++;
        dump_code_window(m, callsite, 2u, 8u);
    }
}

static void maybe_log_systempatch_context(machine_t *m, const char *reason)
{
    uint32_t cur_thrd = 0;
    uint32_t sec0;
    uint32_t sec1;
    const char *sec1_kind = "default";
    uint32_t proc_table = 0;

    if (!m || !m->wince.systempatch_seen)
        return;
    if (m->wince.systempatch_context_diag_count >= WINCE_SYSTEMPATCH_CTX_MAX)
        return;

    sec0 = load_pa_word(m, 0x18C0u);
    sec1 = load_pa_word(m, 0x18C4u);
    (void)load_va_word(m, UINT32_C(0x80669844), &cur_thrd);

    if (sec1 == m->wince.diag_shared_l2_table && sec1 != 0)
        sec1_kind = "shared";
    else if (sec1 != 0 && sec1 != 0x8008BC18u)
        sec1_kind = "process";

    if (sec0 != 0 && sec0 != 0x8008BC18u
        && sec0 != m->wince.diag_shared_l2_table) {
        proc_table = sec0;
    } else if (sec1 != 0 && sec1 != 0x8008BC18u
        && sec1 != m->wince.diag_shared_l2_table) {
        proc_table = sec1;
    }

    fprintf(stderr,
        "[WINCE_PROCESS] reason=%s serial_proc=0x%08X serial_thread=0x%08X"
        " cur_thrd=0x%08X sec0=0x%08X sec1=0x%08X sec1_kind=%s"
        " proc_table=0x%08X shared=0x%08X active_sections=%u\n",
        reason ? reason : "?",
        m->wince.serial_exc_last.proc_valid ? m->wince.serial_exc_last.proc : 0u,
        m->wince.serial_exc_last.thread_valid ? m->wince.serial_exc_last.thread : 0u,
        cur_thrd,
        sec0,
        sec1,
        sec1_kind,
        proc_table,
        m->wince.diag_shared_l2_table,
        (unsigned)count_active_sections(m));
    m->wince.systempatch_context_diag_count++;
}

static void maybe_sample_systempatch_thread_context(machine_t *m,
    struct cpu *cpu)
{
    uint32_t sec0;
    uint32_t sec1;
    uint32_t sec3;
    uint32_t cur_thrd = 0;
    uint32_t obj00 = 0;
    uint32_t obj04 = 0;
    uint32_t obj0c = 0;
    uint32_t obj14 = 0;
    uint32_t obj24 = 0;
    uint32_t obj3c = 0;
    uint32_t ctx08 = 0;
    uint32_t ctx0c = 0;
    uint32_t src_off = 0;
    uint32_t src_idx = 0;
    uint32_t src_slot = 0;
    bool cur_ok = false;
    bool obj00_ok = false;
    bool obj04_ok = false;
    bool obj0c_ok = false;
    bool obj14_ok = false;
    bool obj24_ok = false;
    bool obj3c_ok = false;
    bool ctx08_ok = false;
    bool ctx0c_ok = false;
    bool src_slot_ok = false;
    bool relevant;
    bool changed;
    char cur_buf[16];
    char obj00_buf[16];
    char obj04_buf[16];
    char obj0c_buf[16];
    char obj14_buf[16];
    char obj24_buf[16];
    char obj3c_buf[16];
    char ctx08_buf[16];
    char ctx0c_buf[16];
    char src_slot_buf[16];

    if (!m || !cpu || !m->wince.active)
        return;

    sec0 = load_pa_word(m, 0x18C0u);
    sec1 = load_pa_word(m, 0x18C4u);
    sec3 = load_pa_word(m, 0x18CCu);
    relevant = m->wince.systempatch_seen
        || sec0 == UINT32_C(0x80FE5000)
        || sec3 == UINT32_C(0x80FE5000);
    if (!relevant)
        return;
    if (m->wince.systempatch_thread_ctx_diag_count
        >= WINCE_SYSTEMPATCH_THREAD_CTX_MAX) {
        return;
    }

    cur_ok = load_va_word(m, UINT32_C(0x80669844), &cur_thrd);
    if (cur_ok && cur_thrd >= UINT32_C(0x80000000)
        && cur_thrd < UINT32_C(0x81000000)) {
        obj0c_ok = load_va_word(m, cur_thrd + 0x0Cu, &obj0c);
        obj14_ok = load_va_word(m, cur_thrd + 0x14u, &obj14);
        obj24_ok = load_va_word(m, cur_thrd + 0x24u, &obj24);
        obj3c_ok = load_va_word(m, cur_thrd + 0x3Cu, &obj3c);
        if (obj0c_ok && obj0c >= UINT32_C(0x80000000)
            && obj0c < UINT32_C(0x81000000)) {
            obj00_ok = load_va_word(m, obj0c + 0x00u, &obj00);
            obj04_ok = load_va_word(m, obj0c + 0x04u, &obj04);
            ctx08_ok = load_va_word(m, obj0c + 0x08u, &ctx08);
            ctx0c_ok = load_va_word(m, obj0c + 0x0Cu, &ctx0c);
            if (ctx0c_ok) {
                src_off = ctx0c >> 23;
                src_idx = src_off >> 2;
                if (src_off < 0x100u) {
                    src_slot = load_pa_word(m, 0x18C0u + src_off);
                    src_slot_ok = true;
                }
            }
        }
    }

    changed = !m->wince.systempatch_thread_ctx_valid
        || m->wince.systempatch_last_cur_thrd != cur_thrd
        || m->wince.systempatch_last_obj00 != obj00
        || m->wince.systempatch_last_obj04 != obj04
        || m->wince.systempatch_last_obj0c != obj0c
        || m->wince.systempatch_last_obj14 != obj14
        || m->wince.systempatch_last_obj24 != obj24
        || m->wince.systempatch_last_obj3c != obj3c
        || m->wince.systempatch_last_ctx08 != ctx08
        || m->wince.systempatch_last_ctx0c != ctx0c
        || m->wince.systempatch_last_src_slot != src_slot
        || m->wince.systempatch_last_sec0 != sec0
        || m->wince.systempatch_last_sec1 != sec1
        || m->wince.systempatch_last_sec3 != sec3;
    if (!changed)
        return;

    fprintf(stderr,
        "[WINCE_THREAD_CTX] reason=%s cur_thrd=%s obj+00=%s obj+04=%s"
        " obj+0c=%s obj+14=%s obj+24=%s obj+3c=%s"
        " ctx+08=%s ctx+0c=%s asid=%s0x%02X"
        " src_off=0x%03X src_idx=%u src_slot=%s"
        " sec0=0x%08X sec1=0x%08X sec3=0x%08X"
        " pc=0x%08X ra=0x%08X sp=0x%08X\n",
        m->wince.systempatch_seen ? "systempatch" : "sec3_attach",
        format_word_or_unknown(cur_buf, sizeof(cur_buf), cur_ok, cur_thrd),
        format_word_or_unknown(obj00_buf, sizeof(obj00_buf), obj00_ok, obj00),
        format_word_or_unknown(obj04_buf, sizeof(obj04_buf), obj04_ok, obj04),
        format_word_or_unknown(obj0c_buf, sizeof(obj0c_buf), obj0c_ok, obj0c),
        format_word_or_unknown(obj14_buf, sizeof(obj14_buf), obj14_ok, obj14),
        format_word_or_unknown(obj24_buf, sizeof(obj24_buf), obj24_ok, obj24),
        format_word_or_unknown(obj3c_buf, sizeof(obj3c_buf), obj3c_ok, obj3c),
        format_word_or_unknown(ctx08_buf, sizeof(ctx08_buf), ctx08_ok, ctx08),
        format_word_or_unknown(ctx0c_buf, sizeof(ctx0c_buf), ctx0c_ok, ctx0c),
        obj00_ok ? "" : "?",
        obj00_ok ? (obj00 & 0xFFu) : 0u,
        src_off,
        src_idx,
        format_word_or_unknown(src_slot_buf, sizeof(src_slot_buf), src_slot_ok,
            src_slot),
        sec0,
        sec1,
        sec3,
        (uint32_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP]);
    if (!m->wince.systempatch_thread_ctx_valid
        || m->wince.systempatch_last_obj0c != obj0c
        || m->wince.systempatch_last_ctx0c != ctx0c
        || m->wince.systempatch_last_src_slot != src_slot) {
        dump_code_window(m, (uint32_t)cpu->pc, 4u, 8u);
        if (cpu->cd.mips.gpr[MIPS_GPR_RA] >= 8u) {
            dump_code_window(m,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA] - 8u, 4u, 8u);
        }
    }
    if (sec0 == UINT32_C(0x80FE5000) || sec3 == UINT32_C(0x80FE5000)) {
        log_l2_table_state(m, "thread_ctx", UINT32_C(0x80FE5000),
            UINT32_C(0x01F94B50));
        log_section0_focus_window(m, "thread_ctx", UINT32_C(0x80FE5000));
    }

    m->wince.systempatch_thread_ctx_valid = true;
    m->wince.systempatch_last_cur_thrd = cur_thrd;
    m->wince.systempatch_last_obj00 = obj00;
    m->wince.systempatch_last_obj04 = obj04;
    m->wince.systempatch_last_obj0c = obj0c;
    m->wince.systempatch_last_obj14 = obj14;
    m->wince.systempatch_last_obj24 = obj24;
    m->wince.systempatch_last_obj3c = obj3c;
    m->wince.systempatch_last_ctx08 = ctx08;
    m->wince.systempatch_last_ctx0c = ctx0c;
    m->wince.systempatch_last_src_slot = src_slot;
    m->wince.systempatch_last_sec0 = sec0;
    m->wince.systempatch_last_sec1 = sec1;
    m->wince.systempatch_last_sec3 = sec3;
    m->wince.systempatch_thread_ctx_diag_count++;
}

static void maybe_commit_serial_exception(machine_t *m, const char *reason)
{
    wince_serial_exception_record_t *pending;

    if (!m)
        return;

    pending = &m->wince.serial_exc_pending;
    if (!pending->thread_valid || !pending->pc_valid || !pending->bva_valid)
        return;
    if (!pending->code_valid && !pending->aky_valid)
        return;

    pending->valid = true;
    if (serial_exception_record_equal(pending, &m->wince.serial_exc_last))
        return;

    m->wince.serial_exc_last = *pending;
    if (m->wince.serial_exception_diag_count < WINCE_SERIAL_EXC_LOG_MAX) {
        fprintf(stderr,
            "[WINCE_EXC_SERIAL] reason=%s code=%s0x%03X"
            " thread=0x%08X proc=%s0x%08X aky=%s0x%08X"
            " pc=0x%08X ra=%s0x%08X bva=0x%08X process=\"%s\"\n",
            reason ? reason : "?",
            pending->code_valid ? "" : "?",
            pending->code,
            pending->thread,
            pending->proc_valid ? "" : "?",
            pending->proc,
            pending->aky_valid ? "" : "?",
            pending->aky,
            pending->pc,
            pending->ra_valid ? "" : "?",
            pending->ra,
            pending->bva,
            pending->process_name[0] != '\0'
                ? pending->process_name : "?");
        m->wince.serial_exception_diag_count++;
    }

    if (strcmp(pending->process_name, "SystemPatchModule.exe") == 0) {
        m->wince.systempatch_seen = true;
        maybe_log_systempatch_context(m,
            m->wince.systempatch_process_logged
                ? "serial_exception"
                : "serial_process");
        m->wince.systempatch_process_logged = true;
    }
}

static void maybe_record_serial_exception_line(machine_t *m, const char *line)
{
    wince_serial_exception_record_t *pending;
    uint32_t value;
    const char *p;

    if (!m || !line || !serial_exception_line_is_focus(line))
        return;

    pending = &m->wince.serial_exc_pending;

    if (strstr(line, "Exception ") != NULL) {
        char saved_process[sizeof(pending->process_name)];
        uint32_t saved_proc = pending->proc;
        bool saved_proc_valid = pending->proc_valid;

        memcpy(saved_process, pending->process_name, sizeof(saved_process));
        reset_serial_exception_record(pending);
        memcpy(pending->process_name, saved_process, sizeof(pending->process_name));
        pending->proc = saved_proc;
        pending->proc_valid = saved_proc_valid;
    }

    p = strstr(line, "Process '");
    if (p != NULL) {
        size_t len;

        p += 9;
        len = strcspn(p, "'");
        if (len >= sizeof(pending->process_name))
            len = sizeof(pending->process_name) - 1u;
        memset(pending->process_name, 0, sizeof(pending->process_name));
        memcpy(pending->process_name, p, len);
        if (strcmp(pending->process_name, "SystemPatchModule.exe") == 0
            && !m->wince.systempatch_process_logged) {
            m->wince.systempatch_seen = true;
            maybe_log_systempatch_context(m, "serial_process");
            m->wince.systempatch_process_logged = true;
        }
    }

    if (parse_exception_code(line, &value)) {
        pending->code = value;
        pending->code_valid = true;
    }
    if (parse_hex_field(line, "Thread=", &value)) {
        pending->thread = value;
        pending->thread_valid = true;
    }
    if (parse_hex_field(line, "Proc=", &value)) {
        pending->proc = value;
        pending->proc_valid = true;
    }
    if (parse_hex_field(line, "AKY=", &value)) {
        pending->aky = value;
        pending->aky_valid = true;
    }
    if (parse_hex_field(line, "PC=", &value)) {
        pending->pc = canonicalize_nk_pc(value);
        pending->pc_valid = true;
    }
    if (parse_hex_field(line, "RA=", &value)) {
        pending->ra = canonicalize_nk_pc(value);
        pending->ra_valid = true;
    }
    if (parse_hex_field(line, "BVA=", &value)) {
        pending->bva = value;
        pending->bva_valid = true;
    }

    maybe_commit_serial_exception(m, "serial_line");
}

static void maybe_flush_ppsh_serial_line(machine_t *m)
{
    uint32_t fb_events;
    uint32_t callsite_first;
    uint32_t callsite_last;
    uint32_t ret1 = 0;
    uint32_t ret2 = 0;
    char a0_ascii[96];
    char s0_ascii[96];

    if (!m || m->wince.ppsh_serial_line_len == 0)
        return;

    m->wince.ppsh_serial_line[m->wince.ppsh_serial_line_len] = '\0';
    if (serial_exception_line_is_focus(m->wince.ppsh_serial_line))
        maybe_record_serial_exception_line(m, m->wince.ppsh_serial_line);

    if (!ppsh_debug_message_is_focus(m->wince.ppsh_serial_line)) {
        m->wince.ppsh_serial_line_len = 0;
        return;
    }
    if (m->wince.ppsh_serial_msg_count >= 32) {
        m->wince.ppsh_serial_line_len = 0;
        return;
    }

    m->wince.ppsh_serial_msg_count++;
    callsite_first = m->wince.ppsh_serial_first_ra >= 8u
        ? m->wince.ppsh_serial_first_ra - 8u : 0u;
    callsite_last = m->wince.ppsh_serial_last_ra >= 8u
        ? m->wince.ppsh_serial_last_ra - 8u : 0u;
    fb_events = (uint32_t)m->wince.fb_watch_report_count
        + (uint32_t)m->wince.fb_write_diag_count;
    if (m->wince.ppsh_serial_first_sp != 0) {
        (void)load_va_word(m, m->wince.ppsh_serial_first_sp + 0x14u, &ret1);
        (void)load_va_word(m, m->wince.ppsh_serial_first_sp + 0x2Cu, &ret2);
    }

    fprintf(stderr,
        "[PPSH_UART] #%u first_pc=0x%08X first_ra=0x%08X"
        " last_pc=0x%08X last_ra=0x%08X"
        " call_first=0x%08X call_last=0x%08X"
        " seqs=%u last_cmd=0x%04X polls=%u flags=%u/%u"
        " sections=%u fb=%u line=\"%s\"\n",
        (unsigned)m->wince.ppsh_serial_msg_count,
        m->wince.ppsh_serial_first_pc,
        m->wince.ppsh_serial_first_ra,
        m->wince.ppsh_serial_last_pc,
        m->wince.ppsh_serial_last_ra,
        callsite_first,
        callsite_last,
        (unsigned)m->wince.ppsh_cmd_seq_count,
        (unsigned)m->wince.ppsh_seq_cmd,
        (unsigned)m->wince.ppsh_poll_episode_count,
        (unsigned)m->wince.ppsh_flag_set_count,
        (unsigned)m->wince.ppsh_flag_clear_count,
        (unsigned)count_active_sections(m),
        (unsigned)fb_events,
        m->wince.ppsh_serial_line);
    fprintf(stderr,
        "[PPSH_UART] #%u regs sp=0x%08X"
        " a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X"
        " v0=0x%08X v1=0x%08X"
        " s0=0x%08X s1=0x%08X s2=0x%08X s3=0x%08X s4=0x%08X t9=0x%08X"
        " stack=%08X/%08X/%08X/%08X"
        " ret1=0x%08X ret2=0x%08X\n",
        (unsigned)m->wince.ppsh_serial_msg_count,
        m->wince.ppsh_serial_first_sp,
        m->wince.ppsh_serial_first_a0,
        m->wince.ppsh_serial_first_a1,
        m->wince.ppsh_serial_first_a2,
        m->wince.ppsh_serial_first_a3,
        m->wince.ppsh_serial_first_v0,
        m->wince.ppsh_serial_first_v1,
        m->wince.ppsh_serial_first_s0,
        m->wince.ppsh_serial_first_s1,
        m->wince.ppsh_serial_first_s2,
        m->wince.ppsh_serial_first_s3,
        m->wince.ppsh_serial_first_s4,
        m->wince.ppsh_serial_first_t9,
        m->wince.ppsh_serial_stack0,
        m->wince.ppsh_serial_stack1,
        m->wince.ppsh_serial_stack2,
        m->wince.ppsh_serial_stack3,
        ret1,
        ret2);

    memset(a0_ascii, 0, sizeof(a0_ascii));
    memset(s0_ascii, 0, sizeof(s0_ascii));
    if (m->cpu && load_utf16_ascii(m, m->cpu, m->wince.ppsh_serial_first_a0,
            a0_ascii, sizeof(a0_ascii))) {
        fprintf(stderr, "[PPSH_UART] #%u a0_utf16=\"%s\"\n",
            (unsigned)m->wince.ppsh_serial_msg_count, a0_ascii);
    }
    if (m->cpu && load_utf16_ascii(m, m->cpu, m->wince.ppsh_serial_first_s0,
            s0_ascii, sizeof(s0_ascii))) {
        fprintf(stderr, "[PPSH_UART] #%u s0_utf16=\"%s\"\n",
            (unsigned)m->wince.ppsh_serial_msg_count, s0_ascii);
    }

    if (((m->wince.ppsh_serial_first_a0 != 0
          && m->wince.ppsh_serial_first_a0 != m->wince.ppsh_last_callsite_fmt)
         || (m->wince.ppsh_serial_first_sp != 0
             && m->wince.ppsh_serial_first_sp != m->wince.ppsh_last_serial_dump_sp))
        && m->wince.ppsh_debug_dump_count < 8) {
        m->wince.ppsh_last_callsite_fmt = m->wince.ppsh_serial_first_a0;
        m->wince.ppsh_last_serial_dump_sp = m->wince.ppsh_serial_first_sp;
        m->wince.ppsh_last_debug_callsite = callsite_last;
        m->wince.ppsh_debug_dump_count++;
        dump_code_window(m, callsite_last, 8u, 8u);
        dump_code_window(m, m->wince.ppsh_serial_first_pc, 4u, 8u);
        dump_va_window(m, "ppsh_uart_a0",
            m->wince.ppsh_serial_first_a0 & ~UINT32_C(0x1F), 0x60u);
        if (m->wince.ppsh_serial_first_sp != 0)
            dump_va_window(m, "ppsh_uart_stack",
                m->wince.ppsh_serial_first_sp, 0x40u);
        if (ret1 >= UINT32_C(0x80000000))
            dump_code_window(m, ret1 - 8u, 4u, 10u);
        if (ret2 >= UINT32_C(0x80000000))
            dump_code_window(m, ret2 - 8u, 4u, 10u);
    } else if (callsite_last != 0
        && callsite_last != m->wince.ppsh_last_debug_callsite
        && m->wince.ppsh_debug_dump_count < 8) {
        m->wince.ppsh_last_debug_callsite = callsite_last;
        m->wince.ppsh_debug_dump_count++;
        dump_code_window(m, callsite_last, 8u, 8u);
        dump_code_window(m, m->wince.ppsh_serial_first_pc, 4u, 8u);
    }

    m->wince.ppsh_serial_line_len = 0;
}

static size_t collect_stack_return_sites(machine_t *m, uint32_t sp,
    uint32_t *ret_offs, uint32_t *ret_addrs, size_t cap, uint32_t max_scan)
{
    size_t count = 0;
    uint32_t off;

    if (!m || sp == 0 || !ret_offs || !ret_addrs || cap == 0)
        return 0;
    if (max_scan == 0)
        max_scan = 0x40u;

    for (off = 0; off < max_scan; off += 4u) {
        uint32_t word = 0;
        size_t i;
        bool seen = false;

        if (!load_va_word(m, sp + off, &word))
            continue;
        if (word < 0x80060000u || word >= 0x81000000u)
            continue;

        for (i = 0; i < count; i++) {
            if (ret_addrs[i] == word) {
                seen = true;
                break;
            }
        }
        if (seen)
            continue;

        ret_offs[count] = off;
        ret_addrs[count] = word;
        count++;
        if (count >= cap)
            break;
    }

    return count;
}

static void maybe_dump_ppsh_helper_context(machine_t *m, struct cpu *cpu,
    uint16_t cmd)
{
    uint32_t pc;
    uint32_t ra;
    uint32_t sp;
    uint32_t caller;
    uint32_t stk0 = 0;
    uint32_t stk1 = 0;
    uint32_t stk2 = 0;
    uint32_t stk3 = 0;
    uint32_t ret_offs[4] = {0};
    uint32_t ret_addrs[4] = {0};
    size_t ret_count = 0;

    if (!m || !cpu)
        return;

    pc = (uint32_t)cpu->pc;
    ra = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA];
    sp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
    caller = ra >= 8u ? ra - 8u : 0u;
    (void)load_va_word(m, sp + 0u, &stk0);
    (void)load_va_word(m, sp + 4u, &stk1);
    (void)load_va_word(m, sp + 8u, &stk2);
    (void)load_va_word(m, sp + 12u, &stk3);
    ret_count = collect_stack_return_sites(m, sp, ret_offs, ret_addrs,
        sizeof(ret_addrs) / sizeof(ret_addrs[0]), 0x40u);

    fprintf(stderr,
        "[PPSH_HELPER] #%u cmd=0x%04X pc=0x%08X ra=0x%08X caller=0x%08X"
        " sp=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X"
        " v0=0x%08X v1=0x%08X s0=0x%08X s1=0x%08X s2=0x%08X s3=0x%08X s4=0x%08X"
        " stack=%08X/%08X/%08X/%08X\n",
        (unsigned)m->wince.ppsh_cmd_seq_count,
        (unsigned)cmd,
        pc,
        ra,
        caller,
        sp,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V1],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S2],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S3],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S4],
        stk0, stk1, stk2, stk3);
    if (ret_count > 0) {
        size_t i;

        fprintf(stderr,
            "[PPSH_HELPER_RET] #%u", (unsigned)m->wince.ppsh_cmd_seq_count);
        for (i = 0; i < ret_count; i++) {
            fprintf(stderr, " ret%zu=%#010x@+0x%02X callsite=0x%08X",
                i,
                ret_addrs[i],
                ret_offs[i],
                ret_addrs[i] >= 8u ? ret_addrs[i] - 8u : 0u);
        }
        fputc('\n', stderr);
    }

    if (m->wince.ppsh_helper_dump_count >= 6u)
        return;
    if (pc == m->wince.ppsh_last_helper_pc)
        return;

    m->wince.ppsh_last_helper_pc = pc;
    m->wince.ppsh_helper_dump_count++;
    dump_code_window(m, pc, 8u, 12u);
    if (caller != 0)
        dump_code_window(m, caller, 8u, 12u);
    if (ret_count > 1 && ret_addrs[1] >= 8u)
        dump_code_window(m, ret_addrs[1] - 8u, 8u, 12u);
    if (ret_count > 2 && ret_addrs[2] >= 8u)
        dump_code_window(m, ret_addrs[2] - 8u, 8u, 12u);
    if (sp != 0)
        dump_va_window(m, "ppsh_helper_stack", sp, 0x40u);
}

static void maybe_dump_ppsh_flag_context(machine_t *m, struct cpu *cpu,
    uint32_t old_flag, uint32_t new_flag)
{
    uint32_t pc;
    uint32_t ra;
    uint32_t sp;
    uint32_t caller;
    uint32_t ret_offs[4] = {0};
    uint32_t ret_addrs[4] = {0};
    size_t ret_count = 0;
    bool has_97ec8 = false;
    bool has_81a68 = false;
    size_t i;

    if (!m || !cpu)
        return;

    pc = (uint32_t)cpu->pc;
    ra = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA];
    sp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
    caller = ra >= 8u ? ra - 8u : 0u;
    ret_count = collect_stack_return_sites(m, sp, ret_offs, ret_addrs,
        sizeof(ret_addrs) / sizeof(ret_addrs[0]), 0x40u);
    for (i = 0; i < ret_count; i++) {
        if (ret_addrs[i] == 0x80097EC8u)
            has_97ec8 = true;
        else if (ret_addrs[i] == 0x80081A68u)
            has_81a68 = true;
    }

    fprintf(stderr,
        "[PPSH_FLAG_CTX] #%u %u->%u pc=0x%08X ra=0x%08X caller=0x%08X"
        " sp=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X"
        " v0=0x%08X v1=0x%08X s0=0x%08X s1=0x%08X s2=0x%08X s3=0x%08X s4=0x%08X\n",
        (unsigned)m->wince.ppsh_flag_transition_count,
        old_flag,
        new_flag,
        pc,
        ra,
        caller,
        sp,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V1],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S2],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S3],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S4]);
    if (ret_count > 0) {
        size_t i;

        fprintf(stderr, "[PPSH_FLAG_RET] #%u",
            (unsigned)m->wince.ppsh_flag_transition_count);
        for (i = 0; i < ret_count; i++) {
            fprintf(stderr, " ret%zu=%#010x@+0x%02X callsite=0x%08X",
                i,
                ret_addrs[i],
                ret_offs[i],
                ret_addrs[i] >= 8u ? ret_addrs[i] - 8u : 0u);
        }
        fputc('\n', stderr);
    }

    if (m->wince.ppsh_flag_dump_count >= 6u)
        return;
    if (pc == m->wince.ppsh_last_flag_pc)
        return;

    m->wince.ppsh_last_flag_pc = pc;
    m->wince.ppsh_flag_dump_count++;
    dump_code_window(m, pc, 8u, 12u);
    if (caller != 0)
        dump_code_window(m, caller, 8u, 12u);
    if (ret_count > 1 && ret_addrs[1] >= 8u)
        dump_code_window(m, ret_addrs[1] - 8u, 8u, 12u);
    if (ret_count > 2 && ret_addrs[2] >= 8u)
        dump_code_window(m, ret_addrs[2] - 8u, 8u, 12u);
    if (ret_count > 3 && ret_addrs[3] >= 8u)
        dump_code_window(m, ret_addrs[3] - 8u, 8u, 12u);
    if (m->wince.ppsh_flag_transition_count <= 6u)
        log_ppsh_timeout_state(m, "flag");
    if (!m->wince.ppsh_timeout_path_dumped
        && (has_97ec8 || has_81a68)) {
        m->wince.ppsh_timeout_path_dumped = true;
        fprintf(stderr,
            "[PPSH_PATH] ppfs_timeout pc=0x%08X ra=0x%08X sp=0x%08X"
            " ret_97ec8=%d ret_81a68=%d seqs=%u last_cmd=0x%04X"
            " polls=%u flags=%u/%u\n",
            pc, ra, sp,
            has_97ec8 ? 1 : 0,
            has_81a68 ? 1 : 0,
            (unsigned)m->wince.ppsh_cmd_seq_count,
            (unsigned)m->wince.ppsh_seq_cmd,
            (unsigned)m->wince.ppsh_poll_episode_count,
            (unsigned)m->wince.ppsh_flag_set_count,
            (unsigned)m->wince.ppsh_flag_clear_count);
        log_ppsh_timeout_state(m, "timeout");
        dump_code_window(m, 0x80081A60u, 8u, 12u);
        dump_code_window(m, 0x80097EC0u, 8u, 12u);
        dump_code_window(m, 0x800815E0u, 8u, 12u);
        dump_code_window(m, 0x8008C564u, 8u, 12u);
        dump_code_window(m, 0x80099924u, 8u, 12u);
        dump_code_window(m, 0x800A11B0u, 8u, 12u);
        dump_va_window(m, "ppsh_evt_97a0", UINT32_C(0x806697A0), 0x40u);
        dump_va_window(m, "ppsh_evt_97c0", UINT32_C(0x806697C0), 0x40u);
    }
    if (sp != 0)
        dump_va_window(m, "ppsh_flag_stack", sp, 0x40u);
}

typedef struct {
    uint32_t pc;
    const char *label;
} ppsh_exact_pc_t;

static void maybe_note_ppsh_exact_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32)
{
    static const ppsh_exact_pc_t targets[] = {
        { 0x80097CC4u, "ppfs_evt_failfast_call" },
        { 0x80081A60u, "ppfs_list_walk" },
        { 0x80097EC0u, "ppfs_timeout_dispatch" },
        { 0x80098C70u, "ppfs_evt_owner_scan" },
        { 0x80098DACu, "ppfs_evt_wait_call" },
        { 0x80098E08u, "ppfs_evt_state57" },
        { 0x80098E20u, "ppfs_evt_retry_call" },
        { 0x80098E44u, "ppfs_evt_payload_copy" },
        { 0x80098EF4u, "ppfs_evt_final_call" },
        { 0x800998C0u, "ppfs_evt_seed" },
        { 0x8008C564u, "ppfs_tlb_reset" },
        { 0x80099924u, "ppfs_evt_update" },
        { 0x800A12B8u, "ppfs_cond_store" },
        { 0x800A12E0u, "ppfs_inc_counter" },
    };
    uint32_t pc32;
    uint32_t sp;
    uint32_t ret_offs[6] = {0};
    uint32_t ret_addrs[6] = {0};
    size_t ret_count;
    size_t i;

    if (!m || !cpu || !ppsh_trace_enabled(m))
        return;
    if (m->wince.ppsh_cmd_seq_count == 0
        && m->wince.ppsh_flag_transition_count == 0)
        return;

    pc32 = canonicalize_nk_pc(raw_pc32);
    for (i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        uint32_t a0;
        uint32_t a0w0 = 0;
        uint32_t a0w1 = 0;
        uint32_t a0w2 = 0;
        uint32_t a0w3 = 0;
        bool a0w0_ok = false;
        bool a0w1_ok = false;
        bool a0w2_ok = false;
        bool a0w3_ok = false;
        char a0w0_buf[16];
        char a0w1_buf[16];
        char a0w2_buf[16];
        char a0w3_buf[16];

        if (pc32 != targets[i].pc)
            continue;
        if ((m->wince.ppsh_exact_pc_mask & (UINT32_C(1) << i)) != 0)
            return;

        m->wince.ppsh_exact_pc_mask |= (UINT32_C(1) << i);
        sp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
        a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
        ret_count = collect_stack_return_sites(m, sp, ret_offs, ret_addrs,
            sizeof(ret_addrs) / sizeof(ret_addrs[0]), 0x80u);
        if (a0 != 0) {
            a0w0_ok = load_va_word(m, a0 + 0u, &a0w0);
            a0w1_ok = load_va_word(m, a0 + 4u, &a0w1);
            a0w2_ok = load_va_word(m, a0 + 8u, &a0w2);
            a0w3_ok = load_va_word(m, a0 + 12u, &a0w3);
        }

        fprintf(stderr,
            "[PPSH_PC] %s pc=0x%08X ra=0x%08X sp=0x%08X"
            " a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X"
            " v0=0x%08X v1=0x%08X s0=0x%08X s1=0x%08X s2=0x%08X s3=0x%08X s4=0x%08X\n",
            targets[i].label,
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            sp,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S2],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S3],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S4]);
        fprintf(stderr,
            "[PPSH_PC] %s a0_words=%s/%s/%s/%s\n",
            targets[i].label,
            format_word_or_unknown(a0w0_buf, sizeof(a0w0_buf), a0w0_ok, a0w0),
            format_word_or_unknown(a0w1_buf, sizeof(a0w1_buf), a0w1_ok, a0w1),
            format_word_or_unknown(a0w2_buf, sizeof(a0w2_buf), a0w2_ok, a0w2),
            format_word_or_unknown(a0w3_buf, sizeof(a0w3_buf), a0w3_ok, a0w3));
        log_ppsh_timeout_state(m, targets[i].label);
        if (ret_count > 0) {
            size_t j;

            fprintf(stderr, "[PPSH_PC_RET] %s", targets[i].label);
            for (j = 0; j < ret_count; j++) {
                fprintf(stderr, " ret%zu=%#010x@+0x%02X callsite=0x%08X",
                    j,
                    ret_addrs[j],
                    ret_offs[j],
                    ret_addrs[j] >= 8u ? ret_addrs[j] - 8u : 0u);
            }
            fputc('\n', stderr);
        }

        dump_code_window(m, pc32, 8u, 12u);
        for (i = 0; i < ret_count; i++) {
            if (ret_addrs[i] >= 8u)
                dump_code_window(m, ret_addrs[i] - 8u, 8u, 12u);
        }
        if (sp != 0)
            dump_va_window(m, "ppsh_pc_stack", sp, 0x80u);
        return;
    }
}

static void log_ppsh_timeout_state(machine_t *m, const char *tag)
{
    uint32_t db08 = 0;
    uint32_t db10 = 0;
    uint32_t db34 = 0;
    uint32_t dac0_ptr = 0;
    uint32_t dac0_slot38 = 0;
    uint32_t obj97a0[4] = {0};
    uint32_t obj97c0[4] = {0};
    bool db08_ok;
    bool db10_ok;
    bool db34_ok;
    bool dac0_ok;
    bool slot38_ok = false;
    bool obj97a0_ok[4];
    bool obj97c0_ok[4];
    char db08_buf[16];
    char db10_buf[16];
    char db34_buf[16];
    char dac0_buf[16];
    char slot38_buf[16];
    char a_buf[4][16];
    char c_buf[4][16];
    size_t i;

    if (!m)
        return;

    db08_ok = load_va_word(m, UINT32_C(0xFFFFD808), &db08);
    db10_ok = load_va_word(m, UINT32_C(0xFFFFDB10), &db10);
    db34_ok = load_va_word(m, UINT32_C(0xFFFFDB34), &db34);
    dac0_ok = load_va_word(m, UINT32_C(0xFFFFDAC0), &dac0_ptr);
    if (dac0_ok && dac0_ptr >= UINT32_C(0x80000000)
        && dac0_ptr < UINT32_C(0x81000000)) {
        slot38_ok = load_va_word(m, dac0_ptr + 0x38u, &dac0_slot38);
    }

    for (i = 0; i < 4; i++) {
        obj97a0_ok[i] = load_va_word(m, UINT32_C(0x806697A0) + (uint32_t)(i * 4u),
            &obj97a0[i]);
        obj97c0_ok[i] = load_va_word(m, UINT32_C(0x806697C0) + (uint32_t)(i * 4u),
            &obj97c0[i]);
    }

    fprintf(stderr,
        "[PPSH_STATE] tag=%s db08=%s db10=%s db34=%s dac0=%s slot38=%s"
        " seqs=%u last_cmd=0x%04X polls=%u flags=%u/%u\n",
        tag ? tag : "?",
        format_word_or_unknown(db08_buf, sizeof(db08_buf), db08_ok, db08),
        format_word_or_unknown(db10_buf, sizeof(db10_buf), db10_ok, db10),
        format_word_or_unknown(db34_buf, sizeof(db34_buf), db34_ok, db34),
        format_word_or_unknown(dac0_buf, sizeof(dac0_buf), dac0_ok, dac0_ptr),
        format_word_or_unknown(slot38_buf, sizeof(slot38_buf), slot38_ok,
            dac0_slot38),
        (unsigned)m->wince.ppsh_cmd_seq_count,
        (unsigned)m->wince.ppsh_seq_cmd,
        (unsigned)m->wince.ppsh_poll_episode_count,
        (unsigned)m->wince.ppsh_flag_set_count,
        (unsigned)m->wince.ppsh_flag_clear_count);
    fprintf(stderr,
        "[PPSH_STATE] tag=%s obj97a0=%s/%s/%s/%s obj97c0=%s/%s/%s/%s\n",
        tag ? tag : "?",
        format_word_or_unknown(a_buf[0], sizeof(a_buf[0]), obj97a0_ok[0], obj97a0[0]),
        format_word_or_unknown(a_buf[1], sizeof(a_buf[1]), obj97a0_ok[1], obj97a0[1]),
        format_word_or_unknown(a_buf[2], sizeof(a_buf[2]), obj97a0_ok[2], obj97a0[2]),
        format_word_or_unknown(a_buf[3], sizeof(a_buf[3]), obj97a0_ok[3], obj97a0[3]),
        format_word_or_unknown(c_buf[0], sizeof(c_buf[0]), obj97c0_ok[0], obj97c0[0]),
        format_word_or_unknown(c_buf[1], sizeof(c_buf[1]), obj97c0_ok[1], obj97c0[1]),
        format_word_or_unknown(c_buf[2], sizeof(c_buf[2]), obj97c0_ok[2], obj97c0[2]),
        format_word_or_unknown(c_buf[3], sizeof(c_buf[3]), obj97c0_ok[3], obj97c0[3]));
}

static void dump_pointer_bytes(machine_t *m, const char *label, uint32_t va)
{
    unsigned char buf[16];
    size_t i;

    if (!m || !label)
        return;

    memset(buf, 0, sizeof(buf));
    if (!load_va_bytes(m, va, buf, sizeof(buf))) {
        fprintf(stderr,
            "[WINCE_PTR] %s va=0x%08X space=%s status=unmapped\n",
            label, va, classify_va_space(va));
        return;
    }

    fprintf(stderr,
        "[WINCE_PTR] %s va=0x%08X space=%s bytes=",
        label, va, classify_va_space(va));
    for (i = 0; i < sizeof(buf); i++)
        fprintf(stderr, "%02X", buf[i]);
    fputc('\n', stderr);
}

static void maybe_log_serial_exception_correlation(machine_t *m,
    struct cpu *cpu, uint32_t exccode, uint32_t fault_vaddr,
    const char *phase)
{
    const wince_serial_exception_record_t *rec;
    struct mips_coproc *cp0;
    uint32_t epc;
    bool exact_bva;
    bool same_page;
    bool epc_match;
    bool ra_match;

    if (!m || !cpu)
        return;
    if (m->wince.serial_exception_corr_count >= WINCE_SERIAL_CORR_LOG_MAX)
        return;

    rec = &m->wince.serial_exc_last;
    if (!rec->valid || !rec->pc_valid || !rec->bva_valid)
        return;

    cp0 = cpu->cd.mips.coproc[0];
    if (!cp0)
        return;

    epc = (uint32_t)cp0->reg[COP0_EPC];
    exact_bva = fault_vaddr == rec->bva;
    same_page = (fault_vaddr & ~UINT32_C(0xFFF))
        == (rec->bva & ~UINT32_C(0xFFF));
    epc_match = canonicalize_nk_pc(epc) == canonicalize_nk_pc(rec->pc);
    ra_match = rec->ra_valid
        && canonicalize_nk_pc((uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA])
            == canonicalize_nk_pc(rec->ra);

    if (!exact_bva && !same_page && !epc_match && !ra_match)
        return;

    fprintf(stderr,
        "[WINCE_EXC_CORR] phase=%s exc=%u fault=0x%08X epc=0x%08X"
        " serial_code=0x%03X serial_pc=0x%08X serial_ra=%s0x%08X"
        " serial_bva=0x%08X page_match=%d exact_bva=%d epc_match=%d"
        " ra_match=%d thread=0x%08X proc=%s0x%08X process=\"%s\"\n",
        phase ? phase : "?",
        exccode,
        fault_vaddr,
        epc,
        rec->code,
        rec->pc,
        rec->ra_valid ? "" : "?",
        rec->ra,
        rec->bva,
        same_page ? 1 : 0,
        exact_bva ? 1 : 0,
        epc_match ? 1 : 0,
        ra_match ? 1 : 0,
        rec->thread_valid ? rec->thread : 0u,
        rec->proc_valid ? "" : "?",
        rec->proc,
        rec->process_name[0] != '\0' ? rec->process_name : "?");
    m->wince.serial_exception_corr_count++;

    if (m->wince.systempatch_seen && !m->wince.systempatch_first_exception_logged) {
        maybe_log_systempatch_context(m, "first_correlated_exception");
        m->wince.systempatch_first_exception_logged = true;
    }
}

static void maybe_log_hot_page_verdict(machine_t *m, struct cpu *cpu,
    uint32_t probe_va, uint32_t fault_vaddr, uint32_t exccode,
    const char *tag)
{
    struct mips_coproc *cp0;
    wince_hot_page_verdict_t *verdict;
    wince_hot_page_verdict_t candidate;
    uint32_t section_idx;
    uint32_t section_val;
    uint32_t l2_off;
    uint32_t l2_val;
    uint32_t pte_off;
    uint32_t lo0 = 0;
    uint32_t lo1 = 0;
    uint32_t selected_lo;
    bool odd_page;
    unsigned prev_severity;
    unsigned new_severity;

    if (!m || !cpu)
        return;

    if (probe_va == UINT32_C(0x01F8F8F8)) {
        verdict = &m->wince.hot_page_01f8f8f8;
    } else if (probe_va == UINT32_C(0x01F94B50)) {
        verdict = &m->wince.hot_page_01f94b50;
    } else if (probe_va == UINT32_C(0x02041FA8)) {
        verdict = &m->wince.hot_page_02041fa8;
    } else {
        return;
    }

    cp0 = cpu->cd.mips.coproc[0];
    if (!cp0)
        return;

    section_idx = (probe_va >> 25) & 0x3Fu;
    section_val = load_pa_word(m, 0x18C0u + section_idx * 4u);
    l2_off = (probe_va >> 14) & 0x7FCu;
    l2_val = section_val != 0 ? load_table_word(m, section_val, l2_off) : 0;
    pte_off = (probe_va >> 10) & 0x38u;
    if ((int32_t)l2_val < 0) {
        lo0 = load_table_word(m, l2_val, pte_off + 12u);
        lo1 = load_table_word(m, l2_val, pte_off + 16u);
    }

    odd_page = ((probe_va >> 12) & 1u) != 0;
    selected_lo = odd_page ? lo1 : lo0;

    memset(&candidate, 0, sizeof(candidate));
    candidate.seen = true;
    candidate.logged = true;
    candidate.probe_va = probe_va;
    candidate.fault_va = fault_vaddr;
    candidate.section_idx = section_idx;
    candidate.section_val = section_val;
    candidate.l2_off = l2_off;
    candidate.l2_val = l2_val;
    candidate.pte_off = pte_off;
    candidate.lo0 = lo0;
    candidate.lo1 = lo1;
    candidate.selected_lo = selected_lo;
    candidate.entryhi = (uint32_t)cp0->reg[COP0_ENTRYHI];
    candidate.asid = candidate.entryhi & 0xFFu;
    candidate.odd_page = odd_page;
    candidate.selected_valid = (selected_lo & ENTRYLO_V) != 0;

    prev_severity = hot_page_verdict_severity(verdict);
    new_severity = hot_page_verdict_severity(&candidate);
    if (verdict->seen && new_severity < prev_severity)
        return;
    if (verdict->seen && new_severity == prev_severity
        && candidate.fault_va != candidate.probe_va
        && verdict->fault_va == verdict->probe_va) {
        return;
    }

    *verdict = candidate;

    fprintf(stderr,
        "[WINCE_PAGE_VERDICT] tag=%s exc=%u probe=0x%08X fault=0x%08X"
        " sec[%u]=0x%08X l2_off=0x%03X l2=0x%08X pte_off=0x%02X"
        " lo0=0x%08X lo1=0x%08X selected=%s:0x%08X valid=%d"
        " entryhi=0x%08X asid=%u\n",
        tag ? tag : "?",
        exccode,
        probe_va,
        fault_vaddr,
        section_idx,
        section_val,
        l2_off,
        l2_val,
        pte_off,
        lo0,
        lo1,
        odd_page ? "lo1" : "lo0",
        selected_lo,
        candidate.selected_valid ? 1 : 0,
        candidate.entryhi,
        candidate.asid);
}

static void maybe_note_section0_source_pc(machine_t *m, struct cpu *cpu,
    uint32_t pc32)
{
    uint32_t obj;
    uint32_t obj0c = 0;
    uint32_t obj14 = 0;
    uint32_t obj24 = 0;
    uint32_t obj3c = 0;
    uint32_t ctx08 = 0;
    uint32_t ctx0c = 0;
    uint32_t src_off = 0;
    uint32_t src_idx = 0;
    uint32_t src_slot_va = 0;
    uint32_t src_slot_val = 0;
    bool obj0c_ok = false;
    bool obj14_ok = false;
    bool obj24_ok = false;
    bool obj3c_ok = false;
    bool ctx08_ok = false;
    bool ctx0c_ok = false;
    bool src_slot_ok = false;
    char obj0c_buf[16];
    char obj14_buf[16];
    char obj24_buf[16];
    char obj3c_buf[16];
    char ctx08_buf[16];
    char ctx0c_buf[16];
    char src_slot_buf[16];

    if (!m || !cpu)
        return;
    if (m->wince.section0_source_probe_count >= 12)
        return;

    obj = pc32 == UINT32_C(0x8008B594)
        ? (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0]
        : (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];

    obj0c_ok = load_va_word(m, obj + 0x0Cu, &obj0c);
    obj14_ok = load_va_word(m, obj + 0x14u, &obj14);
    obj24_ok = load_va_word(m, obj + 0x24u, &obj24);
    obj3c_ok = load_va_word(m, obj + 0x3Cu, &obj3c);
    if (obj0c_ok && obj0c >= UINT32_C(0x80000000) && obj0c < UINT32_C(0x81000000)) {
        ctx08_ok = load_va_word(m, obj0c + 0x08u, &ctx08);
        ctx0c_ok = load_va_word(m, obj0c + 0x0Cu, &ctx0c);
        if (ctx0c_ok) {
            src_off = ctx0c >> 23;
            src_idx = src_off >> 2;
            src_slot_va = UINT32_C(0xFFFFD8C0) + src_off;
            src_slot_ok = load_va_word(m, src_slot_va, &src_slot_val);
        }
    }

    fprintf(stderr,
        "[WINCE_SEC0_SRC] label=%s pc=0x%08X ra=0x%08X sp=0x%08X"
        " obj=0x%08X obj+0c=%s obj+14=%s obj+24=%s obj+3c=%s"
        " ctx+08=%s ctx+0c=%s src_off=0x%03X src_idx=%u"
        " src_slot=%s reg_t2=0x%08X reg_t0=0x%08X sec0_before=0x%08X\n",
        pc32 == UINT32_C(0x8008B594) ? "scheduler_switch" : "thread_attach",
        pc32,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
        obj,
        format_word_or_unknown(obj0c_buf, sizeof(obj0c_buf), obj0c_ok, obj0c),
        format_word_or_unknown(obj14_buf, sizeof(obj14_buf), obj14_ok, obj14),
        format_word_or_unknown(obj24_buf, sizeof(obj24_buf), obj24_ok, obj24),
        format_word_or_unknown(obj3c_buf, sizeof(obj3c_buf), obj3c_ok, obj3c),
        format_word_or_unknown(ctx08_buf, sizeof(ctx08_buf), ctx08_ok, ctx08),
        format_word_or_unknown(ctx0c_buf, sizeof(ctx0c_buf), ctx0c_ok, ctx0c),
        src_off,
        src_idx,
        format_word_or_unknown(src_slot_buf, sizeof(src_slot_buf), src_slot_ok,
            src_slot_val),
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T2],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T0],
        load_pa_word(m, 0x18C0u));
    dump_code_window(m, pc32, 6u, 8u);
    if (src_slot_ok && src_slot_val == UINT32_C(0x80FE5000)) {
        log_l2_table_state(m, "sec0_src_slot", src_slot_val,
            UINT32_C(0x01F94B50));
        log_section0_focus_window(m, "sec0_src_slot", src_slot_val);
    }

    m->wince.section0_source_probe_count++;
}

static void maybe_note_exception_hot_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32)
{
    uint32_t pc32;
    uint32_t a0;
    uint32_t a3;
    uint32_t a0_d0 = 0;
    uint32_t a0_e8 = 0;
    uint32_t a0_ec = 0;
    uint32_t a0_plus_0 = 0;
    uint32_t a0_plus_4 = 0;
    uint32_t a0_plus_8 = 0;
    uint32_t a0_plus_c = 0;
    uint32_t a0_plus_10 = 0;
    uint32_t sp_48 = 0;
    uint32_t sp_50 = 0;
    uint32_t sp_54 = 0;
    uint32_t sp_60 = 0;
    uint32_t sp_34 = 0;
    uint32_t sp_38 = 0;
    uint32_t s1;
    uint32_t s4;
    uint32_t v104 = 0;
    uint32_t v11c = 0;
    uint32_t v120 = 0;
    bool a0_d0_ok = false;
    bool a0_e8_ok = false;
    bool a0_ec_ok = false;
    bool a0_plus_0_ok = false;
    bool a0_plus_4_ok = false;
    bool a0_plus_8_ok = false;
    bool a0_plus_c_ok = false;
    bool a0_plus_10_ok = false;
    bool sp_48_ok = false;
    bool sp_50_ok = false;
    bool sp_54_ok = false;
    bool sp_60_ok = false;
    bool sp_34_ok = false;
    bool sp_38_ok = false;
    bool v104_ok = false;
    bool v11c_ok = false;
    bool v120_ok = false;
    uint32_t dac0 = 0;
    uint32_t dac0_18 = 0;
    uint32_t dac0_1c = 0;
    uint32_t dac0_38 = 0;
    bool dac0_ok = false;
    bool dac0_18_ok = false;
    bool dac0_1c_ok = false;
    bool dac0_38_ok = false;
    char a0_d0_buf[16];
    char a0_e8_buf[16];
    char a0_ec_buf[16];
    char a0_plus_0_buf[16];
    char a0_plus_4_buf[16];
    char a0_plus_8_buf[16];
    char a0_plus_c_buf[16];
    char a0_plus_10_buf[16];
    char sp_48_buf[16];
    char sp_50_buf[16];
    char sp_54_buf[16];
    char sp_60_buf[16];
    char sp_34_buf[16];
    char sp_38_buf[16];
    char v104_buf[16];
    char v11c_buf[16];
    char v120_buf[16];
    char dac0_buf[16];
    char dac0_18_buf[16];
    char dac0_1c_buf[16];
    char dac0_38_buf[16];

    if (!m || !cpu)
        return;

    pc32 = canonicalize_nk_pc(raw_pc32);
    if (pc32 != 0x80094E4Cu && pc32 != 0x80094E8Cu
        && m->wince.hot_fault_probe_count >= WINCE_HOT_FAULT_PROBE_MAX)
        return;
    a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
    a3 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3];
    s1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1];
    s4 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S4];

    switch (pc32) {
    case 0x80094E4Cu:
        if (m->wince.section3_head_probe_count >= 4u)
            return;
        a0_d0_ok = load_va_word(m, a0 + 0xD0u, &a0_d0);
        a0_e8_ok = load_va_word(m, a0 + 0xE8u, &a0_e8);
        a0_ec_ok = load_va_word(m, a0 + 0xECu, &a0_ec);
        fprintf(stderr,
            "[WINCE_HOT_PC] label=systempatch_desc_prepare pc=0x%08X"
            " ra=0x%08X sp=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X"
            " a0+d0=%s a0+e8=%s a0+ec=%s\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            a0,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
            format_word_or_unknown(a0_d0_buf, sizeof(a0_d0_buf), a0_d0_ok, a0_d0),
            format_word_or_unknown(a0_e8_buf, sizeof(a0_e8_buf), a0_e8_ok, a0_e8),
            format_word_or_unknown(a0_ec_buf, sizeof(a0_ec_buf), a0_ec_ok, a0_ec));
        dump_section3_context_head(m, "desc_prepare", pc32);
        dump_pointer_bytes(m, "desc_prepare_a0", a0);
        dump_code_window(m, pc32, 4u, 10u);
        m->wince.section3_head_probe_count++;
        return;

    case 0x80094E8Cu:
        if (m->wince.section3_head_probe_count >= 4u)
            return;
        sp_34_ok = load_va_word(m,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP] + 0x34u, &sp_34);
        sp_38_ok = load_va_word(m,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP] + 0x38u, &sp_38);
        fprintf(stderr,
            "[WINCE_HOT_PC] label=systempatch_desc_post_alloc pc=0x%08X"
            " ra=0x%08X sp=0x%08X v0=0x%08X stack34=%s stack38=%s"
            " s0=0x%08X s1=0x%08X\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
            format_word_or_unknown(sp_34_buf, sizeof(sp_34_buf), sp_34_ok, sp_34),
            format_word_or_unknown(sp_38_buf, sizeof(sp_38_buf), sp_38_ok, sp_38),
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1]);
        dump_section3_context_head(m, "desc_post_alloc", pc32);
        dump_section3_descriptor_window(m, "desc_post_alloc");
        dump_code_window(m, pc32, 4u, 10u);
        m->wince.section3_head_probe_count++;
        return;

    case 0x800A5794u:
    case 0x800A57C0u:
        fprintf(stderr,
            "[WINCE_HOT_PC] label=%s pc=0x%08X ra=0x%08X sp=0x%08X"
            " a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X"
            " s0=0x%08X s1=0x%08X s2=0x%08X s3=0x%08X s4=0x%08X"
            " s4_space=%s\n",
            pc32 == 0x800A5794u ? "systempatch_fault_entry"
                : "systempatch_fault_lw_s4",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            a0,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
            a3,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0],
            s1,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S2],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S3],
            s4,
            classify_va_space(s4));
        dump_pointer_bytes(m, "fault_s4", s4);
        dump_pointer_bytes(m, "fault_s1", s1);
        dump_pointer_bytes(m, "fault_a3", a3);
        dump_code_window(m, pc32, 4u, 8u);
        if ((uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA] >= 8u)
            dump_code_window(m,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA] - 8u, 4u, 8u);
        maybe_log_systempatch_context(m, "hot_pc_a57c0");
        m->wince.hot_fault_probe_count++;
        return;

    case 0x800A4078u:
        fprintf(stderr,
            "[WINCE_HOT_PC] label=systempatch_fault_return pc=0x%08X"
            " sp=0x%08X v0=0x%08X v1=0x%08X a0=0x%08X a1=0x%08X"
            " a2=0x%08X a3=0x%08X\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V1],
            a0,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
            a3);
        dump_code_window(m, pc32, 4u, 8u);
        m->wince.hot_fault_probe_count++;
        return;

    case 0x800A43E4u:
        sp_48_ok = load_va_word(m,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP] + 0x48u, &sp_48);
        sp_50_ok = load_va_word(m,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP] + 0x50u, &sp_50);
        sp_54_ok = load_va_word(m,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP] + 0x54u, &sp_54);
        sp_60_ok = load_va_word(m,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP] + 0x60u, &sp_60);
        fprintf(stderr,
            "[WINCE_HOT_PC] label=systempatch_lookup_begin pc=0x%08X"
            " ra=0x%08X sp=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X"
            " s0=0x%08X s1=0x%08X stack48=%s stack50=%s stack54=%s"
            " stack60=%s\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            a0,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0],
            s1,
            format_word_or_unknown(sp_48_buf, sizeof(sp_48_buf), sp_48_ok, sp_48),
            format_word_or_unknown(sp_50_buf, sizeof(sp_50_buf), sp_50_ok, sp_50),
            format_word_or_unknown(sp_54_buf, sizeof(sp_54_buf), sp_54_ok, sp_54),
            format_word_or_unknown(sp_60_buf, sizeof(sp_60_buf), sp_60_ok, sp_60));
        dump_pointer_bytes(m, "lookup_base_a0", a0);
        dump_code_window(m, pc32, 6u, 8u);
        m->wince.hot_fault_probe_count++;
        return;

    case 0x800A4428u:
        a0_plus_0_ok = load_va_word(m, a0 + 0x0u, &a0_plus_0);
        a0_plus_4_ok = load_va_word(m, a0 + 0x4u, &a0_plus_4);
        a0_plus_8_ok = load_va_word(m, a0 + 0x8u, &a0_plus_8);
        a0_plus_c_ok = load_va_word(m, a0 + 0xCu, &a0_plus_c);
        a0_plus_10_ok = load_va_word(m, a0 + 0x10u, &a0_plus_10);
        fprintf(stderr,
            "[WINCE_HOT_PC] label=systempatch_lookup_probe pc=0x%08X"
            " ra=0x%08X sp=0x%08X v0=0x%08X v1=0x%08X a0=0x%08X"
            " s0=0x%08X rec0=%s rec4=%s rec8=%s recC=%s rec10=%s\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V1],
            a0,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0],
            format_word_or_unknown(a0_plus_0_buf, sizeof(a0_plus_0_buf),
                a0_plus_0_ok, a0_plus_0),
            format_word_or_unknown(a0_plus_4_buf, sizeof(a0_plus_4_buf),
                a0_plus_4_ok, a0_plus_4),
            format_word_or_unknown(a0_plus_8_buf, sizeof(a0_plus_8_buf),
                a0_plus_8_ok, a0_plus_8),
            format_word_or_unknown(a0_plus_c_buf, sizeof(a0_plus_c_buf),
                a0_plus_c_ok, a0_plus_c),
            format_word_or_unknown(a0_plus_10_buf, sizeof(a0_plus_10_buf),
                a0_plus_10_ok, a0_plus_10));
        dump_pointer_bytes(m, "lookup_candidate_a0", a0);
        dump_pointer_bytes(m, "lookup_table_v0",
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0]);
        dump_code_window(m, pc32, 4u, 8u);
        m->wince.hot_fault_probe_count++;
        return;

    case 0x80094FD4u:
        v104_ok = load_va_word(m, a0 + 0x104u, &v104);
        v11c_ok = load_va_word(m, a0 + 0x11Cu, &v11c);
        v120_ok = load_va_word(m, a0 + 0x120u, &v120);
        dac0_ok = load_va_word(m, UINT32_C(0xFFFFDAC0), &dac0);
        if (dac0_ok && dac0 >= UINT32_C(0x80000000) && dac0 < UINT32_C(0x81000000)) {
            dac0_18_ok = load_va_word(m, dac0 + 0x18u, &dac0_18);
            dac0_1c_ok = load_va_word(m, dac0 + 0x1Cu, &dac0_1c);
            dac0_38_ok = load_va_word(m, dac0 + 0x38u, &dac0_38);
        }
        fprintf(stderr,
            "[WINCE_HOT_PC] label=systempatch_setup pc=0x%08X ra=0x%08X"
            " sp=0x%08X a0=0x%08X a0+104=%s a0+11c=%s a0+120=%s"
            " dac0=%s dac0+18=%s dac0+1c=%s dac0+38=%s\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            a0,
            format_word_or_unknown(v104_buf, sizeof(v104_buf), v104_ok, v104),
            format_word_or_unknown(v11c_buf, sizeof(v11c_buf), v11c_ok, v11c),
            format_word_or_unknown(v120_buf, sizeof(v120_buf), v120_ok, v120),
            format_word_or_unknown(dac0_buf, sizeof(dac0_buf), dac0_ok, dac0),
            format_word_or_unknown(dac0_18_buf, sizeof(dac0_18_buf), dac0_18_ok, dac0_18),
            format_word_or_unknown(dac0_1c_buf, sizeof(dac0_1c_buf), dac0_1c_ok, dac0_1c),
            format_word_or_unknown(dac0_38_buf, sizeof(dac0_38_buf), dac0_38_ok, dac0_38));
        dump_pointer_bytes(m, "setup_a0", a0);
        dump_section3_context_head(m, "systempatch_setup", pc32);
        dump_code_window(m, pc32, 4u, 8u);
        maybe_log_systempatch_context(m, "hot_pc_94fd4");
        m->wince.hot_fault_probe_count++;
        m->wince.section3_head_probe_count++;
        return;

    default:
        return;
    }
}

static void maybe_note_hot_l2_alloc_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32)
{
    uint32_t pc32;
    uint32_t sp;
    uint32_t count = 0;
    uint32_t out_ptr = 0;
    uint32_t processed = 0;
    uint32_t arg16 = 0;
    uint32_t arg20 = 0;
    bool count_ok = false;
    bool processed_ok = false;
    bool arg16_ok = false;
    bool arg20_ok = false;
    char arg16_buf[16];
    char arg20_buf[16];

    if (!m || !cpu)
        return;

    pc32 = canonicalize_nk_pc(raw_pc32);
    switch (pc32) {
    case 0x80098054u:
    case 0x8009805Cu:
    case 0x80098090u:
    case 0x800982A0u:
    case 0x800982A8u:
    case 0x8009837Cu:
    case 0x80098384u:
        break;
    default:
        return;
    }

    if (m->wince.hot_l2_alloc_probe_count >= 32u)
        return;

    sp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
    switch (pc32) {
    case 0x80098054u:
        count_ok = load_va_word(m, sp + 0x10u, &count);
        (void)load_va_word(m, sp + 0x14u, &out_ptr);
        log_alloc_scan_state(m, cpu, "release_scan_call",
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
            count_ok ? count : 0u,
            false, 0u, false, 0u);
        fprintf(stderr,
            "[WINCE_L2_ALLOC] label=release_scan_meta out_ptr=0x%08X\n",
            out_ptr);
        m->wince.hot_l2_alloc_probe_count++;
        return;

    case 0x8009805Cu:
        count_ok = load_va_word(m, sp + 0x10u, &count);
        processed_ok = load_va_word(m, sp + 0x30u, &processed);
        log_alloc_scan_state(m, cpu, "release_scan_ret",
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
            count_ok ? count : 0u,
            processed_ok, processed, true,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0]);
        m->wince.hot_l2_alloc_probe_count++;
        return;

    case 0x80098090u:
        arg16_ok = load_va_word(m, sp + 0x10u, &arg16);
        arg20_ok = load_va_word(m, sp + 0x14u, &arg20);
        fprintf(stderr,
            "[WINCE_L2_CLEANUP] label=release_cleanup_call pc=0x%08X"
            " ra=0x%08X sp=0x%08X base=0x%08X type=0x%08X"
            " start_idx=%u count=%u arg16=%s arg20=%s\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            sp,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
            format_word_or_unknown(arg16_buf, sizeof(arg16_buf), arg16_ok, arg16),
            format_word_or_unknown(arg20_buf, sizeof(arg20_buf), arg20_ok, arg20));
        log_alloc_scan_state(m, cpu, "release_cleanup_state",
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
            0u,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
            false, 0u, false, 0u);
        m->wince.hot_l2_alloc_probe_count++;
        return;

    case 0x800982A0u:
        count_ok = load_va_word(m, sp + 0x10u, &count);
        log_alloc_scan_state(m, cpu, "publish_scan_call",
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
            count_ok ? count : 0u,
            false, 0u, false, 0u);
        m->wince.hot_l2_alloc_probe_count++;
        return;

    case 0x800982A8u:
        count_ok = load_va_word(m, sp + 0x10u, &count);
        log_alloc_scan_state(m, cpu, "publish_scan_ret",
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
            count_ok ? count : 0u,
            false, 0u, true, (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0]);
        m->wince.hot_l2_alloc_probe_count++;
        return;

    case 0x8009837Cu:
        count_ok = load_va_word(m, sp + 0x10u, &count);
        (void)load_va_word(m, sp + 0x14u, &out_ptr);
        log_alloc_scan_state(m, cpu, "validate_scan_call",
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
            count_ok ? count : 0u,
            false, 0u, false, 0u);
        fprintf(stderr,
            "[WINCE_L2_ALLOC] label=validate_scan_meta out_ptr=0x%08X\n",
            out_ptr);
        m->wince.hot_l2_alloc_probe_count++;
        return;

    case 0x80098384u:
        count_ok = load_va_word(m, sp + 0x10u, &count);
        processed_ok = load_va_word(m, sp + 0x64u, &processed);
        log_alloc_scan_state(m, cpu, "validate_scan_ret",
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
            count_ok ? count : 0u,
            processed_ok, processed, true,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0]);
        m->wince.hot_l2_alloc_probe_count++;
        return;

    default:
        return;
    }
}

static void maybe_note_callback_slot_pc(machine_t *m, struct cpu *cpu,
    uint32_t pc32)
{
    uint32_t base_pa = m->wince.callback_slot_watch_pa;
    uint32_t s2 = 0;
    uint32_t s2_slot = 0;
    uint32_t t7 = 0;
    uint32_t t8 = 0;
    uint32_t flag = 0;
    uint32_t ptr = 0;
    uint32_t aux = 0;
    uint32_t arg = 0;
    uint32_t sp;
    uint32_t stk0 = 0;
    uint32_t stk1 = 0;
    uint32_t stk2 = 0;
    uint32_t stk3 = 0;
    uint32_t ret_offs[4] = {0};
    uint32_t ret_addrs[4] = {0};
    size_t ret_count = 0;
    size_t i;

    if (!m || !cpu)
        return;
    if (m->wince.callback_slot_diag_count >= 40u)
        return;

    if (m->wince.callback_slot_watch_armed) {
        flag = load_pa_word(m, base_pa + 0x00u);
        ptr = load_pa_word(m, base_pa + 0x04u);
        aux = load_pa_word(m, base_pa + 0x08u);
        arg = load_pa_word(m, base_pa + 0x0Cu);
    }

    sp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
    s2 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S2];
    t7 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T7];
    t8 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T8];
    (void)load_va_word(m, s2 + 0x08u, &s2_slot);
    (void)load_va_word(m, sp + 0x00u, &stk0);
    (void)load_va_word(m, sp + 0x04u, &stk1);
    (void)load_va_word(m, sp + 0x08u, &stk2);
    (void)load_va_word(m, sp + 0x0Cu, &stk3);
    ret_count = collect_stack_return_sites(m, sp, ret_offs, ret_addrs,
        sizeof(ret_addrs) / sizeof(ret_addrs[0]), 0x40u);

    switch (pc32) {
    case 0x01F84A5Cu:
    case 0x800BFA5Cu:
        fprintf(stderr,
            "[WINCE_CB_PC] label=callback_wrapper_entry pc=0x%08X"
            " ra=0x%08X sp=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X"
            " t6=0x%08X armed=%u slot_pa=0x%08X flag=0x%08X ptr=0x%08X"
            " aux=0x%08X arg=0x%08X stack=%08X/%08X/%08X/%08X\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            sp,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T6],
            m->wince.callback_slot_watch_armed ? 1u : 0u,
            base_pa,
            flag,
            ptr,
            aux,
            arg,
            stk0, stk1, stk2, stk3);
        if (ret_count > 0) {
            fprintf(stderr,
                "[WINCE_CB_RET] label=callback_wrapper_entry");
            for (i = 0; i < ret_count; i++) {
                fprintf(stderr, " ret%zu=%#010x@+0x%02X callsite=0x%08X",
                    i, ret_addrs[i], ret_offs[i],
                    ret_addrs[i] >= 8u ? ret_addrs[i] - 8u : 0u);
            }
            fputc('\n', stderr);
        }
        dump_code_window(m, pc32, 4u, 12u);
        maybe_log_callback_object_state(m, cpu, "wrapper_entry",
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0]);
        m->wince.callback_slot_diag_count++;
        return;

    case 0x01F84A7Cu:
    case 0x800BFA7Cu:
        fprintf(stderr,
            "[WINCE_CB_PC] label=callback_init_call pc=0x%08X"
            " ra=0x%08X sp=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X"
            " t6=0x%08X armed=%u slot_pa=0x%08X flag=0x%08X ptr=0x%08X"
            " aux=0x%08X arg=0x%08X stack=%08X/%08X/%08X/%08X\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            sp,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T6],
            m->wince.callback_slot_watch_armed ? 1u : 0u,
            base_pa,
            flag,
            ptr,
            aux,
            arg,
            stk0, stk1, stk2, stk3);
        if (ret_count > 0) {
            fprintf(stderr,
                "[WINCE_CB_RET] label=callback_init_call");
            for (i = 0; i < ret_count; i++) {
                fprintf(stderr, " ret%zu=%#010x@+0x%02X callsite=0x%08X",
                    i, ret_addrs[i], ret_offs[i],
                    ret_addrs[i] >= 8u ? ret_addrs[i] - 8u : 0u);
            }
            fputc('\n', stderr);
        }
        dump_code_window(m, pc32, 6u, 12u);
        maybe_log_callback_object_state(m, cpu, "wrapper_init_call",
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0]);
        m->wince.callback_slot_diag_count++;
        return;

    case 0x01F84AA8u:
    case 0x800BFAA8u:
        fprintf(stderr,
            "[WINCE_CB_PC] label=callback_publish_call pc=0x%08X"
            " ra=0x%08X sp=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X"
            " slot_pa=0x%08X flag=0x%08X ptr=0x%08X aux=0x%08X arg=0x%08X\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            sp,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
            base_pa,
            flag,
            ptr,
            aux,
            arg);
        dump_code_window(m, pc32, 6u, 12u);
        dump_pointer_bytes(m, "callback_slot_base", UINT32_C(0x01FE6544));
        maybe_log_callback_object_state(m, cpu, "publish_call", arg);
        m->wince.callback_slot_diag_count++;
        return;

    case 0x8008FF00u:
        fprintf(stderr,
            "[WINCE_CB_PC] label=callback_dispatch_entry pc=0x%08X"
            " ra=0x%08X sp=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X"
            " slot_pa=0x%08X flag=0x%08X ptr=0x%08X aux=0x%08X arg=0x%08X\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            sp,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
            base_pa,
            flag,
            ptr,
            aux,
            arg);
        maybe_log_callback_object_state(m, cpu, "dispatch_entry",
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0]);
        m->wince.callback_slot_diag_count++;
        return;

    case 0x80090024u:
        fprintf(stderr,
            "[WINCE_CB_PC] label=callback_dispatch_jalr pc=0x%08X"
            " ra=0x%08X a0=0x%08X t7=0x%08X slot_pa=0x%08X"
            " flag=0x%08X ptr=0x%08X aux=0x%08X arg=0x%08X\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
            t7,
            base_pa,
            flag,
            ptr,
            aux,
            arg);
        maybe_log_callback_object_state(m, cpu, "dispatch_jalr",
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0]);
        m->wince.callback_slot_diag_count++;
        return;

    case 0x80090044u:
        fprintf(stderr,
            "[WINCE_CB_PC] label=callback_dispatch_ret pc=0x%08X"
            " ra=0x%08X v0=0x%08X a0=0x%08X\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0]);
        maybe_log_callback_object_state(m, cpu, "dispatch_ret",
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0]);
        m->wince.callback_slot_diag_count++;
        return;

    case 0x80092488u:
    {
        uint32_t s2_08 = 0, s2_0c = 0, s2_10 = 0;
        (void)load_va_word(m, s2 + 0x08u, &s2_08);
        (void)load_va_word(m, s2 + 0x0Cu, &s2_0c);
        (void)load_va_word(m, s2 + 0x10u, &s2_10);
        fprintf(stderr,
            "[WINCE_CB_PC] label=callback_table_store pc=0x%08X"
            " ra=0x%08X sp=0x%08X s2=0x%08X s2+8=0x%08X s2+C=0x%08X"
            " s2+10=0x%08X v0=0x%08X v1=0x%08X t7=0x%08X t8=0x%08X\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            sp,
            s2,
            s2_08,
            s2_0c,
            s2_10,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V1],
            t7,
            t8);
        dump_code_window(m, pc32, 6u, 12u);
        dump_pointer_bytes(m, "callback_table_store_slot",
            UINT32_C(0x01FE6544));
        maybe_log_callback_object_state(m, cpu, "table_store", s2);
        m->wince.callback_slot_diag_count++;
        return;
    }

    case 0x8009248Cu:
    {
        uint32_t s2_08 = 0;
        (void)load_va_word(m, s2 + 0x08u, &s2_08);
        fprintf(stderr,
            "[WINCE_CB_PC] label=callback_table_store_post pc=0x%08X"
            " ra=0x%08X sp=0x%08X s2=0x%08X s2+8=0x%08X"
            " v0=0x%08X v1=0x%08X\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            sp,
            s2,
            s2_08,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V1]);
        dump_pointer_bytes(m, "callback_table_store_post_slot",
            UINT32_C(0x01FE6544));
        m->wince.callback_slot_diag_count++;
        return;
    }

    case 0x80097000u:
    {
        uint32_t a0v = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
        bool is_hot_l2 = (a0v == UINT32_C(0x80FFC1C8));
        bool in_l2_family =
            (a0v >= UINT32_C(0x80FF0000) && a0v < UINT32_C(0x81000000));
        bool should_log = is_hot_l2 ||
            (in_l2_family && m->wince.teardown_trace_count < 8u);

        if (!should_log)
            return;
        if (is_hot_l2) {
            if (m->wince.teardown_hot_logged)
                return;
            m->wince.teardown_hot_logged = true;
        } else {
            m->wince.teardown_trace_count++;
        }

        fprintf(stderr,
            "[WINCE_CB_PC] label=%s pc=0x%08X ra=0x%08X sp=0x%08X"
            " a0=0x%08X a1=0x%08X s0=0x%08X s1=0x%08X"
            " s2=0x%08X s3=0x%08X\n",
            is_hot_l2 ? "l2_teardown_hot" : "l2_teardown_entry",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            sp,
            a0v,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1],
            s2,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S3]);
        {
            unsigned w;
            for (w = 0; w < 16u; w++) {
                uint32_t pw = 0;
                uint32_t va = a0v + (uint32_t)(w * 4u);
                bool ok = load_va_word(m, va, &pw);
                fprintf(stderr,
                    "[WINCE_L2_TEARDOWN] tag=%s va=0x%08X w=0x%08X%s\n",
                    is_hot_l2 ? "hot_page" : "pre_zero",
                    va, pw, ok ? "" : " (unmapped)");
            }
        }
        if (ret_count > 0) {
            fprintf(stderr,
                "[WINCE_CB_RET] label=%s",
                is_hot_l2 ? "l2_teardown_hot" : "l2_teardown_entry");
            for (i = 0; i < ret_count; i++) {
                fprintf(stderr,
                    " ret%zu=%#010x@+0x%02X callsite=0x%08X",
                    i, ret_addrs[i], ret_offs[i],
                    ret_addrs[i] >= 8u ? ret_addrs[i] - 8u : 0u);
            }
            fputc('\n', stderr);
        }
        m->wince.callback_slot_diag_count++;
        return;
    }

    case 0x800971C0u:
    {
        uint32_t s1_v = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1];
        uint32_t s3_v = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S3];
        bool slot_aware =
            (s2 >= UINT32_C(0x01FE0000) && s2 < UINT32_C(0x04000000)) ||
            (s3_v >= UINT32_C(0x01FE0000) && s3_v < UINT32_C(0x04000000));
        if (s1_v == 0u || !slot_aware
            || m->wince.teardown_caller_count >= 8u)
            return;
        m->wince.teardown_caller_count++;
        fprintf(stderr,
            "[WINCE_CB_PC] label=l2_teardown_caller_loop pc=0x%08X"
            " ra=0x%08X sp=0x%08X s0=0x%08X s1=0x%08X s2=0x%08X"
            " s3=0x%08X s4=0x%08X s5=0x%08X\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            sp,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0],
            s1_v,
            s2,
            s3_v,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S4],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S5]);
        dump_code_window(m, pc32, 10u, 4u);
        if (ret_count > 0) {
            fprintf(stderr,
                "[WINCE_CB_RET] label=l2_teardown_caller_loop");
            for (i = 0; i < ret_count; i++) {
                fprintf(stderr,
                    " ret%zu=%#010x@+0x%02X callsite=0x%08X",
                    i, ret_addrs[i], ret_offs[i],
                    ret_addrs[i] >= 8u ? ret_addrs[i] - 8u : 0u);
            }
            fputc('\n', stderr);
        }
        m->wince.callback_slot_diag_count++;
        return;
    }

    case 0x80098144u:
        if (!m->wince.teardown_tail_dumped) {
            m->wince.teardown_tail_dumped = true;
            fprintf(stderr,
                "[WINCE_CB_PC] label=l2_teardown_tail pc=0x%08X"
                " ra=0x%08X sp=0x%08X v0=0x%08X"
                " s0=0x%08X s1=0x%08X s2=0x%08X s3=0x%08X"
                " s4=0x%08X s5=0x%08X s6=0x%08X s7=0x%08X\n",
                pc32,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
                sp,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1],
                s2,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S3],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S4],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S5],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S6],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S7]);
            dump_code_window(m, pc32, 16u, 8u);
            if (ret_count > 0) {
                fprintf(stderr,
                    "[WINCE_CB_RET] label=l2_teardown_tail");
                for (i = 0; i < ret_count; i++) {
                    fprintf(stderr,
                        " ret%zu=%#010x@+0x%02X callsite=0x%08X",
                        i, ret_addrs[i], ret_offs[i],
                        ret_addrs[i] >= 8u ? ret_addrs[i] - 8u : 0u);
                }
                fputc('\n', stderr);
            }
            m->wince.callback_slot_diag_count++;
        }
        return;

    case 0x800A3244u:
    {
        uint32_t a0v = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
        uint32_t pfn_pa = (a0v & UINT32_C(0x3FFFFFC0)) << 6;

        if (!m->wince.free_helper_entry_dumped) {
            m->wince.free_helper_entry_dumped = true;
            fprintf(stderr,
                "[WINCE_FREE_HELPER] entry pc=0x%08X ra=0x%08X"
                " sp=0x%08X a0=0x%08X masked_pfn=0x%08X"
                " a1=0x%08X a2=0x%08X a3=0x%08X"
                " s0=0x%08X s1=0x%08X s2=0x%08X\n",
                pc32,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
                sp, a0v, pfn_pa,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1],
                s2);
            dump_code_window(m, pc32, 2u, 24u);
            if (ret_count > 0) {
                fprintf(stderr,
                    "[WINCE_CB_RET] label=free_helper_entry");
                for (i = 0; i < ret_count; i++) {
                    fprintf(stderr,
                        " ret%zu=%#010x@+0x%02X callsite=0x%08X",
                        i, ret_addrs[i], ret_offs[i],
                        ret_addrs[i] >= 8u ? ret_addrs[i] - 8u : 0u);
                }
                fputc('\n', stderr);
            }
        }
        if (m->wince.free_helper_call_count < 16u) {
            m->wince.free_helper_call_count++;
            fprintf(stderr,
                "[WINCE_FREE_CALL] #%u a0=0x%08X masked_pfn=0x%08X"
                " ra=0x%08X\n",
                (unsigned)m->wince.free_helper_call_count,
                a0v, pfn_pa,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
        }
        m->wince.callback_slot_diag_count++;
        return;
    }

    case 0x80096E88u:
        if (m->wince.verify_helper_entry_count < 8u) {
            uint32_t rav = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA];
            uint32_t stk4 = 0;
            m->wince.verify_helper_entry_count++;
            (void)load_va_word(m, sp + 0x50u, &stk4);
            fprintf(stderr,
                "[WINCE_VERIFY_ENTRY] #%u pc=0x%08X ra=0x%08X"
                " sp=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X"
                " a3=0x%08X stk_arg4=0x%08X\n",
                (unsigned)m->wince.verify_helper_entry_count,
                pc32, rav, sp,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
                stk4);
            if (ret_count > 0) {
                fprintf(stderr,
                    "[WINCE_CB_RET] label=verify_helper_#%u",
                    (unsigned)m->wince.verify_helper_entry_count);
                for (i = 0; i < ret_count; i++) {
                    fprintf(stderr,
                        " ret%zu=%#010x@+0x%02X callsite=0x%08X",
                        i, ret_addrs[i], ret_offs[i],
                        ret_addrs[i] >= 8u ? ret_addrs[i] - 8u : 0u);
                }
                fputc('\n', stderr);
            }
            m->wince.callback_slot_diag_count++;
        }
        return;

    case 0x800A1134u:
        if (m->wince.alloc_helper_count < 8u) {
            m->wince.alloc_helper_count++;
            fprintf(stderr,
                "[WINCE_ALLOC_HELPER] #%u pc=0x%08X ra=0x%08X"
                " sp=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X"
                " a3=0x%08X v0=0x%08X s0=0x%08X s1=0x%08X"
                " s2=0x%08X\n",
                (unsigned)m->wince.alloc_helper_count,
                pc32,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
                sp,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1],
                s2);
            if (ret_count > 0) {
                fprintf(stderr,
                    "[WINCE_CB_RET] label=alloc_helper_#%u",
                    (unsigned)m->wince.alloc_helper_count);
                for (i = 0; i < ret_count; i++) {
                    fprintf(stderr,
                        " ret%zu=%#010x@+0x%02X callsite=0x%08X",
                        i, ret_addrs[i], ret_offs[i],
                        ret_addrs[i] >= 8u ? ret_addrs[i] - 8u : 0u);
                }
                fputc('\n', stderr);
            }
            m->wince.callback_slot_diag_count++;
        }
        return;

    case 0x800970A8u:
        if (m->wince.walker_entry_count < 8u) {
            uint32_t stk20 = 0, stk28 = 0, stk30 = 0, stk48 = 0;
            uint32_t stk50 = 0, stk54 = 0, stk58 = 0, stk94 = 0;
            m->wince.walker_entry_count++;
            (void)load_va_word(m, sp + 0x20u, &stk20);
            (void)load_va_word(m, sp + 0x28u, &stk28);
            (void)load_va_word(m, sp + 0x30u, &stk30);
            (void)load_va_word(m, sp + 0x48u, &stk48);
            (void)load_va_word(m, sp + 0x50u, &stk50);
            (void)load_va_word(m, sp + 0x54u, &stk54);
            (void)load_va_word(m, sp + 0x58u, &stk58);
            (void)load_va_word(m, sp + 0x94u, &stk94);
            fprintf(stderr,
                "[WINCE_WALKER_ENTRY] #%u pc=0x%08X ra=0x%08X"
                " sp=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X"
                " a3=0x%08X v0=0x%08X v1=0x%08X\n",
                (unsigned)m->wince.walker_entry_count,
                pc32,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
                sp,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V1]);
            fprintf(stderr,
                "[WINCE_WALKER_STK] #%u sp+0x20=0x%08X sp+0x28=0x%08X"
                " sp+0x30=0x%08X sp+0x48=0x%08X sp+0x50=0x%08X"
                " sp+0x54=0x%08X sp+0x58=0x%08X sp+0x94=0x%08X\n",
                (unsigned)m->wince.walker_entry_count,
                stk20, stk28, stk30, stk48, stk50, stk54,
                stk58, stk94);
            {
                unsigned w;
                for (w = 0; w < 16u; w++) {
                    uint32_t pte = 0;
                    uint32_t va = UINT32_C(0x80FFC1C8)
                                  + (uint32_t)(w * 4u);
                    bool ok = load_va_word(m, va, &pte);
                    fprintf(stderr,
                        "[WINCE_WALKER_PTE] #%u va=0x%08X"
                        " pte=0x%08X bit30=%u V=%u D=%u C=%u%s\n",
                        (unsigned)m->wince.walker_entry_count,
                        va, pte,
                        (pte >> 30) & 1u,
                        (pte >> 1) & 1u,
                        (pte >> 2) & 1u,
                        (pte >> 3) & 7u,
                        ok ? "" : " (unmapped)");
                }
            }
            /* Phase V: dump the pool-5 object array at
             * 0x80FFC000..0x80FFC1D0 (7 consecutive 76-byte
             * objects) and the full L1 tail region. */
            if (m->wince.walker_entry_count == 1u) {
                unsigned o;
                for (o = 0; o < 8u; o++) {
                    uint32_t base = UINT32_C(0x80FFC000)
                                    + (uint32_t)(o * 0x4Cu);
                    uint32_t w0 = 0, w4 = 0, w8 = 0, wC = 0;
                    uint32_t w10 = 0, w14 = 0, w18 = 0, w1C = 0;
                    (void)load_va_word(m, base + 0x00u, &w0);
                    (void)load_va_word(m, base + 0x04u, &w4);
                    (void)load_va_word(m, base + 0x08u, &w8);
                    (void)load_va_word(m, base + 0x0Cu, &wC);
                    (void)load_va_word(m, base + 0x10u, &w10);
                    (void)load_va_word(m, base + 0x14u, &w14);
                    (void)load_va_word(m, base + 0x18u, &w18);
                    (void)load_va_word(m, base + 0x1Cu, &w1C);
                    fprintf(stderr,
                        "[WINCE_POOL5_OBJ] #%u base=0x%08X"
                        " +0=0x%08X +4=0x%08X +8=0x%08X +C=0x%08X"
                        " +10=0x%08X +14=0x%08X +18=0x%08X"
                        " +1C=0x%08X\n",
                        o, base, w0, w4, w8, wC, w10, w14,
                        w18, w1C);
                }
                /* Full L1 tail region: dump 0x80668CC0..0x80669500
                 * to see the handle table shape. */
                {
                    unsigned k;
                    for (k = 0; k < 64u; k++) {
                        uint32_t v = 0;
                        uint32_t va = UINT32_C(0x80669400)
                                      + (uint32_t)(k * 4u);
                        (void)load_va_word(m, va, &v);
                        fprintf(stderr,
                            "[WINCE_L1_TAIL] va=0x%08X val=0x%08X\n",
                            va, v);
                    }
                }
            }
            /* Phase U: dump the L1 section-table region
             * around the worker's critical check word at
             * 0x806694A0 (= L1_base + 504*4). */
            if (m->wince.walker_entry_count == 1u) {
                uint32_t critical = 0;
                unsigned j;
                (void)load_va_word(m, UINT32_C(0x806694A0),
                    &critical);
                fprintf(stderr,
                    "[WINCE_L1_CRITICAL] va=0x806694A0"
                    " val=0x%08X (worker checks ==1)\n",
                    critical);
                /* Dump 32 words centred on 0x806694A0 to
                 * see neighbouring L1 entries and their
                 * current state. */
                for (j = 0; j < 32u; j++) {
                    uint32_t v = 0;
                    uint32_t va = UINT32_C(0x80669480)
                                  + (uint32_t)(j * 4u);
                    (void)load_va_word(m, va, &v);
                    fprintf(stderr,
                        "[WINCE_L1_REGION] va=0x%08X val=0x%08X%s\n",
                        va, v,
                        va == UINT32_C(0x806694A0) ? " <-- check" :
                        (va == UINT32_C(0x806694B8) ? " <-- hot L2" :
                         ""));
                }
            }
            /* Phase T (late): dump pool descriptor table at
             * teardown time. The early one-shot probe fires
             * during cold-boot kseg1 zero-fill before NK has
             * initialised anything. Walker entry fires deep
             * into the guest boot, so its snapshot reflects
             * the real state at the allocation-failure moment. */
            if (m->wince.walker_entry_count == 1u) {
                unsigned p;
                for (p = 0; p < 20u; p++) {
                    uint32_t base = UINT32_C(0x806600B8)
                                    + (uint32_t)(p * 20u);
                    uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0, w4 = 0;
                    (void)load_va_word(m, base + 0x00u, &w0);
                    (void)load_va_word(m, base + 0x04u, &w1);
                    (void)load_va_word(m, base + 0x08u, &w2);
                    (void)load_va_word(m, base + 0x0Cu, &w3);
                    (void)load_va_word(m, base + 0x10u, &w4);
                    fprintf(stderr,
                        "[WINCE_POOL_LATE] pool=%u base=0x%08X"
                        " +0=0x%08X +4=0x%08X +8=0x%08X"
                        " +C=0x%08X +10=0x%08X\n",
                        p, base, w0, w1, w2, w3, w4);
                }
            }
            m->wince.callback_slot_diag_count++;
        }
        return;

    case 0x80098108u:
        if (m->wince.verify_helper_ret_count < 8u) {
            uint32_t stk20 = 0;
            uint32_t stk30 = 0;
            uint32_t stk50 = 0;
            m->wince.verify_helper_ret_count++;
            (void)load_va_word(m, sp + 0x20u, &stk20);
            (void)load_va_word(m, sp + 0x30u, &stk30);
            (void)load_va_word(m, sp + 0x50u, &stk50);
            fprintf(stderr,
                "[WINCE_VERIFY_RET] #%u pc=0x%08X v0=0x%08X"
                " v1=0x%08X sp=0x%08X sp+0x20=0x%08X"
                " sp+0x30=0x%08X sp+0x50=0x%08X\n",
                (unsigned)m->wince.verify_helper_ret_count,
                pc32,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V1],
                sp, stk20, stk30, stk50);
            m->wince.callback_slot_diag_count++;
        }
        return;

    case 0x80097FC4u:
        if (!m->wince.teardown_section_entry_dumped) {
            m->wince.teardown_section_entry_dumped = true;
            fprintf(stderr,
                "[WINCE_SECTION_TEARDOWN] entry pc=0x%08X ra=0x%08X"
                " sp=0x%08X v0=0x%08X v1=0x%08X a0=0x%08X a1=0x%08X"
                " a2=0x%08X a3=0x%08X s0=0x%08X s1=0x%08X\n",
                pc32,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
                sp,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V1],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1]);
            /* Walk the stack for return addresses at depth 0x80,
             * wider than the default 0x40, to find the outer
             * caller that asked to tear down this section range. */
            {
                uint32_t ret_offs2[6] = {0};
                uint32_t ret_addrs2[6] = {0};
                size_t rc2 = collect_stack_return_sites(m, sp,
                    ret_offs2, ret_addrs2,
                    sizeof(ret_addrs2) / sizeof(ret_addrs2[0]),
                    0x80u);
                size_t k;
                if (rc2 > 0) {
                    fprintf(stderr,
                        "[WINCE_CB_RET] label=section_teardown_entry");
                    for (k = 0; k < rc2; k++) {
                        fprintf(stderr,
                            " ret%zu=%#010x@+0x%02X callsite=0x%08X",
                            k, ret_addrs2[k], ret_offs2[k],
                            ret_addrs2[k] >= 8u ? ret_addrs2[k] - 8u : 0u);
                    }
                    fputc('\n', stderr);
                }
            }
            m->wince.callback_slot_diag_count++;
        }
        return;

    case 0x800971B4u:
        if (m->wince.loop_prejal_count < 16u) {
            uint32_t s0v = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0];
            uint32_t a0v = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
            uint32_t pte = 0;
            (void)load_va_word(m, s0v + 0x0Cu, &pte);
            m->wince.loop_prejal_count++;
            fprintf(stderr,
                "[WINCE_FREE_LOOP] #%u s0=0x%08X s1=0x%08X"
                " s2=0x%08X s5=0x%08X pte_at_0x0C=0x%08X"
                " a0=0x%08X masked_pfn=0x%08X ra=0x%08X\n",
                (unsigned)m->wince.loop_prejal_count,
                s0v,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1],
                s2,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S5],
                pte, a0v,
                (a0v & UINT32_C(0x3FFFFFC0)) << 6,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
            m->wince.callback_slot_diag_count++;
        }
        return;

    case 0x800984CCu:
        if (!m->wince.publish_scan_call_dumped) {
            m->wince.publish_scan_call_dumped = true;
            fprintf(stderr,
                "[WINCE_CB_PC] label=publish_scan_call pc=0x%08X"
                " ra=0x%08X sp=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X"
                " a3=0x%08X s0=0x%08X s1=0x%08X s2=0x%08X\n",
                pc32,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
                sp,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1],
                s2);
            dump_code_window(m, pc32, 4u, 8u);
            m->wince.callback_slot_diag_count++;
        }
        return;

    case 0x800984B4u:
        if (!m->wince.publish_scan_cont_dumped) {
            m->wince.publish_scan_cont_dumped = true;
            fprintf(stderr,
                "[WINCE_CB_PC] label=publish_scan_cont pc=0x%08X"
                " ra=0x%08X sp=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X"
                " a3=0x%08X s0=0x%08X s1=0x%08X s2=0x%08X t9=0x%08X\n",
                pc32,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
                sp,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1],
                s2,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T9]);
            dump_code_window(m, pc32, 4u, 8u);
            m->wince.callback_slot_diag_count++;
        }
        return;

    case 0x80096F40u:
        if (!m->wince.state_bit_setter_dumped) {
            m->wince.state_bit_setter_dumped = true;
            fprintf(stderr,
                "[WINCE_CB_PC] label=l2_state_bit_set pc=0x%08X"
                " ra=0x%08X sp=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X"
                " s0=0x%08X s2=0x%08X\n",
                pc32,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
                sp,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0],
                s2);
            dump_code_window(m, pc32, 4u, 8u);
            m->wince.callback_slot_diag_count++;
        }
        return;

    case 0x80092798u:
        fprintf(stderr,
            "[WINCE_CB_PC] label=callback_gate_enter pc=0x%08X"
            " ra=0x%08X sp=0x%08X s2=0x%08X\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            sp,
            s2);
        maybe_log_callback_object_state(m, cpu, "gate_enter", s2);
        m->wince.callback_slot_diag_count++;
        return;

    case 0x01F8F4D4u:
        fprintf(stderr,
            "[WINCE_CB_PC] label=callback_consumer_entry pc=0x%08X"
            " ra=0x%08X sp=0x%08X slot_pa=0x%08X"
            " flag=0x%08X ptr=0x%08X aux=0x%08X arg=0x%08X\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            sp,
            base_pa,
            flag,
            ptr,
            aux,
            arg);
        dump_code_window(m, pc32, 6u, 12u);
        dump_pointer_bytes(m, "callback_slot_base", UINT32_C(0x01FE6544));
        maybe_log_callback_object_state(m, cpu, "consumer_entry", arg);
        if (!m->wince.cb_rearm_logged && cpu->translate_v2p != NULL) {
            uint64_t pa64 = 0;
            int ok = cpu->translate_v2p(cpu,
                (uint64_t)UINT32_C(0x01FE6544), &pa64,
                FLAG_NOEXCEPTIONS);
            m->wince.cb_rearm_logged = true;
            if (!ok) {
                fprintf(stderr,
                    "[WINCE_CB_REARM] result=unmapped va=0x01FE6544"
                    " pc=0x%08X\n", pc32);
            } else {
                uint32_t new_pa = (uint32_t)pa64 & ~UINT32_C(0xFFF);
                new_pa |= UINT32_C(0x01FE6544) & UINT32_C(0xFFF);
                if (new_pa != m->wince.callback_slot_watch_pa) {
                    fprintf(stderr,
                        "[WINCE_CB_REARM] old=0x%08X new=0x%08X"
                        " pc=0x%08X\n",
                        m->wince.callback_slot_watch_pa, new_pa, pc32);
                    m->wince.callback_slot_watch_pa = new_pa;
                    m->wince.callback_slot_watch_armed = true;
                    m->wince.cb_slot_first_seen = 0;
                    m->wince.cb_slot_zero_seen = 0;
                    memset(m->wince.cb_slot_first_pc, 0,
                        sizeof(m->wince.cb_slot_first_pc));
                    memset(m->wince.cb_slot_first_ra, 0,
                        sizeof(m->wince.cb_slot_first_ra));
                    memset(m->wince.cb_slot_first_val, 0,
                        sizeof(m->wince.cb_slot_first_val));
                    memset(m->wince.cb_slot_zero_pc, 0,
                        sizeof(m->wince.cb_slot_zero_pc));
                    memset(m->wince.cb_slot_zero_ra, 0,
                        sizeof(m->wince.cb_slot_zero_ra));
                    memset(m->wince.cb_slot_first_instr, 0,
                        sizeof(m->wince.cb_slot_first_instr));
                } else {
                    fprintf(stderr,
                        "[WINCE_CB_REARM] unchanged pa=0x%08X pc=0x%08X\n",
                        new_pa, pc32);
                }
            }
        }
        m->wince.callback_slot_diag_count++;
        return;

    case 0x01F8F4FCu:
        fprintf(stderr,
            "[WINCE_CB_PC] label=callback_consumer_jalr pc=0x%08X"
            " ra=0x%08X sp=0x%08X v0=0x%08X t7=0x%08X"
            " slot_pa=0x%08X flag=0x%08X ptr=0x%08X aux=0x%08X"
            " arg=0x%08X\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            sp,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
            t7,
            base_pa,
            flag,
            ptr,
            aux,
            arg);
        dump_code_window(m, pc32, 6u, 12u);
        dump_pointer_bytes(m, "callback_slot_base", UINT32_C(0x01FE6544));
        maybe_log_callback_object_state(m, cpu, "consumer_jalr", arg);
        m->wince.callback_slot_diag_count++;
        return;

    case 0x01FFA93Cu:
    case 0x8013593Cu:
        fprintf(stderr,
            "[WINCE_CB_PC] label=callback_flag_store pc=0x%08X"
            " ra=0x%08X v0=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X"
            " armed=%u slot_pa=0x%08X flag=0x%08X ptr=0x%08X\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
            m->wince.callback_slot_watch_armed ? 1u : 0u,
            base_pa,
            flag,
            ptr);
        m->wince.callback_slot_diag_count++;
        return;

    case 0x02070418u:
    case 0x801AB218u:
        fprintf(stderr,
            "[WINCE_CB_PC] label=callback_ptr_store_half pc=0x%08X"
            " ra=0x%08X v0=0x%08X a0=0x%08X armed=%u slot_pa=0x%08X"
            " flag=0x%08X ptr=0x%08X aux=0x%08X arg=0x%08X\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
            m->wince.callback_slot_watch_armed ? 1u : 0u,
            base_pa,
            flag,
            ptr,
            aux,
            arg);
        m->wince.callback_slot_diag_count++;
        return;

    default:
        return;
    }
}

static void maybe_note_section3_install_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32)
{
    uint32_t pc32;
    uint32_t ra;
    uint32_t sp;
    uint32_t sec3;
    uint32_t slot0;
    uint32_t slot694;
    uint32_t slot7e4;
    uint32_t slot7e8;

    if (!m || !cpu)
        return;
    if (m->wince.section3_install_probe_count >= 24)
        return;

    pc32 = canonicalize_nk_pc(raw_pc32);
    ra = canonicalize_nk_pc((uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);

    switch (pc32) {
    case 0x800975E4u:
    case 0x8009769Cu:
    case 0x800976A4u:
    case 0x800976D0u:
    case 0x800A335Cu:
    case 0x80099924u:
        break;
    default:
        return;
    }

    if (pc32 == 0x800A335Cu && ra != 0x800976A4u)
        return;
    if (pc32 == 0x80099924u && ra != 0x800976D8u)
        return;

    sp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
    sec3 = load_pa_word(m, 0x18CCu);
    slot0 = load_pa_word(m, 0x00FE5000u);
    slot694 = load_pa_word(m, 0x00FE5694u);
    slot7e4 = load_pa_word(m, 0x00FE57E4u);
    slot7e8 = load_pa_word(m, 0x00FE57E8u);

    fprintf(stderr,
        "[WINCE_SEC3_PATH] pc=0x%08X ra=0x%08X sp=0x%08X"
        " v0=0x%08X v1=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X"
        " sec3=0x%08X armed=%u page0=0x%08X p694=0x%08X p7e4=0x%08X"
        " p7e8=0x%08X\n",
        pc32,
        ra,
        sp,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V1],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
        sec3,
        m->wince.section3_page_watch_armed ? 1u : 0u,
        slot0,
        slot694,
        slot7e4,
        slot7e8);

    if (pc32 == 0x800976A4u || pc32 == 0x800976D0u
        || pc32 == 0x80099924u) {
        log_l2_table_state(m, "sec3_path", UINT32_C(0x80FE5000),
            UINT32_C(0x01F94B50));
        log_section0_focus_window(m, "sec3_path", UINT32_C(0x80FE5000));
    }

    m->wince.section3_install_probe_count++;
}

static void log_section3_owner_state(machine_t *m, struct cpu *cpu,
    const char *tag, uint32_t pc32)
{
    uint32_t sec0 = 0;
    uint32_t sec3 = 0;
    uint32_t db08 = 0;
    uint32_t db48 = 0;
    uint32_t obj97a0[8] = {0};
    uint32_t obj97c0[8] = {0};
    uint32_t a0 = 0;
    uint32_t a0_00 = 0;
    uint32_t a0_04 = 0;
    uint32_t a0_08 = 0;
    uint32_t a0_0c = 0;
    uint32_t a0_88 = 0;
    uint32_t a0_8c = 0;
    uint32_t a0_90 = 0;
    uint32_t a0_9c = 0;
    uint32_t slot0 = 0;
    uint32_t slot694 = 0;
    uint32_t slot7e4 = 0;
    bool sec0_ok;
    bool sec3_ok;
    bool db08_ok;
    bool db48_ok;
    bool obj97a0_ok[8];
    bool obj97c0_ok[8];
    bool a0_00_ok = false;
    bool a0_04_ok = false;
    bool a0_08_ok = false;
    bool a0_0c_ok = false;
    bool a0_88_ok = false;
    bool a0_8c_ok = false;
    bool a0_90_ok = false;
    bool a0_9c_ok = false;
    char sec0_buf[16];
    char sec3_buf[16];
    char db08_buf[16];
    char db48_buf[16];
    char a0_00_buf[16];
    char a0_04_buf[16];
    char a0_08_buf[16];
    char a0_0c_buf[16];
    char a0_88_buf[16];
    char a0_8c_buf[16];
    char a0_90_buf[16];
    char a0_9c_buf[16];
    char obj_a_buf[8][16];
    char obj_c_buf[8][16];
    size_t i;

    if (!m || !cpu)
        return;

    sec0_ok = true;
    sec0 = load_pa_word(m, 0x18C0u);
    sec3_ok = true;
    sec3 = load_pa_word(m, 0x18CCu);
    db08_ok = load_va_word(m, UINT32_C(0xFFFFD808), &db08);
    db48_ok = load_va_word(m, UINT32_C(0xFFFFDB48), &db48);
    slot0 = load_pa_word(m, 0x00FE5000u);
    slot694 = load_pa_word(m, 0x00FE5694u);
    slot7e4 = load_pa_word(m, 0x00FE57E4u);

    for (i = 0; i < 8; i++) {
        obj97a0_ok[i] = load_va_word(m,
            UINT32_C(0x806697A0) + (uint32_t)(i * 4u), &obj97a0[i]);
        obj97c0_ok[i] = load_va_word(m,
            UINT32_C(0x806697C0) + (uint32_t)(i * 4u), &obj97c0[i]);
    }

    a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
    if (a0 >= UINT32_C(0x80000000) && a0 < UINT32_C(0x81000000)) {
        a0_00_ok = load_va_word(m, a0 + 0x00u, &a0_00);
        a0_04_ok = load_va_word(m, a0 + 0x04u, &a0_04);
        a0_08_ok = load_va_word(m, a0 + 0x08u, &a0_08);
        a0_0c_ok = load_va_word(m, a0 + 0x0Cu, &a0_0c);
        a0_88_ok = load_va_word(m, a0 + 0x88u, &a0_88);
        a0_8c_ok = load_va_word(m, a0 + 0x8Cu, &a0_8c);
        a0_90_ok = load_va_word(m, a0 + 0x90u, &a0_90);
        a0_9c_ok = load_va_word(m, a0 + 0x9Cu, &a0_9c);
    }

    fprintf(stderr,
        "[WINCE_SEC3_OWNER] tag=%s pc=0x%08X ra=0x%08X sp=0x%08X"
        " a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X"
        " s0=0x%08X s1=0x%08X s2=0x%08X"
        " sec0=%s sec3=%s db08=%s db48=%s"
        " page0=0x%08X p694=0x%08X p7e4=0x%08X\n",
        tag ? tag : "?",
        pc32,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S2],
        format_word_or_unknown(sec0_buf, sizeof(sec0_buf), sec0_ok, sec0),
        format_word_or_unknown(sec3_buf, sizeof(sec3_buf), sec3_ok, sec3),
        format_word_or_unknown(db08_buf, sizeof(db08_buf), db08_ok, db08),
        format_word_or_unknown(db48_buf, sizeof(db48_buf), db48_ok, db48),
        slot0, slot694, slot7e4);
    fprintf(stderr,
        "[WINCE_SEC3_OWNER] tag=%s obj97a0=%s/%s/%s/%s/%s/%s/%s/%s\n",
        tag ? tag : "?",
        format_word_or_unknown(obj_a_buf[0], sizeof(obj_a_buf[0]), obj97a0_ok[0], obj97a0[0]),
        format_word_or_unknown(obj_a_buf[1], sizeof(obj_a_buf[1]), obj97a0_ok[1], obj97a0[1]),
        format_word_or_unknown(obj_a_buf[2], sizeof(obj_a_buf[2]), obj97a0_ok[2], obj97a0[2]),
        format_word_or_unknown(obj_a_buf[3], sizeof(obj_a_buf[3]), obj97a0_ok[3], obj97a0[3]),
        format_word_or_unknown(obj_a_buf[4], sizeof(obj_a_buf[4]), obj97a0_ok[4], obj97a0[4]),
        format_word_or_unknown(obj_a_buf[5], sizeof(obj_a_buf[5]), obj97a0_ok[5], obj97a0[5]),
        format_word_or_unknown(obj_a_buf[6], sizeof(obj_a_buf[6]), obj97a0_ok[6], obj97a0[6]),
        format_word_or_unknown(obj_a_buf[7], sizeof(obj_a_buf[7]), obj97a0_ok[7], obj97a0[7]));
    fprintf(stderr,
        "[WINCE_SEC3_OWNER] tag=%s obj97c0=%s/%s/%s/%s/%s/%s/%s/%s"
        " a0_words=%s/%s/%s/%s a0_tail=%s/%s/%s/%s\n",
        tag ? tag : "?",
        format_word_or_unknown(obj_c_buf[0], sizeof(obj_c_buf[0]), obj97c0_ok[0], obj97c0[0]),
        format_word_or_unknown(obj_c_buf[1], sizeof(obj_c_buf[1]), obj97c0_ok[1], obj97c0[1]),
        format_word_or_unknown(obj_c_buf[2], sizeof(obj_c_buf[2]), obj97c0_ok[2], obj97c0[2]),
        format_word_or_unknown(obj_c_buf[3], sizeof(obj_c_buf[3]), obj97c0_ok[3], obj97c0[3]),
        format_word_or_unknown(obj_c_buf[4], sizeof(obj_c_buf[4]), obj97c0_ok[4], obj97c0[4]),
        format_word_or_unknown(obj_c_buf[5], sizeof(obj_c_buf[5]), obj97c0_ok[5], obj97c0[5]),
        format_word_or_unknown(obj_c_buf[6], sizeof(obj_c_buf[6]), obj97c0_ok[6], obj97c0[6]),
        format_word_or_unknown(obj_c_buf[7], sizeof(obj_c_buf[7]), obj97c0_ok[7], obj97c0[7]),
        format_word_or_unknown(a0_00_buf, sizeof(a0_00_buf), a0_00_ok, a0_00),
        format_word_or_unknown(a0_04_buf, sizeof(a0_04_buf), a0_04_ok, a0_04),
        format_word_or_unknown(a0_08_buf, sizeof(a0_08_buf), a0_08_ok, a0_08),
        format_word_or_unknown(a0_0c_buf, sizeof(a0_0c_buf), a0_0c_ok, a0_0c),
        format_word_or_unknown(a0_88_buf, sizeof(a0_88_buf), a0_88_ok, a0_88),
        format_word_or_unknown(a0_8c_buf, sizeof(a0_8c_buf), a0_8c_ok, a0_8c),
        format_word_or_unknown(a0_90_buf, sizeof(a0_90_buf), a0_90_ok, a0_90),
        format_word_or_unknown(a0_9c_buf, sizeof(a0_9c_buf), a0_9c_ok, a0_9c));
}

static bool load_section3_descriptor_focus(machine_t *m, uint32_t *desc_ptr_out,
    uint32_t *desc_base_out)
{
    uint32_t desc_ptr = 0;

    if (!m || !desc_ptr_out || !desc_base_out)
        return false;
    if (!load_va_word(m, UINT32_C(0x806697DC), &desc_ptr))
        return false;
    if (desc_ptr < UINT32_C(0x80000000) || desc_ptr >= UINT32_C(0x81000000))
        return false;

    *desc_ptr_out = desc_ptr;
    *desc_base_out = desc_ptr & ~UINT32_C(0x1F);
    return true;
}

static void dump_section3_descriptor_window(machine_t *m, const char *tag)
{
    uint32_t desc_ptr = 0;
    uint32_t desc_base = 0;

    if (!load_section3_descriptor_focus(m, &desc_ptr, &desc_base)) {
        fprintf(stderr, "[WINCE_SEC3_DESC] tag=%s ptr=?\n",
            tag ? tag : "?");
        return;
    }
    dump_section3_descriptor_at(m, tag, desc_ptr);
}

static void dump_section3_descriptor_at(machine_t *m, const char *tag,
    uint32_t desc_ptr)
{
    uint32_t desc_base;
    uint32_t words[8] = {0};
    bool ok[8];
    char word_buf[8][16];
    size_t i;

    if (!m || desc_ptr < UINT32_C(0x80000000) || desc_ptr >= UINT32_C(0x81000000)) {
        fprintf(stderr, "[WINCE_SEC3_DESC] tag=%s ptr=%08X invalid\n",
            tag ? tag : "?", desc_ptr);
        return;
    }

    desc_base = desc_ptr & ~UINT32_C(0x1F);

    for (i = 0; i < 8; i++)
        ok[i] = load_va_word(m, desc_base + (uint32_t)(i * 4u), &words[i]);
    for (i = 0; i < 8; i++) {
        if (ok[i])
            snprintf(word_buf[i], sizeof(word_buf[i]), "%08X", words[i]);
        else
            snprintf(word_buf[i], sizeof(word_buf[i]), "????????");
    }

    fprintf(stderr,
        "[WINCE_SEC3_DESC] tag=%s ptr=0x%08X base=0x%08X"
        " w0=%s w1=%s w2=%s w3=%s w4=%s w5=%s w6=%s w7=%s\n",
        tag ? tag : "?",
        desc_ptr,
        desc_base,
        word_buf[0], word_buf[1], word_buf[2], word_buf[3],
        word_buf[4], word_buf[5], word_buf[6], word_buf[7]);
}

static void dump_section3_wrap_window(machine_t *m, const char *tag,
    uint32_t wrap_va)
{
    static const uint32_t offsets[] = {
        0x00u, 0x04u, 0x08u, 0x0Cu, 0x10u, 0x14u, 0x18u, 0x1Cu,
    };
    uint32_t words[sizeof(offsets) / sizeof(offsets[0])] = {0};
    bool ok[sizeof(offsets) / sizeof(offsets[0])] = { false };
    char buf[sizeof(offsets) / sizeof(offsets[0])][16];
    size_t i;

    if (!m || wrap_va < UINT32_C(0x80000000) || wrap_va >= UINT32_C(0x81000000)) {
        fprintf(stderr, "[WINCE_SEC3_WRAP] tag=%s va=%08X invalid\n",
            tag ? tag : "?", wrap_va);
        return;
    }

    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
        ok[i] = load_va_word(m, wrap_va + offsets[i], &words[i]);
    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        if (ok[i])
            snprintf(buf[i], sizeof(buf[i]), "%08X", words[i]);
        else
            snprintf(buf[i], sizeof(buf[i]), "????????");
    }

    fprintf(stderr,
        "[WINCE_SEC3_WRAP] tag=%s va=0x%08X"
        " +00=%s +04=%s +08=%s +0C=%s +10=%s +14=%s +18=%s +1C=%s\n",
        tag ? tag : "?",
        wrap_va,
        buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
}

static void dump_section3_retobj_window(machine_t *m, const char *tag,
    uint32_t obj_va)
{
    static const uint32_t offsets[] = {
        0x00u, 0x04u, 0x08u, 0x0Cu, 0x10u, 0x14u, 0x18u,
        0x48u, 0x4Cu, 0x50u, 0x54u, 0x58u,
        0x88u, 0x8Cu, 0x90u, 0x9Cu,
    };
    uint32_t words[sizeof(offsets) / sizeof(offsets[0])] = {0};
    bool ok[sizeof(offsets) / sizeof(offsets[0])] = { false };
    char buf[sizeof(offsets) / sizeof(offsets[0])][16];
    size_t i;

    if (!m || obj_va < UINT32_C(0x80000000) || obj_va >= UINT32_C(0x81000000)) {
        fprintf(stderr, "[WINCE_SEC3_RETOBJ] tag=%s va=%08X invalid\n",
            tag ? tag : "?", obj_va);
        return;
    }

    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
        ok[i] = load_va_word(m, obj_va + offsets[i], &words[i]);
    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        if (ok[i])
            snprintf(buf[i], sizeof(buf[i]), "%08X", words[i]);
        else
            snprintf(buf[i], sizeof(buf[i]), "????????");
    }

    fprintf(stderr,
        "[WINCE_SEC3_RETOBJ] tag=%s va=0x%08X"
        " +00=%s +04=%s +08=%s +0C=%s +10=%s +14=%s +18=%s"
        " +48=%s +4C=%s +50=%s +54=%s +58=%s"
        " +88=%s +8C=%s +90=%s +9C=%s\n",
        tag ? tag : "?",
        obj_va,
        buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6],
        buf[7], buf[8], buf[9], buf[10], buf[11],
        buf[12], buf[13], buf[14], buf[15]);
}

static bool resolve_section3_gate_entry(machine_t *m, uint32_t key_in,
    uint32_t *key_out, uint32_t *entry_out, uint32_t *state_ptr_out,
    bool *state_ok_out, unsigned char *state_byte_out, uint32_t *obj_out)
{
    uint32_t key = key_in;
    uint32_t translated = 0;
    uint32_t base = 0;
    uint32_t range_lo = 0;
    uint32_t range_hi = 0;
    uint32_t entry = 0;
    uint32_t entry_key = 0;
    uint32_t state_ptr = 0;
    uint32_t obj = 0;
    unsigned char state_byte = 0;
    bool state_ok = false;

    if (!m)
        return false;

    if (key >= 64u && key < 96u) {
        if (!load_va_word(m, UINT32_C(0xFFFFD704) + key * 4u, &translated))
            return false;
        key = translated;
    }

    if (key == 0)
        return false;
    if (!load_va_word(m, UINT32_C(0xFFFFDAC8), &base)
        || !load_va_word(m, UINT32_C(0x80669540), &range_lo)
        || !load_va_word(m, UINT32_C(0x80669544), &range_hi))
        return false;

    entry = (key & UINT32_C(0x1FFFFFFC)) + base;
    if (entry >= range_lo) {
        if (entry >= range_hi)
            return false;
        if (!load_va_word(m, entry + 8u, &entry_key) || entry_key != key)
            return false;
    }

    (void)load_va_word(m, entry + 0x14u, &state_ptr);
    (void)load_va_word(m, entry + 0x18u, &obj);
    if (state_ptr >= UINT32_C(0x80000000) && state_ptr < UINT32_C(0x81000000))
        state_ok = load_va_bytes(m, state_ptr + 5u, &state_byte, 1u);

    if (key_out)
        *key_out = key;
    if (entry_out)
        *entry_out = entry;
    if (state_ptr_out)
        *state_ptr_out = state_ptr;
    if (state_ok_out)
        *state_ok_out = state_ok;
    if (state_byte_out)
        *state_byte_out = state_byte;
    if (obj_out)
        *obj_out = obj;
    return true;
}

static void maybe_log_section3_gate_snapshot(machine_t *m, struct cpu *cpu,
    uint32_t pc32, uint32_t sec3)
{
    uint32_t raw_key;
    uint32_t frame_ra = 0;
    uint32_t frame_s0 = 0;
    uint32_t frame_s1 = 0;
    uint32_t frame_s2 = 0;
    uint32_t frame_s3 = 0;
    uint32_t slot_ptr = 0;
    uint32_t slot_words[4] = {0};
    uint32_t key;
    uint32_t translated = 0;
    uint32_t masked = 0;
    uint32_t base = 0;
    uint32_t range_lo = 0;
    uint32_t range_hi = 0;
    uint32_t entry = 0;
    uint32_t entry_key = 0;
    uint32_t state_ptr = 0;
    uint32_t obj = 0;
    uint32_t obj_88 = 0;
    uint32_t obj_8c = 0;
    uint32_t obj_90 = 0;
    uint32_t obj_9c = 0;
    uint32_t retobj = 0;
    uint32_t retobj_00 = 0;
    uint32_t retobj_04 = 0;
    uint32_t retobj_08 = 0;
    uint32_t retobj_0c = 0;
    uint32_t retobj_88 = 0;
    uint32_t retobj_8c = 0;
    uint32_t retobj_90 = 0;
    uint32_t retobj_9c = 0;
    bool translated_ok = false;
    bool frame_ra_ok = false;
    bool frame_s0_ok = false;
    bool frame_s1_ok = false;
    bool frame_s2_ok = false;
    bool frame_s3_ok = false;
    bool slot_ptr_ok = false;
    bool slot_word_ok[4] = { false, false, false, false };
    bool base_ok = false;
    bool lo_ok = false;
    bool hi_ok = false;
    bool entry_ok = false;
    bool entry_key_ok = false;
    bool state_ptr_ok = false;
    bool obj_ok = false;
    bool obj88_ok = false;
    bool obj8c_ok = false;
    bool obj90_ok = false;
    bool obj9c_ok = false;
    bool retobj_ok = false;
    bool retobj_00_ok = false;
    bool retobj_04_ok = false;
    bool retobj_08_ok = false;
    bool retobj_0c_ok = false;
    bool retobj_88_ok = false;
    bool retobj_8c_ok = false;
    bool retobj_90_ok = false;
    bool retobj_9c_ok = false;
    bool state_ok = false;
    unsigned char state_byte = 0;
    const char *reason = "ok";
    char key_buf[16];
    char frame_ra_buf[16];
    char frame_s0_buf[16];
    char frame_s1_buf[16];
    char frame_s2_buf[16];
    char frame_s3_buf[16];
    char slot_ptr_buf[16];
    char slot_word_buf[4][16];
    char masked_buf[16];
    char base_buf[16];
    char lo_buf[16];
    char hi_buf[16];
    char entry_buf[16];
    char entry_key_buf[16];
    char state_ptr_buf[16];
    char obj_buf[16];
    char obj88_buf[16];
    char obj8c_buf[16];
    char obj90_buf[16];
    char obj9c_buf[16];
    char retobj_buf[16];
    char retobj_00_buf[16];
    char retobj_04_buf[16];
    char retobj_08_buf[16];
    char retobj_0c_buf[16];
    char retobj_88_buf[16];
    char retobj_8c_buf[16];
    char retobj_90_buf[16];
    char retobj_9c_buf[16];
    char state_buf[8];

    if (!m || !cpu)
        return;
    if (m->wince.section3_gate_probe_count >= 12u)
        return;
    if (pc32 != UINT32_C(0x8009A9CC) && pc32 != UINT32_C(0x8009A7F8))
        return;

    raw_key = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
    frame_s0_ok = load_va_word(m,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP] + 0x14u, &frame_s0);
    frame_s1_ok = load_va_word(m,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP] + 0x18u, &frame_s1);
    frame_s2_ok = load_va_word(m,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP] + 0x1Cu, &frame_s2);
    frame_s3_ok = load_va_word(m,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP] + 0x20u, &frame_s3);
    frame_ra_ok = load_va_word(m,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP] + 0x24u, &frame_ra);
    slot_ptr_ok = load_va_word(m,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP] + 0x5Cu, &slot_ptr);
    if (slot_ptr_ok && slot_ptr != 0u) {
        for (unsigned i = 0; i < 4u; i++) {
            slot_word_ok[i] = load_va_word(m,
                slot_ptr + (uint32_t)(i * 4u), &slot_words[i]);
        }
    }
    key = raw_key;

    if (key >= 64u && key < 96u) {
        translated_ok = load_va_word(m, UINT32_C(0xFFFFD704) + key * 4u, &translated);
        if (translated_ok) {
            key = translated;
        } else {
            reason = "trans_fail";
        }
    }

    masked = key & UINT32_C(0x1FFFFFFC);
    base_ok = load_va_word(m, UINT32_C(0xFFFFDAC8), &base);
    lo_ok = load_va_word(m, UINT32_C(0x80669540), &range_lo);
    hi_ok = load_va_word(m, UINT32_C(0x80669544), &range_hi);

    if (!base_ok || !lo_ok || !hi_ok) {
        reason = "base_bounds_fail";
    } else {
        entry = masked + base;
        entry_ok = true;
        if (entry >= range_lo) {
            if (entry >= range_hi) {
                entry_ok = false;
                reason = "entry_hi_oob";
            } else if (!load_va_word(m, entry + 8u, &entry_key)) {
                entry_ok = false;
                reason = "entry_key_read_fail";
            } else {
                entry_key_ok = true;
                if (entry_key != key) {
                    entry_ok = false;
                    reason = "key_mismatch";
                } else {
                    reason = "entry_match";
                }
            }
        } else {
            reason = "entry_below_lo";
        }

        if (entry_ok) {
            state_ptr_ok = load_va_word(m, entry + 0x14u, &state_ptr);
            obj_ok = load_va_word(m, entry + 0x18u, &obj);
            if (state_ptr_ok
                && state_ptr >= UINT32_C(0x80000000)
                && state_ptr < UINT32_C(0x81000000)) {
                state_ok = load_va_bytes(m, state_ptr + 5u, &state_byte, 1u);
                if (state_ok)
                    reason = state_byte == 4u ? "state4" : "state_not4";
                else
                    reason = "state_byte_fail";
            } else if (!state_ptr_ok) {
                reason = "state_ptr_fail";
            } else {
                reason = "state_ptr_badva";
            }
            if (obj_ok
                && obj >= UINT32_C(0x80000000)
                && obj < UINT32_C(0x81000000)) {
                obj88_ok = load_va_word(m, obj + 0x88u, &obj_88);
                obj8c_ok = load_va_word(m, obj + 0x8Cu, &obj_8c);
                obj90_ok = load_va_word(m, obj + 0x90u, &obj_90);
                obj9c_ok = load_va_word(m, obj + 0x9Cu, &obj_9c);
            }
        }
    }

    if (state_ok)
        snprintf(state_buf, sizeof(state_buf), "%02X", state_byte);
    else
        snprintf(state_buf, sizeof(state_buf), "??");
    retobj = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0];
    if (retobj >= UINT32_C(0x80000000) && retobj < UINT32_C(0x81000000)) {
        retobj_ok = true;
        retobj_00_ok = load_va_word(m, retobj + 0x00u, &retobj_00);
        retobj_04_ok = load_va_word(m, retobj + 0x04u, &retobj_04);
        retobj_08_ok = load_va_word(m, retobj + 0x08u, &retobj_08);
        retobj_0c_ok = load_va_word(m, retobj + 0x0Cu, &retobj_0c);
        retobj_88_ok = load_va_word(m, retobj + 0x88u, &retobj_88);
        retobj_8c_ok = load_va_word(m, retobj + 0x8Cu, &retobj_8c);
        retobj_90_ok = load_va_word(m, retobj + 0x90u, &retobj_90);
        retobj_9c_ok = load_va_word(m, retobj + 0x9Cu, &retobj_9c);
        if (pc32 == UINT32_C(0x8009A7F8)
            && !m->wince.section3_retobj_watch_armed
            && retobj >= UINT32_C(0x80660000)) {
            m->wince.section3_retobj_watch_armed = true;
            m->wince.section3_retobj_watch_va = retobj;
            m->wince.section3_retobj_write_count = 0;
            fprintf(stderr,
                "[WINCE_SEC3_RETOBJ_ARM] va=0x%08X pa=0x%08X"
                " PC=0x%08X RA=0x%08X sec0=0x%08X sec3=0x%08X\n",
                retobj,
                table_va_to_pa(retobj),
                pc32,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
                load_pa_word(m, 0x18C0u),
                sec3);
            dump_section3_retobj_window(m, "arm", retobj);
        }
    }
    for (unsigned i = 0; i < 4u; i++) {
        if (slot_word_ok[i])
            snprintf(slot_word_buf[i], sizeof(slot_word_buf[i]), "%08X",
                slot_words[i]);
        else
            snprintf(slot_word_buf[i], sizeof(slot_word_buf[i]), "????????");
    }

    fprintf(stderr,
        "[WINCE_SEC3_GATE] tag=%s pc=0x%08X ra=0x%08X sp=0x%08X"
        " a0=0x%08X a1=0x%08X v0=0x%08X"
        " frame_ra=%s frame_s0=%s frame_s1=%s frame_s2=%s frame_s3=%s"
        " translated=%u key=%s masked=%s"
        " slot=%s slot0=%s slot1=%s slot2=%s slot3=%s"
        " base=%s lo=%s hi=%s entry=%s entry+8=%s state_ptr=%s state5=%s"
        " obj=%s obj+88=%s obj+8c=%s obj+90=%s obj+9c=%s"
        " ret=%s ret0=%s ret4=%s ret8=%s retc=%s"
        " ret+88=%s ret+8c=%s ret+90=%s ret+9c=%s"
        " sec0=0x%08X sec3=0x%08X reason=%s\n",
        pc32 == UINT32_C(0x8009A9CC) ? "a9cc" : "a7f8",
        pc32,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
        raw_key,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
        format_word_or_unknown(frame_ra_buf, sizeof(frame_ra_buf),
            frame_ra_ok, frame_ra),
        format_word_or_unknown(frame_s0_buf, sizeof(frame_s0_buf),
            frame_s0_ok, frame_s0),
        format_word_or_unknown(frame_s1_buf, sizeof(frame_s1_buf),
            frame_s1_ok, frame_s1),
        format_word_or_unknown(frame_s2_buf, sizeof(frame_s2_buf),
            frame_s2_ok, frame_s2),
        format_word_or_unknown(frame_s3_buf, sizeof(frame_s3_buf),
            frame_s3_ok, frame_s3),
        translated_ok ? 1u : 0u,
        format_word_or_unknown(key_buf, sizeof(key_buf), true, key),
        format_word_or_unknown(masked_buf, sizeof(masked_buf), true, masked),
        format_word_or_unknown(slot_ptr_buf, sizeof(slot_ptr_buf), slot_ptr_ok, slot_ptr),
        slot_word_buf[0], slot_word_buf[1], slot_word_buf[2], slot_word_buf[3],
        format_word_or_unknown(base_buf, sizeof(base_buf), base_ok, base),
        format_word_or_unknown(lo_buf, sizeof(lo_buf), lo_ok, range_lo),
        format_word_or_unknown(hi_buf, sizeof(hi_buf), hi_ok, range_hi),
        format_word_or_unknown(entry_buf, sizeof(entry_buf), entry_ok, entry),
        format_word_or_unknown(entry_key_buf, sizeof(entry_key_buf), entry_key_ok, entry_key),
        format_word_or_unknown(state_ptr_buf, sizeof(state_ptr_buf), state_ptr_ok, state_ptr),
        state_buf,
        format_word_or_unknown(obj_buf, sizeof(obj_buf), obj_ok, obj),
        format_word_or_unknown(obj88_buf, sizeof(obj88_buf), obj88_ok, obj_88),
        format_word_or_unknown(obj8c_buf, sizeof(obj8c_buf), obj8c_ok, obj_8c),
        format_word_or_unknown(obj90_buf, sizeof(obj90_buf), obj90_ok, obj_90),
        format_word_or_unknown(obj9c_buf, sizeof(obj9c_buf), obj9c_ok, obj_9c),
        format_word_or_unknown(retobj_buf, sizeof(retobj_buf), retobj_ok, retobj),
        format_word_or_unknown(retobj_00_buf, sizeof(retobj_00_buf), retobj_00_ok, retobj_00),
        format_word_or_unknown(retobj_04_buf, sizeof(retobj_04_buf), retobj_04_ok, retobj_04),
        format_word_or_unknown(retobj_08_buf, sizeof(retobj_08_buf), retobj_08_ok, retobj_08),
        format_word_or_unknown(retobj_0c_buf, sizeof(retobj_0c_buf), retobj_0c_ok, retobj_0c),
        format_word_or_unknown(retobj_88_buf, sizeof(retobj_88_buf), retobj_88_ok, retobj_88),
        format_word_or_unknown(retobj_8c_buf, sizeof(retobj_8c_buf), retobj_8c_ok, retobj_8c),
        format_word_or_unknown(retobj_90_buf, sizeof(retobj_90_buf), retobj_90_ok, retobj_90),
        format_word_or_unknown(retobj_9c_buf, sizeof(retobj_9c_buf), retobj_9c_ok, retobj_9c),
        load_pa_word(m, 0x18C0u),
        sec3,
        reason);
    m->wince.section3_gate_probe_count++;
}

static bool load_section3_context_head(machine_t *m, uint32_t *ctx_ptr_out,
    uint32_t *head_ptr_out)
{
    uint32_t ctx_ptr = 0;
    uint32_t head_ptr = 0;

    if (!m || !ctx_ptr_out || !head_ptr_out)
        return false;
    if (!load_va_word(m, UINT32_C(0xFFFFDAC0), &ctx_ptr))
        return false;
    if (ctx_ptr < UINT32_C(0x80000000) || ctx_ptr >= UINT32_C(0x81000000))
        return false;
    if (!load_va_word(m, ctx_ptr + 0x18u, &head_ptr))
        return false;

    *ctx_ptr_out = ctx_ptr;
    *head_ptr_out = head_ptr;
    return true;
}

static void dump_section3_context_head(machine_t *m, const char *tag,
    uint32_t pc32)
{
    uint32_t ctx_ptr = 0;
    uint32_t head_ptr = 0;
    uint32_t desc_ptr = 0;
    uint32_t desc_base = 0;
    uint32_t words[6] = {0};
    bool head_ok = false;
    bool desc_ok = false;
    bool word_ok[6];
    bool head_matches_desc = false;
    char ctx_buf[16];
    char head_buf[16];
    char desc_buf[16];
    char word_buf[6][16];
    size_t i;

    if (!m)
        return;

    head_ok = load_section3_context_head(m, &ctx_ptr, &head_ptr);
    desc_ok = load_section3_descriptor_focus(m, &desc_ptr, &desc_base);
    for (i = 0; i < 6; i++)
        word_ok[i] = head_ok && head_ptr != 0
            && load_va_word(m, head_ptr + (uint32_t)(i * 4u), &words[i]);
    if (head_ok && desc_ok) {
        head_matches_desc = head_ptr >= desc_base
            && head_ptr < desc_base + UINT32_C(0x40);
    }
    for (i = 0; i < 6; i++) {
        if (word_ok[i])
            snprintf(word_buf[i], sizeof(word_buf[i]), "%08X", words[i]);
        else
            snprintf(word_buf[i], sizeof(word_buf[i]), "????????");
    }

    fprintf(stderr,
        "[WINCE_CTX_HEAD] tag=%s pc=0x%08X ctx=%s slot=%s"
        " head_space=%s desc=%s head_matches_desc=%u"
        " w0=%s w1=%s w2=%s w3=%s w4=%s w5=%s\n",
        tag ? tag : "?",
        pc32,
        format_word_or_unknown(ctx_buf, sizeof(ctx_buf), head_ok, ctx_ptr),
        format_word_or_unknown(head_buf, sizeof(head_buf), head_ok, head_ptr),
        head_ok ? classify_va_space(head_ptr) : "?",
        format_word_or_unknown(desc_buf, sizeof(desc_buf), desc_ok, desc_ptr),
        head_matches_desc ? 1u : 0u,
        word_buf[0], word_buf[1], word_buf[2],
        word_buf[3], word_buf[4], word_buf[5]);
}

static void maybe_note_section3_callback_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32)
{
    static const struct {
        uint32_t pc;
        const char *label;
    } targets[] = {
        { 0x80099324u, "desc_fill" },
        { 0x80099528u, "cb_select" },
        { 0x80099538u, "cb_call" },
        { 0x80099540u, "cb_ret" },
        { 0x80099794u, "desc_bind" },
    };
    uint32_t pc32;
    uint32_t sec3;
    size_t i;

    if (!m || !cpu)
        return;

    sec3 = load_pa_word(m, 0x18CCu);
    if (!m->wince.section3_page_watch_armed
        && sec3 != UINT32_C(0x80FE5000))
        return;

    pc32 = canonicalize_nk_pc(raw_pc32);
    for (i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        uint32_t cb_index = 0;
        uint32_t cb_target = 0;
        uint32_t desc_ptr = 0;
        uint32_t desc_base = 0;
        bool cb_target_ok = false;
        bool desc_ok = false;
        char cb_buf[16];

        if (pc32 != targets[i].pc)
            continue;
        if ((m->wince.section3_callback_pc_mask & (UINT32_C(1) << i)) != 0)
            return;

        m->wince.section3_callback_pc_mask |= (UINT32_C(1) << i);
        desc_ok = load_section3_descriptor_focus(m, &desc_ptr, &desc_base);
        cb_index = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1] & 0xFFu;
        if (cb_index < 16u) {
            cb_target_ok = load_va_word(m,
                UINT32_C(0x80075824) + cb_index * 4u, &cb_target);
        }
        if (cb_target_ok)
            snprintf(cb_buf, sizeof(cb_buf), "%08X", cb_target);
        else
            snprintf(cb_buf, sizeof(cb_buf), "????????");

        fprintf(stderr,
            "[WINCE_SEC3_CB] tag=%s pc=0x%08X ra=0x%08X sp=0x%08X"
            " s1=0x%08X s3=0x%08X s7=0x%08X t3=0x%08X v0=0x%08X"
            " sec0=0x%08X sec3=0x%08X desc=%s cb_idx=%u cb=%s"
            " page0=0x%08X p694=0x%08X p7e4=0x%08X\n",
            targets[i].label,
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S3],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S7],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T3],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
            load_pa_word(m, 0x18C0u),
            sec3,
            desc_ok ? "set" : "?",
            cb_index,
            cb_buf,
            load_pa_word(m, 0x00FE5000u),
            load_pa_word(m, 0x00FE5694u),
            load_pa_word(m, 0x00FE57E4u));
        dump_code_window(m, pc32, 8u, 12u);
        if (cb_target_ok && cb_target >= UINT32_C(0x80060000)
            && cb_target < UINT32_C(0x81000000)) {
            dump_code_window(m, cb_target, 4u, 8u);
        }
        dump_section3_descriptor_window(m, targets[i].label);
        m->wince.section3_callback_probe_count++;
        return;
    }
}

static void maybe_note_section3_order_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32)
{
    uint32_t pc32;
    uint32_t sec3;

    if (!m || !cpu)
        return;
    if (m->wince.section3_order_probe_count >= 12u)
        return;

    pc32 = canonicalize_nk_pc(raw_pc32);
    sec3 = load_pa_word(m, 0x18CCu);

    switch (pc32) {
    case 0x800A2520u: {
        uint32_t a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
        uint32_t sec_idx = a0 >> 25;
        uint32_t l1_idx = (a0 >> 16) & 0x1FFu;
        uint32_t sub_idx = (a0 >> 12) & 0x0Fu;
        uint32_t sec_base = 0;
        uint32_t l1_val = 0;
        uint32_t slot_0c = 0;
        bool sec_ok = false;
        bool l1_ok = false;
        bool slot_ok = false;
        char sec_buf[16];
        char l1_buf[16];
        char slot_buf[16];

        if (sec_idx < 64u)
            sec_ok = load_va_word(m, UINT32_C(0xFFFFD8C0) + sec_idx * 4u, &sec_base);
        if (sec_ok)
            l1_ok = load_va_word(m, sec_base + l1_idx * 4u, &l1_val);
        if (l1_ok)
            slot_ok = load_va_word(m, l1_val + sub_idx * 4u + 0x0Cu, &slot_0c);

        fprintf(stderr,
            "[WINCE_SEC3_ORDER] tag=producer_start pc=0x%08X ra=0x%08X"
            " a0=0x%08X sec_idx=%u sec_base=%s l1_idx=0x%03X l1=%s"
            " sub_idx=0x%X slot+0c=%s\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            a0,
            sec_idx,
            format_word_or_unknown(sec_buf, sizeof(sec_buf), sec_ok, sec_base),
            l1_idx,
            format_word_or_unknown(l1_buf, sizeof(l1_buf), l1_ok, l1_val),
            sub_idx,
            format_word_or_unknown(slot_buf, sizeof(slot_buf), slot_ok, slot_0c));
        m->wince.section3_order_probe_count++;
        return;
    }

    case 0x8009A9CCu:
    case 0x8009A7F8u: {
        uint32_t sp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
        uint32_t a0_arg = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
        uint32_t a1_arg = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1];
        uint32_t key = 0;
        uint32_t entry = 0;
        uint32_t state_ptr = 0;
        uint32_t obj = 0;
        uint32_t obj_90 = 0;
        bool gate_ok = false;
        bool state_ok = false;
        bool obj90_ok = false;
        unsigned char state_byte = 0;
        char key_buf[16];
        char entry_buf[16];
        char state_ptr_buf[16];
        char obj_buf[16];
        char obj90_buf[16];
        char a0_buf[16];
        char state_buf[8];

        if (!m->wince.section3_page_watch_armed
            && sec3 != UINT32_C(0x80FE5000))
            return;
        gate_ok = resolve_section3_gate_entry(m, a0_arg, &key, &entry,
            &state_ptr, &state_ok, &state_byte, &obj);
        if (gate_ok
            && obj >= UINT32_C(0x80000000)
            && obj < UINT32_C(0x81000000)) {
            obj90_ok = load_va_word(m, obj + 0x90u, &obj_90);
        }
        if (state_ok)
            snprintf(state_buf, sizeof(state_buf), "%02X", state_byte);
        else
            snprintf(state_buf, sizeof(state_buf), "??");

        fprintf(stderr,
            "[WINCE_SEC3_ORDER] tag=%s pc=0x%08X ra=0x%08X sp=0x%08X"
            " a0=%s a1=0x%08X key=%s entry=%s state_ptr=%s state5=%s"
            " match4=%u obj=%s obj+90=%s sec0=0x%08X sec3=0x%08X\n",
            pc32 == UINT32_C(0x8009A9CC) ? "consumer_a9cc" : "consumer_a7f8",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            sp,
            format_word_or_unknown(a0_buf, sizeof(a0_buf), true, a0_arg),
            a1_arg,
            format_word_or_unknown(key_buf, sizeof(key_buf), gate_ok, key),
            format_word_or_unknown(entry_buf, sizeof(entry_buf), gate_ok, entry),
            format_word_or_unknown(state_ptr_buf, sizeof(state_ptr_buf), gate_ok, state_ptr),
            state_buf,
            state_ok && state_byte == 4u ? 1u : 0u,
            format_word_or_unknown(obj_buf, sizeof(obj_buf), gate_ok, obj),
            format_word_or_unknown(obj90_buf, sizeof(obj90_buf), obj90_ok, obj_90),
            load_pa_word(m, 0x18C0u),
            sec3);
        if (gate_ok)
            dump_section3_descriptor_window(m,
                pc32 == UINT32_C(0x8009A9CC)
                    ? "consumer_a9cc"
                    : "consumer_a7f8");
        m->wince.section3_order_probe_count++;
        return;
    }

    default:
        return;
    }
}

static void maybe_note_section3_caller_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32)
{
    uint32_t pc32;
    uint32_t sp;
    uint32_t sec3;
    uint32_t stack_words[10] = { 0 };
    static const uint32_t stack_offsets[10] = {
        0x88u, 0x8Cu, 0x90u, 0x94u, 0x98u,
        0x9Cu, 0xA0u, 0xA4u, 0xA8u, 0xACu,
    };
    bool stack_ok[10] = { false };
    uint32_t cp0_badvaddr;
    uint32_t cp0_cause;
    uint32_t cp0_epc;
    const char *tag;
    char stack88_buf[16];
    char stack_buf[9][16];

    if (!m || !cpu)
        return;
    if (m->wince.section3_caller_probe_count >= 12u)
        return;

    pc32 = canonicalize_nk_pc(raw_pc32);
    if (pc32 != UINT32_C(0x8008707C)
        && pc32 != UINT32_C(0x80087084)
        && pc32 != UINT32_C(0x800870FC)
        && pc32 != UINT32_C(0x80087104)
        && pc32 != UINT32_C(0x80087200)) {
        return;
    }
    sec3 = load_pa_word(m, 0x18CCu);
    if (sec3 != UINT32_C(0x80FE5000))
        return;

    sp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
    for (unsigned i = 0; i < 10u; i++) {
        stack_ok[i] = load_va_word(m, sp + stack_offsets[i], &stack_words[i]);
    }
    for (unsigned i = 0; i < 9u; i++) {
        format_word_or_unknown(stack_buf[i], sizeof(stack_buf[i]),
            stack_ok[i + 1], stack_words[i + 1]);
    }
    cp0_badvaddr = (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR];
    cp0_cause = (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_CAUSE];
    cp0_epc = (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_EPC];
    if (pc32 == UINT32_C(0x8008707C))
        tag = "pre_816e0";
    else if (pc32 == UINT32_C(0x80087084))
        tag = "post_816e0";
    else if (pc32 == UINT32_C(0x800870FC))
        tag = "cleanup_cd4e";
    else if (pc32 == UINT32_C(0x80087104))
        tag = "cleanup_cd2a";
    else
        tag = "cleanup_c9ea";

    fprintf(stderr,
        "[WINCE_SEC3_CALLER] tag=%s pc=0x%08X ra=0x%08X sp=0x%08X"
        " a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X s0=0x%08X"
        " badvaddr=0x%08X cause=0x%08X epc=0x%08X"
        " out=%s s8c=%s s90=%s s94=%s s98=%s s9c=%s sa0=%s sa4=%s sa8=%s sac=%s"
        " sec0=0x%08X sec3=0x%08X\n",
        tag,
        pc32,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
        sp,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0],
        cp0_badvaddr,
        cp0_cause,
        cp0_epc,
        format_word_or_unknown(stack88_buf, sizeof(stack88_buf),
            stack_ok[0], stack_words[0]),
        stack_buf[0], stack_buf[1], stack_buf[2], stack_buf[3],
        stack_buf[4], stack_buf[5], stack_buf[6], stack_buf[7], stack_buf[8],
        load_pa_word(m, 0x18C0u),
        sec3);
    m->wince.section3_caller_probe_count++;
}

static void maybe_note_section3_source_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32)
{
    uint32_t pc32;
    uint32_t global_desc = 0;
    uint32_t s0 = 0;
    uint32_t s1 = 0;
    uint32_t s2 = 0;
    uint32_t s0_00 = 0;
    uint32_t s0_04 = 0;
    uint32_t s0_8c = 0;
    uint32_t s0_90 = 0;
    bool global_ok = false;
    bool s0_00_ok = false;
    bool s0_04_ok = false;
    bool s0_8c_ok = false;
    bool s0_90_ok = false;
    char global_buf[16];
    char s0_buf[16];
    char s1_buf[16];
    char s2_buf[16];
    char s0_00_buf[16];
    char s0_04_buf[16];
    char s0_8c_buf[16];
    char s0_90_buf[16];
    const char *tag;

    if (!m || !cpu)
        return;
    if (m->wince.section3_source_probe_count >= 12u)
        return;

    pc32 = canonicalize_nk_pc(raw_pc32);
    if (pc32 != UINT32_C(0x80081AD0)
        && pc32 != UINT32_C(0x80081C74)
        && pc32 != UINT32_C(0x80081CC0)
        && pc32 != UINT32_C(0x80081E44)) {
        return;
    }

    if (pc32 == UINT32_C(0x80081AD0)) {
        uint32_t a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
        uint32_t a1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1];
        uint32_t a2 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2];
        uint32_t a3 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3];

        if (!(a0 == 0u && a1 == 1u && a2 == 0u && a3 == 0u))
            return;
        global_ok = load_va_word(m, UINT32_C(0x806797DC), &global_desc);
        fprintf(stderr,
            "[WINCE_SEC3_SRC] tag=entry pc=0x%08X ra=0x%08X sp=0x%08X"
            " a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X global=%s"
            " sec0=0x%08X sec3=0x%08X\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            a0, a1, a2, a3,
            format_word_or_unknown(global_buf, sizeof(global_buf),
                global_ok, global_desc),
            load_pa_word(m, 0x18C0u),
            load_pa_word(m, 0x18CCu));
        m->wince.section3_source_probe_count++;
        return;
    }

    if (pc32 == UINT32_C(0x80081E44)) {
        uint32_t a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
        uint32_t a1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1];
        uint32_t a2 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2];
        uint32_t a0_00 = 0;
        uint32_t a0_18 = 0;
        uint32_t a0_48 = 0;
        uint32_t a0_90 = 0;
        uint32_t a0_104 = 0;
        bool a0_00_ok = false;
        bool a0_18_ok = false;
        bool a0_48_ok = false;
        bool a0_90_ok = false;
        bool a0_104_ok = false;
        char a0_buf[16];
        char a1_buf[16];
        char a2_buf[16];
        char a0_00_buf[16];
        char a0_18_buf[16];
        char a0_48_buf[16];
        char a0_90_buf[16];
        char a0_104_buf[16];

        if (a0 < UINT32_C(0x80FE9000) || a0 >= UINT32_C(0x80FF0000))
            return;

        a0_00_ok = load_va_word(m, a0 + 0x00u, &a0_00);
        a0_18_ok = load_va_word(m, a0 + 0x18u, &a0_18);
        a0_48_ok = load_va_word(m, a0 + 0x48u, &a0_48);
        a0_90_ok = load_va_word(m, a0 + 0x90u, &a0_90);
        a0_104_ok = load_va_word(m, a0 + 0x104u, &a0_104);

        fprintf(stderr,
            "[WINCE_SEC3_SRC] tag=init_obj pc=0x%08X ra=0x%08X sp=0x%08X"
            " a0=%s a1=%s a2=%s a0+00=%s a0+18=%s a0+48=%s"
            " a0+90=%s a0+104=%s watch=%u watch_va=0x%08X"
            " sec0=0x%08X sec3=0x%08X\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            format_word_or_unknown(a0_buf, sizeof(a0_buf), true, a0),
            format_word_or_unknown(a1_buf, sizeof(a1_buf), true, a1),
            format_word_or_unknown(a2_buf, sizeof(a2_buf), true, a2),
            format_word_or_unknown(a0_00_buf, sizeof(a0_00_buf), a0_00_ok, a0_00),
            format_word_or_unknown(a0_18_buf, sizeof(a0_18_buf), a0_18_ok, a0_18),
            format_word_or_unknown(a0_48_buf, sizeof(a0_48_buf), a0_48_ok, a0_48),
            format_word_or_unknown(a0_90_buf, sizeof(a0_90_buf), a0_90_ok, a0_90),
            format_word_or_unknown(a0_104_buf, sizeof(a0_104_buf), a0_104_ok, a0_104),
            m->wince.section3_retobj_watch_armed ? 1u : 0u,
            m->wince.section3_retobj_watch_va,
            load_pa_word(m, 0x18C0u),
            load_pa_word(m, 0x18CCu));
        m->wince.section3_source_probe_count++;
        return;
    }

    tag = pc32 == UINT32_C(0x80081C74) ? "a3_zero_path" : "return_path";
    s0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0];
    s1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1];
    s2 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S2];
    global_ok = load_va_word(m, UINT32_C(0x806797DC), &global_desc);
    if (s0 >= UINT32_C(0x80000000) && s0 < UINT32_C(0x81000000)) {
        s0_00_ok = load_va_word(m, s0 + 0x00u, &s0_00);
        s0_04_ok = load_va_word(m, s0 + 0x04u, &s0_04);
        s0_8c_ok = load_va_word(m, s0 + 0x8Cu, &s0_8c);
        s0_90_ok = load_va_word(m, s0 + 0x90u, &s0_90);
    }

    fprintf(stderr,
        "[WINCE_SEC3_SRC] tag=%s pc=0x%08X ra=0x%08X sp=0x%08X"
        " s0=%s s1=%s s2=%s global=%s"
        " s0+00=%s s0+04=%s s0+8c=%s s0+90=%s"
        " sec0=0x%08X sec3=0x%08X\n",
        tag,
        pc32,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
        format_word_or_unknown(s0_buf, sizeof(s0_buf), true, s0),
        format_word_or_unknown(s1_buf, sizeof(s1_buf), true, s1),
        format_word_or_unknown(s2_buf, sizeof(s2_buf), true, s2),
        format_word_or_unknown(global_buf, sizeof(global_buf),
            global_ok, global_desc),
        format_word_or_unknown(s0_00_buf, sizeof(s0_00_buf), s0_00_ok, s0_00),
        format_word_or_unknown(s0_04_buf, sizeof(s0_04_buf), s0_04_ok, s0_04),
        format_word_or_unknown(s0_8c_buf, sizeof(s0_8c_buf), s0_8c_ok, s0_8c),
        format_word_or_unknown(s0_90_buf, sizeof(s0_90_buf), s0_90_ok, s0_90),
        load_pa_word(m, 0x18C0u),
        load_pa_word(m, 0x18CCu));
    if (s1 >= UINT32_C(0x80FE9CC0) && s1 < UINT32_C(0x80FE9D00))
        dump_section3_descriptor_window(m, tag);
    m->wince.section3_source_probe_count++;
}

static void maybe_note_section3_obj_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32)
{
    uint32_t pc32;
    uint32_t obj = 0;
    uint32_t obj0 = 0;
    uint32_t obj4 = 0;
    uint32_t obj8 = 0;
    uint32_t obj10 = 0;
    uint32_t obj14 = 0;
    uint32_t obj18 = 0;
    uint32_t obj1c = 0;
    uint32_t arg1_word0 = 0;
    bool obj_ok = false;
    bool obj0_ok = false;
    bool obj4_ok = false;
    bool obj8_ok = false;
    bool obj10_ok = false;
    bool obj14_ok = false;
    bool obj18_ok = false;
    bool obj1c_ok = false;
    bool arg1_word0_ok = false;
    const char *tag;
    char obj_buf[16];
    char obj0_buf[16];
    char obj4_buf[16];
    char obj8_buf[16];
    char obj10_buf[16];
    char obj14_buf[16];
    char obj18_buf[16];
    char obj1c_buf[16];
    char arg1_buf[16];

    if (!m || !cpu)
        return;
    if (m->wince.section3_obj_probe_count >= 12u)
        return;

    pc32 = canonicalize_nk_pc(raw_pc32);
    if (pc32 == UINT32_C(0x80086BB0)
        || pc32 == UINT32_C(0x80086EDC)
        || pc32 == UINT32_C(0x80086EE0)) {
        uint32_t obj_va;
        uint32_t aux_va;
        uint32_t obj_00 = 0;
        uint32_t obj_04 = 0;
        uint32_t obj_08 = 0;
        uint32_t obj_0c = 0;
        uint32_t obj_10 = 0;
        uint32_t obj_14 = 0;
        uint32_t obj_18 = 0;
        uint32_t obj_90 = 0;
        bool obj_00_ok = false;
        bool obj_04_ok = false;
        bool obj_08_ok = false;
        bool obj_0c_ok = false;
        bool obj_10_ok = false;
        bool obj_14_ok = false;
        bool obj_18_ok = false;
        bool obj_90_ok = false;
        char obj_buf[16];
        char aux_buf[16];
        char obj_00_buf[16];
        char obj_04_buf[16];
        char obj_08_buf[16];
        char obj_0c_buf[16];
        char obj_10_buf[16];
        char obj_14_buf[16];
        char obj_18_buf[16];
        char obj_90_buf[16];
        const char *tag;

        if (pc32 == UINT32_C(0x80086BB0)) {
            obj_va = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1];
            aux_va = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2];
            tag = "caller_9a0b0";
        } else {
            obj_va = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
            aux_va = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1];
            tag = pc32 == UINT32_C(0x80086EDC)
                ? "pre_81e44_setup"
                : "call_81e44";
        }

        if (obj_va < UINT32_C(0x80FE9000) || obj_va >= UINT32_C(0x80FF0000))
            return;

        obj_00_ok = load_va_word(m, obj_va + 0x00u, &obj_00);
        obj_04_ok = load_va_word(m, obj_va + 0x04u, &obj_04);
        obj_08_ok = load_va_word(m, obj_va + 0x08u, &obj_08);
        obj_0c_ok = load_va_word(m, obj_va + 0x0Cu, &obj_0c);
        obj_10_ok = load_va_word(m, obj_va + 0x10u, &obj_10);
        obj_14_ok = load_va_word(m, obj_va + 0x14u, &obj_14);
        obj_18_ok = load_va_word(m, obj_va + 0x18u, &obj_18);
        obj_90_ok = load_va_word(m, obj_va + 0x90u, &obj_90);

        fprintf(stderr,
            "[WINCE_SEC3_SRC] tag=%s pc=0x%08X ra=0x%08X sp=0x%08X"
            " obj=%s aux=%s"
            " obj+00=%s obj+04=%s obj+08=%s obj+0c=%s"
            " obj+10=%s obj+14=%s obj+18=%s obj+90=%s"
            " watch=%u watch_va=0x%08X sec0=0x%08X sec3=0x%08X\n",
            tag,
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            format_word_or_unknown(obj_buf, sizeof(obj_buf), true, obj_va),
            format_word_or_unknown(aux_buf, sizeof(aux_buf), true, aux_va),
            format_word_or_unknown(obj_00_buf, sizeof(obj_00_buf), obj_00_ok, obj_00),
            format_word_or_unknown(obj_04_buf, sizeof(obj_04_buf), obj_04_ok, obj_04),
            format_word_or_unknown(obj_08_buf, sizeof(obj_08_buf), obj_08_ok, obj_08),
            format_word_or_unknown(obj_0c_buf, sizeof(obj_0c_buf), obj_0c_ok, obj_0c),
            format_word_or_unknown(obj_10_buf, sizeof(obj_10_buf), obj_10_ok, obj_10),
            format_word_or_unknown(obj_14_buf, sizeof(obj_14_buf), obj_14_ok, obj_14),
            format_word_or_unknown(obj_18_buf, sizeof(obj_18_buf), obj_18_ok, obj_18),
            format_word_or_unknown(obj_90_buf, sizeof(obj_90_buf), obj_90_ok, obj_90),
            m->wince.section3_retobj_watch_armed ? 1u : 0u,
            m->wince.section3_retobj_watch_va,
            load_pa_word(m, 0x18C0u),
            load_pa_word(m, 0x18CCu));
        if (!m->wince.section3_retobj_watch_armed) {
            m->wince.section3_retobj_watch_armed = true;
            m->wince.section3_retobj_watch_va = obj_va;
            m->wince.section3_retobj_write_count = 0;
            fprintf(stderr,
                "[WINCE_SEC3_RETOBJ_ARM] va=0x%08X pa=0x%08X"
                " PC=0x%08X RA=0x%08X source=%s\n",
                obj_va,
                table_va_to_pa(obj_va),
                pc32,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
                tag);
            dump_section3_retobj_window(m, tag, obj_va);
        }
        m->wince.section3_source_probe_count++;
        return;
    }

    if (pc32 == UINT32_C(0x8009A0B0)) {
        uint32_t a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
        uint32_t a1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1];
        uint32_t a2 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2];
        uint32_t a1_00 = 0;
        uint32_t a1_04 = 0;
        uint32_t a1_08 = 0;
        uint32_t a1_0c = 0;
        uint32_t a1_10 = 0;
        uint32_t a1_14 = 0;
        uint32_t a1_18 = 0;
        uint32_t a1_90 = 0;
        bool a1_00_ok = false;
        bool a1_04_ok = false;
        bool a1_08_ok = false;
        bool a1_0c_ok = false;
        bool a1_10_ok = false;
        bool a1_14_ok = false;
        bool a1_18_ok = false;
        bool a1_90_ok = false;
        char a0_buf[16];
        char a1_buf[16];
        char a2_buf[16];
        char a1_00_buf[16];
        char a1_04_buf[16];
        char a1_08_buf[16];
        char a1_0c_buf[16];
        char a1_10_buf[16];
        char a1_14_buf[16];
        char a1_18_buf[16];
        char a1_90_buf[16];

        if (a0 != UINT32_C(0x80074C38) && a0 != UINT32_C(0x80074C50))
            return;
        if (a1 < UINT32_C(0x80000000) || a1 >= UINT32_C(0x81000000))
            return;

        a1_00_ok = load_va_word(m, a1 + 0x00u, &a1_00);
        a1_04_ok = load_va_word(m, a1 + 0x04u, &a1_04);
        a1_08_ok = load_va_word(m, a1 + 0x08u, &a1_08);
        a1_0c_ok = load_va_word(m, a1 + 0x0Cu, &a1_0c);
        a1_10_ok = load_va_word(m, a1 + 0x10u, &a1_10);
        a1_14_ok = load_va_word(m, a1 + 0x14u, &a1_14);
        a1_18_ok = load_va_word(m, a1 + 0x18u, &a1_18);
        a1_90_ok = load_va_word(m, a1 + 0x90u, &a1_90);

        if (a0 == UINT32_C(0x80074C38)
            && !m->wince.section3_retobj_watch_armed
            && a1 >= UINT32_C(0x80660000)) {
            m->wince.section3_retobj_watch_armed = true;
            m->wince.section3_retobj_watch_va = a1;
            m->wince.section3_retobj_write_count = 0;
            fprintf(stderr,
                "[WINCE_SEC3_RETOBJ_ARM] va=0x%08X pa=0x%08X"
                " PC=0x%08X RA=0x%08X source=create_handle\n",
                a1,
                table_va_to_pa(a1),
                pc32,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
            dump_section3_retobj_window(m, "create_handle", a1);
        }

        fprintf(stderr,
            "[WINCE_SEC3_OBJ] tag=%s pc=0x%08X ra=0x%08X sp=0x%08X"
            " a0=%s a1=%s a2=%s"
            " a1+00=%s a1+04=%s a1+08=%s a1+0c=%s"
            " a1+10=%s a1+14=%s a1+18=%s a1+90=%s"
            " watch=%u watch_va=0x%08X sec0=0x%08X sec3=0x%08X\n",
            a0 == UINT32_C(0x80074C38) ? "create_4c38" : "create_4c50",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            format_word_or_unknown(a0_buf, sizeof(a0_buf), true, a0),
            format_word_or_unknown(a1_buf, sizeof(a1_buf), true, a1),
            format_word_or_unknown(a2_buf, sizeof(a2_buf), true, a2),
            format_word_or_unknown(a1_00_buf, sizeof(a1_00_buf), a1_00_ok, a1_00),
            format_word_or_unknown(a1_04_buf, sizeof(a1_04_buf), a1_04_ok, a1_04),
            format_word_or_unknown(a1_08_buf, sizeof(a1_08_buf), a1_08_ok, a1_08),
            format_word_or_unknown(a1_0c_buf, sizeof(a1_0c_buf), a1_0c_ok, a1_0c),
            format_word_or_unknown(a1_10_buf, sizeof(a1_10_buf), a1_10_ok, a1_10),
            format_word_or_unknown(a1_14_buf, sizeof(a1_14_buf), a1_14_ok, a1_14),
            format_word_or_unknown(a1_18_buf, sizeof(a1_18_buf), a1_18_ok, a1_18),
            format_word_or_unknown(a1_90_buf, sizeof(a1_90_buf), a1_90_ok, a1_90),
            m->wince.section3_retobj_watch_armed ? 1u : 0u,
            m->wince.section3_retobj_watch_va,
            load_pa_word(m, 0x18C0u),
            load_pa_word(m, 0x18CCu));
        m->wince.section3_obj_probe_count++;
        return;
    } else if (pc32 == UINT32_C(0x800A9F6C)) {
        obj = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
        obj_ok = obj >= UINT32_C(0x80000000) && obj < UINT32_C(0x81000000);
        if (!obj_ok)
            return;
        obj14_ok = load_va_word(m, obj + 0x14u, &obj14);
        if (!obj14_ok || obj14 != UINT32_C(0x80074C68))
            return;

        obj0_ok = load_va_word(m, obj + 0x00u, &obj0);
        obj4_ok = load_va_word(m, obj + 0x04u, &obj4);
        obj8_ok = load_va_word(m, obj + 0x08u, &obj8);
        obj10_ok = load_va_word(m, obj + 0x10u, &obj10);
        obj18_ok = load_va_word(m, obj + 0x18u, &obj18);
        obj1c_ok = load_va_word(m, obj + 0x1Cu, &obj1c);
        arg1_word0_ok = load_va_word(m,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1], &arg1_word0);
        tag = "cb_entry";
    } else if (pc32 == UINT32_C(0x8009A104) || pc32 == UINT32_C(0x8009A10C)) {
        obj_ok = load_va_word(m,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP] + 0x1Cu, &obj);
        if (!obj_ok || obj < UINT32_C(0x80000000) || obj >= UINT32_C(0x81000000))
            return;
        obj14_ok = load_va_word(m, obj + 0x14u, &obj14);
        if (!obj14_ok || obj14 != UINT32_C(0x80074C68))
            return;

        obj0_ok = load_va_word(m, obj + 0x00u, &obj0);
        obj4_ok = load_va_word(m, obj + 0x04u, &obj4);
        obj8_ok = load_va_word(m, obj + 0x08u, &obj8);
        obj10_ok = load_va_word(m, obj + 0x10u, &obj10);
        obj18_ok = load_va_word(m, obj + 0x18u, &obj18);
        obj1c_ok = load_va_word(m, obj + 0x1Cu, &obj1c);
        tag = pc32 == UINT32_C(0x8009A104) ? "post_cb" : "post_cb_delay";
    } else {
        return;
    }

    fprintf(stderr,
        "[WINCE_SEC3_OBJ] tag=%s pc=0x%08X ra=0x%08X sp=0x%08X"
        " v0=0x%08X a0=0x%08X a1=%s"
        " obj=%s obj0=%s obj4=%s obj8=%s obj10=%s obj14=%s obj18=%s obj1c=%s"
        " sec0=0x%08X sec3=0x%08X\n",
        tag,
        pc32,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
        format_word_or_unknown(arg1_buf, sizeof(arg1_buf),
            arg1_word0_ok, arg1_word0),
        format_word_or_unknown(obj_buf, sizeof(obj_buf), obj_ok, obj),
        format_word_or_unknown(obj0_buf, sizeof(obj0_buf), obj0_ok, obj0),
        format_word_or_unknown(obj4_buf, sizeof(obj4_buf), obj4_ok, obj4),
        format_word_or_unknown(obj8_buf, sizeof(obj8_buf), obj8_ok, obj8),
        format_word_or_unknown(obj10_buf, sizeof(obj10_buf), obj10_ok, obj10),
        format_word_or_unknown(obj14_buf, sizeof(obj14_buf), obj14_ok, obj14),
        format_word_or_unknown(obj18_buf, sizeof(obj18_buf), obj18_ok, obj18),
        format_word_or_unknown(obj1c_buf, sizeof(obj1c_buf), obj1c_ok, obj1c),
        load_pa_word(m, 0x18C0u),
        load_pa_word(m, 0x18CCu));
    m->wince.section3_obj_probe_count++;
}

static void maybe_note_section3_queue_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32)
{
    uint32_t pc32;

    if (!m || !cpu)
        return;
    if (m->wince.section3_queue_probe_count >= 16u)
        return;

    pc32 = canonicalize_nk_pc(raw_pc32);
    if (pc32 == UINT32_C(0x80099F6C)) {
        uint32_t a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
        uint32_t a1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1];
        uint32_t a0_08 = 0;
        uint32_t a0_0c = 0;
        uint32_t a0_10 = 0;
        uint32_t a0_14 = 0;
        uint32_t a0_18 = 0;
        uint32_t a0_1c = 0;
        bool a0_08_ok = false;
        bool a0_0c_ok = false;
        bool a0_10_ok = false;
        bool a0_14_ok = false;
        bool a0_18_ok = false;
        bool a0_1c_ok = false;
        char a0_08_buf[16];
        char a0_0c_buf[16];
        char a0_10_buf[16];
        char a0_14_buf[16];
        char a0_18_buf[16];
        char a0_1c_buf[16];

        if (a0 < UINT32_C(0x80000000) || a0 >= UINT32_C(0x81000000))
            return;

        a0_08_ok = load_va_word(m, a0 + 0x08u, &a0_08);
        a0_0c_ok = load_va_word(m, a0 + 0x0Cu, &a0_0c);
        a0_10_ok = load_va_word(m, a0 + 0x10u, &a0_10);
        a0_14_ok = load_va_word(m, a0 + 0x14u, &a0_14);
        a0_18_ok = load_va_word(m, a0 + 0x18u, &a0_18);
        a0_1c_ok = load_va_word(m, a0 + 0x1Cu, &a0_1c);

        fprintf(stderr,
            "[WINCE_SEC3_QUEUE] tag=wrap_commit pc=0x%08X ra=0x%08X"
            " sp=0x%08X a0=0x%08X a1=0x%08X"
            " a0+08=%s a0+0c=%s a0+10=%s a0+14=%s a0+18=%s a0+1c=%s"
            " watch=%u watch_va=0x%08X\n",
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            a0,
            a1,
            format_word_or_unknown(a0_08_buf, sizeof(a0_08_buf), a0_08_ok, a0_08),
            format_word_or_unknown(a0_0c_buf, sizeof(a0_0c_buf), a0_0c_ok, a0_0c),
            format_word_or_unknown(a0_10_buf, sizeof(a0_10_buf), a0_10_ok, a0_10),
            format_word_or_unknown(a0_14_buf, sizeof(a0_14_buf), a0_14_ok, a0_14),
            format_word_or_unknown(a0_18_buf, sizeof(a0_18_buf), a0_18_ok, a0_18),
            format_word_or_unknown(a0_1c_buf, sizeof(a0_1c_buf), a0_1c_ok, a0_1c),
            m->wince.section3_retobj_watch_armed ? 1u : 0u,
            m->wince.section3_retobj_watch_va);
        if (m->wince.section3_retobj_watch_armed
            && (a0 == (m->wince.section3_retobj_watch_va & ~UINT32_C(0x3))
                || a0_18 == m->wince.section3_retobj_watch_va)) {
            dump_section3_retobj_window(m, "wrap_commit", m->wince.section3_retobj_watch_va);
        }
        m->wince.section3_queue_probe_count++;
        return;
    }

    if (pc32 == UINT32_C(0x800998C0)) {
        uint32_t a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
        uint32_t ev0 = 0;
        uint32_t ev1 = 0;
        uint32_t ev2 = 0;
        uint32_t ev3 = 0;
        uint32_t dac0 = 0;
        uint32_t dac4 = 0;
        uint32_t d808 = 0;
        bool ev0_ok = false;
        bool ev1_ok = false;
        bool ev2_ok = false;
        bool ev3_ok = false;
        bool dac0_ok = false;
        bool dac4_ok = false;
        bool d808_ok = false;
        char ev0_buf[16];
        char ev1_buf[16];
        char ev2_buf[16];
        char ev3_buf[16];
        char dac0_buf[16];
        char dac4_buf[16];
        char d808_buf[16];
        const char *label = NULL;

        switch (a0) {
        case UINT32_C(0x80669740):
            label = "evt_9740";
            break;
        case UINT32_C(0x806697A0):
            label = "evt_97a0";
            break;
        case UINT32_C(0x806696C0):
            label = "evt_96c0";
            break;
        default:
            return;
        }

        ev0_ok = load_va_word(m, a0 + 0x00u, &ev0);
        ev1_ok = load_va_word(m, a0 + 0x04u, &ev1);
        ev2_ok = load_va_word(m, a0 + 0x08u, &ev2);
        ev3_ok = load_va_word(m, a0 + 0x0Cu, &ev3);
        dac0_ok = load_va_word(m, UINT32_C(0xFFFFDAC0), &dac0);
        dac4_ok = load_va_word(m, UINT32_C(0xFFFFDAC4), &dac4);
        d808_ok = load_va_word(m, UINT32_C(0xFFFFD808), &d808);

        fprintf(stderr,
            "[WINCE_SEC3_QUEUE] tag=%s pc=0x%08X ra=0x%08X sp=0x%08X"
            " a0=0x%08X ev0=%s ev1=%s ev2=%s ev3=%s"
            " dac0=%s dac4=%s d808=%s watch=%u watch_va=0x%08X\n",
            label,
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            a0,
            format_word_or_unknown(ev0_buf, sizeof(ev0_buf), ev0_ok, ev0),
            format_word_or_unknown(ev1_buf, sizeof(ev1_buf), ev1_ok, ev1),
            format_word_or_unknown(ev2_buf, sizeof(ev2_buf), ev2_ok, ev2),
            format_word_or_unknown(ev3_buf, sizeof(ev3_buf), ev3_ok, ev3),
            format_word_or_unknown(dac0_buf, sizeof(dac0_buf), dac0_ok, dac0),
            format_word_or_unknown(dac4_buf, sizeof(dac4_buf), dac4_ok, dac4),
            format_word_or_unknown(d808_buf, sizeof(d808_buf), d808_ok, d808),
            m->wince.section3_retobj_watch_armed ? 1u : 0u,
            m->wince.section3_retobj_watch_va);
        m->wince.section3_queue_probe_count++;
        return;
    }
}

static void maybe_note_section3_worker_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32)
{
    static const struct {
        uint32_t pc;
        const char *label;
    } targets[] = {
        { 0x801916A8u, "worker_entry" },
        { 0x80084120u, "worker_core" },
        { 0x80083BE8u, "type4_enqueue" },
        { 0x8008406Cu, "type4_splice" },
        { 0x80084658u, "type1_post" },
        { 0x80083204u, "type1_commit" },
        { 0x80088354u, "type1_wake" },
    };
    uint32_t pc32;
    uint32_t watch;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
    uint32_t ctx = 0;
    uint32_t ctx34 = 0;
    uint32_t ctx58 = 0;
    uint32_t ctx194 = 0;
    uint32_t obj00 = 0;
    uint32_t obj04 = 0;
    uint32_t obj08 = 0;
    uint32_t obj0c = 0;
    uint32_t obj48 = 0;
    uint32_t obj88 = 0;
    uint32_t obj8c = 0;
    uint32_t obj90 = 0;
    bool ctx_ok = false;
    bool ctx34_ok = false;
    bool ctx58_ok = false;
    bool ctx194_ok = false;
    bool obj00_ok = false;
    bool obj04_ok = false;
    bool obj08_ok = false;
    bool obj0c_ok = false;
    bool obj48_ok = false;
    bool obj88_ok = false;
    bool obj8c_ok = false;
    bool obj90_ok = false;
    bool a0_match;
    bool a1_match;
    bool a2_match;
    bool a3_match;
    char ctx_buf[16];
    char ctx34_buf[16];
    char ctx58_buf[16];
    char ctx194_buf[16];
    char obj00_buf[16];
    char obj04_buf[16];
    char obj08_buf[16];
    char obj0c_buf[16];
    char obj48_buf[16];
    char obj88_buf[16];
    char obj8c_buf[16];
    char obj90_buf[16];
    size_t i;

    if (!m || !cpu)
        return;
    if (!m->wince.section3_retobj_watch_armed)
        return;
    if (m->wince.section3_worker_probe_count >= 16u)
        return;

    pc32 = canonicalize_nk_pc(raw_pc32);
    for (i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        if (pc32 != targets[i].pc)
            continue;

        watch = m->wince.section3_retobj_watch_va;
        a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
        a1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1];
        a2 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2];
        a3 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3];
        a0_match = a0 == watch;
        a1_match = a1 == watch;
        a2_match = a2 == watch;
        a3_match = a3 == watch;

        ctx_ok = load_va_word(m, UINT32_C(0xFFFFDAC0), &ctx);
        if (ctx_ok && ctx >= UINT32_C(0x80000000) && ctx < UINT32_C(0x81000000)) {
            ctx34_ok = load_va_word(m, ctx + 0x34u, &ctx34);
            ctx58_ok = load_va_word(m, ctx + 0x58u, &ctx58);
            ctx194_ok = load_va_word(m, ctx + 0x194u, &ctx194);
        }

        obj00_ok = load_va_word(m, watch + 0x00u, &obj00);
        obj04_ok = load_va_word(m, watch + 0x04u, &obj04);
        obj08_ok = load_va_word(m, watch + 0x08u, &obj08);
        obj0c_ok = load_va_word(m, watch + 0x0Cu, &obj0c);
        obj48_ok = load_va_word(m, watch + 0x48u, &obj48);
        obj88_ok = load_va_word(m, watch + 0x88u, &obj88);
        obj8c_ok = load_va_word(m, watch + 0x8Cu, &obj8c);
        obj90_ok = load_va_word(m, watch + 0x90u, &obj90);

        fprintf(stderr,
            "[WINCE_SEC3_WORKER] tag=%s pc=0x%08X ra=0x%08X sp=0x%08X"
            " a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X"
            " match=%u/%u/%u/%u watch=0x%08X"
            " ctx=%s ctx+34=%s ctx+58=%s ctx+194=%s"
            " obj+00=%s obj+04=%s obj+08=%s obj+0c=%s"
            " obj+48=%s obj+88=%s obj+8c=%s obj+90=%s"
            " sec0=0x%08X sec3=0x%08X\n",
            targets[i].label,
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            a0, a1, a2, a3,
            a0_match ? 1u : 0u,
            a1_match ? 1u : 0u,
            a2_match ? 1u : 0u,
            a3_match ? 1u : 0u,
            watch,
            format_word_or_unknown(ctx_buf, sizeof(ctx_buf), ctx_ok, ctx),
            format_word_or_unknown(ctx34_buf, sizeof(ctx34_buf), ctx34_ok, ctx34),
            format_word_or_unknown(ctx58_buf, sizeof(ctx58_buf), ctx58_ok, ctx58),
            format_word_or_unknown(ctx194_buf, sizeof(ctx194_buf), ctx194_ok, ctx194),
            format_word_or_unknown(obj00_buf, sizeof(obj00_buf), obj00_ok, obj00),
            format_word_or_unknown(obj04_buf, sizeof(obj04_buf), obj04_ok, obj04),
            format_word_or_unknown(obj08_buf, sizeof(obj08_buf), obj08_ok, obj08),
            format_word_or_unknown(obj0c_buf, sizeof(obj0c_buf), obj0c_ok, obj0c),
            format_word_or_unknown(obj48_buf, sizeof(obj48_buf), obj48_ok, obj48),
            format_word_or_unknown(obj88_buf, sizeof(obj88_buf), obj88_ok, obj88),
            format_word_or_unknown(obj8c_buf, sizeof(obj8c_buf), obj8c_ok, obj8c),
            format_word_or_unknown(obj90_buf, sizeof(obj90_buf), obj90_ok, obj90),
            load_pa_word(m, 0x18C0u),
            load_pa_word(m, 0x18CCu));

        if (a0_match || a1_match || a2_match || a3_match
            || pc32 == UINT32_C(0x801916A8)
            || pc32 == UINT32_C(0x80084120)) {
            dump_section3_retobj_window(m, targets[i].label, watch);
        }
        if (pc32 == UINT32_C(0x80083BE8))
            note_type4_order_event(m, cpu,
                &m->wince.type4_order_enqueue_seq, "worker_enqueue");

        m->wince.section3_worker_probe_count++;
        return;
    }
}

static void maybe_note_section3_type4_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32)
{
    static const struct {
        uint32_t pc;
        const char *label;
    } targets[] = {
        { 0x80081BE4u, "type4_wrap_call" },
        { 0x80081BECu, "type4_wrap_ret" },
        { 0x80081C14u, "type3_init_begin" },
        { 0x80081C24u, "type3_pool_ret" },
        { 0x80081C54u, "type3_bind" },
        { 0x80081C84u, "type3_finalize" },
        { 0x80081CC0u, "type3_publish" },
    };
    uint32_t pc32;
    uint32_t s0;
    uint32_t s1;
    uint32_t s2;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t v0;
    char s0_buf[16];
    char s1_buf[16];
    size_t i;

    if (!m || !cpu)
        return;
    if (m->wince.section3_type4_probe_count >= 24u)
        return;

    pc32 = canonicalize_nk_pc(raw_pc32);
    for (i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        if (pc32 != targets[i].pc)
            continue;

        s0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0];
        s1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1];
        s2 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S2];
        a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
        a1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1];
        a2 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2];
        v0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0];

        fprintf(stderr,
            "[WINCE_SEC3_TYPE4] tag=%s pc=0x%08X ra=0x%08X sp=0x%08X"
            " a0=0x%08X a1=0x%08X a2=0x%08X v0=0x%08X"
            " s0=%s s1=%s s2=0x%08X sec0=0x%08X sec3=0x%08X\n",
            targets[i].label,
            pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            a0,
            a1,
            a2,
            v0,
            format_word_or_unknown(s0_buf, sizeof(s0_buf),
                s0 >= UINT32_C(0x80000000) && s0 < UINT32_C(0x81000000), s0),
            format_word_or_unknown(s1_buf, sizeof(s1_buf),
                s1 >= UINT32_C(0x80000000) && s1 < UINT32_C(0x81000000), s1),
            s2,
            load_pa_word(m, 0x18C0u),
            load_pa_word(m, 0x18CCu));
        if (s0 >= UINT32_C(0x80000000) && s0 < UINT32_C(0x81000000))
            dump_section3_retobj_window(m, targets[i].label, s0);
        if (s1 >= UINT32_C(0x80000000) && s1 < UINT32_C(0x81000000))
            dump_section3_wrap_window(m, targets[i].label,
                s1 & UINT32_C(0xFFFFFFFC));
        m->wince.section3_type4_probe_count++;
        return;
    }
}

static void maybe_note_section3_type4_gate_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32)
{
    uint32_t pc32;
    uint32_t wrap_va;
    uint32_t payload_va;
    uint32_t handle_va;
    uint32_t handle_live = 0;
    bool handle_live_ok = false;
    uint32_t ra;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t s0;
    uint32_t s1;
    uint32_t obj8c = 0;
    uint32_t obj90 = 0;
    bool obj8c_ok = false;
    bool obj90_ok = false;
    char obj8c_buf[16];
    char obj90_buf[16];
    const char *tag = NULL;

    if (!m || !cpu)
        return;
    if (m->wince.type4_gate_probe_count >= 12u)
        return;

    wrap_va = m->wince.type4_wrap_watch_va;
    payload_va = m->wince.type4_payload_watch_va;
    handle_va = m->wince.type4_handle_watch_va;
    if (wrap_va == 0u || payload_va == 0u)
        return;
    handle_live_ok = load_va_word(m, wrap_va + 0x08u, &handle_live);
    if (handle_live_ok && handle_live != 0u)
        handle_va = handle_live;

    pc32 = canonicalize_nk_pc(raw_pc32);
    ra = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA];
    a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
    a1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1];
    a2 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2];
    s0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0];
    s1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1];

    switch (pc32) {
    case UINT32_C(0x800A1200):
        if (ra != UINT32_C(0x80081C24) || s0 != payload_va)
            return;
        tag = "a1200_enter";
        break;
    case UINT32_C(0x8009A12C):
        if (a0 != handle_va)
            return;
        tag = (ra == UINT32_C(0x80081C40)) ? "a912c_enter" : "a912c_live";
        break;
    case UINT32_C(0x800A11B0):
        if (a0 == wrap_va && a1 == 2u && ra == UINT32_C(0x8009A11C)) {
            tag = "wrap_free_ctor_fail";
        } else if (a0 == payload_va && a1 == 3u
            && ra == UINT32_C(0x80081C0C)) {
            tag = "payload_free_wrap_fail";
        } else if (a0 == payload_va && a1 == 3u
            && ra == UINT32_C(0x80081C4C)) {
            tag = "payload_free_a1200_fail";
        } else {
            return;
        }
        break;
    default:
        return;
    }

    obj8c_ok = load_va_word(m, payload_va + 0x8Cu, &obj8c);
    obj90_ok = load_va_word(m, payload_va + 0x90u, &obj90);
    fprintf(stderr,
        "[WINCE_TYPE4_GATE] tag=%s pc=0x%08X ra=0x%08X sp=0x%08X"
        " a0=0x%08X a1=0x%08X a2=0x%08X s0=0x%08X s1=0x%08X"
        " wrap=0x%08X handle=0x%08X payload=0x%08X"
        " sec0=0x%08X sec3=0x%08X obj8c=%s obj90=%s\n",
        tag,
        pc32,
        ra,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
        a0,
        a1,
        a2,
        s0,
        s1,
        wrap_va,
        handle_va,
        payload_va,
        load_pa_word(m, 0x18C0u),
        load_pa_word(m, 0x18CCu),
        format_word_or_unknown(obj8c_buf, sizeof(obj8c_buf), obj8c_ok, obj8c),
        format_word_or_unknown(obj90_buf, sizeof(obj90_buf), obj90_ok, obj90));
    dump_section3_wrap_window(m, tag, wrap_va);
    dump_section3_retobj_window(m, tag, payload_va);
    if (pc32 == UINT32_C(0x8009A12C))
        note_type4_order_event(m, cpu,
            &m->wince.type4_order_cleanup_seq, "handle_cleanup");
    m->wince.type4_gate_probe_count++;
}

static void maybe_note_section3_type4_state_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32)
{
    uint32_t pc32;
    uint32_t wrap_va;
    uint32_t payload_va;
    uint32_t live_handle = 0;
    bool live_handle_ok = false;
    uint32_t entry_key = 0;
    uint32_t entry = 0;
    uint32_t state_ptr = 0;
    uint32_t obj = 0;
    bool state_ok = false;
    unsigned char state_byte = 0;
    uint32_t state_words[6] = {0};
    bool state_words_ok[6] = { false };
    unsigned char state18 = 0;
    unsigned char state19 = 0;
    bool state18_ok = false;
    bool state19_ok = false;
    uint32_t obj88 = 0;
    uint32_t obj8c = 0;
    uint32_t obj90 = 0;
    uint32_t obj9c = 0;
    bool obj88_ok = false;
    bool obj8c_ok = false;
    bool obj90_ok = false;
    bool obj9c_ok = false;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
    uint32_t ra;
    uint32_t sp;
    uint32_t sec0;
    uint32_t sec3;
    char live_handle_buf[16];
    char state_ptr_buf[16];
    char obj_buf[16];
    char obj88_buf[16];
    char obj8c_buf[16];
    char obj90_buf[16];
    char obj9c_buf[16];
    char state5_buf[3];
    char state18_buf[3];
    char state19_buf[3];
    char statew_buf[6][16];
    const char *tag = NULL;
    size_t i;

    if (!m || !cpu)
        return;
    if (m->wince.type4_state_probe_count >= 16u)
        return;

    wrap_va = m->wince.type4_wrap_watch_va;
    payload_va = m->wince.type4_payload_watch_va;
    if (wrap_va == 0u || payload_va == 0u)
        return;

    live_handle_ok = load_va_word(m, wrap_va + 0x08u, &live_handle);
    pc32 = canonicalize_nk_pc(raw_pc32);
    a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
    a1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1];
    a2 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2];
    a3 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3];
    ra = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA];
    sp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];

    switch (pc32) {
    case UINT32_C(0x8008130C):
        if (!live_handle_ok || a0 != live_handle)
            return;
        switch (a1) {
        case 1u:
            tag = "envt_mode1";
            break;
        case 2u:
            tag = "envt_mode2";
            break;
        case 3u:
            tag = "envt_mode3";
            break;
        default:
            tag = "envt_modeX";
            break;
        }
        break;
    case UINT32_C(0x8008A544):
        if (!live_handle_ok || a0 != live_handle)
            return;
        tag = "envt_kick";
        break;
    case UINT32_C(0x8008406C):
        if (a0 != payload_va)
            return;
        tag = "state4_splice";
        state_ptr = a1;
        break;
    case UINT32_C(0x80085554):
        if (a0 != payload_va)
            return;
        tag = "payload_busy";
        break;
    default:
        return;
    }

    if (live_handle_ok && live_handle != 0u) {
        uint32_t resolved_key = 0;
        uint32_t resolved_entry = 0;
        uint32_t resolved_state_ptr = 0;
        uint32_t resolved_obj = 0;
        bool resolved_state_ok = false;
        unsigned char resolved_state_byte = 0;

        if (resolve_section3_gate_entry(m, live_handle,
            &resolved_key, &resolved_entry, &resolved_state_ptr,
            &resolved_state_ok, &resolved_state_byte, &resolved_obj)) {
            entry_key = resolved_key;
            entry = resolved_entry;
            if (state_ptr == 0u)
                state_ptr = resolved_state_ptr;
            state_ok = resolved_state_ok;
            state_byte = resolved_state_byte;
            obj = resolved_obj;
        }
    }

    if (state_ptr >= UINT32_C(0x80000000) && state_ptr < UINT32_C(0x81000000)) {
        static const uint32_t state_offsets[] = {
            0x00u, 0x04u, 0x08u, 0x0Cu, 0x10u, 0x14u,
        };

        for (i = 0; i < sizeof(state_offsets) / sizeof(state_offsets[0]); i++) {
            state_words_ok[i] = load_va_word(m, state_ptr + state_offsets[i],
                &state_words[i]);
        }
        state18_ok = load_va_bytes(m, state_ptr + 0x18u, &state18, 1u);
        state19_ok = load_va_bytes(m, state_ptr + 0x19u, &state19, 1u);
    }

    if (obj == 0u)
        obj = payload_va;
    if (obj >= UINT32_C(0x80000000) && obj < UINT32_C(0x81000000)) {
        obj88_ok = load_va_word(m, obj + 0x88u, &obj88);
        obj8c_ok = load_va_word(m, obj + 0x8Cu, &obj8c);
        obj90_ok = load_va_word(m, obj + 0x90u, &obj90);
        obj9c_ok = load_va_word(m, obj + 0x9Cu, &obj9c);
    }

    for (i = 0; i < 6u; i++)
        format_word_or_unknown(statew_buf[i], sizeof(statew_buf[i]),
            state_words_ok[i], state_words[i]);
    if (state_ok) {
        snprintf(state5_buf, sizeof(state5_buf), "%02X", state_byte);
    } else {
        snprintf(state5_buf, sizeof(state5_buf), "??");
    }
    if (state18_ok) {
        snprintf(state18_buf, sizeof(state18_buf), "%02X", state18);
    } else {
        snprintf(state18_buf, sizeof(state18_buf), "??");
    }
    if (state19_ok) {
        snprintf(state19_buf, sizeof(state19_buf), "%02X", state19);
    } else {
        snprintf(state19_buf, sizeof(state19_buf), "??");
    }

    sec0 = load_pa_word(m, 0x18C0u);
    sec3 = load_pa_word(m, 0x18CCu);
    fprintf(stderr,
        "[WINCE_TYPE4_STATE] tag=%s pc=0x%08X ra=0x%08X sp=0x%08X"
        " a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X"
        " wrap=0x%08X handle=%s key=0x%08X entry=0x%08X"
        " state=%s state5=%s st0=%s st4=%s st8=%s stc=%s st10=%s st14=%s"
        " st18=%s st19=%s obj=%s obj88=%s obj8c=%s obj90=%s obj9c=%s"
        " sec0=0x%08X sec3=0x%08X\n",
        tag,
        pc32,
        ra,
        sp,
        a0,
        a1,
        a2,
        a3,
        wrap_va,
        format_word_or_unknown(live_handle_buf, sizeof(live_handle_buf),
            live_handle_ok, live_handle),
        entry_key,
        entry,
        format_word_or_unknown(state_ptr_buf, sizeof(state_ptr_buf),
            state_ptr >= UINT32_C(0x80000000) && state_ptr < UINT32_C(0x81000000),
            state_ptr),
        state5_buf,
        statew_buf[0], statew_buf[1], statew_buf[2], statew_buf[3],
        statew_buf[4], statew_buf[5],
        state18_buf,
        state19_buf,
        format_word_or_unknown(obj_buf, sizeof(obj_buf),
            obj >= UINT32_C(0x80000000) && obj < UINT32_C(0x81000000), obj),
        format_word_or_unknown(obj88_buf, sizeof(obj88_buf), obj88_ok, obj88),
        format_word_or_unknown(obj8c_buf, sizeof(obj8c_buf), obj8c_ok, obj8c),
        format_word_or_unknown(obj90_buf, sizeof(obj90_buf), obj90_ok, obj90),
        format_word_or_unknown(obj9c_buf, sizeof(obj9c_buf), obj9c_ok, obj9c),
        sec0,
        sec3);
    dump_section3_wrap_window(m, tag, wrap_va);
    dump_section3_retobj_window(m, tag, payload_va);
    if (state_ptr >= UINT32_C(0x80000000) && state_ptr < UINT32_C(0x81000000))
        dump_va_window(m, tag, state_ptr, 0x20u);
    m->wince.type4_state_probe_count++;
}

static void maybe_note_section3_owner_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32)
{
    static const struct {
        uint32_t pc;
        const char *label;
    } targets[] = {
        { 0x80099924u, "evt_update" },
        { 0x8008406Cu, "owner_link" },
        { 0x80084214u, "owner_state_88" },
        { 0x80084274u, "owner_state_post" },
        { 0x800845C4u, "owner_state_tail" },
        { 0x800845D4u, "owner_state_entry" },
        { 0x800819A4u, "owner_retpath" },
    };
    uint32_t pc32;
    uint32_t sec3;
    size_t i;

    if (!m || !cpu)
        return;

    sec3 = load_pa_word(m, 0x18CCu);
    if (!m->wince.section3_page_watch_armed
        && sec3 != UINT32_C(0x80FE5000))
        return;

    pc32 = canonicalize_nk_pc(raw_pc32);
    for (i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        if (pc32 != targets[i].pc)
            continue;
        if ((m->wince.section3_owner_pc_mask & (UINT32_C(1) << i)) != 0)
            return;
        m->wince.section3_owner_pc_mask |= (UINT32_C(1) << i);
        log_section3_owner_state(m, cpu, targets[i].label, pc32);
        dump_code_window(m, pc32, 8u, 12u);
        if ((uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA] >= 8u) {
            dump_code_window(m,
                canonicalize_nk_pc((uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]) - 8u,
                8u, 12u);
        }
        dump_va_window(m, "sec3_owner_evt97a0", UINT32_C(0x806697A0), 0x40u);
        return;
    }
}

static bool sample_framebuffer(machine_t *m, uint8_t *sample_out)
{
    struct vfb_data *fb;
    const unsigned char *src;
    size_t span;
    size_t copy_len;
    size_t chunk;

    if (!m || !sample_out || !m->gxe_machine)
        return false;

    fb = m->gxe_machine->fb;
    if (!fb || !fb->framebuffer || fb->framebuffer_size == 0)
        return false;

    src = fb->framebuffer;
    memset(sample_out, 0, WINCE_FB_SAMPLE_BYTES);
    copy_len = fb->framebuffer_size < WINCE_FB_SAMPLE_CHUNK_BYTES
        ? fb->framebuffer_size : WINCE_FB_SAMPLE_CHUNK_BYTES;
    span = fb->framebuffer_size > WINCE_FB_SAMPLE_CHUNK_BYTES
        ? fb->framebuffer_size - WINCE_FB_SAMPLE_CHUNK_BYTES : 0;

    for (chunk = 0; chunk < WINCE_FB_SAMPLE_CHUNKS; chunk++) {
        size_t src_off = 0;

        if (WINCE_FB_SAMPLE_CHUNKS > 1 && span != 0) {
            src_off = (span * chunk) / (WINCE_FB_SAMPLE_CHUNKS - 1);
        }
        memcpy(sample_out + chunk * WINCE_FB_SAMPLE_CHUNK_BYTES,
            src + src_off, copy_len);
    }

    return true;
}

static bool toc_name_is_focus(const char *name)
{
    static const char *focus[] = {
        "gwes",
        "explorer",
        "shell",
        "filesys",
        "device",
        "touch",
        "ddi",
        "keybddr",
    };
    size_t i;

    if (!name || name[0] == '\0')
        return false;

    for (i = 0; i < sizeof(focus) / sizeof(focus[0]); i++) {
        if (strstr(name, focus[i]) != NULL)
            return true;
    }
    return false;
}

static void dump_recent_pc_ring(machine_t *m, const char *tag, uint32_t limit)
{
    uint32_t total;
    uint32_t count;
    uint32_t start;
    uint32_t i;

    if (!m || !m->wince.pc_ring_active)
        return;

    total = m->wince.pc_ring_idx;
    if (total == 0) {
        fprintf(stderr, "[PC_RING] %s empty\n", tag ? tag : "recent");
        return;
    }

    count = total < limit ? total : limit;
    start = total - count;
    fprintf(stderr, "[PC_RING] %s last %u of %u samples:\n",
        tag ? tag : "recent", count, total);
    for (i = 0; i < count; i++) {
        uint32_t idx = (start + i) % WINCE_PC_RING_SIZE;
        uint32_t pc = m->wince.pc_ring[idx];
        uint32_t pa = pc & 0x1FFFFFFFu;
        const char *region = "???";

        if (pa >= 0x1FC00000u)
            region = "ROM";
        else if (pa >= 0xF00000u && pa < 0x1000000u)
            region = "SPL";
        else if (pa >= 0x60000u && pa < 0x100000u)
            region = "NK";
        else if (pa < 0x10000u)
            region = "LOW";

        fprintf(stderr,
            "[PC_RING] %s [%2u] PC=0x%08X (%s) SP=0x%08X Status=0x%08X\n",
            tag ? tag : "recent",
            i,
            pc,
            region,
            m->wince.pc_ring_sp[idx],
            m->wince.pc_ring_status[idx]);
    }
}

static void ppsh_finish_sequence(machine_t *m, const char *reason)
{
    if (!m || !m->wince.ppsh_seq_active)
        return;

    ppsh_flow_log(m,
        "[PPSH_SEQ] #%u done reason=%s cmd=0x%04X"
        " status_reads=%u data_reads=%u last_status=0x%04X"
        " last_data=0x%04X sections=%u fb_watch=%u fb_events=%u\n",
        (unsigned)m->wince.ppsh_cmd_seq_count,
        reason ? reason : "unknown",
        (unsigned)m->wince.ppsh_seq_cmd,
        (unsigned)m->wince.ppsh_seq_status_reads,
        (unsigned)m->wince.ppsh_seq_data_reads,
        (unsigned)m->wince.ppsh_seq_last_status,
        (unsigned)m->wince.ppsh_seq_last_data,
        (unsigned)count_active_sections(m),
        m->wince.fb_watch_armed ? 1u : 0u,
        (unsigned)(m->wince.fb_watch_report_count
            + m->wince.fb_write_diag_count));

    m->wince.ppsh_seq_active = false;
    m->wince.ppsh_seq_status_reads = 0;
    m->wince.ppsh_seq_data_reads = 0;
    m->wince.ppsh_seq_read_budget = 0;
    m->wince.ppsh_seq_cmd = 0;
    m->wince.ppsh_seq_start_pc = 0;
    m->wince.ppsh_seq_last_status = 0;
    m->wince.ppsh_seq_last_data = 0;
}

static void ppsh_close_poll_episode(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32, const char *reason)
{
    uint32_t pc32;

    if (!m || !cpu || !m->wince.ppsh_poll_active)
        return;

    pc32 = canonicalize_nk_pc(raw_pc32);
    m->wince.ppsh_poll_active = false;
    m->wince.ppsh_poll_exit_count++;
    m->wince.ppsh_poll_last_iters = m->wince.ppsh_poll_iters;
    m->wince.ppsh_poll_exit_pc = pc32;
    ppsh_flow_log(m,
        "[PPSH_FLOW] poll_exit #%u episode=%u target=0x%08X"
        " iters=%u reason=%s v0=0x%08X ra=0x%08X sp=0x%08X\n",
        (unsigned)m->wince.ppsh_poll_exit_count,
        (unsigned)m->wince.ppsh_poll_episode_count,
        pc32,
        (unsigned)m->wince.ppsh_poll_last_iters,
        reason ? reason : "unknown",
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP]);
    m->wince.ppsh_poll_iters = 0;
    m->wince.ppsh_poll_next_milestone = 0;
}

static uint32_t ppsh_next_poll_milestone(uint32_t current)
{
    if (current < 10u)
        return 10u;
    if (current < 100u)
        return 100u;
    if (current < 1000u)
        return 1000u;
    if (current < 10000u)
        return 10000u;
    if (current < 100000u)
        return 100000u;
    if (current < 500000u)
        return 500000u;
    if (current < 1000000u)
        return 1000000u;
    return current + 1000000u;
}

static void maybe_trace_ppsh_helper_pc(machine_t *m, struct cpu *cpu,
    uint32_t raw_pc32)
{
    uint32_t pc32;

    if (!m || !cpu || !ppsh_trace_enabled(m))
        return;

    pc32 = canonicalize_nk_pc(raw_pc32);

    switch (pc32) {
    case 0x800784F4u:
        m->wince.ppsh_send_entry_count++;
        ppsh_finish_sequence(m, "next_send_entry");
        ppsh_flow_log(m,
            "[PPSH_FLOW] send_command #%u pc=0x%08X ra=0x%08X"
            " sp=0x%08X a0=0x%08X a1=0x%08X\n",
            (unsigned)m->wince.ppsh_send_entry_count,
            raw_pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1]);
        break;
    case 0x8007846Cu:
        m->wince.ppsh_read_entry_count++;
        ppsh_finish_sequence(m, "next_read_entry");
        ppsh_flow_log(m,
            "[PPSH_FLOW] read_response #%u pc=0x%08X ra=0x%08X"
            " sp=0x%08X a0=0x%08X v0=0x%08X\n",
            (unsigned)m->wince.ppsh_read_entry_count,
            raw_pc32,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0]);
        break;
    case 0x800785E8u:
        if (!m->wince.ppsh_poll_active) {
            m->wince.ppsh_poll_active = true;
            m->wince.ppsh_poll_episode_count++;
            m->wince.ppsh_poll_iters = 0;
            m->wince.ppsh_poll_next_milestone = 1u;
            m->wince.ppsh_poll_entry_ra =
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA];
            m->wince.ppsh_poll_entry_sp =
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
            ppsh_flow_log(m,
                "[PPSH_FLOW] poll_start #%u pc=0x%08X ra=0x%08X"
                " sp=0x%08X v0=0x%08X\n",
                (unsigned)m->wince.ppsh_poll_episode_count,
                raw_pc32,
                m->wince.ppsh_poll_entry_ra,
                m->wince.ppsh_poll_entry_sp,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0]);
        }

        m->wince.ppsh_poll_iters++;
        if (m->wince.ppsh_poll_iters == m->wince.ppsh_poll_next_milestone) {
            ppsh_flow_log(m,
                "[PPSH_FLOW] poll_iter #%u episode=%u pc=0x%08X"
                " v0=0x%08X ra=0x%08X sp=0x%08X\n",
                (unsigned)m->wince.ppsh_poll_iters,
                (unsigned)m->wince.ppsh_poll_episode_count,
                raw_pc32,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP]);
            m->wince.ppsh_poll_next_milestone =
                ppsh_next_poll_milestone(m->wince.ppsh_poll_iters);
        }
        break;
    case 0x80078600u:
    case 0x80078604u:
        ppsh_close_poll_episode(m, cpu, raw_pc32, "helper_pc");
        break;
    default:
        break;
    }
}

static void maybe_arm_fb_watch(machine_t *m, struct cpu *cpu,
    const char *reason)
{
    struct vfb_data *fb;

    if (!m || !cpu || !m->wince.active || m->wince.fb_watch_armed
        || !m->gxe_machine) {
        return;
    }

    fb = m->gxe_machine->fb;
    if (!fb || !sample_framebuffer(m, m->wince.fb_watch_baseline))
        return;

    m->wince.fb_watch_armed = true;
    m->wince.fb_watch_baseline_valid = true;
    m->wince.fb_watch_report_count = 0;
    m->wince.fb_write_diag_count = 0;
    m->wince.fb_watch_pc_ring_dumped = false;
    m->wince.fb_watch_arm_pc = (uint32_t)cpu->pc;
    fb->write_observer = wince_fb_write_observer;
    fb->write_observer_opaque = m;

    fprintf(stderr,
        "[WINCE_FB] watch armed reason=%s pc=0x%08X fb_base=0x%08" PRIx64
        " size=%zu sample=%uB\n",
        reason ? reason : "unknown",
        (uint32_t)cpu->pc,
        fb->baseaddr,
        fb->framebuffer_size,
        (unsigned)WINCE_FB_SAMPLE_BYTES);
}

void wince_boot_note_usermode_entry(machine_t *m)
{
    if (!m || !m->cpu || !m->wince.active)
        return;

    m->wince.ppsh_trace_armed = true;
    if (!m->wince.fb_watch_armed)
        maybe_arm_fb_watch(m, m->cpu, "usermode_entry");
}

static void maybe_track_fb_runtime_changes(machine_t *m, struct cpu *cpu)
{
    uint8_t current[WINCE_FB_SAMPLE_BYTES];
    size_t i;

    if (!m || !cpu || !m->wince.fb_watch_armed
        || !m->wince.fb_watch_baseline_valid
        || m->wince.fb_watch_report_count >= WINCE_FB_REPORT_MAX) {
        return;
    }
    if (!sample_framebuffer(m, current))
        return;
    if (memcmp(current, m->wince.fb_watch_baseline,
            WINCE_FB_SAMPLE_BYTES) == 0) {
        return;
    }

    for (i = 0; i < WINCE_FB_SAMPLE_BYTES; i++) {
        if (current[i] != m->wince.fb_watch_baseline[i]) {
            m->wince.fb_watch_report_count++;
            fprintf(stderr,
                "[WINCE_FB] sample_change #%u pc=0x%08X ra=0x%08X"
                " sp=0x%08X diff=%zu old=%02X new=%02X\n",
                (unsigned)m->wince.fb_watch_report_count,
                (uint32_t)cpu->pc,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
                i,
                m->wince.fb_watch_baseline[i],
                current[i]);
            if (!m->wince.fb_watch_pc_ring_dumped) {
                dump_recent_pc_ring(m, "fb_sample", WINCE_FB_PC_RING_LIMIT);
                m->wince.fb_watch_pc_ring_dumped = true;
            }
            memcpy(m->wince.fb_watch_baseline, current,
                WINCE_FB_SAMPLE_BYTES);
            break;
        }
    }
}

static void wince_fb_write_observer(struct vfb_data *fb, struct cpu *cpu,
    void *opaque, uint64_t relative_addr, size_t len)
{
    machine_t *m = (machine_t *)opaque;

    if (!fb || !cpu || !m || !m->wince.active || !m->wince.fb_watch_armed)
        return;

    if (m->wince.fb_write_diag_count >= WINCE_FB_REPORT_MAX) {
        fb->write_observer = NULL;
        fb->write_observer_opaque = NULL;
        return;
    }

    m->wince.fb_write_diag_count++;
    fprintf(stderr,
        "[WINCE_FB] direct_write #%u paddr=0x%08" PRIx64
        " off=0x%08" PRIx64 " len=%zu pc=0x%08X ra=0x%08X\n",
        (unsigned)m->wince.fb_write_diag_count,
        fb->baseaddr + relative_addr,
        relative_addr,
        len,
        (uint32_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
    if (!m->wince.fb_watch_pc_ring_dumped) {
        dump_recent_pc_ring(m, "fb_write", WINCE_FB_PC_RING_LIMIT);
        m->wince.fb_watch_pc_ring_dumped = true;
    }

    if (m->wince.fb_write_diag_count >= WINCE_FB_REPORT_MAX) {
        fb->write_observer = NULL;
        fb->write_observer_opaque = NULL;
    }
}

static void maybe_dump_toc_summary(machine_t *m, uint32_t ptoc)
{
    uint32_t physfirst = 0;
    uint32_t physlast = 0;
    uint32_t nummods = 0;
    uint32_t numfiles = 0;
    uint32_t ramstart = 0;
    uint32_t ramfree = 0;
    uint32_t ramend = 0;
    uint32_t copy_entries = 0;
    uint32_t copy_offset = 0;
    uint32_t mods_base;
    uint32_t files_base;
    uint32_t i;
    uint32_t logged_mods = 0;
    uint32_t logged_files = 0;

    if (!m || m->wince.toc_dumped || ptoc == 0)
        return;

    m->wince.toc_dumped = true;
    if (!load_va_word(m, ptoc + 0x08u, &physfirst)
        || !load_va_word(m, ptoc + 0x0Cu, &physlast)
        || !load_va_word(m, ptoc + WINCE_ROMHDR_NMODS_OFF, &nummods)
        || !load_va_word(m, ptoc + WINCE_ROMHDR_NUMFILES_OFF, &numfiles)
        || !load_va_word(m, ptoc + WINCE_ROMHDR_RAMSTART_OFF, &ramstart)
        || !load_va_word(m, ptoc + WINCE_ROMHDR_RAMFREE_OFF, &ramfree)
        || !load_va_word(m, ptoc + WINCE_ROMHDR_RAMEND_OFF, &ramend)
        || !load_va_word(m, ptoc + WINCE_ROMHDR_COPYENTRIES_OFF, &copy_entries)
        || !load_va_word(m, ptoc + WINCE_ROMHDR_COPYOFFSET_OFF, &copy_offset)) {
        fprintf(stderr, "[WINCE_TOC] unable to read ROMHDR at pTOC=0x%08X\n",
            ptoc);
        return;
    }

    fprintf(stderr,
        "[WINCE_TOC] pTOC=0x%08X physfirst=0x%08X physlast=0x%08X"
        " nummods=%u numfiles=%u ramstart=0x%08X ramfree=0x%08X"
        " ramend=0x%08X copy_entries=%u copy_offset=0x%08X\n",
        ptoc, physfirst, physlast, nummods, numfiles, ramstart, ramfree,
        ramend, copy_entries, copy_offset);

    if (nummods == 0 || nummods > 256 || numfiles > 256)
        return;

    mods_base = ptoc + WINCE_ROMHDR_MODTABLE_OFF;
    files_base = mods_base + nummods * WINCE_TOCENTRY_SIZE;

    for (i = 0; i < nummods; i++) {
        uint32_t entry = mods_base + i * WINCE_TOCENTRY_SIZE;
        uint32_t attrs = 0;
        uint32_t size = 0;
        uint32_t name_ptr = 0;
        uint32_t load_addr = 0;
        char name[96];
        bool focus;

        if (!load_va_word(m, entry + 0x00u, &attrs)
            || !load_va_word(m, entry + 0x0Cu, &size)
            || !load_va_word(m, entry + 0x10u, &name_ptr)
            || !load_va_word(m, entry + 0x1Cu, &load_addr)
            || !load_guest_c_string(m, name_ptr, name, sizeof(name))) {
            continue;
        }

        focus = toc_name_is_focus(name);
        if (i >= 8 && !focus)
            continue;
        if (logged_mods >= 20 && !focus)
            continue;

        fprintf(stderr,
            "[WINCE_TOC] mod[%u] attr=0x%08X size=0x%08X load=0x%08X"
            " name=%s\n",
            i, attrs, size, load_addr, name);
        logged_mods++;
    }

    for (i = 0; i < numfiles; i++) {
        uint32_t entry = files_base + i * WINCE_FILEENTRY_SIZE;
        uint32_t attrs = 0;
        uint32_t size = 0;
        uint32_t csize = 0;
        uint32_t name_ptr = 0;
        char name[96];
        bool focus;

        if (!load_va_word(m, entry + 0x00u, &attrs)
            || !load_va_word(m, entry + 0x0Cu, &size)
            || !load_va_word(m, entry + 0x10u, &csize)
            || !load_va_word(m, entry + 0x14u, &name_ptr)
            || !load_guest_c_string(m, name_ptr, name, sizeof(name))) {
            continue;
        }

        focus = toc_name_is_focus(name);
        if (i >= 6 && !focus)
            continue;
        if (logged_files >= 12 && !focus)
            continue;

        fprintf(stderr,
            "[WINCE_TOC] file[%u] attr=0x%08X size=0x%08X csize=0x%08X"
            " name=%s\n",
            i, attrs, size, csize, name);
        logged_files++;
    }
}

static bool try_discover_ptoc(machine_t *m, uint32_t *ptoc_out)
{
    static const uint32_t nk_bases[] = {
        0x80060000u,
        0x80029000u,
    };
    size_t i;

    if (!m || !ptoc_out)
        return false;

    for (i = 0; i < sizeof(nk_bases) / sizeof(nk_bases[0]); i++) {
        uint32_t sig = 0;
        uint32_t ptoc = 0;
        uint32_t physfirst = 0;
        uint32_t physlast = 0;

        if (!load_va_word(m, nk_bases[i] + 0x40u, &sig) || sig != 0x43454345u)
            continue;
        if (!load_va_word(m, nk_bases[i] + 0x44u, &ptoc))
            continue;
        if (!load_va_word(m, ptoc + 0x08u, &physfirst)
            || !load_va_word(m, ptoc + 0x0Cu, &physlast)) {
            continue;
        }
        if (physfirst != nk_bases[i] || physlast <= physfirst)
            continue;

        *ptoc_out = ptoc;
        return true;
    }

    return false;
}

static void log_l2_table_state(machine_t *m, const char *tag,
    uint32_t table_va, uint32_t focus_vaddr)
{
    uint32_t focus_off;

    if (!m || table_va == 0)
        return;

    focus_off = (focus_vaddr != 0)
        ? ((focus_vaddr >> 14) & 0x7FCu)
        : 0x694u;

    fprintf(stderr,
        "[WINCE_L2_STATE] %s table=0x%08X pa=0x%08X"
        " focus_va=0x%08X focus_off=0x%03X focus=0x%08X"
        " dll600=0x%08X dll694=0x%08X dll7f8=0x%08X\n",
        tag ? tag : "-",
        table_va,
        table_va_to_pa(table_va),
        focus_vaddr,
        focus_off,
        load_table_word(m, table_va, focus_off),
        load_table_word(m, table_va, 0x600u),
        load_table_word(m, table_va, 0x694u),
        load_table_word(m, table_va, 0x7F8u));
}

static void log_section0_focus_window(machine_t *m, const char *tag,
    uint32_t table_va)
{
    if (!m || table_va == 0)
        return;

    fprintf(stderr,
        "[WINCE_SECTION0_FOCUS] %s table=0x%08X pa=0x%08X"
        " slot694=0x%08X slot7e0=0x%08X slot7e4=0x%08X slot7e8=0x%08X\n",
        tag ? tag : "-",
        table_va,
        table_va_to_pa(table_va),
        load_table_word(m, table_va, 0x694u),
        load_table_word(m, table_va, 0x7E0u),
        load_table_word(m, table_va, 0x7E4u),
        load_table_word(m, table_va, 0x7E8u));
}

static void maybe_log_tlb_table_write(machine_t *m, struct cpu *cpu,
    uint64_t paddr, uint64_t len, uint64_t val)
{
    uint32_t p32 = (uint32_t)paddr;
    uint32_t value = (uint32_t)val;

    if (!m || !cpu || len != 4)
        return;

    if (paddr >= 0x000018C0u && paddr + len <= 0x000019C0u) {
        unsigned idx = (unsigned)((p32 - 0x18C0u) >> 2);
        uint32_t old = m->wince.section_table_shadow[idx];

        if (m->wince.section_write_diag_count < 96) {
            fprintf(stderr,
                "[WINCE_SECTION_WRITE] idx=%u old=0x%08X"
                " new=0x%08X PC=0x%08X RA=0x%08X\n",
                idx,
                old,
                value,
                (uint32_t)cpu->pc,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
            m->wince.section_write_diag_count++;
        }

        m->wince.section_table_shadow[idx] = value;

        if (value != 0 && value != 0x8008BC18u && idx < 2) {
            uint32_t marker = load_table_word(m, value, 0x694u);
            const char *kind = marker == 1u ? "shared" : "process";
            if (marker == 1u) {
                m->wince.diag_shared_l2_table = value;
            } else if (value != m->wince.diag_shared_l2_table) {
                m->wince.diag_process_l2_table = value;
            }
            if ((old == 0 || old == 0x8008BC18u)
                && m->wince.process_map_diag_count < 32) {
                fprintf(stderr,
                    "[WINCE_PROCESS] section[%u] kind=%s table=0x%08X"
                    " marker=0x%08X PC=0x%08X RA=0x%08X\n",
                    idx,
                    kind,
                    value,
                    marker,
                    (uint32_t)cpu->pc,
                    (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
                m->wince.process_map_diag_count++;
            }
            if (m->wince.section_write_diag_count <= 96)
                log_l2_table_state(m, "section_write", value,
                    idx == 0 ? 0x01A50000u : 0x0201FF00u);
            if (idx == 0
                && (value == UINT32_C(0x80FE5000)
                    || old == UINT32_C(0x80FE5000))
                && m->wince.section0_focus_diag_count < 16) {
                uint32_t pc32 = canonicalize_nk_pc((uint32_t)cpu->pc);
                fprintf(stderr,
                    "[WINCE_SECTION0_TABLE] old=0x%08X new=0x%08X"
                    " PC=0x%08X RA=0x%08X\n",
                    old,
                    value,
                    (uint32_t)cpu->pc,
                    (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
                if (old == UINT32_C(0x80FE5000)) {
                    log_l2_table_state(m, "section0_old", old,
                        UINT32_C(0x01F94B50));
                    log_section0_focus_window(m, "section0_old", old);
                }
                if (value == UINT32_C(0x80FE5000)) {
                    log_l2_table_state(m, "section0_new", value,
                        UINT32_C(0x01F94B50));
                    log_section0_focus_window(m, "section0_new", value);
                }
                if (pc32 == UINT32_C(0x8008B594)
                    || pc32 == UINT32_C(0x8008B0DC)) {
                    maybe_note_section0_source_pc(m, cpu, pc32);
                }
                m->wince.section0_focus_diag_count++;
            }
            if (m->wince.systempatch_seen)
                maybe_log_systempatch_context(m, "section_write");
        }

        if (idx == 3
            && (value == UINT32_C(0x80FE5000)
                || old == UINT32_C(0x80FE5000))
            && m->wince.section3_focus_diag_count < 16) {
            if (value == UINT32_C(0x80FE5000) && old != value) {
                m->wince.section3_page_watch_armed = true;
                m->wince.section3_page_diag_count = 0;
                m->wince.section3_desc_write_count = 0;
                m->wince.section3_desc_read_count = 0;
                m->wince.section3_pool_write_count = 0;
                m->wince.section3_retobj_watch_armed = false;
                m->wince.section3_retobj_watch_va = 0;
                m->wince.section3_retobj_write_count = 0;
                m->wince.section3_callback_probe_count = 0;
                m->wince.section3_callback_pc_mask = 0;
                if (!m->wince.section3_step_trace_done
                    && !m->wince.section3_step_trace_active) {
                    m->wince.section3_step_trace_pending = false;
                    m->wince.section3_step_trace_active = true;
                    m->wince.section3_step_trace_remaining = 96;
                    single_step = true;
                }
                fprintf(stderr,
                    "[WINCE_SEC3_ARM] state=armed old=0x%08X new=0x%08X"
                    " PC=0x%08X RA=0x%08X trace=%s rem=%u\n",
                    old,
                    value,
                    (uint32_t)cpu->pc,
                    (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
                    m->wince.section3_step_trace_active ? "active" : "pending",
                    (unsigned)m->wince.section3_step_trace_remaining);
            } else if (old == UINT32_C(0x80FE5000) && value != old) {
                m->wince.section3_page_watch_armed = false;
                m->wince.section3_retobj_watch_armed = false;
                m->wince.section3_retobj_watch_va = 0;
                m->wince.section3_pool_write_count = 0;
                m->wince.section3_step_trace_pending = false;
                fprintf(stderr,
                    "[WINCE_SEC3_ARM] state=disarmed old=0x%08X new=0x%08X"
                    " PC=0x%08X RA=0x%08X\n",
                    old,
                    value,
                    (uint32_t)cpu->pc,
                    (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
            }
            fprintf(stderr,
                "[WINCE_SECTION3_TABLE] old=0x%08X new=0x%08X"
                " PC=0x%08X RA=0x%08X\n",
                old,
                value,
                (uint32_t)cpu->pc,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
            if (old == UINT32_C(0x80FE5000)) {
                log_l2_table_state(m, "section3_old", old,
                    UINT32_C(0x01F94B50));
                log_section0_focus_window(m, "section3_old", old);
            }
            if (value == UINT32_C(0x80FE5000)) {
                log_l2_table_state(m, "section3_new", value,
                    UINT32_C(0x01F94B50));
                log_section0_focus_window(m, "section3_new", value);
            }
            m->wince.section3_focus_diag_count++;
        }
    }

    for (int which = 0; which < 2; which++) {
        uint32_t table_va = which == 0 ? m->wince.diag_shared_l2_table
            : m->wince.diag_process_l2_table;
        const char *table_name = which == 0 ? "shared" : "process";
        uint32_t table_pa;
        uint32_t off;

        if (table_va == 0)
            continue;

        table_pa = table_va_to_pa(table_va);
        if (!range_overlaps(paddr, len, table_pa + 0x600u, 0x200u))
            continue;

        off = p32 - table_pa;
        if (m->wince.l2_write_diag_count < 96) {
            fprintf(stderr,
                "[WINCE_L2_WRITE] table=%s table_va=0x%08X"
                " off=0x%03X new=0x%08X PC=0x%08X RA=0x%08X\n",
                table_name,
                table_va,
                off,
                value,
                (uint32_t)cpu->pc,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
            m->wince.l2_write_diag_count++;
        }
    }

    if (range_overlaps(paddr, len, WINCE_HOT_USER_L2_TABLE_PA,
            WINCE_HOT_USER_L2_TRACE_BYTES)) {
        unsigned n;
        m->wince.hot_user_l2_write_count++;
        n = (unsigned)m->wince.hot_user_l2_write_count;
        if (n <= 96u) {
            fprintf(stderr,
                "[WINCE_HOT_L2W] #%u off=0x%02" PRIx64 " len=%" PRIu64
                " val=0x%08X pc=0x%08X ra=0x%08X\n",
                n,
                paddr - WINCE_HOT_USER_L2_TABLE_PA,
                len,
                value,
                (uint32_t)cpu->pc,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
            log_hot_user_l2_state(m, "write");
        }
        if (!m->wince.hot_l2w_probe_dumped) {
            m->wince.hot_l2w_probe_dumped = true;
            fprintf(stderr,
                "[WINCE_L2W_PROBE] candidate store sites"
                " (dyntrans-stale pc=0x80097000, epc=0x8009004C BD=1):\n");
            dump_code_window(m, UINT32_C(0x8009004C), 2u, 4u);
            dump_code_window(m, UINT32_C(0x80097000), 4u, 8u);
            dump_code_window(m, UINT32_C(0x800971C0), 6u, 4u);
            fprintf(stderr,
                "[WINCE_L2W_PROBE] outer caller around 0x8009813C"
                " (Phase H target predicate):\n");
            dump_code_window(m, UINT32_C(0x80098140), 20u, 16u);
            fprintf(stderr,
                "[WINCE_L2W_PROBE] outer caller prologue 0x800980C0"
                " (entry of function containing 0x8009813C):\n");
            dump_code_window(m, UINT32_C(0x80098000), 0u, 48u);
            fprintf(stderr,
                "[WINCE_L2W_PROBE] Phase I: real prologue window"
                " 0x80097FC0..0x80098000:\n");
            dump_code_window(m, UINT32_C(0x80097FE0), 8u, 8u);
            fprintf(stderr,
                "[WINCE_L2W_PROBE] Phase I: helper 0x80096E88 (v0"
                " gates second walker):\n");
            dump_code_window(m, UINT32_C(0x80096E88), 0u, 24u);
            fprintf(stderr,
                "[WINCE_L2W_PROBE] Phase J: real function entry"
                " 0x80097F80..0x80097FC4:\n");
            dump_code_window(m, UINT32_C(0x80097FA0), 8u, 8u);
            fprintf(stderr,
                "[WINCE_L2W_PROBE] Phase K: caller frame A"
                " 0x8008DA00..0x8008DA24:\n");
            dump_code_window(m, UINT32_C(0x8008DA10), 4u, 4u);
            fprintf(stderr,
                "[WINCE_L2W_PROBE] Phase K: caller frame B1"
                " 0x8008DEA0..0x8008DEB8:\n");
            dump_code_window(m, UINT32_C(0x8008DEA8), 2u, 4u);
            fprintf(stderr,
                "[WINCE_L2W_PROBE] Phase K: caller frame B2"
                " 0x8008DE30..0x8008DE48:\n");
            dump_code_window(m, UINT32_C(0x8008DE3C), 3u, 3u);
            fprintf(stderr,
                "[WINCE_L2W_PROBE] Phase K: va-to-section helper"
                " 0x800998C0:\n");
            dump_code_window(m, UINT32_C(0x800998C0), 0u, 16u);
            fprintf(stderr,
                "[WINCE_L2W_PROBE] Phase L: outer function body"
                " around 0x8008DA00 (-0x40..+0x20):\n");
            dump_code_window(m, UINT32_C(0x8008DA00), 16u, 8u);
            fprintf(stderr,
                "[WINCE_L2W_PROBE] Phase L: outer function prologue"
                " search window 0x8008D900..0x8008D9C0:\n");
            dump_code_window(m, UINT32_C(0x8008D960), 24u, 24u);
            fprintf(stderr,
                "[WINCE_L2W_PROBE] Phase L: extended body"
                " 0x8008DA20..0x8008DAA0:\n");
            dump_code_window(m, UINT32_C(0x8008DA60), 16u, 16u);
            fprintf(stderr,
                "[WINCE_L2W_PROBE] Phase M: coordinator prologue"
                " 0x8008D800..0x8008D900:\n");
            dump_code_window(m, UINT32_C(0x8008D860), 24u, 24u);
            fprintf(stderr,
                "[WINCE_L2W_PROBE] Phase M: query/verify 0x80098180"
                " (a2=0x1000 entry gating teardown flag):\n");
            dump_code_window(m, UINT32_C(0x80098180), 0u, 32u);
            fprintf(stderr,
                "[WINCE_L2W_PROBE] Phase M: sibling MM primitive"
                " 0x80097844:\n");
            dump_code_window(m, UINT32_C(0x80097844), 0u, 24u);
            fprintf(stderr,
                "[WINCE_L2W_PROBE] Phase N: shared address resolver"
                " 0x80096CC8:\n");
            dump_code_window(m, UINT32_C(0x80096CC8), 0u, 28u);
            fprintf(stderr,
                "[WINCE_L2W_PROBE] Phase P: real opcode handlers"
                " 0x80096D40..0x80096D80:\n");
            dump_code_window(m, UINT32_C(0x80096D60), 8u, 8u);
            fprintf(stderr,
                "[WINCE_L2W_PROBE] Phase R: verify helper body"
                " 0x80096E88..0x80096FE0:\n");
            dump_code_window(m, UINT32_C(0x80096F30), 42u, 42u);
            fprintf(stderr,
                "[WINCE_L2W_PROBE] Phase S: allocator 0x800A1134"
                " body (first 32 words):\n");
            dump_code_window(m, UINT32_C(0x800A1134), 0u, 32u);
            fprintf(stderr,
                "[WINCE_L2W_PROBE] Phase T: pool descriptor table"
                " 0x806600B8..0x80660200 (20 descriptors):\n");
            {
                unsigned p;
                for (p = 0; p < 20u; p++) {
                    uint32_t base = UINT32_C(0x806600B8)
                                    + (uint32_t)(p * 20u);
                    uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0, w4 = 0;
                    (void)load_va_word(m, base + 0x00u, &w0);
                    (void)load_va_word(m, base + 0x04u, &w1);
                    (void)load_va_word(m, base + 0x08u, &w2);
                    (void)load_va_word(m, base + 0x0Cu, &w3);
                    (void)load_va_word(m, base + 0x10u, &w4);
                    fprintf(stderr,
                        "[WINCE_POOL] pool=%u base=0x%08X"
                        " +0=0x%08X +4=0x%08X +8=0x%08X"
                        " +C=0x%08X +10=0x%08X\n",
                        p, base, w0, w1, w2, w3, w4);
                }
            }
            fprintf(stderr,
                "[WINCE_L2W_PROBE] Phase O: opcode jump table"
                " 0x80075714..0x80075794 (32 entries):\n");
            {
                unsigned k;
                for (k = 0; k < 32u; k++) {
                    uint32_t v = 0;
                    uint32_t va = UINT32_C(0x80075714)
                                  + (uint32_t)(k * 4u);
                    bool ok = load_va_word(m, va, &v);
                    fprintf(stderr,
                        "[WINCE_JUMPTBL] opcode=%u va=0x%08X"
                        " handler=0x%08X%s\n",
                        k + 1u, va, v, ok ? "" : " (unmapped)");
                }
            }
        }
        if (n == 30u) {
            /* Phase Y: at the first walker zero-store (#30 = first
             * PTE zeroed by FUN_800970A8), read the saved register
             * frame from the walker's stack. The walker's prologue
             * at 0x800970A8..0x800970E0 stores:
             *   sp+0x18: s0
             *   sp+0x1C: s1
             *   sp+0x20: s2
             *   sp+0x24: s3
             *   sp+0x28: s4
             *   sp+0x2C: s5
             *   sp+0x30: s6
             *   sp+0x34: s7
             *   sp+0x38: s8
             *   sp+0x3C: ra  ← THE REAL CALLER PC
             * before any callee work happens. By #30 the walker is
             * deep in its loop and ra has been clobbered by jal
             * 0x8007EA74, but the saved frame is intact. */
            uint32_t spv = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
            uint32_t saved_s0 = 0, saved_s1 = 0, saved_s2 = 0;
            uint32_t saved_s3 = 0, saved_s4 = 0, saved_s5 = 0;
            uint32_t saved_s6 = 0, saved_s7 = 0, saved_s8 = 0;
            uint32_t saved_ra = 0;
            (void)load_va_word(m, spv + 0x18u, &saved_s0);
            (void)load_va_word(m, spv + 0x1Cu, &saved_s1);
            (void)load_va_word(m, spv + 0x20u, &saved_s2);
            (void)load_va_word(m, spv + 0x24u, &saved_s3);
            (void)load_va_word(m, spv + 0x28u, &saved_s4);
            (void)load_va_word(m, spv + 0x2Cu, &saved_s5);
            (void)load_va_word(m, spv + 0x30u, &saved_s6);
            (void)load_va_word(m, spv + 0x34u, &saved_s7);
            (void)load_va_word(m, spv + 0x38u, &saved_s8);
            (void)load_va_word(m, spv + 0x3Cu, &saved_ra);
            fprintf(stderr,
                "[WINCE_WALKER_FRAME] sp=0x%08X saved_ra=0x%08X"
                " (real caller PC) callsite=0x%08X\n",
                spv, saved_ra,
                saved_ra >= 8u ? saved_ra - 8u : 0u);
            fprintf(stderr,
                "[WINCE_WALKER_FRAME] saved s0=0x%08X s1=0x%08X"
                " s2=0x%08X s3=0x%08X s4=0x%08X s5=0x%08X"
                " s6=0x%08X s7=0x%08X s8=0x%08X\n",
                saved_s0, saved_s1, saved_s2, saved_s3,
                saved_s4, saved_s5, saved_s6, saved_s7, saved_s8);

            /* Phase Z: walk one frame up to find FUN_80097F44's
             * caller. Walker's sp = walker frame top, FUN_80097F44's
             * sp during body = walker_sp + 0x40 (walker frame size),
             * its saved ra is at sp+0x1C per its epilogue. The
             * caller of FUN_80097F44 then enters at saved_ra-8. */
            {
                uint32_t f44_sp = spv + 0x40u;
                uint32_t f44_ra = 0;
                uint32_t f44_s0 = 0, f44_s1 = 0;
                (void)load_va_word(m, f44_sp + 0x1Cu, &f44_ra);
                (void)load_va_word(m, f44_sp + 0x18u, &f44_s0);
                /* Try to peek the caller's saved frame too. */
                uint32_t caller_sp = f44_sp + 0x50u;
                uint32_t caller_ra_guess[4] = {0};
                unsigned k;
                for (k = 0; k < 4u; k++) {
                    (void)load_va_word(m,
                        caller_sp + 0x10u + (uint32_t)(k * 4u),
                        &caller_ra_guess[k]);
                }
                fprintf(stderr,
                    "[WINCE_F44_FRAME] f44_sp=0x%08X saved_ra=0x%08X"
                    " (real F44 caller PC) callsite=0x%08X"
                    " saved_s0=0x%08X\n",
                    f44_sp, f44_ra,
                    f44_ra >= 8u ? f44_ra - 8u : 0u,
                    f44_s0);
                fprintf(stderr,
                    "[WINCE_F44_FRAME] caller_sp=0x%08X"
                    " caller_stk[0x10]=0x%08X [0x14]=0x%08X"
                    " [0x18]=0x%08X [0x1C]=0x%08X\n",
                    caller_sp,
                    caller_ra_guess[0], caller_ra_guess[1],
                    caller_ra_guess[2], caller_ra_guess[3]);

                /* Phase Z+: FUN_8008FE8C stores the module
                 * descriptor pointer at its sp+0x20, which is at
                 * caller_sp+0x20 = 0x0201FD50 in our case. Read
                 * the module descriptor and key fields. */
                {
                    uint32_t mod_desc_ptr = 0;
                    (void)load_va_word(m, caller_sp + 0x20u,
                        &mod_desc_ptr);
                    fprintf(stderr,
                        "[WINCE_MODULE] descriptor_ptr=0x%08X\n",
                        mod_desc_ptr);
                    /* Phase AE: read the kernel error code field
                     * at *(_DAT_FFFFDAC0 + 0x38). _DAT_FFFFDAC0 is
                     * loaded from VA 0xFFFFDAC0 (per FUN_800927CC).
                     * That VA is the ctx_ptr at PA 0x000018C0 per
                     * CLAUDE.md MM notes. */
                    {
                        uint32_t ctx_ptr = 0;
                        uint32_t err_code = 0;
                        (void)load_va_word(m, UINT32_C(0xFFFFDAC0),
                            &ctx_ptr);
                        if (ctx_ptr != 0) {
                            (void)load_va_word(m, ctx_ptr + 0x38u,
                                &err_code);
                        }
                        const char *err_name = "?";
                        if (err_code == 0x45A) err_name = "DLL_INIT_FAILED";
                        else if (err_code == 0x0E) err_name = "NOT_ENOUGH_MEMORY";
                        else if (err_code == 0x57) err_name = "'W' MM dispatcher";
                        else if (err_code == 0x32) err_name = "'2' opcode-disabled";
                        else if (err_code == 0x08) err_name = "alloc-pool-fail";
                        else if (err_code == 0) err_name = "(clean / no error)";
                        fprintf(stderr,
                            "[WINCE_KERNEL_ERR] ctx_ptr=0x%08X"
                            " err@+0x38=0x%08X (%s)\n",
                            ctx_ptr, err_code, err_name);
                    }
                    if (mod_desc_ptr != 0
                        && (mod_desc_ptr & 3u) == 0) {
                        uint32_t md0 = 0, md4 = 0, md8 = 0, mdC = 0;
                        uint32_t md54 = 0, md80 = 0, mdC0 = 0, mdCC = 0;
                        (void)load_va_word(m, mod_desc_ptr + 0x00u, &md0);
                        (void)load_va_word(m, mod_desc_ptr + 0x04u, &md4);
                        (void)load_va_word(m, mod_desc_ptr + 0x08u, &md8);
                        (void)load_va_word(m, mod_desc_ptr + 0x0Cu, &mdC);
                        (void)load_va_word(m, mod_desc_ptr + 0x54u, &md54);
                        (void)load_va_word(m, mod_desc_ptr + 0x80u, &md80);
                        (void)load_va_word(m, mod_desc_ptr + 0xC0u, &mdC0);
                        (void)load_va_word(m, mod_desc_ptr + 0xCCu, &mdCC);
                        fprintf(stderr,
                            "[WINCE_MODULE] +0=0x%08X +4=0x%08X +8=0x%08X"
                            " +C=0x%08X (lpName?)\n",
                            md0, md4, md8, mdC);
                        fprintf(stderr,
                            "[WINCE_MODULE] +54=0x%08X (base_va)"
                            " +80=0x%08X +C0=0x%08X (depmask)"
                            " +CC=0x%08X (chain)\n",
                            md54, md80, mdC0, mdCC);
                        /* If md+0xC looks like a kseg0 string ptr,
                         * dump the first 32 bytes as ASCII. */
                        if (mdC >= 0x80000000u && mdC < 0x80800000u) {
                            unsigned k;
                            char nbuf[33] = {0};
                            for (k = 0; k < 32u; k++) {
                                uint32_t w = 0;
                                if (load_va_word(m, mdC + k, &w)) {
                                    nbuf[k] = (char)(w & 0xFF);
                                } else {
                                    nbuf[k] = '?';
                                }
                                if (nbuf[k] == 0) break;
                            }
                            fprintf(stderr,
                                "[WINCE_MODULE] name@0x%08X='%.32s'\n",
                                mdC, nbuf);
                        }
                        /* Phase AB: dump descriptor fields and
                         * try to identify the failing DLL via
                         * +0x60 (DllMain entry per FUN_800927CC). */
                        {
                            unsigned k;
                            uint32_t dll_main = 0;
                            for (k = 0; k < 64u; k++) {
                                uint32_t v = 0;
                                uint32_t va = mod_desc_ptr
                                              + (uint32_t)(k * 4u);
                                bool ok2 = load_va_word(m, va, &v);
                                fprintf(stderr,
                                    "[WINCE_MODULE_FIELD] +0x%02X=0x%08X%s\n",
                                    k * 4u, v,
                                    ok2 ? "" : " (unmapped)");
                            }
                            (void)load_va_word(m, mod_desc_ptr + 0x60u,
                                &dll_main);
                            fprintf(stderr,
                                "[WINCE_MODULE] DllMain entry"
                                " 0x%08X\n", dll_main);
                            /* Phase AC: read the TOC entry at
                             * +0x64 to extract the DLL name.
                             * Try both 24-byte and 32-byte layouts:
                             * lpszFileName is typically at offset
                             * 0x0C (24-byte form) or 0x10 (32-byte
                             * form). Dump 8 words and we can pick
                             * the right one. */
                            uint32_t toc_va = 0;
                            (void)load_va_word(m, mod_desc_ptr + 0x64u,
                                &toc_va);
                            if (toc_va >= UINT32_C(0x80060000)
                                && toc_va < UINT32_C(0x80700000)) {
                                unsigned tk;
                                fprintf(stderr,
                                    "[WINCE_TOC] entry_va=0x%08X\n",
                                    toc_va);
                                for (tk = 0; tk < 8u; tk++) {
                                    uint32_t v = 0;
                                    (void)load_va_word(m,
                                        toc_va + (uint32_t)(tk * 4u),
                                        &v);
                                    fprintf(stderr,
                                        "[WINCE_TOC] +0x%02X=0x%08X\n",
                                        tk * 4u, v);
                                }
                                /* Try lpszFileName at offsets 0x0C
                                 * and 0x10 - whichever is a kseg0
                                 * string pointer wins. */
                                uint32_t name_ptrs[2] = {0};
                                (void)load_va_word(m, toc_va + 0x0Cu,
                                    &name_ptrs[0]);
                                (void)load_va_word(m, toc_va + 0x10u,
                                    &name_ptrs[1]);
                                int j;
                                for (j = 0; j < 2; j++) {
                                    uint32_t np = name_ptrs[j];
                                    if (np >= UINT32_C(0x80060000)
                                        && np < UINT32_C(0x80700000)) {
                                        char nb[64] = {0};
                                        unsigned ci;
                                        for (ci = 0; ci < 63u; ci++) {
                                            uint32_t w = 0;
                                            if (!load_va_word(m,
                                                np + ci, &w)) break;
                                            nb[ci] = (char)(w & 0xFF);
                                            if (nb[ci] == 0) break;
                                        }
                                        fprintf(stderr,
                                            "[WINCE_TOC] name_at_+0x%X"
                                            " ptr=0x%08X str='%s'\n",
                                            (j == 0 ? 0x0C : 0x10),
                                            np, nb);
                                    }
                                }
                            }
                        }
                    }
                }
                /* Phase AA: dump 48 words of stack from
                 * caller_sp upward to find FUN_80090144's
                 * saved ra and its caller's saved ra etc.
                 * Look for words in [0x80060000, 0x80700000)
                 * that look like return addresses. */
                {
                    unsigned k;
                    fprintf(stderr,
                        "[WINCE_STACK_WALK] base=0x%08X (FUN_8008FE8C body sp)\n",
                        caller_sp);
                    for (k = 0; k < 48u; k++) {
                        uint32_t v = 0;
                        uint32_t va = caller_sp + (uint32_t)(k * 4u);
                        bool ok = load_va_word(m, va, &v);
                        const char *tag = "";
                        if (ok && v >= UINT32_C(0x80060000)
                            && v < UINT32_C(0x80700000)) {
                            tag = " <- ra-shape";
                        }
                        fprintf(stderr,
                            "[WINCE_STACK_WALK] sp+0x%02X va=0x%08X"
                            " val=0x%08X%s\n",
                            k * 4u, va, v, tag);
                    }
                }
            }
        }
        if (n >= 25u && n <= 40u) {
            struct mips_coproc *cp0 = cpu->cd.mips.coproc[0];
            uint32_t pc32 = (uint32_t)cpu->pc;
            uint32_t spv = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
            uint32_t ret_offs[4] = {0};
            uint32_t ret_addrs[4] = {0};
            size_t rc = collect_stack_return_sites(m, spv,
                ret_offs, ret_addrs,
                sizeof(ret_addrs) / sizeof(ret_addrs[0]), 0x40u);
            size_t k;

            fprintf(stderr,
                "[WINCE_HOT_L2W_REGS] #%u pc=0x%08X epc=0x%08X"
                " status=0x%08X cause=0x%08X sp=0x%08X ra=0x%08X"
                " a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X\n",
                n, pc32,
                (uint32_t)cp0->reg[COP0_EPC],
                (uint32_t)cp0->reg[COP0_STATUS],
                (uint32_t)cp0->reg[COP0_CAUSE],
                spv,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3]);
            fprintf(stderr,
                "[WINCE_HOT_L2W_REGS] #%u s0=0x%08X s1=0x%08X"
                " s2=0x%08X s3=0x%08X s4=0x%08X s5=0x%08X"
                " s6=0x%08X s7=0x%08X\n",
                n,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S2],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S3],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S4],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S5],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S6],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S7]);
            fprintf(stderr,
                "[WINCE_HOT_L2W_REGS] #%u t0=0x%08X t1=0x%08X"
                " t2=0x%08X t3=0x%08X t4=0x%08X t5=0x%08X"
                " t6=0x%08X t7=0x%08X t8=0x%08X t9=0x%08X\n",
                n,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T1],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T2],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T3],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T4],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T5],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T6],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T7],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T8],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T9]);
            dump_code_window(m, pc32, 5u, 2u);
            {
                uint64_t off = paddr - WINCE_HOT_USER_L2_TABLE_PA;
                uint32_t pte_slot = (uint32_t)(off / 8u);
                uint32_t va_lo = UINT32_C(0x01FE0000) + pte_slot * 0x2000u;
                uint32_t va_hi = va_lo + 0x1000u;
                fprintf(stderr,
                    "[WINCE_HOT_L2W_VA] #%u off=0x%02" PRIx64
                    " pte_slot=%u va_lo=0x%08X va_hi=0x%08X\n",
                    n, off, pte_slot, va_lo, va_hi);
            }
            if (rc > 0) {
                fprintf(stderr,
                    "[WINCE_CB_RET] label=hot_l2w_#%u", n);
                for (k = 0; k < rc; k++) {
                    fprintf(stderr,
                        " ret%zu=%#010x@+0x%02X callsite=0x%08X",
                        k, ret_addrs[k], ret_offs[k],
                        ret_addrs[k] >= 8u ? ret_addrs[k] - 8u : 0u);
                }
                fputc('\n', stderr);
            }
        }
    }

    /* Phase F: light-touch watch on the FULL 4KB L2 page, but only
     * for writes that fall OUTSIDE the hot 0x40 window already
     * covered above. Catches adjacent memsets / neighbour writers
     * that might be zero-bombing our PTE entries via a wider
     * sweep. One-liner per write, cap 256. */
    if (range_overlaps(paddr, len, WINCE_HOT_USER_L2_PAGE_PA,
            WINCE_HOT_USER_L2_PAGE_BYTES)
        && !range_overlaps(paddr, len, WINCE_HOT_USER_L2_TABLE_PA,
            WINCE_HOT_USER_L2_TRACE_BYTES)) {
        m->wince.hot_l2p_write_count++;
        if (m->wince.hot_l2p_write_count <= 256u) {
            fprintf(stderr,
                "[WINCE_HOT_L2P] #%u off=0x%03" PRIx64 " len=%" PRIu64
                " val=0x%08X pc=0x%08X ra=0x%08X hot_cnt=%u\n",
                (unsigned)m->wince.hot_l2p_write_count,
                paddr - WINCE_HOT_USER_L2_PAGE_PA,
                len,
                value,
                (uint32_t)cpu->pc,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
                (unsigned)m->wince.hot_user_l2_write_count);
        }
    }

    /* Phase E: watch the L1 section-table slot whose L2 pointer
     * equals 0x80FFC1C8 (PA 0x006694B8 / kseg0 VA 0x806694B8). The
     * teardown loop at pc=0x80097000 ra=0x800971C0 operates with
     * s5=0x806694B8. If anything clears or rewrites this slot
     * between publish #29 and teardown #30, that caller is the
     * actor that decided the L2 page was free. */
    if (range_overlaps(paddr, len, UINT32_C(0x006694B8), 4u)) {
        m->wince.l1_slot_write_count++;
        if (m->wince.l1_slot_write_count <= 32u) {
            struct mips_coproc *cp0 = cpu->cd.mips.coproc[0];
            uint32_t pc32 = (uint32_t)cpu->pc;
            uint32_t spv = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
            uint32_t ret_offs[4] = {0};
            uint32_t ret_addrs[4] = {0};
            size_t rc = collect_stack_return_sites(m, spv,
                ret_offs, ret_addrs,
                sizeof(ret_addrs) / sizeof(ret_addrs[0]), 0x40u);
            size_t k;
            uint32_t prev = load_pa_word(m, UINT32_C(0x006694B8));

            fprintf(stderr,
                "[WINCE_L1_SLOT_W] #%u prev=0x%08X new=0x%08X"
                " pc=0x%08X epc=0x%08X status=0x%08X cause=0x%08X"
                " sp=0x%08X ra=0x%08X a0=0x%08X a1=0x%08X"
                " hot_l2w_cnt=%u\n",
                (unsigned)m->wince.l1_slot_write_count,
                prev, (uint32_t)value,
                pc32,
                (uint32_t)cp0->reg[COP0_EPC],
                (uint32_t)cp0->reg[COP0_STATUS],
                (uint32_t)cp0->reg[COP0_CAUSE],
                spv,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
                (unsigned)m->wince.hot_user_l2_write_count);
            fprintf(stderr,
                "[WINCE_L1_SLOT_W] #%u s0=0x%08X s1=0x%08X"
                " s2=0x%08X s3=0x%08X s4=0x%08X s5=0x%08X"
                " s6=0x%08X s7=0x%08X\n",
                (unsigned)m->wince.l1_slot_write_count,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S2],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S3],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S4],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S5],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S6],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S7]);
            dump_code_window(m, pc32, 4u, 4u);
            if (rc > 0) {
                fprintf(stderr,
                    "[WINCE_CB_RET] label=l1_slot_w_#%u",
                    (unsigned)m->wince.l1_slot_write_count);
                for (k = 0; k < rc; k++) {
                    fprintf(stderr,
                        " ret%zu=%#010x@+0x%02X callsite=0x%08X",
                        k, ret_addrs[k], ret_offs[k],
                        ret_addrs[k] >= 8u ? ret_addrs[k] - 8u : 0u);
                }
                fputc('\n', stderr);
            }
        }
    }
}

static void maybe_log_systempatch_thread_write(machine_t *m, struct cpu *cpu,
    uint64_t paddr, size_t len, uint64_t val)
{
    uint32_t cur_thread_pa = 0;
    uint32_t obj_pa = 0;
    const char *tag = NULL;
    uint32_t old = 0;
    bool old_known = false;
    bool log_new_obj = false;
    uint32_t new_obj = 0;
    uint32_t new_obj00 = 0;
    uint32_t new_obj04 = 0;
    uint32_t new_ctx08 = 0;
    uint32_t new_ctx0c = 0;
    uint32_t new_src_off = 0;
    uint32_t new_src_idx = 0;
    uint32_t new_src_slot = 0;
    bool new_obj00_ok = false;
    bool new_obj04_ok = false;
    bool new_ctx08_ok = false;
    bool new_ctx0c_ok = false;
    bool new_src_slot_ok = false;
    char new_obj00_buf[16];
    char new_obj04_buf[16];
    char new_ctx08_buf[16];
    char new_ctx0c_buf[16];
    char new_src_slot_buf[16];

    if (!m || !cpu)
        return;
    if (!m->wince.section3_page_watch_armed
        && !m->wince.systempatch_thread_ctx_valid) {
        return;
    }
    if (m->wince.systempatch_thread_write_count >= 32)
        return;

    if (m->wince.systempatch_last_cur_thrd >= UINT32_C(0x80000000)
        && m->wince.systempatch_last_cur_thrd < UINT32_C(0x81000000)) {
        cur_thread_pa = m->wince.systempatch_last_cur_thrd & 0x1FFFFFFFu;
        if (range_overlaps(paddr, (uint64_t)len, cur_thread_pa + 0x0Cu, 4u)) {
            tag = "thread+0c";
            old = m->wince.systempatch_last_obj0c;
            old_known = m->wince.systempatch_thread_ctx_valid;
        } else if (range_overlaps(paddr, (uint64_t)len,
            cur_thread_pa + 0x14u, 4u)) {
            tag = "thread+14";
            old = m->wince.systempatch_last_obj14;
            old_known = m->wince.systempatch_thread_ctx_valid;
        } else if (range_overlaps(paddr, (uint64_t)len,
            cur_thread_pa + 0x24u, 4u)) {
            tag = "thread+24";
            old = m->wince.systempatch_last_obj24;
            old_known = m->wince.systempatch_thread_ctx_valid;
        } else if (range_overlaps(paddr, (uint64_t)len,
            cur_thread_pa + 0x3Cu, 4u)) {
            tag = "thread+3c";
            old = m->wince.systempatch_last_obj3c;
            old_known = m->wince.systempatch_thread_ctx_valid;
        }
    }

    if (tag == NULL && m->wince.systempatch_last_obj0c >= UINT32_C(0x80000000)
        && m->wince.systempatch_last_obj0c < UINT32_C(0x81000000)) {
        obj_pa = m->wince.systempatch_last_obj0c & 0x1FFFFFFFu;
        if (range_overlaps(paddr, (uint64_t)len, obj_pa + 0x00u, 4u)) {
            tag = "ctx+00";
            old = m->wince.systempatch_last_obj00;
            old_known = m->wince.systempatch_thread_ctx_valid;
        } else if (range_overlaps(paddr, (uint64_t)len,
            obj_pa + 0x04u, 4u)) {
            tag = "ctx+04";
            old = m->wince.systempatch_last_obj04;
            old_known = m->wince.systempatch_thread_ctx_valid;
        } else if (range_overlaps(paddr, (uint64_t)len, obj_pa + 0x08u, 4u)) {
            tag = "ctx+08";
            old = m->wince.systempatch_last_ctx08;
            old_known = m->wince.systempatch_thread_ctx_valid;
        } else if (range_overlaps(paddr, (uint64_t)len, obj_pa + 0x0Cu, 4u)) {
            tag = "ctx+0c";
            old = m->wince.systempatch_last_ctx0c;
            old_known = m->wince.systempatch_thread_ctx_valid;
        }
    }

    if (tag == NULL)
        return;

    fprintf(stderr,
        "[WINCE_THREAD_W] tag=%s PA=0x%08" PRIx64 " len=%zu"
        " old=%s0x%08X new=0x%08llX cur_thrd=0x%08X obj0c=0x%08X"
        " sec0=0x%08X sec3=0x%08X PC=0x%08" PRIx64 " RA=0x%08X\n",
        tag,
        paddr,
        len,
        old_known ? "" : "?",
        old,
        (unsigned long long)val,
        m->wince.systempatch_last_cur_thrd,
        m->wince.systempatch_last_obj0c,
        load_pa_word(m, 0x18C0u),
        load_pa_word(m, 0x18CCu),
        (uint64_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);

    if (strcmp(tag, "thread+0c") == 0) {
        new_obj = (uint32_t)val;
        if (new_obj >= UINT32_C(0x80000000)
            && new_obj < UINT32_C(0x81000000)) {
            log_new_obj = true;
            new_obj00_ok = load_va_word(m, new_obj + 0x00u, &new_obj00);
            new_obj04_ok = load_va_word(m, new_obj + 0x04u, &new_obj04);
            new_ctx08_ok = load_va_word(m, new_obj + 0x08u, &new_ctx08);
            new_ctx0c_ok = load_va_word(m, new_obj + 0x0Cu, &new_ctx0c);
            if (new_ctx0c_ok) {
                new_src_off = new_ctx0c >> 23;
                new_src_idx = new_src_off >> 2;
                if (new_src_off < 0x100u) {
                    new_src_slot = load_pa_word(m, 0x18C0u + new_src_off);
                    new_src_slot_ok = true;
                }
            }
        }
    }

    if (log_new_obj) {
        fprintf(stderr,
            "[WINCE_THREAD_W_CTX] new_obj=0x%08X obj+00=%s obj+04=%s"
            " ctx+08=%s ctx+0c=%s asid=%s0x%02X"
            " src_off=0x%03X src_idx=%u src_slot=%s"
            " sec0=0x%08X sec3=0x%08X PC=0x%08" PRIx64
            " RA=0x%08X\n",
            new_obj,
            format_word_or_unknown(new_obj00_buf, sizeof(new_obj00_buf),
                new_obj00_ok, new_obj00),
            format_word_or_unknown(new_obj04_buf, sizeof(new_obj04_buf),
                new_obj04_ok, new_obj04),
            format_word_or_unknown(new_ctx08_buf, sizeof(new_ctx08_buf),
                new_ctx08_ok, new_ctx08),
            format_word_or_unknown(new_ctx0c_buf, sizeof(new_ctx0c_buf),
                new_ctx0c_ok, new_ctx0c),
            new_obj00_ok ? "" : "?",
            new_obj00_ok ? (new_obj00 & 0xFFu) : 0u,
            new_src_off,
            new_src_idx,
            format_word_or_unknown(new_src_slot_buf,
                sizeof(new_src_slot_buf), new_src_slot_ok, new_src_slot),
            load_pa_word(m, 0x18C0u),
            load_pa_word(m, 0x18CCu),
            (uint64_t)cpu->pc,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
    }

    m->wince.systempatch_thread_write_count++;
}

static void maybe_log_section0_hot_slot_access(machine_t *m, struct cpu *cpu,
    uint64_t paddr, size_t len, uint64_t val, bool is_write)
{
    const uint32_t table_pa = UINT32_C(0x00FE5000);
    const uint32_t table_va = UINT32_C(0x80FE5000);
    const char *tag = NULL;
    uint32_t sec0;
    uint32_t sec3;
    uint16_t *count;
    uint16_t limit;

    if (!m || !cpu)
        return;

    if (!range_overlaps(paddr, (uint64_t)len, table_pa, 0x800u))
        return;

    if (range_overlaps(paddr, (uint64_t)len, table_pa + 0x7E4u, 4u)) {
        tag = "slot7e4";
    } else if (range_overlaps(paddr, (uint64_t)len, table_pa + 0x7E0u, 4u)) {
        tag = "slot7e0";
    } else if (range_overlaps(paddr, (uint64_t)len, table_pa + 0x7E8u, 4u)) {
        tag = "slot7e8";
    } else if (range_overlaps(paddr, (uint64_t)len, table_pa + 0x7F0u, 4u)) {
        tag = "slot7f0";
    } else if (range_overlaps(paddr, (uint64_t)len, table_pa + 0x7F4u, 4u)) {
        tag = "slot7f4";
    } else if (range_overlaps(paddr, (uint64_t)len, table_pa + 0x7F8u, 4u)) {
        tag = "slot7f8";
    } else if (range_overlaps(paddr, (uint64_t)len, table_pa + 0x694u, 4u)) {
        tag = "slot694";
    }
    if (tag == NULL)
        return;

    sec0 = load_pa_word(m, 0x18C0u);
    sec3 = load_pa_word(m, 0x18CCu);
    if (!m->wince.section3_page_watch_armed
        && sec0 != UINT32_C(0x80FE5000)
        && sec3 != UINT32_C(0x80FE5000)) {
        return;
    }

    /*
     * Keep the post-arm budget focused on the hot `0x01F94B50` slot.  Reads
     * for neighboring slots add little signal once the fault storm starts, but
     * writes to any nearby entry are still useful for spotting a misindexed
     * publish.
     */
    if (!is_write && strcmp(tag, "slot7e0") != 0
        && strcmp(tag, "slot7e4") != 0)
        return;

    count = is_write ? &m->wince.section0_focus_slot_write_count
        : &m->wince.section0_focus_slot_read_count;
    limit = is_write ? 64u : 24u;
    if (*count >= limit)
        return;

    fprintf(stderr,
        "[WINCE_SEC0_RAM] %c %s off=0x%03" PRIx64 " len=%zu val=0x%llX"
        " sec0=0x%08X sec3=0x%08X PC=0x%08" PRIx64 " RA=0x%08X\n",
        is_write ? 'W' : 'R',
        tag,
        paddr - table_pa,
        len,
        (unsigned long long)val,
        sec0,
        sec3,
        (uint64_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
    log_l2_table_state(m, tag, table_va, UINT32_C(0x01F94B50));
    log_section0_focus_window(m, tag, table_va);

    (*count)++;
}

static void maybe_log_section3_raw_write(machine_t *m, struct cpu *cpu,
    uint64_t paddr, size_t len, uint64_t val)
{
    if (!m || !cpu)
        return;
    if (!range_overlaps(paddr, (uint64_t)len, 0x000018CCu, 4u))
        return;
    if (m->wince.section3_raw_diag_count >= 24)
        return;

    fprintf(stderr,
        "[WINCE_SEC3_RAW] W PA=0x%08" PRIx64 " len=%zu val=0x%llX"
        " hit_sec3=%u PC=0x%08" PRIx64 " RA=0x%08X\n",
        paddr,
        len,
        (unsigned long long)val,
        1u,
        (uint64_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);

    m->wince.section3_raw_diag_count++;
}

static void maybe_log_section3_page_write(machine_t *m, struct cpu *cpu,
    uint64_t paddr, size_t len, uint64_t val)
{
    const uint32_t table_pa = UINT32_C(0x00FE5000);
    const uint32_t table_va = UINT32_C(0x80FE5000);
    const char *tag = "table";

    if (!m || !cpu)
        return;
    if (!range_overlaps(paddr, (uint64_t)len, table_pa, 0x1000u))
        return;
    if (!m->wince.section3_page_watch_armed)
        return;
    if (m->wince.section3_page_diag_count >= 64)
        return;

    if (range_overlaps(paddr, (uint64_t)len, table_pa + 0x7E4u, 4u)) {
        tag = "slot7e4";
    } else if (range_overlaps(paddr, (uint64_t)len, table_pa + 0x7E0u, 4u)) {
        tag = "slot7e0";
    } else if (range_overlaps(paddr, (uint64_t)len, table_pa + 0x7E8u, 4u)) {
        tag = "slot7e8";
    } else if (range_overlaps(paddr, (uint64_t)len, table_pa + 0x694u, 4u)) {
        tag = "slot694";
    } else if (range_overlaps(paddr, (uint64_t)len, table_pa + 0x000u, 4u)) {
        tag = "slot000";
    }

    fprintf(stderr,
        "[WINCE_SEC3_PAGE] W %s off=0x%03" PRIx64 " len=%zu val=0x%llX"
        " PC=0x%08" PRIx64 " RA=0x%08X\n",
        tag,
        paddr - table_pa,
        len,
        (unsigned long long)val,
        (uint64_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);

    if (strcmp(tag, "table") != 0) {
        log_l2_table_state(m, "section3_page", table_va, UINT32_C(0x01F94B50));
        log_section0_focus_window(m, "section3_page", table_va);
    }

    m->wince.section3_page_diag_count++;
}

static void maybe_log_section3_owner_write(machine_t *m, struct cpu *cpu,
    uint64_t paddr, size_t len, uint64_t val)
{
    const uint32_t obj_pa = UINT32_C(0x006697A0);
    const uint32_t obj2_pa = UINT32_C(0x006697C0);
    uint32_t sec3;
    const char *tag;

    if (!m || !cpu)
        return;
    if (!range_overlaps(paddr, (uint64_t)len, obj_pa, 0x20u)
        && !range_overlaps(paddr, (uint64_t)len, obj2_pa, 0x20u))
        return;

    sec3 = load_pa_word(m, 0x18CCu);
    if (!m->wince.section3_page_watch_armed
        && sec3 != UINT32_C(0x80FE5000))
        return;
    if (m->wince.section3_owner_write_count >= 24u)
        return;

    tag = range_overlaps(paddr, (uint64_t)len, obj_pa, 0x20u)
        ? "evt97a0"
        : "evt97c0";
    fprintf(stderr,
        "[WINCE_SEC3_OWNER_W] %s+0x%02" PRIx64 " len=%zu val=0x%llX"
        " PC=0x%08" PRIx64 " RA=0x%08X\n",
        tag,
        paddr - (strcmp(tag, "evt97a0") == 0 ? obj_pa : obj2_pa),
        len,
        (unsigned long long)val,
        (uint64_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
    log_section3_owner_state(m, cpu, tag,
        canonicalize_nk_pc((uint32_t)cpu->pc));
    m->wince.section3_owner_write_count++;
}

static void maybe_log_section3_desc_write(machine_t *m, struct cpu *cpu,
    uint64_t paddr, size_t len, uint64_t val)
{
    const uint32_t desc_pa = UINT32_C(0x00FE9CC0);
    const uint32_t desc_len = UINT32_C(0x40);
    uint32_t sec3;

    if (!m || !cpu)
        return;
    if (!range_overlaps(paddr, (uint64_t)len, desc_pa, desc_len))
        return;

    sec3 = load_pa_word(m, 0x18CCu);
    if (!m->wince.section3_page_watch_armed
        && sec3 != UINT32_C(0x80FE5000))
        return;
    if (m->wince.section3_desc_write_count >= 24u)
        return;

    fprintf(stderr,
        "[WINCE_SEC3_DESC_W] off=0x%02" PRIx64 " len=%zu val=0x%llX"
        " PC=0x%08" PRIx64 " RA=0x%08X\n",
        paddr - desc_pa,
        len,
        (unsigned long long)val,
        (uint64_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
    dump_section3_descriptor_window(m, "desc_write");
    m->wince.section3_desc_write_count++;
}

static void maybe_log_section3_pool_write(machine_t *m, struct cpu *cpu,
    uint64_t paddr, size_t len, uint64_t val)
{
    const uint32_t pool_pa = UINT32_C(0x00FE9CD0);
    const uint32_t wrap_va = UINT32_C(0x80FE9CDC);
    const uint32_t payload_va = UINT32_C(0x80FE9DA4);
    const uint32_t wrap_pa = UINT32_C(0x00FE9CDC);
    const uint32_t payload_pa = UINT32_C(0x00FE9DA4);
    const uint32_t pool_len = UINT32_C(0x180);
    uint32_t sec3;
    const char *tag = "pool";

    if (!m || !cpu)
        return;
    if (!range_overlaps(paddr, (uint64_t)len, pool_pa, pool_len))
        return;

    sec3 = load_pa_word(m, 0x18CCu);
    if (m->wince.section3_pool_write_count >= 64u)
        return;

    if (range_overlaps(paddr, (uint64_t)len, wrap_pa, 0x20u)) {
        tag = "wrap";
    } else if (range_overlaps(paddr, (uint64_t)len, payload_pa, 0xA0u)) {
        tag = "payload";
    }

    fprintf(stderr,
        "[WINCE_SEC3_POOL_W] tag=%s off=0x%03" PRIx64 " len=%zu val=0x%llX"
        " sec0=0x%08X sec3=0x%08X PC=0x%08" PRIx64 " RA=0x%08X\n",
        tag,
        paddr - pool_pa,
        len,
        (unsigned long long)val,
        load_pa_word(m, 0x18C0u),
        sec3,
        (uint64_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
    if (strcmp(tag, "wrap") == 0 || strcmp(tag, "payload") == 0) {
        dump_section3_wrap_window(m, "pool_write", wrap_va);
        dump_section3_retobj_window(m, "pool_write", payload_va);
    }
    if (range_overlaps(paddr, (uint64_t)len, wrap_pa + 0x14u, 8u)) {
        fprintf(stderr,
            "[WINCE_SEC3_POOL_W] wrap_ctor off=0x%02" PRIx64
            " wrap=0x%08X payload=0x%08X state_ptr=0x%08X payload_ptr=0x%08X\n",
            paddr - wrap_pa,
            wrap_va,
            payload_va,
            load_pa_word(m, wrap_pa + 0x14u),
            load_pa_word(m, wrap_pa + 0x18u));
    }
    if (range_overlaps(paddr, (uint64_t)len, payload_pa, 0x20u)) {
        fprintf(stderr,
            "[WINCE_SEC3_POOL_W] payload_head off=0x%02" PRIx64
            " payload=0x%08X +00=0x%08X +04=0x%08X +08=0x%08X +0C=0x%08X\n",
            paddr - payload_pa,
            payload_va,
            load_pa_word(m, payload_pa + 0x00u),
            load_pa_word(m, payload_pa + 0x04u),
            load_pa_word(m, payload_pa + 0x08u),
            load_pa_word(m, payload_pa + 0x0Cu));
    }
    if (strcmp(tag, "payload") == 0
        && m->wince.type4_payload_watch_va == payload_va
        && range_overlaps(paddr, (uint64_t)len, payload_pa + 0x88u, 0x18u)
        && m->wince.type4_ready_write_count < 16u) {
        fprintf(stderr,
            "[WINCE_TYPE4_READY_W] off=0x%02" PRIx64 " len=%zu val=0x%llX"
            " payload=0x%08X sec0=0x%08X sec3=0x%08X"
            " PC=0x%08" PRIx64 " RA=0x%08X\n",
            paddr - payload_pa,
            len,
            (unsigned long long)val,
            payload_va,
            load_pa_word(m, 0x18C0u),
            sec3,
            (uint64_t)cpu->pc,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
        dump_section3_retobj_window(m, "type4_ready_write", payload_va);
        m->wince.type4_ready_write_count++;
    }
    m->wince.section3_pool_write_count++;
}

static void maybe_log_section3_ctor_field_write(machine_t *m, struct cpu *cpu,
    uint64_t paddr, size_t len, uint64_t val)
{
    const uint32_t wrap_pa = UINT32_C(0x00FE9CDC);
    const uint32_t wrap_va = UINT32_C(0x80FE9CDC);
    const uint32_t payload_pa = UINT32_C(0x00FE9DA4);
    const uint32_t payload_va = UINT32_C(0x80FE9DA4);
    uint32_t sp;
    uint32_t saved_ra = 0;
    uint32_t saved_arg0 = 0;
    uint32_t saved_arg1 = 0;
    uint32_t saved_arg2 = 0;
    uint32_t stack_words[4] = { 0 };
    bool saved_ra_ok = false;
    bool saved_arg0_ok = false;
    bool saved_arg1_ok = false;
    bool saved_arg2_ok = false;
    bool stack_ok[4] = { false };
    uint32_t sec3;
    const char *tag;
    char saved_ra_buf[16];
    char saved_arg0_buf[16];
    char saved_arg1_buf[16];
    char saved_arg2_buf[16];
    char stack0_buf[16];
    char stack1_buf[16];
    char stack2_buf[16];
    char stack3_buf[16];

    if (!m || !cpu)
        return;
    if (!range_overlaps(paddr, (uint64_t)len, wrap_pa + 0x14u, 8u)
        && !range_overlaps(paddr, (uint64_t)len, payload_pa, 0x20u)) {
        return;
    }
    if (m->wince.section3_ctor_probe_count >= 32u)
        return;

    sec3 = load_pa_word(m, 0x18CCu);
    tag = range_overlaps(paddr, (uint64_t)len, wrap_pa + 0x14u, 8u)
        ? "wrap_ctor"
        : "payload_head";

    fprintf(stderr,
        "[WINCE_SEC3_CTOR_W] tag=%s off=0x%02" PRIx64 " len=%zu val=0x%llX"
        " sec0=0x%08X sec3=0x%08X PC=0x%08" PRIx64 " RA=0x%08X\n",
        tag,
        paddr - (strcmp(tag, "wrap_ctor") == 0 ? wrap_pa : payload_pa),
        len,
        (unsigned long long)val,
        load_pa_word(m, 0x18C0u),
        sec3,
        (uint64_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
    sp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
    saved_ra_ok = load_va_word(m, sp + 0x14u, &saved_ra);
    saved_arg0_ok = load_va_word(m, sp + 0x20u, &saved_arg0);
    saved_arg1_ok = load_va_word(m, sp + 0x24u, &saved_arg1);
    saved_arg2_ok = load_va_word(m, sp + 0x28u, &saved_arg2);
    for (unsigned i = 0; i < 4u; i++) {
        stack_ok[i] = load_va_word(m, sp + i * 4u, &stack_words[i]);
    }
    fprintf(stderr,
        "[WINCE_SEC3_CTOR_CTX] a0=0x%08X a1=0x%08X a2=0x%08X sp=0x%08X"
        " saved_ra=%s saved_a0=%s saved_a1=%s saved_a2=%s"
        " s0=0x%08X s1=0x%08X s2=0x%08X s3=0x%08X"
        " stk0=%s stk1=%s stk2=%s stk3=%s\n",
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2],
        sp,
        format_word_or_unknown(saved_ra_buf, sizeof(saved_ra_buf),
            saved_ra_ok, saved_ra),
        format_word_or_unknown(saved_arg0_buf, sizeof(saved_arg0_buf),
            saved_arg0_ok, saved_arg0),
        format_word_or_unknown(saved_arg1_buf, sizeof(saved_arg1_buf),
            saved_arg1_ok, saved_arg1),
        format_word_or_unknown(saved_arg2_buf, sizeof(saved_arg2_buf),
            saved_arg2_ok, saved_arg2),
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S2],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S3],
        format_word_or_unknown(stack0_buf, sizeof(stack0_buf),
            stack_ok[0], stack_words[0]),
        format_word_or_unknown(stack1_buf, sizeof(stack1_buf),
            stack_ok[1], stack_words[1]),
        format_word_or_unknown(stack2_buf, sizeof(stack2_buf),
            stack_ok[2], stack_words[2]),
        format_word_or_unknown(stack3_buf, sizeof(stack3_buf),
            stack_ok[3], stack_words[3]));
    dump_section3_wrap_window(m, "ctor_write", wrap_va);
    dump_section3_retobj_window(m, "ctor_write", payload_va);
    if (strcmp(tag, "wrap_ctor") == 0
        && paddr == wrap_pa + 0x18u
        && saved_ra_ok
        && saved_ra == UINT32_C(0x80081BEC)
        && saved_arg0_ok
        && saved_arg0 == UINT32_C(0x80074C68)
        && saved_arg1_ok
        && saved_arg1 == UINT32_C(0x80FE9DA4)
        && !m->wince.type4_step_trace_active
        && !m->wince.type4_step_trace_done) {
        m->wince.type4_wrap_watch_va = wrap_va;
        m->wince.type4_payload_watch_va = payload_va;
        m->wince.type4_handle_watch_va = load_pa_word(m, wrap_pa + 0x08u);
        m->wince.type4_ready_write_count = 0;
        note_type4_order_event(m, cpu,
            &m->wince.type4_order_ctor_seq, "wrap_ctor");
        m->wince.type4_step_trace_pending = false;
        m->wince.type4_step_trace_active = true;
        m->wince.type4_step_trace_remaining = 192u;
        single_step = true;
        fprintf(stderr,
            "[WINCE_TYPE4_STEP] armed pc=0x%08X saved_ra=0x%08X"
            " wrap=0x%08X handle=0x%08X payload=0x%08X rem=%u trace=active"
            " sp=0x%08X ra=0x%08X\n",
            canonicalize_nk_pc((uint32_t)cpu->pc),
            saved_ra,
            wrap_va,
            m->wince.type4_handle_watch_va,
            payload_va,
            (unsigned)m->wince.type4_step_trace_remaining,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
    }
    m->wince.section3_ctor_probe_count++;
}

static void maybe_log_section3_focus_obj_write(machine_t *m, struct cpu *cpu,
    uint64_t paddr, size_t len, uint64_t val)
{
    uint32_t desc_base = 0;
    uint32_t wrap_va;
    uint32_t payload_va;
    uint32_t wrap_pa;
    uint32_t payload_pa;
    uint32_t sec3;
    const char *tag;

    if (!m || !cpu)
        return;
    if (!load_section3_descriptor_focus(m, &wrap_va, &desc_base))
        return;

    wrap_va = desc_base + 0x1Cu;
    payload_va = desc_base + 0xE4u;
    wrap_pa = table_va_to_pa(wrap_va);
    payload_pa = table_va_to_pa(payload_va);

    if (!range_overlaps(paddr, (uint64_t)len, wrap_pa, 0x20u)
        && !range_overlaps(paddr, (uint64_t)len, payload_pa, 0xA0u)) {
        return;
    }

    sec3 = load_pa_word(m, 0x18CCu);
    if (!m->wince.section3_page_watch_armed
        && sec3 != UINT32_C(0x80FE5000))
        return;
    if (m->wince.section3_focusobj_write_count >= 32u)
        return;

    tag = range_overlaps(paddr, (uint64_t)len, wrap_pa, 0x20u)
        ? "wrap"
        : "payload";
    fprintf(stderr,
        "[WINCE_SEC3_OBJ_W] tag=%s off=0x%02" PRIx64 " len=%zu val=0x%llX"
        " wrap=0x%08X payload=0x%08X"
        " sec0=0x%08X sec3=0x%08X PC=0x%08" PRIx64 " RA=0x%08X\n",
        tag,
        paddr - (strcmp(tag, "wrap") == 0 ? wrap_pa : payload_pa),
        len,
        (unsigned long long)val,
        wrap_va,
        payload_va,
        load_pa_word(m, 0x18C0u),
        sec3,
        (uint64_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
    dump_section3_wrap_window(m, tag, wrap_va);
    dump_section3_retobj_window(m, tag, payload_va);
    if (strcmp(tag, "wrap") == 0
        && range_overlaps(paddr, (uint64_t)len, wrap_pa + 0x04u, 4u)
        && m->wince.type4_wrap_watch_va == wrap_va
        && m->wince.type4_payload_watch_va == payload_va) {
        note_type4_order_event(m, cpu,
            &m->wince.type4_order_link_seq, "wrap_link");
    }
    m->wince.section3_focusobj_write_count++;
}

static void maybe_log_section3_desc_read(machine_t *m, struct cpu *cpu,
    uint64_t paddr, size_t len, uint64_t val)
{
    const uint32_t desc_pa = UINT32_C(0x00FE9CC0);
    const uint32_t desc_len = UINT32_C(0x40);
    uint64_t off;
    uint32_t sec3;

    if (!m || !cpu)
        return;
    if (!range_overlaps(paddr, (uint64_t)len, desc_pa, desc_len))
        return;

    off = paddr - desc_pa;
    if (m->wince.type4_wrap_watch_va != 0u
        && off != 0x30u
        && off != 0x34u) {
        return;
    }

    sec3 = load_pa_word(m, 0x18CCu);
    if (!m->wince.section3_page_watch_armed
        && sec3 != UINT32_C(0x80FE5000))
        return;
    if (m->wince.section3_desc_read_count >= 24u)
        return;

    fprintf(stderr,
        "[WINCE_SEC3_DESC_R] off=0x%02" PRIx64 " len=%zu val=0x%llX"
        " sec0=0x%08X sec3=0x%08X PC=0x%08" PRIx64 " RA=0x%08X\n",
        off,
        len,
        (unsigned long long)val,
        load_pa_word(m, 0x18C0u),
        sec3,
        (uint64_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
    if (off == 0x30u
        && canonicalize_nk_pc((uint32_t)cpu->pc) == UINT32_C(0x8009A9CC)) {
        note_type4_order_event(m, cpu,
            &m->wince.type4_order_state_seq, "consumer_state");
    } else if (off == 0x34u) {
        uint32_t pc32 = canonicalize_nk_pc((uint32_t)cpu->pc);
        if (pc32 == UINT32_C(0x8009A7F8)
            || pc32 == UINT32_C(0x8009A778)
            || pc32 == UINT32_C(0x8009A850)) {
            note_type4_order_event(m, cpu,
                &m->wince.type4_order_payload_seq, "consumer_payload");
        }
    }
    maybe_log_section3_gate_snapshot(m, cpu,
        canonicalize_nk_pc((uint32_t)cpu->pc), sec3);
    dump_section3_descriptor_window(m, "desc_read");
    m->wince.section3_desc_read_count++;
}

static void maybe_log_section3_retobj_write(machine_t *m, struct cpu *cpu,
    uint64_t paddr, size_t len, uint64_t val)
{
    uint32_t obj_va;
    uint32_t obj_pa;
    uint32_t sec3;

    if (!m || !cpu)
        return;
    if (!m->wince.section3_retobj_watch_armed)
        return;

    obj_va = m->wince.section3_retobj_watch_va;
    if (obj_va < UINT32_C(0x80000000) || obj_va >= UINT32_C(0x81000000))
        return;

    obj_pa = table_va_to_pa(obj_va);
    if (!range_overlaps(paddr, (uint64_t)len, obj_pa, 0xA0u))
        return;
    if (m->wince.section3_retobj_write_count >= 24u)
        return;

    sec3 = load_pa_word(m, 0x18CCu);
    fprintf(stderr,
        "[WINCE_SEC3_RETOBJ_W] off=0x%02" PRIx64 " len=%zu val=0x%llX"
        " sec0=0x%08X sec3=0x%08X PC=0x%08" PRIx64 " RA=0x%08X\n",
        paddr - obj_pa,
        len,
        (unsigned long long)val,
        load_pa_word(m, 0x18C0u),
        sec3,
        (uint64_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
    dump_section3_retobj_window(m, "write", obj_va);
    m->wince.section3_retobj_write_count++;
}

static void maybe_log_section3_head_write(machine_t *m, struct cpu *cpu,
    uint64_t paddr, size_t len, uint64_t val)
{
    uint32_t ctx_ptr = 0;
    uint32_t head_ptr = 0;
    uint32_t slot_pa;

    if (!m || !cpu)
        return;
    if (m->wince.section3_head_write_count >= 16u)
        return;
    if (!load_section3_context_head(m, &ctx_ptr, &head_ptr))
        return;

    slot_pa = (ctx_ptr & UINT32_C(0x1FFFFFFF)) + 0x18u;
    if (!range_overlaps(paddr, (uint64_t)len, slot_pa, 4u))
        return;

    fprintf(stderr,
        "[WINCE_CTX_HEAD_W] slot_pa=0x%08X len=%zu val=0x%llX"
        " PC=0x%08" PRIx64 " RA=0x%08X\n",
        slot_pa,
        len,
        (unsigned long long)val,
        (uint64_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
    dump_section3_context_head(m, "head_write",
        canonicalize_nk_pc((uint32_t)cpu->pc));
    m->wince.section3_head_write_count++;
}

/* ------------------------------------------------------------------ */
/*  Diagnostic / dump helpers                                           */
/* ------------------------------------------------------------------ */

static const char *format_word_or_unknown(char *buf, size_t buf_size, bool ok,
    uint32_t value)
{
    if (!ok)
        return "????????";

    snprintf(buf, buf_size, "%08X", value);
    return buf;
}

static const char *format_half_or_unknown(char *buf, size_t buf_size, bool ok,
    uint16_t value)
{
    if (!ok)
        return "????";

    snprintf(buf, buf_size, "%04X", value);
    return buf;
}

static void set_watch_observer(machine_t *m, bool enable)
{
    m->wince.suppress_watch_observer = !enable;
}

static bool watched_ram_range_name(uint64_t paddr, uint64_t len,
    const char **name)
{
    if (range_overlaps(paddr, len, 0x00002200u, 0x000000D4u)) {
        *name = "resume_ctx";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0000250Cu, 4u)) {
        *name = "cold_probe_250c";
        return true;
    }
    if (range_overlaps(paddr, len, 0x00002518u, 4u)) {
        *name = "warm_flag_2518";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0000251Cu, 4u)) {
        *name = "boot_flags_251c";
        return true;
    }
    if (range_overlaps(paddr, len, 0x00002528u, 4u)) {
        *name = "resume_ptr_2528";
        return true;
    }
    if (range_overlaps(paddr, len, 0x00002554u, 4u)) {
        *name = "ram_size_2554";
        return true;
    }
    /* Section table at PA 0x18C0: all 64 entries (256 bytes) */
    if (range_overlaps(paddr, len, 0x000018C0u, 0x100u)) {
        *name = "section_tbl";
        return true;
    }
    /* KData+0xA0 (PA 0x18A0): PSL trap dispatch base pointer */
    if (range_overlaps(paddr, len, 0x000018A0u, 4u)) {
        *name = "psl_trap_base";
        return true;
    }
    /* KData current thread/process links used by the refill path */
    if (range_overlaps(paddr, len, 0x00001AB0u, 4u)) {
        *name = "ctx_link";
        return true;
    }
    if (range_overlaps(paddr, len, 0x00001AC0u, 4u)) {
        *name = "ctx_ptr";
        return true;
    }
    /* KData+0x9C (PA 0x189C): refill_mask */
    if (range_overlaps(paddr, len, 0x0000189Cu, 4u)) {
        *name = "refill_mask";
        return true;
    }
    return false;
}

static bool watched_mmio_range_name(uint64_t paddr, uint64_t len,
    const char **name)
{
    if (range_overlaps(paddr, len, 0x0A008010u, 4u)) {
        *name = "vrc4173_8010";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A001120u, 4u)) {
        *name = "vrc4173_1120";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A001128u, 4u)) {
        *name = "vrc4173_1128";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A00112Cu, 4u)) {
        *name = "vrc4173_112c";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A001134u, 4u)) {
        *name = "vrc4173_1134";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A001138u, 4u)) {
        *name = "vrc4173_1138";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A00113Cu, 4u)) {
        *name = "vrc4173_113c";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A001B10u, 4u)) {
        *name = "vrc4173_1b10";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A001B14u, 4u)) {
        *name = "vrc4173_1b14";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A001B20u, 4u)) {
        *name = "vrc4173_1b20";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A001B2Cu, 4u)) {
        *name = "vrc4173_1b2c";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A000004u, 4u)) {
        *name = "vrc4173_0004";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A00104Cu, 4u)) {
        *name = "vrc4173_104c";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A00A042u, 2u)) {
        *name = "buttons_a042";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A00A044u, 2u)) {
        *name = "buttons_a044";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A00A07Cu, 2u)) {
        *name = "buttons_a07c";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A00A03Cu, 4u)) {
        *name = "buttons_a03c";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A00A0E0u, 4u)) {
        *name = "nand_a0e0";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A000300u, 4u)) {
        *name = "vrc4173_0300";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A000390u, 4u)) {
        *name = "vrc4173_0390";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A001054u, 2u)) {
        *name = "vrc4173_1054";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A001B50u, 4u)) {
        *name = "vrc4173_1b50";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A001B58u, 4u)) {
        *name = "vrc4173_1b58";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A0008A0u, 4u)) {
        *name = "vrc4173_08a0";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A000A00u, 4u)) {
        *name = "vrc4173_0a00";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0A00130Cu, 4u)) {
        *name = "vrc4173_130c";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0F000046u, 2u)) {
        *name = "vr41xx_0046";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0F000100u, 4u)) {
        *name = "vr41xx_0100";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0F000104u, 2u)) {
        *name = "vr41xx_0104";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0F000144u, 2u)) {
        *name = "vr41xx_0144";
        return true;
    }
    if (range_overlaps(paddr, len, 0x0F0000C0u, 2u)) {
        *name = "pmu_c0";
        return true;
    }
    return false;
}

static void maybe_log_boot_path_probe(machine_t *m, uint32_t raw_pc32)
{
    uint32_t pc32 = raw_pc32;
    uint64_t bit = 0;
    const char *name = NULL;
    uint32_t pa2400;
    uint32_t pa2404;
    uint32_t pa2408;
    uint32_t pa250c;
    uint32_t pa2518;
    uint32_t pa251c;
    uint32_t pa2524;
    uint32_t pa254c;
    uint32_t pa2554;
    uint32_t pa2528;
    uint32_t pa2700;
    uint32_t latch0004 = 0;
    uint32_t a03c = 0;
    uint32_t nand_a0e0 = 0;
    uint32_t vrc8010 = 0;
    uint32_t vrc104c = 0;
    uint32_t vrc1b50 = 0;
    uint32_t vrc1b58 = 0;
    uint32_t vrc08a0 = 0;
    uint32_t vrc0a00 = 0;
    uint32_t vrc1120 = 0;
    uint32_t vrc112c = 0;
    uint32_t vrc1134 = 0;
    uint32_t vrc130c = 0;
    uint32_t vrc1b10 = 0;
    uint32_t vrc1b20 = 0;
    uint32_t lat8010 = 0;
    uint32_t lat1120 = 0;
    uint32_t lat112c = 0;
    uint32_t lat1b10 = 0;
    uint32_t lat1b20 = 0;
    uint32_t vrc0300 = 0;
    uint32_t vrc0390 = 0;
    uint16_t btn_a042 = 0;
    uint16_t btn_a044 = 0;
    uint16_t btn_a07c = 0;
    uint16_t vrc1054 = 0;
    uint16_t pmu_c0 = 0;
    uint16_t vr0046 = 0;
    uint16_t vr0104 = 0;
    uint16_t vr0144 = 0;
    uint32_t vr0100 = 0;
    bool vrc8010_ok;
    bool latch0004_ok;
    bool a03c_ok;
    bool nand_a0e0_ok;
    bool vrc104c_ok;
    bool vrc1b50_ok;
    bool vrc1b58_ok;
    bool vrc08a0_ok;
    bool vrc0a00_ok;
    bool vrc1120_ok;
    bool vrc112c_ok;
    bool vrc1134_ok;
    bool vrc130c_ok;
    bool vrc1b10_ok;
    bool vrc1b20_ok;
    bool lat8010_ok;
    bool lat1120_ok;
    bool lat112c_ok;
    bool lat1b10_ok;
    bool lat1b20_ok;
    bool btn_ok;
    bool btn044_ok;
    bool btn07c_ok;
    bool vrc1054_ok;
    bool vrc0300_ok;
    bool vrc0390_ok;
    bool pmu_c0_ok;
    bool vr0100_ok;
    bool vr0046_ok;
    bool vr0104_ok;
    bool vr0144_ok;
    char b8010[9];
    char b0004[9];
    char ba03c[9];
    char ba0e0[9];
    char b104c[9];
    char b1b50[9];
    char b1b58[9];
    char b08a0[9];
    char b0a00[9];
    char b1120[9];
    char b112c[9];
    char b1134[9];
    char b130c[9];
    char b1b10[9];
    char b1b20[9];
    char blat8010[9];
    char blat1120[9];
    char blat112c[9];
    char blat1b10[9];
    char blat1b20[9];
    char ba042[5];
    char ba044[5];
    char ba07c[5];
    char b1054[5];
    char b0300[9];
    char b0390[9];
    char bpmu[5];
    char b0046[5];
    char b0100[9];
    char b0104[5];
    char b0144[5];

    if ((pc32 & 0xE0000000u) == 0x80000000u
        || (pc32 & 0xE0000000u) == 0xA0000000u)
        pc32 = (pc32 & 0x1FFFFFFFu) | 0x80000000u;

    switch (pc32) {
    case 0x80076B50u:
        bit = WINCE_PATH_PROBE_76B50;
        name = "nk_entry_76b50";
        break;
    case 0x80076BA0u:
        bit = WINCE_PATH_PROBE_76BA0;
        name = "nk_uncached_entry";
        break;
    case 0x80076C60u:
        bit = WINCE_PATH_PROBE_76C60;
        name = "main_hw_init_76c60";
        break;
    case 0x80076CBCu:
        bit = WINCE_PATH_PROBE_76CBC;
        name = "version_gate_2400";
        break;
    case 0x80076E68u:
        bit = WINCE_PATH_PROBE_76E68;
        name = "hibernate_gate_76e68";
        break;
    case 0x80076FBCu:
        bit = WINCE_PATH_PROBE_76FBC;
        name = "preinit_wait_0144_0800";
        break;
    case 0x80076FE0u:
        bit = WINCE_PATH_PROBE_76FE0;
        name = "preinit_fallback_79460";
        break;
    case 0x80077260u:
        bit = WINCE_PATH_PROBE_77260;
        name = "wait_0144_timeout_79460";
        break;
    case 0x800772F0u:
        bit = WINCE_PATH_PROBE_772F0;
        name = "cont_772f0";
        break;
    case 0x80077344u:
        bit = WINCE_PATH_PROBE_77344;
        name = "keys_to_251c";
        break;
    case 0x8007742Cu:
        bit = WINCE_PATH_PROBE_7742C;
        name = "cont_after_76e68";
        break;
    case 0x8007757Cu:
        bit = WINCE_PATH_PROBE_7757C;
        name = "cont_version_gate";
        break;
    case 0x800775B0u:
        bit = WINCE_PATH_PROBE_775B0;
        name = "ram_size_select";
        break;
    case 0x800775B8u:
        bit = WINCE_PATH_PROBE_775B8;
        name = "cold_mem_clear";
        break;
    case 0x80077664u:
        bit = WINCE_PATH_PROBE_77664;
        name = "cont_seq2408_gate";
        break;
    case 0x8007767Cu:
        bit = WINCE_PATH_PROBE_7767C;
        name = "late_dispatch_entry";
        break;
    case 0x800776F0u:
        bit = WINCE_PATH_PROBE_776F0;
        name = "late_cold_setup";
        break;
    case 0x80077738u:
        bit = WINCE_PATH_PROBE_77738;
        name = "cont_join_77738";
        break;
    case 0x80077770u:
        bit = WINCE_PATH_PROBE_77770;
        name = "late_keysplit_a07c";
        break;
    case 0x80077820u:
        bit = WINCE_PATH_PROBE_77820;
        name = "boot_dispatch";
        break;
    case 0x8007794Cu:
        bit = WINCE_PATH_PROBE_7794C;
        name = "dispatch_fallback_7794c";
        break;
    case 0x8007796Cu:
        bit = WINCE_PATH_PROBE_7796C;
        name = "cold_kernel_prep";
        break;
    case 0x80077A14u:
        bit = WINCE_PATH_PROBE_77A14;
        name = "cold_kernel_trampoline";
        break;
    case 0x80079430u:
        bit = WINCE_PATH_PROBE_79430;
        name = "resume_snapshot";
        break;
    case 0x80079460u:
        bit = WINCE_PATH_PROBE_79460;
        name = "preinit_tail";
        break;
    case 0x80079488u:
        bit = WINCE_PATH_PROBE_79488;
        name = "resume_split";
        break;
    case 0x800794C8u:
        bit = WINCE_PATH_PROBE_794C8;
        name = "cold_path";
        break;
    case 0x80079510u:
        bit = WINCE_PATH_PROBE_79510;
        name = "common_tail";
        break;
    case 0x800795D8u:
        bit = WINCE_PATH_PROBE_795D8;
        name = "warm_resume_split";
        break;
    case 0x80079634u:
        bit = WINCE_PATH_PROBE_79634;
        name = "warm_path_entry";
        break;
    case 0x80079730u:
        bit = WINCE_PATH_PROBE_79730;
        name = "warm_gpr_restore";
        break;
    case 0x80079898u:
        bit = WINCE_PATH_PROBE_79898;
        name = "low_power_helper";
        break;
    case 0x80079910u:
        bit = WINCE_PATH_PROBE_79910;
        name = "low_power_branch";
        break;
    case 0x80079990u:
        bit = WINCE_PATH_PROBE_79990;
        name = "suspend_wait";
        break;
    case 0x8007A3FCu:
        bit = WINCE_PATH_PROBE_7A3FC;
        name = "idle_dispatch";
        break;
    case 0x8007B398u:
        bit = WINCE_PATH_PROBE_7B398;
        name = "cold_kernel_entry";
        break;
    case 0x8007B57Cu:
        bit = WINCE_PATH_PROBE_7B57C;
        name = "cold_kernel_main";
        break;
    case 0x8008B21Cu:
        bit = WINCE_PATH_PROBE_8B21C;
        name = "scheduler_entry";
        break;
    case 0x8008B528u:
        bit = WINCE_PATH_PROBE_8B528;
        name = "scheduler_dispatch";
        break;
    case 0x800947C8u:
        bit = WINCE_PATH_PROBE_947C8;
        name = "kernel_init_ptoc";
        break;
    default:
        return;
    }

    if ((m->wince.boot_path_probe_mask & bit) != 0)
        return;
    m->wince.boot_path_probe_mask |= bit;

    pa2400 = load_pa_word(m, 0x2400u);
    pa2404 = load_pa_word(m, 0x2404u);
    pa2408 = load_pa_word(m, 0x2408u);
    pa250c = load_pa_word(m, 0x250Cu);
    pa2518 = load_pa_word(m, 0x2518u);
    pa251c = load_pa_word(m, 0x251Cu);
    pa2524 = load_pa_word(m, 0x2524u);
    pa2528 = load_pa_word(m, 0x2528u);
    pa254c = load_pa_word(m, 0x254Cu);
    pa2554 = load_pa_word(m, 0x2554u);
    pa2700 = load_pa_word(m, 0x2700u);

    set_watch_observer(m, false);
    vrc8010_ok = load_va_word(m, 0xAA008010u, &vrc8010);
    latch0004_ok = load_va_word(m, 0xAA000004u, &latch0004);
    a03c_ok = load_va_word(m, 0xAA00A03Cu, &a03c);
    nand_a0e0_ok = load_va_word(m, 0xAA00A0E0u, &nand_a0e0);
    vrc104c_ok = load_va_word(m, 0xAA00104Cu, &vrc104c);
    btn_ok = load_va_half(m, 0xAA00A042u, &btn_a042);
    btn044_ok = load_va_half(m, 0xAA00A044u, &btn_a044);
    btn07c_ok = load_va_half(m, 0xAA00A07Cu, &btn_a07c);
    vrc1054_ok = load_va_half(m, 0xAA001054u, &vrc1054);
    vrc1b50_ok = load_va_word(m, 0xAA001B50u, &vrc1b50);
    vrc1b58_ok = load_va_word(m, 0xAA001B58u, &vrc1b58);
    vrc08a0_ok = load_va_word(m, 0xAA0008A0u, &vrc08a0);
    vrc0a00_ok = load_va_word(m, 0xAA000A00u, &vrc0a00);
    vrc1120_ok = load_va_word(m, 0xAA001120u, &vrc1120);
    vrc112c_ok = load_va_word(m, 0xAA00112Cu, &vrc112c);
    vrc1134_ok = load_va_word(m, 0xAA001134u, &vrc1134);
    vrc130c_ok = load_va_word(m, 0xAA00130Cu, &vrc130c);
    vrc1b10_ok = load_va_word(m, 0xAA001B10u, &vrc1b10);
    vrc1b20_ok = load_va_word(m, 0xAA001B20u, &vrc1b20);
    vrc0300_ok = load_va_word(m, 0xAA000300u, &vrc0300);
    vrc0390_ok = load_va_word(m, 0xAA000390u, &vrc0390);
    lat8010_ok = load_vrc_latch_word(0x0A008010u, &lat8010);
    lat1120_ok = load_vrc_latch_word(0x0A001120u, &lat1120);
    lat112c_ok = load_vrc_latch_word(0x0A00112Cu, &lat112c);
    lat1b10_ok = load_vrc_latch_word(0x0A001B10u, &lat1b10);
    lat1b20_ok = load_vrc_latch_word(0x0A001B20u, &lat1b20);
    vr0100_ok = load_va_word(m, 0xAF000100u, &vr0100);
    vr0046_ok = load_va_half(m, 0xAF000046u, &vr0046);
    vr0104_ok = load_va_half(m, 0xAF000104u, &vr0104);
    vr0144_ok = load_va_half(m, 0xAF000144u, &vr0144);
    pmu_c0_ok = load_va_half(m, 0xAF0000C0u, &pmu_c0);
    set_watch_observer(m, true);

    fprintf(stderr,
        "[WINCE_PATH] %s PC=0x%08X Canon=0x%08X RA=0x%08X SP=0x%08X"
        " V0=0x%08X V1=0x%08X T0=0x%08X"
        " Status=0x%08X Cause=0x%08X EPC=0x%08X"
        " PA2400=0x%08X PA2404=0x%08X PA2408=0x%08X PA250C=0x%08X"
        " PA2518=0x%08X PA251C=0x%08X PA2524=0x%08X PA2528=0x%08X"
        " PA254C=0x%08X PA2554=0x%08X"
        " PA2700=0x%08X"
        " VRC8010=0x%s LATCH0004=0x%s BTN_A03C=0x%s NAND_A0E0=0x%s VRC104C=0x%s"
        " BTN_A042=0x%s BTN_A044=0x%s BTN_A07C=0x%s VRC1054=0x%s"
        " VRC08A0=0x%s VRC0A00=0x%s VRC1120=0x%s VRC112C=0x%s"
        " VRC1134=0x%s VRC130C=0x%s VRC1B10=0x%s VRC1B20=0x%s"
        " LAT8010=0x%s LAT1120=0x%s LAT112C=0x%s LAT1B10=0x%s LAT1B20=0x%s"
        " VRC0300=0x%s VRC0390=0x%s VRC1B50=0x%s VRC1B58=0x%s"
        " VR0046=0x%s VR0100=0x%s VR0104=0x%s VR0144=0x%s PMU_C0=0x%s\n",
        name,
        raw_pc32,
        pc32,
        (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
        (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
        (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_V0],
        (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_V1],
        (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_T0],
        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS],
        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE],
        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC],
        pa2400,
        pa2404,
        pa2408,
        pa250c,
        pa2518,
        pa251c,
        pa2524,
        pa2528,
        pa254c,
        pa2554,
        pa2700,
        format_word_or_unknown(b8010, sizeof(b8010), vrc8010_ok, vrc8010),
        format_word_or_unknown(b0004, sizeof(b0004), latch0004_ok, latch0004),
        format_word_or_unknown(ba03c, sizeof(ba03c), a03c_ok, a03c),
        format_word_or_unknown(ba0e0, sizeof(ba0e0), nand_a0e0_ok, nand_a0e0),
        format_word_or_unknown(b104c, sizeof(b104c), vrc104c_ok, vrc104c),
        format_half_or_unknown(ba042, sizeof(ba042), btn_ok, btn_a042),
        format_half_or_unknown(ba044, sizeof(ba044), btn044_ok, btn_a044),
        format_half_or_unknown(ba07c, sizeof(ba07c), btn07c_ok, btn_a07c),
        format_half_or_unknown(b1054, sizeof(b1054), vrc1054_ok, vrc1054),
        format_word_or_unknown(b08a0, sizeof(b08a0), vrc08a0_ok, vrc08a0),
        format_word_or_unknown(b0a00, sizeof(b0a00), vrc0a00_ok, vrc0a00),
        format_word_or_unknown(b1120, sizeof(b1120), vrc1120_ok, vrc1120),
        format_word_or_unknown(b112c, sizeof(b112c), vrc112c_ok, vrc112c),
        format_word_or_unknown(b1134, sizeof(b1134), vrc1134_ok, vrc1134),
        format_word_or_unknown(b130c, sizeof(b130c), vrc130c_ok, vrc130c),
        format_word_or_unknown(b1b10, sizeof(b1b10), vrc1b10_ok, vrc1b10),
        format_word_or_unknown(b1b20, sizeof(b1b20), vrc1b20_ok, vrc1b20),
        format_word_or_unknown(blat8010, sizeof(blat8010), lat8010_ok, lat8010),
        format_word_or_unknown(blat1120, sizeof(blat1120), lat1120_ok, lat1120),
        format_word_or_unknown(blat112c, sizeof(blat112c), lat112c_ok, lat112c),
        format_word_or_unknown(blat1b10, sizeof(blat1b10), lat1b10_ok, lat1b10),
        format_word_or_unknown(blat1b20, sizeof(blat1b20), lat1b20_ok, lat1b20),
        format_word_or_unknown(b0300, sizeof(b0300), vrc0300_ok, vrc0300),
        format_word_or_unknown(b0390, sizeof(b0390), vrc0390_ok, vrc0390),
        format_word_or_unknown(b1b50, sizeof(b1b50), vrc1b50_ok, vrc1b50),
        format_word_or_unknown(b1b58, sizeof(b1b58), vrc1b58_ok, vrc1b58),
        format_half_or_unknown(b0046, sizeof(b0046), vr0046_ok, vr0046),
        format_word_or_unknown(b0100, sizeof(b0100), vr0100_ok, vr0100),
        format_half_or_unknown(b0104, sizeof(b0104), vr0104_ok, vr0104),
        format_half_or_unknown(b0144, sizeof(b0144), vr0144_ok, vr0144),
        format_half_or_unknown(bpmu, sizeof(bpmu), pmu_c0_ok, pmu_c0));

    if (bit == WINCE_PATH_PROBE_947C8) {
        maybe_dump_toc_summary(m,
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_A0]);
    }

    /* Step 4: One-shot PSL dispatch table dump on scheduler_dispatch */
    if (bit == WINCE_PATH_PROBE_8B528) {
        unsigned pi;
        uint32_t trap_base = 0;

        /* KData+0xA0 (VA 0xFFFFD8A0) — system trap dispatch pointer */
        (void)load_va_word(m, 0xFFFFD8A0u, &trap_base);
        fprintf(stderr, "[PSL_TABLE] KData+0xA0=0x%08X (trap base)\n",
            trap_base);
        if (trap_base != 0) {
            for (pi = 0; pi < 32; pi++) {
                uint32_t entry = 0;
                (void)load_va_word(m, trap_base + pi * 4u, &entry);
                if (entry != 0 || pi < 4)
                    fprintf(stderr,
                        "[PSL_TABLE]   [%2u] = 0x%08X\n",
                        pi, entry);
            }
        }

        /* ROM callback table at PA 0x51680 (11 groups, 55 words) */
        fprintf(stderr, "[PSL_TABLE] ROM callbacks PA=0x51680:\n");
        for (pi = 0; pi < 55; pi += 5) {
            fprintf(stderr,
                "[PSL_TABLE]   cb[%u]: %08X %08X %08X %08X %08X\n",
                pi / 5,
                load_pa_word(m, 0x51680u + pi * 4u),
                load_pa_word(m, 0x51680u + (pi + 1u) * 4u),
                load_pa_word(m, 0x51680u + (pi + 2u) * 4u),
                load_pa_word(m, 0x51680u + (pi + 3u) * 4u),
                load_pa_word(m, 0x51680u + (pi + 4u) * 4u));
        }
    }
}

static void dump_pa_words(machine_t *m, const char *label, uint32_t pa,
    size_t word_count)
{
    size_t i;

    fprintf(stderr, "[WINCE_CKPT] %-14s PA=0x%08X", label, pa);
    for (i = 0; i < word_count; i++)
        fprintf(stderr, " %08X", load_pa_word(m, pa + (uint32_t)(i * 4u)));
    fprintf(stderr, "\n");
}

static void dump_ctx_window(machine_t *m, uint32_t pa, uint32_t size)
{
    uint32_t off;

    for (off = 0; off < size; off += 16u) {
        fprintf(stderr,
            "[WINCE_CKPT] ctx+0x%03X: %08X %08X %08X %08X\n",
            off,
            load_pa_word(m, pa + off + 0u),
            load_pa_word(m, pa + off + 4u),
            load_pa_word(m, pa + off + 8u),
            load_pa_word(m, pa + off + 12u));
    }
}

static void dump_va_window(machine_t *m, const char *label, uint32_t va,
    uint32_t size)
{
    uint32_t off;

    for (off = 0; off < size; off += 16u) {
        uint32_t w0 = 0;
        uint32_t w1 = 0;
        uint32_t w2 = 0;
        uint32_t w3 = 0;
        bool ok0 = load_va_word(m, va + off + 0u, &w0);
        bool ok1 = load_va_word(m, va + off + 4u, &w1);
        bool ok2 = load_va_word(m, va + off + 8u, &w2);
        bool ok3 = load_va_word(m, va + off + 12u, &w3);
        char b0[9];
        char b1[9];
        char b2[9];
        char b3[9];

        fprintf(stderr,
            "[WINCE_HANDLER] %s+0x%03X: %s %s %s %s\n",
            label,
            off,
            format_word_or_unknown(b0, sizeof(b0), ok0, w0),
            format_word_or_unknown(b1, sizeof(b1), ok1, w1),
            format_word_or_unknown(b2, sizeof(b2), ok2, w2),
            format_word_or_unknown(b3, sizeof(b3), ok3, w3));
    }
}

static void dump_code_window(machine_t *m, uint32_t pc, size_t before_words,
    size_t after_words)
{
    int rel;
    int start = -(int)before_words;
    int stop = (int)after_words;

    for (rel = start; rel <= stop; rel++) {
        uint32_t va = pc + (uint32_t)(rel * 4);
        uint32_t word = 0;
        bool ok = load_va_word(m, va, &word);
        char buf[9];

        fprintf(stderr,
            "[WINCE_CODE] %c rel=%+03d VA=0x%08X word=%s\n",
            rel == 0 ? '*' : ' ',
            rel * 4,
            va,
            format_word_or_unknown(buf, sizeof(buf), ok, word));
    }
}

static void dump_gpr_window(machine_t *m)
{
    size_t i;

    for (i = 0; i < 32; i += 4u) {
        fprintf(stderr,
            "[WINCE_REGS] %-3s=0x%08X %-3s=0x%08X %-3s=0x%08X %-3s=0x%08X\n",
            wince_gpr_names[i + 0u], (uint32_t)m->cpu->cd.mips.gpr[i + 0u],
            wince_gpr_names[i + 1u], (uint32_t)m->cpu->cd.mips.gpr[i + 1u],
            wince_gpr_names[i + 2u], (uint32_t)m->cpu->cd.mips.gpr[i + 2u],
            wince_gpr_names[i + 3u], (uint32_t)m->cpu->cd.mips.gpr[i + 3u]);
    }
}

/* ------------------------------------------------------------------ */
/*  Checkpoint logging                                                  */
/* ------------------------------------------------------------------ */

static bool log_checkpoint_header(machine_t *m, const char *tag,
    const char *detail)
{
    uint32_t status;
    uint32_t cause;
    uint32_t epc;
    uint32_t badvaddr;
    uint32_t wired;
    uint32_t entryhi;
    uint32_t pagemask;

    if (!m->wince.active)
        return false;

    status = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS];
    cause = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE];
    epc = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC];
    badvaddr = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR];
    wired = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_WIRED];
    entryhi = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_ENTRYHI];
    pagemask = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_PAGEMASK];

    fprintf(stderr,
        "[WINCE_CKPT] %s detail=%s PC=0x%08" PRIx64
        " Status=0x%08X Cause=0x%08X EPC=0x%08X BadVA=0x%08X"
        " Wired=0x%08X EntryHi=0x%08X PageMask=0x%08X"
        " owner=%d ready=%d\n",
        tag,
        detail ? detail : "-",
        (uint64_t)m->cpu->pc,
        status,
        cause,
        epc,
        badvaddr,
        wired,
        entryhi,
        pagemask,
        (int)m->wince.vector_owner,
        m->wince.vectors_ready ? 1 : 0);

    return true;
}

static void maybe_log_checkpoint(machine_t *m, const char *tag,
    const char *detail)
{
    if (!log_checkpoint_header(m, tag, detail))
        return;

    dump_pa_words(m, "vec_low_0000", 0x00000000u, WINCE_VECTOR_WORDS);
    dump_pa_words(m, "vec_low_0080", 0x00000080u, WINCE_VECTOR_WORDS);
    dump_pa_words(m, "vec_low_0100", 0x00000100u, WINCE_VECTOR_WORDS);
    dump_pa_words(m, "vec_low_0180", 0x00000180u, WINCE_VECTOR_WORDS);
    dump_pa_words(m, "vec_bev_0000", 0x1FC00000u, WINCE_VECTOR_WORDS);
    dump_pa_words(m, "vec_bev_0200", 0x1FC00200u, WINCE_VECTOR_WORDS);
    dump_pa_words(m, "vec_bev_0380", 0x1FC00380u, WINCE_VECTOR_WORDS);
    dump_ctx_window(m, 0x00002000u, 0x300u);

    /* Step 1: Compare installed low vectors against NK.exe source handlers.
     * TLB refill source: VA 0x8008C418, installed at PA 0x0000.
     * General exception source: VA 0x8008B240, installed at PA 0x0180. */
    {
        uint32_t pa_vec[WINCE_VECTOR_WORDS];
        uint32_t nk_src[WINCE_VECTOR_WORDS];
        size_t i;
        int mismatch;
        static const struct {
            uint32_t pa;
            uint32_t src_va;
            const char *name;
        } vec_pairs[] = {
            { 0x00000000u, 0x8008C418u, "tlb_refill" },
            { 0x00000180u, 0x8008B240u, "general_exc" },
        };
        unsigned vp;

        for (vp = 0; vp < 2; vp++) {
            read_block(m, vec_pairs[vp].pa, pa_vec, WINCE_VECTOR_WORDS);
            mismatch = -1;
            for (i = 0; i < WINCE_VECTOR_WORDS; i++) {
                uint32_t w;
                if (!load_va_word(m, vec_pairs[vp].src_va
                    + (uint32_t)(i * 4u), &w))
                    w = 0xDEADBEEFu;
                nk_src[i] = w;
                if (mismatch < 0 && w != pa_vec[i])
                    mismatch = (int)i;
            }
            fprintf(stderr,
                "[VECTOR_MATCH] %s: PA=0x%08X vs VA=0x%08X match=%s",
                vec_pairs[vp].name, vec_pairs[vp].pa,
                vec_pairs[vp].src_va,
                mismatch < 0 ? "YES" : "NO");
            if (mismatch >= 0)
                fprintf(stderr, " first_diff_word=%d"
                    " pa_val=0x%08X nk_val=0x%08X",
                    mismatch, pa_vec[mismatch],
                    nk_src[mismatch]);
            fprintf(stderr, "\n");
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Low-vector state machine                                            */
/* ------------------------------------------------------------------ */

static void scan_low_vectors(machine_t *m)
{
    uint32_t tlb[WINCE_VECTOR_WORDS];
    uint32_t general[WINCE_VECTOR_WORDS];
    bool tlb_real;
    bool general_real;
    bool ready;

    if (!m->wince.active)
        return;

    read_block(m, 0x00000000u, tlb, WINCE_VECTOR_WORDS);
    read_block(m, 0x00000180u, general, WINCE_VECTOR_WORDS);

    tlb_real = block_has_nonzero(tlb, WINCE_VECTOR_WORDS);
    general_real = block_has_nonzero(general, WINCE_VECTOR_WORDS);
    ready = tlb_real || general_real;

    if (m->wince.vector_owner == WINCE_VECTOR_NONE && ready) {
        m->wince.vector_owner = WINCE_VECTOR_GUEST;
        m->wince.low_vector_observed_valid = false;
        invalidate_all(m);
    }

    if (m->wince.vector_owner == WINCE_VECTOR_GUEST
        && ready && !m->wince.vectors_ready) {
        m->wince.vectors_ready = true;
        maybe_log_checkpoint(m, "vector_owner", "guest-low-vectors");
    }
}

/* ------------------------------------------------------------------ */
/*  Exception detection                                                 */
/* ------------------------------------------------------------------ */

static void maybe_note_first_exception(machine_t *m)
{
    uint32_t cause;
    uint32_t badvaddr;
    uint32_t pc_norm;

    if (!m->wince.active || !m->wince.cold_boot_redirected
        || m->wince.first_exception_logged)
        return;

    cause = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE];
    badvaddr = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR];
    pc_norm = (uint32_t)m->cpu->pc & 0x1FFFFFFFu;

    if (badvaddr >= 0x00000400u && pc_norm >= 0x00000400u)
        return;

    m->wince.first_exception_logged = true;
    maybe_log_checkpoint(m, "first_exception", "post-redirect");
    (void)cause;
}

/* ------------------------------------------------------------------ */
/*  Runtime low-vector drift tracking                                   */
/* ------------------------------------------------------------------ */

static void maybe_track_low_vector_runtime_changes(machine_t *m)
{
    uint32_t current_tlb[WINCE_VECTOR_WORDS];
    uint32_t current_general[WINCE_VECTOR_WORDS];
    bool tlb_changed;
    bool general_changed;

    if (!m || !m->wince.active || !m->wince.cold_boot_redirected
        || !m->wince.vectors_ready) {
        return;
    }

    read_block(m, 0x00000000u, current_tlb, WINCE_VECTOR_WORDS);
    read_block(m, 0x00000180u, current_general, WINCE_VECTOR_WORDS);

    if (!m->wince.low_vector_observed_valid) {
        read_block(m, 0x00000000u, m->wince.observed_low_tlb,
            WINCE_VECTOR_WORDS);
        read_block(m, 0x00000180u, m->wince.observed_low_general,
            WINCE_VECTOR_WORDS);
        m->wince.low_vector_observed_valid = true;
        return;
    }

    tlb_changed = !block_matches(current_tlb, m->wince.observed_low_tlb,
        WINCE_VECTOR_WORDS);
    general_changed = !block_matches(current_general,
        m->wince.observed_low_general, WINCE_VECTOR_WORDS);
    if (!tlb_changed && !general_changed)
        return;

    invalidate_all(m);
    if (m->wince.vector_owner != WINCE_VECTOR_GUEST)
        m->wince.vector_owner = WINCE_VECTOR_GUEST;

    if (!m->wince.low_vector_runtime_drift_logged) {
        uint32_t sp = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP];
        uint32_t pc = (uint32_t)m->cpu->pc;

        m->wince.low_vector_runtime_drift_logged = true;
        fprintf(stderr,
            "[WINCE_VECTOR_DRIFT] pc=0x%08X ra=0x%08X sp=0x%08X"
            " epc=0x%08X badva=0x%08X status=0x%08X cause=0x%08X"
            " entryhi=0x%08X tlb_changed=%d general_changed=%d\n",
            pc,
            (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
            sp,
            (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC],
            (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR],
            (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS],
            (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE],
            (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_ENTRYHI],
            tlb_changed ? 1 : 0,
            general_changed ? 1 : 0);
        fprintf(stderr,
            "[WINCE_VECTOR_DRIFT] old_low_0000=%08X/%08X/%08X/%08X"
            " new_low_0000=%08X/%08X/%08X/%08X\n",
            m->wince.observed_low_tlb[0],
            m->wince.observed_low_tlb[1],
            m->wince.observed_low_tlb[2],
            m->wince.observed_low_tlb[3],
            current_tlb[0],
            current_tlb[1],
            current_tlb[2],
            current_tlb[3]);
        fprintf(stderr,
            "[WINCE_VECTOR_DRIFT] old_low_0180=%08X/%08X/%08X/%08X"
            " new_low_0180=%08X/%08X/%08X/%08X\n",
            m->wince.observed_low_general[0],
            m->wince.observed_low_general[1],
            m->wince.observed_low_general[2],
            m->wince.observed_low_general[3],
            current_general[0],
            current_general[1],
            current_general[2],
            current_general[3]);
        dump_gpr_window(m);
        dump_code_window(m, pc, 4u, 12u);
        if (sp != 0)
            dump_va_window(m, "low_vector_drift_stack", sp - 0x40u, 0x80u);
        maybe_log_checkpoint(m, "low_vector_drift", "runtime-change");
    }

    memcpy(m->wince.observed_low_tlb, current_tlb,
        sizeof(m->wince.observed_low_tlb));
    memcpy(m->wince.observed_low_general, current_general,
        sizeof(m->wince.observed_low_general));
}

/* ------------------------------------------------------------------ */
/*  Cold-boot scheduler probes                                          */
/* ------------------------------------------------------------------ */

static bool cold_boot_scheduler_probe_pc_match(uint32_t value)
{
    return (value >= UINT32_C(0x80079900) && value <= UINT32_C(0x800799C0))
        || (value >= UINT32_C(0x80089760) && value <= UINT32_C(0x800897C0))
        || (value >= UINT32_C(0x80089870) && value <= UINT32_C(0x800898F8))
        || (value >= UINT32_C(0x8008B4F0) && value <= UINT32_C(0x8008B6C0))
        || (value >= UINT32_C(0x8007A3F0) && value <= UINT32_C(0x8007A6B0))
        || (value >= UINT32_C(0x80032780) && value <= UINT32_C(0x800328B8))
        || (value >= UINT32_C(0x80048D40) && value <= UINT32_C(0x80048D90))
        || (value >= UINT32_C(0x80068B00) && value <= UINT32_C(0x80068B30));
}

static bool cold_boot_scheduler_probe_epc_match(uint32_t value)
{
    return cold_boot_scheduler_probe_pc_match(value)
        || value == UINT32_C(0x80089A50)
        || value == UINT32_C(0x80048D6C);
}

static void maybe_log_cold_boot_scheduler_probe(machine_t *m,
    uint32_t sampled_pc)
{
    uint32_t pc;
    uint32_t epc;
    uint32_t sp;
    uint32_t s0;
    uint32_t s1;
    uint32_t status;
    uint32_t cause;
    uint32_t badva;

    if (!m || !m->wince.active
        || !m->wince.cold_boot_redirected) {
        return;
    }
    if ((m->wince.cold_boot_pc_probes_logged
        & WINCE_COLD_LATE_PROBE_LOGGED) != 0) {
        return;
    }

    pc = sampled_pc;
    epc = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC];
    badva = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR];
    if (!cold_boot_scheduler_probe_pc_match(pc)
        && !cold_boot_scheduler_probe_epc_match(epc)
        && badva != UINT32_C(0x0201FE2C)) {
        return;
    }

    m->wince.cold_boot_pc_probes_logged |= WINCE_COLD_LATE_PROBE_LOGGED;
    sp = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP];
    s0 = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_S0];
    s1 = (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_S1];
    status = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS];
    cause = (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE];

    fprintf(stderr,
        "[WINCE_COLD_LATE] pc=0x%08X epc=0x%08X sp=0x%08X"
        " s0=0x%08X s1=0x%08X status=0x%08X cause=0x%08X"
        " badva=0x%08X\n",
        pc,
        epc,
        sp,
        s0,
        s1,
        status,
        cause,
        badva);

    dump_gpr_window(m);
    dump_va_window(m, "cold_late_pc", pc & ~UINT32_C(0x1F), 0x60u);
    if (epc != 0)
        dump_va_window(m, "cold_late_epc", epc & ~UINT32_C(0x1F), 0x60u);
    dump_va_window(m, "cold_late_sched_961c", UINT32_C(0x8067961C), 0x20u);
    dump_va_window(m, "cold_late_sched_9654", UINT32_C(0x80679654), 0x20u);
    dump_va_window(m, "cold_late_sched_96d8", UINT32_C(0x806796D8), 0x20u);
    dump_va_window(m, "cold_late_sched_96f8", UINT32_C(0x806796F8), 0x20u);
    dump_va_window(m, "cold_late_sched_9780", UINT32_C(0x80679780), 0x20u);
    dump_va_window(m, "cold_late_obj_table", UINT32_C(0x8066BFC0), 0x40u);
    if (s0 != 0)
        dump_va_window(m, "cold_late_s0", s0 & ~UINT32_C(0x1F), 0x120u);
    if (s1 != 0)
        dump_va_window(m, "cold_late_s1", s1 & ~UINT32_C(0x1F), 0x60u);

    /* Scheduler globals from FUN_8008B528 / FUN_8007A3FC decompilation */
    dump_va_window(m, "sched_runlist",  UINT32_C(0x80669800), 0x100u);
    dump_va_window(m, "sched_kerndata", UINT32_C(0x80660000), 0x100u);
    dump_va_window(m, "sched_timers",   UINT32_C(0x8066BF80), 0x80u);
    dump_va_window(m, "sched_misc",     UINT32_C(0x80669500), 0x80u);
    /* OAL vtable at 0x8066BFC0 (pointed to by 0x80660000) */
    dump_va_window(m, "oal_vtable",     UINT32_C(0x8066BFC0), 0x40u);

    /* PC diversity analysis: scan pc_ring[] for distinct PCs */
    if (m->wince.pc_ring_active && m->wince.pc_ring_idx > 0) {
        uint32_t total = m->wince.pc_ring_idx;
        uint32_t count = total < WINCE_PC_RING_SIZE
            ? total : WINCE_PC_RING_SIZE;
        uint32_t distinct = 0;
        uint32_t usermode = 0;
        uint32_t last_non_idle = 0;
        uint32_t seen[64];
        uint32_t nseen = 0;

        for (uint32_t i = 0; i < count; i++) {
            uint32_t idx = (total - count + i) % WINCE_PC_RING_SIZE;
            uint32_t ring_pc = m->wince.pc_ring[idx];
            bool found = false;

            if (ring_pc < UINT32_C(0x80000000))
                usermode++;
            if (!((ring_pc >= UINT32_C(0x80079000)
                   && ring_pc < UINT32_C(0x800799FF))
                  || (ring_pc >= UINT32_C(0x8007A300)
                      && ring_pc < UINT32_C(0x8007A500))
                  || (ring_pc >= UINT32_C(0x8008B400)
                      && ring_pc < UINT32_C(0x8008B600))))
                last_non_idle = ring_pc;
            for (uint32_t j = 0; j < nseen; j++) {
                if (seen[j] == ring_pc) { found = true; break; }
            }
            if (!found && nseen < 64)
                seen[nseen++] = ring_pc;
        }
        distinct = nseen;
        fprintf(stderr,
            "[WINCE_COLD_LATE] pc_diversity: samples=%u distinct=%u"
            " usermode=%u last_non_idle=0x%08X\n",
            count, distinct, usermode, last_non_idle);
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

void wince_boot_attach_machine(machine_t *m)
{
    g_active_wince_machine = m;
}

void wince_boot_detach_machine(machine_t *m)
{
    if (g_active_wince_machine == m)
        g_active_wince_machine = NULL;
}

void wince_boot_init(machine_t *m)
{
    memset(&m->wince, 0, sizeof(m->wince));
    m->wince.active = (m->cfg.nand_path != NULL);
    m->wince.vector_owner = WINCE_VECTOR_NONE;
    m->wince.callback_slot_watch_pa = WINCE_CALLBACK_SLOT_CANDIDATE_PA;
}

void wince_boot_note_spl_handoff(machine_t *m)
{
    if (!m->wince.active || m->wince.spl_handoff_logged)
        return;
    m->wince.spl_handoff_logged = true;
    maybe_log_checkpoint(m, "spl_handoff", "nk-entry");
}

void wince_boot_note_cold_boot_redirect(machine_t *m, const char *detail)
{
    if (!m->wince.active)
        return;
    m->wince.cold_boot_redirected = true;
    maybe_log_checkpoint(m, "cold_boot_redirect", detail);
}

void wince_boot_note_first_exception(machine_t *m, const char *detail)
{
    if (!m->wince.active || m->wince.first_exception_logged)
        return;
    m->wince.first_exception_logged = true;
    maybe_log_checkpoint(m, "first_exception", detail);
}

void wince_boot_note_fatal_stop(machine_t *m, const char *reason)
{
    if (!m->wince.active || m->wince.fatal_exit_logged)
        return;
    m->wince.fatal_exit_logged = true;
    maybe_log_checkpoint(m, "fatal_stop", reason);
}

/*
 *  ROM DMA polling function intercept.
 *
 *  The ROM's MIPS16 function at 0x9FC013F0 polls the DMA controller for
 *  completion.  On real hardware, the VRC4173 DMA autonomously transfers
 *  NAND page data to a RAM buffer.  Our emulator only supports CPU-mediated
 *  FIFO reads, so we simulate the autonomous transfer by copying the current
 *  NAND page directly to the buffer address passed in $a0.
 *
 *  This fires on every call to 0x13F0, including the first call (where the
 *  ROM also reads via FIFO).  The FIFO read will overwrite our copy with
 *  identical data, so there's no conflict.
 */
#define ROM_DMA_POLL_PC  0x9FC013F0u
#define DMA_AUTOCOPY_MAX 20
static int dma_autocopy_count = 0;

static void maybe_dma_autocopy(machine_t *m, struct cpu *cpu)
{
    uint32_t pc32 = (uint32_t)cpu->pc;
    if (pc32 != ROM_DMA_POLL_PC)
        return;

    /* The DMA autocopy approach was investigated and found to be wrong.
     * The delay slot of the JAL to 0x13F0 overwrites $a0 with $s0,
     * so all three calls use buffer 0x80010060 (not 0x80008084).
     * The callback table at 0x80008080 is populated by the processing
     * code at 0x1160-0x11A6, not by DMA transfer.
     * Keeping the function entry detection for future diagnostics. */
    return; /* autocopy disabled — wrong approach */

    if (!m->nand.dma_active)
        return;
    uint32_t buf_va = (uint32_t)cpu->cd.mips.gpr[4];
    uint32_t buf_pa = buf_va & 0x1FFFFFFFu;
    if (buf_pa < 0x8000u || buf_pa >= 0x9000u)
        return;
    if (dma_autocopy_count >= DMA_AUTOCOPY_MAX)
        return;
    dma_autocopy_count++;

    uint32_t nand_addr = m->nand.dma_nand_addr;
    uint32_t nbytes = m->nand.dma_total_bytes;
    if (nbytes > 2048) nbytes = 2048;  /* sanity cap */

    fprintf(stderr,
        "[DMA_AUTOCOPY] PC=0x%08X buf=0x%08X nand=0x%06X len=%u"
        " cursor=%u/%u #%d\n",
        pc32, buf_va, nand_addr, nbytes,
        m->nand.dma_cursor, m->nand.dma_total_bytes,
        dma_autocopy_count);

    /* Copy NAND data to the CPU-visible buffer.
     * This simulates autonomous DMA transfer — the hardware copies
     * NAND page data to RAM without CPU intervention.
     * We do NOT change dma_cursor so that FIFO-based reads (used by
     * the first DMA call) continue to work normally. */
    for (uint32_t i = 0; i < nbytes; i++) {
        uint8_t byte;
        if (m->nand.image && (nand_addr + i) < m->nand.image_size)
            byte = m->nand.image[nand_addr + i];
        else
            byte = 0xFFu;
        cpu->memory_rw(cpu, cpu->mem,
            (uint64_t)(buf_va + i), &byte, 1,
            MEM_WRITE, CACHE_DATA);
    }
}

/*
 *  Called from the MIPS16 slow interpreter on every ROM instruction.
 *  Checks if we're at the DMA polling function entry and performs
 *  the autonomous DMA copy if needed.
 */
void wince_boot_check_dma_autocopy(struct cpu *cpu)
{
    machine_t *m = g_active_wince_machine;
    if (!m || !m->wince.active)
        return;
    maybe_dma_autocopy(m, cpu);

    /* Callback table frame copy removed — it was a workaround that
     * prevented the immediate epilogue crash but provided wrong values
     * (saved registers instead of callback function pointers), causing
     * the boot dispatcher to jump to invalid addresses.  The real fix
     * needs the ROM processing code at 0x1160-0x11A6 to correctly
     * populate the callback table at PA 0x8080. */
}

/*
 *  Splash display probe: log calls to OAL display dispatcher at 0x80078E10.
 *  a0=10 → clear screen, a0=0 → "Starting...", a0=6 → "Initializing..."
 */
static void maybe_log_splash_dispatch(machine_t *m, struct cpu *cpu)
{
    static int splash_count = 0;
    uint32_t pc = (uint32_t)cpu->pc & 0x1FFFFFFFu;

    if (pc != (0x80078E10u & 0x1FFFFFFFu) || splash_count >= 20)
        return;
    splash_count++;
    fprintf(stderr, "[SPLASH] call #%d a0=%u PC=0x%08X RA=0x%08X\n",
        splash_count,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0],
        (uint32_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
}

/*
 *  Debug serial: intercept MMIO writes to the VRC4173 SIU and VR4131 SIU
 *  that carry debug output from NK.exe. Called from the MMIO access observer
 *  whenever a write hits the SIU range. Also dumps the OAL debug function
 *  pointer at VA 0x80660084 once during init.
 *
 *  Additionally, on each tick, check if PC is at known debug output functions
 *  (0x8007B670 = OEMWriteDebugString, 0x8007B8C8 = NKDbgPrintfW) and capture
 *  the wide string from $a0.
 */
static void maybe_capture_debug_serial(machine_t *m, struct cpu *cpu)
{
    static int dbg_count = 0;
    static bool ptr_dumped = false;
    uint32_t pc;

    /* One-shot: dump the OAL debug output function pointer */
    if (m->cfg.debug_serial && !ptr_dumped && m->wince.cold_boot_copy_done) {
        uint32_t fptr = 0;
        ptr_dumped = true;
        (void)load_va_word(m, UINT32_C(0x80660084), &fptr);
        fprintf(stderr, "[DBGSERIAL] OAL debug func ptr @0x80660084 = 0x%08X\n",
            fptr);
    }

    pc = (uint32_t)cpu->pc & 0x1FFFFFFFu;

    /* Catch OEMWriteDebugString (0x8007B670) — takes wchar_t * in $a0 */
    if (pc == (0x8007B670u & 0x1FFFFFFFu) && dbg_count < 500) {
        uint32_t a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
        if (a0 != 0) {
            char ascii[128];

            if (load_utf16_ascii(m, cpu, a0, ascii, sizeof(ascii))) {
                maybe_log_ppsh_debug_message(m, cpu, "OEMWriteDebugString",
                    a0, ascii);
                if (m->cfg.debug_serial) {
                    dbg_count++;
                    printf("[DBGSERIAL] %s\n", ascii);
                    fflush(stdout);
                }
            }
        }
    }

    /* Also catch NKDbgPrintfW (0x8007B8C8) — format string in $a0 */
    if (pc == (0x8007B8C8u & 0x1FFFFFFFu) && dbg_count < 500) {
        uint32_t a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
        if (a0 != 0) {
            char ascii[128];

            if (load_utf16_ascii(m, cpu, a0, ascii, sizeof(ascii))) {
                maybe_log_ppsh_debug_message(m, cpu, "NKDbgPrintfW",
                    a0, ascii);
                if (m->cfg.debug_serial) {
                    dbg_count++;
                    printf("[DBGSERIAL] %s\n", ascii);
                    fflush(stdout);
                }
            }
        }
    }
}

void wince_boot_on_vr41xx_tick(struct machine *gxm, struct cpu *cpu)
{
    machine_t *m = wince_boot_from_gx(gxm);

    if (!m || !m->wince.active)
        return;

    /* Detect NK.exe code execution during machine_run (the per-batch
     * check in be300_run_batch misses NK.exe entry because it can enter
     * and crash within a single batch). */
    if (!m->wince.cold_boot_copy_done && m->nand_data) {
        uint32_t pc32 = (uint32_t)cpu->pc;
        uint32_t pa = pc32 & 0x1FFFFFFFu;
        if (pa >= 0x60000u && pa < 0x100000u) {
            m->wince.cold_boot_copy_done = true;
            m->wince.cold_boot_redirected = true;
            fprintf(stderr,
                "[WINCE_CKPT] nk_entry_detected PC=0x%08X PA=0x%08X"
                " SP=0x%08X RA=0x%08X Status=0x%08X\n",
                pc32, pa,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
                (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_STATUS]);
            /* Dump PA 0x2700 boot signature (FUN_80079E48 checks this) */
            fprintf(stderr, "[WINCE_CKPT] boot_sig PA=0x2700:");
            {
                size_t i;
                for (i = 0; i < 16; i++)
                    fprintf(stderr, " %02X",
                        load_pa_word(m, 0x2700u + (uint32_t)(i * 4u))
                            & 0xFFu);
            }
            fprintf(stderr, "\n");
            dump_pa_words(m, "resume_ctx", 0x00002200u, 8);
            /* Activate PC ring for diversity analysis */
            if (!m->wince.pc_ring_active)
                wince_boot_pc_ring_activate(m);
        }
    }

    /* PC ring buffer — sample every tick once PA 0x24FC is detected */
    if (m->wince.pc_ring_active) {
        uint32_t idx = m->wince.pc_ring_idx % WINCE_PC_RING_SIZE;
        m->wince.pc_ring[idx] = (uint32_t)cpu->pc;
        m->wince.pc_ring_sp[idx] =
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
        m->wince.pc_ring_status[idx] =
            (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_STATUS];
        m->wince.pc_ring_idx++;
    }

    maybe_log_splash_dispatch(m, cpu);
    maybe_capture_debug_serial(m, cpu);
    maybe_log_boot_path_probe(m, (uint32_t)cpu->pc);
    scan_low_vectors(m);
    maybe_track_low_vector_runtime_changes(m);
    maybe_note_first_exception(m);
    maybe_log_cold_boot_scheduler_probe(m, (uint32_t)cpu->pc);
    if (!m->wince.toc_dumped && m->wince.cold_boot_copy_done) {
        uint32_t ptoc = 0;

        if (try_discover_ptoc(m, &ptoc))
            maybe_dump_toc_summary(m, ptoc);
    }
    if (!m->wince.fb_watch_armed) {
        uint32_t pc32 = (uint32_t)cpu->pc;

        if (pc32 > 0x1000u && pc32 < 0x80000000u)
            maybe_arm_fb_watch(m, cpu, "first_usermode_pc");
        else if ((m->wince.cold_boot_pc_probes_logged
                & WINCE_COLD_LATE_PROBE_LOGGED) != 0) {
            maybe_arm_fb_watch(m, cpu, "cold_late_probe");
        }
    }
    maybe_track_fb_runtime_changes(m, cpu);
    maybe_sample_systempatch_thread_context(m, cpu);

    /* PPSH flag-word sampling: poll PA 0x66001C (VA 0x8066001C) as a
     * secondary state signal. Dyntrans fast-path stores can bypass the
     * RAM observer, so the tick hook remains the reliable transition view. */
    if (m->wince.cold_boot_copy_done) {
        uint32_t flag = load_pa_word(m, 0x66001Cu);
        if (flag != m->wince.ppsh_flag_prev) {
            if (m->wince.ppsh_flag_prev == 0 && flag == 1)
                m->wince.ppsh_flag_set_count++;
            else if (m->wince.ppsh_flag_prev == 1 && flag == 0)
                m->wince.ppsh_flag_clear_count++;

            m->wince.ppsh_flag_transition_count++;
            if (m->wince.ppsh_flag_transition_count <= 50) {
            fprintf(stderr,
                "[PPSH_FLAG] PA 0x66001C: %u→%u #%d"
                " PC=0x%08X instrs=%llu\n",
                    m->wince.ppsh_flag_prev, flag,
                    (int)m->wince.ppsh_flag_transition_count,
                (uint32_t)cpu->pc,
                (unsigned long long)cpu->ninstrs);
            }
            maybe_dump_ppsh_flag_context(m, cpu, m->wince.ppsh_flag_prev, flag);
            if (m->wince.ppsh_flag_prev == 1 && flag == 0) {
                if (m->wince.ppsh_poll_active)
                    ppsh_close_poll_episode(m, cpu, (uint32_t)cpu->pc,
                        "flag_clear");
                fprintf(stderr,
                    "[PPSH_FLAG] *** FLAG CLEARED — dumping PC ring ***\n");
                dump_recent_pc_ring(m, "ppsh_flag_cleared", 64);
            }
            m->wince.ppsh_flag_prev = flag;
        }
    }

    /* PIU pen-state change detection (generates VRIP PIU interrupt) */
    {
        extern void be300_touch_tick(machine_t *m);
        be300_touch_tick(m);
    }
}

void wince_boot_note_timer_config(struct machine *gxm, struct cpu *cpu,
    uint64_t relative_addr, uint64_t value)
{
    machine_t *m = wince_boot_from_gx(gxm);
    (void)cpu;

    if (!m || !m->wince.active || m->wince.timer_config_logged)
        return;

    m->wince.timer_config_logged = true;
    maybe_log_checkpoint(m, "timer_config", "vr41xx-rtcl1");
    {
        fprintf(stderr,
            "[WINCE_CKPT] timer_register off=0x%03" PRIx64
            " value=0x%04" PRIx64 "\n",
            relative_addr, value);
    }
}

static bool is_idle_exception_focus_pc(uint32_t raw_pc32)
{
    uint32_t pc32 = raw_pc32;

    if ((pc32 & 0xE0000000u) == 0x80000000u
        || (pc32 & 0xE0000000u) == 0xA0000000u) {
        pc32 = (pc32 & 0x1FFFFFFFu) | 0x80000000u;
    }

    return (pc32 >= UINT32_C(0x80079900) && pc32 <= UINT32_C(0x80079A20))
        || (pc32 >= UINT32_C(0x8007A3E0) && pc32 <= UINT32_C(0x8007A460))
        || (pc32 >= UINT32_C(0x8008B200) && pc32 <= UINT32_C(0x8008B560));
}

void wince_boot_note_interrupt_exception(struct cpu *cpu, uint32_t exccode)
{
    machine_t *m;

    if (!cpu || exccode != EXCEPTION_INT)
        return;

    m = wince_boot_from_gx(cpu->machine);
    if (!m || !m->wince.active || !m->wince.cold_boot_redirected
        || !m->wince.vectors_ready || m->wince.irq_exception_snapshot_logged
        || !is_idle_exception_focus_pc((uint32_t)cpu->pc)) {
        return;
    }

    m->wince.irq_exception_snapshot_logged = true;
    fprintf(stderr,
        "[WINCE_IRQ] entry pc=0x%08X at=0x%08X t6=0x%08X sp=0x%08X"
        " ra=0x%08X status=0x%08X cause=0x%08X epc=0x%08X"
        " delay=%d owner=%d ready=%d\n",
        (uint32_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_AT],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T6],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
        (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_STATUS],
        (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_CAUSE],
        (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_EPC],
        cpu->delay_slot,
        (int)m->wince.vector_owner,
        m->wince.vectors_ready ? 1 : 0);
    dump_code_window(m, (uint32_t)cpu->pc, 4u, 10u);
    dump_pa_words(m, "low_0180", 0x00000180u, WINCE_VECTOR_WORDS);
}

void wince_boot_note_tlb_exception(struct cpu *cpu, uint32_t exccode,
    uint32_t vaddr)
{
    machine_t *m;
    uint32_t status;
    uint32_t cause;
    uint32_t epc;
    uint32_t badvaddr;

    if (!cpu)
        return;

    m = wince_boot_from_gx(cpu->machine);
    if (!m || !m->wince.active || !m->wince.cold_boot_redirected)
        return;

    status = (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_STATUS];

    /* Log TLB exceptions for DLL VA range (0x01800000-0x02000000) */
    if (vaddr >= 0x01800000u && vaddr < 0x02000000u) {
        static int dll_tlb_count = 0;
        if (dll_tlb_count < 20) {
            uint32_t entryhi = (uint32_t)cpu->cd.mips.coproc[0]
                ->reg[COP0_ENTRYHI];
            uint32_t ctx = (uint32_t)cpu->cd.mips.coproc[0]
                ->reg[COP0_CONTEXT];
            uint32_t sec_idx = (vaddr >> 25) & 0x3Fu;
            uint32_t sec_val = 0;
            (void)load_va_word(m, 0xFFFFD8C0u + sec_idx * 4u, &sec_val);
            fprintf(stderr,
                "[DLL_TLB] exc=%u vaddr=0x%08X pc=0x%08X"
                " sp=0x%08X asid=%u bev=%u exl=%u"
                " sec[%u]=0x%08X ctx=0x%08X #%d\n",
                exccode, vaddr, (uint32_t)cpu->pc,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
                entryhi & 0xFFu,
                (status >> 22) & 1u,
                (status >> 1) & 1u,
                sec_idx, sec_val, ctx,
                dll_tlb_count);
            dll_tlb_count++;
        }
    }
    /* Also log TLB misses/MOD for ALL user-mode VAs */
    if (vaddr < 0x80000000u) {
        static int user_tlb_count = 0;
        static int section_table_dumped = 0;
        if (user_tlb_count < 20 || (exccode == 1 && user_tlb_count < 50)) {
            uint32_t entryhi = (uint32_t)cpu->cd.mips.coproc[0]
                ->reg[COP0_ENTRYHI];
            uint32_t ctx = (uint32_t)cpu->cd.mips.coproc[0]
                ->reg[COP0_CONTEXT];
            uint32_t sec_idx = (vaddr >> 25) & 0x3Fu;
            uint32_t sec_val = 0;
            (void)load_va_word(m, 0xFFFFD8C0u + sec_idx * 4u, &sec_val);
            fprintf(stderr,
                "[USER_TLB] exc=%u vaddr=0x%08X pc=0x%08X"
                " sp=0x%08X asid=%u bev=%u exl=%u"
                " sec[%u]=0x%08X ctx=0x%08X #%d\n",
                exccode, vaddr, (uint32_t)cpu->pc,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
                entryhi & 0xFFu,
                (status >> 22) & 1u,
                (status >> 1) & 1u,
                sec_idx, sec_val, ctx,
                user_tlb_count);
            /* If section entry is valid, dump 2nd-level PTE.
             * Handler algorithm (from 0x8008C418):
             *   L2 byte offset = (badva >> 14) & 0x7FC
             *   L2 entry = *(section_entry + L2_offset)
             *   PTE group offset = (badva >> 10) & 0x38
             *   EntryLo0 = *(L2_entry + PTE_offset + 12)
             *   EntryLo1 = *(L2_entry + PTE_offset + 16) */
            if (sec_val != 0 && user_tlb_count < 10) {
                uint32_t l2_boff = (vaddr >> 14) & 0x7FCu;
                uint32_t l2_ptr = sec_val + l2_boff;
                uint32_t l2_val = 0;
                (void)load_va_word(m, l2_ptr, &l2_val);
                fprintf(stderr,
                    "[USER_TLB_L2] vaddr=0x%08X sec=0x%08X"
                    " l2_off=0x%X l2_val=0x%08X"
                    " (at VA 0x%08X) valid=%d\n",
                    vaddr, sec_val, l2_boff, l2_val,
                    l2_ptr, (int32_t)l2_val < 0);
                /* If L2 entry is valid (bit 31 set), dump PTE */
                if ((int32_t)l2_val < 0) {
                    uint32_t pte_off = (vaddr >> 10) & 0x38u;
                    uint32_t lo0 = 0, lo1 = 0, blk0 = 0;
                    uint32_t rmask = 0;
                    (void)load_va_word(m, l2_val, &blk0);
                    (void)load_va_word(m, 0xFFFFD89Cu,
                        &rmask);
                    (void)load_va_word(m,
                        l2_val + pte_off + 12u, &lo0);
                    (void)load_va_word(m,
                        l2_val + pte_off + 16u, &lo1);
                    fprintf(stderr,
                        "[USER_TLB_PTE] l2=0x%08X"
                        " pte_off=0x%X blk0=0x%08X"
                        " mask=0x%08X"
                        " lo0=0x%08X lo1=0x%08X\n",
                        l2_val, pte_off, blk0,
                        rmask, lo0, lo1);
                }
            }
            user_tlb_count++;
        }

        /* Step 2: One-shot section table dump on first user-space TLB miss.
         * Handler uses (badva >> 25) & 0x3F as section index.
         * Each section = 32MB. Process slots = 32MB each.
         * Slot 0 = section[0], slot 1 = section[1], etc. */
        if (!section_table_dumped) {
            unsigned si;
            section_table_dumped = 1;
            fprintf(stderr,
                "[SECTION_TABLE] dump at first user-space TLB miss"
                " (vaddr=0x%08X, section=%u):\n",
                vaddr, (vaddr >> 25) & 0x3Fu);
            for (si = 0; si < 64; si++) {
                uint32_t sv = 0;
                (void)load_va_word(m, 0xFFFFD8C0u + si * 4u, &sv);
                if (sv != 0 || si < 4)
                    fprintf(stderr,
                        "[SECTION_TABLE]   [%2u] = 0x%08X%s\n",
                        si, sv,
                        (si == 0) ? " (slot0, 0x00-0x01FFFFFF)" :
                        (si == 1) ? " (slot1, 0x02-0x03FFFFFF)" :
                        (si == 2) ? " (slot2, 0x04-0x05FFFFFF)" :
                        (si == 3) ? " (slot3, 0x06-0x07FFFFFF)" :
                        "");
            }
        }

        /* Step 5 validation: Context register BadVPN2 formula check */
        {
            static int ctx_check_count = 0;
            if (ctx_check_count < 5) {
                uint32_t v = vaddr;
                uint32_t old_ctx = ((((v & 0xFFFFE000u) >> 11)
                    << 4) & 0x01fffff0u);
                uint32_t new_ctx = (((v >> 11) << 4)
                    & 0x01fffff0u);
                fprintf(stderr,
                    "[CONTEXT_CHECK] vaddr=0x%08X"
                    " old_formula=0x%08X new_formula=0x%08X"
                    " diff=%d\n",
                    v, old_ctx, new_ctx,
                    old_ctx != new_ctx);
                ctx_check_count++;
            }
        }
    }

    m->wince.tlb_fault_snapshot_logged = true;
    cause = (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_CAUSE];
    epc = (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_EPC];
    badvaddr = (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR];

    fprintf(stderr,
        "[WINCE_TLB] first_exception exc=%u pc=0x%08X epc=0x%08X"
        " vaddr=0x%08X badvaddr=0x%08X status=0x%08X cause=0x%08X"
        " owner=%d ready=%d sp=0x%08X ra=0x%08X\n",
        exccode,
        (uint32_t)cpu->pc,
        epc,
        vaddr,
        badvaddr,
        status,
        cause,
        (int)m->wince.vector_owner,
        m->wince.vectors_ready ? 1 : 0,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);

    dump_live_tlb(m);
    dump_tlb_match_for_va(m, vaddr, "fault_va");
    if (same_4k_page(vaddr, UINT32_C(0x01F8F8F8)))
        maybe_log_hot_page_verdict(m, cpu, UINT32_C(0x01F8F8F8), vaddr,
            exccode, "first_exception");
    if (same_4k_page(vaddr, UINT32_C(0x01F94B50)))
        maybe_log_hot_page_verdict(m, cpu, UINT32_C(0x01F94B50), vaddr,
            exccode, "first_exception");
    if (same_4k_page(vaddr, UINT32_C(0x02041FA8)))
        maybe_log_hot_page_verdict(m, cpu, UINT32_C(0x02041FA8), vaddr,
            exccode, "first_exception");
    dump_tlb_match_for_va(m, UINT32_C(0xFFFFD000), "helper_high");
    dump_tlb_match_for_va(m, UINT32_C(0xFFFFDAC0), "ctx_ptr");
    dump_tlb_match_for_va(m, UINT32_C(0xFFFFDAB0), "ctx_link");
    dump_tlb_match_for_va(m, UINT32_C(0xFFFFBFA8), "nested_stack");
    dump_tlb_match_for_va(m, UINT32_C(0xFFFFC09C), "vector_save");
    dump_tlb_match_for_va(m, UINT32_C(0x1FFFD794), "queue_slot");

    dump_va_peek(m, "ctx_ptr", UINT32_C(0xFFFFDAC0));
    dump_va_peek(m, "ctx_link", UINT32_C(0xFFFFDAB0));
    dump_va_peek(m, "cause_mask", UINT32_C(0xFFFFD890));
    dump_va_peek(m, "saved_t0", UINT32_C(0xFFFFD888));
    dump_va_peek(m, "nest_count", UINT32_C(0xFFFFD884));
    dump_va_peek(m, "refill_tblf", UINT32_C(0xFFFFD8FC));
    dump_va_peek(m, "refill_tbl0", UINT32_C(0xFFFFD8C0));
    dump_va_peek(m, "refill_tbl1", UINT32_C(0xFFFFD8C4));
    dump_va_peek(m, "refill_mask", UINT32_C(0xFFFFD89C));
    dump_va_peek(m, "db00", UINT32_C(0xFFFFDB00));
    dump_va_peek(m, "db04", UINT32_C(0xFFFFDB04));
    dump_va_peek(m, "db08", UINT32_C(0xFFFFDB08));
    dump_va_peek(m, "db0c", UINT32_C(0xFFFFDB0C));
    dump_va_peek(m, "db1c", UINT32_C(0xFFFFDB1C));
    dump_va_peek(m, "db20", UINT32_C(0xFFFFDB20));
    dump_va_peek(m, "db34", UINT32_C(0xFFFFDB34));
    dump_va_peek(m, "mm_first", UINT32_C(0x80669540));
    dump_va_peek(m, "mm_last", UINT32_C(0x80669544));
    dump_va_peek(m, "queue_count", UINT32_C(0x80669548));
    dump_va_peek(m, "queue_base", UINT32_C(0x8066954C));
    dump_va_peek(m, "queue_head", UINT32_C(0x80669554));
    dump_va_peek(m, "cur_thrd", UINT32_C(0x80669844));

    dump_pa_words(m, "pa_18c0", 0x000018C0u, 16);
    dump_va_window(m, "refill_tbl", UINT32_C(0xFFFFD8C0), 0x40u);
    dump_va_window(m, "db_state", UINT32_C(0xFFFFDB00), 0x40u);
    dump_va_window(m, "mm_state", UINT32_C(0x80669540), 0x20u);
}

void wince_boot_note_tlb_exception_post(struct cpu *cpu, uint32_t exccode,
    uint32_t vaddr)
{
    machine_t *m;
    struct mips_coproc *cp0;
    uint32_t section_idx;
    uint32_t section_val;
    uint32_t l2_off;
    uint32_t l2_val;
    uint32_t pte_off;
    uint32_t lo0 = 0;
    uint32_t lo1 = 0;
    uint32_t selected_lo = 0;
    uint32_t repeat;

    if (!cpu)
        return;

    m = wince_boot_from_gx(cpu->machine);
    if (!m || !m->wince.active || !m->wince.cold_boot_redirected)
        return;
    if (vaddr >= 0x80000000u && vaddr < 0xFFFF0000u)
        return;

    cp0 = cpu->cd.mips.coproc[0];
    if (!cp0)
        return;

    if (m->wince.last_tlb_post_vaddr == vaddr
        && m->wince.last_tlb_post_pc == (uint32_t)cpu->pc) {
        if (m->wince.last_tlb_post_repeat < UINT16_MAX)
            m->wince.last_tlb_post_repeat++;
    } else {
        m->wince.last_tlb_post_vaddr = vaddr;
        m->wince.last_tlb_post_pc = (uint32_t)cpu->pc;
        m->wince.last_tlb_post_repeat = 1;
    }
    repeat = m->wince.last_tlb_post_repeat;

    if (m->wince.tlb_post_diag_count >= 160 && repeat != 4
        && repeat != 16 && repeat != 64)
        return;

    section_idx = (vaddr >> 25) & 0x3Fu;
    section_val = load_pa_word(m, 0x18C0u + section_idx * 4u);
    l2_off = (vaddr >> 14) & 0x7FCu;
    l2_val = section_val != 0 ? load_table_word(m, section_val, l2_off) : 0;
    pte_off = (vaddr >> 10) & 0x38u;
    if ((int32_t)l2_val < 0) {
        lo0 = load_table_word(m, l2_val, pte_off + 12u);
        lo1 = load_table_word(m, l2_val, pte_off + 16u);
        selected_lo = ((vaddr >> 12) & 1u) != 0 ? lo1 : lo0;
        if (!(selected_lo & ENTRYLO_V))
            selected_lo = (lo0 & ENTRYLO_V) ? lo0 : lo1;
    }

    fprintf(stderr,
        "[WINCE_TLB_POST] exc=%u fault=0x%08X vector_pc=0x%08X"
        " epc=0x%08X badvaddr=0x%08X entryhi=0x%08X"
        " context=0x%08X status=0x%08X cause=0x%08X asid=%u"
        " sec[%u]=0x%08X l2_off=0x%03X l2=0x%08X"
        " pte_off=0x%02X lo0=0x%08X lo1=0x%08X repeat=%u"
        " sp=0x%08X ra=0x%08X\n",
        exccode,
        vaddr,
        (uint32_t)cpu->pc,
        (uint32_t)cp0->reg[COP0_EPC],
        (uint32_t)cp0->reg[COP0_BADVADDR],
        (uint32_t)cp0->reg[COP0_ENTRYHI],
        (uint32_t)cp0->reg[COP0_CONTEXT],
        (uint32_t)cp0->reg[COP0_STATUS],
        (uint32_t)cp0->reg[COP0_CAUSE],
        (unsigned)((uint32_t)cp0->reg[COP0_ENTRYHI] & 0xFFu),
        section_idx,
        section_val,
        l2_off,
        l2_val,
        pte_off,
        lo0,
        lo1,
        repeat,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);

    maybe_log_serial_exception_correlation(m, cpu, exccode, vaddr, "post");
    if (same_4k_page(vaddr, UINT32_C(0x01F8F8F8))
        || (m->wince.serial_exc_last.bva_valid
            && same_4k_page(m->wince.serial_exc_last.bva,
                UINT32_C(0x01F8F8F8)))) {
        maybe_log_hot_page_verdict(m, cpu, UINT32_C(0x01F8F8F8), vaddr,
            exccode, "post_fault");
    }
    if (same_4k_page(vaddr, UINT32_C(0x01F94B50))
        || (m->wince.serial_exc_last.bva_valid
            && same_4k_page(m->wince.serial_exc_last.bva,
                UINT32_C(0x01F94B50)))) {
        maybe_log_hot_page_verdict(m, cpu, UINT32_C(0x01F94B50), vaddr,
            exccode, "post_fault");
    }
    if (same_4k_page(vaddr, UINT32_C(0x02041FA8))
        || (m->wince.serial_exc_last.bva_valid
            && same_4k_page(m->wince.serial_exc_last.bva,
                UINT32_C(0x02041FA8)))) {
        maybe_log_hot_page_verdict(m, cpu, UINT32_C(0x02041FA8), vaddr,
            exccode, "post_fault");
    }

    if (section_val != 0 && m->wince.tlb_post_diag_count < 32)
        log_l2_table_state(m, "post_fault", section_val, vaddr);
    if (same_4k_page(vaddr, UINT32_C(0x01FE6550))
        && selected_lo != 0) {
        maybe_log_callback_slot_state(m, cpu, "tlb_post_01fe6550",
            selected_lo);
    }

    if (!m->wince.exc_slot1_dumped
        && ((vaddr >= UINT32_C(0x03FE6540) && vaddr < UINT32_C(0x03FE6580))
            || vaddr == UINT32_C(0x01FE6558))) {
        uint32_t epc_v = (uint32_t)cp0->reg[COP0_EPC];
        m->wince.exc_slot1_dumped = true;
        fprintf(stderr,
            "[WINCE_CB_FAULT] exc=%u vaddr=0x%08X epc=0x%08X pc=0x%08X"
            " sp=0x%08X ra=0x%08X cause=0x%08X\n",
            exccode, vaddr, epc_v, (uint32_t)cpu->pc,
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
            (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
            (uint32_t)cp0->reg[COP0_CAUSE]);
        dump_code_window(m, epc_v, 2u, 2u);
        dump_gpr_window(m);
        {
            uint32_t sp_v = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
            unsigned k;
            for (k = 0; k < 12u; k++) {
                uint32_t w = 0;
                uint32_t sva = sp_v + (uint32_t)(k * 4u);
                bool ok = load_va_word(m, sva, &w);
                fprintf(stderr,
                    "[WINCE_CB_FAULT_STK] sp+0x%02X=0x%08X %s\n",
                    k * 4u, sva, ok ? "" : "(unmapped)");
                (void)w;
                if (ok)
                    fprintf(stderr,
                        "[WINCE_CB_FAULT_STK] val=0x%08X\n", w);
            }
        }
    }

    m->wince.tlb_post_diag_count++;
}

void wince_boot_note_eret(struct cpu *cpu)
{
    machine_t *m;
    uint32_t target;

    if (!cpu)
        return;

    m = wince_boot_from_gx(cpu->machine);
    if (!m || !m->wince.active || !m->wince.cold_boot_redirected
        || !m->wince.irq_exception_snapshot_logged
        || m->wince.eret_snapshot_logged) {
        return;
    }

    target = (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_EPC];
    if (!is_idle_exception_focus_pc(target))
        return;

    m->wince.eret_snapshot_logged = true;
    fprintf(stderr,
        "[WINCE_IRQ] eret target=0x%08X at=0x%08X t6=0x%08X sp=0x%08X"
        " ra=0x%08X status=0x%08X cause=0x%08X\n",
        target,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_AT],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T6],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
        (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_STATUS],
        (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_CAUSE]);
    dump_code_window(m, target, 4u, 10u);
}

bool wince_boot_timer_irq_allowed(struct machine *gxm, struct cpu *cpu)
{
    (void)gxm;
    (void)cpu;
    return true;
}

void wince_boot_note_low_vector_write(struct cpu *cpu, uint64_t paddr,
    size_t len)
{
    machine_t *m;

    if (!cpu || !cpu->machine)
        return;

    m = wince_boot_from_gx(cpu->machine);
    if (!m || !m->wince.active || m->wince.suppress_vector_write_observer)
        return;
    if (!range_overlaps(paddr, (uint64_t)len, 0x00000000u, 0x00000400u))
        return;

    invalidate_all(m);
    m->wince.low_vector_observed_valid = false;
    if (m->wince.vector_owner != WINCE_VECTOR_GUEST) {
        m->wince.vector_owner = WINCE_VECTOR_GUEST;
        m->wince.vectors_ready = false;
        if (!m->wince.low_vector_guest_write_logged) {
            fprintf(stderr,
                "[WINCE_CKPT] guest_low_vector_write"
                " paddr=0x%08" PRIx64 " len=%zu\n",
                paddr, len);
            m->wince.low_vector_guest_write_logged = true;
        }
    }
}

void wince_boot_note_fb_oob(struct cpu *cpu, uint64_t paddr, size_t len)
{
    machine_t *m;

    if (!cpu || !cpu->machine)
        return;

    m = wince_boot_from_gx(cpu->machine);
    if (!m || !m->wince.active || m->wince.first_fb_oob_logged)
        return;
    if (paddr < 0x0A228000ULL || paddr >= 0x0A240000ULL)
        return;

    m->wince.first_fb_oob_logged = true;
    fprintf(stderr,
        "[WINCE_FB] first out-of-range framebuffer write"
        " paddr=0x%08" PRIx64 " len=%zu pc=0x%08" PRIx64 "\n",
        paddr, len, (uint64_t)cpu->pc);
}

void wince_boot_pc_ring_activate(machine_t *m)
{
    if (!m || !m->wince.active || m->wince.pc_ring_active)
        return;
    m->wince.pc_ring_active = true;
    m->wince.pc_ring_idx = 0;
    fprintf(stderr, "[WINCE_CKPT] pc_ring activated\n");
}

void wince_boot_pc_ring_dump(machine_t *m)
{
    uint32_t total;
    uint32_t start;
    uint32_t count;
    uint32_t i;

    if (!m || !m->wince.active || !m->wince.pc_ring_active)
        return;

    total = m->wince.pc_ring_idx;
    if (total == 0) {
        fprintf(stderr, "[PC_RING] empty\n");
        return;
    }

    count = total < WINCE_PC_RING_SIZE ? total : WINCE_PC_RING_SIZE;
    start = total < WINCE_PC_RING_SIZE ? 0 : total % WINCE_PC_RING_SIZE;

    fprintf(stderr, "[PC_RING] %u samples (last %u shown):\n", total, count);
    for (i = 0; i < count; i++) {
        uint32_t idx = (start + i) % WINCE_PC_RING_SIZE;
        uint32_t pc = m->wince.pc_ring[idx];
        uint32_t pa = pc & 0x1FFFFFFFu;
        const char *region = "???";
        if (pa >= 0x1FC00000u)
            region = "ROM";
        else if (pa >= 0xF00000u && pa < 0x1000000u)
            region = "SPL";
        else if (pa >= 0x60000u && pa < 0x100000u)
            region = "NK";
        else if (pa < 0x10000u)
            region = "LOW";

        fprintf(stderr, "[PC_RING] [%3u] PC=0x%08X (%s)"
            " SP=0x%08X Status=0x%08X\n",
            i, pc, region,
            m->wince.pc_ring_sp[idx],
            m->wince.pc_ring_status[idx]);
    }
}

void wince_boot_crash_pc_dump(struct machine *gxm)
{
    machine_t *m = wince_boot_from_gx(gxm);
    if (m)
        wince_boot_pc_ring_dump(m);
}

void wince_boot_note_pc(struct cpu *cpu, uint32_t pc32)
{
    machine_t *m;

    if (!cpu)
        return;

    m = wince_boot_from_gx(cpu->machine);
    if (!m || !m->wince.active)
        return;

    maybe_log_boot_path_probe(m, pc32);
    maybe_note_ppsh_exact_pc(m, cpu, pc32);
    maybe_note_exception_hot_pc(m, cpu, pc32);
    maybe_note_hot_l2_alloc_pc(m, cpu, pc32);
    maybe_note_callback_slot_pc(m, cpu, pc32);
    maybe_note_section3_install_pc(m, cpu, pc32);
    maybe_note_section3_callback_pc(m, cpu, pc32);
    maybe_note_section3_order_pc(m, cpu, pc32);
    maybe_note_section3_caller_pc(m, cpu, pc32);
    maybe_note_section3_source_pc(m, cpu, pc32);
    maybe_note_section3_obj_pc(m, cpu, pc32);
    maybe_note_section3_queue_pc(m, cpu, pc32);
    maybe_note_section3_worker_pc(m, cpu, pc32);
    maybe_note_section3_type4_pc(m, cpu, pc32);
    maybe_note_section3_type4_gate_pc(m, cpu, pc32);
    maybe_note_section3_type4_state_pc(m, cpu, pc32);
    maybe_note_section3_owner_pc(m, cpu, pc32);
}

void wince_boot_note_ppsh_command(struct cpu *cpu, uint16_t cmd)
{
    machine_t *m;

    if (!cpu)
        return;

    m = wince_boot_from_gx(cpu->machine);
    if (!ppsh_trace_enabled(m))
        return;

    if (m->wince.ppsh_poll_active)
        ppsh_close_poll_episode(m, cpu, (uint32_t)cpu->pc, "next_command");

    if (m->wince.ppsh_seq_active)
        ppsh_finish_sequence(m, "next_command");

    if (m->wince.ppsh_cmd_seq_count >= WINCE_PPSH_SEQ_MAX) {
        if (!m->wince.ppsh_seq_cap_logged) {
            fprintf(stderr,
                "[PPSH_SEQ] sequence logging capped at %u commands\n",
                (unsigned)WINCE_PPSH_SEQ_MAX);
            m->wince.ppsh_seq_cap_logged = true;
        }
        return;
    }

    m->wince.ppsh_cmd_seq_count++;
    m->wince.ppsh_seq_active = true;
    m->wince.ppsh_seq_cmd = cmd;
    m->wince.ppsh_seq_start_pc = (uint32_t)cpu->pc;
    m->wince.ppsh_seq_last_status = 0;
    m->wince.ppsh_seq_last_data = 0;
    m->wince.ppsh_seq_status_reads = 0;
    m->wince.ppsh_seq_data_reads = 0;
    m->wince.ppsh_seq_read_budget = WINCE_PPSH_SEQ_READ_LOG_MAX;

    ppsh_flow_log(m,
        "[PPSH_SEQ] #%u start cmd=0x%04X pc=0x%08X"
        " ra=0x%08X sp=0x%08X sections=%u fb_watch=%u\n",
        (unsigned)m->wince.ppsh_cmd_seq_count,
        (unsigned)cmd,
        (uint32_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
        (unsigned)count_active_sections(m),
        m->wince.fb_watch_armed ? 1u : 0u);
    maybe_dump_ppsh_helper_context(m, cpu, cmd);
}

void wince_boot_note_ppsh_status_read(struct cpu *cpu, uint16_t status)
{
    machine_t *m;

    if (!cpu)
        return;

    m = wince_boot_from_gx(cpu->machine);
    if (!ppsh_trace_enabled(m) || !m->wince.ppsh_seq_active)
        return;

    maybe_trace_ppsh_helper_pc(m, cpu, (uint32_t)cpu->pc);
    m->wince.ppsh_seq_last_status = status;
    m->wince.ppsh_seq_status_reads++;
    if (m->wince.ppsh_seq_read_budget == 0)
        return;

    ppsh_flow_log(m,
        "[PPSH_SEQ] #%u status[%u]=0x%04X pc=0x%08X"
        " v0=0x%08X ra=0x%08X\n",
        (unsigned)m->wince.ppsh_cmd_seq_count,
        (unsigned)m->wince.ppsh_seq_status_reads,
        (unsigned)status,
        (uint32_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
    m->wince.ppsh_seq_read_budget--;
    if (m->wince.ppsh_seq_read_budget == 0)
        ppsh_finish_sequence(m, "read_budget");
}

void wince_boot_note_ppsh_data_read(struct cpu *cpu, uint16_t word)
{
    machine_t *m;

    if (!cpu)
        return;

    m = wince_boot_from_gx(cpu->machine);
    if (!ppsh_trace_enabled(m) || !m->wince.ppsh_seq_active)
        return;

    m->wince.ppsh_seq_last_data = word;
    m->wince.ppsh_seq_data_reads++;
    if (m->wince.ppsh_seq_read_budget == 0)
        return;

    ppsh_flow_log(m,
        "[PPSH_SEQ] #%u data[%u]=0x%04X pc=0x%08X"
        " ra=0x%08X\n",
        (unsigned)m->wince.ppsh_cmd_seq_count,
        (unsigned)m->wince.ppsh_seq_data_reads,
        (unsigned)word,
        (uint32_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
    m->wince.ppsh_seq_read_budget--;
    if (m->wince.ppsh_seq_read_budget == 0)
        ppsh_finish_sequence(m, "read_budget");
}

void wince_boot_note_serial_tx(struct cpu *cpu, unsigned char ch)
{
    machine_t *m;
    size_t len;

    if (!cpu)
        return;

    m = wince_boot_from_gx(cpu->machine);
    if (!m || !m->wince.active || !m->wince.cold_boot_copy_done)
        return;

    if (ch == '\r')
        return;

    if (ch == '\n') {
        maybe_flush_ppsh_serial_line(m);
        return;
    }

    if (ch < 0x20 || ch > 0x7eu) {
        maybe_flush_ppsh_serial_line(m);
        return;
    }

    len = m->wince.ppsh_serial_line_len;
    if (len == 0) {
        m->wince.ppsh_serial_first_pc = (uint32_t)cpu->pc;
        m->wince.ppsh_serial_first_ra = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA];
        m->wince.ppsh_serial_first_sp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
        m->wince.ppsh_serial_first_a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
        m->wince.ppsh_serial_first_a1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1];
        m->wince.ppsh_serial_first_a2 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2];
        m->wince.ppsh_serial_first_a3 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3];
        m->wince.ppsh_serial_first_v0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0];
        m->wince.ppsh_serial_first_v1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V1];
        m->wince.ppsh_serial_first_s0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0];
        m->wince.ppsh_serial_first_s1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1];
        m->wince.ppsh_serial_first_s2 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S2];
        m->wince.ppsh_serial_first_s3 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S3];
        m->wince.ppsh_serial_first_s4 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S4];
        m->wince.ppsh_serial_first_t9 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T9];
        m->wince.ppsh_serial_stack0 = 0;
        m->wince.ppsh_serial_stack1 = 0;
        m->wince.ppsh_serial_stack2 = 0;
        m->wince.ppsh_serial_stack3 = 0;
        (void)load_va_word(m, m->wince.ppsh_serial_first_sp + 0u,
            &m->wince.ppsh_serial_stack0);
        (void)load_va_word(m, m->wince.ppsh_serial_first_sp + 4u,
            &m->wince.ppsh_serial_stack1);
        (void)load_va_word(m, m->wince.ppsh_serial_first_sp + 8u,
            &m->wince.ppsh_serial_stack2);
        (void)load_va_word(m, m->wince.ppsh_serial_first_sp + 12u,
            &m->wince.ppsh_serial_stack3);
    } else if (len >= sizeof(m->wince.ppsh_serial_line) - 1u) {
        maybe_flush_ppsh_serial_line(m);
        m->wince.ppsh_serial_first_pc = (uint32_t)cpu->pc;
        m->wince.ppsh_serial_first_ra = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA];
        m->wince.ppsh_serial_first_sp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
        m->wince.ppsh_serial_first_a0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A0];
        m->wince.ppsh_serial_first_a1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A1];
        m->wince.ppsh_serial_first_a2 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A2];
        m->wince.ppsh_serial_first_a3 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_A3];
        m->wince.ppsh_serial_first_v0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V0];
        m->wince.ppsh_serial_first_v1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_V1];
        m->wince.ppsh_serial_first_s0 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S0];
        m->wince.ppsh_serial_first_s1 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S1];
        m->wince.ppsh_serial_first_s2 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S2];
        m->wince.ppsh_serial_first_s3 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S3];
        m->wince.ppsh_serial_first_s4 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_S4];
        m->wince.ppsh_serial_first_t9 = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T9];
        m->wince.ppsh_serial_stack0 = 0;
        m->wince.ppsh_serial_stack1 = 0;
        m->wince.ppsh_serial_stack2 = 0;
        m->wince.ppsh_serial_stack3 = 0;
        (void)load_va_word(m, m->wince.ppsh_serial_first_sp + 0u,
            &m->wince.ppsh_serial_stack0);
        (void)load_va_word(m, m->wince.ppsh_serial_first_sp + 4u,
            &m->wince.ppsh_serial_stack1);
        (void)load_va_word(m, m->wince.ppsh_serial_first_sp + 8u,
            &m->wince.ppsh_serial_stack2);
        (void)load_va_word(m, m->wince.ppsh_serial_first_sp + 12u,
            &m->wince.ppsh_serial_stack3);
        len = 0;
    }

    m->wince.ppsh_serial_line[len++] = (char)ch;
    m->wince.ppsh_serial_line_len = (uint16_t)len;
    m->wince.ppsh_serial_last_pc = (uint32_t)cpu->pc;
    m->wince.ppsh_serial_last_ra = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA];
}

void wince_boot_log_summary(machine_t *m)
{
    const char *classification = "unresolved";
    const char *exc_class = "unresolved";
    const char *exc_reason = "no_hot_fault_data";
    const char *type4_order = "incomplete";
    uint32_t active_sections;
    uint32_t fb_events;

    if (!m || !m->wince.active)
        return;

    if (m->wince.ppsh_seq_active)
        ppsh_finish_sequence(m, "shutdown");
    if (m->wince.ppsh_poll_active && m->cpu)
        ppsh_close_poll_episode(m, m->cpu, (uint32_t)m->cpu->pc, "shutdown");
    maybe_flush_ppsh_serial_line(m);
    if (m->cpu && !m->wince.hot_page_01f8f8f8.logged
        && m->wince.serial_exc_last.bva_valid
        && same_4k_page(m->wince.serial_exc_last.bva,
            UINT32_C(0x01F8F8F8))) {
        maybe_log_hot_page_verdict(m, m->cpu, UINT32_C(0x01F8F8F8),
            m->wince.serial_exc_last.bva,
            m->wince.serial_exc_last.code_valid
                ? m->wince.serial_exc_last.code : 0u,
            "shutdown");
    }
    if (m->cpu && !m->wince.hot_page_01f94b50.logged
        && m->wince.serial_exc_last.bva_valid
        && same_4k_page(m->wince.serial_exc_last.bva,
            UINT32_C(0x01F94B50))) {
        maybe_log_hot_page_verdict(m, m->cpu, UINT32_C(0x01F94B50),
            m->wince.serial_exc_last.bva,
            m->wince.serial_exc_last.code_valid
                ? m->wince.serial_exc_last.code : 0u,
            "shutdown");
    }
    if (m->cpu && !m->wince.hot_page_02041fa8.logged
        && m->wince.serial_exc_last.bva_valid
        && same_4k_page(m->wince.serial_exc_last.bva,
            UINT32_C(0x02041FA8))) {
        maybe_log_hot_page_verdict(m, m->cpu, UINT32_C(0x02041FA8),
            m->wince.serial_exc_last.bva,
            m->wince.serial_exc_last.code_valid
                ? m->wince.serial_exc_last.code : 0u,
            "shutdown");
    }

    active_sections = count_active_sections(m);
    fb_events = (uint32_t)m->wince.fb_watch_report_count
        + (uint32_t)m->wince.fb_write_diag_count;

    if (m->wince.ppsh_poll_episode_count == 0) {
        classification = "no_poll_loop_observed";
    } else if (active_sections <= 1 && fb_events == 0
        && m->wince.ppsh_flag_clear_count > 0) {
        classification = "poll_loop_no_boot_progress";
    } else if (active_sections > 1 || fb_events > 0) {
        classification = "poll_loop_with_boot_progress";
    } else if (m->wince.ppsh_flag_set_count > 0
        && m->wince.ppsh_flag_clear_count == 0) {
        classification = "flag_sets_without_clear";
    }

    fprintf(stderr,
        "[PPSH_SUMMARY] class=%s send=%u read=%u poll_episodes=%u"
        " poll_exits=%u last_iters=%u last_exit=0x%08X"
        " seqs=%u flags=%u sets=%u clears=%u writes=%u"
        " sections=%u fb_watch=%u fb_events=%u\n",
        classification,
        (unsigned)m->wince.ppsh_send_entry_count,
        (unsigned)m->wince.ppsh_read_entry_count,
        (unsigned)m->wince.ppsh_poll_episode_count,
        (unsigned)m->wince.ppsh_poll_exit_count,
        (unsigned)m->wince.ppsh_poll_last_iters,
        m->wince.ppsh_poll_exit_pc,
        (unsigned)m->wince.ppsh_cmd_seq_count,
        (unsigned)m->wince.ppsh_flag_transition_count,
        (unsigned)m->wince.ppsh_flag_set_count,
        (unsigned)m->wince.ppsh_flag_clear_count,
        (unsigned)m->wince.ppsh_flag_write_count,
        (unsigned)active_sections,
        m->wince.fb_watch_armed ? 1u : 0u,
        (unsigned)fb_events);

    if (m->wince.type4_order_payload_seq != 0u
        && m->wince.type4_order_enqueue_seq == 0u) {
        type4_order = "consumer_without_enqueue";
    } else if (m->wince.type4_order_cleanup_seq != 0u
        && m->wince.type4_order_enqueue_seq == 0u) {
        type4_order = "cleanup_without_enqueue";
    } else if (m->wince.type4_order_payload_seq != 0u
        && m->wince.type4_order_enqueue_seq != 0u) {
        type4_order =
            (m->wince.type4_order_payload_seq < m->wince.type4_order_enqueue_seq)
            ? "consumer_before_enqueue"
            : "enqueue_before_consumer";
    } else if (m->wince.type4_order_enqueue_seq != 0u) {
        type4_order = "enqueue_without_consumer";
    }
    fprintf(stderr,
        "[WINCE_TYPE4_SUMMARY] order=%s ctor=%u link=%u state=%u payload=%u"
        " enqueue=%u cleanup=%u wrap=0x%08X handle=0x%08X payload_va=0x%08X\n",
        type4_order,
        (unsigned)m->wince.type4_order_ctor_seq,
        (unsigned)m->wince.type4_order_link_seq,
        (unsigned)m->wince.type4_order_state_seq,
        (unsigned)m->wince.type4_order_payload_seq,
        (unsigned)m->wince.type4_order_enqueue_seq,
        (unsigned)m->wince.type4_order_cleanup_seq,
        m->wince.type4_wrap_watch_va,
        m->wince.type4_handle_watch_va,
        m->wince.type4_payload_watch_va);

    if (m->wince.hot_page_01f8f8f8.seen
        && m->wince.hot_page_01f8f8f8.section_val != 0
        && m->wince.hot_page_01f8f8f8.l2_val == 0) {
        exc_class = "missing_or_stale_page_tables";
        exc_reason = "01f8f8f8_l2_zero";
    } else if (m->wince.hot_page_01f94b50.seen
        && m->wince.hot_page_01f94b50.section_val != 0
        && m->wince.hot_page_01f94b50.l2_val == 0) {
        exc_class = "missing_or_stale_page_tables";
        exc_reason = "01f94b50_l2_zero";
    } else if (m->wince.hot_page_02041fa8.seen
        && m->wince.hot_page_02041fa8.section_val != 0
        && (m->wince.hot_page_02041fa8.l2_val == 0
            || !m->wince.hot_page_02041fa8.selected_valid)) {
        exc_class = "missing_or_stale_page_tables";
        exc_reason = "02041fa8_page_invalid";
    } else if ((m->wince.hot_page_01f8f8f8.seen
            && m->wince.hot_page_01f8f8f8.selected_valid)
        || (m->wince.hot_page_01f94b50.seen
            && m->wince.hot_page_01f94b50.selected_valid)
        || (m->wince.hot_page_02041fa8.seen
            && m->wince.hot_page_02041fa8.selected_valid)) {
        exc_class = "refill_or_tlb_install_semantics";
        exc_reason = "page_present_but_faulting";
    } else if (m->wince.systempatch_seen && active_sections <= 1) {
        exc_class = "wrong_process_section_context";
        exc_reason = "systempatch_with_single_active_section";
    } else if (m->wince.hot_fault_probe_count > 0) {
        exc_class = "guest_pointer_or_object_corruption";
        exc_reason = "hot_pc_probe_without_mapping_failure";
    }

    if (m->wince.serial_exc_last.valid) {
        fprintf(stderr,
            "[WINCE_EXC_SUMMARY] class=%s reason=%s serial_code=0x%03X"
            " serial_pc=0x%08X serial_ra=%s0x%08X serial_bva=0x%08X"
            " process=\"%s\" corr=%u hot_pc=%u sec0=0x%08X sec1=0x%08X"
            " verdict01f8_l2=0x%08X verdict01f9_l2=0x%08X"
            " verdict20_l2=0x%08X\n",
            exc_class,
            exc_reason,
            m->wince.serial_exc_last.code,
            m->wince.serial_exc_last.pc,
            m->wince.serial_exc_last.ra_valid ? "" : "?",
            m->wince.serial_exc_last.ra,
            m->wince.serial_exc_last.bva,
            m->wince.serial_exc_last.process_name[0] != '\0'
                ? m->wince.serial_exc_last.process_name : "?",
            (unsigned)m->wince.serial_exception_corr_count,
            (unsigned)m->wince.hot_fault_probe_count,
            load_pa_word(m, 0x18C0u),
            load_pa_word(m, 0x18C4u),
            m->wince.hot_page_01f8f8f8.l2_val,
            m->wince.hot_page_01f94b50.l2_val,
            m->wince.hot_page_02041fa8.l2_val);
    } else {
        fprintf(stderr,
            "[WINCE_EXC_SUMMARY] class=%s reason=%s serial_code=none"
            " corr=%u hot_pc=%u sec0=0x%08X sec1=0x%08X\n",
            exc_class,
            exc_reason,
            (unsigned)m->wince.serial_exception_corr_count,
            (unsigned)m->wince.hot_fault_probe_count,
            load_pa_word(m, 0x18C0u),
            load_pa_word(m, 0x18C4u));
    }
}

bool wince_boot_arm_step_trace(struct cpu *cpu, uint32_t pc32)
{
    machine_t *m;
    uint32_t canon_pc = pc32;

    if (!cpu)
        return false;

    m = wince_boot_from_gx(cpu->machine);
    if (!m || !m->wince.active || !m->wince.cold_boot_copy_done)
        return false;

    if ((canon_pc & 0xE0000000u) == 0x80000000u
        || (canon_pc & 0xE0000000u) == 0xA0000000u)
        canon_pc = (canon_pc & 0x1FFFFFFFu) | 0x80000000u;

    if (m->wince.nk_step_trace_done || m->wince.nk_step_trace_active)
        return false;
    if (canon_pc < 0x80079400u || canon_pc >= 0x80079800u)
        return false;

    m->wince.nk_step_trace_active = true;
    m->wince.nk_step_trace_remaining = 256;
    fprintf(stderr,
        "[WINCE_STEP] armed PC=0x%08X Canon=0x%08X"
        " RA=0x%08X SP=0x%08X Status=0x%08X\n",
        pc32,
        canon_pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
        (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_STATUS]);
    return true;
}

void wince_boot_note_ram_access(struct cpu *cpu, uint64_t paddr,
    const unsigned char *data, size_t len, bool is_write)
{
    machine_t *m;
    const char *name;
    uint64_t val = 0;
    uint16_t *count;
    bool old_suppress;
    size_t i;

    if (!cpu || !cpu->machine || !data || len == 0)
        return;

    m = wince_boot_from_gx(cpu->machine);
    if (!m || !m->wince.active || m->wince.suppress_watch_observer)
        return;

    for (i = 0; i < len && i < 8; i++)
        val |= (uint64_t)data[i] << (8 * i);

    if (is_write && paddr == 0x66001Cu && len == 4) {
        m->wince.ppsh_flag_write_count++;
        if (m->wince.ppsh_flag_write_count <= 8) {
            fprintf(stderr,
                "[PPSH_FLAG_WRITE] #%u value=0x%08llX"
                " PC=0x%08X instrs=%llu\n",
                (unsigned)m->wince.ppsh_flag_write_count,
                (unsigned long long)val,
                (uint32_t)cpu->pc,
                (unsigned long long)cpu->ninstrs);
        }
    }
    if (is_write
        && range_overlaps(paddr, (uint64_t)len,
            WINCE_CALLBACK_SLOT_CANDIDATE_PA,
            WINCE_CALLBACK_SLOT_TRACE_BYTES)) {
        log_callback_slot_write(m, cpu, WINCE_CALLBACK_SLOT_CANDIDATE_PA,
            paddr, len, val, "candidate");
    }
    if (is_write
        && range_overlaps(paddr, (uint64_t)len,
            WINCE_CALLBACK_OBJ_CANDIDATE_PA,
            WINCE_CALLBACK_OBJ_TRACE_BYTES)) {
        log_callback_object_write(m, cpu, paddr, len, val);
    }
    if (is_write && m->wince.callback_slot_watch_armed
        && m->wince.callback_slot_watch_pa != WINCE_CALLBACK_SLOT_CANDIDATE_PA
        && range_overlaps(paddr, (uint64_t)len,
            m->wince.callback_slot_watch_pa,
            WINCE_CALLBACK_SLOT_TRACE_BYTES)) {
        log_callback_slot_write(m, cpu, m->wince.callback_slot_watch_pa,
            paddr, len, val, "resolved");
    }
    if (is_write
        && (range_overlaps(paddr, (uint64_t)len, 0x006697A0u, 0x20u)
            || range_overlaps(paddr, (uint64_t)len, 0x006697C0u, 0x20u))) {
        const char *obj_name = range_overlaps(paddr, (uint64_t)len,
            0x006697A0u, 0x20u) ? "evt97a0" : "evt97c0";
        uint32_t base = strcmp(obj_name, "evt97a0") == 0
            ? UINT32_C(0x006697A0) : UINT32_C(0x006697C0);

        m->wince.ppsh_obj_write_count++;
        if (m->wince.ppsh_obj_write_count <= 24u) {
            fprintf(stderr,
                "[PPSH_OBJ] #%u %s+0x%02" PRIx64 " len=%zu val=0x%llX"
                " PC=0x%08" PRIx64 " RA=0x%08X\n",
                (unsigned)m->wince.ppsh_obj_write_count,
                obj_name,
                paddr - base,
                len,
                (unsigned long long)val,
                (uint64_t)cpu->pc,
                (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
        }
    }

    old_suppress = m->wince.suppress_watch_observer;
    m->wince.suppress_watch_observer = true;
    if (is_write)
        maybe_log_section3_raw_write(m, cpu, paddr, len, val);
    if (is_write)
        maybe_log_section3_page_write(m, cpu, paddr, len, val);
    if (is_write)
        maybe_log_section3_desc_write(m, cpu, paddr, len, val);
    if (is_write)
        maybe_log_section3_pool_write(m, cpu, paddr, len, val);
    if (is_write)
        maybe_log_section3_ctor_field_write(m, cpu, paddr, len, val);
    if (is_write)
        maybe_log_section3_focus_obj_write(m, cpu, paddr, len, val);
    if (!is_write)
        maybe_log_section3_desc_read(m, cpu, paddr, len, val);
    if (is_write)
        maybe_log_section3_retobj_write(m, cpu, paddr, len, val);
    if (is_write)
        maybe_log_section3_head_write(m, cpu, paddr, len, val);
    if (is_write)
        maybe_log_section3_owner_write(m, cpu, paddr, len, val);
    if (is_write)
        maybe_log_systempatch_thread_write(m, cpu, paddr, len, val);
    maybe_log_section0_hot_slot_access(m, cpu, paddr, len, val, is_write);

    if (is_write)
        maybe_log_tlb_table_write(m, cpu, paddr, (uint64_t)len, val);
    m->wince.suppress_watch_observer = old_suppress;

    if (!watched_ram_range_name(paddr, (uint64_t)len, &name))
        return;

    count = is_write ? &m->wince.ram_watch_write_count
        : &m->wince.ram_watch_read_count;
    if (*count >= 256)
        return;
    (*count)++;

    fprintf(stderr,
        "[WINCE_RAM] %c %s PA=0x%08" PRIx64 " len=%zu val=0x%llX"
        " PC=0x%08" PRIx64 " RA=0x%08X\n",
        is_write ? 'W' : 'R',
        name,
        paddr,
        len,
        (unsigned long long)val,
        (uint64_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);

}

void wince_boot_note_mmio_access(struct machine *gxm, struct cpu *cpu,
    uint64_t paddr, size_t len, uint64_t value, bool is_write)
{
    machine_t *m;
    const char *name;
    uint16_t *count;

    if (!cpu)
        return;

    m = wince_boot_from_gx(gxm != NULL ? gxm : cpu->machine);
    if (!m || !m->wince.active || m->wince.suppress_watch_observer)
        return;
    if (!watched_mmio_range_name(paddr, (uint64_t)len, &name))
        return;

    count = is_write ? &m->wince.mmio_watch_write_count
        : &m->wince.mmio_watch_read_count;
    if (*count >= 256)
        return;
    (*count)++;

    fprintf(stderr,
        "[WINCE_MMIO] %c %s PA=0x%08" PRIx64 " len=%zu val=0x%llX"
        " PC=0x%08" PRIx64 " RA=0x%08X\n",
        is_write ? 'W' : 'R',
        name,
        paddr,
        len,
        (unsigned long long)value,
        (uint64_t)cpu->pc,
        (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA]);
}

void wince_boot_note_idle_transition(struct cpu *cpu, const char *event,
    const char *mode, uint32_t status, uint32_t cause, uint32_t enabled,
    uint32_t mask, uint32_t raw_pending, uint32_t count, uint32_t compare,
    int compare_pending, int is_halted)
{
    machine_t *m;
    uint32_t pc;
    uint32_t epc;
    uint32_t sp;
    uint32_t ra;
    uint32_t badva;
    uint16_t vr0046 = 0;
    uint16_t vr008c = 0;
    uint16_t vr00a6 = 0;
    uint16_t vr0118 = 0;
    uint32_t lat1120 = 0;
    uint32_t lat112c = 0;
    uint32_t lat1b10 = 0;
    uint32_t lat1b20 = 0;

    if (!cpu)
        return;

    m = wince_boot_from_gx(cpu->machine);
    if (!m || !m->wince.active || !m->wince.cold_boot_redirected)
        return;
    if (m->wince.idle_diag_count >= 48)
        return;

    pc = (uint32_t)cpu->pc;
    epc = (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_EPC];
    badva = (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR];
    if (!cold_boot_scheduler_probe_pc_match(pc)
        && !cold_boot_scheduler_probe_epc_match(epc)
        && badva != UINT32_C(0x0201FE2C)) {
        return;
    }

    set_watch_observer(m, false);
    (void)load_va_half(m, UINT32_C(0xAF000046), &vr0046);
    (void)load_va_half(m, UINT32_C(0xAF00008C), &vr008c);
    (void)load_va_half(m, UINT32_C(0xAF0000A6), &vr00a6);
    (void)load_va_half(m, UINT32_C(0xAF000118), &vr0118);
    (void)load_vrc_latch_word(UINT32_C(0x0A001120), &lat1120);
    (void)load_vrc_latch_word(UINT32_C(0x0A00112C), &lat112c);
    (void)load_vrc_latch_word(UINT32_C(0x0A001B10), &lat1b10);
    (void)load_vrc_latch_word(UINT32_C(0x0A001B20), &lat1b20);
    set_watch_observer(m, true);

    sp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
    ra = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA];

    fprintf(stderr,
        "[WINCE_IDLE] event=%s mode=%s pc=0x%08X epc=0x%08X"
        " ra=0x%08X sp=0x%08X status=0x%08X cause=0x%08X"
        " enabled=%u mask=0x%08X raw=0x%08X count=0x%08X"
        " compare=0x%08X cmp_pending=%d halted=%d"
        " vr0046=0x%04X vr008c=0x%04X vr00a6=0x%04X vr0118=0x%04X"
        " lat1120=0x%08X lat112c=0x%08X lat1b10=0x%08X lat1b20=0x%08X\n",
        event ? event : "?",
        mode ? mode : "?",
        pc,
        epc,
        ra,
        sp,
        status,
        cause,
        enabled,
        mask,
        raw_pending,
        count,
        compare,
        compare_pending,
        is_halted,
        vr0046,
        vr008c,
        vr00a6,
        vr0118,
        lat1120,
        lat112c,
        lat1b10,
        lat1b20);
    m->wince.idle_diag_count++;
}
