/*
 *  ui_layout.h — window geometry and touch-panel mapping for the desktop
 *  frontend, kept free of SDL so it can be unit tested.
 *
 *  All window coordinates are in window points (SDL mouse/window units),
 *  not backing pixels. Frame coordinates are pixels of the (trimmed) bezel
 *  artwork.
 *
 *  The BE-300 resistive panel is taller than the LCD: rows 0..319 cover the
 *  240x320 LCD and rows 320..359 cover the strip of seven printed icons
 *  below it. Both the bezel and the frameless layouts expose that strip.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define UI_TOUCH_LCD_W      240u
#define UI_TOUCH_LCD_H      320u
#define UI_TOUCH_STRIP_H    40u
#define UI_TOUCH_PANEL_MAX_Y (UI_TOUCH_LCD_H + UI_TOUCH_STRIP_H - 1u)

#define UI_ZOOM_MIN 0.5
#define UI_ZOOM_MAX 6.0

typedef struct {
    double x, y, w, h;
} ui_rectf_t;

typedef struct {
    /* Configuration (set before ui_layout_fit / ui_layout_natural_size). */
    bool       bezel;        /* draw the BE-300 bezel artwork */
    bool       strip;        /* frameless only: show the icon strip */
    uint32_t   fb_w, fb_h;   /* guest LCD size in pixels */
    uint32_t   frame_w, frame_h;  /* bezel artwork size */
    ui_rectf_t frame_lcd;    /* LCD aperture inside the artwork */
    ui_rectf_t frame_icon;   /* icon strip inside the artwork */

    /* Derived by ui_layout_fit. */
    double     zoom;         /* window points per guest LCD pixel */
    double     scale_x;      /* window points per artwork pixel (bezel) */
    double     scale_y;
    ui_rectf_t frame_dst;    /* artwork destination (bezel) */
    ui_rectf_t lcd_dst;      /* LCD destination, integer aligned */
    ui_rectf_t strip_dst;    /* icon strip destination */
} ui_layout_t;

/* Icon strip height in guest pixels for a frameless window: 1/8 of the LCD
 * height, matching the 40 panel rows below a 320-row LCD. */
uint32_t ui_layout_strip_px(const ui_layout_t *l);

/* Estimate the artwork icon strip from the detected LCD aperture. */
ui_rectf_t ui_layout_icon_from_lcd(ui_rectf_t lcd, uint32_t frame_h);

/* Window size (points) for the given zoom, rounded up. */
void ui_layout_natural_size(const ui_layout_t *l, double zoom,
    int *w_out, int *h_out);

/* Aspect ratio (w / h) of the window content. */
double ui_layout_aspect(const ui_layout_t *l);

/* Largest zoom that fits a window of win_w x win_h points. */
double ui_layout_zoom_for(const ui_layout_t *l, int win_w, int win_h);

/* Lay the content out centered in a win_w x win_h window. */
void ui_layout_fit(ui_layout_t *l, int win_w, int win_h);

/* Window point -> artwork pixel. False when there is no bezel. */
bool ui_layout_window_to_frame(const ui_layout_t *l, double x, double y,
    double *fx_out, double *fy_out);

/* Window point -> panel coordinate (x 0..239, y 0..359). False when the
 * point is outside both the LCD and the icon strip. */
bool ui_layout_window_to_touch(const ui_layout_t *l, double x, double y,
    uint16_t *tx_out, uint16_t *ty_out);

/* True when the point lies over the LCD or the icon strip. */
bool ui_layout_point_is_touch(const ui_layout_t *l, double x, double y);
