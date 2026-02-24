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
#define RTC_RTCL2LREG   0x14u   /* RTC long-2 low                  */
#define RTC_RTCL2HREG   0x16u   /* RTC long-2 high                 */

typedef struct {
    uint64_t etime;       /* simulated elapsed ticks              */
    uint64_t ecmp;        /* compare register                     */
    uint32_t rtcl1;       /* calendar long-1 (stub)               */
    uint32_t rtcl2;       /* calendar long-2 (stub)               */
} rtc_state_t;

void     rtc_init (rtc_state_t *s);
uint32_t rtc_read (rtc_state_t *s, uint32_t offset, unsigned size);
void     rtc_write(rtc_state_t *s, uint32_t offset, unsigned size, uint32_t val);

/* Advance elapsed time counter; call periodically from the machine loop */
void     rtc_tick (rtc_state_t *s, uint64_t ticks);
