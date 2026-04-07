#include <stdio.h>
#include "cmu.h"

void cmu_init(cmu_state_t *s, bool warm)
{
    if (warm) {
        s->sticky_bits = 0x0902;
        s->clkmsk = s->sticky_bits;
    } else {
        s->sticky_bits = 0;
        s->clkmsk = 0;
    }
}

uint32_t cmu_read(cmu_state_t *s, uint32_t offset, unsigned size)
{
    if (offset == CMU_CMUCLKMSK || offset == CMU_CMUCLKMSK + 0x2u) {
        uint32_t v = s->clkmsk;
        if (size >= 4u)
            return (v << 16) | v;
        return v;
    }
    fprintf(stderr, "[CMU] Unhandled read offset 0x%02X\n", offset);
    return 0;
}

void cmu_write(cmu_state_t *s, uint32_t offset, unsigned size, uint32_t val)
{
    (void)size;
    if (offset == CMU_CMUCLKMSK || offset == CMU_CMUCLKMSK + 0x2u) {
        s->clkmsk = (uint16_t)val | s->sticky_bits;
    } else {
        fprintf(stderr, "[CMU] Unhandled write offset 0x%02X = 0x%04X\n",
                offset, val);
    }
}
