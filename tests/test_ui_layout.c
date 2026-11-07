/* Window geometry and touch-panel mapping for the desktop frontend. */
#include "ui_layout.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static void touch(const ui_layout_t *l, double x, double y,
    int want_x, int want_y)
{
    uint16_t tx = 0, ty = 0;
    CHECK(ui_layout_window_to_touch(l, x, y, &tx, &ty));
    if (tx != want_x || ty != want_y) {
        fprintf(stderr, "touch(%.1f,%.1f) = %u,%u want %d,%d\n",
            x, y, tx, ty, want_x, want_y);
        exit(1);
    }
}

static void miss(const ui_layout_t *l, double x, double y)
{
    uint16_t tx, ty;
    CHECK(!ui_layout_window_to_touch(l, x, y, &tx, &ty));
}

static void frameless_strip(void)
{
    ui_layout_t l = { .strip = true, .fb_w = 240, .fb_h = 320 };
    int w, h;

    CHECK(ui_layout_strip_px(&l) == 40);
    ui_layout_natural_size(&l, 1.0, &w, &h);
    CHECK(w == 240 && h == 360);
    ui_layout_natural_size(&l, 2.0, &w, &h);
    CHECK(w == 480 && h == 720);

    ui_layout_fit(&l, 240, 360);
    CHECK(l.zoom == 1.0);
    touch(&l, 0, 0, 0, 0);
    touch(&l, 239.9, 319.9, 239, 319);
    touch(&l, 120, 320, 120, 320);       /* first strip row */
    touch(&l, 120, 359.9, 120, 359);     /* last strip row */
    miss(&l, 120, 360);
    miss(&l, 240, 10);

    /* Odd window: letterboxed, mapping follows the content. */
    ui_layout_fit(&l, 1000, 720);
    CHECK(l.zoom == 2.0);
    CHECK(l.lcd_dst.x == 260 && l.lcd_dst.y == 0);
    touch(&l, 260, 0, 0, 0);
    touch(&l, 260 + 479.9, 639.9, 239, 319);
    touch(&l, 260 + 240, 640 + 40, 120, 340);
    miss(&l, 100, 100);

    /* Fractional zoom still covers 0..359 exactly. */
    ui_layout_fit(&l, 333, 500);
    CHECK(fabs(l.zoom - 333.0 / 240.0) < 1e-9);
    touch(&l, l.strip_dst.x + l.strip_dst.w - 0.01,
        l.strip_dst.y + l.strip_dst.h - 0.01, 239, 359);

    /* Without the strip the window is the bare LCD. */
    l.strip = false;
    ui_layout_natural_size(&l, 1.0, &w, &h);
    CHECK(w == 240 && h == 320);
    ui_layout_fit(&l, 240, 320);
    miss(&l, 10, 330);
}

static void bezel(void)
{
    ui_layout_t l = {
        .bezel = true, .fb_w = 240, .fb_h = 320,
        .frame_w = 1024, .frame_h = 1536,
        .frame_lcd = { 187, 218, 644, 859 },
    };
    int w, h;
    double fx, fy;

    l.frame_icon = ui_layout_icon_from_lcd(l.frame_lcd, l.frame_h);
    CHECK(l.frame_icon.y == 218 + 859 && l.frame_icon.h == 107);

    ui_layout_natural_size(&l, 1.0, &w, &h);
    ui_layout_fit(&l, w, h);
    CHECK(fabs(l.zoom - 1.0) < 1e-3);
    CHECK(l.lcd_dst.w == 240 && l.lcd_dst.h == 320);
    touch(&l, l.lcd_dst.x, l.lcd_dst.y, 0, 0);
    touch(&l, l.strip_dst.x + 0.01, l.strip_dst.y + 0.01, 0, 320);
    touch(&l, l.strip_dst.x + l.strip_dst.w - 0.01,
        l.strip_dst.y + l.strip_dst.h - 0.01, 239, 359);
    miss(&l, 1, 1);

    CHECK(ui_layout_window_to_frame(&l, l.lcd_dst.x, l.lcd_dst.y, &fx, &fy));
    CHECK(fabs(fx - 187) < 1e-6 && fabs(fy - 218) < 1e-6);

    /* Double size window doubles the zoom and keeps the aperture aligned. */
    ui_layout_natural_size(&l, 2.0, &w, &h);
    ui_layout_fit(&l, w, h);
    CHECK(fabs(l.zoom - 2.0) < 1e-3);
    CHECK(l.lcd_dst.w == 480 && l.lcd_dst.h == 640);
    CHECK(ui_layout_window_to_frame(&l, l.lcd_dst.x, l.lcd_dst.y, &fx, &fy));
    CHECK(fabs(fx - 187) < 1e-6 && fabs(fy - 218) < 1e-6);

    CHECK(fabs(ui_layout_aspect(&l) -
        (1024.0 * 240 / 644) / (1536.0 * 320 / 859)) < 1e-9);
}

int main(void)
{
    frameless_strip();
    bezel();
    puts("ui_layout ok");
    return 0;
}
