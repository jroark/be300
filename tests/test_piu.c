/* VRC4173 PIU regression tests (VRC4173 UM chapter 9). */

#include "hw/piu.h"

#include <stdio.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define AUTO_SCAN_CONTROL 0x0304u
#define TOUCH_IRQ_MASK    0x1D00u

static void configure_fast_scan(piu_state_t *p)
{
    piu_write(p, 0x04, TOUCH_IRQ_MASK, 0);
    piu_write(p, 0x08, 1, 0); /* 30 us scan interval. */
    piu_write(p, 0x0C, 0, 0); /* No stabilization delay. */
    piu_write(p, 0x00, AUTO_SCAN_CONTROL, 0);
}

static void test_reset_command_and_defaults(void)
{
    piu_state_t p;

    piu_init(&p);
    configure_fast_scan(&p);
    piu_set_touch(&p, true, 100, 200, 0);
    piu_tick(&p, 40000);
    CHECK(p.page_valid[0]);
    CHECK((p.regs[1] & (PIU_PENDING_PENCHG | PIU_PENDING_PAGE0)) != 0);

    piu_write(&p, 0x00, 0x0401, 40000);
    CHECK(p.padstate == PIU_PADSTATE_DISABLE);
    CHECK(p.regs[0] == 0);
    CHECK(p.regs[1] == 0);
    CHECK(p.regs[2] == PIU_RESET_SCAN_INTERVAL);
    CHECK(p.regs[3] == PIU_RESET_STABLE);
    CHECK(!p.page_valid[0] && !p.page_valid[1]);
    CHECK(!p.data_armed);
    CHECK((piu_read(&p, 0x00, 40000) & 1u) == 0);
    CHECK(!piu_irq_pending(&p));
}

static void test_pen_state_latch_and_queued_release(void)
{
    piu_state_t p;

    piu_init(&p);
    configure_fast_scan(&p);
    piu_set_touch(&p, true, 20, 30, 0);
    CHECK((piu_read(&p, 0x00, 0) & 0x2000u) != 0);
    CHECK((p.regs[1] & PIU_PENDING_PENCHG) != 0);
    CHECK(piu_irq_pending(&p));

    piu_set_touch(&p, false, 20, 30, 1000);
    CHECK((piu_read(&p, 0x00, 1000) & 0x2000u) != 0);
    piu_write(&p, 0x04, TOUCH_IRQ_MASK | PIU_PENDING_PENCHG, 1000);
    CHECK((p.regs[1] & PIU_PENDING_PENCHG) != 0);
    CHECK((piu_read(&p, 0x00, 1000) & 0x2000u) == 0);
    piu_write(&p, 0x04, TOUCH_IRQ_MASK | PIU_PENDING_PENCHG, 1000);
    CHECK((p.regs[1] & PIU_PENDING_PENCHG) == 0);
}

static void test_page_order_and_adc_endpoints(void)
{
    piu_state_t p;

    piu_init(&p);
    configure_fast_scan(&p);
    piu_set_touch(&p, true, 0, 0, 0);
    piu_tick(&p, 40000);
    CHECK((p.regs[1] & PIU_PENDING_PAGE0) != 0);
    CHECK(piu_read(&p, 0x20, 40000) == 0x81E1u);
    CHECK(piu_read(&p, 0x24, 40000) == 0x8E30u);
    CHECK(piu_read(&p, 0x28, 40000) == 0x8D1Bu);
    CHECK(piu_read(&p, 0x2C, 40000) == 0x8300u);

    piu_write(&p, 0x04, TOUCH_IRQ_MASK | PIU_PENDING_PAGE0, 40000);
    piu_set_touch(&p, true, 239, 359, 110000);
    CHECK((p.regs[1] & PIU_PENDING_PAGE1) != 0);
    CHECK(piu_read(&p, 0x50, 110000) == 0x8E30u);
    CHECK(piu_read(&p, 0x54, 110000) == 0x81E1u);
    CHECK(piu_read(&p, 0x58, 110000) == 0x8300u);
    CHECK(piu_read(&p, 0x5C, 110000) == 0x8D1Bu);

    piu_write(&p, 0x04, TOUCH_IRQ_MASK | PIU_PENDING_PAGE1, 110000);
    piu_tick(&p, 180000);
    CHECK((p.regs[1] & PIU_PENDING_PAGE0) != 0);
}

static void test_full_pages_require_explicit_restart(void)
{
    piu_state_t p;

    piu_init(&p);
    configure_fast_scan(&p);
    piu_set_touch(&p, true, 120, 180, 0);
    piu_tick(&p, 40000);
    piu_tick(&p, 110000);
    CHECK(p.page_valid[0] && p.page_valid[1]);
    CHECK((p.regs[1] & PIU_PENDING_TOUCH_DATA) == PIU_PENDING_TOUCH_DATA);

    piu_tick(&p, 180000);
    CHECK((p.regs[1] & PIU_PENDING_DATALOST) != 0);
    CHECK(p.padstate == PIU_PADSTATE_STANDBY);
    CHECK(p.page_valid[0] && p.page_valid[1]);
    CHECK(!p.data_armed);

    piu_write(&p, 0x00, AUTO_SCAN_CONTROL, 180000);
    CHECK(p.padstate == PIU_PADSTATE_STANDBY);
    CHECK(!p.data_armed);

    piu_write(&p, 0x04, TOUCH_IRQ_MASK | PIU_PENDING_PAGE0 |
        PIU_PENDING_PAGE1 | PIU_PENDING_DATALOST, 180000);
    CHECK((p.regs[1] & PIU_PENDING_COORDINATE) == 0);
    CHECK(p.padstate == PIU_PADSTATE_STANDBY);
    CHECK(!p.data_armed);

    piu_write(&p, 0x00, AUTO_SCAN_CONTROL, 180000);
    CHECK(p.padstate == PIU_PADSTATE_PEN_DATA);
    CHECK(p.data_armed);
    piu_tick(&p, 220000);
    CHECK((p.regs[1] & PIU_PENDING_PAGE0) != 0);
}

int main(void)
{
    test_reset_command_and_defaults();
    test_pen_state_latch_and_queued_release();
    test_page_order_and_adc_endpoints();
    test_full_pages_require_explicit_restart();
    if (failures) {
        fprintf(stderr, "test_piu: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_piu: all tests passed");
    return 0;
}
