#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "nand.h"

static uint8_t nand_image_byte(const nand_state_t *s, uint32_t off)
{
    if (!s->image || off >= s->image_size)
        return 0xFFu;
    return s->image[off];
}

void nand_init(nand_state_t *s, const uint8_t *image, size_t size)
{
    memset(s, 0, sizeof(*s));
    s->image      = image;
    s->image_size = size;
    s->ready      = true;
    s->state      = NAND_STATE_IDLE;
    s->portctl    = 0;
    memset(s->legacy_regs, 0x40, sizeof(s->legacy_regs));
}

/* Resolve current page_addr + column into a byte offset in the NAND image.
 * Sets nand_offset, xfer_length, and xfer_cursor. */
static void nand_setup_transfer(nand_state_t *s)
{
    uint32_t raw_offset = s->page_addr * NAND_PAGE_RAW;

    switch (s->state) {
    case NAND_STATE_READ_DATA:
        if (s->column < NAND_PAGE_DATA) {
            s->nand_offset = raw_offset + s->column;
            s->xfer_length = NAND_PAGE_DATA - s->column;
        } else {
            s->nand_offset = raw_offset + NAND_PAGE_DATA;
            s->xfer_length = 0;
        }
        break;
    case NAND_STATE_READ_OOB:
        s->nand_offset  = raw_offset + NAND_PAGE_DATA;
        s->xfer_length  = NAND_PAGE_OOB;
        break;
    case NAND_STATE_READ_ID:
        s->nand_offset  = 0;
        s->xfer_length  = 4;  /* ID bytes */
        break;
    default:
        s->nand_offset  = 0;
        s->xfer_length  = 0;
        break;
    }
    s->xfer_cursor = 0;
}

/* Stream read used by SPL transfer engine through 0xB000 window. */
static uint64_t nand_stream_read(nand_state_t *s, unsigned size)
{
    uint64_t val = 0;
    uint32_t base = s->stream_base + s->stream_cursor;

    for (unsigned i = 0; i < size; i++) {
        uint8_t byte = nand_image_byte(s, base + i);
        val |= ((uint64_t)byte) << (8u * i);
    }
    s->stream_cursor += size;
    return val;
}

