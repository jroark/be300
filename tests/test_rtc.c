/* VR4131 RTC regression tests (VR4131 UM §13). */

#include "hw/rtc.h"

#include <inttypes.h>
#include <stdio.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static uint64_t etime_value(const rtc_state_t *s)
{
    return s->etime & UINT64_C(0x0000FFFFFFFFFFFF);
}

static void test_cold_reset_and_epoch(void)
{
    rtc_state_t r;
    uint64_t feb28, mar1;

    rtc_init(&r);
    CHECK(r.etime == 0);
    CHECK(r.ecmp == 0);
    CHECK(r.rtcint == 0);

    CHECK(rtc_init_from_ymdhms(&r, 1850, 1, 1, 0, 0, 0) == 0);
    CHECK(etime_value(&r) == 0);
    CHECK(rtc_init_from_ymdhms(&r, 1850, 1, 2, 0, 0, 0) == 0);
    CHECK(etime_value(&r) == UINT64_C(86400) * 32768);

    CHECK(rtc_init_from_ymdhms(&r, 2000, 2, 28, 0, 0, 0) == 0);
    feb28 = etime_value(&r);
    CHECK(rtc_init_from_ymdhms(&r, 2000, 3, 1, 0, 0, 0) == 0);
    mar1 = etime_value(&r);
    CHECK(mar1 - feb28 == UINT64_C(2) * 86400 * 32768);

    CHECK(rtc_init_from_ymdhms(&r, 1849, 12, 31, 0, 0, 0) == -1);
    CHECK(rtc_init_from_ymdhms(&r, 1900, 2, 29, 0, 0, 0) == -1);
    CHECK(rtc_init_from_ymdhms(&r, 2000, 2, 29, 24, 0, 0) == -1);
}

static void test_register_access_and_wrap(void)
{
    rtc_state_t r;

    rtc_init(&r);
    rtc_write(&r, RTC_ETIMELREG, 2, 0xBEEF);
    rtc_write(&r, RTC_ETIMEMREG, 2, 0xCAFE);
    rtc_write(&r, RTC_ETIMEHREG, 2, 0x1234);
    CHECK(etime_value(&r) == UINT64_C(0x1234CAFEBEEF));
    CHECK(rtc_read(&r, RTC_ETIMELREG, 2) == 0xBEEF);
    CHECK(rtc_read(&r, RTC_ETIMEMREG, 2) == 0xCAFE);
    CHECK(rtc_read(&r, RTC_ETIMEHREG, 2) == 0x1234);
    CHECK(rtc_read(&r, RTC_ETIMELREG, 4) == UINT32_C(0xCAFEBEEF));

    r.etime = UINT64_C(0x0000FFFFFFFFFFFE);
    rtc_tick(&r, 3);
    CHECK(r.etime == 1);
}

static void test_elapsed_compare(void)
{
    rtc_state_t r;

    rtc_init(&r);
    rtc_write(&r, RTC_ECMPLREG, 2, 10);
    CHECK(r.elapsed_compare_armed == 1);
    CHECK((r.rtcint & RTCINT_ELAPSEDTIME_INT) == 0);
    rtc_tick(&r, 9);
    CHECK((r.rtcint & RTCINT_ELAPSEDTIME_INT) == 0);
    rtc_tick(&r, 1);
    CHECK((r.rtcint & RTCINT_ELAPSEDTIME_INT) != 0);
    CHECK(r.elapsed_compare_fired == 1);

    rtc_write(&r, RTC_RTCINTREG, 2, RTCINT_ELAPSEDTIME_INT);
    CHECK((r.rtcint & RTCINT_ELAPSEDTIME_INT) == 0);
    rtc_tick(&r, 100);
    CHECK((r.rtcint & RTCINT_ELAPSEDTIME_INT) == 0);

    rtc_write(&r, RTC_ECMPLREG, 2, 200);
    CHECK(r.elapsed_compare_fired == 0);
    rtc_tick(&r, 90);
    CHECK((r.rtcint & RTCINT_ELAPSEDTIME_INT) != 0);
}

int main(void)
{
    test_cold_reset_and_epoch();
    test_register_access_and_wrap();
    test_elapsed_compare();
    if (failures) {
        fprintf(stderr, "test_rtc: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_rtc: all tests passed");
    return 0;
}
