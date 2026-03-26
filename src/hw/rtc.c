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

void rtc_init(rtc_state_t *s)
{
    /*
     * Warm-state seed from hardware_survey/HardwareDump6.txt:
     * ETIMEH/ETIMEM/ETIMEL = 0xA5B3/0x5149/0xBB10.
     */
    s->etime = UINT64_C(0xA5B35149BB10);
    s->etime_latched = s->etime;
    s->etime_reads = 0;
    s->etime_read_step = 1;
    /*
     * Initialise ECMP above the starting ETIME so the elapsed-time
     * compare interrupt does not fire before the kernel programs ECMP.
     * Linux 2.6 writes all 48 bits via write_elapsedtime_compare();
     * Linux 2.4 uses RTCL1 instead and never touches ECMP.
     */
    s->ecmp  = s->etime + UINT64_C(0x100000000);
    s->rtcl1 = 0;
    s->rtcl2 = 0;
    s->tclock = 0;
    s->rtcint = 0;
    s->elapsed_compare_fired = 0;
}

uint32_t rtc_read(rtc_state_t *s, uint32_t offset, unsigned size)
{
    if (offset == RTC_ETIMELREG || offset == RTC_ETIMEMREG || offset == RTC_ETIMEHREG) {
        uint32_t step = s->etime_read_step;
        if (step != 0u) {
            /*
             * Optional read-assist for SPL polling loops.  When disabled
             * (step=0), ETIME advances only via rtc_tick().
             */
            s->etime += step;
            rtc_update_elapsed_irq(s);
        }

        /* Keep a stable snapshot across multiword read sequences. */
        if (s->etime_reads == 0)
            s->etime_latched = s->etime;

        uint64_t snap = s->etime_latched;
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

        s->etime_reads++;
        if (s->etime_reads >= 6)
            s->etime_reads = 0;
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
    (void)size;
    switch (offset) {
    /* ETIME is read-only on real hardware; accept writes as a no-op */
    case RTC_ETIMELREG:
    case RTC_ETIMEMREG:
    case RTC_ETIMEHREG:
        break;
    case RTC_ECMPLREG:
        s->ecmp = (s->ecmp & ~UINT64_C(0xFFFF))
                | ((uint64_t)(val & 0xFFFF));
        rtc_rearm_elapsed_irq(s);
        break;
    case RTC_ECMPMREG:
        s->ecmp = (s->ecmp & ~UINT64_C(0xFFFF0000))
                | ((uint64_t)(val & 0xFFFF) << 16);
        rtc_rearm_elapsed_irq(s);
        break;
    case RTC_ECMPHREG:
        s->ecmp = (s->ecmp & ~UINT64_C(0xFFFF00000000))
                | ((uint64_t)(val & 0xFFFF) << 32);
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
