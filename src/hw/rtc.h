#pragma once
#include <stdint.h>

/*
 * RTC — Real-Time Clock / Elapsed Time Unit (VR4131 hardware manual §13)
 *
 * The VR4131 RTC provides a 48-bit elapsed-time counter (ETIME) that
 * increments at the RTC clock frequency (~32.768 kHz or 1 Hz depending
 * on configuration), plus a comparison register pair for alarms.
 *
 * We return simulated time starting from 0 at machine boot.
 */

/* Register offsets from RTC base (0x0F000100) */
#define RTC_ETIMELREG   0x00u   /* Elapsed time low  (bits 15:0)   */
#define RTC_ETIMEMREG   0x02u   /* Elapsed time mid  (bits 31:16)  */
#define RTC_ETIMEHREG   0x04u   /* Elapsed time high (bits 47:32)  */
#define RTC_ECMPLREG    0x08u   /* Compare low                     */
#define RTC_ECMPMREG    0x0Au   /* Compare mid                     */
#define RTC_ECMPHREG    0x0Cu   /* Compare high                    */
#define RTC_RTCL1LREG   0x10u   /* RTC long-1 low  (calendar)      */
#define RTC_RTCL1HREG   0x12u   /* RTC long-1 high                 */
#define RTC_RTCL1CNTLREG 0x14u  /* RTC long-1 counter low          */
#define RTC_RTCL1CNTHREG 0x16u  /* RTC long-1 counter high         */
#define RTC_RTCL2LREG   0x18u   /* RTC long-2 low                  */
#define RTC_RTCL2HREG   0x1Au   /* RTC long-2 high                 */
#define RTC_RTCL2CNTLREG 0x1Cu  /* RTC long-2 counter low          */
#define RTC_RTCL2CNTHREG 0x1Eu  /* RTC long-2 counter high         */

/* RTC2 block (base + 0x20, i.e. PA 0x0F000120) */
#define RTC_TCLKLREG     0x20u
#define RTC_TCLKHREG     0x22u
#define RTC_TCLKCNTLREG  0x24u
#define RTC_TCLKCNTHREG  0x26u
#define RTC_RTCINTREG    0x3Eu

#define RTCINT_TCLOCK_INT      0x08u
#define RTCINT_RTCLONG2_INT    0x04u
#define RTCINT_RTCLONG1_INT    0x02u
#define RTCINT_ELAPSEDTIME_INT 0x01u

typedef struct {
    uint64_t etime;       /* simulated elapsed ticks              */
    uint64_t ecmp;        /* compare register                     */
    uint32_t rtcl1;       /* calendar long-1 (stub)               */
    uint32_t rtcl2;       /* calendar long-2 (stub)               */
    uint32_t tclock;      /* programmable tclock period (stub)    */
    uint16_t rtcint;      /* RTC interrupt status                 */
    uint8_t  elapsed_compare_fired; /* ECMP compare has fired since last ECMP write */
    uint64_t etime_latched;   /* stable snapshot across multi-read sequence */
    uint8_t  etime_reads;     /* number of ETIME register reads in snapshot */
    uint32_t etime_read_step; /* ticks added on each ETIME read (0 = disabled) */
} rtc_state_t;

void     rtc_init (rtc_state_t *s);
uint32_t rtc_read (rtc_state_t *s, uint32_t offset, unsigned size);
void     rtc_write(rtc_state_t *s, uint32_t offset, unsigned size, uint32_t val);

/* Advance elapsed time counter; call periodically from the machine loop */
void     rtc_tick (rtc_state_t *s, uint64_t ticks);
