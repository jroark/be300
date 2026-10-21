/*
 * aiu.c - Casio wave-output DMA block on the BE-300 Kj companion.
 *
 * See aiu.h and docs/BE300_AUDIO.md for the register map.  This module
 * owns the DMA cursor and the drained-buffer status; the companion latch
 * (be300_devices.c) owns register storage and the GIRQ0 interrupt line.
 *
 * The DMA cursor is paced by host monotonic time, the same clock the
 * main-loop throttle uses, so the guest sees buffer-drained interrupts at
 * the real playback rate and the host SDL queue stays level.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "machine.h"
#include "memory.h"

#include "../be300.h"
#include "../ui.h"
#include "aiu.h"

#define AIU_LATCH_BASE      0x0A000000u
#define AIU_FRAME_STAGE     512u

/* The main loop runs far more often than the sample rate; batch pulls to
 * roughly 6 ms at 11025 Hz unless a buffer boundary is due sooner. */
#define AIU_MIN_BATCH       64u

/* A ring buffer longer than this means the guest is mid-update. */
#define AIU_MAX_BUF_BYTES   0x10000u

/* Never consume more than this much wall time in one tick; a stalled host
 * (debugger, SIGSTOP) resynchronizes instead of bursting interrupts. */
#define AIU_MAX_CATCHUP_NS  100000000ULL

/*
 * Wave DMA is paced on host time on purpose: its buffers feed the host's
 * audio output, which plays in real time. Pacing it on emulated time
 * would play audio too fast or too slow whenever emulation runs faster
 * or slower than real time (--speed), at the cost of guest-visible DMA
 * completion following the host clock.
 */
static uint64_t aiu_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint32_t aiu_reg(uint32_t off)
{
    uint32_t v = 0;

    be300_vrc4173_latch_read_u32(AIU_LATCH_BASE + off, &v);
    return v;
}

/*
 * wavedev 0x019111DC picks the code written to 0x880 bits 6:4 and a codec
 * double-rate bit from the requested sample rate:
 *   <10001 Hz -> 0x40       10001..15000 -> 0x10    15001..19000 -> 0x30
 *   19001..26000 -> 0x00    26001..40000 -> 0x30+x2  >40000 -> 0x00+x2
 * The x2 bit is bit 7 of the 0xDxxx command wavedev writes to 0x3C0.
 */
static uint32_t aiu_rate_hz(const aiu_state_t *s)
{
    uint32_t base;

    switch (aiu_reg(AIU_REG_CTRL) & 0x70u) {
    case 0x40: base = 8000;  break;
    case 0x10: base = 11025; break;
    case 0x30: base = 16000; break;
    default:   base = 22050; break;
    }
    return s->codec_double ? base * 2u : base;
}

static uint8_t aiu_channels(void)
{
    /* wavedev 0x01911C0C clears 0x888 bit 1 for its stereo formats. */
    return (aiu_reg(AIU_REG_FORMAT) & 0x2u) ? 1u : 2u;
}

static void aiu_load_buffer(aiu_state_t *s, unsigned idx)
{
    uint32_t start = aiu_reg(idx ? AIU_REG_BUF1_START : AIU_REG_BUF0_START);
    uint32_t end = aiu_reg(idx ? AIU_REG_BUF1_END : AIU_REG_BUF0_END);

    /* wavedev programs kseg1 addresses (0xA004A000); the bus uses the PA. */
    start &= 0x1FFFFFFFu;
    end &= 0x1FFFFFFFu;
    s->buf_start[idx] = start;
    s->buf_len[idx] = (end >= start && end - start < AIU_MAX_BUF_BYTES)
        ? end - start + 1u : 0u;
}

void aiu_init(aiu_state_t *s, bool log_mmio)
{
    if (!s)
        return;
    memset(s, 0, sizeof(*s));
    s->log_mmio = log_mmio;
}

uint32_t aiu_read_status(const aiu_state_t *s)
{
    return s ? s->status : 0u;
}

bool aiu_irq_pending(const aiu_state_t *s)
{
    return s && (s->status & 0x3u) != 0;
}

bool aiu_handles_write(uint32_t off, unsigned len)
{
    uint32_t end = off + len;

    return (off < AIU_REG_ACK1 + 4u && end > AIU_REG_DMA_ENABLE) ||
           (off < AIU_REG_CODEC_CMD + 4u && end > AIU_REG_CODEC_CMD);
}

static void aiu_start(machine_t *m)
{
    aiu_state_t *s = &m->aiu;

    s->running = true;
    s->cur_buf = 0;
    s->pos = 0;
    s->rate_hz = aiu_rate_hz(s);
    s->channels = aiu_channels();
    aiu_load_buffer(s, 0);
    aiu_load_buffer(s, 1);
    s->start_ns = aiu_now_ns();
    s->frames_done = 0;
    if (s->log_mmio)
        fprintf(stderr,
            "[AIU] start rate=%u ch=%u buf0=0x%05x+%u buf1=0x%05x+%u\n",
            s->rate_hz, s->channels, s->buf_start[0], s->buf_len[0],
            s->buf_start[1], s->buf_len[1]);
}

