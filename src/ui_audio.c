/*
 *  src/ui_audio.c — host audio for the SDL front end: the SDL output
 *  device (or, in the web build, the ring web/worker.js drains), the
 *  Casio buzzer's square-wave pulses and the AIU wave stream resampled to
 *  44.1 kHz mono. Split out of src/ui.c.
 */

#include "ui.h"
#include "ui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_SDL2

#include <SDL.h>

static bool sdl_audio_initialized = false;
static SDL_AudioDeviceID audio_device = 0;

#define BUZZER_SAMPLE_RATE      44100
#define BUZZER_MAX_QUEUE_MS       250
#define BUZZER_RETRIGGER_GAP_MS   120
#define BUZZER_DEFAULT_HZ        1200
#define BUZZER_DEFAULT_MS          60
static uint32_t last_buzzer_tick = 0;

/* Real audio path (AIU codec). Streams continuously, so allow a deeper
 * queue than the buzzer's one-shot pulses. */
#define AIU_HOST_SAMPLE_RATE     BUZZER_SAMPLE_RATE
#define AIU_MAX_QUEUE_MS               120

static bool ui_audio_env_enabled(void)
{
    const char *v = getenv("BE300_AUDIO");

    if (!v || !*v)
        return true;
    return strcmp(v, "0") != 0 &&
        strcmp(v, "false") != 0 &&
        strcmp(v, "FALSE") != 0 &&
        strcmp(v, "off") != 0 &&
        strcmp(v, "OFF") != 0;
}

void ui_audio_init(void)
{
    SDL_AudioSpec want, have;

    last_buzzer_tick = 0;
    if (!ui_audio_env_enabled())
        return;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[UI] SDL audio init failed: %s — continuing silent\n",
                SDL_GetError());
        return;
    }
    sdl_audio_initialized = true;

    memset(&want, 0, sizeof(want));
    want.freq = BUZZER_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;

    audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!audio_device) {
        fprintf(stderr,
            "[UI] SDL_OpenAudioDevice failed: %s — continuing silent\n",
            SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        sdl_audio_initialized = false;
        return;
    }

    SDL_PauseAudioDevice(audio_device, 0);
}

void ui_audio_destroy(void)
{
    if (audio_device) {
        SDL_ClearQueuedAudio(audio_device);
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
    }
    if (sdl_audio_initialized) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        sdl_audio_initialized = false;
    }
}

/* Host audio sink: 44100 Hz mono S16. Native builds queue to the SDL
 * device. The web build runs in a Web Worker, which has no AudioContext,
 * so it fills a ring that web/worker.js drains and posts to the page. */
#ifdef __EMSCRIPTEN__
#define WEB_AUDIO_RING_SAMPLES (BUZZER_SAMPLE_RATE / 2u)
static int16_t web_audio_ring[WEB_AUDIO_RING_SAMPLES];
static size_t web_audio_head;
static size_t web_audio_count;
static bool web_audio_enabled;

void ui_web_audio_set_enabled(bool enabled)
{
    web_audio_enabled = enabled;
    web_audio_head = 0;
    web_audio_count = 0;
}

size_t ui_web_audio_drain(int16_t *dst, size_t max_samples)
{
    size_t n = web_audio_count < max_samples ? web_audio_count : max_samples;
    size_t tail = (web_audio_head + WEB_AUDIO_RING_SAMPLES - web_audio_count) %
        WEB_AUDIO_RING_SAMPLES;

    for (size_t i = 0; i < n; i++)
        dst[i] = web_audio_ring[(tail + i) % WEB_AUDIO_RING_SAMPLES];
    web_audio_count -= n;
    return n;
}

static bool ui_audio_sink_ready(void) { return web_audio_enabled; }
static uint32_t ui_audio_sink_queued_bytes(void)
{
    return (uint32_t)(web_audio_count * sizeof(int16_t));
}
static void ui_audio_sink_clear(void) { web_audio_count = 0; }
static void ui_audio_sink_put(const int16_t *pcm, size_t count)
{
    /* Overwrite the oldest samples when the worker falls behind. */
    for (size_t i = 0; i < count; i++) {
        web_audio_ring[web_audio_head] = pcm[i];
        web_audio_head = (web_audio_head + 1u) % WEB_AUDIO_RING_SAMPLES;
        if (web_audio_count < WEB_AUDIO_RING_SAMPLES)
            web_audio_count++;
    }
}
#else
static bool ui_audio_sink_ready(void) { return audio_device != 0; }
static uint32_t ui_audio_sink_queued_bytes(void)
{
    return SDL_GetQueuedAudioSize(audio_device);
}
static void ui_audio_sink_clear(void) { SDL_ClearQueuedAudio(audio_device); }
static void ui_audio_sink_put(const int16_t *pcm, size_t count)
{
    SDL_QueueAudio(audio_device, pcm, (uint32_t)(count * sizeof(*pcm)));
}
#endif

