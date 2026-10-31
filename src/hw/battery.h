/* BE-300 battery, AC adapter and charger, as Windows CE's battery driver
 * (gwes.exe BatteryPDDGetStatus, 0xB02CC-0xB09DC) reads them:
 *
 *   GIUPIODL (0x0F000144) bit 12: 1 on battery, 0 with the AC adapter in.
 *   GIUPIODL bit 2, with AC:      1 charging, 0 charged.
 *   PIU A/D port (0x0A000390):    12-bit battery voltage; CE's default
 *                                 calibration is 0xBF8 = 3.7 V, 0xD96 = 4.2 V.
 *
 * Real dumps read 0xC6D..0xE22 there (docs/hardware/hw_dump_diffs.txt). The
 * level maps piecewise linearly onto CE's own scale: 0 % at 3.4 V (the
 * min - 3 * span / 5 edge gwes computes, the manual's cut-off), then 10, 25,
 * 50, 75 and 100 % at its band edges (3.65, 3.7, 3.8, 4.0, 4.2 V). CE then
 * shows the level rounded up to its next band. See docs/BATTERY.md. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BATTERY_AC,             /* adapter in, charged (CE: GPIO12 0, GPIO2 0) */
    BATTERY_CHARGING,       /* adapter in, charging (GPIO12 0, GPIO2 1) */
    BATTERY_DISCHARGING,    /* on battery (GPIO12 1) */
} battery_mode_t;

#define BATTERY_GPIO_ON_BATTERY 0x1000u     /* GIUPIODL bit 12 */
#define BATTERY_GPIO_CHARGING   0x0004u     /* GIUPIODL bit 2 */
#define BATTERY_CAL_MAX         0x0D96u     /* gwes 0xB0C08 */
#define BATTERY_CAL_MIN         0x0BF8u     /* gwes 0xB0C04 */

typedef struct {
    battery_mode_t mode;
    uint32_t milli_pct;         /* 0..100000 */
    uint32_t drain_milli_per_min;
    uint64_t last_ns;
    bool started;
} battery_state_t;

void battery_init(battery_state_t *s, unsigned pct, battery_mode_t mode,
    unsigned drain_pct_per_min);
/* Drains (on battery) or charges (CHARGING, at the same rate, ending in AC)
 * up to emulated time now_ns. */
void battery_advance(battery_state_t *s, uint64_t now_ns);
/* pins with bits 12 and 2 replaced by the battery state. */
uint16_t battery_gpio(const battery_state_t *s, uint16_t pins);
uint16_t battery_adc(const battery_state_t *s);
unsigned battery_pct(const battery_state_t *s);
bool battery_parse_mode(const char *name, battery_mode_t *mode);
