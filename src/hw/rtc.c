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

void rtc_init(rtc_state_t *s, bool warm)
{
    s->tclock = 0;
    s->rtcint = 0;
    s->elapsed_compare_fired = 0;

    if (warm) {
        /* Warm start (Linux): seed from hardware survey */
        s->etime = UINT64_C(0xA5B35149BB10);
        s->etime_latched = s->etime;
        s->ecmp  = s->etime + UINT64_C(0x100000000);
        s->rtcl1 = 0x00000021u;
        s->rtcl2 = 0x0000FFFFu;
    } else {
        /* Cold boot: no battery-backed RTC, all zero */
        s->etime = 0;
        s->etime_latched = 0;
        s->ecmp  = 0;
        s->rtcl1 = 0;
        s->rtcl2 = 0;
    }
}

uint32_t rtc_read(rtc_state_t *s, uint32_t offset, unsigned size)
{
    if (offset == RTC_ETIMELREG || offset == RTC_ETIMEMREG || offset == RTC_ETIMEHREG) {
        static int etime_read_count = 0;
        uint64_t snap = s->etime;
        etime_read_count++;
        if (offset == RTC_ETIMELREG &&
            (etime_read_count <= 3 || (etime_read_count & 0xFFFFF) == 0)) {
            fprintf(stderr, "[RTC_ETIME_RD] etime=0x%llX size=%u #%d\n",
                    (unsigned long long)snap, size, etime_read_count);
        }
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
    s->rtcl1 += (uint32_t)ticks;
    rtc_update_elapsed_irq(s);
}
