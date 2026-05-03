#pragma once

#include "be300.h"

typedef void (*ui_frame_callback_t)(const uint16_t *pixels,
                                    uint32_t width,
                                    uint32_t height,
                                    uint32_t stride,
                                    void *user_data);

int  ui_init(machine_t *m);
void ui_update(machine_t *m);
void ui_destroy(machine_t *m);
bool ui_should_quit(machine_t *m);
void ui_save_screenshot(machine_t *m);
void ui_buzzer_pulse(uint32_t frequency_hz, uint32_t duration_ms);
/* Queue PCM frames to the host SDL audio sink.
 *  - samples: little-endian S16 frames, channels-interleaved.
 *  - count_frames: number of frames (NOT bytes; one frame = channels samples).
 *  - guest_rate_hz: source sample rate; resampled to the host output rate
 *    (44100 Hz mono) using a simple linear interpolator.
 *  - channels: 1 (mono) or 2 (stereo, downmixed to mono).
 * Safe to call when SDL audio is not initialized - the call becomes a no-op. */
void ui_audio_queue_pcm(const int16_t *samples, size_t count_frames,
                        uint32_t guest_rate_hz, uint8_t channels);
void ui_set_frame_callback(ui_frame_callback_t cb, void *user_data);
