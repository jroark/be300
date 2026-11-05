/*
 *  PCMCIA jacket: Windows CE's socket 1 on the BE-300 (see jacket.h).
 *  The values below are what a real BE-300 read with cards in the jacket
 *  (glaze-os docs/hardware/PCCARDS.md, VeoTest snap1/s1probe captures).
 */
#include <string.h>

#include "jacket.h"

/* Register offsets within 0x0B000000. */
#define REG_CTRL        0x500u
#define REG_RESET       0x504u
#define REG_STATUS      0x508u
#define CTRL_UP         0x0Bu       /* supply bits 0 and 3, output bit 1 */

void jacket_init(jacket_t *j, cf_state_t *card, cf_state_t *cf)
{
    memset(j, 0, sizeof(*j));
    j->card = card;
    j->cf = cf;
}

bool jacket_bus_on(const jacket_t *j)
{
    return j && j->cf && (j->cf->companion_page[0xB00] & 1u);
}

static uint32_t reg(const jacket_t *j, uint32_t offset)
{
    return j->regs[(offset & (JACKET_REG_SIZE - 1u)) / 4u];
}

bool jacket_card_up(jacket_t *j)
{
    bool up = j && cf_present(j->card) && jacket_bus_on(j) &&
              (reg(j, REG_CTRL) & CTRL_UP) == CTRL_UP && !(reg(j, REG_RESET) & 1u);
    if (j)
        j->was_up = up;
    return up;
}

/* STATUS with the card up, per card (real cards in the jacket): it carries
 * the card's ready/BVD/WP lines, so it differs between cards. */
static uint32_t card_status(const cf_state_t *card)
{
    switch (card->card_kind) {
    case CF_CARD_3C589: return 0x3F8u;
    case CF_CARD_XI825: return 0x3B9u;     /* an Orinoco/WaveLAN (--hermes) */
    case CF_CARD_MODEM: return 0x3F9u;     /* a USR/Megahertz XJ/CC1560 */
    default:            return 0x2F9u;     /* an AmbiCom NE2000 */
    }
}

static bool pin(uint32_t offset)
{
    switch (offset) {
    case 0x104u: case 0x108u: case 0x10Cu: case 0x110u:
    case 0x114u: case 0x118u: case 0x150u:
        return true;
    default:
        return false;
    }
}

static uint32_t reg_read32(jacket_t *j, uint32_t offset)
{
    if (!jacket_bus_on(j))
        return 0;                       /* an unpowered controller reads 0 */
    bool present = cf_present(j->card);
    switch (offset) {
    case 0x000u: return 1u;
    case 0x008u: return 0x55AA55AAu;
    case 0x00Cu: return 0xAA55AA55u;
    case 0x010u: return 0x7402u;
    case 0x100u: return 0x641u;
    case REG_STATUS:
        return jacket_card_up(j) ? card_status(j->card) : 0u;
    default:
        break;
    }
    if (pin(offset)) {
        /* Bits 0-1 as written (3 arms both edges), bits 4-5 latched edges
         * (none modelled), bit 6 the level: with a card, 0x104-0x10C read
         * high and the others low (0x43, 0x43, 0x43, 3, 3, 3; 0x150 = 3). */
        bool high = present && offset <= 0x10Cu;
        return (reg(j, offset) & 0x0Fu) | (high ? 0x40u : 0u);
    }
    return reg(j, offset);
}

uint64_t jacket_reg_read(jacket_t *j, uint32_t offset, unsigned size)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        uint32_t at = offset + i;
        if (at >= JACKET_REG_SIZE)
            break;
        uint32_t word = reg_read32(j, at & ~3u);
        v |= (uint64_t)((word >> (8u * (at & 3u))) & 0xFFu) << (8u * i);
    }
    return v;
}

void jacket_reg_write(jacket_t *j, uint32_t offset, unsigned size, uint64_t value)
{
    for (unsigned i = 0; i < size; i++) {
        uint32_t at = offset + i;
        if (at >= JACKET_REG_SIZE)
            break;
        uint32_t shift = 8u * (at & 3u);
        uint32_t *w = &j->regs[at / 4u];
        *w = (*w & ~(0xFFu << shift)) | ((uint32_t)((value >> (8u * i)) & 0xFFu) << shift);
    }
}

uint64_t jacket_io_read(jacket_t *j, uint32_t offset, unsigned size)
{
    if (!jacket_card_up(j))
        return 0;
    return cf_window_read(j->card, offset & 0xFFFu, size);
}

void jacket_io_write(jacket_t *j, uint32_t offset, unsigned size, uint64_t value)
{
    if (jacket_card_up(j))
        cf_window_write(j->card, offset & 0xFFFu, size, value);
}

/* Attribute memory: the CIS at even addresses, configuration registers
 * written through it. */
uint64_t jacket_attr_read(jacket_t *j, uint32_t offset, unsigned size)
{
    if (!jacket_card_up(j))
        return 0;
    return cf_cis_read(j->card, offset, size);
}

void jacket_attr_write(jacket_t *j, uint32_t offset, unsigned size, uint64_t value)
{
    if (jacket_card_up(j))
        cf_cis_write(j->card, offset, size, value);
}

uint8_t jacket_companion_byte(const jacket_t *j, uint32_t offset, uint8_t byte)
{
    bool present = cf_present(j->card);
    switch (offset) {
    case 0x008u: return present ? 0x04u : 0x0Cu;
    case 0x04Cu: return present ? 0x0Cu : 0x48u;
    case 0xB50u: return jacket_bus_on(j) ? (uint8_t)(byte | 0x08u) : byte;
    default:     return byte;
    }
}
