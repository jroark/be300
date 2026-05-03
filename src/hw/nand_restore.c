/*
 *  src/hw/nand_restore.c — NANDWRITER restore engine.
 *
 *  Models the recovery-mode register bank at VRC4173 offsets 0x0980,
 *  0x0B00-0x0D00 (page buffer) and 0x0C00-0x0CC0 (control), plus the
 *  scattered side-band registers in the 0x1100/0x1B20/0x1CC0 range.
 *  The implementation is fully self-contained: it lives in its own
 *  s->restore_* slice of nand_state_t and never reaches into the SPL
 *  or ROM transfer engines.
 *
 *  Split out of src/hw/nand.c. Public API (declared in hw/nand.h):
 *    nand_restore_handles_offset, nand_restore_read, nand_restore_write.
 *  Private helper consumed by nand_init():
 *    nand_restore_seed_regs (declared in nand_internal.h).
 */

#include <stdint.h>
#include <string.h>

#include "nand.h"
#include "nand_internal.h"

static uint32_t nand_restore_reg_index(uint32_t offset)
{
    return offset >> 2;
}

static uint32_t nand_restore_get_reg(const nand_state_t *s, uint32_t offset)
{
    uint32_t idx = nand_restore_reg_index(offset);

    if (idx >= sizeof(s->restore_regs) / sizeof(s->restore_regs[0]))
        return 0;
    return s->restore_regs[idx];
}

static void nand_restore_set_reg(nand_state_t *s, uint32_t offset, uint32_t value)
{
    uint32_t idx = nand_restore_reg_index(offset);

    if (idx < sizeof(s->restore_regs) / sizeof(s->restore_regs[0]))
        s->restore_regs[idx] = value;
}

static void nand_restore_set_ready(nand_state_t *s, bool ready)
{
    uint32_t reg = nand_restore_get_reg(s, NAND_REG_RESTORE_C40);

    if (ready)
        reg |= 0x00000001u;
    else
        reg &= ~0x00000001u;
    nand_restore_set_reg(s, NAND_REG_RESTORE_C40, reg);
}

void nand_restore_seed_regs(nand_state_t *s)
{
    static const struct {
        uint16_t off;
        uint32_t val;
    } seed[] = {
        { 0x0980u, 0x00000001u }, { 0x0984u, 0x00000202u },
        { 0x0988u, 0x00000F70u }, { 0x098Cu, 0x00000000u },
        { 0x0C00u, 0x00000020u }, { 0x0C04u, 0x00000002u },
        { 0x0C08u, 0x00000050u }, { 0x0C0Cu, 0x000000F1u },
        { 0x0C10u, 0x00000002u }, { 0x0C14u, 0x00000000u },
        { 0x0C18u, 0x00000000u }, { 0x0C1Cu, 0x00000000u },
        { 0x0C20u, 0x00000000u }, { 0x0C24u, 0x00000014u },
        { 0x0C28u, 0x00000000u }, { 0x0C2Cu, 0x00000001u },
        { 0x0C30u, 0x0000E000u }, { 0x0C34u, 0x000001FFu },
        { 0x0C38u, 0x00000000u }, { 0x0C3Cu, 0x00001F1Fu },
        { 0x0C40u, 0x00000500u }, { 0x0C44u, 0x00000000u },
        { 0x0C48u, 0x0000030Fu }, { 0x0C4Cu, 0x00000000u },
        { 0x1100u, 0x00000001u }, { 0x1104u, 0x00000001u },
        { 0x1108u, 0x00000001u }, { 0x110Cu, 0x00000001u },
        { 0x1128u, 0x00000001u },
        { 0x1B20u, 0x00000001u },
        { 0x1CC0u, 0x00000001u }, { 0x1CC4u, 0x00000201u },
        { 0x1CC8u, 0x00001000u }, { 0x1CCCu, 0x00000000u },
    };
    size_t i;

    memset(s->restore_regs, 0, sizeof(s->restore_regs));
    memset(s->restore_page_buffer, 0xFF, sizeof(s->restore_page_buffer));
    s->restore_page_addr = 0;
    s->restore_stream_pos = 0;
    s->restore_stream_limit = 0;
    s->restore_stream_base = 0;
    s->restore_phase = 0;
    s->restore_mode = 0;
    s->restore_last_cmd = 0;
    s->restore_addr_count = 0;
    s->restore_ecc_count = 0;
    s->restore_status_ok = true;

    for (i = 0; i < sizeof(seed) / sizeof(seed[0]); i++)
        nand_restore_set_reg(s, seed[i].off, seed[i].val);
}

static uint32_t nand_restore_page_from_addr(const nand_state_t *s)
{
    if (s->restore_addr_count < 3)
        return s->restore_page_addr;

    return (uint32_t)s->restore_addr_bytes[1]
         | ((uint32_t)s->restore_addr_bytes[2] << 8);
}

