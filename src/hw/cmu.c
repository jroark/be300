#include <stdio.h>
#include "cmu.h"

void cmu_init(cmu_state_t *s)
{
    /*
     * WORKAROUND: Seed CMU clock mask from warm-state hardware survey.
     *
     * On real hardware CMUCLKMSK at PA 0x0F000060 was observed as
     * 0x0902 across multiple warm-state dumps (HardwareDump6.txt).
     * The VR4131 datasheet POR default for CMUCLKMSK is not clearly
     * documented.  On a truly cold boot the register might be 0 or
     * a chip-specific default.
     *
     * The sticky_bits mechanism preserves these bits across writes:
     * WinCE's resume path writes 0 to CMUCLKMSK before later OR-ing
     * in 0x0800.  Without the sticky bits, the register would
     * collapse to 0, potentially disabling clocks that the ROM or
     * SPL expects to be running.
     */
    s->sticky_bits = 0x0902;  /* WORKAROUND: warm-state seed */
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
         * WORKAROUND: OR in the warm-state sticky bits on every write.
         *
         * WinCE's resume path writes 0 to CMUCLKMSK before later
         * OR-ing in the bits it needs.  Without preserving the
         * sticky bits, the intermediate 0 write would disable
         * clocks that the ROM assumed were running.  On real
         * hardware the clock enable bits might be latched or have
         * hardware-enforced minimums that prevent a full collapse
         * to zero.
         */
        s->clkmsk = (uint16_t)val | s->sticky_bits;  /* WORKAROUND */
    } else {
        fprintf(stderr, "[CMU] Unhandled write offset 0x%02X = 0x%04X\n",
                offset, val);
    }
}
