#pragma once
#include <stdint.h>
#include "be300.h"

/*
 * Load a flat binary ROM image at PA_RESET_VECTOR (0x1FC00000).
 */
int loader_load_rom(machine_t *m, const char *path);

/*
 * Preload a raw RAM image at PA_SDRAM_BASE.
 */
int loader_load_ram(machine_t *m, const char *path);

/*
 * Load an ELF32 LE MIPS kernel into SDRAM.
 */
int loader_load_elf(machine_t *m, const char *path,
                    uint32_t *entry_va_out,
                    uint32_t *jiffies_pa_out);

/*
 * Load a WinCE NAND image (B000FF-format SPL).
 */
int loader_load_nand(machine_t *m, const char *path,
                     uint32_t *entry_va_out);
