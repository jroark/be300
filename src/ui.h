#pragma once

#include "be300.h"

int  ui_init(machine_t *m);
void ui_update(machine_t *m);
void ui_destroy(machine_t *m);
bool ui_should_quit(machine_t *m);
void ui_save_screenshot(machine_t *m);
/* A new guest DMA stream starts with fresh interpolation history. */
void ui_audio_reset_stream(void);
void ui_buzzer_pulse(uint32_t frequency_hz, uint32_t duration_ms);
/* Queue PCM frames to the host SDL audio sink.
 *  - samples: little-endian S16 frames, channels-interleaved.
 *  - count_frames: number of frames (NOT bytes; one frame = channels samples).
 *  - guest_rate_hz: source sample rate; resampled to the host output rate
 *    (44100 Hz stereo) using a simple linear interpolator.
 *  - channels: 1 (mono) or 2 (stereo, preserved).
 * Safe to call when SDL audio is not initialized - the call becomes a no-op. */
void ui_audio_queue_pcm(const int16_t *samples, size_t count_frames,
                        uint32_t guest_rate_hz, uint8_t channels);
#if defined(__EMSCRIPTEN__) || defined(BE300_AUDIO_WEB)
/* Web build: buzzer and PCM output land in a 44100 Hz interleaved stereo S16 ring that
 * web/worker.js drains, instead of an SDL device. Enabling resets the ring. */
void   ui_web_audio_set_enabled(bool enabled);
/* Capacity and return value are frames; dst holds max_frames * 2 samples. */
size_t ui_web_audio_drain(int16_t *dst, size_t max_frames);
#endif
