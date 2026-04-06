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
     * Seed the BCU window from the stable hardware survey readback at
     * PA 0x0F000000:
     *   0x0F000000: 0000000C 100C4444 26721242 00000000
     *   0x0F000010: 00005002 0883020C 00000000 00000000
     *
     * The boot ROM polls the 32-bit word at 0xAF000000 and expects it to
     * be non-zero after early serial/BCU setup. Zero-seeding BCUCNTREG1/2
     * leaves that poll permanently closed and traps the ROM in the retry
     * loop at 0x9FC003A0.
     */
    s->bcucntreg1  = 0x000C;
    s->bcucntreg2  = 0x0000;
    s->romsizereg  = 0x4444;
    s->romspeedreg = 0x100C;
    s->io0sizereg  = 0x1242;
    s->io0speedreg = 0x2672;
    s->io1speedreg = 0x0000;
    s->revidreg    = 0x5002;
    s->clkspeedreg = 0x020C;
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
