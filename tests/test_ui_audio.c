/* Compile the production SDL adapter and browser ring with a recording SDL
 * sink. No sound device or guest image is needed to inspect exact PCM bytes. */
#include "ui_internal.h"
#include "audio_pcm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static int16_t captured[50000 * AUDIO_PCM_CHANNELS];
static size_t captured_frames;
static Uint32 now = 1000;
static bool opened;

int SDL_InitSubSystem(Uint32 flags) { (void)flags; return 0; }
void SDL_QuitSubSystem(Uint32 flags) { (void)flags; }
const char *SDL_GetError(void) { return "test sink"; }
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int capture,
    const SDL_AudioSpec *want, SDL_AudioSpec *have, int changes)
{
    (void)device; (void)capture; (void)changes;
    CHECK(want->channels == 2 && want->freq == 44100 && want->format == AUDIO_S16SYS);
    *have = *want; opened = true; return 1;
}
void SDL_CloseAudioDevice(SDL_AudioDeviceID device) { (void)device; opened = false; }
void SDL_PauseAudioDevice(SDL_AudioDeviceID device, int pause) { (void)device; (void)pause; }
void SDL_ClearQueuedAudio(SDL_AudioDeviceID device) { (void)device; captured_frames = 0; }
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID device) { (void)device; return (Uint32)(captured_frames * AUDIO_PCM_FRAME_BYTES); }
int SDL_QueueAudio(SDL_AudioDeviceID device, const void *pcm, Uint32 len)
{
    CHECK(device == 1 && len % AUDIO_PCM_FRAME_BYTES == 0);
    CHECK(captured_frames * AUDIO_PCM_FRAME_BYTES + len <= sizeof captured);
    memcpy(captured + captured_frames * 2, pcm, len);
    captured_frames += len / AUDIO_PCM_FRAME_BYTES;
    return 0;
}
Uint32 SDL_GetTicks(void) { return now; }

#ifdef BE300_AUDIO_WEB
static void clear_sink(void)
{
    ui_web_audio_set_enabled(true);
}
static size_t drain_sink(int16_t *pcm, size_t frames)
{
    return ui_web_audio_drain(pcm, frames);
}
#else
static void clear_sink(void) { captured_frames = 0; ui_audio_reset_stream(); }
static size_t drain_sink(int16_t *pcm, size_t frames)
{
    if (frames > captured_frames) frames = captured_frames;
    memcpy(pcm, captured, frames * AUDIO_PCM_FRAME_BYTES);
    memmove(captured, captured + frames * 2, (captured_frames - frames) * AUDIO_PCM_FRAME_BYTES);
    captured_frames -= frames;
    return frames;
}
#endif
int main(void)
{
    int16_t input[] = {-32768, 0, 0, 32767, 2345, -2345};
    int16_t output[20000];
    ui_audio_init(); CHECK(opened); clear_sink();
    ui_audio_queue_pcm(input, 3, 44100, 2);
    CHECK(drain_sink(output, 3) == 3 && !memcmp(input, output, sizeof input));
    clear_sink();
    int16_t mono[] = {1000, -2000};
    ui_audio_queue_pcm(mono, 2, 22050, 1);
    CHECK(drain_sink(output, 3) == 3);
    CHECK(output[0] == 1000 && output[1] == 1000);
    CHECK(output[2] == -500 && output[3] == -500);
    CHECK(output[4] == -2000 && output[5] == -2000);
    /* New streams with the same format cannot interpolate across old tracks. */
    ui_audio_reset_stream();
    ui_audio_queue_pcm(mono, 1, 22050, 1);
    CHECK(drain_sink(output, 10) == 1 && output[0] == 1000 && output[1] == 1000);
    clear_sink(); now += 500;
    ui_buzzer_pulse(1000, 20);
    size_t frames = drain_sink(output, 10000);
    CHECK(frames == 882);
    for (size_t i = 0; i < frames; i++) CHECK(output[i * 2] == output[i * 2 + 1]);
    /* Queue limits remain time-based after doubling bytes per frame. */
    clear_sink();
    int16_t *large = calloc(12000, sizeof *large); CHECK(large);
    ui_audio_queue_pcm(large, 6000, 44100, 2);
    ui_audio_queue_pcm(input, 3, 44100, 2);
    CHECK(drain_sink(output, 10000) == 6000);
    free(large);
#ifdef BE300_AUDIO_WEB
    clear_sink(); ui_audio_queue_pcm(input, 3, 44100, 2);
    ui_web_audio_set_enabled(false);
    CHECK(ui_web_audio_drain(output, 10000) == 0);
    ui_audio_queue_pcm(input, 3, 44100, 2);
    CHECK(ui_web_audio_drain(output, 10000) == 0);
    ui_web_audio_set_enabled(true);
    ui_audio_queue_pcm(input, 3, 44100, 2);
    CHECK(ui_web_audio_drain(output, 3) == 3 && !memcmp(input, output, sizeof input));
#endif
    ui_audio_destroy(); CHECK(!opened);
    puts("stereo frontend sink: PASS");
    return 0;
}
