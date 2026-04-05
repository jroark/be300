#include <stdio.h>
#include "rtc.h"

static inline void rtc_update_elapsed_irq(rtc_state_t *s)
{
    if (s->etime < s->ecmp)
        return;
    if (s->elapsed_compare_fired)
        return;

    s->rtcint |= RTCINT_ELAPSEDTIME_INT;
    s->elapsed_compare_fired = 1;
}

static inline void rtc_rearm_elapsed_irq(rtc_state_t *s)
{
    s->elapsed_compare_fired = 0;
    rtc_update_elapsed_irq(s);
}

static inline uint64_t rtc_48bit_mask(void)
{
    return UINT64_C(0x0000FFFFFFFFFFFF);
}

static void rtc_write_48bit(uint64_t *target, uint32_t offset, unsigned size,
    uint32_t val)
{
    uint64_t reg = *target;

    switch (offset) {
    case RTC_ETIMELREG:
    case RTC_ECMPLREG:
        if (size >= 4) {
            reg = (reg & ~UINT64_C(0xFFFFFFFF))
                | (uint64_t)val;
        } else {
            reg = (reg & ~UINT64_C(0xFFFF))
                | (uint64_t)(val & 0xFFFFu);
        }
        break;
    case RTC_ETIMEMREG:
    case RTC_ECMPMREG:
        if (size >= 4) {
            reg = (reg & ~(UINT64_C(0xFFFFFFFF) << 16))
                | ((uint64_t)val << 16);
        } else {
            reg = (reg & ~(UINT64_C(0xFFFF) << 16))
                | ((uint64_t)(val & 0xFFFFu) << 16);
        }
        break;
    case RTC_ETIMEHREG:
    case RTC_ECMPHREG:
        reg = (reg & ~(UINT64_C(0xFFFF) << 32))
            | ((uint64_t)(val & 0xFFFFu) << 32);
        break;
    default:
        return;
    }

    *target = reg & rtc_48bit_mask();
}

void rtc_init(rtc_state_t *s)
{
    /*
     * WORKAROUND: Seed RTC elapsed-time counter from a warm-state
     * hardware dump (hardware_survey/HardwareDump6.txt).
     *
     * On real hardware after a cold boot the RTC elapsed-time counter
     * holds whatever value it accumulated before power was removed
     * (the RTC is battery-backed and keeps running).  A truly cold
     * device (battery removed for hours) would have a low or wrapped
     * ETIME.  We seed with a mid-range value observed from a running
     * device so that the kernel's elapsed-time driver doesn't see
     * an extreme value.  The specific value chosen is:
     *   ETIMEH=0xA5B3  ETIMEM=0x5149  ETIMEL=0xBB10
     */
    s->etime = UINT64_C(0xA5B35149BB10);  /* WORKAROUND: warm-state seed */
    s->etime_latched = s->etime;
    /*
     * Initialise ECMP above the starting ETIME so the elapsed-time
     * compare interrupt does not fire before the kernel programs ECMP.
     * Linux 2.6 writes all 48 bits via write_elapsedtime_compare();
     * Linux 2.4 uses RTCL1 instead and never touches ECMP.
     */
    s->ecmp  = s->etime + UINT64_C(0x100000000);
    /*
     * WORKAROUND: Seed RTCL1/RTCL2 from warm-state survey values
     * at VR4131 address 0x0F000110.
     *
     *   RTCL1LREG = 0x0021 (timer interval, stable across captures)
     *   RTCL2LREG = 0xFFFF (timer interval, stable across captures)
     *
     * The counter halves vary across captures, so those are derived
     * from ETIME at runtime rather than seeded with a fixed value.
     * On a truly cold boot these would be 0 until programmed by the
     * kernel.
     */
    s->rtcl1 = 0x00000021u;  /* WORKAROUND: warm-state seed */
    s->rtcl2 = 0x0000FFFFu;  /* WORKAROUND: warm-state seed */
    s->tclock = 0;
    s->rtcint = 0;
    s->elapsed_compare_fired = 0;
}

