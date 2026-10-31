/* Digital LCD behavior recovered from WinCE SPL/OAL and ddi.dll.
 * No guest binaries or timing assumptions; optics only via the approximate curve. */
#include "hw/lcd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static void enable(lcd_state_t *s)
{
    lcd_write(s, 0x1100, 4, 1);
    lcd_write(s, 0x110C, 4, 1);
    lcd_write(s, 0x1128, 4, 1);
    lcd_write(s, 0x980, 4, 1);
    lcd_write(s, 0xC48, 4, 0x30F);
}

static void serial(lcd_state_t *s, unsigned command, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        unsigned pins = 0xA5000000 | (((command >> (7 - i)) & 1) << 2);
        lcd_write(s, 0xC4C, 4, pins);
        lcd_write(s, 0xC4C, 1, pins | 2);
    }
    lcd_write(s, 0xC4C, 1, 0);
    lcd_write(s, 0xC4C, 1, 1);
    lcd_write(s, 0xC4C, 1, 0);
}

int main(void)
{
    lcd_state_t s;
    lcd_init(&s);
    CHECK(!lcd_scanout_enabled(&s) && !lcd_backlight_enabled(&s));
    CHECK(lcd_read(&s, 0xC48, 4) == 0 && !s.contrast_valid);
    CHECK(lcd_handles(0xC00, 8) && lcd_handles(0x98F, 1));
    CHECK(!lcd_handles(0xC4F, 2) && !lcd_handles(0x990, 1));
    CHECK(!lcd_handles(0x1103, 2) && !lcd_handles(0x1110, 1));
    CHECK(!lcd_handles(UINT32_MAX, 4) && !lcd_handles(0xC00, 0));
    lcd_write(&s, 0xC00, 8, UINT64_C(0xFEDCBA9876543210));
    CHECK(lcd_read(&s, 0xC00, 8) == UINT64_C(0xFEDCBA9876543210));
    CHECK(lcd_read(&s, 0xC01, 2) == 0x5432);
    lcd_write(&s, 0xC02, 2, 0x1234);
    CHECK(lcd_read(&s, 0xC00, 4) == 0x12343210);
    lcd_write(&s, 0xC4F, 2, 0xFFFF); /* out-of-bank access is not split */
    CHECK(lcd_read(&s, 0xC4F, 1) == 0);
    enable(&s); /* parameters are storage, not a hardcoded boot whitelist */
    CHECK(lcd_scanout_enabled(&s) && lcd_backlight_enabled(&s));
    for (unsigned bit = 1; bit <= 0x200; bit <<= 1) {
        if (!(bit & 0x30F)) continue;
        lcd_write(&s, 0xC48, 4, 0x30F & ~bit);
        CHECK(!lcd_scanout_enabled(&s));
    }
    enable(&s);
    const unsigned gates[] = {0x1100, 0x110C};
    for (unsigned i = 0; i < 2; i++) {
        lcd_write(&s, gates[i], 4, 0xFE);
        CHECK(!lcd_scanout_enabled(&s));
        lcd_write(&s, gates[i], 1, 0xFF);
        CHECK(lcd_scanout_enabled(&s));
    }
    serial(&s, 0xC1, 7);
    CHECK(!s.contrast_valid);
    serial(&s, 0xC1, 8);
    CHECK(s.contrast_valid && s.contrast_command == 0xC1);
    CHECK(lcd_read(&s, 0xC4C, 4) == 0xA5000000);
    lcd_write(&s, 0xC4F, 1, 0xFF);
    CHECK(s.contrast_bits == 0 && s.contrast_command == 0xC1);
    serial(&s, 0xAB, 8);
    CHECK(s.contrast_command == 0xAB);
    serial(&s, 0xC1, 8); /* nominal contrast presents pixels unchanged */

    /* Two rows with padding sentinels, checking both frontend formats. */
    const uint16_t src[] = {0xF800, 0x07E0, 0x001F, 0xDEAD,
                            0xFFFF, 0, 0x8410, 0xBEEF};
    uint16_t original[8], rgb[6];
    uint8_t rgba[24];
    memcpy(original, src, sizeof(src));
    lcd_copy_rgb565(&s, src, rgb, 3, 2, 4);
    CHECK(rgb[0] == 0xF800 && rgb[2] == 0x001F && rgb[3] == 0xFFFF && rgb[5] == 0x8410);
    lcd_copy_rgba8888(&s, src, rgba, 3, 2, 4);
    const uint8_t expected[] = {255,0,0,255, 0,255,0,255, 0,0,255,255,
                                255,255,255,255, 0,0,0,255, 132,130,132,255};
    CHECK(!memcmp(rgba, expected, sizeof(expected)));
    const unsigned off_regs[] = {0xC48, 0x1100, 0x110C, 0x980, 0x1128};
    for (unsigned i = 0; i < 5; i++) {
        enable(&s);
        lcd_write(&s, off_regs[i], 4, 0);
        lcd_copy_rgb565(&s, src, rgb, 3, 2, 4);
        lcd_copy_rgba8888(&s, src, rgba, 3, 2, 4);
        for (unsigned j = 0; j < 6; j++) {
            CHECK(rgb[j] == 0);
            CHECK(!rgba[j*4] && !rgba[j*4+1] && !rgba[j*4+2] && rgba[j*4+3] == 255);
        }
        CHECK(!memcmp(src, original, sizeof(src)));
    }
    enable(&s);
    lcd_write(&s, 0x98C, 4, 0); /* zero duty = maximum, not off */
    lcd_copy_rgb565(&s, src, rgb, 3, 2, 4);
    CHECK(rgb[0] == 0xF800 && rgb[3] == 0xFFFF);
    /* Approximate brightness: stock duty table dims in order, black stays black. */
    const uint32_t duties[] = {0x3D5, 0x7AC, 0x9F9, 0xD0B};
    unsigned last = 256;
    lcd_write(&s, 0x988, 4, 0xF70);
    for (unsigned i = 0; i < 4; i++) {
        lcd_write(&s, 0x98C, 4, duties[i]);
        lcd_copy_rgba8888(&s, src, rgba, 3, 2, 4);
        CHECK(rgba[12] < last && rgba[12] > 0 && rgba[16] == 0 && rgba[15] == 255);
        last = rgba[12];
        lcd_copy_rgb565(&s, src, rgb, 3, 2, 4);
        CHECK(rgb[3] < 0xFFFF && rgb[4] == 0);
    }
    CHECK(!memcmp(src, original, sizeof(src)));
    /* Approximate contrast: a higher level (lower command) spreads grey from mid. */
    lcd_write(&s, 0x98C, 4, 0);
    const uint16_t grey[] = {0x4208, 0xC618}; /* below and above mid-grey */
    uint8_t lo[8], hi[8];
    serial(&s, 0xAB, 8);
    lcd_copy_rgba8888(&s, grey, hi, 2, 1, 2);
    serial(&s, 0xD0, 8);
    lcd_copy_rgba8888(&s, grey, lo, 2, 1, 2);
    CHECK(hi[0] < 66 && hi[4] > 198 && lo[0] > 66 && lo[4] < 198);
    serial(&s, 0xC1, 8);
    lcd_copy_rgba8888(&s, grey, lo, 2, 1, 2);
    CHECK(lo[0] == 66 && lo[4] == 198);
    lcd_init(&s);
    CHECK(!lcd_scanout_enabled(&s) && !s.contrast_valid);
    uint32_t period,duty;
    CHECK(!lcd_buzzer_signal(&s,&period,&duty));
    CHECK(lcd_handles(0x9A0,8) && lcd_handles(0x9AB,1) && !lcd_handles(0x9AB,2));
    /* Backlight enable and its period are not buzzer triggers. */
    enable(&s);lcd_write(&s,0x984,4,0x202);lcd_write(&s,0x988,4,0xf70);
    CHECK(!lcd_buzzer_signal(&s,&period,&duty));
    lcd_write(&s,0x9A4,4,1411);lcd_write(&s,0x9A8,4,705);lcd_write(&s,0x9A0,4,1);
    CHECK(lcd_buzzer_signal(&s,&period,&duty) && period==1412 && duty==706);
    lcd_write(&s,0x980,4,0);lcd_write(&s,0xC48,4,0);
    CHECK(lcd_buzzer_signal(&s,&period,&duty)); /* separate output */
    lcd_write(&s,0x1128,4,0);CHECK(!lcd_buzzer_signal(&s,&period,&duty));
    lcd_write(&s,0x1128,4,1);lcd_write(&s,0x984,4,2);CHECK(!lcd_buzzer_signal(&s,&period,&duty));
    lcd_write(&s,0x984,4,0x202);CHECK(lcd_buzzer_signal(&s,&period,&duty));
    lcd_write(&s,0x9A0,2,0);CHECK(!lcd_buzzer_signal(&s,&period,&duty));
    puts("LCD gates, serial contrast, stride and shared presentation passed");
    return 0;
}
