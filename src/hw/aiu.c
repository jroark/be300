/*
 * aiu.c - Casio audio block on the BE-300 VRC4173 latch.
 *
 * See aiu.h for the architectural notes (why this isn't a separately
 * registered GXemul device, where the registers live, and what's
 * uncertain about the bit assignments).
 *
 * This module owns:
 *   - a small cursor / change-detection state struct (aiu_state_t),
 *   - a per-tick pull that reads PCM bytes from guest RAM at the
 *     wavedev-programmed DMA pointers and queues them to the host SDL
 *     audio sink via ui_audio_queue_pcm().
 *
 * It does NOT own any MMIO storage - the VRC4173 latch (be300_devices.c)
 * already holds the register backing store and seeds it at hardware-reset
 * values.  We read those backing bytes through the existing exported
 * helper be300_vrc4173_latch_read_u32() so the latch remains the single
 * source of truth.
 */

#include <stdio.h>
#include <string.h>

#include "machine.h"
#include "memory.h"

#include "../be300.h"
#include "../ui.h"
#include "aiu.h"

/* Maximum PCM bytes pulled per tick.  At 22050 Hz / S16 mono this is a
 * little under 100 ms of audio per call, which keeps SDL's queue topped
 * up at ~30 Hz tick rate without monopolising the main loop. */
#define AIU_MAX_BYTES_PER_TICK   4096u

/* Sanity bound for the DMA ring length.  The hardware-captured ring is
 * 1532 bytes; real wavedev rings are typically up to ~16 KiB.  Anything
 * larger almost certainly means we picked up a stale/garbage base/end
 * pair the guest is mid-write on; ignore until it stabilises. */
#define AIU_MAX_RING_BYTES       0x10000u

uint32_t aiu_rate_index_to_hz(uint32_t index)
{
    /*
     * Best-guess lookup matching the VRC4173 UM Ch. 10 rate ordering.
     * The cold-reset value seeded at +0x880 is 4 (-> 22050 Hz), which
     * matches the most plausible default for a PDA wave codec.  Refine
     * this table when a live wavedev trace pins down the encoding.
     */
    static const uint32_t table[] = {
         8000, 11025, 12000, 16000,
        22050, 24000, 32000, 44100,
        48000,
    };
    if (index < sizeof(table) / sizeof(table[0]))
        return table[index];
    return 22050u;
}

static uint8_t aiu_channels_from_reg(uint32_t value)
{
    return (value == 2u) ? 2u : 1u;
}

static uint32_t aiu_bytes_per_sample_from_width(uint32_t value)
{
    /*
     * The hardware reset seed uses width=3, which is interpreted here as
     * signed 16-bit PCM. Unknown values are clamped to S16 until a wavedev
     * trace proves another encoding.
     */
    (void)value;
    return 2u;
}

void aiu_init(aiu_state_t *s, bool log_mmio)
{
    if (!s)
        return;
    memset(s, 0, sizeof(*s));
    s->log_mmio = log_mmio;
}

/* Read 32 bits LE from the latch backing store at the given full PA. */
static bool aiu_latch_read_u32(uint32_t pa, uint32_t *out)
{
    return be300_vrc4173_latch_read_u32(pa, out);
}

