#pragma once

/*
 * AIU - Casio audio block exposed through the VRC4173 latch.
 *
 * Hardware path: the BE-300 does NOT use the VRC4173 UM Ch. 10 AIU at
 * VRC4173 base+0x0E0..+0x0FC (Pass 33 confirmed zero accesses there during
 * cold boot). The active audio path is a Casio-custom block at the
 * following VRC4173 latch offsets, captured at hardware-reset values in
 * src/be300_devices.c:2778-2817 (`audio_regs[]`):
 *
 *   +0x390..+0x3F4   control / mixer / status
 *   +0x880..+0x8CC   sample-rate, channels, width, DMA pointers
 *   +0x1114..+0x111C companion power/wake enables
 *
 * The wavedev.dll driver maps these via user VA 0x001B0000 (per the
 * comment at src/be300_devices.c:2774). On cold boot wavedev does NOT
 * load - it loads lazily when something user-mode plays a wav. Therefore
 * this device has zero boot-path regression risk and stays default-off
 * behind --audio until proven stable.
 *
 * Coexistence: GXemul memory_device_register() refuses overlapping
 * regions (gxemul/src/core/memory.c:368), so the AIU is NOT a separately
 * registered device. The latch's existing access function continues to
 * own the backing store; aiu_tick() polls the latch each main-loop
 * iteration, pulls PCM frames from guest RAM via the DMA pointers, and
 * queues them to the host SDL sink.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct be300_state;
typedef struct be300_state machine_t;

/* AIU register offsets within the VRC4173 latch (PA 0x0A000000 base). */
#define AIU_REG_CTRL           0x03C0u  /* bit 0 = global enable (uncertain)   */
#define AIU_REG_STATUS_W1C     0x03F4u  /* IRQ status, write-1-clear (uncertain)*/
#define AIU_REG_RATE_INDEX     0x0880u  /* sample-rate lookup index (uncertain) */
#define AIU_REG_CHANNELS       0x0884u  /* 1 = mono (uncertain)                 */
#define AIU_REG_WIDTH          0x0888u  /* 3 = 16-bit (uncertain)               */
#define AIU_REG_DMA_ENABLE     0x088Cu  /* bit 0 = DMA channel enable           */
#define AIU_REG_PLAY_BASE_PA   0x08B0u  /* playback ring base PA                */
#define AIU_REG_PLAY_END_PA    0x08B4u  /* playback ring end PA (exclusive)     */
#define AIU_REG_CAPTURE_BASE   0x08B8u  /* capture ring base PA (out of scope)  */
#define AIU_REG_CAPTURE_END    0x08BCu  /* capture ring end PA  (out of scope)  */

typedef struct aiu_state {
    bool     log_mmio;
    bool     started;            /* true once we've observed a non-seed DMA base */
    uint32_t prev_base;          /* last observed playback base (for change detect) */
    uint32_t prev_end;
    uint32_t play_pos;           /* current byte offset within [base, end) */
    uint64_t pulled_bytes;       /* total PCM bytes pulled (logging) */
    uint64_t tick_count;
} aiu_state_t;

void aiu_init(aiu_state_t *s, bool log_mmio);
void aiu_tick(machine_t *m);

/* Convert the 0x880 rate index to Hz. Returns 22050 for unknown indices.
 * Lookup table is best-guess (VRC4173 UM Ch. 10 ordering); refine when a
 * runtime wavedev trace lands. */
uint32_t aiu_rate_index_to_hz(uint32_t index);
