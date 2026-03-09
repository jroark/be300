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

/*
 * Load a WinCE NAND image containing a B000FF-format SPL bootloader.
 *
 * Parses the B000FF header at NAND offset 0x4000, loads BIN records
 * into Unicorn RAM at their specified addresses (kseg0 → PA), and
 * returns the entry point VA in *entry_va_out.
 *
 * Also stores the raw NAND data in m->nand_data / m->nand_size for
 * later NAND controller emulation.
 *
 * Returns 0 on success, -1 on error.
 */
int loader_load_nand(machine_t *m, const char *path,
                     uint32_t *entry_va_out);
