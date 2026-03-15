/*
 * tlb_shadow.c — Shadow TLB recording & VA->PA translation
 *
 * Maintains a shadow copy of the MIPS TLB by observing MTC0 and TLBWI/TLBWR
 * instructions. Provides VA->PA lookup used by diagnostic probes and the
 * kuseg page mapping workaround (tlb_map_kuseg_page).
 *
 * Extracted from machine.c.
 */

#include "tlb_shadow.h"
#include "machine.h"
#include "mem_alias.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* cp0_shadow_write                                                    */
/* ------------------------------------------------------------------ */

void cp0_shadow_write(machine_t *m, uint32_t rd, uint32_t sel, uint64_t val)
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

/* ------------------------------------------------------------------ */
/* shadow_tlb_record_write                                             */
/* ------------------------------------------------------------------ */

void shadow_tlb_record_write(machine_t *m, uint32_t tlb_insn, uint32_t pc)
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

/* ------------------------------------------------------------------ */
/* shadow_tlb_lookup                                                   */
/* ------------------------------------------------------------------ */

tlb_lookup_result_t shadow_tlb_lookup(machine_t *m, uint32_t va)
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

/* ------------------------------------------------------------------ */
/* trace_user_handoff_entry_probe                                      */
/* ------------------------------------------------------------------ */

bool trace_user_handoff_entry_probe(machine_t *m, const char *tag,
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

/* ------------------------------------------------------------------ */
/* trace_user_handoff_fault_once                                       */
/* ------------------------------------------------------------------ */

void trace_user_handoff_fault_once(machine_t *m, uint32_t fault_pc,
                                   uint32_t fault_va, uint64_t raw_badv)
{
    (void)trace_user_handoff_entry_probe(m, "FAULT", fault_pc, fault_va,
                                         raw_badv, true);
}

/* ------------------------------------------------------------------ */
/* shadow_tlb_populate                                                 */
/* ------------------------------------------------------------------ */

bool shadow_tlb_populate(machine_t *m, uint32_t va, bool include_pair,
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