void nand_write(nand_state_t *s, uint32_t offset, unsigned size,
                uint64_t value, bool log)
{
    if (log)
        fprintf(stderr, "[NAND] W%u offset=0x%04X val=0x%08" PRIX64 "\n",
                size * 8, offset, value);

    /* Control/timing registers — just latch */
    if (offset >= NAND_CTRL_BASE && offset < NAND_CTRL_END) {
        uint32_t idx = (offset - NAND_CTRL_BASE) >> 2;
        if (idx < 8) s->ctrl_regs[idx] = (uint32_t)value;
        return;
    }

    /* SPL transfer engine registers (0xA400-0xA4FF). */
    if (offset >= NAND_XFER_BASE && offset < NAND_XFER_END) {
        uint32_t idx = (offset - NAND_XFER_BASE) >> 2;
        uint8_t data_byte = (uint8_t)(value & 0xFFu);

        if (idx < (sizeof(s->xfer_regs) / sizeof(s->xfer_regs[0])))
            s->xfer_regs[idx] = (uint32_t)value;

        if (offset == NAND_REG_XFER_CMD) {
            /* New command phase starts a new A420 address sequence. */
            s->xfer_addr_count = 0;
        } else if (offset == NAND_REG_XFER_ADDR) {
            /* A420 feeds a 24-bit byte address (little-endian). */
            if (s->xfer_addr_count < sizeof(s->xfer_addr_bytes)) {
                s->xfer_addr_bytes[s->xfer_addr_count++] = data_byte;
            } else {
                memmove(s->xfer_addr_bytes, s->xfer_addr_bytes + 1,
                        sizeof(s->xfer_addr_bytes) - 1);
                s->xfer_addr_bytes[sizeof(s->xfer_addr_bytes) - 1] = data_byte;
            }

            if (s->xfer_addr_count >= 3) {
                s->xfer_addr24 = (uint32_t)s->xfer_addr_bytes[0]
                               | ((uint32_t)s->xfer_addr_bytes[1] << 8)
                               | ((uint32_t)s->xfer_addr_bytes[2] << 16);
            }
        } else if (offset == NAND_REG_XFER_KICK) {
            /* Engine kick toggles busy->ready in our simplified model. */
            s->ready = false;
        } else if (offset == NAND_REG_XFER_MODE) {
            /*
             * SPL uses mode 5 for the first 520-byte burst, then mode 4 for
             * the trailing 8-byte burst. Start stream on mode 5 and keep
             * cursor continuity across mode 4.
             */
            if (data_byte == 0x05u) {
                s->stream_base = s->xfer_addr24;
                s->stream_cursor = 0;
                s->stream_active = true;
            }
            s->ready = true;
        }
        return;
    }

    /* Enable register */
    if (offset >= NAND_ENABLE_BASE && offset < NAND_ENABLE_END) {
        s->enabled = (value & 1) != 0;
        if (log) fprintf(stderr, "[NAND] controller %s\n",
                         s->enabled ? "enabled" : "disabled");
        return;
    }

    /* Command register */
    if (offset == NAND_REG_CMD) {
        uint8_t cmd = (uint8_t)(value & 0xFF);
        s->last_cmd    = cmd;
        s->addr_cycle  = 0;
        s->xfer_cursor = 0;
        s->ready       = false;

        switch (cmd) {
        case NAND_CMD_READ0:
            s->state  = NAND_STATE_READ_DATA;
            s->column = 0;
            break;
        case NAND_CMD_READ1:
            s->state  = NAND_STATE_READ_DATA;
            s->column = 256;
            break;
        case NAND_CMD_READOOB:
            s->state = NAND_STATE_READ_OOB;
            break;
        case NAND_CMD_READID:
            s->state = NAND_STATE_READ_ID;
            nand_setup_transfer(s);
            s->ready = true;
            break;
        case NAND_CMD_RESET:
            s->state = NAND_STATE_IDLE;
            s->ready = true;
            break;
        default:
            if (log) fprintf(stderr, "[NAND] unknown cmd 0x%02X\n", cmd);
            s->state = NAND_STATE_IDLE;
            s->ready = true;
            break;
        }
        return;
    }

    /* Address byte register */
    if (offset == NAND_REG_ADDR) {
        uint8_t abyte = (uint8_t)(value & 0xFF);
        switch (s->addr_cycle) {
        case 0:
            /* Column address (within 256-byte half) */
            if (s->state == NAND_STATE_READ_DATA)
                s->column = (s->column & ~0xFFu) | abyte;
            break;
        case 1:
            /* Page address bits [7:0] → row bits [7:0] */
            s->page_addr = (s->page_addr & ~0xFFu) | abyte;
            break;
        case 2:
            /* Page address bits [15:8] → row bits [15:8] */
            s->page_addr = (s->page_addr & ~0xFF00u) | ((uint32_t)abyte << 8);
            break;
        case 3:
            /* Page address bits [23:16] → row bits [23:16] (if needed) */
            s->page_addr = (s->page_addr & ~0xFF0000u) | ((uint32_t)abyte << 16);
            break;
        }
        s->addr_cycle++;

        /* After all address cycles, set up the transfer */
        if (s->addr_cycle >= 3 && s->state != NAND_STATE_READ_ID) {
            nand_setup_transfer(s);
            s->ready = true;
        }
        return;
    }

    /* Legacy port control register */
    if (offset == NAND_REG_PORTCTL) {
        s->portctl = (uint16_t)(value & 0xFFFFu);
        return;
    }

    /* Indexed legacy data register writes (D7F8 selects byte register). */
    if (offset == NAND_REG_DATA) {
        uint32_t idx = (uint8_t)(s->portctl & 0xFFu);
        for (unsigned i = 0; i < size; i++)
            s->legacy_regs[(uint8_t)(idx + i)] = (uint8_t)((value >> (8u * i)) & 0xFFu);
        return;
    }

    /* Status/ready register write — some controllers allow clearing */
    if (offset == NAND_REG_READY) {
        return;
    }
}