void ui_buzzer_pulse(uint32_t frequency_hz, uint32_t duration_ms)
{
    uint32_t queued_max;
    uint32_t now;
    uint32_t period;
    uint32_t samples;
    int16_t *pcm;

    if (!ui_audio_sink_ready())
        return;

    now = SDL_GetTicks();
    if (ui_audio_sink_queued_bytes() > 0 ||
        (last_buzzer_tick != 0 &&
         !ui_tick_reached(now, last_buzzer_tick + BUZZER_RETRIGGER_GAP_MS)))
        return;
    last_buzzer_tick = now;

    if (frequency_hz == 0)
        frequency_hz = BUZZER_DEFAULT_HZ;
    if (duration_ms == 0)
        duration_ms = BUZZER_DEFAULT_MS;

    if (frequency_hz < 80)
        frequency_hz = 80;
    if (frequency_hz > 6000)
        frequency_hz = 6000;
    if (duration_ms > 500)
        duration_ms = 500;

    samples = (BUZZER_SAMPLE_RATE * duration_ms) / 1000u;
    if (samples == 0)
        return;

    pcm = malloc((size_t)samples * sizeof(*pcm));
    if (!pcm)
        return;

    period = BUZZER_SAMPLE_RATE / frequency_hz;
    if (period < 2)
        period = 2;

    for (uint32_t i = 0; i < samples; i++) {
        uint32_t edge = (i * 12u) / samples;
        int32_t amp = 6500;

        if (edge < 12u)
            amp = (amp * (int32_t)edge) / 12;
        if (i > samples - (samples / 8u + 1u)) {
            uint32_t rem = samples - i;
            uint32_t tail = samples / 8u + 1u;
            amp = (amp * (int32_t)rem) / (int32_t)tail;
        }

        pcm[i] = ((i % period) < (period / 2u))
            ? (int16_t)amp
            : (int16_t)-amp;
    }

    queued_max = (BUZZER_SAMPLE_RATE * sizeof(int16_t) *
        BUZZER_MAX_QUEUE_MS) / 1000u;
    if (ui_audio_sink_queued_bytes() > queued_max)
        ui_audio_sink_clear();

    ui_audio_sink_put(pcm, samples);
    free(pcm);
}

static int32_t ui_audio_frame_mono(const int16_t *samples, size_t i,
                                   uint8_t channels)
{
    if (channels == 2u)
        return ((int32_t)samples[i * 2u] + (int32_t)samples[i * 2u + 1u]) / 2;
    return samples[i];
}

void ui_audio_queue_pcm(const int16_t *samples, size_t count_frames,
                        uint32_t guest_rate_hz, uint8_t channels)
{
    /* Linear-interpolating resampler whose phase and last input frame carry
     * across calls, so the AIU can feed it arbitrarily small chunks without
     * per-chunk rounding error (pitch drift) or boundary clicks. */
    static uint32_t last_rate_hz;
    static double phase;          /* next output position, in input frames
                                   * relative to this chunk; -1..0 = between
                                   * the previous chunk's last frame and
                                   * this chunk's first */
    static int32_t prev_frame;
    uint32_t queued_max;
    int16_t out[1024];
    size_t out_count = 0;
    double step;

    if (!ui_audio_sink_ready() || !samples || count_frames == 0)
        return;
    if (channels == 0)
        channels = 1;
    if (guest_rate_hz == 0)
        guest_rate_hz = AIU_HOST_SAMPLE_RATE;

    /* Backpressure: if the host queue is already deeper than our cap,
     * drop this batch. Keeps wall-clock latency bounded. */
    queued_max = (AIU_HOST_SAMPLE_RATE * (uint32_t)sizeof(int16_t) *
        AIU_MAX_QUEUE_MS) / 1000u;
    if (ui_audio_sink_queued_bytes() > queued_max) {
        last_rate_hz = 0;
        return;
    }

    if (guest_rate_hz != last_rate_hz) {
        last_rate_hz = guest_rate_hz;
        phase = 0.0;
        prev_frame = ui_audio_frame_mono(samples, 0, channels);
    }
    step = (double)guest_rate_hz / (double)AIU_HOST_SAMPLE_RATE;

    while (phase < (double)(count_frames - 1u) + 1e-9 ||
           phase < 0.0) {
        int32_t s0, s1, mixed;
        double frac;

        if (phase < 0.0) {
            s0 = prev_frame;
            s1 = ui_audio_frame_mono(samples, 0, channels);
            frac = phase + 1.0;
        } else {
            size_t i0 = (size_t)phase;
            size_t i1 = (i0 + 1u < count_frames) ? i0 + 1u : i0;

            s0 = ui_audio_frame_mono(samples, i0, channels);
            s1 = ui_audio_frame_mono(samples, i1, channels);
            frac = phase - (double)i0;
        }
        mixed = (int32_t)((double)s0 + (double)(s1 - s0) * frac);
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;
        out[out_count++] = (int16_t)mixed;
        if (out_count == sizeof(out) / sizeof(out[0])) {
            ui_audio_sink_put(out, out_count);
            out_count = 0;
        }
        phase += step;
    }
    prev_frame = ui_audio_frame_mono(samples, count_frames - 1u, channels);
    phase -= (double)count_frames;

    if (out_count > 0)
        ui_audio_sink_put(out, out_count);
}

#else  /* !HAVE_SDL2 — headless stubs */

void ui_buzzer_pulse(uint32_t frequency_hz, uint32_t duration_ms)
    { (void)frequency_hz; (void)duration_ms; }
void ui_audio_queue_pcm(const int16_t *samples, size_t count_frames,
                        uint32_t guest_rate_hz, uint8_t channels)
    { (void)samples; (void)count_frames; (void)guest_rate_hz; (void)channels; }

#endif  /* HAVE_SDL2 */
