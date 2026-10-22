/*
 *  src/ui_button_mask.h — the BE-300 frame's button hit regions, found in
 *  the black-and-white mask src/ui/assets/buttons_dpad_bw_mask.png. The
 *  desktop front end analyses the mask at startup; the web front end uses
 *  web/button-regions.mjs, which tests/test_button_mask.c keeps in step.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UI_BUTTON_REGION_NONE = 0,
    UI_BUTTON_REGION_DPAD,
    UI_BUTTON_REGION_ROCKET,
    UI_BUTTON_REGION_POWER,
    UI_BUTTON_REGION_OK,
    UI_BUTTON_REGION_ESC,
    UI_BUTTON_REGION_COUNT
} ui_button_region_kind_t;

typedef struct {
    bool valid;
    uint32_t area;
    uint32_t min_x;
    uint32_t min_y;
    uint32_t max_x;
    uint32_t max_y;
    double cx;
    double cy;
} ui_button_region_t;

typedef struct {
    uint8_t *active;            /* 1 where the mask is a button, row-major */
    uint32_t width;
    uint32_t height;
    ui_button_region_t regions[UI_BUTTON_REGION_COUNT];
} ui_button_mask_t;

/* Threshold an RGBA mask and find the five button regions; replaces *mask
 * on success and leaves it alone on failure. */
bool ui_button_mask_build(ui_button_mask_t *mask, const uint8_t *rgba,
    uint32_t width, uint32_t height);
void ui_button_mask_free(ui_button_mask_t *mask);

/* Button bits (btn set 1 / set 2) under frame pixel (bx, by), if any. */
bool ui_button_mask_hit(const ui_button_mask_t *mask, double bx, double by,
    uint8_t *btn_set1_out, uint8_t *btn_set2_out);
