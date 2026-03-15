#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include "wince_diag.h"
#include "machine.h"

/* Constants are in wince_diag.h */

typedef struct {
    const char *name;
    uint32_t start_pa;
    uint32_t end_pa; /* exclusive */
} wince_region_track_desc_t;

static const wince_region_track_desc_t wince_region_track_descs[WINCE_REGION_TRACK_COUNT] = {
    { "cb_tbl_0x00051680",   WINCE_TRACE_CB_PA_START,         WINCE_TRACE_CB_PA_END },
    { "bootparam_0x0001D000", WINCE_TRACE_BOOTPARAM0_PA_START, WINCE_TRACE_BOOTPARAM0_PA_END },
    { "bootparam_0x0002D000", WINCE_TRACE_BOOTPARAM1_PA_START, WINCE_TRACE_BOOTPARAM1_PA_END },
    { "bootctx_0x00006000",   WINCE_TRACE_BOOTCTX_PA_START,    WINCE_TRACE_BOOTCTX_PA_END },
};

static inline bool pc_in_wince_fail_corridor(uint32_t pc32)
{
    return pc32 >= WINCE_FAIL_CORRIDOR_START && pc32 <= WINCE_FAIL_CORRIDOR_END;
}

void init_wince_region_tracks(machine_t *m)
{
    if (!m)
        return;

    for (uint32_t i = 0; i < WINCE_REGION_TRACK_COUNT; i++) {
        wince_region_track_t *t = &m->wince_region_tracks[i];
        memset(t, 0, sizeof(*t));
        t->start_pa = wince_region_track_descs[i].start_pa;
        t->end_pa = wince_region_track_descs[i].end_pa;
    }
}

static inline bool write_value_is_zero(unsigned size, uint64_t value)
{
    if (size >= 8u)
        return value == 0u;
    uint64_t mask = (UINT64_C(1) << (size * 8u)) - 1u;
    return (value & mask) == 0u;
}

static inline uint64_t write_value_mask(unsigned size)
{
    if (size >= 8u)
        return UINT64_MAX;
    return (UINT64_C(1) << (size * 8u)) - 1u;
}

static bool read_old_write_value_addr(uc_engine *uc, uint64_t address,
                                      unsigned size, uint64_t *out)
{
    uint8_t bytes[8] = {0};
    if (!out || size == 0u || size > sizeof(bytes))
        return false;
    if (uc_mem_read(uc, address, bytes, size) != UC_ERR_OK)
        return false;

    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++)
        v |= (uint64_t)bytes[i] << (i * 8u);
    *out = v;
    return true;
}

static bool read_old_write_value(uc_engine *uc, uint32_t pa, unsigned size, uint64_t *out)
{
    uint8_t bytes[8] = {0};
    if (!out || size == 0u || size > sizeof(bytes))
        return false;

    uc_err err = uc_mem_read(uc, (uint64_t)pa, bytes, size);
    if (err != UC_ERR_OK) {
        uint64_t kseg0 = mips_sext(UINT32_C(0x80000000) + pa);
        err = uc_mem_read(uc, kseg0, bytes, size);
    }
    if (err != UC_ERR_OK)
        return false;

    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++)
        v |= (uint64_t)bytes[i] << (i * 8u);
    *out = v;
    return true;
}

static void update_wince_region_tracks(machine_t *m, uc_engine *uc, uint32_t pa, uint32_t pc,
                                       unsigned size, uint64_t value)
{
    if (!m || size == 0u || size > 8u)
        return;

    uint64_t mask = write_value_mask(size);
    uint64_t new_val = value & mask;
    uint64_t old_val = 0;
    bool old_valid = read_old_write_value(uc, pa, size, &old_val);
    bool old_nonzero = old_valid && ((old_val & mask) != 0u);
    bool new_zero = (new_val == 0u);

    for (uint32_t i = 0; i < WINCE_REGION_TRACK_COUNT; i++) {
        wince_region_track_t *t = &m->wince_region_tracks[i];
        if (pa < t->start_pa || pa >= t->end_pa)
            continue;

        t->writes++;
        if (!t->first_valid) {
            t->first_valid = true;
            t->first_pa = pa;
            t->first_pc = pc;
            t->first_size = (uint8_t)size;
            t->first_value = new_val;
        }

        t->last_valid = true;
        t->last_pa = pa;
        t->last_pc = pc;
        t->last_size = (uint8_t)size;
        t->last_value = new_val;

        if (old_nonzero && new_zero) {
            t->nz_to_zero_count++;
            if (!t->first_nz_to_zero_valid) {
                t->first_nz_to_zero_valid = true;
                t->first_nz_to_zero_pa = pa;
                t->first_nz_to_zero_pc = pc;
                t->first_nz_to_zero_size = (uint8_t)size;
                t->first_nz_to_zero_old = old_val & mask;
                t->first_nz_to_zero_new = new_val;
            }

            t->last_nz_to_zero_valid = true;
            t->last_nz_to_zero_pa = pa;
            t->last_nz_to_zero_pc = pc;
            t->last_nz_to_zero_size = (uint8_t)size;
            t->last_nz_to_zero_old = old_val & mask;
            t->last_nz_to_zero_new = new_val;

            if (m->cfg.log_wince_stall && m->wince_region_nz2z_logs < 128u) {
                fprintf(stderr,
                        "[WINCE_REGION_NZ2Z] region=%s pa=0x%08X pc=0x%08X size=%u"
                        " old=0x%016" PRIX64 " new=0x%016" PRIX64 "\n",
                        wince_region_track_descs[i].name,
                        pa, pc, size,
                        t->last_nz_to_zero_old, t->last_nz_to_zero_new);
                m->wince_region_nz2z_logs++;
            }
        }

        if (m->cfg.log_wince_stall &&
            pc_in_wince_fail_corridor(pc) &&
            m->wince_pa_watch_logs < 320u) {
            fprintf(stderr,
                    "[WINCE_REGION_WRITE] region=%s pa=0x%08X pc=0x%08X size=%u"
                    " old=%s0x%016" PRIX64 " new=0x%016" PRIX64 "\n",
                    wince_region_track_descs[i].name, pa, pc, size,
                    old_valid ? "" : "ERR:",
                    old_valid ? (old_val & mask) : 0u,
                    new_val);
            m->wince_pa_watch_logs++;
        }
    }
}

static const char *wince_div_stack_slot_name(uint32_t idx)
{
    return (idx == 0u) ? "s0_slot_sp_plus_20" :
           (idx == 1u) ? "ra_slot_sp_plus_24" : "slot_unknown";
}

static const char *wince_div_stack_window_name(uint32_t idx)
{
    return (idx == 0u) ? "caller" :
           (idx == 1u) ? "callee" : "window_unknown";
}

static const char *wince_div_stack_phase_name(uint32_t idx)
{
    return (idx == 0u) ? "arm" :
           (idx == 1u) ? "entry" :
           (idx == 2u) ? "return" :
           (idx == 3u) ? "crash" : "phase_unknown";
}

static inline bool addr32_in_window(uint32_t addr, uint32_t start, uint32_t end)
{
    if (start <= end)
        return addr >= start && addr < end;
    return addr >= start || addr < end;
}

static bool read_guest_va_bytes_direct(uc_engine *uc, uint32_t va32,
                                       void *buf, size_t size)
{
    if (!uc || !buf || size == 0u)
        return false;
    if (uc_mem_read(uc, (uint64_t)va32, buf, size) == UC_ERR_OK)
        return true;
    if (uc_mem_read(uc, mips_sext(va32), buf, size) == UC_ERR_OK)
        return true;
    return false;
}

static bool read_guest_u32_direct(uc_engine *uc, uint32_t va32, uint32_t *out)
{
    return out != NULL && read_guest_va_bytes_direct(uc, va32, out, sizeof(*out));
}

static bool read_guest_va_value_direct(uc_engine *uc, uint32_t va32,
                                       unsigned size, uint64_t *out)
{
    uint8_t bytes[8] = {0};
    if (!out || size == 0u || size > sizeof(bytes))
        return false;
    if (!read_guest_va_bytes_direct(uc, va32, bytes, size))
        return false;

    uint64_t value = 0;
    for (unsigned i = 0; i < size; i++)
        value |= (uint64_t)bytes[i] << (i * 8u);
    *out = value;
    return true;
}

