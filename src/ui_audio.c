/*
 *  src/ui_audio.c — host audio for the SDL front end: the SDL output
 *  device (or, in the web build, the ring web/worker.js drains), the
 *  Casio buzzer's square-wave pulses and the AIU wave stream resampled to
 *  44.1 kHz stereo. Split out of src/ui.c.
 */

#include "ui.h"
#include "ui_internal.h"
#include "audio_pcm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_SDL2

#include <SDL.h>

static bool sdl_audio_initialized = false;
static SDL_AudioDeviceID audio_device = 0;
static struct audio_pcm_resampler pcm_resampler;

#define BUZZER_SAMPLE_RATE      AUDIO_PCM_RATE
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
    audio_pcm_reset(&pcm_resampler);
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
    want.channels = AUDIO_PCM_CHANNELS;
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

void ui_audio_reset_stream(void)
{
    audio_pcm_reset(&pcm_resampler);
}

void ui_audio_destroy(void)
{
    audio_pcm_reset(&pcm_resampler);
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

/* Host audio sink: 44100 Hz interleaved stereo S16. Native builds queue to the SDL
 * device. The web build runs in a Web Worker, which has no AudioContext,
 * so it fills a ring that web/worker.js drains and posts to the page. */
#if defined(__EMSCRIPTEN__) || defined(BE300_AUDIO_WEB)
static struct audio_pcm_ring web_audio_ring;
static bool web_audio_enabled;

void ui_web_audio_set_enabled(bool enabled)
{
    web_audio_enabled = enabled;
    audio_pcm_ring_clear(&web_audio_ring);
    audio_pcm_reset(&pcm_resampler);
    last_buzzer_tick = 0;
}

size_t ui_web_audio_drain(int16_t *dst, size_t max_frames)
{
    return audio_pcm_ring_drain(&web_audio_ring, dst, max_frames);
}

static bool ui_audio_sink_ready(void) { return web_audio_enabled; }
static uint32_t ui_audio_sink_queued_bytes(void)
{
    return (uint32_t)(web_audio_ring.frames * AUDIO_PCM_FRAME_BYTES);
}
static void ui_audio_sink_clear(void) { audio_pcm_ring_clear(&web_audio_ring); }
static void ui_audio_sink_put(void *context, const int16_t *pcm, size_t frames)
{
    (void)context;
    audio_pcm_ring_put(&web_audio_ring, pcm, frames);
}
#else
static bool ui_audio_sink_ready(void) { return audio_device != 0; }
static uint32_t ui_audio_sink_queued_bytes(void)
{
    return SDL_GetQueuedAudioSize(audio_device);
}
static void ui_audio_sink_clear(void) { SDL_ClearQueuedAudio(audio_device); }
static void ui_audio_sink_put(void *context, const int16_t *pcm, size_t frames)
{
    (void)context;
    SDL_QueueAudio(audio_device, pcm, (uint32_t)(frames * AUDIO_PCM_FRAME_BYTES));
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

    pcm = malloc((size_t)samples * AUDIO_PCM_FRAME_BYTES);
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

        pcm[i * 2] = pcm[i * 2 + 1] = ((i % period) < (period / 2u))
            ? (int16_t)amp
            : (int16_t)-amp;
    }

    queued_max = (BUZZER_SAMPLE_RATE * AUDIO_PCM_FRAME_BYTES *
        BUZZER_MAX_QUEUE_MS) / 1000u;
    if (ui_audio_sink_queued_bytes() > queued_max)
        ui_audio_sink_clear();

    ui_audio_sink_put(NULL, pcm, samples);
    free(pcm);
}

void ui_audio_queue_pcm(const int16_t *samples, size_t count_frames,
                        uint32_t guest_rate_hz, uint8_t channels)
{
    if (!ui_audio_sink_ready()) {
        audio_pcm_reset(&pcm_resampler);
        return;
    }
    uint32_t queued_max = (AIU_HOST_SAMPLE_RATE * AUDIO_PCM_FRAME_BYTES *
        AIU_MAX_QUEUE_MS) / 1000u;
    if (ui_audio_sink_queued_bytes() > queued_max) {
        audio_pcm_reset(&pcm_resampler);
        return;
    }
    audio_pcm_convert(&pcm_resampler, samples, count_frames, guest_rate_hz,
        channels, ui_audio_sink_put, NULL);
}

#else  /* !HAVE_SDL2 — headless stubs */

void ui_audio_reset_stream(void) {}
void ui_buzzer_pulse(uint32_t frequency_hz, uint32_t duration_ms)
    { (void)frequency_hz; (void)duration_ms; }
void ui_audio_queue_pcm(const int16_t *samples, size_t count_frames,
                        uint32_t guest_rate_hz, uint8_t channels)
    { (void)samples; (void)count_frames; (void)guest_rate_hz; (void)channels; }

#endif  /* HAVE_SDL2 */
