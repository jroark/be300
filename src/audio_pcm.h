/* Host PCM format and streaming conversion. Casio BE-300 service manual,
 * printed pp. 2/52: stereo headphone output; guest DMA already supplies L/R. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIO_PCM_RATE 44100u
#define AUDIO_PCM_CHANNELS 2u
#define AUDIO_PCM_FRAME_BYTES (AUDIO_PCM_CHANNELS * sizeof(int16_t))
#define AUDIO_PCM_RING_FRAMES (AUDIO_PCM_RATE / 2u)

typedef void (*audio_pcm_sink)(void *context, const int16_t *pcm, size_t frames);

struct audio_pcm_resampler {
    uint32_t rate;
    uint8_t channels;
    bool primed;
    uint64_t phase;             /* next output position, in 1/44100 input frames */
    int16_t previous[AUDIO_PCM_CHANNELS];
};

void audio_pcm_reset(struct audio_pcm_resampler *s);
/* Input is S16 mono or interleaved stereo; sink receives stereo frames.
 * A rate/channel change starts a new stream. Zero rate/channels mean 44100/mono. */
void audio_pcm_convert(struct audio_pcm_resampler *s, const int16_t *input,
    size_t frames, uint32_t rate, uint8_t channels, audio_pcm_sink sink, void *context);

struct audio_pcm_ring {
    int16_t pcm[AUDIO_PCM_RING_FRAMES * AUDIO_PCM_CHANNELS];
    size_t head, frames;
};
void audio_pcm_ring_clear(struct audio_pcm_ring *r);
/* Overflow discards the oldest complete frames, preserving L/R alignment. */
void audio_pcm_ring_put(struct audio_pcm_ring *r, const int16_t *pcm, size_t frames);
size_t audio_pcm_ring_drain(struct audio_pcm_ring *r, int16_t *pcm, size_t max_frames);
