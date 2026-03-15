#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <unicorn/unicorn.h>

/* Forward declaration */
typedef struct machine_s machine_t;

/* Memory aliasing & kseg mapping functions.
 * Extracted from machine.c — manages kseg0/kseg1/kuseg memory aliases
 * and PA coherence for Unicorn's flat memory model. */

/* Check whether addr falls in any SDRAM alias (PA, kseg0, kseg1) and
 * return the offset within SDRAM if so. */
bool sdram_alias_pa_offset(const machine_t *m, uint64_t addr, uint64_t *off_out);

/* Write/read a u32 to all SDRAM alias regions (PA, kseg0, kseg1). */
void write_pa_u32_all_aliases(machine_t *m, uint32_t pa, uint32_t val);
bool read_pa_u32_all_aliases(machine_t *m, uint32_t pa, uint32_t *out);

/* Verify alias coherence after setup (diagnostic). */
void alias_coherence_probe(machine_t *m);

/* Map a 1MB block at map_base backed by SDRAM at pa_base.
 * Uses shared backing pointer if available, otherwise copies. */
bool map_kseg_alias_block(machine_t *m, uint64_t map_base, uint64_t pa_base);

/* Map a kseg0/kseg1 mirror block (both sign-extended and zero-extended
 * addresses for MIPS64 compatibility). */
bool map_kseg_mirror_block(machine_t *m, uint64_t va_block);

/* Map a kuseg page backed by SDRAM content (workaround for Unicorn
 * softmmu TLB issues). */
void tlb_map_kuseg_page(machine_t *m, uint64_t kuseg_va, uint64_t pa,
                        uint64_t page_bytes);