static uint32_t nand_restore_block_from_addr(const nand_state_t *s)
{
    if (s->restore_addr_count < 2)
        return s->restore_page_addr / NAND_BLOCK_PAGES;

    return ((uint32_t)(s->restore_addr_bytes[0] >> 5) & 0x7u)
         | ((uint32_t)s->restore_addr_bytes[1] << 3);
}

static void nand_restore_fill_page_buffer(nand_state_t *s, uint32_t page)
{
    uint32_t page_off;

    memset(s->restore_page_buffer, 0xFF, sizeof(s->restore_page_buffer));
    if (!s->image || page >= NAND_BLOCK_COUNT * NAND_BLOCK_PAGES)
        return;

    page_off = page * NAND_PAGE_DATA;
    if (page_off + NAND_PAGE_DATA <= s->image_size)
        memcpy(s->restore_page_buffer, s->image + page_off, NAND_PAGE_DATA);

    for (uint32_t i = 0; i < NAND_PAGE_OOB; i++)
        s->restore_page_buffer[NAND_PAGE_DATA + i] =
            nand_stream_oob_byte(s, page, i);
}

static void nand_restore_begin_read(nand_state_t *s)
{
    uint32_t page = nand_restore_page_from_addr(s);

    s->restore_page_addr = page;
    nand_restore_fill_page_buffer(s, page);
    s->restore_stream_base = 0;
    s->restore_stream_pos = 0;
    s->restore_stream_limit = 0;
    s->restore_status_ok = page < NAND_BLOCK_COUNT * NAND_BLOCK_PAGES;
    nand_restore_set_reg(s, NAND_REG_RESTORE_C20,
        s->restore_status_ok ? 0x00000001u : 0x00000000u);
    nand_restore_set_ready(s, true);
}

static void nand_restore_begin_program(nand_state_t *s)
{
    s->restore_page_addr = nand_restore_page_from_addr(s);
    memset(s->restore_page_buffer, 0xFF, sizeof(s->restore_page_buffer));
    s->restore_stream_base = 0;
    s->restore_stream_pos = 0;
    s->restore_stream_limit = 0;
    s->restore_status_ok = true;
    nand_restore_set_reg(s, NAND_REG_RESTORE_C20, 0x00000000u);
    nand_restore_set_reg(s, NAND_REG_RESTORE_CC0, 0x00000000u);
    nand_restore_set_ready(s, true);
}

static void nand_restore_commit_program(nand_state_t *s)
{
    uint32_t page_off;
    uint32_t block;

    if (!s->image || s->restore_page_addr >= NAND_BLOCK_COUNT * NAND_BLOCK_PAGES) {
        s->restore_status_ok = false;
        nand_restore_set_reg(s, NAND_REG_RESTORE_C20, 0x00000000u);
        nand_restore_set_ready(s, true);
        return;
    }

    page_off = s->restore_page_addr * NAND_PAGE_DATA;
    for (uint32_t i = 0; i < NAND_PAGE_DATA && (page_off + i) < s->image_size; i++)
        s->image[page_off + i] &= s->restore_page_buffer[i];

    s->dirty = true;
    block = s->restore_page_addr / NAND_BLOCK_PAGES;
    if (block < NAND_BLOCK_COUNT)
        s->block_data_state[block] = 1;
    s->restore_status_ok = true;
    nand_restore_set_reg(s, NAND_REG_RESTORE_C20, 0x00000001u);
    nand_restore_set_ready(s, true);
}

static void nand_restore_commit_erase(nand_state_t *s)
{
    uint32_t block = nand_restore_block_from_addr(s);
    uint32_t start_page;
    uint32_t start_off;

    if (!s->image || block >= NAND_BLOCK_COUNT) {
        s->restore_status_ok = false;
        nand_restore_set_reg(s, NAND_REG_RESTORE_C20, 0x00000000u);
        nand_restore_set_ready(s, true);
        return;
    }

    start_page = block * NAND_BLOCK_PAGES;
    start_off = start_page * NAND_PAGE_DATA;
    memset(s->image + start_off, 0xFF, NAND_BLOCK_PAGES * NAND_PAGE_DATA);
    s->dirty = true;
    s->block_data_state[block] = 0;
    s->restore_status_ok = true;
    nand_restore_set_reg(s, NAND_REG_RESTORE_C20, 0x00000001u);
    nand_restore_set_ready(s, true);
}

