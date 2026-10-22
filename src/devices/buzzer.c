/*
 *  src/devices/buzzer.c — the Casio buzzer registers in the VRC4173 latch
 *  (BLG block at 0x0980, CMM word at 0x1128; Casio SDK buzzer.h ranges,
 *  hardware.txt:186-188), turned into host buzzer pulses through the UI.
 *  Split out of src/be300_devices.c.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"

#include "vrc4173_latch.h"
#include "ui.h"

static uint32_t be300_buzzer_peek_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

void be300_buzzer_seed(struct be300_vrc4173_latch *d)
{
    /*
     * Real-hardware idle values:
     *   docs/hardware/hw_dump_vrc4173.txt:1084 -> BlgReg first 4 words
     *   docs/hardware/hw_dump_vrc4173.txt:48   -> CmmReg word at 0x1128
     */
    static const struct {
        uint16_t off;
        uint32_t val;
    } blg_seed[] = {
        { 0x0000u, 0x00000001u },
        { 0x0004u, 0x00000202u },
        { 0x0008u, 0x00000F70u },
        { 0x000Cu, 0x00000000u },
    };
    uint32_t cmm = 0x00000001u;

    if (!d)
        return;

    for (unsigned i = 0; i < sizeof(blg_seed) / sizeof(blg_seed[0]); i++) {
        uint16_t off = blg_seed[i].off;
        uint32_t v = blg_seed[i].val;
        if (off + 4u <= BE300_BUZZER_BLG_LEN) {
            memcpy(&d->buzzer.blg[off], &v, 4);
            memcpy(&d->bytes[BE300_BUZZER_BLG_OFF + off], &v, 4);
        }
    }

    memcpy(d->buzzer.cmm, &cmm, sizeof(cmm));
    memcpy(&d->bytes[BE300_BUZZER_CMM_OFF], &cmm, sizeof(cmm));
}

static bool be300_buzzer_range_overlap(uint32_t off, unsigned len,
    uint32_t range_off, uint32_t range_len)
{
    uint64_t a0 = off;
    uint64_t a1 = a0 + len;
    uint64_t b0 = range_off;
    uint64_t b1 = b0 + range_len;

    return len > 0 && a0 < b1 && b0 < a1;
}

static uint32_t be300_buzzer_tone_hz(const struct be300_buzzer_state *b)
{
    uint32_t period = be300_buzzer_peek_le32(&b->blg[0x0008]) & 0xffffu;
    uint32_t hz;

    if (period == 0)
        return 1200;

    /*
     * docs/hardware/hw_dump_vrc4173.txt:1084 captures 0x0F70 in the
     * timing register; treating it as a 4 MHz divisor yields roughly
     * 1 kHz, consistent with a piezo notification tone. This conversion
     * only affects host audio rendering, not guest-visible register values.
     */
    hz = 4000000u / period;
    if (hz < 80)
        hz = 80;
    if (hz > 6000)
        hz = 6000;
    return hz;
}

static uint32_t be300_buzzer_tone_ms(const struct be300_buzzer_state *b)
{
    uint32_t cfg = be300_buzzer_peek_le32(&b->blg[0x0004]);
    uint32_t ms = 45u + ((cfg & 0xffu) * 8u);

    if (ms < 35)
        ms = 35;
    if (ms > 220)
        ms = 220;
    return ms;
}

void be300_buzzer_note_write(struct be300_vrc4173_latch *d,
    uint32_t off, unsigned len, const unsigned char *data)
{
    struct be300_buzzer_state *b;
    uint32_t old_control;
    uint32_t new_control;
    bool touched_blg;
    bool touched_cmm;

    if (!d || !data || len == 0)
        return;

    touched_blg = be300_buzzer_range_overlap(off, len,
        BE300_BUZZER_BLG_OFF, BE300_BUZZER_BLG_LEN);
    touched_cmm = be300_buzzer_range_overlap(off, len,
        BE300_BUZZER_CMM_OFF, BE300_BUZZER_CMM_LEN);
    if (!touched_blg && !touched_cmm)
        return;

    b = &d->buzzer;
    old_control = be300_buzzer_peek_le32(&b->blg[0]);

    for (unsigned i = 0; i < len; i++) {
        uint32_t byte_off = off + i;

        if (byte_off >= BE300_BUZZER_BLG_OFF &&
            byte_off < BE300_BUZZER_BLG_OFF + BE300_BUZZER_BLG_LEN) {
            uint32_t rel = byte_off - BE300_BUZZER_BLG_OFF;
            b->blg[rel] = data[i];
        }
        if (byte_off >= BE300_BUZZER_CMM_OFF &&
            byte_off < BE300_BUZZER_CMM_OFF + BE300_BUZZER_CMM_LEN) {
            uint32_t rel = byte_off - BE300_BUZZER_CMM_OFF;
            b->cmm[rel] = data[i];
        }
    }

    new_control = be300_buzzer_peek_le32(&b->blg[0]);

    /*
     * Two strobe patterns are used by ce_buzzer.dll on real hardware:
     *   - Clear-then-set: BLG[0] goes 0 -> 1 (caught by the edge arm below).
     *   - Set-while-latched: BLG[0] stays at 1 and the driver just rewrites
     *     the tone-config words (0x0984, 0x0988, 0x09A0, 0x09A4, 0x09A8).
     * Writes confined to the 0x098C divider word are tone-shape adjustments
     * mid-beep and must not retrigger. The 120ms gate inside ui_buzzer_pulse
     * consolidates the burst of config writes per beep into one host tone.
     */
    if ((new_control & 1u) && !(old_control & 1u)) {
        ui_buzzer_pulse(be300_buzzer_tone_hz(b), be300_buzzer_tone_ms(b));
        return;
    }
    if ((new_control & 1u) && touched_blg &&
        !be300_buzzer_range_overlap(off, len, 0x098Cu, 4u)) {
        ui_buzzer_pulse(be300_buzzer_tone_hz(b), be300_buzzer_tone_ms(b));
        return;
    }
}
