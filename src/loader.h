#pragma once
#include <stdint.h>

typedef struct machine_s machine_t;

/*
 * Load a flat binary ROM image from disk into the Unicorn address space.
 * The image is written starting at PA_RESET_VECTOR (0x1FC00000) which
 * sits within the ROM region mapped at PA_ROM_BASE.
 *
 * Returns 0 on success, -1 on error.
 */
int loader_load_rom(machine_t *m, const char *path);

/*
 * Optionally preload a raw RAM image (e.g. a saved state) at PA_SDRAM_BASE.
 * Returns 0 on success, -1 on error.
 */
int loader_load_ram(machine_t *m, const char *path);

/*
 * Load an ELF32 LE MIPS kernel directly into SDRAM.
 *
 * Each PT_LOAD segment is written to its physical address
 * (p_paddr & 0x1FFFFFFF).  BSS (memsz > filesz) is zeroed.
 * The ELF entry point virtual address is written to *entry_va_out.
 *
 * Returns 0 on success, -1 on error.
 */
int loader_load_elf(machine_t *m, const char *path,
                    uint32_t *entry_va_out,
                    uint32_t *jiffies_pa_out);
