/* Battery, serial EEPROM and PIU A/D port models (hw/battery.h,
 * hw/eeprom.h, hw/piu.h), driven the way Windows CE's drivers drive them. */

#include "hw/battery.h"
#include "hw/eeprom.h"
#include "hw/piu.h"

#include <stdio.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define MINUTE_NS 60000000000ull

static void test_battery_levels(void)
{
    battery_state_t b;
    uint32_t span = BATTERY_CAL_MAX - BATTERY_CAL_MIN;

    battery_init(&b, 100, BATTERY_AC, 0);
    CHECK(battery_adc(&b) == BATTERY_CAL_MAX);
    CHECK(battery_gpio(&b, 0xAAE2u) == 0xAAE2u);   /* CE's dump: AC, charged */

    battery_init(&b, 0, BATTERY_DISCHARGING, 0);
    CHECK(battery_adc(&b) == BATTERY_CAL_MIN - span * 3 / 5);
    battery_init(&b, 10, BATTERY_DISCHARGING, 0);
    CHECK(battery_adc(&b) == BATTERY_CAL_MIN - span / 10);
    battery_init(&b, 25, BATTERY_DISCHARGING, 0);
    CHECK(battery_adc(&b) == BATTERY_CAL_MIN);
    /* 40 %: between CE's 25 and 50 % edges, so CE shows 50. */
    battery_init(&b, 40, BATTERY_DISCHARGING, 0);
    CHECK(battery_adc(&b) > BATTERY_CAL_MIN &&
        battery_adc(&b) <= BATTERY_CAL_MAX - span * 4 / 5);
    CHECK(battery_gpio(&b, 0xAAE2u) == (0xAAE2u | BATTERY_GPIO_ON_BATTERY));

    battery_init(&b, 50, BATTERY_CHARGING, 0);
    CHECK(battery_gpio(&b, 0xAAE6u & ~BATTERY_GPIO_CHARGING) ==
        (0xAAE2u | BATTERY_GPIO_CHARGING));

    /* 30 % draining at 20 %/min: 10 % after a minute, empty after 1.5. */
    battery_init(&b, 30, BATTERY_DISCHARGING, 20);
    battery_advance(&b, 5 * MINUTE_NS);    /* starts the clock */
    battery_advance(&b, 6 * MINUTE_NS);
    CHECK(battery_pct(&b) == 10);
    battery_advance(&b, 8 * MINUTE_NS);
    CHECK(battery_pct(&b) == 0);

    /* Charging ends in AC (charged) at 100 %. */
    battery_init(&b, 90, BATTERY_CHARGING, 20);
    battery_advance(&b, 0);
    battery_advance(&b, MINUTE_NS);
    CHECK(battery_pct(&b) == 100 && b.mode == BATTERY_AC);

    battery_mode_t mode;
    CHECK(battery_parse_mode("battery", &mode) && mode == BATTERY_DISCHARGING);
    CHECK(!battery_parse_mode("full", &mode));
}

/* eeprom.dll's bus helpers (01A21510 START, 01A21574 bit out, 01A215D4 bit
 * in, 01A216FC STOP), without the delays. */
static void scl(eeprom_state_t *e, int v) { eeprom_set_scl(e, v != 0); }
static void sda(eeprom_state_t *e, int v) { eeprom_set_sda(e, v != 0); }

static void ce_start(eeprom_state_t *e)
{
    scl(e, 1); sda(e, 1); scl(e, 1); sda(e, 0); scl(e, 0); sda(e, 1);
}

static void ce_bit_out(eeprom_state_t *e, int bit)
{
    scl(e, 0); sda(e, bit); scl(e, 1); scl(e, 0);
}

static int ce_bit_in(eeprom_state_t *e)
{
    int v;
    scl(e, 0); sda(e, 1); scl(e, 1); v = eeprom_sda(e); scl(e, 0);
    return v;
}

static int ce_send(eeprom_state_t *e, uint8_t byte)   /* 0 when acknowledged */
{
    for (int bit = 7; bit >= 0; bit--)
        ce_bit_out(e, (byte >> bit) & 1);
    return ce_bit_in(e);
}

static uint8_t ce_recv(eeprom_state_t *e, int last)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (uint8_t)((v << 1) | ce_bit_in(e));
    ce_bit_out(e, last);
    return v;
}

static void ce_stop(eeprom_state_t *e)
{
    sda(e, 0); scl(e, 0); scl(e, 1); sda(e, 1);
}

