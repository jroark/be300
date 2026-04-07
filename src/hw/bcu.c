#include <stdio.h>
#include <string.h>
#include "bcu.h"

#define BCU_WINDOW_SIZE 0x20u

static inline void bcu_put_u16(uint8_t *buf, uint32_t off, uint16_t v)
{
    buf[off + 0u] = (uint8_t)(v & 0xFFu);
    buf[off + 1u] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline uint16_t bcu_get_u16(const uint8_t *buf, uint32_t off)
{
    return (uint16_t)(buf[off + 0u] | ((uint16_t)buf[off + 1u] << 8));
}

static void bcu_serialize_window(const bcu_state_t *s, uint8_t *buf)
{
    memset(buf, 0, BCU_WINDOW_SIZE);
    bcu_put_u16(buf, BCU_BCUCNTREG1,  s->bcucntreg1);
    bcu_put_u16(buf, BCU_BCUCNTREG2,  s->bcucntreg2);
    bcu_put_u16(buf, BCU_ROMSIZEREG,  s->romsizereg);
    bcu_put_u16(buf, BCU_ROMSPEEDREG, s->romspeedreg);
    bcu_put_u16(buf, BCU_IO0SIZEREG,  s->io0sizereg);
    bcu_put_u16(buf, BCU_IO0SPEEDREG, s->io0speedreg);
    bcu_put_u16(buf, BCU_IO1SPEEDREG, s->io1speedreg);
    bcu_put_u16(buf, BCU_REVIDREG,    s->revidreg);
    bcu_put_u16(buf, BCU_CLKSPEEDREG, s->clkspeedreg);
}

static void bcu_apply_window_writes(bcu_state_t *s, const uint8_t *buf)
{
    s->bcucntreg1  = bcu_get_u16(buf, BCU_BCUCNTREG1);
    s->bcucntreg2  = bcu_get_u16(buf, BCU_BCUCNTREG2);
    s->romsizereg  = bcu_get_u16(buf, BCU_ROMSIZEREG);
    s->romspeedreg = bcu_get_u16(buf, BCU_ROMSPEEDREG);
    s->io0sizereg  = bcu_get_u16(buf, BCU_IO0SIZEREG);
    s->io0speedreg = bcu_get_u16(buf, BCU_IO0SPEEDREG);
    s->io1speedreg = bcu_get_u16(buf, BCU_IO1SPEEDREG);
    s->clkspeedreg = bcu_get_u16(buf, BCU_CLKSPEEDREG);
}

void bcu_init(bcu_state_t *s)
{
    /*
     * Cold boot: BCU registers at hardware-reset defaults.
     *
     * REVIDREG (0x5002) and CLKSPEEDREG (0x020C) are read-only hardware
     * values — chip revision and pin-strap clock configuration.  The ROM
     * programs the bus timing registers (BCUCNTREG1, ROM/IO speed/size)
     * during early initialization.
     */
    s->bcucntreg1  = 0;
    s->bcucntreg2  = 0;
    s->romsizereg  = 0;
    s->romspeedreg = 0;
    s->io0sizereg  = 0;
    s->io0speedreg = 0;
    s->io1speedreg = 0;
    s->revidreg    = 0x5002;  /* read-only chip revision ID */
    s->clkspeedreg = 0x020C;  /* read-only pin-strap clock config */
}

uint32_t bcu_read(bcu_state_t *s, uint32_t offset, unsigned size)
{
    uint8_t window[BCU_WINDOW_SIZE];
    uint32_t val = 0;

    if (size == 0u || size > 4u || (uint64_t)offset + size > BCU_WINDOW_SIZE)
        return 0;

    bcu_serialize_window(s, window);
    for (unsigned i = 0; i < size; i++)
        val |= (uint32_t)window[offset + i] << (i * 8u);
    return val;
}

void bcu_write(bcu_state_t *s, uint32_t offset, unsigned size, uint32_t val)
{
    uint8_t window[BCU_WINDOW_SIZE];
    uint16_t old_revid;

    if (size == 0u || size > 4u || (uint64_t)offset + size > BCU_WINDOW_SIZE)
        return;

    bcu_serialize_window(s, window);
    old_revid = s->revidreg;
    for (unsigned i = 0; i < size; i++)
        window[offset + i] = (uint8_t)((val >> (i * 8u)) & 0xFFu);

    /* REVIDREG is read-only. */
    bcu_put_u16(window, BCU_REVIDREG, old_revid);
    bcu_apply_window_writes(s, window);
}