uint32_t rtc_read(rtc_state_t *s, uint32_t offset, unsigned size)
{
    if (offset == RTC_ETIMELREG || offset == RTC_ETIMEMREG || offset == RTC_ETIMEHREG) {
        uint64_t snap = s->etime;
        uint32_t ret = 0;

        if (size >= 4) {
            if (offset == RTC_ETIMELREG) ret =  (uint32_t)(snap & 0xFFFFFFFFu);
            if (offset == RTC_ETIMEMREG) ret =  (uint32_t)((snap >> 16) & 0xFFFFFFFFu);
            if (offset == RTC_ETIMEHREG) ret =  (uint32_t)((snap >> 32) & 0xFFFFFFFFu);
        } else {
            if (offset == RTC_ETIMELREG) ret =  (uint32_t)(snap & 0xFFFF);
            if (offset == RTC_ETIMEMREG) ret =  (uint32_t)((snap >> 16) & 0xFFFF);
            if (offset == RTC_ETIMEHREG) ret =  (uint32_t)((snap >> 32) & 0xFFFF);
        }
        return ret;
    }

    switch (offset) {
    case RTC_ECMPLREG:  return  (uint32_t)(s->ecmp         & 0xFFFF);
    case RTC_ECMPMREG:  return  (uint32_t)((s->ecmp  >> 16) & 0xFFFF);
    case RTC_ECMPHREG:  return  (uint32_t)((s->ecmp  >> 32) & 0xFFFF);
    case RTC_RTCL1LREG: return  s->rtcl1 & 0xFFFF;
    case RTC_RTCL1HREG: return (s->rtcl1 >> 16) & 0xFFFF;
    case RTC_RTCL1CNTLREG: return (uint32_t)(s->etime & 0xFFFF);
    case RTC_RTCL1CNTHREG: return (uint32_t)((s->etime >> 16) & 0xFFFF);
    case RTC_RTCL2LREG: return  s->rtcl2 & 0xFFFF;
    case RTC_RTCL2HREG: return (s->rtcl2 >> 16) & 0xFFFF;
    case RTC_RTCL2CNTLREG: return (uint32_t)(s->etime & 0xFFFF);
    case RTC_RTCL2CNTHREG: return (uint32_t)((s->etime >> 16) & 0xFFFF);
    case RTC_TCLKLREG: return s->tclock & 0xFFFF;
    case RTC_TCLKHREG: return (s->tclock >> 16) & 0xFFFF;
    case RTC_TCLKCNTLREG: return (uint32_t)(s->etime & 0xFFFF);
    case RTC_TCLKCNTHREG: return (uint32_t)((s->etime >> 16) & 0xFFFF);
    case RTC_RTCINTREG: return s->rtcint;
    case 0x28:
    case 0x2A:
    case 0x2C:
    case 0x2E:
    case 0x30:
    case 0x32:
    case 0x34:
    case 0x36:
    case 0x38:
    case 0x3A:
    case 0x3C:
        return 0; /* RFU in RTC2 window on VR4131 */
    default:
        fprintf(stderr, "[RTC] Unhandled read offset 0x%02X\n", offset);
        return 0;
    }
}

void rtc_write(rtc_state_t *s, uint32_t offset, unsigned size, uint32_t val)
{
    switch (offset) {
    case RTC_ETIMELREG:
    case RTC_ETIMEMREG:
    case RTC_ETIMEHREG:
        rtc_write_48bit(&s->etime, offset, size, val);
        s->etime_latched = s->etime;
        rtc_update_elapsed_irq(s);
        break;
    case RTC_ECMPLREG:
        rtc_write_48bit(&s->ecmp, offset, size, val);
        rtc_rearm_elapsed_irq(s);
        break;
    case RTC_ECMPMREG:
    case RTC_ECMPHREG:
        rtc_write_48bit(&s->ecmp, offset, size, val);
        rtc_rearm_elapsed_irq(s);
        break;
    case RTC_RTCL1LREG:
        s->rtcl1 = (s->rtcl1 & 0xFFFF0000) | (val & 0xFFFF);
        break;
    case RTC_RTCL1HREG:
        s->rtcl1 = (s->rtcl1 & 0x0000FFFF) | ((val & 0xFFFF) << 16);
        break;
    case RTC_RTCL2LREG:
        s->rtcl2 = (s->rtcl2 & 0xFFFF0000) | (val & 0xFFFF);
        break;
    case RTC_RTCL2HREG:
        s->rtcl2 = (s->rtcl2 & 0x0000FFFF) | ((val & 0xFFFF) << 16);
        break;
    case RTC_TCLKLREG:
        s->tclock = (s->tclock & 0xFFFF0000u) | (val & 0xFFFFu);
        break;
    case RTC_TCLKHREG:
        s->tclock = (s->tclock & 0x0000FFFFu) | ((val & 0xFFFFu) << 16);
        break;
    case RTC_RTCINTREG:
        /* Write-one-to-clear interrupt status bits. */
        s->rtcint &= (uint16_t)~val;
        break;
    case 0x28:
    case 0x2A:
    case 0x2C:
    case 0x2E:
    case 0x30:
    case 0x32:
    case 0x34:
    case 0x36:
    case 0x38:
    case 0x3A:
    case 0x3C:
        break; /* RFU */
    default:
        fprintf(stderr, "[RTC] Unhandled write offset 0x%02X = 0x%04X\n",
                offset, val);
        break;
    }
}

void rtc_tick(rtc_state_t *s, uint64_t ticks)
{
    s->etime += ticks;
    rtc_update_elapsed_irq(s);
}
