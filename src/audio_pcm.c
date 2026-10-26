#include "audio_pcm.h"
#include <string.h>

void audio_pcm_reset(struct audio_pcm_resampler *s)
{
    memset(s, 0, sizeof(*s));
}

void audio_pcm_convert(struct audio_pcm_resampler *s, const int16_t *input,
    size_t frames, uint32_t rate, uint8_t channels, audio_pcm_sink sink, void *context)
{
    int16_t output[512 * AUDIO_PCM_CHANNELS];
    size_t produced = 0;

    if (!s || !input || !frames || !sink)
        return;
    if (!rate) rate = AUDIO_PCM_RATE;
    if (!channels) channels = 1;
    if (channels > AUDIO_PCM_CHANNELS)
        return;
    if (s->rate != rate || s->channels != channels) {
        audio_pcm_reset(s);
        s->rate = rate;
        s->channels = channels;
    }

    /* Retain one source frame across calls. Integer phase makes the output
     * independent of how the guest splits its DMA buffers into host chunks. */
    for (size_t i = 0; i < frames; i++) {
        int16_t current[2] = { input[i * channels], input[i * channels + channels - 1] };
        if (!s->primed) {
            output[produced * 2] = current[0];
            output[produced * 2 + 1] = current[1];
            produced++;
            s->phase = rate;
            s->primed = true;
        } else {
            while (s->phase <= AUDIO_PCM_RATE) {
                for (unsigned c = 0; c < AUDIO_PCM_CHANNELS; c++) {
                    int64_t fraction = (int64_t)s->phase;
                    int64_t weighted = (int64_t)s->previous[c] *
                        (AUDIO_PCM_RATE - fraction) + (int64_t)current[c] * fraction;
                    output[produced * 2 + c] = (int16_t)(weighted / AUDIO_PCM_RATE);
                }
                produced++;
                s->phase += rate;
                if (produced == 512) {
                    sink(context, output, produced);
                    produced = 0;
                }
            }
            s->phase -= AUDIO_PCM_RATE;
        }
        memcpy(s->previous, current, sizeof(current));
        if (produced == 512) {
            sink(context, output, produced);
            produced = 0;
        }
    }
    if (produced)
        sink(context, output, produced);
}

void audio_pcm_ring_clear(struct audio_pcm_ring *r)
{
    r->head = r->frames = 0;
}

void audio_pcm_ring_put(struct audio_pcm_ring *r, const int16_t *pcm, size_t frames)
{
    if (!pcm || !frames) return;
    if (frames > AUDIO_PCM_RING_FRAMES) {
        pcm += (frames - AUDIO_PCM_RING_FRAMES) * AUDIO_PCM_CHANNELS;
        frames = AUDIO_PCM_RING_FRAMES;
    }
    size_t first = AUDIO_PCM_RING_FRAMES - r->head;
    if (first > frames) first = frames;
    memcpy(r->pcm + r->head * AUDIO_PCM_CHANNELS, pcm, first * AUDIO_PCM_FRAME_BYTES);
    memcpy(r->pcm, pcm + first * AUDIO_PCM_CHANNELS, (frames - first) * AUDIO_PCM_FRAME_BYTES);
    r->head = (r->head + frames) % AUDIO_PCM_RING_FRAMES;
    r->frames += frames;
    if (r->frames > AUDIO_PCM_RING_FRAMES) r->frames = AUDIO_PCM_RING_FRAMES;
}

size_t audio_pcm_ring_drain(struct audio_pcm_ring *r, int16_t *pcm, size_t max_frames)
{
    if (!pcm) return 0;
    size_t frames = r->frames < max_frames ? r->frames : max_frames;
    size_t tail = (r->head + AUDIO_PCM_RING_FRAMES - r->frames) % AUDIO_PCM_RING_FRAMES;
    size_t first = AUDIO_PCM_RING_FRAMES - tail;
    if (first > frames) first = frames;
    memcpy(pcm, r->pcm + tail * AUDIO_PCM_CHANNELS, first * AUDIO_PCM_FRAME_BYTES);
    memcpy(pcm + first * AUDIO_PCM_CHANNELS, r->pcm, (frames - first) * AUDIO_PCM_FRAME_BYTES);
    r->frames -= frames;
    return frames;
}
