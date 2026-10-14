/* VR4131 PMU/HALTimer regression tests (VR4131 UM §12.2). */

#include "hw/pmu.h"

#include <stdint.h>
#include <stdio.h>

static int failures;
static int reset_calls;
static void *reset_arg;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static void reset_cb(void *opaque)
{
    reset_calls++;
    reset_arg = opaque;
}

static void test_reset_and_w1c(void)
{
    pmu_state_t p;

    pmu_init(&p);
    CHECK(p.pmuintreg == PMUINTREG_RTCRST);
    CHECK(p.pmucntreg == 0);
    pmu_write(&p, PMU_PMUINTREG, 2, PMUINTREG_RTCRST);
    CHECK(p.pmuintreg == 0);
}

static void test_haltimer_and_count_wrap(void)
{
    pmu_state_t p;
    int token;

    reset_calls = 0;
    reset_arg = NULL;
    pmu_init(&p);
    pmu_arm_initial(&p, UINT32_C(0xFFFFFFF0), 1000, reset_cb, &token);
    pmu_tick(&p, UINT32_C(0x00000020));
    CHECK(p.haltimer_remaining_cycles == 952);
    CHECK(reset_calls == 0);

    pmu_tick(&p, UINT32_C(0x00000200));
    CHECK(p.haltimer_remaining_cycles == 472);
    pmu_tick(&p, UINT32_C(0x000003D8));
    CHECK(p.haltimer_remaining_cycles == 0);
    CHECK((p.pmuintreg & PMUINTREG_TIMOUTRST) != 0);
    CHECK(reset_calls == 1);
    CHECK(reset_arg == &token);
    pmu_tick(&p, UINT32_C(0x00000800));
    CHECK(reset_calls == 1);
}

static void test_count_rewrite_and_ack(void)
{
    pmu_state_t p;

    pmu_init(&p);
    pmu_arm_initial(&p, 1000, 1000, reset_cb, NULL);
    pmu_tick(&p, 100);
    CHECK(p.haltimer_remaining_cycles == 1000);
    CHECK(p.last_count_sample == 100);

    pmu_write(&p, PMU_PMUCNTREG, 2, PMUCNTREG_HALTIMERRST);
    CHECK(p.haltimer_remaining_cycles == 0);
    CHECK(p.haltimer_arm_cycles == 0);
}

static void test_soft_reset_state(void)
{
    pmu_state_t p;

    pmu_init(&p);
    pmu_write(&p, PMU_PMUTCLKDIVREG, 2, 0x1234);
    pmu_write(&p, PMU_PMUWAITREG, 2, 0x5678);
    pmu_write(&p, PMU_PMUDIVREG, 2, 0x9ABC);
    pmu_write(&p, PMU_PMUCNT2REG, 2, PMUCNT2REG_SOFTRST);
    CHECK(p.softrst_pending == 1);
    CHECK(p.pending_reset_cause == PMUINTREG_RSTSW);

    pmu_apply_pending_reset_state(&p, 77);
    CHECK((p.pmuintreg & PMUINTREG_RSTSW) != 0);
    CHECK(p.pending_reset_cause == 0);
    CHECK(p.pmutclkdivreg == 0x1234);
    CHECK(p.pmuwaitreg == 0x5678);
    CHECK(p.pmudivreg == 0x9ABC);
    CHECK(p.last_count_sample == 77);
}

int main(void)
{
    test_reset_and_w1c();
    test_haltimer_and_count_wrap();
    test_count_rewrite_and_ack();
    test_soft_reset_state();
    if (failures) {
        fprintf(stderr, "test_pmu: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_pmu: all tests passed");
    return 0;
}