static void nand_restore_prepare_mode(nand_state_t *s, uint8_t mode)
{
    s->restore_mode = mode;
    s->restore_stream_pos = 0;
    s->restore_ecc_count = 0;

    switch (mode) {
    case 0x04u:
        s->restore_stream_base = NAND_PAGE_DATA + 8u;
        s->restore_stream_limit = 8u;
        nand_restore_set_ready(s, true);
        break;
    case 0x05u:
        if (s->restore_last_cmd == NAND_CMD_READ0)
            nand_restore_begin_read(s);
        s->restore_stream_base = 0;
        s->restore_stream_limit = NAND_PAGE_DATA + 8u;
        nand_restore_set_ready(s, true);
        break;
    case 0x06u:
        /*
         * NANDWRITER uses mode 6 after loading a page worth of data into the
         * B-window. Expose a zero syndrome / ECC payload and switch the buffer
         * cursor to the OOB write window so the subsequent 16-byte write lands
         * at page_buffer[512..527].
         */
        nand_restore_set_reg(s, NAND_REG_RESTORE_CA0, 0x00000000u);
        nand_restore_set_reg(s, NAND_REG_RESTORE_CA0 + 0x04u, 0x00000000u);
        nand_restore_set_reg(s, NAND_REG_RESTORE_CA0 + 0x08u, 0x00000000u);
        nand_restore_set_reg(s, NAND_REG_RESTORE_CA0 + 0x0Cu, 0x00000000u);
        nand_restore_set_reg(s, NAND_REG_RESTORE_CC0, 0x00000001u);
        s->restore_stream_base = NAND_PAGE_DATA;
        s->restore_stream_limit = NAND_PAGE_OOB;
        nand_restore_set_ready(s, true);
        break;
    case 0x07u:
        s->restore_stream_base = 0;
        s->restore_stream_limit = NAND_PAGE_DATA;
        nand_restore_set_ready(s, true);
        break;
    case 0x01u:
        nand_restore_set_reg(s, NAND_REG_RESTORE_CC0, 0x00000000u);
        nand_restore_set_ready(s, true);
        break;
    default:
        nand_restore_set_ready(s, true);
        break;
    }
}

static uint64_t nand_restore_buffer_read(nand_state_t *s, unsigned size)
{
    uint64_t val = 0;

    for (unsigned i = 0; i < size; i++) {
        uint8_t byte = 0xFFu;

        if (s->restore_stream_pos < s->restore_stream_limit) {
            uint32_t idx = s->restore_stream_base + s->restore_stream_pos;
            if (idx < sizeof(s->restore_page_buffer))
                byte = s->restore_page_buffer[idx];
            s->restore_stream_pos++;
        }
        val |= (uint64_t)byte << (8u * i);
    }

    return val;
}

static void nand_restore_buffer_write(nand_state_t *s, unsigned size,
                                      uint64_t value)
{
    for (unsigned i = 0; i < size; i++) {
        uint32_t idx;

        if (s->restore_stream_pos >= s->restore_stream_limit)
            break;

        idx = s->restore_stream_base + s->restore_stream_pos;
        if (idx < sizeof(s->restore_page_buffer))
            s->restore_page_buffer[idx] = (uint8_t)((value >> (8u * i)) & 0xFFu);
        s->restore_stream_pos++;
    }
}

bool nand_restore_handles_offset(uint32_t offset)
{
    if ((offset >= NAND_RESTORE_BUF_BASE && offset < NAND_RESTORE_BUF_END) ||
        (offset >= NAND_RESTORE_SIDE_BASE && offset < NAND_RESTORE_SIDE_END))
        return true;

    switch (offset) {
    case 0x1100u:
    case 0x1104u:
    case 0x1108u:
    case 0x110Cu:
    case 0x1128u:
    case 0x1B20u:
    case 0x1CC0u:
    case 0x1CC4u:
    case 0x1CC8u:
    case 0x1CCCu:
        return true;
    default:
        return false;
    }
}

uint64_t nand_restore_read(nand_state_t *s, uint32_t offset, unsigned size)
{
    uint64_t val = 0;

    if (offset >= NAND_RESTORE_BUF_BASE && offset < NAND_RESTORE_BASE) {
        val = nand_restore_buffer_read(s, size);
        goto out;
    }

    if (offset >= NAND_RESTORE_SIDE_BASE && offset < NAND_RESTORE_SIDE_END) {
        uint32_t reg = nand_restore_get_reg(s, offset);
        for (unsigned i = 0; i < size; i++)
            val |= (uint64_t)((reg >> (8u * i)) & 0xFFu) << (8u * i);
        goto out;
    }

    if (offset >= NAND_REG_RESTORE_C00 && offset <= NAND_REG_RESTORE_CC0) {
        uint32_t reg = nand_restore_get_reg(s, offset);

        if (offset == NAND_REG_RESTORE_C20 && s->restore_last_cmd == NAND_CMD_STATUS)
            reg = s->restore_status_ok ? 0x00000001u : 0x00000000u;

        for (unsigned i = 0; i < size; i++)
            val |= (uint64_t)((reg >> (8u * i)) & 0xFFu) << (8u * i);
        goto out;
    }

out:

    return val;
}

