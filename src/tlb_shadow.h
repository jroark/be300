#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <unicorn/unicorn.h>

/* Forward declaration */
typedef struct machine_s machine_t;

/* TLB shadow recording, VA->PA translation, and user handoff tracing.
 * Extracted from machine.c. */

/* Result of a shadow TLB lookup. */
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

/* Record a CP0 shadow register write (MTC0 instruction). */
void cp0_shadow_write(machine_t *m, uint32_t rd, uint32_t sel, uint64_t val);

/* Record a TLB write (TLBWI/TLBWR) in the shadow table. */
void shadow_tlb_record_write(machine_t *m, uint32_t tlb_insn, uint32_t pc);

/* Look up a VA in the shadow TLB table. */
tlb_lookup_result_t shadow_tlb_lookup(machine_t *m, uint32_t va);

/* Populate Unicorn's flat memory region from shadow TLB translation. */
bool shadow_tlb_populate(machine_t *m, uint32_t va, bool include_pair,
                         const char *tag, uint32_t pc_hint);

/* Trace user handoff entry probe (diagnostic). */
bool trace_user_handoff_entry_probe(machine_t *m, const char *tag,
                                    uint32_t pc, uint32_t va,
                                    uint64_t raw_badv, bool first_fault_only);

/* Trace user handoff fault (one-shot diagnostic). */
void trace_user_handoff_fault_once(machine_t *m, uint32_t fault_pc,
                                   uint32_t fault_va, uint64_t raw_badv);