static int ce_read(eeprom_state_t *e, uint8_t selector, uint8_t *buf, int n)
{
    int nack = 0;
    ce_start(e);
    nack |= ce_send(e, 0xA0);
    nack |= ce_send(e, (uint8_t)(selector * 2));
    ce_start(e);
    nack |= ce_send(e, 0xA1);
    for (int i = 0; i < n; i++)
        buf[i] = ce_recv(e, i == n - 1);
    ce_stop(e);
    return nack;
}

static void test_eeprom(void)
{
    eeprom_state_t e;
    uint8_t buf[4];

    eeprom_init(&e);
    CHECK(eeprom_sda(&e));
    e.mem[EEPROM_BATTERY_CAL] = 0xD9;
    e.mem[EEPROM_BATTERY_CAL + 1] = 0x7E;
    e.mem[EEPROM_BATTERY_CAL + 2] = 0x12;
    CHECK(ce_read(&e, 0x4A, buf, 3) == 0);
    CHECK(buf[0] == 0xD9 && buf[1] == 0x7E && buf[2] == 0x12);
    CHECK(e.phase == EEPROM_IDLE && eeprom_sda(&e));

    /* Erased by default; a read of another device gets no acknowledge. */
    eeprom_init(&e);
    CHECK(ce_read(&e, 0x4A, buf, 2) == 0 && buf[0] == 0xFF && buf[1] == 0xFF);
    ce_start(&e);
    CHECK(ce_send(&e, 0xA2) == 1);
    ce_stop(&e);

    /* A write (device, word address, data) lands in the array. */
    ce_start(&e);
    CHECK(ce_send(&e, 0xA0) == 0 && ce_send(&e, 0x10) == 0 && ce_send(&e, 0x5A) == 0);
    ce_stop(&e);
    CHECK(e.mem[0x10] == 0x5A);
}

/* gwes.exe 0xB0388-0xB0574, the battery sample, on the PIU model. */
static void test_piu_adport(void)
{
    piu_state_t p;
    uint16_t ctl, saved_int;

    piu_init(&p);
    p.adc_on = true;
    piu_write(&p, 0x04, 0xDD00u, 0);
    piu_write(&p, 0x00, 0x0306u, 0);        /* touch scan, waiting for the pen */
    CHECK((piu_read(&p, 0x00, 0) & 0x1C00u) == 0x1000u);

    p.aux_adc = 0x1CAB;                     /* 12 bits are kept */
    ctl = piu_read(&p, 0x00, 0);
    piu_write(&p, 0x00, (uint16_t)(ctl & ~0x0105u), 0);
    CHECK((piu_read(&p, 0x00, 0) & 0x1C00u) == 0x0400u);
    piu_write(&p, 0x14, 5, 0);
    piu_write(&p, 0x18, 0x14E, 0);
    ctl = piu_read(&p, 0x00, 0);
    piu_write(&p, 0x00, (uint16_t)(ctl | 0x8u), 0);
    saved_int = piu_read(&p, 0x04, 0);
    piu_write(&p, 0x04, (uint16_t)(saved_int & ~0x4100u), 0);
    piu_write(&p, 0x04, (uint16_t)(piu_read(&p, 0x04, 0) | 0x40u), 0);
    CHECK((piu_read(&p, 0x04, 0) & PIU_PENDING_ADPORT) == 0);
    ctl = piu_read(&p, 0x00, 0);
    piu_write(&p, 0x00, (uint16_t)(ctl | 0x4u), 0);
    CHECK((piu_read(&p, 0x04, 0) & PIU_PENDING_ADPORT) != 0);
    CHECK(piu_read(&p, PIU_ADPORT_OFF, 0) == 0x0CABu);
    CHECK(!piu_irq_pending(&p));            /* bit 6 was masked off */

    /* Restore: acknowledge, A/D mode off, mask back, scan restarted. */
    piu_write(&p, 0x00, (uint16_t)(piu_read(&p, 0x00, 0) & ~0x4u), 0);
    piu_write(&p, 0x04, (uint16_t)(piu_read(&p, 0x04, 0) | 0x40u), 0);
    piu_write(&p, 0x00, (uint16_t)(piu_read(&p, 0x00, 0) & ~0x8u), 0);
    piu_write(&p, 0x04, (uint16_t)(saved_int & ~0xFFu), 0);
    piu_write(&p, 0x00, (uint16_t)(piu_read(&p, 0x00, 0) | 0x104u), 0);
    CHECK((piu_read(&p, 0x04, 0) & 0xFFu) == 0);
    CHECK((piu_read(&p, 0x00, 0) & 0x1C00u) == 0x1000u);
}

int main(void)
{
    test_battery_levels();
    test_eeprom();
    test_piu_adport();
    if (failures) {
        fprintf(stderr, "test_battery: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_battery: OK");
    return 0;
}