void nand_restore_write(nand_state_t *s, uint32_t offset, unsigned size,
                        uint64_t value)
{
    if (offset >= NAND_RESTORE_BUF_BASE && offset < NAND_RESTORE_BASE) {
        nand_restore_buffer_write(s, size, value);
        goto out;
    }

    if (offset >= NAND_RESTORE_SIDE_BASE && offset < NAND_RESTORE_SIDE_END) {
        nand_restore_set_reg(s, offset, (uint32_t)value);
        goto out;
    }

    if (offset == 0x1100u || offset == 0x1104u || offset == 0x1108u ||
        offset == 0x110Cu || offset == 0x1128u || offset == 0x1B20u ||
        offset == 0x1CC0u || offset == 0x1CC4u || offset == 0x1CC8u ||
        offset == 0x1CCCu) {
        nand_restore_set_reg(s, offset, (uint32_t)value);
        goto out;
    }

    if (offset == NAND_REG_RESTORE_C14) {
        s->restore_phase = (uint8_t)(value & 0xFFu);
        nand_restore_set_reg(s, offset, (uint32_t)value);
        goto out;
    }

    if (offset == NAND_REG_RESTORE_C20) {
        uint8_t byte = (uint8_t)(value & 0xFFu);

        nand_restore_set_reg(s, offset, (uint32_t)value);
        if (s->restore_phase == 0x03u) {
            s->restore_last_cmd = byte;
            if (byte == NAND_CMD_READ0) {
                s->restore_addr_count = 0;
                s->restore_status_ok = true;
                nand_restore_set_ready(s, false);
            } else if (byte == NAND_CMD_SEQIN) {
                s->restore_addr_count = 0;
                nand_restore_begin_program(s);
            } else if (byte == NAND_CMD_ERASE1) {
                s->restore_addr_count = 0;
                s->restore_status_ok = true;
                nand_restore_set_ready(s, false);
            } else if (byte == NAND_CMD_ERASE2) {
                nand_restore_commit_erase(s);
            } else if (byte == NAND_CMD_PAGEPROG) {
                nand_restore_commit_program(s);
            } else if (byte == NAND_CMD_STATUS) {
                nand_restore_set_reg(s, offset,
                    s->restore_status_ok ? 0x00000001u : 0x00000000u);
                nand_restore_set_ready(s, true);
            }
        } else if (s->restore_phase == 0x06u) {
            if (s->restore_addr_count < sizeof(s->restore_addr_bytes))
                s->restore_addr_bytes[s->restore_addr_count++] = byte;
            else {
                memmove(s->restore_addr_bytes, s->restore_addr_bytes + 1,
                    sizeof(s->restore_addr_bytes) - 1);
                s->restore_addr_bytes[sizeof(s->restore_addr_bytes) - 1] = byte;
            }

            if (s->restore_last_cmd == NAND_CMD_READ0 ||
                s->restore_last_cmd == NAND_CMD_SEQIN) {
                s->restore_page_addr = nand_restore_page_from_addr(s);
            } else if (s->restore_last_cmd == NAND_CMD_ERASE1) {
                s->restore_page_addr =
                    nand_restore_block_from_addr(s) * NAND_BLOCK_PAGES;
            }
        }
        goto out;
    }

    if (offset == NAND_REG_RESTORE_C64) {
        nand_restore_set_reg(s, offset, (uint32_t)value);
        nand_restore_prepare_mode(s, (uint8_t)(value & 0xFFu));
        goto out;
    }

    if (offset == NAND_REG_RESTORE_C68) {
        nand_restore_set_reg(s, offset, (uint32_t)value);
        if (s->restore_mode == 0x01u && s->restore_ecc_count < 6) {
            s->restore_ecc_words[s->restore_ecc_count++] =
                (uint16_t)(value & 0x03FFu);
            if (s->restore_ecc_count >= 6) {
                nand_restore_set_reg(s, NAND_REG_RESTORE_CA0, 0x00000000u);
                nand_restore_set_reg(s, NAND_REG_RESTORE_CA0 + 0x04u, 0x00000000u);
                nand_restore_set_reg(s, NAND_REG_RESTORE_CA0 + 0x08u, 0x00000000u);
                nand_restore_set_reg(s, NAND_REG_RESTORE_CA0 + 0x0Cu, 0x00000000u);
                nand_restore_set_reg(s, NAND_REG_RESTORE_CC0, 0x00000001u);
            }
        }
        goto out;
    }

    if (offset >= NAND_REG_RESTORE_C00 && offset <= NAND_REG_RESTORE_CC0) {
        nand_restore_set_reg(s, offset, (uint32_t)value);
        goto out;
    }

out:
    return;
}
