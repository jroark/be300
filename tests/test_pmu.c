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
    /* UM §12.2.1: RTCRST is bit 4; DCDST (bit 10) shows the DCD# pin,
       high on a BE-300 (hw_dump_vr4131.txt:35 reads 0x0400). */
    CHECK(PMUINTREG_RTCRST == 0x10 && PMUINTREG_RSTSW == 0x08 &&
          PMUINTREG_TIMOUTRST == 0x20 && PMUINTREG_RTCINTR == 0x200);
    CHECK(pmu_read(&p, PMU_PMUINTREG, 2) == (PMUINTREG_RTCRST | PMUINTREG_DCDST));
    CHECK(p.pmucntreg == 0);
    pmu_write(&p, PMU_PMUINTREG, 2, PMUINTREG_RTCRST);
    CHECK(pmu_read(&p, PMU_PMUINTREG, 2) == PMUINTREG_DCDST);
    /* DCDST is read-only; the memo bits are plain read/write. */
    pmu_write(&p, PMU_PMUINTREG, 2, 0xFFFF);
    CHECK(pmu_read(&p, PMU_PMUINTREG, 2) == (PMUINTREG_DCDST | PMUINTREG_MEMO));
    pmu_write(&p, PMU_PMUINTREG, 2, 0x0040);
    CHECK(pmu_read(&p, PMU_PMUINTREG, 2) == (PMUINTREG_DCDST | 0x0040));
    /* What Windows CE writes before HIBERNATE (nk.exe 0x80079560). */
    p.pmuintreg = 0x03FF;
    pmu_write(&p, PMU_PMUINTREG, 2, 0xFB3B);
    CHECK(pmu_read(&p, PMU_PMUINTREG, 2) == PMUINTREG_DCDST);
    p.dcd_high = 0;
    CHECK(!(pmu_read(&p, PMU_PMUINTREG, 2) & PMUINTREG_DCDST));
}

/* UM §12.1.3: an alarm wake from Hibernate sets RTCINTR, a power-switch
   wake sets nothing (a real BE-300 read 0x0600 and 0x0400, Glaze
   2026-10-31), and both arm the HALTimer again (UM §12.1.2(1)). */
static void test_hibernate_wake(void)
{
    pmu_state_t p;

    reset_calls = 0;
    pmu_init(&p);
    pmu_arm_initial(&p, 0, 1000, reset_cb, NULL);
    pmu_write(&p, PMU_PMUCNTREG, 2, PMUCNTREG_HALTIMERRST);
    pmu_write(&p, PMU_PMUINTREG, 2, 0xFB3B);
    pmu_hibernate_wake(&p, PMUINTREG_RTCINTR, 500);
    CHECK(pmu_read(&p, PMU_PMUINTREG, 2) == (PMUINTREG_RTCINTR | PMUINTREG_DCDST));
    CHECK(p.haltimer_remaining_cycles == 1000);
    pmu_tick(&p, 800);
    CHECK(p.haltimer_remaining_cycles == 700 && reset_calls == 0);
    pmu_write(&p, PMU_PMUCNTREG, 2, PMUCNTREG_HALTIMERRST);
    CHECK(p.haltimer_remaining_cycles == 0);

    pmu_write(&p, PMU_PMUINTREG, 2, 0xFB3B);
    pmu_hibernate_wake(&p, 0, 900);
    CHECK(pmu_read(&p, PMU_PMUINTREG, 2) == PMUINTREG_DCDST);
    pmu_tick(&p, 1300);
    pmu_tick(&p, 1700);
    pmu_tick(&p, 1950);
    CHECK(reset_calls == 1 && (p.pmuintreg & PMUINTREG_TIMOUTRST));

    pmu_reset_switch(&p);
    pmu_apply_pending_reset_state(&p, 2000);
    CHECK(p.pmuintreg & PMUINTREG_RSTSW);
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
    test_hibernate_wake();
    if (failures) {
        fprintf(stderr, "test_pmu: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_pmu: all tests passed");
    return 0;
}
