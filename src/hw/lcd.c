#include "lcd.h"
#include <string.h>

static bool gate_byte(uint32_t off)
{
    return (off >= 0x1100 && off < 0x1104) ||
        (off >= 0x110C && off < 0x1110) ||
        (off >= 0x1128 && off < 0x112C);
}

bool lcd_handles(uint32_t off, unsigned len)
{
    if (!len || len > 8 || off > UINT32_MAX - len)
        return false;
    for (unsigned i = 0; i < len; i++) {
        uint32_t at = off + i;
        if (!(at >= 0xC00 && at < 0xC50) &&
            !(at >= 0x980 && at < 0x990) && !(at >= 0x9A0 && at < 0x9AC) && !gate_byte(at))
            return false;
    }
    return true;
}

static const uint8_t *reg_byte(const lcd_state_t *s, uint32_t off)
{
    if (off >= 0xC00 && off < 0xC50)
        return s->panel + off - 0xC00;
    if (off >= 0x980 && off < 0x9AC)
        return s->backlight + off - 0x980;
    return s->gates + off - 0x1100;
}

void lcd_init(lcd_state_t *s)
{
    /* Cold storage, not the former post-WinCE snapshot. Undocumented POR
     * constants and timing-field meanings are deliberately not synthesized. */
    memset(s, 0, sizeof(*s));
}

uint64_t lcd_read(const lcd_state_t *s, uint32_t off, unsigned len)
{
    uint64_t value = 0;
    if (!lcd_handles(off, len)) return 0;
    for (unsigned i = 0; i < len; i++)
        value |= (uint64_t)*reg_byte(s, off + i) << (i * 8);
    return value;
}

void lcd_write(lcd_state_t *s, uint32_t off, unsigned len, uint64_t value)
{
    if (!lcd_handles(off, len)) return;
    uint32_t old_pins = (uint32_t)lcd_read(s, 0xC4C, 4);
    for (unsigned i = 0; i < len; i++)
        *(uint8_t *)reg_byte(s, off + i) = (uint8_t)(value >> (i * 8));
    if (off < 0xC50 && off + len > 0xC4C) {
        uint32_t pins = (uint32_t)lcd_read(s, 0xC4C, 4);
        /* ddi.dll 01A56A5C: data bit 2, rising clock bit 1,
         * rising latch bit 0; eight bits MSB first. */
        if ((pins & 2) && !(old_pins & 2)) {
            s->contrast_shift = (uint8_t)((s->contrast_shift << 1) | ((pins >> 2) & 1));
            if (s->contrast_bits < 8) s->contrast_bits++;
        }
        if ((pins & 1) && !(old_pins & 1)) {
            if (s->contrast_bits == 8) {
                s->contrast_command = s->contrast_shift;
                s->contrast_valid = true;
            }
            s->contrast_bits = 0;
        }
    }
}

bool lcd_scanout_enabled(const lcd_state_t *s)
{
    /* SPL/OAL end at C48=030F; both disable paths clear these bits.
     * Observe enable state without enforcing a particular write history,
     * parameter table or guessed settle time. */
    return (lcd_read(s, 0xC48, 4) & 0x030F) == 0x030F &&
        (lcd_read(s, 0x1100, 4) & 1) && (lcd_read(s, 0x110C, 4) & 1);
}

bool lcd_backlight_enabled(const lcd_state_t *s)
{
    /* ddi.dll 01A55900: off clears BLG control and CMM bit 0.
     * Duty zero means maximum brightness, not power-off. */
    return (lcd_read(s, 0x980, 4) & 1) && (lcd_read(s, 0x1128, 4) & 1);
}

bool lcd_buzzer_signal(const lcd_state_t *s, uint32_t *period, uint32_t *duty)
{
    *period = ((uint32_t)lcd_read(s,0x9A4,4) & 0xffff) + 1;
    *duty = ((uint32_t)lcd_read(s,0x9A8,4) & 0xffff) + 1;
    return (lcd_read(s,0x9A0,4) & 1) && (lcd_read(s,0x1128,4) & 1) &&
        (lcd_read(s,0x984,4) & 0x0202) == 0x0202 && *duty < *period;
}

void lcd_copy_rgb565(const lcd_state_t *s, const uint16_t *src, uint16_t *dst,
    size_t width, size_t height, size_t stride)
{
    if (!lcd_scanout_enabled(s) || !lcd_backlight_enabled(s)) {
        memset(dst, 0, width * height * sizeof(*dst));
        return;
    }
    for (size_t y = 0; y < height; y++)
        memcpy(dst + y * width, src + y * stride, width * sizeof(*dst));
}

void lcd_copy_rgba8888(const lcd_state_t *s, const uint16_t *src, uint8_t *dst,
    size_t width, size_t height, size_t stride)
{
    bool visible = lcd_scanout_enabled(s) && lcd_backlight_enabled(s);
    for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < width; x++) {
            uint16_t pixel = visible ? src[y * stride + x] : 0;
            unsigned r = (pixel >> 11) & 31, g = (pixel >> 5) & 63, b = pixel & 31;
            size_t at = (y * width + x) * 4;
            dst[at] = (uint8_t)((r << 3) | (r >> 2));
            dst[at + 1] = (uint8_t)((g << 2) | (g >> 4));
            dst[at + 2] = (uint8_t)((b << 3) | (b >> 2));
            dst[at + 3] = 255;
        }
    }
}
