/* Asset-free audio oracles: channel isolation, timebase and streaming boundaries. */
#include "audio_pcm.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
struct capture { int16_t *pcm; size_t frames, capacity; };
static void capture(void *context, const int16_t *pcm, size_t frames)
{
    struct capture *c = context;
    CHECK(c->frames + frames <= c->capacity);
    memcpy(c->pcm + c->frames * 2, pcm, frames * AUDIO_PCM_FRAME_BYTES);
    c->frames += frames;
}
static void isolation(void)
{
    int16_t input[] = {32767, 0, 1234, 0, -32768, 0, 0, 0};
    int16_t result[32];
    struct capture c = {result, 0, 16};
    struct audio_pcm_resampler s = {0};
    audio_pcm_convert(&s, input, 4, 44100, 2, capture, &c);
    CHECK(c.frames == 4 && !memcmp(input, result, sizeof input));
    audio_pcm_reset(&s); c.frames = 0;
    for (int i = 0; i < 4; i++) { input[i * 2 + 1] = input[i * 2]; input[i * 2] = 0; }
    audio_pcm_convert(&s, input, 4, 44100, 2, capture, &c);
    CHECK(c.frames == 4 && !memcmp(input, result, sizeof input));
    audio_pcm_reset(&s); c.frames = 0;
    int16_t mono[] = {-32768, -123, 123, 32767};
    audio_pcm_convert(&s, mono, 4, 0, 0, capture, &c);
    CHECK(c.frames == 4);
    for (int i = 0; i < 4; i++) CHECK(result[i * 2] == mono[i] && result[i * 2 + 1] == mono[i]);
    /* Same-rate channel changes must discard the previous channel layout. */
    c.frames = 0;
    int16_t stereo[] = {23, -45, 67, -89};
    audio_pcm_convert(&s, stereo, 2, 44100, 2, capture, &c);
    CHECK(c.frames == 2 && !memcmp(result, stereo, sizeof stereo));
    /* A rate change starts at the first new frame, not the old tail. */
    c.frames = 0;
    int16_t next[] = {100, -100, 200, -200};
    audio_pcm_convert(&s, next, 2, 22050, 2, capture, &c);
    CHECK(c.frames == 3 && result[0] == 100 && result[1] == -100);
    CHECK(result[2] == 150 && result[3] == -150 && result[4] == 200 && result[5] == -200);
    c.frames = 0;
    audio_pcm_convert(&s, next, 2, 44100, 3, capture, &c);
    CHECK(c.frames == 0);
}
static void streaming(void)
{
    enum { N = 4096, MAX_OUT = 23000 };
    int16_t *input = malloc(N * AUDIO_PCM_FRAME_BYTES);
    int16_t *whole = malloc(MAX_OUT * AUDIO_PCM_FRAME_BYTES);
    int16_t *pieces = malloc(MAX_OUT * AUDIO_PCM_FRAME_BYTES);
    CHECK(input && whole && pieces);
    for (int i = 0; i < N; i++) { input[i * 2] = (int16_t)(i * 17 % 60001 - 30000); input[i * 2 + 1] = -input[i * 2]; }
    const uint32_t rates[] = {8000, 11025, 16000, 22050, 32000, 44100, 48000, 96000};
    for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++) {
        struct audio_pcm_resampler a = {0}, b = {0};
        struct capture ca = {whole, 0, MAX_OUT}, cb = {pieces, 0, MAX_OUT};
        audio_pcm_convert(&a, input, N, rates[r], 2, capture, &ca);
        /* Single-frame and irregular calls cross every interpolation boundary. */
        for (size_t pos = 0; pos < N;) {
            size_t count = pos % 31 + 1;
            if (count > N - pos) count = N - pos;
            audio_pcm_convert(&b, input + pos * 2, count, rates[r], 2, capture, &cb);
            pos += count;
        }
        CHECK(ca.frames == 1 + (uint64_t)(N - 1) * AUDIO_PCM_RATE / rates[r]);
        CHECK(cb.frames == ca.frames && !memcmp(whole, pieces, ca.frames * AUDIO_PCM_FRAME_BYTES));
        for (size_t i = 0; i < ca.frames; i++) CHECK(whole[i * 2] == -whole[i * 2 + 1]);
    }
    /* Manual pp. 52: the three service tones, with an independently checked
     * zero-crossing count at the host sample rate. */
    for (int hz = 500; hz <= 2000; hz *= 2) {
        enum { TONE_RATE = 8000, TONE_FRAMES = 1600 };
        for (int i = 0; i < TONE_FRAMES; i++) { input[i * 2] = (int16_t)(12000 * sin(2 * 3.141592653589793 * hz * i / TONE_RATE)); input[i * 2 + 1] = 0; }
        struct audio_pcm_resampler s = {0}; struct capture c = {whole, 0, MAX_OUT};
        audio_pcm_convert(&s, input, TONE_FRAMES, TONE_RATE, 2, capture, &c);
        int crossings = 0;
        for (size_t i = 1; i < c.frames; i++) {
            CHECK(whole[i * 2 + 1] == 0);
            crossings += whole[(i - 1) * 2] <= 0 && whole[i * 2] > 0;
        }
        CHECK(abs(crossings - hz / 5) <= 1);
    }
    free(input); free(whole); free(pieces);
}
static void ring(void)
{
    size_t capacity = AUDIO_PCM_RING_FRAMES;
    struct audio_pcm_ring *r = calloc(1, sizeof *r);
    int16_t *input = malloc((capacity + 13) * AUDIO_PCM_FRAME_BYTES);
    int16_t *output = malloc((capacity + 2) * AUDIO_PCM_FRAME_BYTES);
    CHECK(r && input && output);
    for (size_t i = 0; i < capacity + 13; i++) { input[i * 2] = (int16_t)i; input[i * 2 + 1] = (int16_t)(-1 - (int)i); }
    CHECK(audio_pcm_ring_drain(r, output, 10) == 0);
    audio_pcm_ring_put(r, input, 7);
    CHECK(audio_pcm_ring_drain(r, output, 1) == 1 && !memcmp(output, input, AUDIO_PCM_FRAME_BYTES));
    CHECK(audio_pcm_ring_drain(r, output, 0) == 0);
    CHECK(audio_pcm_ring_drain(r, NULL, 1) == 0 && r->frames == 6);
    /* Appending past the wrap point drops oldest whole frames. */
    audio_pcm_ring_put(r, input, capacity - 3);
    output[capacity * 2] = 1234; output[capacity * 2 + 1] = -1234;
    CHECK(audio_pcm_ring_drain(r, output, capacity + 1) == capacity);
    CHECK(!memcmp(output, input + 8, 3 * AUDIO_PCM_FRAME_BYTES));
    CHECK(!memcmp(output + 6, input, (capacity - 3) * AUDIO_PCM_FRAME_BYTES));
    CHECK(output[capacity * 2] == 1234 && output[capacity * 2 + 1] == -1234);
    /* A single oversized push retains exactly its final capacity frames. */
    audio_pcm_ring_put(r, input, capacity + 13);
    CHECK(audio_pcm_ring_drain(r, output, capacity) == capacity);
    CHECK(!memcmp(output, input + 26, capacity * AUDIO_PCM_FRAME_BYTES));
    audio_pcm_ring_put(r, input, 7); audio_pcm_ring_clear(r);
    CHECK(audio_pcm_ring_drain(r, output, capacity) == 0);
    free(r); free(input); free(output);
}
int main(void)
{
    isolation(); streaming(); ring();
    puts("stereo PCM: PASS");
    return 0;
}
