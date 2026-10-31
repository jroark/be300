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
        return s->panel + (off - 0xC00);
    if (off >= 0x980 && off < 0x9AC)
        return s->backlight + (off - 0x980);
    return s->gates + (off - 0x1100);
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

static uint8_t lcd_level(int v, int k, int gain)
{
    int c = 128 + (v - 128) * k / 256;
    c = c < 0 ? 0 : c > 255 ? 255 : c;
    return (uint8_t)(c * gain / 256);
}

/* Approximate presentation of the backlight duty and serial contrast so
 * the controls are visible. The panel's optical response is unmeasured:
 * the 30% floor and 8% per contrast step are illustrative, not physical.
 * Fills 8-bit outputs for 5-bit (R/B) and 6-bit (G) inputs; returns false
 * at full backlight and nominal contrast (stock level 5, command C1). */
static bool lcd_levels(const lcd_state_t *s, uint8_t c5[32], uint8_t c6[64])
{
    uint32_t period = (uint32_t)lcd_read(s, 0x988, 4) & 0xffff;
    uint32_t duty = (uint32_t)lcd_read(s, 0x98C, 4) & 0xffff;
    int gain = 256, k = 256;
    /* ddi.dll 01A52050: a larger duty word is a dimmer backlight. */
    if (duty && period)
        gain = duty >= period ? 77 : 77 + (int)(179 * (period - duty) / period);
    /* ddi.dll 01A56A5C sends 208 - 3 * level; lower commands raise contrast. */
    if (s->contrast_valid && s->contrast_command != 0xC1) {
        k = 256 + (0xC1 - (int)s->contrast_command) * 20 / 3;
        k = k < 0 ? 0 : k > 512 ? 512 : k;
    }
    if (gain == 256 && k == 256)
        return false;
    for (int i = 0; i < 32; i++)
        c5[i] = lcd_level((i << 3) | (i >> 2), k, gain);
    for (int i = 0; i < 64; i++)
        c6[i] = lcd_level((i << 2) | (i >> 4), k, gain);
    return true;
}

void lcd_copy_rgb565(const lcd_state_t *s, const uint16_t *src, uint16_t *dst,
    size_t width, size_t height, size_t stride)
{
    uint8_t c5[32], c6[64];
    if (!lcd_scanout_enabled(s) || !lcd_backlight_enabled(s)) {
        memset(dst, 0, width * height * sizeof(*dst));
        return;
    }
    if (!lcd_levels(s, c5, c6)) {
        for (size_t y = 0; y < height; y++)
            memcpy(dst + y * width, src + y * stride, width * sizeof(*dst));
        return;
    }
    for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < width; x++) {
            uint16_t pixel = src[y * stride + x];
            dst[y * width + x] = (uint16_t)((c5[(pixel >> 11) & 31] >> 3) << 11 |
                (c6[(pixel >> 5) & 63] >> 2) << 5 | c5[pixel & 31] >> 3);
        }
    }
}

void lcd_copy_rgba8888(const lcd_state_t *s, const uint16_t *src, uint8_t *dst,
    size_t width, size_t height, size_t stride)
{
    uint8_t c5[32], c6[64];
    bool visible = lcd_scanout_enabled(s) && lcd_backlight_enabled(s);
    if (!visible || !lcd_levels(s, c5, c6)) {
        for (int i = 0; i < 32; i++)
            c5[i] = visible ? (uint8_t)((i << 3) | (i >> 2)) : 0;
        for (int i = 0; i < 64; i++)
            c6[i] = visible ? (uint8_t)((i << 2) | (i >> 4)) : 0;
    }
    for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < width; x++) {
            uint16_t pixel = src[y * stride + x];
            size_t at = (y * width + x) * 4;
            dst[at] = c5[(pixel >> 11) & 31];
            dst[at + 1] = c6[(pixel >> 5) & 63];
            dst[at + 2] = c5[pixel & 31];
            dst[at + 3] = 255;
        }
    }
}