static void aiu_stop(machine_t *m)
{
    aiu_state_t *s = &m->aiu;

    s->running = false;
    if (s->log_mmio)
        fprintf(stderr, "[AIU] stop after %llu irqs\n",
            (unsigned long long)s->irq_count);
}

void aiu_note_write(machine_t *m, uint32_t off, unsigned len, uint32_t val)
{
    aiu_state_t *s;
    bool want_run;

    if (!m)
        return;
    s = &m->aiu;

    if (off == AIU_REG_CODEC_CMD && (val & 0xF000u) == 0xD000u)
        s->codec_double = (val & 0x80u) != 0;

    /* wavedev's IST decode (0x0191274C) writes 0x11 to both ack registers
     * after reading 0x8A8. */
    if (off == AIU_REG_ACK0 && (val & 0x1u))
        s->status &= ~0x1u;
    if (off == AIU_REG_ACK1 && (val & 0x1u))
        s->status &= ~0x2u;

    /* Start: 0x898=1 then 0x8A0=1 at the end of wavedev's start sequence
     * (0x01911374).  Stop: 0x8A0=0 / 0x898&=~1 (0x01911CF0), or the OAL
     * power-down at 0x800A8FDC which writes 0x898=0. */
    (void)len;
    want_run = (aiu_reg(AIU_REG_DMA_ENABLE) & 0x1u) &&
               (aiu_reg(AIU_REG_ARM) & 0x1u);
    if (want_run && !s->running)
        aiu_start(m);
    else if (!want_run && s->running)
        aiu_stop(m);
}

/* Copy `frames` frames from guest RAM to the host sink. */
static void aiu_emit(machine_t *m, uint32_t pa, uint32_t frames)
{
    aiu_state_t *s = &m->aiu;
    uint32_t frame_bytes = 2u * s->channels;
    int16_t stage[AIU_FRAME_STAGE * 2u];

    if (!m->cfg.enable_audio)
        return;

    while (frames > 0) {
        uint32_t n = frames < AIU_FRAME_STAGE ? frames : AIU_FRAME_STAGE;
        uint32_t bytes = n * frame_bytes;
        uint32_t page_left = 0x1000u - (pa & 0xFFFu);
        unsigned char *src;

        if (bytes > page_left) {
            n = page_left / frame_bytes;
            if (n == 0) {
                /* Frame straddles a page; frames are 2- or 4-byte aligned
                 * in practice, so just skip it. */
                pa += frame_bytes;
                frames--;
                continue;
            }
            bytes = n * frame_bytes;
        }
        src = memory_paddr_to_hostaddr(m->gxe_machine->memory, pa,
            MEM_READ);
        if (!src)
            return;
        memcpy(stage, src, bytes);
        ui_audio_queue_pcm(stage, n, s->rate_hz, s->channels);
        pa += bytes;
        frames -= n;
    }
}

void aiu_tick(machine_t *m)
{
    aiu_state_t *s;
    uint64_t now, due;
    uint32_t frame_bytes;
    bool raised = false;

    if (!m)
        return;
    s = &m->aiu;
    if (!s->running || s->rate_hz == 0)
        return;

    now = aiu_now_ns();
    if (now - s->start_ns > AIU_MAX_CATCHUP_NS + (s->frames_done *
            1000000000ULL) / s->rate_hz) {
        /* Host stalled; restart the cursor clock at the current frame. */
        s->start_ns = now - (s->frames_done * 1000000000ULL) / s->rate_hz;
    }
    due = ((now - s->start_ns) * (uint64_t)s->rate_hz) / 1000000000ULL;
    if (due <= s->frames_done)
        return;
    due -= s->frames_done;

    frame_bytes = 2u * s->channels;
    if (due < AIU_MIN_BATCH && s->buf_len[s->cur_buf] >= frame_bytes &&
        due < (s->buf_len[s->cur_buf] - s->pos) / frame_bytes)
        return;
    while (due > 0) {
        unsigned idx = s->cur_buf;
        uint32_t left, n;

        if (s->buf_len[idx] < frame_bytes) {
            /* Unprogrammed buffer: treat it as already drained. */
            left = 0;
        } else {
            left = (s->buf_len[idx] - s->pos) / frame_bytes;
        }
        n = (uint32_t)(due < left ? due : left);
        if (n > 0) {
            aiu_emit(m, s->buf_start[idx] + s->pos, n);
            s->pos += n * frame_bytes;
            s->frames_done += n;
            due -= n;
        }
        if (n == left) {
            s->status |= 1u << idx;
            s->irq_count++;
            raised = true;
            s->cur_buf ^= 1u;
            s->pos = 0;
            aiu_load_buffer(s, s->cur_buf);
            if (left == 0) {
                /* Avoid spinning on two empty buffers. */
                s->frames_done += due;
                break;
            }
        }
    }

    if (raised)
        be300_vrc4173_update_audio_irq();
}
