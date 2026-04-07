#pragma once
#include <stdint.h>

/*
 * BCU — Bus Control Unit (VR4131 hardware manual §7)
 *
 * Registers are 16-bit, accessed at 2-byte-aligned addresses.
 * We stub most of these: WinCE writes them during init but they
 * don't need to produce real bus timing behaviour.
 */

/* Register offsets from BCU base (0x0F000000) */
#define BCU_BCUCNTREG1  0x00u   /* Bus control 1 */
#define BCU_BCUCNTREG2  0x02u   /* Bus control 2 */
#define BCU_ROMSIZEREG  0x04u   /* ROM size */
#define BCU_ROMSPEEDREG 0x06u   /* ROM speed */
#define BCU_IO0SIZEREG  0x08u   /* I/O 0 size */
#define BCU_IO0SPEEDREG 0x0Au   /* I/O 0 speed */
#define BCU_IO1SPEEDREG 0x0Cu   /* I/O 1 speed */
#define BCU_REVIDREG    0x10u   /* Revision ID (read-only) */
#define BCU_CLKSPEEDREG 0x14u   /* Clock speed */

#define BCU_REVID_VR4131 0x0104u /* BCU revision for VR4131 */

typedef struct {
    uint16_t bcucntreg1;
    uint16_t bcucntreg2;
    uint16_t romsizereg;
    uint16_t romspeedreg;
    uint16_t io0sizereg;
    uint16_t io0speedreg;
    uint16_t io1speedreg;
    uint16_t revidreg;
    uint16_t clkspeedreg;
} bcu_state_t;

void     bcu_init (bcu_state_t *s);
uint32_t bcu_read (bcu_state_t *s, uint32_t offset, unsigned size);
void     bcu_write(bcu_state_t *s, uint32_t offset, unsigned size, uint32_t val);
