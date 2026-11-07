/*
 *  ui_layout.c — window geometry and touch-panel mapping (see ui_layout.h).
 */
#include "ui_layout.h"

#include <math.h>

static bool rect_contains(const ui_rectf_t *r, double x, double y)
{
    return r->w > 0.0 && r->h > 0.0 &&
        x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

uint32_t ui_layout_strip_px(const ui_layout_t *l)
{
    uint32_t h = l->fb_h / 8u;
    return h ? h : 1u;
}

ui_rectf_t ui_layout_icon_from_lcd(ui_rectf_t lcd, uint32_t frame_h)
{
    ui_rectf_t icon;
    double h = floor(lcd.h / 8.0);

    icon.x = lcd.x;
    icon.y = lcd.y + lcd.h;
    icon.w = lcd.w;
    if (h < 1.0)
        h = 1.0;
    if (icon.y + h > (double)frame_h)
        h = (double)frame_h - icon.y;
    if (h < 1.0)
        h = 1.0;
    icon.h = h;
    return icon;
}

/* Content size at zoom 1.0, in points. */
static void unit_size(const ui_layout_t *l, double *w, double *h)
{
    if (l->bezel && l->frame_lcd.w > 0.0 && l->frame_lcd.h > 0.0) {
        *w = (double)l->frame_w * (double)l->fb_w / l->frame_lcd.w;
        *h = (double)l->frame_h * (double)l->fb_h / l->frame_lcd.h;
        return;
    }
    *w = (double)l->fb_w;
    *h = (double)l->fb_h + (l->strip ? (double)ui_layout_strip_px(l) : 0.0);
}

void ui_layout_natural_size(const ui_layout_t *l, double zoom,
    int *w_out, int *h_out)
{
    double uw, uh;

    unit_size(l, &uw, &uh);
    *w_out = (int)ceil(uw * zoom - 1e-6);
    *h_out = (int)ceil(uh * zoom - 1e-6);
    if (*w_out < 1)
        *w_out = 1;
    if (*h_out < 1)
        *h_out = 1;
}

double ui_layout_aspect(const ui_layout_t *l)
{
    double uw, uh;

    unit_size(l, &uw, &uh);
    return uh > 0.0 ? uw / uh : 1.0;
}

double ui_layout_zoom_for(const ui_layout_t *l, int win_w, int win_h)
{
    double uw, uh, z;

    unit_size(l, &uw, &uh);
    if (uw <= 0.0 || uh <= 0.0 || win_w <= 0 || win_h <= 0)
        return 1.0;
    z = fmin((double)win_w / uw, (double)win_h / uh);
    /* Absorb the ceil() in ui_layout_natural_size: a window created at zoom
     * z reports z back, not z +/- a fraction of a point. Snap to the
     * nearest 1/64 step when that moves the content by under a point and
     * still fits. */
    {
        int nw, nh;
        double r = floor(z * 64.0 + 0.5) / 64.0;
        ui_layout_natural_size(l, r, &nw, &nh);
        if (r > 0.0 && fabs(z - r) * fmax(uw, uh) <= 1.0 &&
            nw <= win_w && nh <= win_h)
            z = r;
    }
    return z > 0.0 ? z : 1.0;
}

void ui_layout_fit(ui_layout_t *l, int win_w, int win_h)
{
    double uw, uh, z, ox, oy;

    unit_size(l, &uw, &uh);
    z = ui_layout_zoom_for(l, win_w, win_h);
    l->zoom = z;
    ox = floor(((double)win_w - uw * z) / 2.0);
    oy = floor(((double)win_h - uh * z) / 2.0);
    if (ox < 0.0)
        ox = 0.0;
    if (oy < 0.0)
        oy = 0.0;

    if (l->bezel && l->frame_lcd.w > 0.0 && l->frame_lcd.h > 0.0) {
        double lcd_w = floor((double)l->fb_w * z + 0.5);
        double lcd_h = floor((double)l->fb_h * z + 0.5);

        if (lcd_w < 1.0)
            lcd_w = 1.0;
        if (lcd_h < 1.0)
            lcd_h = 1.0;
        l->scale_x = lcd_w / l->frame_lcd.w;
        l->scale_y = lcd_h / l->frame_lcd.h;
        /* Keep the LCD on whole points so it scales crisply; the artwork
         * is positioned around it. */
        l->lcd_dst.x = ceil(ox + l->frame_lcd.x * l->scale_x);
        l->lcd_dst.y = ceil(oy + l->frame_lcd.y * l->scale_y);
        l->lcd_dst.w = lcd_w;
        l->lcd_dst.h = lcd_h;
        l->frame_dst.x = l->lcd_dst.x - l->frame_lcd.x * l->scale_x;
        l->frame_dst.y = l->lcd_dst.y - l->frame_lcd.y * l->scale_y;
        l->frame_dst.w = (double)l->frame_w * l->scale_x;
        l->frame_dst.h = (double)l->frame_h * l->scale_y;
        l->strip_dst.x = l->frame_dst.x + l->frame_icon.x * l->scale_x;
        l->strip_dst.y = l->frame_dst.y + l->frame_icon.y * l->scale_y;
        l->strip_dst.w = l->frame_icon.w * l->scale_x;
        l->strip_dst.h = l->frame_icon.h * l->scale_y;
        return;
    }

    l->scale_x = l->scale_y = 0.0;
    l->frame_dst.x = l->frame_dst.y = l->frame_dst.w = l->frame_dst.h = 0.0;
    l->lcd_dst.x = ox;
    l->lcd_dst.y = oy;
    l->lcd_dst.w = floor((double)l->fb_w * z + 0.5);
    l->lcd_dst.h = floor((double)l->fb_h * z + 0.5);
    if (l->strip) {
        l->strip_dst.x = ox;
        l->strip_dst.y = oy + l->lcd_dst.h;
        l->strip_dst.w = l->lcd_dst.w;
        l->strip_dst.h = floor(oy + uh * z + 0.5) - l->strip_dst.y;
        if (l->strip_dst.h < 1.0)
            l->strip_dst.h = 1.0;
    } else {
        l->strip_dst.x = l->strip_dst.y = 0.0;
        l->strip_dst.w = l->strip_dst.h = 0.0;
    }
}

bool ui_layout_window_to_frame(const ui_layout_t *l, double x, double y,
    double *fx_out, double *fy_out)
{
    if (!l->bezel || l->scale_x <= 0.0 || l->scale_y <= 0.0)
        return false;
    *fx_out = (x - l->frame_dst.x) / l->scale_x;
    *fy_out = (y - l->frame_dst.y) / l->scale_y;
    return true;
}

static void rect_to_touch(const ui_rectf_t *r, double x, double y,
    uint32_t y_base, uint32_t y_span, uint16_t *tx_out, uint16_t *ty_out)
{
    int tx = (int)(((x - r->x) * (double)UI_TOUCH_LCD_W) / r->w);
    int ty = (int)y_base + (int)(((y - r->y) * (double)y_span) / r->h);

    if (tx < 0)
        tx = 0;
    if (tx >= (int)UI_TOUCH_LCD_W)
        tx = (int)UI_TOUCH_LCD_W - 1;
    if (ty < (int)y_base)
        ty = (int)y_base;
    if (ty > (int)(y_base + y_span - 1u))
        ty = (int)(y_base + y_span - 1u);
    *tx_out = (uint16_t)tx;
    *ty_out = (uint16_t)ty;
}

bool ui_layout_window_to_touch(const ui_layout_t *l, double x, double y,
    uint16_t *tx_out, uint16_t *ty_out)
{
    if (rect_contains(&l->lcd_dst, x, y)) {
        rect_to_touch(&l->lcd_dst, x, y, 0, UI_TOUCH_LCD_H, tx_out, ty_out);
        return true;
    }
    if (rect_contains(&l->strip_dst, x, y)) {
        rect_to_touch(&l->strip_dst, x, y, UI_TOUCH_LCD_H, UI_TOUCH_STRIP_H,
            tx_out, ty_out);
        return true;
    }
    return false;
}

bool ui_layout_point_is_touch(const ui_layout_t *l, double x, double y)
{
    return rect_contains(&l->lcd_dst, x, y) ||
        rect_contains(&l->strip_dst, x, y);
}
