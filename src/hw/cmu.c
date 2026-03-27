#include <stdio.h>
#include "cmu.h"

void cmu_init(cmu_state_t *s)
{
    /*
     * Warm-state seed from hardware_survey/HardwareDump6.txt:
     * CMUCLKMSK observed at PA 0x0F000060 as 0x0902.
     */
    s->sticky_bits = 0x0902;
    s->clkmsk = s->sticky_bits;
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
        /*
         * The BE-300 warm-state surveys keep 0x0902 latched across runs.
         * WinCE's resume path writes 0 here before later OR-ing in 0x0800;
         * preserving the stable warm-state bits matches the observed page
         * pattern and avoids collapsing the register to zero.
         */
        s->clkmsk = (uint16_t)val | s->sticky_bits;
    } else {
        fprintf(stderr, "[CMU] Unhandled write offset 0x%02X = 0x%04X\n",
                offset, val);
    }
}