uint64_t nand_read(nand_state_t *s, uint32_t offset, unsigned size, bool log)
{
    uint64_t val = 0;

    /* Control/timing registers — return latched value */
    if (offset >= NAND_CTRL_BASE && offset < NAND_CTRL_END) {
        uint32_t idx = (offset - NAND_CTRL_BASE) >> 2;
        val = (idx < 8) ? s->ctrl_regs[idx] : 0;
        goto out;
    }

    /* SPL transfer engine registers (0xA400-0xA4FF). */
    if (offset >= NAND_XFER_BASE && offset < NAND_XFER_END) {
        uint32_t idx = (offset - NAND_XFER_BASE) >> 2;
        if (offset == NAND_REG_XFER_STATUS) {
            val = s->ready ? 0x00000001u : 0u;
        } else if (idx < (sizeof(s->xfer_regs) / sizeof(s->xfer_regs[0]))) {
            val = s->xfer_regs[idx];
        } else {
            val = 0;
        }
        goto out;
    }

    /* SPL stream window (0xB000): return sequential data bytes. */
    if (offset >= NAND_STREAM_BASE && offset < NAND_STREAM_END) {
        if (!s->stream_active)
            val = (size >= 4) ? UINT32_C(0xFFFFFFFF) : UINT64_C(0xFF);
        else
            val = nand_stream_read(s, size);
        goto out;
    }

    /* Enable register */
    if (offset >= NAND_ENABLE_BASE && offset < NAND_ENABLE_END) {
        val = s->enabled ? 1 : 0;
        goto out;
    }

    /* Ready/status polling */
    if (offset == NAND_REG_READY) {
        val = s->ready ? 0x0001u : 0x0000u;
        goto out;
    }

    /* Legacy port control register */
    if (offset == NAND_REG_PORTCTL) {
        val = s->portctl;
        goto out;
    }

    /* Device ID */
    if (offset == NAND_REG_DEVID) {
        val = 0xF237u;
        goto out;
    }

    /* Data port — indexed legacy register protocol and NAND transfer data. */
    if (offset == NAND_REG_DATA) {
        uint32_t idx = (uint8_t)(s->portctl & 0xFFu);
        bool data_mode =
            (size >= 2) &&
            (s->state == NAND_STATE_READ_DATA ||
             s->state == NAND_STATE_READ_OOB ||
             s->state == NAND_STATE_READ_ID) &&
            (s->xfer_length > 0);

        if (data_mode) {
            if (!s->image || s->xfer_cursor >= s->xfer_length) {
                val = 0xFFFF;
                goto out;
            }
            uint32_t img_off = s->nand_offset + s->xfer_cursor;
            if (img_off + 1 < s->image_size) {
                val = s->image[img_off] | ((uint16_t)s->image[img_off + 1] << 8);
                s->xfer_cursor += 2;
            } else if (img_off < s->image_size) {
                val = s->image[img_off];
                s->xfer_cursor += 2;
            } else {
                val = 0xFFFF;
            }
            goto out;
        }

        for (unsigned i = 0; i < size; i++)
            val |= ((uint64_t)s->legacy_regs[(uint8_t)(idx + i)]) << (8u * i);
        goto out;
    }

    /* Status register */
    if (offset == NAND_REG_STATUS) {
        val = 0x80u;  /* ready */
        goto out;
    }

    /* Command/address range reads — return 0 */
    if (offset >= NAND_CMD_BASE && offset < NAND_CMD_END) {
        val = 0;
        goto out;
    }

out:
    if (log)
        fprintf(stderr, "[NAND] R%u offset=0x%04X -> 0x%08" PRIX64 "\n",
                size * 8, offset, val);
    return val;
}