void aiu_tick(machine_t *m)
{
    aiu_state_t *s;
    uint32_t ctrl = 0;
    uint32_t dma_en = 0;
    uint32_t base = 0;
    uint32_t end = 0;
    uint32_t rate_idx = 0;
    uint32_t channels = 0;
    uint32_t width = 0;
    uint8_t pcm_channels;
    uint32_t sample_bytes;
    uint32_t frame_bytes;
    uint32_t ring;
    uint32_t budget;

    if (!m || !m->cfg.enable_audio)
        return;
    s = &m->aiu;
    s->tick_count++;

    /* Read the wavedev-programmed control / DMA registers from the
     * VRC4173 latch backing store. */
    if (!aiu_latch_read_u32(0x0A000000u + AIU_REG_CTRL, &ctrl))
        return;
    if (!aiu_latch_read_u32(0x0A000000u + AIU_REG_DMA_ENABLE, &dma_en))
        return;
    if (!aiu_latch_read_u32(0x0A000000u + AIU_REG_PLAY_BASE_PA, &base))
        return;
    if (!aiu_latch_read_u32(0x0A000000u + AIU_REG_PLAY_END_PA, &end))
        return;
    aiu_latch_read_u32(0x0A000000u + AIU_REG_RATE_INDEX, &rate_idx);
    aiu_latch_read_u32(0x0A000000u + AIU_REG_CHANNELS,   &channels);
    aiu_latch_read_u32(0x0A000000u + AIU_REG_WIDTH,      &width);
    pcm_channels = aiu_channels_from_reg(channels);
    sample_bytes = aiu_bytes_per_sample_from_width(width);
    frame_bytes = sample_bytes * (uint32_t)pcm_channels;

    /*
     * Gate on (a) global codec enable bit, (b) DMA-channel enable bit.
     * Both bit assignments are uncertain (see aiu.h) - the cold-reset
     * seed already has 0x3C0=0xD007 (LSB nibble = 7) so without further
     * filtering this would fire from boot.  Suppress that by also
     * requiring that the guest has changed the DMA base/end pair away
     * from the hardware-capture seed values (0x4D000 / 0x4D5FC).
     */
    if (!(ctrl & 1u) || !(dma_en & 1u))
        return;
    if (!s->started) {
        if (base == 0x0004D000u && end == 0x0004D5FCu) {
            /* Still at cold-reset seed; wavedev hasn't programmed real
             * pointers yet.  Don't pull stale RAM. */
            return;
        }
        if (end <= base || (end - base) > AIU_MAX_RING_BYTES) {
            /* In-flight base/end half-update; wait for it to settle. */
            return;
        }
        s->started = true;
        s->prev_base = base;
        s->prev_end = end;
        s->play_pos = 0;
        if (s->log_mmio) {
            fprintf(stderr,
                "[AIU] play start: base=0x%08x end=0x%08x len=%u "
                "rate_idx=%u channels=%u width=%u (ctrl=0x%08x dma_en=0x%08x)\n",
                base, end, end - base, rate_idx, channels, width,
                ctrl, dma_en);
        }
    } else if (base != s->prev_base || end != s->prev_end) {
        if (end <= base || (end - base) > AIU_MAX_RING_BYTES)
            return;
        s->prev_base = base;
        s->prev_end = end;
        s->play_pos = 0;
        if (s->log_mmio) {
            fprintf(stderr,
                "[AIU] ring change: base=0x%08x end=0x%08x len=%u\n",
                base, end, end - base);
        }
    }

    ring = end - base;
    if (ring == 0)
        return;

    /*
     * Pull up to AIU_MAX_BYTES_PER_TICK from the active ring.  We treat
     * the ring as raw little-endian S16 mono/stereo PCM - uncertain
     * (see aiu.h), but the most likely shape for a WinCE waveOut driver.
     * If the trace shows a different format, this is the one place to fix.
     */
    budget = AIU_MAX_BYTES_PER_TICK;
    while (budget > 0u) {
        uint32_t remain;
        uint32_t chunk;
        unsigned char *src;

        remain = ring - s->play_pos;
        chunk = (remain < budget) ? remain : budget;
        if (chunk == 0u)
            break;
        /* Whole-frame alignment so the int16_t cast and stereo indexing are
         * well-defined. */
        chunk -= chunk % frame_bytes;
        if (chunk == 0u)
            break;

        src = memory_paddr_to_hostaddr(m->gxe_machine->memory,
            base + s->play_pos, MEM_READ);
        if (!src) {
            if (s->log_mmio) {
                fprintf(stderr,
                    "[AIU] paddr->host failed at PA 0x%08x; pausing pull\n",
                    base + s->play_pos);
            }
            break;
        }

        if (s->log_mmio && s->pulled_bytes < 64u) {
            /* First few bytes per stream so wrong sample format is
             * immediately visible in the log. */
            uint32_t preview = (chunk < 8u) ? chunk : 8u;
            fprintf(stderr,
                "[AIU] pull pa=0x%08x len=%u first=", base + s->play_pos,
                chunk);
            for (uint32_t i = 0; i < preview; i++)
                fprintf(stderr, "%s%02x", i ? " " : "", src[i]);
            fprintf(stderr, "\n");
        }

        ui_audio_queue_pcm((const int16_t *)src, chunk / frame_bytes,
            aiu_rate_index_to_hz(rate_idx), pcm_channels);

        s->pulled_bytes += chunk;
        s->play_pos += chunk;
        budget -= chunk;

        if (s->play_pos >= ring) {
            s->play_pos = 0;
            /* No status-bit poke here - see aiu.h note: the latch backing
             * store doesn't model W1C semantics, and many wavedev drivers
             * poll the cursor directly.  If trace shows the driver
             * waiting on +0x3F4 bit 0, add the W1C handler in
             * be300_devices.c at that point. */
        }
    }
}
