/*
 *  src/hw/nand_internal.h — declarations shared between nand.c and the
 *  files split out of it. Not included by anything outside src/hw/nand*.c.
 */
#pragma once

#include <stdint.h>

#include "nand.h"

/* Synthesised OOB metadata for a logical page (data-only restore images
 * carry no physical OOB). Used by the SPL stream path in nand.c and by
 * the restore-engine page filler in nand_restore.c.
 *
 * Background: the restore images are 16,449,536-byte data-only dumps
 * (ce/restore_images/RESTORE_IMAGES.md), so we reconstruct the per-block
 * tags NANDWRITER would have stored in spare. See the in-source comment
 * for the 0x55AA tag layout. */
uint8_t nand_stream_oob_byte(nand_state_t *s,
    uint32_t page, uint32_t oob_idx);

/* NANDWRITER restore-engine reset/seed helper invoked from nand_init(). */
void    nand_restore_seed_regs(nand_state_t *s);
