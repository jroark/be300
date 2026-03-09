#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "nand.h"

void nand_init(nand_state_t *s, const uint8_t *image, size_t size)
{
    memset(s, 0, sizeof(*s));
    s->image      = image;
    s->image_size = size;
    s->ready      = true;
    s->state      = NAND_STATE_IDLE;
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

void nand_write(nand_state_t *s, uint32_t offset, unsigned size,
                uint64_t value, bool log)
{
    (void)size;

    if (log)
        fprintf(stderr, "[NAND] W offset=0x%04X val=0x%04" PRIX64 "\n",
                offset, value);

    /* Control/timing registers — just latch */
    if (offset >= NAND_CTRL_BASE && offset < NAND_CTRL_END) {
        uint32_t idx = (offset - NAND_CTRL_BASE) >> 2;
        if (idx < 8) s->ctrl_regs[idx] = (uint32_t)value;
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

    /* Data port write — ignored for read-only emulation */
    if (offset == NAND_REG_DATA) {
        return;
    }

    /* Status/ready register write — some controllers allow clearing */
    if (offset == NAND_REG_READY) {
        return;
    }
}

uint64_t nand_read(nand_state_t *s, uint32_t offset, unsigned size, bool log)
{
    (void)size;
    uint64_t val = 0;

    /* Control/timing registers — return latched value */
    if (offset >= NAND_CTRL_BASE && offset < NAND_CTRL_END) {
        uint32_t idx = (offset - NAND_CTRL_BASE) >> 2;
        val = (idx < 8) ? s->ctrl_regs[idx] : 0;
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

    /* Device ID */
    if (offset == NAND_REG_DEVID) {
        val = 0xF237u;
        goto out;
    }

    /* Alternate status register */
    if (offset == NAND_REG_ALTSTS) {
        val = 0x40u; /* ready */
        goto out;
    }

    /* Data port — return next 16-bit LE from image */
    if (offset == NAND_REG_DATA) {
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

    /* Status register */
    if (offset == NAND_REG_STATUS) {
        val = 0x40u;  /* ready, no error */
        goto out;
    }

    /* Command/address range reads — return 0 */
    if (offset >= NAND_CMD_BASE && offset < NAND_CMD_END) {
        val = 0;
        goto out;
    }

out:
    if (log)
        fprintf(stderr, "[NAND] R offset=0x%04X -> 0x%04" PRIX64 "\n",
                offset, val);
    return val;
}
