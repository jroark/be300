/* ScCmcu watchdog (src/hw/cmcu.h): the ROM's and Windows CE's sequences. */

#include "hw/cmcu.h"

#include <stdio.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define S(x) ((uint64_t)(x) * UINT64_C(1000000000))

int main(void)
{
    cmcu_state_t c;

    cmcu_init(&c);
    /* The boot ROM's stop (0x9FC00D2E-0x9FC00D46) completes at power-on. */
    cmcu_write(&c, CMCU_ARG0, 0, 0);
    cmcu_write(&c, CMCU_CMD0, 0, 0);
    cmcu_write(&c, CMCU_ARG0, cmcu_read(&c, CMCU_ARG0) | 0x35, 0);
    cmcu_write(&c, CMCU_CMD0, cmcu_read(&c, CMCU_CMD0) | 0x5C, 0);
    CHECK(cmcu_read(&c, CMCU_STATUS) != 3 && !cmcu_expired(&c, S(1000)));

    /* CE's start: TIMEOUT 150 s, then 0x53/0x3A until STATUS reads 3. */
    cmcu_write(&c, CMCU_CONFIG, 0x10000, S(10));
    cmcu_write(&c, CMCU_TIMEOUT, 0x96, S(10));
    cmcu_write(&c, CMCU_ARG, 0x53, S(10));
    cmcu_write(&c, CMCU_CMD, 0x3A, S(10));
    CHECK(cmcu_read(&c, CMCU_STATUS) == 3 && cmcu_read(&c, CMCU_CONFIG) == 0x10000);
    /* Reloads keep it from firing; 150 s without one resets. */
    cmcu_write(&c, CMCU_TIMEOUT, 0x96, S(150));
    CHECK(!cmcu_expired(&c, S(299)));
    CHECK(cmcu_expired(&c, S(300)) && !cmcu_expired(&c, S(301)));
    CHECK(cmcu_read(&c, CMCU_STATUS) == 0);

    /* Stopped before a sleep, it never fires. */
    cmcu_write(&c, CMCU_ARG, 0x53, S(400));
    cmcu_write(&c, CMCU_CMD, 0x3A, S(400));
    cmcu_write(&c, CMCU_ARG0, 0x35, S(401));
    cmcu_write(&c, CMCU_CMD0, 0x5C, S(401));
    CHECK(cmcu_read(&c, CMCU_STATUS) == 0 && !cmcu_expired(&c, S(10000)));
    /* Neither half of a command does anything alone. */
    cmcu_write(&c, CMCU_ARG, 0x00, S(10000));
    cmcu_write(&c, CMCU_CMD, 0x3A, S(10000));
    CHECK(cmcu_read(&c, CMCU_STATUS) == 0);

    if (failures) {
        fprintf(stderr, "test_cmcu: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_cmcu: all tests passed");
    return 0;
}
