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
void ui_set_frame_callback(ui_frame_callback_t cb, void *user_data);
