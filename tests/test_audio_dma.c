/* Host mute must not change guest DMA advancement, buffer status or IRQs. */
#include "be300.h"
#include "machine.h"
#include "memory.h"
#include <stdlib.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static uint32_t regs[0x1000 / 4];
static uint64_t now_ns;
static int16_t ram[0x2000 / 2], captured[256];
static size_t emitted;
static unsigned irq_updates, stream_resets;

int audio_test_clock_gettime(clockid_t clock, struct timespec *ts)
{
    (void)clock;
    ts->tv_sec = (time_t)(now_ns / 1000000000u);
    ts->tv_nsec = (long)(now_ns % 1000000000u);
    return 0;
}
bool be300_vrc4173_latch_read_u32(uint32_t pa, uint32_t *out)
{
    CHECK(pa >= 0x0a000000 && pa < 0x0a001000);
    *out = regs[(pa - 0x0a000000) / 4]; return true;
}
void be300_vrc4173_update_audio_irq(void) { irq_updates++; }
void ui_audio_reset_stream(void) { stream_resets++; }
void ui_audio_queue_pcm(const int16_t *pcm, size_t frames, uint32_t rate, uint8_t channels)
{
    CHECK(rate == 44100 && channels == 2 && emitted + frames <= 128);
    memcpy(captured + emitted * 2, pcm, frames * 4); emitted += frames;
}
unsigned char *memory_paddr_to_hostaddr(struct memory *memory, uint64_t pa, int write)
{
    (void)memory; CHECK(write == MEM_READ && pa >= 0x1000 && pa < 0x3000);
    return (unsigned char *)ram + pa - 0x1000;
}
static aiu_state_t run(bool audible)
{
    machine_t m = {0}; struct machine gx = {0};
    m.gxe_machine = &gx; m.cfg.enable_audio = audible;
    memset(regs, 0, sizeof regs); now_ns = emitted = irq_updates = stream_resets = 0;
    aiu_init(&m.aiu, false);
    regs[AIU_REG_CODEC_CMD / 4] = 0xd087;
    aiu_note_write(&m, AIU_REG_CODEC_CMD, 4, 0xd087);
    regs[AIU_REG_BUF0_START / 4] = 0x1000;
    regs[AIU_REG_BUF0_END / 4] = 0x10ff;
    regs[AIU_REG_BUF1_START / 4] = 0x2000;
    regs[AIU_REG_BUF1_END / 4] = 0x20ff;
    regs[AIU_REG_DMA_ENABLE / 4] = regs[AIU_REG_ARM / 4] = 1;
    aiu_note_write(&m, AIU_REG_ARM, 4, 1);
    CHECK(m.aiu.running && stream_resets == 1);
    now_ns = (64u * 1000000000ULL + 44100u - 1) / 44100u;
    aiu_tick(&m);
    CHECK(m.aiu.frames_done == 64 && m.aiu.cur_buf == 1 && m.aiu.pos == 0);
    CHECK(aiu_read_status(&m.aiu) == 1 && aiu_irq_pending(&m.aiu) && irq_updates == 1);
    CHECK(emitted == (audible ? 64u : 0u));
    if (audible) CHECK(!memcmp(captured, ram, 256));
    aiu_note_write(&m, AIU_REG_ACK0, 4, 1);
    CHECK(aiu_read_status(&m.aiu) == 0);
    now_ns = (128u * 1000000000ULL + 44100u - 1) / 44100u;
    aiu_tick(&m);
    CHECK(m.aiu.frames_done == 128 && m.aiu.cur_buf == 0 && m.aiu.pos == 0);
    CHECK(aiu_read_status(&m.aiu) == 2 && irq_updates == 2);
    if (audible) CHECK(emitted == 128 && !memcmp(captured + 128, ram + 0x800, 256));
    regs[AIU_REG_ARM / 4] = 0;
    aiu_note_write(&m, AIU_REG_ARM, 4, 0);
    CHECK(!m.aiu.running);
    return m.aiu;
}
int main(void)
{
    for (size_t i = 0; i < sizeof ram / sizeof ram[0]; i++) ram[i] = (int16_t)(i * 3 - 2000);
    aiu_state_t audible = run(true), muted = run(false);
    CHECK(audible.frames_done == muted.frames_done && audible.status == muted.status &&
        audible.irq_count == muted.irq_count && audible.pos == muted.pos &&
        audible.cur_buf == muted.cur_buf && audible.running == muted.running);
    puts("audio DMA mute independence: PASS"); return 0;
}
