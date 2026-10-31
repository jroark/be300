/* BE-300 battery model. See battery.h. */
#include "hw/battery.h"

#include <string.h>

void battery_init(battery_state_t *s, unsigned pct, battery_mode_t mode,
    unsigned drain_pct_per_min)
{
    memset(s, 0, sizeof(*s));
    s->mode = mode;
    s->milli_pct = (pct > 100 ? 100 : pct) * 1000u;
    s->drain_milli_per_min = drain_pct_per_min * 1000u;
}

void battery_advance(battery_state_t *s, uint64_t now_ns)
{
    uint64_t delta;

    if (!s->started || now_ns < s->last_ns) {
        s->started = true;
        s->last_ns = now_ns;
        return;
    }
    if (!s->drain_milli_per_min || s->mode == BATTERY_AC) {
        s->last_ns = now_ns;
        return;
    }
    /* Whole milli-percent steps only, keeping the remainder in last_ns. */
    uint64_t ns_per_step = 60000000000ull / s->drain_milli_per_min;
    delta = (now_ns - s->last_ns) / ns_per_step;
    if (!delta)
        return;
    s->last_ns += delta * ns_per_step;
    if (s->mode == BATTERY_DISCHARGING) {
        s->milli_pct = delta >= s->milli_pct ? 0 : s->milli_pct - (uint32_t)delta;
    } else if (delta >= 100000u - s->milli_pct) {
        s->milli_pct = 100000u;
        s->mode = BATTERY_AC;
    } else {
        s->milli_pct += (uint32_t)delta;
    }
}

uint16_t battery_gpio(const battery_state_t *s, uint16_t pins)
{
    pins &= (uint16_t)~(BATTERY_GPIO_ON_BATTERY | BATTERY_GPIO_CHARGING);
    if (s->mode == BATTERY_DISCHARGING)
        pins |= BATTERY_GPIO_ON_BATTERY;
    else if (s->mode == BATTERY_CHARGING)
        pins |= BATTERY_GPIO_CHARGING;
    return pins;
}

/* The inverse of Windows CE's scale (battery.h): piecewise linear through
 * 0 % at 3.4 V and its band edges, 10, 25, 50, 75 and 100 %. */
uint16_t battery_adc(const battery_state_t *s)
{
    int32_t span = BATTERY_CAL_MAX - BATTERY_CAL_MIN;
    const int32_t at[6] = {
        BATTERY_CAL_MIN - span * 3 / 5,         /* gwes 0xB0CC0: cut-off */
        BATTERY_CAL_MIN - span / 10,            /* 10 % edge */
        BATTERY_CAL_MIN,                        /* 25 % */
        BATTERY_CAL_MAX - span * 4 / 5,         /* 50 % */
        BATTERY_CAL_MAX - span * 2 / 5,         /* 75 % */
        BATTERY_CAL_MAX,                        /* 100 % */
    };
    static const int32_t milli[6] = { 0, 10000, 25000, 50000, 75000, 100000 };
    int32_t m = (int32_t)s->milli_pct;

    for (int i = 1; i < 6; i++)
        if (m <= milli[i])
            return (uint16_t)(at[i - 1] + ((at[i] - at[i - 1]) * (m - milli[i - 1]) +
                              (milli[i] - milli[i - 1]) / 2) / (milli[i] - milli[i - 1]));
    return BATTERY_CAL_MAX;
}

unsigned battery_pct(const battery_state_t *s)
{
    return s->milli_pct / 1000u;
}

bool battery_parse_mode(const char *name, battery_mode_t *mode)
{
    if (!strcmp(name, "ac"))
        *mode = BATTERY_AC;
    else if (!strcmp(name, "charging"))
        *mode = BATTERY_CHARGING;
    else if (!strcmp(name, "battery"))
        *mode = BATTERY_DISCHARGING;
    else
        return false;
    return true;
}
