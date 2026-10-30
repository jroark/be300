/* Compile the production SDL adapter and browser ring with a recording SDL
 * sink. No sound device or guest image is needed to inspect exact PCM bytes. */
#include "ui_internal.h"
#include "audio_pcm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static Uint32 now = 1000;
static bool opened;
static SDL_AudioSpec output_spec;

int SDL_InitSubSystem(Uint32 flags) { (void)flags; return 0; }
void SDL_QuitSubSystem(Uint32 flags) { (void)flags; }
const char *SDL_GetError(void) { return "test sink"; }
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int capture,
    const SDL_AudioSpec *want, SDL_AudioSpec *have, int changes)
{
    (void)device; (void)capture; (void)changes;
    CHECK(want->channels == 2 && want->freq == 44100 && want->format == AUDIO_S16SYS);
    *have = *want; output_spec = *want; opened = true; return 1;
}
void SDL_CloseAudioDevice(SDL_AudioDeviceID device) { (void)device; opened = false; }
void SDL_PauseAudioDevice(SDL_AudioDeviceID device, int pause) { (void)device; (void)pause; }
void SDL_LockAudioDevice(SDL_AudioDeviceID device) { CHECK(device==1 && opened); }
void SDL_UnlockAudioDevice(SDL_AudioDeviceID device) { CHECK(device==1 && opened); }
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
static void clear_sink(void) { ui_audio_reset_stream(); }
static size_t drain_sink(int16_t *pcm, size_t frames)
{
    CHECK(output_spec.callback);
    output_spec.callback(output_spec.userdata,(Uint8 *)pcm,(int)(frames*AUDIO_PCM_FRAME_BYTES));
    return frames;
}
#endif
int main(void)
{
    int16_t input[] = {-32768, 0, 0, 32767, 2345, -2345};
    int16_t output[20000];
    ui_audio_init();
#ifndef BE300_AUDIO_WEB
    CHECK(opened);
#endif
    clear_sink();
    memset(output,0x7f,sizeof(output));
    size_t quiet=drain_sink(output,100);
    for(size_t i=0;i<quiet*2;i++)CHECK(!output[i]); /* no invented boot chirps */
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
    CHECK(drain_sink(output, 1) == 1 && output[0] == 1000 && output[1] == 1000);
    clear_sink(); now += 500;
    ui_buzzer_pulse(1000, 20);
    size_t frames = drain_sink(output, 882);
    CHECK(frames == 882);
    for (size_t i = 0; i < frames; i++) CHECK(output[i * 2] == output[i * 2 + 1]);
    /* Queue limits remain time-based after doubling bytes per frame. */
    clear_sink();
    int16_t *large = calloc(12000, sizeof *large); CHECK(large);
    ui_audio_queue_pcm(large, 6000, 44100, 2);
    ui_audio_queue_pcm(input, 3, 44100, 2);
    CHECK(drain_sink(output, 6000) == 6000);
    free(large);
    /* Real PWM runs alongside stereo wave output, without suppressing either. */
    clear_sink();
    int16_t wave[200];for(unsigned i=0;i<100;i++){wave[i*2]=1000;wave[i*2+1]=-1000;}
    ui_audio_set_buzzer(1412,706,true);
    ui_audio_queue_pcm(wave,100,44100,2);
    now+=3;
    CHECK(drain_sink(output,100)==100);
    for(unsigned i=0;i<100;i++){
        CHECK(output[i*2]-output[i*2+1]==2000);
        CHECK(output[i*2]==7500 || output[i*2]==-5500);
    }
#ifdef BE300_AUDIO_WEB
    CHECK(drain_sink(output,100)==32); /* only 3 ms of tone, never future audio */
    CHECK(drain_sink(output,100)==0);
#endif
    ui_audio_set_buzzer(1412,706,false);
    quiet=drain_sink(output,100);
    for(size_t i=0;i<quiet*2;i++)CHECK(!output[i]);
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
