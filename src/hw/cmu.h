#pragma once
#include <stdint.h>

/*
 * CMU — Clock Mask Unit (VR4131 hardware manual §10)
 *
 * Single 16-bit register CMUCLKMSK at offset 0x00 (PA 0x0F000060).
 * Controls which peripheral clocks are gated on/off.
 * Stub: accept writes, return last written value.
 */

#define CMU_CMUCLKMSK 0x00u

typedef struct {
    uint16_t clkmsk;
} cmu_state_t;

void     cmu_init (cmu_state_t *s);
uint32_t cmu_read (cmu_state_t *s, uint32_t offset, unsigned size);
void     cmu_write(cmu_state_t *s, uint32_t offset, unsigned size, uint32_t val);
