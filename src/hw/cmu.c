#include <stdio.h>
#include "cmu.h"

void cmu_init(cmu_state_t *s)
{
    /* All clocks enabled at reset */
    s->clkmsk = 0xFFFF;
}

uint32_t cmu_read(cmu_state_t *s, uint32_t offset, unsigned size)
{
    (void)size;
    if (offset == CMU_CMUCLKMSK)
        return s->clkmsk;
    fprintf(stderr, "[CMU] Unhandled read offset 0x%02X\n", offset);
    return 0;
}

void cmu_write(cmu_state_t *s, uint32_t offset, unsigned size, uint32_t val)
{
    (void)size;
    if (offset == CMU_CMUCLKMSK) {
        s->clkmsk = (uint16_t)val;
    } else {
        fprintf(stderr, "[CMU] Unhandled write offset 0x%02X = 0x%04X\n",
                offset, val);
    }
}