static uint32_t fnv1a32_update(uint32_t hash, const void *buf, size_t size)
{
    const uint8_t *p = buf;
    for (size_t i = 0; i < size; i++) {
        hash ^= p[i];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static wince_div_stack_window_trace_t *wince_div_stack_find_window(
    wince_div_stack_watch_t *w, uint32_t addr, uint32_t *out_idx)
{
    if (!w)
        return NULL;
    for (uint32_t i = 0; i < WINCE_DIV_STACK_WINDOW_COUNT; i++) {
        if (w->windows[i].base == w->windows[i].end)
            continue;
        if (!addr32_in_window(addr, w->windows[i].base, w->windows[i].end))
            continue;
        if (out_idx)
            *out_idx = i;
        return &w->windows[i];
    }
    return NULL;
}

/* Diagnostic-only VA→PA read for kseg2/kseg3 addresses via the shadow TLB.
 * This is separate from shadow_tlb_lookup() which intentionally rejects VA >= 0x80000000.
 * Returns true on success and writes the value and translated PA through out pointers. */
static bool wince_diag_read_va_via_shadow_tlb(machine_t *m, uc_engine *uc,
                                               uint32_t va, uint32_t *out_value,
                                               uint32_t *out_pa)
{
    if (!m || !uc)
        return false;

    uint32_t pa = 0;

    if (va >= 0x80000000u && va < 0xA0000000u) {
        /* kseg0: unmapped, cached */
        pa = va - 0x80000000u;
    } else if (va >= 0xA0000000u && va < 0xC0000000u) {
        /* kseg1: unmapped, uncached */
        pa = va - 0xA0000000u;
    } else if (va >= 0xC0000000u) {
        /* kseg2/kseg3: TLB-mapped — search shadow TLB */
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
        if (best_idx == 0xFFFFFFFFu)
            return false;  /* no TLB match */

        uint32_t pm = m->shadow_tlb_pagemask[best_idx];
        uint64_t page_bytes = tlb_leaf_bytes_from_pagemask(pm);
        if (page_bytes < 0x1000u)
            return false;

        bool odd_page = (((uint64_t)va & page_bytes) != 0u);
        uint32_t lo = odd_page ? m->shadow_tlb_lo1[best_idx]
                               : m->shadow_tlb_lo0[best_idx];
        if ((lo & 0x2u) == 0u)
            return false;  /* entry not valid */

        uint64_t pfn = (uint64_t)((lo >> 6) & 0xFFFFFu);
        uint64_t pa_page = (pfn << 10) & ~(page_bytes - 1u);
        uint64_t page_offset = (uint64_t)va & (page_bytes - 1u);
        pa = (uint32_t)(pa_page + page_offset);
    } else {
        /* kuseg: use standard shadow_tlb_lookup via existing path */
        return false;
    }

    if (pa >= m->cfg.sdram_size)
        return false;

    uint32_t word = 0;
    uc_err err = uc_mem_read(uc, (uint64_t)pa, &word, sizeof(word));
    if (err != UC_ERR_OK)
        return false;

    if (out_value)
        *out_value = word;
    if (out_pa)
        *out_pa = pa;
    return true;
}

static void wince_diag_format_va_word(machine_t *m, uc_engine *uc, uint32_t va,
                                      char *buf, size_t buf_size)
{
    uint32_t value = 0;
    uint32_t pa = 0;

    if (!buf || buf_size == 0u)
        return;

    if (wince_diag_read_va_via_shadow_tlb(m, uc, va, &value, &pa)) {
        snprintf(buf, buf_size, "0x%08X(pa=0x%08X,val=0x%08X)",
                 va, pa, value);
        return;
    }

    if (read_guest_u32_direct(uc, va, &value)) {
        snprintf(buf, buf_size, "0x%08X(pa=RAW,val=0x%08X)", va, value);
        return;
    }

    snprintf(buf, buf_size, "0x%08X(pa=MISS,val=MISS)", va);
}

static void wince_div_log_resume_globals(machine_t *m, uc_engine *uc,
                                         const char *tag,
                                         uint32_t pc32, uint32_t sp32, uint32_t ra32)
{
    uint64_t s0 = 0, v0 = 0;
    uint32_t g94ec = 0, g94f0 = 0, g9508 = 0, g9510 = 0;
    bool ok94ec = false, ok94f0 = false, ok9508 = false, ok9510 = false;
    char obj_desc[64];

    if (!m || !uc || !tag)
        return;

    uc_reg_read(uc, UC_MIPS_REG_S0, &s0);
    uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
    ok94ec = read_pa_u32_all_aliases(m, UINT32_C(0x006794EC), &g94ec);
    ok94f0 = read_pa_u32_all_aliases(m, UINT32_C(0x006794F0), &g94f0);
    ok9508 = read_pa_u32_all_aliases(m, UINT32_C(0x00679508), &g9508);
    ok9510 = read_pa_u32_all_aliases(m, UINT32_C(0x00679510), &g9510);
    wince_diag_format_va_word(m, uc, (uint32_t)s0 + UINT32_C(0x000002C0),
                              obj_desc, sizeof(obj_desc));

    fprintf(stderr,
            "[%s] pc=0x%08X sp=0x%08X ra=0x%08X s0=0x%08X"
            " s0_2c0=%s"
            " g94ec=%s0x%08X g94f0=%s0x%08X"
            " g9508=%s0x%08X g9510=%s0x%08X"
            " v0=0x%08X\n",
            tag, pc32, sp32, ra32, (uint32_t)s0,
            obj_desc,
            ok94ec ? "" : "ERR:", g94ec,
            ok94f0 ? "" : "ERR:", g94f0,
            ok9508 ? "" : "ERR:", g9508,
            ok9510 ? "" : "ERR:", g9510,
            (uint32_t)v0);
}

static void wince_div_stack_capture_phase(machine_t *m, uc_engine *uc, uint32_t phase)
{
    if (!m || !uc || phase >= WINCE_DIV_STACK_PHASE_COUNT)
        return;

    wince_div_stack_watch_t *w = &m->wince_div_stack_watch;
    if (!w->active || w->phase_captured[phase])
        return;

    for (uint32_t window_idx = 0; window_idx < WINCE_DIV_STACK_WINDOW_COUNT; window_idx++) {
        const wince_div_stack_window_trace_t *window = &w->windows[window_idx];
        wince_div_frame_snapshot_t *snap = &w->snapshots[phase][window_idx];
        memset(snap, 0, sizeof(*snap));
        snap->base = window->base;
        snap->size = window->end - window->base;
        snap->valid = snap->size != 0u;
        if (!snap->valid)
            continue;

        snap->words = snap->size / 4u;
        uint32_t hash = UINT32_C(2166136261);
        for (uint32_t off = 0; off < snap->size; off += 4u) {
            uint32_t word = 0;
            uint32_t va_addr = window->base + off;
            bool ok = read_guest_u32_direct(uc, va_addr, &word);
            if (!ok) {
                /* TLB fallback for kseg2/kseg3 VAs */
                uint32_t tlb_pa = 0;
                ok = wince_diag_read_va_via_shadow_tlb(m, uc, va_addr, &word, &tlb_pa);
            }
            uint8_t valid_flag = ok ? 1u : 0u;
            hash = fnv1a32_update(hash, &valid_flag, sizeof(valid_flag));
            hash = fnv1a32_update(hash, &word, sizeof(word));
            if (ok) {
                snap->valid_words++;
                if (word != 0u)
                    snap->nonzero_words++;
            }
        }
        snap->hash = hash;
    }

    for (uint32_t i = 0; i < WINCE_DIV_STACK_SLOT_COUNT; i++) {
        uint32_t value = 0;
        uint32_t slot_va = w->slots[i].addr;
        bool ok = read_guest_u32_direct(uc, slot_va, &value);
        uint32_t tlb_pa = 0;
        bool tlb_hit = false;
        if (!ok) {
            /* TLB fallback for kseg2/kseg3 stack slot VAs */
            tlb_hit = wince_diag_read_va_via_shadow_tlb(m, uc, slot_va, &value, &tlb_pa);
            if (tlb_hit)
                ok = true;
        }
        w->slots[i].phase_valid[phase] = ok;
        w->slots[i].phase_value[phase] = value;

        /* Log TLB-aware read result for diagnostics */
        static uint32_t tlb_read_logs = 0;
        if (tlb_read_logs < 128u) {
            const char *slot_name = (i == 0) ? "s0_slot" : "ra_slot";
            if (tlb_hit) {
                fprintf(stderr,
                        "[WINCE_DIV_STACK_TLB_READ] phase=%u slot=%s va=0x%08X"
                        " tlb_hit=1 pa=0x%08X value=0x%08X\n",
                        phase, slot_name, slot_va, tlb_pa, value);
            } else if (slot_va >= 0xC0000000u) {
                fprintf(stderr,
                        "[WINCE_DIV_STACK_TLB_READ] phase=%u slot=%s va=0x%08X"
                        " tlb_hit=0 reason=no_match\n",
                        phase, slot_name, slot_va);
            }
            tlb_read_logs++;
        }
    }

    /* Step 2: Verify caller's correct save slots at arm_sp + 0x20 (s0) and + 0x24 (ra) */
    if (phase == 0u || phase == 3u) {
        static uint32_t correct_slot_logs = 0;
        const struct { const char *name; uint32_t offset; } cslots[] = {
            { "caller_s0", 0x20u },
            { "caller_ra", 0x24u },
        };
        for (uint32_t ci = 0; ci < 2u; ci++) {
            uint32_t cva = w->arm_sp + cslots[ci].offset;
            uint32_t cval = 0;
            uint32_t cpa = 0;
            bool c_direct = read_guest_u32_direct(uc, cva, &cval);
            bool c_tlb = false;
            if (!c_direct)
                c_tlb = wince_diag_read_va_via_shadow_tlb(m, uc, cva, &cval, &cpa);
            bool c_ok = c_direct || c_tlb;
            if (correct_slot_logs < 32u) {
                fprintf(stderr,
                        "[WINCE_DIV_CORRECT_SLOT_READ] phase=%u name=%s"
                        " va=0x%08X tlb_hit=%u pa=0x%08X value=0x%08X ok=%u\n",
                        phase, cslots[ci].name, cva,
                        c_tlb ? 1u : 0u, c_tlb ? cpa : (cva & 0x1FFFFFFFu),
                        c_ok ? cval : 0u, c_ok ? 1u : 0u);
                correct_slot_logs++;
            }
        }
    }

    w->phase_captured[phase] = true;
}

static void wince_div_stack_watch_arm(machine_t *m, uc_engine *uc,
                                      uint32_t call_pc, uint32_t sp_before_call)
{
    if (!m || !uc || !is_wince_boot_machine(m) || !m->cfg.log_wince_stall)
        return;

    wince_div_stack_watch_t *w = &m->wince_div_stack_watch;
    memset(w, 0, sizeof(*w));
    w->active = true;
    w->arm_pc = call_pc;
    w->arm_sp = sp_before_call;
    w->caller_window_start = sp_before_call - 0x40u;
    w->caller_window_end = sp_before_call + 0x40u;
    w->callee_window_start = sp_before_call - 0x50u;
    w->callee_window_end = sp_before_call - 0x28u;
    w->callee_window_valid = true;
    w->windows[0].base = w->caller_window_start;
    w->windows[0].end = w->caller_window_end;
    w->windows[1].base = w->callee_window_start;
    w->windows[1].end = w->callee_window_end;
    w->slots[0].addr = sp_before_call - 0x08u; /* SP return + 0x20 */
    w->slots[1].addr = sp_before_call - 0x04u; /* SP return + 0x24 */
    wince_div_stack_capture_phase(m, uc, 0u);

    if (m->wince_div_logs < 256u) {
        fprintf(stderr,
                "[WINCE_DIV_STACK_ARM] call_pc=0x%08X arm_sp=0x%08X"
                " caller=[0x%08X,0x%08X) callee=[0x%08X,0x%08X)"
                " s0_slot=0x%08X ra_slot=0x%08X\n",
                w->arm_pc, w->arm_sp,
                w->caller_window_start, w->caller_window_end,
                w->callee_window_start, w->callee_window_end,
                w->slots[0].addr, w->slots[1].addr);
        m->wince_div_logs++;
    }
}

static void wince_div_stack_watch_mark_completed(machine_t *m)
{
    if (!m)
        return;
    wince_div_stack_watch_t *w = &m->wince_div_stack_watch;
    if (!w->active)
        return;
    w->completed = true;
}

static void wince_div_stack_watch_record_write(machine_t *m, uc_engine *uc,
                                               uint32_t addr32, uint32_t pc,
                                               unsigned size, uint64_t value)
{
    if (!m || !uc || size == 0u || size > 8u)
        return;
    if (!is_wince_boot_machine(m) || !m->cfg.log_wince_stall)
        return;

    wince_div_stack_watch_t *w = &m->wince_div_stack_watch;
    if (!w->active)
        return;

    uint64_t mask = write_value_mask(size);
    uint64_t old_value = 0;
    bool old_valid = read_guest_va_value_direct(uc, addr32, size, &old_value);
    uint64_t new_value = value & mask;
    uint32_t window_idx = 0;
    wince_div_stack_window_trace_t *window =
        wince_div_stack_find_window(w, addr32, &window_idx);
    if (!window)
        return;

    window->writes++;
    if (!window->first_write_valid) {
        window->first_write_valid = true;
        window->first_write_old_valid = old_valid;
        window->first_write_pc = pc;
        window->first_write_addr = addr32;
        window->first_write_size = (uint8_t)size;
        window->first_write_old = old_value & mask;
        window->first_write_new = new_value;
    }
    window->last_write_valid = true;
    window->last_write_old_valid = old_valid;
    window->last_write_pc = pc;
    window->last_write_addr = addr32;
    window->last_write_size = (uint8_t)size;
    window->last_write_old = old_value & mask;
    window->last_write_new = new_value;

    bool slot_hit = false;
    for (uint32_t i = 0; i < WINCE_DIV_STACK_SLOT_COUNT; i++) {
        wince_div_stack_slot_t *slot = &w->slots[i];
        if (addr32 != slot->addr)
            continue;
        slot_hit = true;
        slot->writes++;
        if (!slot->first_valid) {
            slot->first_valid = true;
            slot->first_pc = pc;
            slot->first_addr = addr32;
            slot->first_size = (uint8_t)size;
            slot->first_old = old_value & mask;
            slot->first_new = new_value;
            slot->first_old_valid = old_valid;
        }
        slot->last_valid = true;
        slot->last_pc = pc;
        slot->last_addr = addr32;
        slot->last_size = (uint8_t)size;
        slot->last_old = old_value & mask;
        slot->last_new = new_value;
        slot->last_old_valid = old_valid;

        if (m->wince_div_logs < 320u && w->slot_logs < 48u) {
            fprintf(stderr,
                    "[WINCE_DIV_STACK_SLOT_WRITE] slot=%s addr=0x%08X"
                    " pc=0x%08X size=%u old=%s0x%016" PRIX64
                    " new=0x%016" PRIX64 " writes=%u\n",
                    wince_div_stack_slot_name(i),
                    addr32, pc, size,
                    old_valid ? "" : "ERR:",
                    old_valid ? (old_value & mask) : 0u,
                    new_value, slot->writes);
            m->wince_div_logs++;
            w->slot_logs++;
        }
    }

    if (!slot_hit && m->wince_div_logs < 320u && w->window_logs < 16u) {
        fprintf(stderr,
                "[WINCE_DIV_STACK_WRITE] frame=%s addr=0x%08X pc=0x%08X size=%u"
                " old=%s0x%016" PRIX64 " new=0x%016" PRIX64
                " window_writes=%u\n",
                wince_div_stack_window_name(window_idx),
                addr32, pc, size,
                old_valid ? "" : "ERR:",
                old_valid ? (old_value & mask) : 0u,
                new_value, window->writes);
        m->wince_div_logs++;
        w->window_logs++;
    }
}

static void wince_div_stack_watch_record_read(machine_t *m, uint32_t pc,
                                              uint32_t addr32, unsigned size,
                                              uint32_t reg_idx, uint64_t value)
{
    if (!m || size == 0u || size > 8u)
        return;
    wince_div_stack_watch_t *w = &m->wince_div_stack_watch;
    if (!w->active)
        return;

    uint32_t window_idx = 0;
    wince_div_stack_window_trace_t *window =
        wince_div_stack_find_window(w, addr32, &window_idx);
    if (!window)
        return;

    window->reads++;
    if (!window->first_read_valid) {
        window->first_read_valid = true;
        window->first_read_pc = pc;
        window->first_read_addr = addr32;
        window->first_read_size = (uint8_t)size;
        window->first_read_reg = (uint8_t)reg_idx;
        window->first_read_value = value;
    }
    window->last_read_valid = true;
    window->last_read_pc = pc;
    window->last_read_addr = addr32;
    window->last_read_size = (uint8_t)size;
    window->last_read_reg = (uint8_t)reg_idx;
    window->last_read_value = value;

    bool slot_hit = false;
    for (uint32_t i = 0; i < WINCE_DIV_STACK_SLOT_COUNT; i++) {
        wince_div_stack_slot_t *slot = &w->slots[i];
        if (addr32 != slot->addr)
            continue;
        slot_hit = true;
        slot->reads++;
        if (!slot->first_read_valid) {
            slot->first_read_valid = true;
            slot->first_read_pc = pc;
            slot->first_read_addr = addr32;
            slot->first_read_size = (uint8_t)size;
            slot->first_read_reg = (uint8_t)reg_idx;
            slot->first_read_value = value;
        }
        slot->last_read_valid = true;
        slot->last_read_pc = pc;
        slot->last_read_addr = addr32;
        slot->last_read_size = (uint8_t)size;
        slot->last_read_reg = (uint8_t)reg_idx;
        slot->last_read_value = value;

        if (m->wince_div_logs < 320u && w->slot_logs < 64u) {
            fprintf(stderr,
                    "[WINCE_DIV_STACK_SLOT_READ] slot=%s addr=0x%08X"
                    " pc=0x%08X size=%u reg=$%u val=0x%016" PRIX64
                    " reads=%u\n",
                    wince_div_stack_slot_name(i),
                    addr32, pc, size, reg_idx, value, slot->reads);
            m->wince_div_logs++;
            w->slot_logs++;
        }
    }

    if (!slot_hit && m->wince_div_logs < 320u && w->window_logs < 24u) {
        fprintf(stderr,
                "[WINCE_DIV_STACK_READ] frame=%s addr=0x%08X pc=0x%08X size=%u"
                " reg=$%u val=0x%016" PRIX64 " window_reads=%u\n",
                wince_div_stack_window_name(window_idx),
                addr32, pc, size, reg_idx, value, window->reads);
        m->wince_div_logs++;
        w->window_logs++;
    }
}

static void wince_div_stack_watch_complete_pending_load(machine_t *m, uc_engine *uc)
{
    if (!m || !uc)
        return;
    wince_div_stack_watch_t *w = &m->wince_div_stack_watch;
    if (!w->active || !w->pending_load.active)
        return;

    uint64_t reg_value = 0;
    uc_reg_read(uc, UC_MIPS_REG_0 + (int)w->pending_load.reg_idx, &reg_value);
    wince_div_stack_watch_record_read(m, w->pending_load.pc,
                                      w->pending_load.addr,
                                      w->pending_load.size,
                                      w->pending_load.reg_idx,
                                      reg_value & write_value_mask(w->pending_load.size));
    memset(&w->pending_load, 0, sizeof(w->pending_load));
}

void wince_div_stack_watch_step(machine_t *m, uc_engine *uc,
                                       uint32_t pc32, uint32_t insn)
{
    if (!m || !uc || !is_wince_boot_machine(m) || !m->cfg.log_wince_stall)
        return;

    wince_div_stack_watch_t *w = &m->wince_div_stack_watch;
    if (!w->active)
        return;

    wince_div_stack_watch_complete_pending_load(m, uc);
    if (pc32 == 0x80096790u)
        wince_div_stack_capture_phase(m, uc, 1u);
    else if (pc32 == 0x80096908u)
        wince_div_stack_capture_phase(m, uc, 2u);

    uint32_t op = (insn >> 26) & 0x3Fu;
    uint32_t rs = (insn >> 21) & 0x1Fu;
    uint32_t rt = (insn >> 16) & 0x1Fu;
    int32_t imm = (int32_t)(int16_t)(insn & 0xFFFFu);
    unsigned size = 0u;
    bool is_load = false;
    bool is_store = false;

    switch (op) {
    case 0x20u: /* LB */
    case 0x24u: /* LBU */
        size = 1u;
        is_load = true;
        break;
    case 0x21u: /* LH */
    case 0x25u: /* LHU */
        size = 2u;
        is_load = true;
        break;
    case 0x23u: /* LW */
        size = 4u;
        is_load = true;
        break;
    case 0x28u: /* SB */
        size = 1u;
        is_store = true;
        break;
    case 0x29u: /* SH */
        size = 2u;
        is_store = true;
        break;
    case 0x2Bu: /* SW */
        size = 4u;
        is_store = true;
        break;
    default:
        return;
    }

    uint64_t base = 0;
    uc_reg_read(uc, UC_MIPS_REG_0 + (int)rs, &base);
    uint32_t addr32 = (uint32_t)((uint32_t)base + (uint32_t)imm);
    if (!wince_div_stack_find_window(w, addr32, NULL))
        return;

    if (is_store) {
        uint64_t rt_value = 0;
        uc_reg_read(uc, UC_MIPS_REG_0 + (int)rt, &rt_value);
        wince_div_stack_watch_record_write(m, uc, addr32, pc32, size, rt_value);
        return;
    }

    w->pending_load.active = true;
    w->pending_load.pc = pc32;
    w->pending_load.addr = addr32;
    w->pending_load.size = (uint8_t)size;
    w->pending_load.reg_idx = (uint8_t)rt;
    w->pending_load.slot_idx = UINT8_C(0xFF);
    w->pending_load.window_idx = UINT8_C(0xFF);
}

static const char *wince_div_stack_slot_origin(const wince_div_stack_slot_t *slot)
{
    if (!slot)
        return "unknown";
    if (!slot->phase_valid[0])
        return "invalid_at_arm";
    if (slot->phase_value[0] == 0u)
        return "zero_at_arm";
    if (slot->phase_valid[1] && slot->phase_value[1] == 0u)
        return "zero_by_entry";
    if (slot->phase_valid[2] && slot->phase_value[2] == 0u)
        return "zero_on_return";
    if (slot->phase_valid[3] && slot->phase_value[3] == 0u)
        return "zero_before_crash";
    return "nonzero_through_crash";
}

void log_wince_div_stack_watch_summary(machine_t *m, uc_engine *uc,
                                              const char *reason)
{
    if (!m || !uc)
        return;
    wince_div_stack_watch_t *w = &m->wince_div_stack_watch;
    if (!w->active)
        return;

    wince_div_stack_watch_complete_pending_load(m, uc);
    wince_div_stack_capture_phase(m, uc, 3u);

    fprintf(stderr,
            "[WINCE_DIV_STACK_SUMMARY] reason=%s active=%u completed=%u"
            " call_pc=0x%08X arm_sp=0x%08X"
            " caller=[0x%08X,0x%08X) callee=[0x%08X,0x%08X)\n",
            reason, w->active ? 1u : 0u, w->completed ? 1u : 0u,
            w->arm_pc, w->arm_sp,
            w->caller_window_start, w->caller_window_end,
            w->callee_window_start, w->callee_window_end);

    for (uint32_t window_idx = 0; window_idx < WINCE_DIV_STACK_WINDOW_COUNT; window_idx++) {
        const wince_div_stack_window_trace_t *window = &w->windows[window_idx];
        const wince_div_frame_snapshot_t *arm = &w->snapshots[0][window_idx];
        const wince_div_frame_snapshot_t *entry = &w->snapshots[1][window_idx];
        const wince_div_frame_snapshot_t *ret = &w->snapshots[2][window_idx];
        const wince_div_frame_snapshot_t *crash = &w->snapshots[3][window_idx];
        fprintf(stderr,
                "[WINCE_DIV_STACK_WINDOW_SUMMARY] reason=%s frame=%s"
                " base=0x%08X end=0x%08X reads=%u writes=%u"
                " arm(hash=0x%08X valid=%u nz=%u)"
                " entry(hash=0x%08X valid=%u nz=%u)"
                " return(hash=0x%08X valid=%u nz=%u)"
                " crash(hash=0x%08X valid=%u nz=%u)"
                " first_read=%s(pc=0x%08X addr=0x%08X size=%u reg=$%u val=0x%016" PRIX64 ")"
                " last_read=%s(pc=0x%08X addr=0x%08X size=%u reg=$%u val=0x%016" PRIX64 ")"
                " first_write=%s(pc=0x%08X addr=0x%08X size=%u old=%s0x%016" PRIX64 " new=0x%016" PRIX64 ")"
                " last_write=%s(pc=0x%08X addr=0x%08X size=%u old=%s0x%016" PRIX64 " new=0x%016" PRIX64 ")\n",
                reason, wince_div_stack_window_name(window_idx),
                window->base, window->end, window->reads, window->writes,
                arm->hash, arm->valid_words, arm->nonzero_words,
                entry->hash, entry->valid_words, entry->nonzero_words,
                ret->hash, ret->valid_words, ret->nonzero_words,
                crash->hash, crash->valid_words, crash->nonzero_words,
                window->first_read_valid ? "" : "none",
                window->first_read_pc, window->first_read_addr,
                (unsigned)window->first_read_size, (unsigned)window->first_read_reg,
                window->first_read_value,
                window->last_read_valid ? "" : "none",
                window->last_read_pc, window->last_read_addr,
                (unsigned)window->last_read_size, (unsigned)window->last_read_reg,
                window->last_read_value,
                window->first_write_valid ? "" : "none",
                window->first_write_pc, window->first_write_addr,
                (unsigned)window->first_write_size,
                window->first_write_old_valid ? "" : "ERR:",
                window->first_write_old_valid ? window->first_write_old : 0u,
                window->first_write_new,
                window->last_write_valid ? "" : "none",
                window->last_write_pc, window->last_write_addr,
                (unsigned)window->last_write_size,
                window->last_write_old_valid ? "" : "ERR:",
                window->last_write_old_valid ? window->last_write_old : 0u,
                window->last_write_new);
    }

    for (uint32_t i = 0; i < WINCE_DIV_STACK_SLOT_COUNT; i++) {
        const wince_div_stack_slot_t *slot = &w->slots[i];
        bool final_ok = slot->phase_valid[3];
        uint32_t final_word = slot->phase_value[3];

        fprintf(stderr,
                "[WINCE_DIV_STACK_SLOT_SUMMARY] reason=%s slot=%s"
                " addr=0x%08X reads=%u writes=%u zero_origin=%s"
                " first_read=%s(pc=0x%08X addr=0x%08X size=%u reg=$%u val=0x%016" PRIX64 ")"
                " last_read=%s(pc=0x%08X addr=0x%08X size=%u reg=$%u val=0x%016" PRIX64 ")"
                " first_write=%s(pc=0x%08X addr=0x%08X size=%u old=%s0x%016" PRIX64
                " new=0x%016" PRIX64 ")"
                " last_write=%s(pc=0x%08X addr=0x%08X size=%u old=%s0x%016" PRIX64
                " new=0x%016" PRIX64 ")"
                " final=%s0x%08X\n",
                reason, wince_div_stack_slot_name(i), slot->addr,
                slot->reads, slot->writes, wince_div_stack_slot_origin(slot),
                slot->first_read_valid ? "" : "none",
                slot->first_read_pc, slot->first_read_addr,
                (unsigned)slot->first_read_size, (unsigned)slot->first_read_reg,
                slot->first_read_value,
                slot->last_read_valid ? "" : "none",
                slot->last_read_pc, slot->last_read_addr,
                (unsigned)slot->last_read_size, (unsigned)slot->last_read_reg,
                slot->last_read_value,
                slot->first_valid ? "" : "none",
                slot->first_pc, slot->first_addr,
                (unsigned)slot->first_size,
                slot->first_old_valid ? "" : "ERR:",
                slot->first_old_valid ? slot->first_old : 0u,
                slot->first_new,
                slot->last_valid ? "" : "none",
                slot->last_pc, slot->last_addr,
                (unsigned)slot->last_size,
                slot->last_old_valid ? "" : "ERR:",
                slot->last_old_valid ? slot->last_old : 0u,
                slot->last_new,
                final_ok ? "" : "ERR:", final_ok ? final_word : 0u);

        fprintf(stderr,
                "[WINCE_DIV_STACK_SLOT_TIMELINE] reason=%s slot=%s"
                " addr=0x%08X zero_origin=%s"
                " arm=%s0x%08X entry=%s0x%08X return=%s0x%08X crash=%s0x%08X\n",
                reason, wince_div_stack_slot_name(i), slot->addr,
                wince_div_stack_slot_origin(slot),
                slot->phase_valid[0] ? "" : "ERR:", slot->phase_value[0],
                slot->phase_valid[1] ? "" : "ERR:", slot->phase_value[1],
                slot->phase_valid[2] ? "" : "ERR:", slot->phase_value[2],
                slot->phase_valid[3] ? "" : "ERR:", slot->phase_value[3]);
    }

    for (uint32_t phase = 0; phase < WINCE_DIV_STACK_PHASE_COUNT; phase++) {
        fprintf(stderr,
                "[WINCE_DIV_STACK_PHASE_SUMMARY] reason=%s phase=%s"
                " caller_hash=0x%08X caller_valid=%u caller_nz=%u"
                " callee_hash=0x%08X callee_valid=%u callee_nz=%u\n",
                reason, wince_div_stack_phase_name(phase),
                w->snapshots[phase][0].hash,
                w->snapshots[phase][0].valid_words,
                w->snapshots[phase][0].nonzero_words,
                w->snapshots[phase][1].hash,
                w->snapshots[phase][1].valid_words,
                w->snapshots[phase][1].nonzero_words);
    }
}

void log_wince_region_track_summary(const machine_t *m, const char *reason)
{
    if (!m)
        return;

    for (uint32_t i = 0; i < WINCE_REGION_TRACK_COUNT; i++) {
        const wince_region_track_t *t = &m->wince_region_tracks[i];
        const char *name = wince_region_track_descs[i].name;
        if (!t->first_valid) {
            fprintf(stderr,
                    "[WINCE_REGION_SUMMARY] reason=%s region=%s writes=0 nz2z=0\n",
                    reason, name);
            continue;
        }

        fprintf(stderr,
                "[WINCE_REGION_SUMMARY] reason=%s region=%s writes=%u nz2z=%u"
                " first(pc=0x%08X pa=0x%08X size=%u val=0x%016" PRIX64 ")"
                " last(pc=0x%08X pa=0x%08X size=%u val=0x%016" PRIX64 ")\n",
                reason, name, t->writes, t->nz_to_zero_count,
                t->first_pc, t->first_pa, (unsigned)t->first_size, t->first_value,
                t->last_pc, t->last_pa, (unsigned)t->last_size, t->last_value);

        if (t->first_nz_to_zero_valid) {
            fprintf(stderr,
                    "[WINCE_REGION_NZ2Z_SUMMARY] reason=%s region=%s"
                    " first(pc=0x%08X pa=0x%08X size=%u old=0x%016" PRIX64
                    " new=0x%016" PRIX64 ")"
                    " last(pc=0x%08X pa=0x%08X size=%u old=0x%016" PRIX64
                    " new=0x%016" PRIX64 ")\n",
                    reason, name,
                    t->first_nz_to_zero_pc, t->first_nz_to_zero_pa,
                    (unsigned)t->first_nz_to_zero_size,
                    t->first_nz_to_zero_old, t->first_nz_to_zero_new,
                    t->last_nz_to_zero_pc, t->last_nz_to_zero_pa,
                    (unsigned)t->last_nz_to_zero_size,
                    t->last_nz_to_zero_old, t->last_nz_to_zero_new);
        }
    }
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

bool wince_fptbl_read_hook(uc_engine *uc, uc_mem_type type,
                                   uint64_t address, int size, int64_t value,
                                   void *user_data)
{
    (void)type;
    (void)value;
    machine_t *m = user_data;
    if (m->wince_fptbl_read_logs >= 64u)
        return true;

    uint32_t pa = (uint32_t)address & UINT32_C(0x1FFFFFFF);
    uint32_t slot = (pa - WINCE_TRACE_FPTBL_PA_START) / 4u;

    uint64_t pc64 = 0, ra64 = 0, a0_64 = 0, a1_64 = 0, a2_64 = 0, a3_64 = 0;
    uint64_t v0_64 = 0, t9_64 = 0, sp64 = 0;
    uc_reg_read(uc, UC_MIPS_REG_PC, &pc64);
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra64);
    uc_reg_read(uc, UC_MIPS_REG_A0, &a0_64);
    uc_reg_read(uc, UC_MIPS_REG_A1, &a1_64);
    uc_reg_read(uc, UC_MIPS_REG_A2, &a2_64);
    uc_reg_read(uc, UC_MIPS_REG_A3, &a3_64);
    uc_reg_read(uc, UC_MIPS_REG_V0, &v0_64);
    uc_reg_read(uc, UC_MIPS_REG_T9, &t9_64);
    uc_reg_read(uc, UC_MIPS_REG_SP, &sp64);

    /* Read the value at the target address */
    uint32_t mem_val = 0;
    uc_mem_read(uc, address, &mem_val, sizeof(mem_val));

    uint32_t pc = (uint32_t)pc64;
    fprintf(stderr,
            "[FPTBL_READ] slot=%u pa=0x%08X pc=0x%08X ra=0x%08X value=0x%08X"
            " a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X v0=0x%08X t9=0x%08X sp=0x%08X\n",
            slot, pa, pc, (uint32_t)ra64, mem_val,
            (uint32_t)a0_64, (uint32_t)a1_64, (uint32_t)a2_64, (uint32_t)a3_64,
            (uint32_t)v0_64, (uint32_t)t9_64, (uint32_t)sp64);

    if (slot == 4u) {
        fprintf(stderr,
                "[FPTBL_READ_SLOT4] REACHED — producer dispatch would fire"
                " pc=0x%08X ra=0x%08X value=0x%08X\n",
                pc, (uint32_t)ra64, mem_val);
    }

    /* One-shot: dump 16 instructions around the reading PC */
    if (!m->wince_fptbl_context_dumped) {
        m->wince_fptbl_context_dumped = true;
        fprintf(stderr, "[FPTBL_READ] PC context dump around 0x%08X:\n", pc);
        for (int off = -8; off <= 8; off++) {
            uint32_t ctx_addr = pc + (uint32_t)(off * 4);
            uint32_t ctx_word = 0;
            if (uc_mem_read(uc, (uint64_t)ctx_addr, &ctx_word, 4) == UC_ERR_OK) {
                const char *marker = (off == 0) ? " <<" : "";
                fprintf(stderr, "  0x%08X: %08X%s\n", ctx_addr, ctx_word, marker);
            }
        }
    }

    m->wince_fptbl_read_logs++;
    return true;
}

bool wince_pa_watch_write_hook(uc_engine *uc, uc_mem_type type,
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
    update_wince_region_tracks(m, uc, pa, pc, (unsigned)size, uval);
    wince_div_stack_watch_record_write(m, uc, (uint32_t)address, pc, (unsigned)size, uval);

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
    } else if (pa >= WINCE_TRACE_OBJPTR_PA_START && pa < WINCE_TRACE_OBJPTR_PA_END) {
        bool is_zero = write_value_is_zero((unsigned)size, uval);
        m->wince_objptr_writes++;
        if (!is_zero)
            m->wince_objptr_nonzero = true;
        if (m->cfg.log_wince_stall && m->wince_pa_watch_logs < 208u) {
            fprintf(stderr,
                    "[WINCE_OBJPTR_WATCH] writes=%u nonzero=%u"
                    " pa=0x%08X pc=0x%08X size=%u val=0x%016" PRIX64 "\n",
                    m->wince_objptr_writes,
                    m->wince_objptr_nonzero ? 1u : 0u,
                    pa, pc, (unsigned)size, uval);
            m->wince_pa_watch_logs++;
        }
    } else if (pa >= WINCE_TRACE_RESUME_GLOBAL_PA_START &&
               pa < WINCE_TRACE_RESUME_GLOBAL_PA_END) {
        static uint32_t resume_global_logs = 0;
        if (m->cfg.log_wince_stall && resume_global_logs < 64u) {
            fprintf(stderr,
                    "[WINCE_DIV_RESUME_GLOBAL_WRITE] pa=0x%08X pc=0x%08X"
                    " size=%u val=0x%016" PRIX64 "\n",
                    pa, pc, (unsigned)size, uval);
            resume_global_logs++;
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
    } else if (pa >= WINCE_TRACE_GATE_PA_START && pa < WINCE_TRACE_GATE_PA_END) {
        static uint32_t gate_patch_logs = 0;
        if (m->cfg.log_wince_stall && gate_patch_logs < 64u) {
            uint32_t g0 = 0, g1 = 0, g2 = 0, g3 = 0;
            uint64_t a0 = 0, a1 = 0, a2 = 0;
            uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
            uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
            uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
            uint32_t src = (uint32_t)a1;
            if (pc == 0x80F02880u)
                src = ((uint32_t)a1 - 1u);
            uint8_t src_b = 0;
            uc_err src_err = uc_mem_read(uc, (uint64_t)src, &src_b, sizeof(src_b));
            bool ok0 = read_guest_u32(uc, 0x800795E8u, &g0);
            bool ok1 = read_guest_u32(uc, 0x800795ECu, &g1);
            bool ok2 = read_guest_u32(uc, 0x800795F0u, &g2);
            bool ok3 = read_guest_u32(uc, 0x800795F4u, &g3);
            fprintf(stderr,
                    "[WINCE_GATE_PATCH] pa=0x%08X pc=0x%08X size=%u"
                    " val=0x%016" PRIX64
                    " a0=0x%08X a1=0x%08X a2=0x%08X src=0x%08X src_b=0x%02X src_ok=%u"
                    " gate=[%08X %08X %08X %08X] ok=[%u %u %u %u]\n",
                    pa, pc, (unsigned)size, uval,
                    (uint32_t)a0, (uint32_t)a1, (uint32_t)a2,
                    src, (unsigned)src_b, src_err == UC_ERR_OK ? 1u : 0u,
                    g0, g1, g2, g3,
                    ok0 ? 1u : 0u, ok1 ? 1u : 0u,
                    ok2 ? 1u : 0u, ok3 ? 1u : 0u);
            gate_patch_logs++;
        }
    } else if (pa >= WINCE_TRACE_GATE_SRC_PA_START && pa < WINCE_TRACE_GATE_SRC_PA_END) {
        static uint32_t gate_src_logs = 0;
        if (m->cfg.log_wince_stall && gate_src_logs < 96u) {
            fprintf(stderr,
                    "[WINCE_GATE_SRC] pa=0x%08X pc=0x%08X size=%u val=0x%016" PRIX64 "\n",
                    pa, pc, (unsigned)size, uval);
            gate_src_logs++;
        }
    } else if (pa >= WINCE_TRACE_STACK_PA_START && pa < WINCE_TRACE_STACK_PA_END) {
        static uint32_t stack_logs = 0;
        if (m->cfg.log_wince_stall && stack_logs < 192u) {
            fprintf(stderr,
                    "[WINCE_STACK_WATCH] pa=0x%08X pc=0x%08X size=%u val=0x%016" PRIX64 "\n",
                    pa, pc, (unsigned)size, uval);
            stack_logs++;
        }
    } else if (pa >= WINCE_TRACE_CALLER_FRAME_PA_START && pa < WINCE_TRACE_CALLER_FRAME_PA_END) {
        static uint32_t caller_frame_logs = 0;
        if (m->cfg.log_wince_stall && caller_frame_logs < 128u) {
            const char *tag = "";
            if (pc == 0x80096800u) tag = " EXPECTED_S0_STORE";
            else if (pc == 0x80096808u) tag = " EXPECTED_RA_STORE";
            fprintf(stderr,
                    "[WINCE_DIV_CALLER_FRAME_WRITE] pa=0x%08X pc=0x%08X size=%u"
                    " val=0x%016" PRIX64 "%s\n",
                    pa, pc, (unsigned)size, uval, tag);
            caller_frame_logs++;
        }
    } else if (pa >= WINCE_TRACE_S0_OBJ_PA_START && pa < WINCE_TRACE_S0_OBJ_PA_END) {
        static uint32_t s0_obj_logs = 0;
        if (m->cfg.log_wince_stall && s0_obj_logs < 64u) {
            fprintf(stderr,
                    "[WINCE_DIV_S0_OBJ_WRITE] pa=0x%08X pc=0x%08X size=%u"
                    " val=0x%016" PRIX64 "\n",
                    pa, pc, (unsigned)size, uval);
            s0_obj_logs++;
        }
    }

    return true;
}

void log_wince_pa_watch_summary(const machine_t *m, const char *reason)
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

void log_wince_pa_watch_nonzero(const machine_t *m, const char *reason)
{
    if (!m || !m->wince_pa_watch_active)
        return;
    fprintf(stderr,
            "[WINCE_PA_WATCH_NONZERO] reason=%s vectors_nonzero=%u"
            " ctx_nonzero=%u vectors_writes=%u ctx_writes=%u"
            " cb_nonzero=%u cb_writes=%u objptr_nonzero=%u objptr_writes=%u"
            " obj_nonzero=%u obj_writes=%u\n",
            reason,
            m->wince_vec_watch.saw_nonzero ? 1u : 0u,
            m->wince_ctx_watch.saw_nonzero ? 1u : 0u,
            m->wince_vec_watch.writes,
            m->wince_ctx_watch.writes,
            m->wince_cb_nonzero ? 1u : 0u,
            m->wince_cb_writes,
            m->wince_objptr_nonzero ? 1u : 0u,
            m->wince_objptr_writes,
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

void maybe_record_wince_ctrl_event(machine_t *m, uc_engine *uc,
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

void log_wince_ctrl_hist_summary(const machine_t *m,
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

static inline bool pc_in_wince_div_corridor(uint32_t pc32)
{
    return pc32 >= UINT32_C(0x800968F0) && pc32 <= UINT32_C(0x80096934);
}

static void wince_div_step_log(machine_t *m, uc_engine *uc,
                               uint32_t pc32, uint32_t insn,
                               uint32_t sp32, uint32_t ra32,
                               const char *prefix,
                               uint32_t range_start, uint32_t range_end,
                               bool count_step_log)
{
    if (!m || !uc)
        return;
    if (!(pc32 >= range_start && pc32 <= range_end))
        return;
    if (m->wince_div_logs >= 320u)
        return;

    uint32_t op = (insn >> 26) & 0x3Fu;
    uint32_t rs = (insn >> 21) & 0x1Fu;
    uint32_t rt = (insn >> 16) & 0x1Fu;
    const char *kind = "STEP";
    char detail[128];
    detail[0] = '\0';

    if (op == 0x02u || op == 0x03u) {
        uint32_t target = ((pc32 + 4u) & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2);
        kind = (op == 0x03u) ? "JAL" : "J";
        snprintf(detail, sizeof(detail), "target=0x%08X", target);
    } else if (op == 0x04u || op == 0x05u || op == 0x14u || op == 0x15u) {
        uint64_t vrs = 0, vrt = 0;
        uc_reg_read(uc, UC_MIPS_REG_0 + (int)rs, &vrs);
        uc_reg_read(uc, UC_MIPS_REG_0 + (int)rt, &vrt);
        int32_t imm = (int32_t)(int16_t)(insn & 0xFFFFu);
        uint32_t target = pc32 + 4u + (uint32_t)(imm << 2);
        bool eq = ((uint32_t)vrs == (uint32_t)vrt);
        bool taken = (op == 0x04u || op == 0x14u) ? eq : !eq;
        kind = (op == 0x04u) ? "BEQ" :
               (op == 0x05u) ? "BNE" :
               (op == 0x14u) ? "BEQL" : "BNEL";
        snprintf(detail, sizeof(detail),
                 "taken=%u target=0x%08X rs=0x%08X rt=0x%08X",
                 taken ? 1u : 0u, target, (uint32_t)vrs, (uint32_t)vrt);
    } else if (op == 0x06u || op == 0x07u || op == 0x16u || op == 0x17u) {
        uint64_t vrs = 0;
        uc_reg_read(uc, UC_MIPS_REG_0 + (int)rs, &vrs);
        int32_t imm = (int32_t)(int16_t)(insn & 0xFFFFu);
        uint32_t target = pc32 + 4u + (uint32_t)(imm << 2);
        int32_t sv = (int32_t)(uint32_t)vrs;
        bool taken = (op == 0x06u || op == 0x16u) ? (sv <= 0) : (sv > 0);
        kind = (op == 0x06u) ? "BLEZ" :
               (op == 0x07u) ? "BGTZ" :
               (op == 0x16u) ? "BLEZL" : "BGTZL";
        snprintf(detail, sizeof(detail),
                 "taken=%u target=0x%08X rs=0x%08X",
                 taken ? 1u : 0u, target, (uint32_t)vrs);
    } else if (op == 0x01u) {
        uint64_t vrs = 0;
        uc_reg_read(uc, UC_MIPS_REG_0 + (int)rs, &vrs);
        int32_t imm = (int32_t)(int16_t)(insn & 0xFFFFu);
        uint32_t target = pc32 + 4u + (uint32_t)(imm << 2);
        int32_t sv = (int32_t)(uint32_t)vrs;
        bool is_bltz = (rt == 0x00u);
        bool is_bgez = (rt == 0x01u);
        bool taken = false;
        if (is_bltz)
            taken = (sv < 0);
        else if (is_bgez)
            taken = (sv >= 0);
        kind = is_bltz ? "BLTZ" : (is_bgez ? "BGEZ" : "REGIMM");
        snprintf(detail, sizeof(detail),
                 "taken=%u target=0x%08X rs=0x%08X rt=0x%02X",
                 taken ? 1u : 0u, target, (uint32_t)vrs, rt);
    } else if (op == 0x09u && rs == 29u && rt == 29u) {
        int32_t imm = (int32_t)(int16_t)(insn & 0xFFFFu);
        kind = "ADDIU_SP";
        snprintf(detail, sizeof(detail), "imm=%d next_sp=0x%08X",
                 imm, (uint32_t)((int32_t)sp32 + imm));
    } else if (op == 0x00u) {
        uint32_t funct = insn & 0x3Fu;
        if (funct == 0x08u || funct == 0x09u) {
            uint64_t target = 0;
            uc_reg_read(uc, UC_MIPS_REG_0 + (int)rs, &target);
            kind = (funct == 0x09u) ? "JALR" : "JR";
            snprintf(detail, sizeof(detail), "target=0x%08X rs=$%u",
                     (uint32_t)target, rs);
        }
    }

    fprintf(stderr,
            "[%s] pc=0x%08X insn=0x%08X kind=%s"
            " sp=0x%08X ra=0x%08X %s\n",
            prefix, pc32, insn, kind, sp32, ra32, detail);
    m->wince_div_logs++;
    if (count_step_log)
        m->wince_div_call_trace.step_logs++;
}

static void wince_div_call_step_log(machine_t *m, uc_engine *uc,
                                    uint32_t pc32, uint32_t insn,
                                    uint32_t sp32, uint32_t ra32)
{
    wince_div_step_log(m, uc, pc32, insn, sp32, ra32,
                       "WINCE_DIV_CALL_STEP",
                       UINT32_C(0x80096780), UINT32_C(0x80096910), true);
}

static void wince_div_prearm_step_log(machine_t *m, uc_engine *uc,
                                      uint32_t pc32, uint32_t insn,
                                      uint32_t sp32, uint32_t ra32)
{
    bool caller_seed_window = (pc32 >= UINT32_C(0x80079640) &&
                               pc32 <= UINT32_C(0x80079894));
    bool helper_window = (pc32 >= UINT32_C(0x8007A65C) &&
                          pc32 <= UINT32_C(0x8007A770));
    bool midbody_window = (pc32 >= UINT32_C(0x80096884) &&
                           pc32 <= UINT32_C(0x80096904));
    static uint32_t prearm_logs = 0;

    if (!caller_seed_window && !helper_window && !midbody_window)
        return;
    if (prearm_logs >= 128u)
        return;

    wince_div_step_log(m, uc, pc32, insn, sp32, ra32,
                       "WINCE_DIV_PREARM_STEP",
                       0u, UINT32_MAX, false);
    prearm_logs++;
}

void wince_div_call_trace_step(machine_t *m, uc_engine *uc, uint32_t pc32, uint32_t insn)
{
    wince_div_call_trace_t *t = &m->wince_div_call_trace;
    bool arm_site = (pc32 == 0x80096900u && insn == 0x0C0259E4u);
    bool caller_body_pc = (pc32 >= 0x800967FCu && pc32 <= 0x8009692Cu);
    bool upstream_pc =
        (pc32 >= 0x80079640u && pc32 <= 0x80079894u) ||
        (pc32 >= 0x8007A640u && pc32 <= 0x8007A780u) ||
        (pc32 >= 0x800A7DCCu && pc32 <= 0x800A7E10u);
    if (!arm_site && !t->active && !caller_body_pc && !upstream_pc)
        return;

    uint64_t sp = 0, ra = 0;
    uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
    uint32_t sp32 = (uint32_t)sp;
    uint32_t ra32 = (uint32_t)ra;

    if (arm_site) {
        memset(t, 0, sizeof(*t));
        t->active = true;
        t->call_pc = pc32;
        t->target_pc = 0x80096790u;
        t->return_pc = 0x80096908u;
        t->sp_before_call = sp32;
        t->sp_min = sp32;
        t->sp_max = sp32;
        t->events = 1u;
        t->target_hits = 0u;
        t->nested_calls = 0u;
        t->repeat_hits = 0u;
        t->step_logs = 0u;
        t->last_valid = false;
        t->last_pc = 0u;
        t->last_sp = 0u;
        wince_div_stack_watch_arm(m, uc, t->call_pc, t->sp_before_call);

        if (m->wince_div_logs < 224u) {
            fprintf(stderr,
                    "[WINCE_DIV_CALL_ARM] pc=0x%08X target=0x%08X return=0x%08X"
                    " sp_before=0x%08X ra=0x%08X\n",
                    t->call_pc, t->target_pc, t->return_pc,
                    t->sp_before_call, ra32);
            m->wince_div_logs++;
        }

        /* Step 1: One-shot NK corridor byte dump at the call arm site.
         * Dump PA 0x00096780..0x00096940 (VA 0x80096780..0x80096940) as hex
         * words to stderr for offline disassembly comparison. */
        {
            static bool corridor_dumped = false;
            if (!corridor_dumped) {
                corridor_dumped = true;
                const uint32_t corr_pa_start = 0x00096780u;
                const uint32_t corr_pa_end   = 0x00096940u;
                fprintf(stderr, "[NK_CORRIDOR_DUMP] pa_range=0x%08X..0x%08X bytes=",
                        corr_pa_start, corr_pa_end);
                for (uint32_t pa = corr_pa_start; pa < corr_pa_end; pa += 4u) {
                    uint32_t word = 0;
                    uc_err err = uc_mem_read(uc, (uint64_t)pa, &word, sizeof(word));
                    if (err == UC_ERR_OK)
                        fprintf(stderr, "%08X", word);
                    else
                        fprintf(stderr, "XXXXXXXX");
                }
                fprintf(stderr, "\n");
                /* Also dump as individual words for readability */
                for (uint32_t pa = corr_pa_start; pa < corr_pa_end; pa += 4u) {
                    uint32_t word = 0;
                    uc_err err = uc_mem_read(uc, (uint64_t)pa, &word, sizeof(word));
                    fprintf(stderr, "[NK_CORRIDOR_DUMP] VA=0x%08X PA=0x%08X insn=0x%08X%s\n",
                            0x80000000u + pa, pa, (err == UC_ERR_OK) ? word : 0u,
                            (err != UC_ERR_OK) ? " ERR" : "");
                }
            }
        }

        return;
    }

    /* Pre-arm caller provenance logging for the full caller body. */
    if (m->cfg.log_wince_stall) {
        if (pc32 == 0x800797DCu) {
            static uint32_t ctx_restore_logs = 0;
            if (ctx_restore_logs < 16u) {
                uint64_t t0 = 0;
                uint32_t ctx_va = 0;
                uint32_t ctx_pa = 0;
                uint32_t saved_sp = 0;
                uint32_t saved_ra = 0;
                bool ctx_pa_ok = false;
                bool saved_sp_ok = false;
                bool saved_ra_ok = false;
                char ctx_pa_desc[32];
                char stack0_desc[64];
                char future_s0_desc[64];
                char future_ra_desc[64];
                char saved_sp_desc[64];
                char saved_ra_desc[64];

                uc_reg_read(uc, UC_MIPS_REG_T0, &t0);
                ctx_va = (uint32_t)t0;
                ctx_pa_ok = wince_diag_read_va_via_shadow_tlb(m, uc, ctx_va, NULL, &ctx_pa);
                if (ctx_pa_ok)
                    snprintf(ctx_pa_desc, sizeof(ctx_pa_desc), "0x%08X", ctx_pa);
                else
                    snprintf(ctx_pa_desc, sizeof(ctx_pa_desc), "MISS");
                saved_sp_ok = wince_diag_read_va_via_shadow_tlb(m, uc, ctx_va + 0x6Cu,
                                                                &saved_sp, NULL);
                saved_ra_ok = wince_diag_read_va_via_shadow_tlb(m, uc, ctx_va + 0x74u,
                                                                &saved_ra, NULL);
                wince_diag_format_va_word(m, uc, ctx_va + 0x6Cu,
                                          saved_sp_desc, sizeof(saved_sp_desc));
                wince_diag_format_va_word(m, uc, ctx_va + 0x74u,
                                          saved_ra_desc, sizeof(saved_ra_desc));
                if (saved_sp_ok) {
                    wince_diag_format_va_word(m, uc, saved_sp,
                                              stack0_desc, sizeof(stack0_desc));
                    wince_diag_format_va_word(m, uc, saved_sp - 0x54u,
                                              future_s0_desc, sizeof(future_s0_desc));
                    wince_diag_format_va_word(m, uc, saved_sp - 0x50u,
                                              future_ra_desc, sizeof(future_ra_desc));
                } else {
                    snprintf(stack0_desc, sizeof(stack0_desc), "unknown");
                    snprintf(future_s0_desc, sizeof(future_s0_desc), "unknown");
                    snprintf(future_ra_desc, sizeof(future_ra_desc), "unknown");
                }

                fprintf(stderr,
                        "[WINCE_DIV_CTX_RESTORE] pc=0x%08X ctx_va=0x%08X"
                        " ctx_pa=%s sp=0x%08X ra=0x%08X"
                        " saved_sp=%s saved_ra=%s stack0=%s"
                        " future_s0=%s future_ra=%s"
                        " saved_sp_rt=%s saved_ra_rt=%s\n",
                        pc32, ctx_va,
                        ctx_pa_desc,
                        sp32, ra32,
                        saved_sp_desc, saved_ra_desc, stack0_desc,
                        future_s0_desc, future_ra_desc,
                        saved_sp_ok ? "ok" : "miss",
                        saved_ra_ok ? "ok" : "miss");
                ctx_restore_logs++;
            }
        }

        if (pc32 == 0x800797E0u) {
            static uint32_t ctx_jump_logs = 0;
            if (ctx_jump_logs < 16u) {
                uint64_t t0 = 0;
                uc_reg_read(uc, UC_MIPS_REG_T0, &t0);
                fprintf(stderr,
                        "[WINCE_DIV_CTX_JUMP] pc=0x%08X sp=0x%08X next_sp=0x%08X"
                        " ra=0x%08X t0=0x%08X\n",
                        pc32, sp32, sp32 + 4u, ra32, (uint32_t)t0);
                ctx_jump_logs++;
            }
        }

        if (pc32 == 0x80079660u ||
            pc32 == 0x80096894u ||
            pc32 == 0x8007A660u ||
            pc32 == 0x8007A680u ||
            pc32 == 0x8007A76Cu ||
            pc32 == 0x800A7DCCu ||
            pc32 == 0x8009689Cu ||
            pc32 == 0x800968C0u ||
            pc32 == 0x800968E4u ||
            pc32 == 0x800962B0u ||
            pc32 == 0x800962A0u ||
            pc32 == 0x80096244u) {
            static uint32_t upstream_logs = 0;
            if (upstream_logs < 64u) {
                uint64_t a0 = 0, a1 = 0, a2 = 0, a3 = 0, v0 = 0, s0 = 0, t0 = 0;
                const char *tag = "WINCE_DIV_UPSTREAM";
                uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
                uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
                uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
                uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
                uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
                uc_reg_read(uc, UC_MIPS_REG_S0, &s0);
                uc_reg_read(uc, UC_MIPS_REG_T0, &t0);
                if (pc32 == 0x80079660u)
                    tag = "WINCE_DIV_UPSTREAM_CALLER";
                else if (pc32 == 0x80096894u)
                    tag = "WINCE_DIV_ALT_ENTRY";
                else if (pc32 == 0x8007A660u)
                    tag = "WINCE_DIV_UPSTREAM_SEED";
                else if (pc32 == 0x8007A680u)
                    tag = "WINCE_DIV_UPSTREAM_JAL";
                else if (pc32 == 0x8007A76Cu)
                    tag = "WINCE_DIV_UPSTREAM_RET";
                else if (pc32 == 0x800A7DCCu)
                    tag = "WINCE_DIV_UPSTREAM_SUBCALL";
                else if (pc32 == 0x8009689Cu)
                    tag = "WINCE_DIV_MIDBODY_ENTRY_9C";
                else if (pc32 == 0x800968C0u)
                    tag = "WINCE_DIV_MIDBODY_ENTRY_C0";
                else if (pc32 == 0x800968E4u)
                    tag = "WINCE_DIV_MIDBODY_ENTRY_E4";
                else if (pc32 == 0x800962B0u)
                    tag = "WINCE_DIV_PRODUCER_FUNC";
                else if (pc32 == 0x800962A0u)
                    tag = "WINCE_DIV_PRODUCER_ENTRY_A0";
                else if (pc32 == 0x80096244u)
                    tag = "WINCE_DIV_PRODUCER_TWIN_44";
                fprintf(stderr,
                        "[%s] pc=0x%08X insn=0x%08X sp=0x%08X ra=0x%08X"
                        " a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X"
                        " v0=0x%08X s0=0x%08X t0=0x%08X events=%u\n",
                        tag, pc32, insn, sp32, ra32,
                        (uint32_t)a0, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3,
                        (uint32_t)v0, (uint32_t)s0, (uint32_t)t0, t->events);
                upstream_logs++;
            }
        }

        /* --- Producer function 0x800962B0 reachability instrumentation --- */
        if (pc32 == 0x800962B0u) {
            /* One-shot live byte dump of producer window */
            static bool producer_dump_done = false;
            if (!producer_dump_done && m->cfg.log_wince_stall) {
                producer_dump_done = true;
                fprintf(stderr, "[NK_PRODUCER_DUMP] VA=0x800962B0..0x80096310:");
                for (uint32_t va = 0x800962B0u; va < 0x80096310u; va += 4u) {
                    uint32_t w = 0;
                    uc_mem_read(uc, mips_sext(va), &w, sizeof(w));
                    fprintf(stderr, " %08X", w);
                }
                fprintf(stderr, "\n");
            }
            /* Producer entry log */
            static uint32_t producer_entry_logs = 0;
            if (m->cfg.log_wince_stall && producer_entry_logs < 32u) {
                uint64_t a0 = 0, v1 = 0, s0 = 0, t7 = 0, at = 0;
                uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
                uc_reg_read(uc, UC_MIPS_REG_V1, &v1);
                uc_reg_read(uc, UC_MIPS_REG_S0, &s0);
                uc_reg_read(uc, UC_MIPS_REG_T7, &t7);
                uc_reg_read(uc, UC_MIPS_REG_AT, &at);
                fprintf(stderr,
                        "[WINCE_DIV_PRODUCER_ENTRY] pc=0x%08X sp=0x%08X ra=0x%08X"
                        " a0=0x%08X v1=0x%08X s0=0x%08X t7=0x%08X at=0x%08X"
                        " branch_pred=%s\n",
                        pc32, sp32, ra32,
                        (uint32_t)a0, (uint32_t)v1, (uint32_t)s0,
                        (uint32_t)t7, (uint32_t)at,
                        ((uint32_t)t7 != (uint32_t)at) ? "SKIP(bne)" : "FALL");
                producer_entry_logs++;
            }
        }

        if (pc32 == 0x800962B8u) {
            static uint32_t b8_logs = 0;
            if (m->cfg.log_wince_stall && b8_logs < 32u) {
                uint64_t t8 = 0;
                uc_reg_read(uc, UC_MIPS_REG_T8, &t8);
                fprintf(stderr,
                        "[WINCE_DIV_PRODUCER_PAST_BNE] pc=0x%08X t8(g9510)=0x%08X"
                        " second_branch=%s\n",
                        pc32, (uint32_t)t8,
                        ((uint32_t)t8 != 0) ? "SKIP(bnezl)" : "FALL_TO_STORES");
                b8_logs++;
            }
        }

        if (pc32 == 0x800962D4u) {
            static uint32_t store_logs = 0;
            if (m->cfg.log_wince_stall && store_logs < 32u) {
                uint64_t t0 = 0, a0 = 0, v1 = 0, at = 0;
                uc_reg_read(uc, UC_MIPS_REG_T0, &t0);
                uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
                uc_reg_read(uc, UC_MIPS_REG_V1, &v1);
                uc_reg_read(uc, UC_MIPS_REG_AT, &at);
                uint32_t a1_target = (uint32_t)at + 0x9508u;
                fprintf(stderr,
                        "[WINCE_DIV_PRODUCER_STORE] pc=0x%08X at=0x%08X"
                        " t0(a1_arg)=0x%08X->0x%08X"
                        " a0(a3_arg)=0x%08X->0x806794EC"
                        " v1(obj)=0x%08X\n",
                        pc32, (uint32_t)at,
                        (uint32_t)t0, a1_target,
                        (uint32_t)a0,
                        (uint32_t)v1);
                store_logs++;
            }
        }

        /* --- Init reachability: actual function entries + fptr table dump --- */
        if (pc32 == 0x800962A0u) {
            static uint32_t a0_entry_logs = 0;
            if (m->cfg.log_wince_stall && a0_entry_logs < 16u) {
                uint64_t a0r = 0, v1r = 0, t6r = 0, t7r = 0;
                uc_reg_read(uc, UC_MIPS_REG_A0, &a0r);
                uc_reg_read(uc, UC_MIPS_REG_V1, &v1r);
                uc_reg_read(uc, UC_MIPS_REG_T6, &t6r);
                uc_reg_read(uc, UC_MIPS_REG_T7, &t7r);
                fprintf(stderr,
                        "[INIT_REACH_ENTRY_A0] pc=0x%08X sp=0x%08X ra=0x%08X"
                        " a0=0x%08X v1=0x%08X t6=0x%08X t7=0x%08X\n",
                        pc32, sp32, ra32,
                        (uint32_t)a0r, (uint32_t)v1r,
                        (uint32_t)t6r, (uint32_t)t7r);
                a0_entry_logs++;
            }
        }

        if (pc32 == 0x80096244u) {
            static uint32_t twin_entry_logs = 0;
            if (m->cfg.log_wince_stall && twin_entry_logs < 16u) {
                uint64_t a0r = 0, v1r = 0, t6r = 0, t7r = 0;
                uc_reg_read(uc, UC_MIPS_REG_A0, &a0r);
                uc_reg_read(uc, UC_MIPS_REG_V1, &v1r);
                uc_reg_read(uc, UC_MIPS_REG_T6, &t6r);
                uc_reg_read(uc, UC_MIPS_REG_T7, &t7r);
                fprintf(stderr,
                        "[INIT_REACH_TWIN_ENTRY_44] pc=0x%08X sp=0x%08X ra=0x%08X"
                        " a0=0x%08X v1=0x%08X t6=0x%08X t7=0x%08X\n",
                        pc32, sp32, ra32,
                        (uint32_t)a0r, (uint32_t)v1r,
                        (uint32_t)t6r, (uint32_t)t7r);
                twin_entry_logs++;
            }
        }

        /* --- Corridor outer function entry at 0x800967FC --- */
        if (pc32 == 0x800967FCu) {
            static uint32_t outer_call_count = 0;
            outer_call_count++;
            static uint32_t outer_entry_logs = 0;
            if (outer_entry_logs < 32u) {
                uint64_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
                uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
                uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
                uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
                uc_reg_read(uc, UC_MIPS_REG_A3, &a3);
                fprintf(stderr,
                        "[CORRIDOR_FUNC_ENTRY] call#%u pc=0x%08X ra=0x%08X sp=0x%08X"
                        " a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X\n",
                        outer_call_count, pc32, ra32, sp32,
                        (uint32_t)a0, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3);
                outer_entry_logs++;
            }
        }

        /* --- Corridor ra restore at 0x80096924 (lw ra, 0x24(sp)) --- */
        if (pc32 == 0x80096924u) {
            static uint32_t ra_restore_logs = 0;
            if (ra_restore_logs < 32u) {
                /* Read the value that will be loaded into ra from sp+0x24 */
                uint32_t saved_ra = 0;
                bool saved_ra_ok = (uc_mem_read(uc, mips_sext(sp32 + 0x24u), &saved_ra, 4) == UC_ERR_OK);
                uint32_t saved_s0 = 0;
                bool saved_s0_ok = (uc_mem_read(uc, mips_sext(sp32 + 0x20u), &saved_s0, 4) == UC_ERR_OK);
                fprintf(stderr,
                        "[CORRIDOR_RA_RESTORE] pc=0x%08X sp=0x%08X"
                        " [sp+0x24]=%s0x%08X [sp+0x20]=%s0x%08X"
                        " current_ra=0x%08X\n",
                        pc32, sp32,
                        saved_ra_ok ? "" : "ERR:", saved_ra,
                        saved_s0_ok ? "" : "ERR:", saved_s0,
                        ra32);
                ra_restore_logs++;
                /* If saved ra is zero, dump the full stack frame */
                if (saved_ra == 0u) {
                    fprintf(stderr, "[CORRIDOR_RA_RESTORE_ZERO] sp=0x%08X:", sp32);
                    for (uint32_t off = 0; off < 64u; off += 4u) {
                        uint32_t w = 0;
                        if (uc_mem_read(uc, mips_sext(sp32 + off), &w, 4) == UC_ERR_OK)
                            fprintf(stderr, " [+%02X]=0x%08X", off, w);
                        else
                            fprintf(stderr, " [+%02X]=ERR", off);
                    }
                    fprintf(stderr, "\n");
                }
            }
        }

        /* --- Corridor jr $ra at 0x80096928 --- */
        if (pc32 == 0x80096928u) {
            static uint32_t jr_ra_logs = 0;
            if (jr_ra_logs < 32u) {
                fprintf(stderr,
                        "[CORRIDOR_JR_RA] pc=0x%08X ra=0x%08X sp=0x%08X%s\n",
                        pc32, ra32, sp32,
                        (ra32 == 0u) ? " *** RA IS ZERO — WILL CRASH ***" : "");
                jr_ra_logs++;
            }
        }

        /* --- Corridor 0x800968E4 inner return point tracker --- */
        if (pc32 == 0x800968E4u) {
            static uint32_t corridor_call_count = 0;
            corridor_call_count++;

            /* Read caller instruction at ra-8 (JAL/JALR site) and ra-4 (delay slot) */
            uint32_t caller_insn = 0, delay_insn = 0;
            bool caller_insn_ok = false, delay_insn_ok = false;
            if (ra32 >= 8u && ra32 >= 0x80000000u) {
                caller_insn_ok = (uc_mem_read(uc, mips_sext(ra32 - 8u), &caller_insn, 4) == UC_ERR_OK);
                delay_insn_ok  = (uc_mem_read(uc, mips_sext(ra32 - 4u), &delay_insn, 4) == UC_ERR_OK);
            }

            /* Always log first 16 calls; after that, log only when ra changes or ra==0 */
            static uint32_t corridor_ra_prev = 0xDEADBEEF;
            bool should_log = (corridor_call_count <= 16u) ||
                              (ra32 == 0u) ||
                              (ra32 != corridor_ra_prev);
            static uint32_t corridor_entry_logs = 0;
            if (should_log && corridor_entry_logs < 128u) {
                uint64_t a0 = 0, a1 = 0, v0 = 0, s0 = 0, s1 = 0, t9 = 0;
                uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
                uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
                uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
                uc_reg_read(uc, UC_MIPS_REG_S0, &s0);
                uc_reg_read(uc, UC_MIPS_REG_S1, &s1);
                uc_reg_read(uc, UC_MIPS_REG_T9, &t9);
                fprintf(stderr,
                        "[CORRIDOR_ENTRY] call#%u pc=0x%08X ra=0x%08X sp=0x%08X"
                        " a0=0x%08X a1=0x%08X v0=0x%08X s0=0x%08X s1=0x%08X t9=0x%08X"
                        " caller_insn=%s0x%08X delay=%s0x%08X\n",
                        corridor_call_count, pc32, ra32, sp32,
                        (uint32_t)a0, (uint32_t)a1, (uint32_t)v0,
                        (uint32_t)s0, (uint32_t)s1, (uint32_t)t9,
                        caller_insn_ok ? "" : "ERR:", caller_insn,
                        delay_insn_ok ? "" : "ERR:", delay_insn);
                corridor_entry_logs++;
            }
            corridor_ra_prev = ra32;
        }

        /* --- One-shot function pointer table dump + callback ptr check --- */
        if (pc32 == 0x800968E4u) {
            /* Dump fptr table at 0x80075580..0x800755A0 */
            static bool fptr_table_dumped = false;
            if (!fptr_table_dumped && m->cfg.log_wince_stall) {
                fptr_table_dumped = true;
                fprintf(stderr, "[INIT_REACH_FPTR_TABLE] 0x80075580..0x800755A0:");
                for (uint32_t va = 0x80075580u; va < 0x800755A0u; va += 4u) {
                    uint32_t w = 0;
                    uc_mem_read(uc, mips_sext(va), &w, sizeof(w));
                    fprintf(stderr, " %08X", w);
                }
                fprintf(stderr, "\n");

                /* Check callback pointer global at 0x806794F0 */
                uint32_t cbptr = 0;
                uc_mem_read(uc, mips_sext(0x806794F0u), &cbptr, sizeof(cbptr));
                fprintf(stderr,
                        "[INIT_REACH_CBPTR_CHECK] 0x806794F0 = 0x%08X (%s)\n",
                        cbptr, cbptr ? "SET" : "ZERO/UNSET");

                /* Also check nearby callback globals */
                uint32_t g94EC = 0, g9508 = 0, g9510 = 0;
                uc_mem_read(uc, mips_sext(0x806794ECu), &g94EC, sizeof(g94EC));
                uc_mem_read(uc, mips_sext(0x80679508u), &g9508, sizeof(g9508));
                uc_mem_read(uc, mips_sext(0x80679510u), &g9510, sizeof(g9510));
                fprintf(stderr,
                        "[INIT_REACH_GLOBALS] g94EC=0x%08X g94F0=0x%08X"
                        " g9508=0x%08X g9510=0x%08X\n",
                        g94EC, cbptr, g9508, g9510);
            }
        }

        if (pc32 == 0x800968E4u) {
            static uint32_t e4_state_logs = 0;
            static bool nk_dump_done = false;
            if (e4_state_logs < 32u) {
                wince_div_log_resume_globals(m, uc, "WINCE_DIV_E4_STATE",
                                             pc32, sp32, ra32);
                e4_state_logs++;
            }
            /* One-shot NK code/data dump for offline producer scan */
            if (!nk_dump_done) {
                nk_dump_done = true;
                const uint32_t nk_dump_pa_start = UINT32_C(0x00060000);
                const uint32_t nk_dump_pa_end   = UINT32_C(0x006A0000);
                const uint32_t nk_dump_size = nk_dump_pa_end - nk_dump_pa_start;
                uint8_t *nk_buf = malloc(nk_dump_size);
                if (nk_buf) {
                    uc_err de = uc_mem_read(uc, (uint64_t)nk_dump_pa_start + UINT64_C(0x80000000),
                                            nk_buf, nk_dump_size);
                    if (de == UC_ERR_OK) {
                        FILE *fp = fopen("nk_code_dump.bin", "wb");
                        if (fp) {
                            fwrite(nk_buf, 1, nk_dump_size, fp);
                            fclose(fp);
                            fprintf(stderr,
                                    "[NK_CODE_DUMP] dumped PA 0x%08X-0x%08X (%u bytes)"
                                    " to nk_code_dump.bin\n",
                                    nk_dump_pa_start, nk_dump_pa_end, nk_dump_size);
                        } else {
                            fprintf(stderr, "[NK_CODE_DUMP] fopen failed: %s\n",
                                    strerror(errno));
                        }
                    } else {
                        fprintf(stderr,
                                "[NK_CODE_DUMP] uc_mem_read failed: %s\n",
                                uc_strerror(de));
                    }
                    free(nk_buf);
                }
            }
        }

        if (pc32 == 0x80096908u) {
            static uint32_t postcall_state_logs = 0;
            if (postcall_state_logs < 32u) {
                wince_div_log_resume_globals(m, uc, "WINCE_DIV_POSTCALL_STATE",
                                             pc32, sp32, ra32);
                postcall_state_logs++;
            }
        }

        if (pc32 == 0x80096910u) {
            static uint32_t callback_branch_logs = 0;
            if (callback_branch_logs < 32u) {
                uint64_t v0 = 0;
                uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
                wince_div_log_resume_globals(m, uc, "WINCE_DIV_CALLBACK_STATE",
                                             pc32, sp32, ra32);
                fprintf(stderr,
                        "[WINCE_DIV_CALLBACK_BRANCH] pc=0x%08X sp=0x%08X"
                        " ra=0x%08X v0=0x%08X beql_taken=%u target=0x80096924\n",
                        pc32, sp32, ra32, (uint32_t)v0,
                        ((uint32_t)v0 == 0u) ? 1u : 0u);
                callback_branch_logs++;
            }
        }

        if (pc32 == 0x800967FCu) {
            static uint32_t caller_push_logs = 0;
            if (caller_push_logs < 32u) {
                fprintf(stderr,
                        "[WINCE_DIV_CALLER_PUSH] pc=0x%08X sp=0x%08X"
                        " next_sp=0x%08X ra=0x%08X events=%u\n",
                        pc32, sp32, sp32 - 0x28u, ra32, t->events);
                caller_push_logs++;
            }
        }

        if (pc32 == 0x80096800u) {
            static uint32_t s0_store_logs = 0;
            if (s0_store_logs < 32u) {
                uint64_t s0_val = 0;
                uint32_t store_va = sp32 + 0x20u;
                uint32_t store_pa = 0;
                char pa_desc[32];
                uc_reg_read(uc, UC_MIPS_REG_S0, &s0_val);
                if (wince_diag_read_va_via_shadow_tlb(m, uc, store_va, NULL, &store_pa))
                    snprintf(pa_desc, sizeof(pa_desc), "0x%08X", store_pa);
                else
                    snprintf(pa_desc, sizeof(pa_desc), "TLB_MISS");
                fprintf(stderr,
                        "[WINCE_DIV_CALLER_S0_STORE] pc=0x%08X sp=0x%08X"
                        " store_va=0x%08X store_pa=%s s0=0x%08X\n",
                        pc32, sp32, store_va, pa_desc, (uint32_t)s0_val);
                s0_store_logs++;
            }
        }

        if (pc32 == 0x80096808u) {
            static uint32_t ra_store_logs = 0;
            if (ra_store_logs < 32u) {
                uint32_t store_va = sp32 + 0x24u;
                uint32_t store_pa = 0;
                char pa_desc[32];
                if (wince_diag_read_va_via_shadow_tlb(m, uc, store_va, NULL, &store_pa))
                    snprintf(pa_desc, sizeof(pa_desc), "0x%08X", store_pa);
                else
                    snprintf(pa_desc, sizeof(pa_desc), "TLB_MISS");
                fprintf(stderr,
                        "[WINCE_DIV_CALLER_RA_STORE] pc=0x%08X sp=0x%08X"
                        " store_va=0x%08X store_pa=%s ra=0x%08X\n",
                        pc32, sp32, store_va, pa_desc, ra32);
                ra_store_logs++;
            }
        }

        if (pc32 == 0x8009680Cu) {
            static uint32_t body_logs = 0;
            if (body_logs < 32u) {
                fprintf(stderr,
                        "[WINCE_DIV_CALLER_BODY] pc=0x%08X sp=0x%08X ra=0x%08X\n",
                        pc32, sp32, ra32);
                body_logs++;
            }
        }

        if (pc32 == 0x8009692Cu) {
            static uint32_t caller_pop_logs = 0;
            if (caller_pop_logs < 32u) {
                fprintf(stderr,
                        "[WINCE_DIV_CALLER_POP] pc=0x%08X sp=0x%08X"
                        " next_sp=0x%08X ra=0x%08X events=%u\n",
                        pc32, sp32, sp32 + 0x28u, ra32, t->events);
                caller_pop_logs++;
            }
        }

        if (insn == 0x0C0259E4u && pc32 != 0x80096900u) {
            static uint32_t other_jal_logs = 0;
            if (other_jal_logs < 32u) {
                fprintf(stderr,
                        "[WINCE_DIV_CALLER_JAL_CALLEE] pc=0x%08X sp=0x%08X"
                        " ra=0x%08X events=%u\n",
                        pc32, sp32, ra32, t->events);
                other_jal_logs++;
            }
        }
    }

    if (!t->active)
        wince_div_prearm_step_log(m, uc, pc32, insn, sp32, ra32);

    if (!t->active)
        return;

    t->events++;
    if (sp32 < t->sp_min)
        t->sp_min = sp32;
    if (sp32 > t->sp_max)
        t->sp_max = sp32;
    if (t->last_valid && t->last_pc == pc32) {
        t->repeat_hits++;
        if (m->wince_div_logs < 224u) {
            fprintf(stderr,
                    "[WINCE_DIV_CALL_REPEAT] pc=0x%08X sp_prev=0x%08X sp_now=0x%08X"
                    " repeats=%u events=%u\n",
                    pc32, t->last_sp, sp32, t->repeat_hits, t->events);
            m->wince_div_logs++;
        }
    }

    uint32_t op = (insn >> 26) & 0x3Fu;
    if (op == 0x03u) {
        uint32_t target = ((pc32 + 4u) & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2);
        if (target == t->target_pc) {
            t->nested_calls++;
            if (m->wince_div_logs < 224u) {
                fprintf(stderr,
                        "[WINCE_DIV_CALL_REENTER] pc=0x%08X target=0x%08X"
                        " nested_calls=%u sp=0x%08X ra=0x%08X\n",
                        pc32, target, t->nested_calls, sp32, ra32);
                m->wince_div_logs++;
            }
        }
    }

    if (pc32 == t->target_pc) {
        t->target_hits++;
        if (!t->seen_target) {
            t->seen_target = true;
            t->sp_at_target = sp32;
        }

        /* Step 1+3: Enhanced callee entry logging with a3, CP0, insn_count */
        uint64_t a3_val = 0, cp0_status = 0;
        uc_reg_read(uc, UC_MIPS_REG_A3, &a3_val);
        uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &cp0_status);
        const char *path = ((uint32_t)a3_val == 0) ? "beql_exit" : "full_body";

        if (m->wince_div_logs < 256u) {
            fprintf(stderr,
                    "[WINCE_DIV_CALLEE_ENTRY] count=%u insn_count=%llu"
                    " sp=0x%08X ra=0x%08X a3=0x%08X path=%s"
                    " pending_cause=0x%08X status=0x%08X"
                    " pending_excode=0x%08X events=%u\n",
                    t->target_hits, (unsigned long long)m->insn_count,
                    sp32, ra32, (uint32_t)a3_val, path,
                    m->pending_cause, (uint32_t)cp0_status,
                    m->pending_excode, t->events);
            m->wince_div_logs++;
        }

        /* On replay (second+ entry), dump last 8 ctrl_hist entries */
        if (t->target_hits >= 2u && m->wince_div_logs < 256u) {
            uint32_t dump_count = m->wince_ctrl_hist_count < 8u
                                    ? m->wince_ctrl_hist_count : 8u;
            fprintf(stderr,
                    "[WINCE_DIV_CALLEE_REPLAY] count=%u pending_excode=0x%08X"
                    " ctrl_hist_last_%u:\n",
                    t->target_hits, m->pending_excode, dump_count);
            m->wince_div_logs++;
            for (uint32_t i = 0; i < dump_count && m->wince_div_logs < 256u; i++) {
                uint32_t idx = (m->wince_ctrl_hist_head + WINCE_CTRL_HISTORY_LEN
                                - dump_count + i) % WINCE_CTRL_HISTORY_LEN;
                wince_ctrl_hist_entry_t *e = &m->wince_ctrl_hist[idx];
                static const char *kind_names[] = {
                    "?", "JAL", "JALR", "JR_RA", "MTC0_STATUS"
                };
                const char *kn = (e->kind <= 4) ? kind_names[e->kind] : "?";
                fprintf(stderr,
                        "  [CTRL_HIST %u] pc=0x%08X target=0x%08X"
                        " sp=0x%08X ra=0x%08X kind=%s value=0x%08X\n",
                        i, e->pc, e->target, e->sp, e->ra, kn, e->value);
                m->wince_div_logs++;
            }
        }
    }

    /* Step 1: Log callee epilogue/return path (lw ra at 0x800967F0, jr ra at 0x800967F4) */
    if (pc32 == 0x800967F0u || pc32 == 0x800967F4u) {
        if (m->wince_div_logs < 256u) {
            const char *tag = (pc32 == 0x800967F0u)
                ? "WINCE_DIV_CALLEE_EPILOGUE" : "WINCE_DIV_CALLEE_JR";
            fprintf(stderr,
                    "[%s] pc=0x%08X insn=0x%08X sp=0x%08X ra=0x%08X"
                    " target_hits=%u events=%u\n",
                    tag, pc32, insn, sp32, ra32,
                    t->target_hits, t->events);
            m->wince_div_logs++;
        }
    }

    wince_div_call_step_log(m, uc, pc32, insn, sp32, ra32);

    if (pc32 == t->return_pc) {
        t->sp_at_return = sp32;
        t->sp_drift = (int32_t)((int32_t)sp32 - (int32_t)t->sp_before_call);
        t->active = false;
        t->completed = true;
        wince_div_stack_watch_mark_completed(m);
        if (m->wince_div_logs < 224u) {
            fprintf(stderr,
                    "[WINCE_DIV_CALL_RETURN] pc=0x%08X sp_before=0x%08X"
                    " sp_after=0x%08X drift=%d seen_target=%u events=%u\n",
                    pc32, t->sp_before_call, t->sp_at_return,
                    t->sp_drift, t->seen_target ? 1u : 0u, t->events);
            m->wince_div_logs++;
        }
    }

    t->last_pc = pc32;
    t->last_sp = sp32;
    t->last_valid = true;
}

void wince_div_hist_record(machine_t *m, uc_engine *uc, uint32_t pc32, uint32_t insn)
{
    if (!is_wince_boot_machine(m) || !m->cfg.log_wince_stall)
        return;
    if (!pc_in_wince_div_corridor(pc32))
        return;

    uint64_t ra = 0, sp = 0, v0 = 0, t9 = 0, s0 = 0, status = 0;
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
    uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
    uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
    uc_reg_read(uc, UC_MIPS_REG_T9, &t9);
    uc_reg_read(uc, UC_MIPS_REG_S0, &s0);
    uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &status);

    uint32_t stack20 = 0, stack24 = 0;
    bool stack20_ok = read_guest_u32(uc, (uint32_t)sp + 0x20u, &stack20);
    bool stack24_ok = read_guest_u32(uc, (uint32_t)sp + 0x24u, &stack24);

    uint32_t idx = m->wince_div_hist_head % WINCE_DIV_HISTORY_LEN;
    wince_div_hist_entry_t *e = &m->wince_div_hist[idx];
    e->pc = pc32;
    e->insn = insn;
    e->ra = (uint32_t)ra;
    e->sp = (uint32_t)sp;
    e->v0 = (uint32_t)v0;
    e->t9 = (uint32_t)t9;
    e->s0 = (uint32_t)s0;
    e->status = (uint32_t)status;
    e->stack20 = stack20;
    e->stack24 = stack24;
    e->stack20_ok = stack20_ok ? 1u : 0u;
    e->stack24_ok = stack24_ok ? 1u : 0u;
    e->reserved = 0;

    m->wince_div_hist_head = (m->wince_div_hist_head + 1u) % WINCE_DIV_HISTORY_LEN;
    if (m->wince_div_hist_count < WINCE_DIV_HISTORY_LEN)
        m->wince_div_hist_count++;

    bool key_pc = (pc32 == 0x80096910u ||
                   pc32 == 0x80096918u ||
                   pc32 == 0x80096924u ||
                   pc32 == 0x80096928u ||
                   pc32 == 0x8009692Cu);
    if (key_pc && m->wince_div_logs < 160u) {
        fprintf(stderr,
                "[WINCE_DIV_EVENT] pc=0x%08X insn=0x%08X"
                " ra=0x%08X sp=0x%08X v0=0x%08X t9=0x%08X s0=0x%08X"
                " stk20=%s0x%08X stk24=%s0x%08X status=0x%08X\n",
                e->pc, e->insn, e->ra, e->sp, e->v0, e->t9, e->s0,
                e->stack20_ok ? "" : "ERR:", e->stack20,
                e->stack24_ok ? "" : "ERR:", e->stack24,
                e->status);
        m->wince_div_logs++;
    }
}

void log_wince_div_call_trace_summary(const machine_t *m, const char *reason)
{
    if (!m)
        return;
    const wince_div_call_trace_t *t = &m->wince_div_call_trace;
    if (!t->active && !t->completed)
        return;

    fprintf(stderr,
            "[WINCE_DIV_CALL_SUMMARY] reason=%s active=%u completed=%u"
            " call=0x%08X target=0x%08X return=0x%08X"
            " sp_before=0x%08X sp_target=0x%08X sp_return=0x%08X"
            " drift=%d sp_min=0x%08X sp_max=0x%08X"
            " events=%u seen_target=%u target_hits=%u nested_calls=%u"
            " repeat_hits=%u step_logs=%u\n",
            reason,
            t->active ? 1u : 0u, t->completed ? 1u : 0u,
            t->call_pc, t->target_pc, t->return_pc,
            t->sp_before_call, t->sp_at_target, t->sp_at_return,
            t->sp_drift, t->sp_min, t->sp_max, t->events,
            t->seen_target ? 1u : 0u,
            t->target_hits, t->nested_calls, t->repeat_hits, t->step_logs);
}

void log_wince_div_hist_summary(const machine_t *m,
                                       const char *reason,
                                       uint32_t max_lines)
{
    if (!m || m->wince_div_hist_count == 0)
        return;

    uint32_t emit = m->wince_div_hist_count;
    if (emit > max_lines)
        emit = max_lines;
    uint32_t dropped = m->wince_div_hist_count - emit;
    uint32_t start = (m->wince_div_hist_head + WINCE_DIV_HISTORY_LEN - emit)
                   % WINCE_DIV_HISTORY_LEN;

    fprintf(stderr,
            "[WINCE_DIV_SUMMARY] reason=%s entries=%u emitted=%u dropped=%u"
            " corridor=0x800968F0-0x80096934\n",
            reason, m->wince_div_hist_count, emit, dropped);

    for (uint32_t i = 0; i < emit; i++) {
        uint32_t idx = (start + i) % WINCE_DIV_HISTORY_LEN;
        const wince_div_hist_entry_t *e = &m->wince_div_hist[idx];
        fprintf(stderr,
                "[WINCE_DIV] %02u pc=0x%08X insn=0x%08X"
                " ra=0x%08X sp=0x%08X v0=0x%08X t9=0x%08X s0=0x%08X"
                " stk20=%s0x%08X stk24=%s0x%08X status=0x%08X\n",
                i, e->pc, e->insn,
                e->ra, e->sp, e->v0, e->t9, e->s0,
                e->stack20_ok ? "" : "ERR:", e->stack20,
                e->stack24_ok ? "" : "ERR:", e->stack24,
                e->status);
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

void maybe_probe_wince_ctx_path(machine_t *m, uc_engine *uc,
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

void maybe_probe_wince_bootctx_gate(machine_t *m, uc_engine *uc,
                                           uint32_t pc32)
{
    if (!is_wince_boot_machine(m) || !m->cfg.log_wince_stall)
        return;
    if (pc32 != 0x80079C70u)
        return;

    static uint32_t gate_log_count = 0;
    if (gate_log_count >= 16u)
        return;

    uint64_t ra = 0, a0 = 0, a1 = 0, v0 = 0, status = 0;
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
    uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
    uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
    uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
    uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &status);

    uint32_t boot0 = 0, boot1 = 0, boot2 = 0;
    uint32_t sig0 = 0, sig1 = 0;
    bool ok_boot0 = read_guest_u32(uc, 0xA0006000u, &boot0);
    bool ok_boot1 = read_guest_u32(uc, 0xA0006004u, &boot1);
    bool ok_boot2 = read_guest_u32(uc, 0xA0006008u, &boot2);
    bool ok_sig0 = read_guest_u32(uc, 0x800748D0u, &sig0);
    bool ok_sig1 = read_guest_u32(uc, 0x800748D4u, &sig1);

    bool sig_ok = (ok_boot0 && ok_boot1 && ok_sig0 && ok_sig1 &&
                   boot0 == sig0 && boot1 == sig1);
    fprintf(stderr,
            "[WINCE_BOOTCTX_GATE] pc=0x%08X ra=0x%08X"
            " a0=0x%08X a1=0x%08X v0=0x%08X status=0x%08X"
            " boot=[0x%08X 0x%08X 0x%08X] sig=[0x%08X 0x%08X] match=%u"
            " rd_ok=[%u %u %u %u %u]\n",
            pc32, (uint32_t)ra,
            (uint32_t)a0, (uint32_t)a1, (uint32_t)v0, (uint32_t)status,
            boot0, boot1, boot2, sig0, sig1, sig_ok ? 1u : 0u,
            ok_boot0 ? 1u : 0u, ok_boot1 ? 1u : 0u, ok_boot2 ? 1u : 0u,
            ok_sig0 ? 1u : 0u, ok_sig1 ? 1u : 0u);
    gate_log_count++;
}

void maybe_probe_wince_live_call_targets(machine_t *m, uc_engine *uc,
                                                uint32_t pc32)
{
    if (!is_wince_boot_machine(m) || !m->cfg.log_wince_stall)
        return;
    if (pc32 != 0x80079AC4u && pc32 != 0x8007AFA8u)
        return;

    static uint8_t seen_79ac4 = 0;
    static uint8_t seen_7afa8 = 0;
    if (pc32 == 0x80079AC4u && seen_79ac4)
        return;
    if (pc32 == 0x8007AFA8u && seen_7afa8)
        return;

    uint64_t ra = 0, v0 = 0, t0 = 0, status = 0;
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
    uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
    uc_reg_read(uc, UC_MIPS_REG_T0, &t0);
    uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &status);

    uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0, w4 = 0, w5 = 0;
    bool ok0 = read_guest_u32(uc, pc32 + 0u, &w0);
    bool ok1 = read_guest_u32(uc, pc32 + 4u, &w1);
    bool ok2 = read_guest_u32(uc, pc32 + 8u, &w2);
    bool ok3 = read_guest_u32(uc, pc32 + 12u, &w3);
    bool ok4 = read_guest_u32(uc, pc32 + 16u, &w4);
    bool ok5 = read_guest_u32(uc, pc32 + 20u, &w5);

    fprintf(stderr,
            "[WINCE_LIVE_CALL] pc=0x%08X ra=0x%08X v0=0x%08X t0=0x%08X"
            " status=0x%08X words=[%08X %08X %08X %08X %08X %08X]"
            " ok=[%u %u %u %u %u %u]\n",
            pc32, (uint32_t)ra, (uint32_t)v0, (uint32_t)t0, (uint32_t)status,
            w0, w1, w2, w3, w4, w5,
            ok0 ? 1u : 0u, ok1 ? 1u : 0u, ok2 ? 1u : 0u,
            ok3 ? 1u : 0u, ok4 ? 1u : 0u, ok5 ? 1u : 0u);

    if (pc32 == 0x80079AC4u)
        seen_79ac4 = 1;
    else
        seen_7afa8 = 1;
}

void maybe_probe_wince_gate_path_hits(machine_t *m, uc_engine *uc,
                                             uint32_t pc32)
{
    if (!is_wince_boot_machine(m) || !m->cfg.log_wince_stall)
        return;

    static uint32_t hit_795e8 = 0;
    static uint32_t hit_795f0 = 0;
    static uint32_t hit_79634 = 0;
    static uint32_t hit_79df8 = 0;
    static uint32_t hit_794c8 = 0;

    switch (pc32) {
    case 0x800795E8u:
        hit_795e8++;
        if (hit_795e8 <= 16u)
            fprintf(stderr, "[WINCE_GATE_HIT] pc=0x800795E8 count=%u\n", hit_795e8);
        break;
    case 0x800795F0u: {
        hit_795f0++;
        uint64_t t0 = 0, v0 = 0;
        uc_reg_read(uc, UC_MIPS_REG_T0, &t0);
        uc_reg_read(uc, UC_MIPS_REG_V0, &v0);

        if (hit_795f0 <= 16u) {
            fprintf(stderr,
                    "[WINCE_GATE_HIT] pc=0x800795F0 count=%u t0=0x%08X v0=0x%08X\n",
                    hit_795f0, (uint32_t)t0, (uint32_t)v0);
        }
        if ((uint32_t)v0 == 0u && (uint32_t)t0 == 0xAA00A000u && hit_795f0 <= 32u) {
            fprintf(stderr,
                    "[WINCE_GATE_OBS] pc=0x800795F0 count=%u t0=0xAA00A000 v0=0x00000000\n",
                    hit_795f0);
        }
        break;
    }
    case 0x80079634u:
        hit_79634++;
        if (hit_79634 <= 16u)
            fprintf(stderr, "[WINCE_GATE_HIT] pc=0x80079634 count=%u\n", hit_79634);
        break;
    case 0x80079DF8u:
        hit_79df8++;
        if (hit_79df8 <= 16u)
            fprintf(stderr, "[WINCE_GATE_HIT] pc=0x80079DF8 count=%u\n", hit_79df8);
        break;
    case 0x800794C8u:
        hit_794c8++;
        if (hit_794c8 <= 16u)
            fprintf(stderr, "[WINCE_GATE_HIT] pc=0x800794C8 count=%u\n", hit_794c8);
        break;
    default:
        break;
    }
}

void maybe_probe_wince_bootmode(machine_t *m, uc_engine *uc,
                                       uint32_t pc32, uint32_t insn)
{
    if (!is_wince_boot_machine(m) || !m->cfg.log_wince_stall)
        return;

    /* Observe bootmode gate state without mutating guest memory/registers. */
    if (pc32 == 0x800780B8u) {
        static uint32_t gate_obs_logs = 0;
        uint32_t mode = 0, gate = 0;
        bool mode_ok = read_guest_u32(uc, 0xA001D004u, &mode);
        bool gate_ok = read_guest_u32(uc, 0xA002D000u, &gate);
        if (gate_obs_logs < 8u) {
            fprintf(stderr,
                    "[WINCE_GATE_OBS] pc=0x%08X mode=%s0x%08X gate=%s0x%08X\n",
                    pc32,
                    mode_ok ? "" : "ERR:", mode,
                    gate_ok ? "" : "ERR:", gate);
            gate_obs_logs++;
        }
    }

    /* All_nand + CE_Net path: jal 0x80077E28 (callback vector select). */
    if (insn == 0x0C01DF8Au) {
        static uint32_t sel_logs = 0;
        if (sel_logs < 16u) {
            uint64_t a0 = 0, a1 = 0, a2 = 0, v1 = 0;
            uint32_t mode = 0, boot_gate = 0;
            bool mode_ok = false, gate_ok = false;
            uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
            uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
            uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
            uc_reg_read(uc, UC_MIPS_REG_V1, &v1);
            mode_ok = read_guest_u32(uc, 0xA001D004u, &mode);
            gate_ok = read_guest_u32(uc, 0xA002D000u, &boot_gate);
            fprintf(stderr,
                    "[WINCE_BOOTMODE] pc=0x%08X sel_call a0=0x%08X a1=0x%08X"
                    " a2=0x%08X v1=0x%08X"
                    " mode@A001D004=%s0x%08X gate@A002D000=%s0x%08X\n",
                    pc32, (uint32_t)a0, (uint32_t)a1, (uint32_t)a2, (uint32_t)v1,
                    mode_ok ? "" : "ERR:", mode,
                    gate_ok ? "" : "ERR:", boot_gate);
            sel_logs++;
        }
    }

    if (pc32 == 0x800780B8u || pc32 == 0x80078144u) {
        static uint32_t path_logs = 0;
        if (path_logs < 16u) {
            uint64_t a0 = 0, a1 = 0, a2 = 0, v0 = 0, v1 = 0;
            uint32_t mode = 0, gate = 0;
            bool mode_ok = read_guest_u32(uc, 0xA001D004u, &mode);
            bool gate_ok = read_guest_u32(uc, 0xA002D000u, &gate);
            uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
            uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
            uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
            uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
            uc_reg_read(uc, UC_MIPS_REG_V1, &v1);
            fprintf(stderr,
                    "[WINCE_BOOTMODE] pc=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X"
                    " v0=0x%08X v1=0x%08X mode=%s0x%08X gate=%s0x%08X\n",
                    pc32, (uint32_t)a0, (uint32_t)a1, (uint32_t)a2,
                    (uint32_t)v0, (uint32_t)v1,
                    mode_ok ? "" : "ERR:", mode,
                    gate_ok ? "" : "ERR:", gate);
            path_logs++;
        }
    }

    const char *edge = NULL;
    switch (pc32) {
    case 0x80078004u: edge = "ret_77dc0"; break;
    case 0x8007801Cu: edge = "pre_jalr_sp30"; break;
    case 0x80078020u: edge = "jalr_sp30"; break;
    case 0x80078028u: edge = "ret_jalr_sp30"; break;
    case 0x8007802Cu: edge = "jalr_sp34"; break;
    case 0x80078034u: edge = "ret_jalr_sp34"; break;
    case 0x80078048u: edge = "jalr_sp3c_a0_1"; break;
    case 0x80078050u: edge = "ret_jalr_sp3c_a0_1"; break;
    case 0x80078178u: edge = "branch_t5_to_78248"; break;
    case 0x80078180u: edge = "jalr_sp3c_a0_0"; break;
    case 0x80078188u: edge = "ret_jalr_sp3c_a0_0"; break;
    case 0x8007818Cu: edge = "jalr_sp38"; break;
    case 0x80078194u: edge = "ret_jalr_sp38"; break;
    case 0x80078220u: edge = "branch_t1_to_78248"; break;
    case 0x80078228u: edge = "jalr_sp3c_b"; break;
    case 0x80078230u: edge = "ret_jalr_sp3c_b"; break;
    case 0x80078234u: edge = "jalr_sp38_b"; break;
    case 0x8007823Cu: edge = "ret_jalr_sp38_b"; break;
    case 0x80078244u: edge = "pre_sel_call"; break;
    case 0x80078248u: edge = "sel_call_77e28"; break;
    case 0x80078250u: edge = "post_sel_call"; break;
    case 0x80078254u: edge = "epilogue"; break;
    default: break;
    }
    if (edge != NULL) {
        static uint32_t step_logs = 0;
        if (step_logs < 96u) {
            uint64_t ra = 0, sp = 0, a0 = 0, a1 = 0, a2 = 0, v0 = 0, v1 = 0;
            uint64_t t0 = 0, t1 = 0, t2 = 0, t3 = 0, t6 = 0, t7 = 0, t8 = 0, t9 = 0;
            uint32_t mode = 0, gate = 0, a2_word = 0;
            uint32_t sp_2c = 0, sp_30 = 0, sp_34 = 0, sp_38 = 0, sp_3c = 0, sp_40 = 0, sp_48 = 0;
            uint32_t objptr = 0, objsig = 0;
            uint32_t objptr_w0 = 0, objptr_w1 = 0, objptr_w2 = 0, objptr_w3 = 0;
            uint32_t obj66_w0 = 0, obj66_w1 = 0, obj66_w2 = 0, obj66_w3 = 0;
            uint32_t obj67_w0 = 0, obj67_w1 = 0, obj67_w2 = 0, obj67_w3 = 0;
            bool mode_ok = read_guest_u32(uc, 0xA001D004u, &mode);
            bool gate_ok = read_guest_u32(uc, 0xA002D000u, &gate);

            uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
            uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
            uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
            uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
            uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
            uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
            uc_reg_read(uc, UC_MIPS_REG_V1, &v1);
            uc_reg_read(uc, UC_MIPS_REG_T0, &t0);
            uc_reg_read(uc, UC_MIPS_REG_T1, &t1);
            uc_reg_read(uc, UC_MIPS_REG_T2, &t2);
            uc_reg_read(uc, UC_MIPS_REG_T3, &t3);
            uc_reg_read(uc, UC_MIPS_REG_T6, &t6);
            uc_reg_read(uc, UC_MIPS_REG_T7, &t7);
            uc_reg_read(uc, UC_MIPS_REG_T8, &t8);
            uc_reg_read(uc, UC_MIPS_REG_T9, &t9);

            read_guest_u32(uc, (uint32_t)sp + 0x2Cu, &sp_2c);
            read_guest_u32(uc, (uint32_t)sp + 0x30u, &sp_30);
            read_guest_u32(uc, (uint32_t)sp + 0x34u, &sp_34);
            read_guest_u32(uc, (uint32_t)sp + 0x38u, &sp_38);
            read_guest_u32(uc, (uint32_t)sp + 0x3Cu, &sp_3c);
            read_guest_u32(uc, (uint32_t)sp + 0x40u, &sp_40);
            read_guest_u32(uc, (uint32_t)sp + 0x48u, &sp_48);
            bool a2_ok = read_guest_u32(uc, (uint32_t)a2, &a2_word);

            read_guest_u32(uc, 0x80660000u, &objptr);
            read_guest_u32(uc, 0x80660004u, &objsig);
            read_guest_u32(uc, 0x8066BFC0u + 0u,  &obj66_w0);
            read_guest_u32(uc, 0x8066BFC0u + 4u,  &obj66_w1);
            read_guest_u32(uc, 0x8066BFC0u + 8u,  &obj66_w2);
            read_guest_u32(uc, 0x8066BFC0u + 12u, &obj66_w3);
            read_guest_u32(uc, 0x8067BFC0u + 0u,  &obj67_w0);
            read_guest_u32(uc, 0x8067BFC0u + 4u,  &obj67_w1);
            read_guest_u32(uc, 0x8067BFC0u + 8u,  &obj67_w2);
            read_guest_u32(uc, 0x8067BFC0u + 12u, &obj67_w3);
            if (objptr != 0u) {
                read_guest_u32(uc, objptr + 0u,  &objptr_w0);
                read_guest_u32(uc, objptr + 4u,  &objptr_w1);
                read_guest_u32(uc, objptr + 8u,  &objptr_w2);
                read_guest_u32(uc, objptr + 12u, &objptr_w3);
            }

            fprintf(stderr,
                    "[WINCE_BOOTMODE_EDGE] tag=%s pc=0x%08X insn=0x%08X"
                    " ra=0x%08X sp=0x%08X"
                    " a0=0x%08X a1=0x%08X a2=0x%08X a2w=%s0x%08X"
                    " v0=0x%08X v1=0x%08X"
                    " t0=0x%08X t1=0x%08X t2=0x%08X t3=0x%08X t6=0x%08X t7=0x%08X t8=0x%08X t9=0x%08X"
                    " mode=%s0x%08X gate=%s0x%08X"
                    " stk[2c]=0x%08X [30]=0x%08X [34]=0x%08X [38]=0x%08X [3c]=0x%08X [40]=0x%08X [48]=0x%08X"
                    " objptr=0x%08X objsig=0x%08X objptr_w=[%08X %08X %08X %08X]"
                    " obj66=[%08X %08X %08X %08X] obj67=[%08X %08X %08X %08X]\n",
                    edge, pc32, insn,
                    (uint32_t)ra, (uint32_t)sp,
                    (uint32_t)a0, (uint32_t)a1, (uint32_t)a2, a2_ok ? "" : "ERR:", a2_word,
                    (uint32_t)v0, (uint32_t)v1,
                    (uint32_t)t0, (uint32_t)t1, (uint32_t)t2, (uint32_t)t3,
                    (uint32_t)t6, (uint32_t)t7, (uint32_t)t8, (uint32_t)t9,
                    mode_ok ? "" : "ERR:", mode,
                    gate_ok ? "" : "ERR:", gate,
                    sp_2c, sp_30, sp_34, sp_38, sp_3c, sp_40, sp_48,
                    objptr, objsig, objptr_w0, objptr_w1, objptr_w2, objptr_w3,
                    obj66_w0, obj66_w1, obj66_w2, obj66_w3,
                    obj67_w0, obj67_w1, obj67_w2, obj67_w3);
            step_logs++;
        }
    }

    if (pc32 >= 0x80078000u && pc32 <= 0x80078260u) {
        enum { BOOTMODE_COVER_WORDS = ((0x80078260u - 0x80078000u) / 4u) + 1u };
        static uint8_t seen[BOOTMODE_COVER_WORDS];
        static uint32_t cover_logs = 0;
        uint32_t idx = (pc32 - 0x80078000u) >> 2;
        if (idx < BOOTMODE_COVER_WORDS && !seen[idx] && cover_logs < 192u) {
            uint64_t ra = 0, sp = 0, a0 = 0, a1 = 0, a2 = 0, v0 = 0, v1 = 0;
            seen[idx] = 1u;
            uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
            uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
            uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
            uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
            uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
            uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
            uc_reg_read(uc, UC_MIPS_REG_V1, &v1);
            fprintf(stderr,
                    "[WINCE_BOOTMODE_COVER] pc=0x%08X insn=0x%08X"
                    " ra=0x%08X sp=0x%08X"
                    " a0=0x%08X a1=0x%08X a2=0x%08X"
                    " v0=0x%08X v1=0x%08X\n",
                    pc32, insn,
                    (uint32_t)ra, (uint32_t)sp,
                    (uint32_t)a0, (uint32_t)a1, (uint32_t)a2,
                    (uint32_t)v0, (uint32_t)v1);
            cover_logs++;
        }
    }
}

void maybe_probe_wince_obj_dispatch(machine_t *m, uc_engine *uc,
                                           uint32_t pc32, uint32_t insn)
{
    if (!is_wince_boot_machine(m) || !m->cfg.log_wince_stall)
        return;

    const char *tag = NULL;
    switch (pc32) {
    case 0x80078BC8u: tag = "objptr_check"; break;
    case 0x80078BE8u: tag = "objptr_load"; break;
    case 0x80078BF0u: tag = "slot0_load"; break;
    case 0x80078BF4u: tag = "slot0_jalr"; break;
    case 0x80078BFCu: tag = "slot0_post_jalr"; break;
    default: break;
    }
    if (tag == NULL)
        return;

    static uint32_t logs = 0;
    if (logs >= 64u)
        return;

    uint64_t ra = 0, sp = 0, a0 = 0, v0 = 0, t6 = 0, t7 = 0, t8 = 0;
    uint32_t objptr = 0, objsig = 0;
    uint32_t obj_w0 = 0, obj_w1 = 0, obj_w2 = 0, obj_w3 = 0;
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
    uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
    uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
    uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
    uc_reg_read(uc, UC_MIPS_REG_T6, &t6);
    uc_reg_read(uc, UC_MIPS_REG_T7, &t7);
    uc_reg_read(uc, UC_MIPS_REG_T8, &t8);
    read_guest_u32(uc, 0x80660000u, &objptr);
    read_guest_u32(uc, 0x80660004u, &objsig);
    if (objptr != 0u) {
        read_guest_u32(uc, objptr + 0u, &obj_w0);
        read_guest_u32(uc, objptr + 4u, &obj_w1);
        read_guest_u32(uc, objptr + 8u, &obj_w2);
        read_guest_u32(uc, objptr + 12u, &obj_w3);
    }

    fprintf(stderr,
            "[WINCE_OBJ_DISPATCH] tag=%s pc=0x%08X insn=0x%08X"
            " ra=0x%08X sp=0x%08X a0=0x%08X v0=0x%08X"
            " t6=0x%08X t7=0x%08X t8=0x%08X"
            " objptr=0x%08X objsig=0x%08X obj_w=[%08X %08X %08X %08X]\n",
            tag, pc32, insn,
            (uint32_t)ra, (uint32_t)sp, (uint32_t)a0, (uint32_t)v0,
            (uint32_t)t6, (uint32_t)t7, (uint32_t)t8,
            objptr, objsig, obj_w0, obj_w1, obj_w2, obj_w3);
    logs++;
}

void maybe_probe_wince_bootmode_sp_flow(machine_t *m, uc_engine *uc,
                                               uint32_t pc32, uint32_t insn)
{
    if (!is_wince_boot_machine(m) || !m->cfg.log_wince_stall)
        return;

    bool in_obj_dispatch = (pc32 >= 0x80078BC0u && pc32 <= 0x80078C10u);
    bool in_bootmode_init = (pc32 >= 0x80077FE0u && pc32 <= 0x80078020u);
    bool in_probe_builder = (pc32 >= 0x80077DC0u && pc32 <= 0x80077E30u);
    if (!in_obj_dispatch && !in_bootmode_init && !in_probe_builder)
        return;

    static uint32_t flow_logs = 0;
    static uint32_t flow_seq = 0;
    static bool have_prev = false;
    static uint32_t prev_pc = 0;
    static uint32_t prev_sp = 0;
    if (flow_logs >= 320u)
        return;

    uint64_t sp = 0, ra = 0, v0 = 0, status = 0;
    uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
    uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
    uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &status);

    uint32_t sp32 = (uint32_t)sp;
    uint32_t ra32 = (uint32_t)ra;
    uint32_t v032 = (uint32_t)v0;

    int32_t delta = 0;
    if (have_prev)
        delta = (int32_t)sp32 - (int32_t)prev_sp;

    fprintf(stderr,
            "[WINCE_BOOTMODE_SP] seq=%u pc=0x%08X insn=0x%08X"
            " sp=0x%08X ra=0x%08X v0=0x%08X"
            " prev_pc=0x%08X dSP=%d"
            " STATUS=0x%08X pending_excode=%u\n",
            flow_seq, pc32, insn,
            sp32, ra32, v032,
            have_prev ? prev_pc : 0u, delta,
            (uint32_t)status, m->pending_excode);

    have_prev = true;
    prev_pc = pc32;
    prev_sp = sp32;
    flow_logs++;
    flow_seq++;
}

void maybe_probe_wince_spl_memcpy(machine_t *m, uc_engine *uc,
                                         uint32_t pc32, uint32_t insn)
{
    if (!is_wince_boot_machine(m) || !m->cfg.log_wince_stall)
        return;
    if (pc32 != 0x80F02850u || insn != 0x1006000Fu)
        return;

    static uint32_t logs = 0;
    static uint32_t cb_logs = 0;
    static uint32_t gate_logs = 0;
    static uint32_t mode_logs = 0;

    uint64_t a0 = 0, a1 = 0, a2 = 0, ra = 0;
    uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
    uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
    uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra);

    uint32_t dst = (uint32_t)a0;
    uint32_t src = (uint32_t)a1;
    uint32_t len = (uint32_t)a2;
    uint32_t src_w0 = 0, src_w1 = 0;
    bool src_ok0 = read_guest_u32(uc, src, &src_w0);
    bool src_ok1 = read_guest_u32(uc, src + 4u, &src_w1);

    bool dst_is_cb = (((dst & 0x1FFFFFFFu) >= WINCE_TRACE_CB_PA_START) &&
                      ((dst & 0x1FFFFFFFu) < WINCE_TRACE_CB_PA_END));
    bool dst_is_gate = (((dst & 0x1FFFFFFFu) >= WINCE_TRACE_GATE_PA_START) &&
                        ((dst & 0x1FFFFFFFu) < WINCE_TRACE_GATE_PA_END));
    bool dst_is_mode = (((dst & 0x1FFFFFFFu) >= 0x0001D000u) &&
                        ((dst & 0x1FFFFFFFu) < 0x0001D100u));

    bool should_log = false;
    if (dst_is_cb && cb_logs < 32u) {
        cb_logs++;
        should_log = true;
    } else if (dst_is_gate && gate_logs < 32u) {
        gate_logs++;
        should_log = true;
    } else if (dst_is_mode && mode_logs < 32u) {
        mode_logs++;
        should_log = true;
    } else if (logs < 32u) {
        should_log = true;
    }

    if (!should_log)
        return;

    fprintf(stderr,
            "[WINCE_SPL_MEMCPY] pc=0x%08X ra=0x%08X dst=0x%08X src=0x%08X len=0x%X"
            " src_w0=%s0x%08X src_w1=%s0x%08X"
            " flags=cb:%u gate:%u mode:%u\n",
            pc32, (uint32_t)ra, dst, src, len,
            src_ok0 ? "" : "ERR:", src_w0,
            src_ok1 ? "" : "ERR:", src_w1,
            dst_is_cb ? 1u : 0u,
            dst_is_gate ? 1u : 0u,
            dst_is_mode ? 1u : 0u);
    logs++;
}

/* is_wince_spl_pc is in wince_diag.h */

void log_wince_null_mmio_tail(const machine_t *m, uint32_t max_lines)
{
    if (!m)
        return;

    if (m->mmio_hist_count == 0) {
        fprintf(stderr, "[WINCE_NULL_MMIO] (empty)\n");
        return;
    }

    uint32_t emit = m->mmio_hist_count;
    if (emit > max_lines)
        emit = max_lines;

    uint32_t oldest = (m->mmio_hist_head + WINCE_MMIO_HISTORY_LEN - emit)
                    % WINCE_MMIO_HISTORY_LEN;
    for (uint32_t i = 0; i < emit; i++) {
        uint32_t idx = (oldest + i) % WINCE_MMIO_HISTORY_LEN;
        const mmio_hist_entry_t *e = &m->mmio_hist[idx];
        fprintf(stderr,
                "[WINCE_NULL_MMIO] %02u %c%u PA=0x%08X PC=0x%08X VAL=0x%08" PRIX64 "\n",
                i,
                e->is_write ? 'W' : 'R',
                e->size_bits,
                e->pa,
                e->pc,
                e->value);
    }
}

void log_wince_stall_dump(machine_t *m, uint32_t pc32)
{
    uint32_t insn = 0xFFFFFFFFu;
    uint64_t cp0_status = 0;
    uint64_t cp0_cause = (uint64_t)m->pending_cause;
    uint64_t cp0_epc = m->shadow_cp0_epc ? m->shadow_cp0_epc : m->pending_epc;
    uint64_t cp0_badv = m->shadow_cp0_badvaddr;

    /* Best-effort instruction read (try VA then PA) */
    if (uc_mem_read(m->uc, (uint64_t)(int32_t)pc32, &insn, 4) != UC_ERR_OK)
        uc_mem_read(m->uc, (uint64_t)(pc32 & UINT32_C(0x1FFFFFFF)), &insn, 4);
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
    log_wince_region_track_summary(m, "STALL");
    log_wince_div_hist_summary(m, "STALL", 24u);
    log_wince_div_call_trace_summary(m, "STALL");
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
