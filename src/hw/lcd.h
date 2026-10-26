/* Casio LCD digital controls. WinCE 3.0 SPL 80F034A0, OAL 80078F38,
 * ddi.dll 01A55994/01A56A5C. See docs/BE300_LCD.md. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t panel[0x50];       /* companion C00..C4F, including contrast pins */
    uint8_t backlight[0x10];   /* 980..98F: enable/config/period/duty */
    uint8_t gates[0x40];       /* only 1100, 110C and 1128 belong here */
    uint8_t contrast_shift, contrast_bits, contrast_command;
    bool contrast_valid;
} lcd_state_t;

void lcd_init(lcd_state_t *s);
bool lcd_handles(uint32_t off, unsigned len);
uint64_t lcd_read(const lcd_state_t *s, uint32_t off, unsigned len);
void lcd_write(lcd_state_t *s, uint32_t off, unsigned len, uint64_t value);
bool lcd_scanout_enabled(const lcd_state_t *s);
bool lcd_backlight_enabled(const lcd_state_t *s);
/* Blanking preserves guest VRAM; analog transfer curves are unmeasured. */
void lcd_copy_rgb565(const lcd_state_t *s, const uint16_t *src, uint16_t *dst,
    size_t width, size_t height, size_t stride);
void lcd_copy_rgba8888(const lcd_state_t *s, const uint16_t *src, uint8_t *dst,
    size_t width, size_t height, size_t stride);
