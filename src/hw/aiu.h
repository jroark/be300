#pragma once

/*
 * AIU - Casio wave-output DMA block in the Kj companion window.
 *
 * The BE-300 does NOT use the VRC4173 UM Ch. 10 AIU (Pass 33 saw zero
 * accesses there).  wavedev.dll (XIP module, vbase 0x01910000) drives a
 * Casio block at the companion offsets below; docs/BE300_AUDIO.md has the
 * full register map with the wavedev/NK PCs each meaning was taken from.
 *
 * Playback is DMA of signed 16-bit little-endian PCM (wavedev converts 8-bit
 * and applies volume itself, 0x019114EC) from a current buffer and a next
 * one:
 *   current = [0x8B0, 0x8B4] (end inclusive)
 *   next    = [0x8B8, 0x8BC]
 * wavedev loads both pairs when it starts (0x019114A0-0x019114E8: its
 * buffers at PA 0x4A000 and 0x4D000) and from then on writes only the next
 * pair, whichever of its two buffers it has refilled (0x01912174-0x0191219C).
 * So when the current buffer drains the engine goes on with the next pair,
 * which becomes the current one: a real BE-300 reads the same addresses in
 * both pairs (hw_dump_vrc4173.txt, 0x0A0008B0: 0004D000 0004D5FC 0004D000
 * 0004D5FC), and Glaze on hardware found the status bits are not buffer
 * numbers (glaze-os kernel/drivers/audio.c). Status 0x8A8 bit 0 sets and
 * the source raises companion GIRQ0 bit 8 (AA000004), which the OAL handler
 * 0x800A8DCC turns into SYSINTR 0x14 for wavedev's IST. What sets status
 * bit 1 is not known (wavedev treats it as a different event, 0x01912764);
 * playback never sets it here.
 *
 * This used to alternate strictly between the two pairs with one status
 * bit each. That gave the same result for wavedev, which alternates two
 * fixed buffers, and the wrong one for a guest that queues its own.
 *
 * The latch in be300_devices.c keeps the register backing store; it calls
 * aiu_note_write() after storing a write in the audio range and routes
 * status reads through aiu_read_status().
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct be300_state;
typedef struct be300_state machine_t;

/* Register offsets within the companion latch (PA 0x0A000000 base). */
#define AIU_REG_CODEC_CMD      0x03C0u  /* codec serial command               */
#define AIU_REG_CODEC_READY    0x03C4u  /* non-zero = codec ready             */
#define AIU_REG_CTRL           0x0880u  /* bits 6:4 rate code, bit 3 enable   */
#define AIU_REG_FORMAT         0x0888u  /* bit 1 = mono                       */
#define AIU_REG_DMA_ENABLE     0x0898u  /* bit 0 = DMA run                    */
#define AIU_REG_ARM            0x08A0u  /* bit 0 = output armed               */
#define AIU_REG_STATUS         0x08A8u  /* bit 0 = current buffer drained     */
#define AIU_REG_BUF0_START     0x08B0u
#define AIU_REG_BUF0_END       0x08B4u  /* inclusive                          */
#define AIU_REG_BUF1_START     0x08B8u
#define AIU_REG_BUF1_END       0x08BCu  /* inclusive                          */
#define AIU_REG_ACK0           0x08C8u  /* bit 0 written = clear status bit 0 */
#define AIU_REG_ACK1           0x08CCu  /* bit 0 written = clear status bit 1 */

/* Companion level-1 source bit (AA000004) and its enable (AA000030). */
#define AIU_GIRQ0_SOURCE       0x00000100u
#define AIU_SOURCE_EN_OFF      0x0030u
#define AIU_SOURCE_EN_BIT      0x00000008u

typedef struct aiu_state {
    bool     log_mmio;
    bool     running;
    uint8_t  cur_buf;            /* unused: the current pair is index 0 */
    uint32_t buf_start[2];       /* [0] current, [1] next as last latched (PA) */
    uint32_t buf_len[2];
    uint32_t pos;                /* byte offset within cur_buf */
    uint32_t status;             /* 0x8A8 bits 0..1 */
    bool     codec_double;       /* bit 7 of the last 0xDxxx codec command */
    uint32_t rate_hz;
    uint8_t  channels;
    uint64_t start_ns;           /* emulated-time origin of the frame cursor */
    uint64_t frames_done;        /* frames consumed since start_ns */
    uint64_t irq_count;
} aiu_state_t;

void aiu_init(aiu_state_t *s, bool log_mmio);
void aiu_tick(machine_t *m);
void aiu_note_write(machine_t *m, uint32_t off, unsigned len, uint32_t val);
uint32_t aiu_read_status(const aiu_state_t *s);
bool aiu_irq_pending(const aiu_state_t *s);
bool aiu_handles_write(uint32_t off, unsigned len);
