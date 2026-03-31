#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "host_io.h"
#include "nand.h"

static uint8_t nand_image_byte(const nand_state_t *s, uint32_t off)
{
    if (!s->image || off >= s->image_size)
        return 0xFFu;
    return s->image[off];
}

static uint8_t nand_stream_oob_byte(const nand_state_t *s,
                                    uint32_t page, uint32_t oob_idx)
{
    uint32_t block = page / NAND_BLOCK_PAGES;
    uint32_t page_in_block = page % NAND_BLOCK_PAGES;

    if (oob_idx >= NAND_PAGE_OOB)
        return 0xFFu;

    /*
     * The restore images are data-only dumps (no physical OOB bytes).
     * Synthesize minimal per-block metadata on page 0 so SPL ReadLogBlock()
     * validation can build a straightforward logical->physical map:
     *   OOB[0..1] = 0x55AA (little-endian: AA 55),
     *   OOB[2] = 0x0F, OOB[4..7] = logical block id.
     */
    if (page_in_block == 0 && block < NAND_BLOCK_COUNT) {
        switch (oob_idx) {
        case 0: return 0xAAu;
        case 1: return 0x55u;
        case 2: return 0x0Fu;
        case 4: return (uint8_t)(block & 0xFFu);
        case 5: return (uint8_t)((block >> 8) & 0xFFu);
        case 6: return 0x00u;
        case 7: return 0x00u;
        default: return 0xFFu;
        }
    }

    return 0xFFu;
}

static uint8_t nand_stream_byte(const nand_state_t *s, uint32_t cursor_pos)
{
    uint32_t absolute = s->stream_col + cursor_pos;
    uint32_t page = s->stream_page + (absolute / NAND_PAGE_RAW);
    uint32_t in_page = absolute % NAND_PAGE_RAW;

    if (in_page < NAND_PAGE_DATA) {
        uint32_t off = page * NAND_PAGE_DATA + in_page;
        return nand_image_byte(s, off);
    }
    return nand_stream_oob_byte(s, page, in_page - NAND_PAGE_DATA);
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

    for (unsigned i = 0; i < size; i++) {
        uint8_t byte = nand_stream_byte(s, s->stream_cursor + i);
        val |= ((uint64_t)byte) << (8u * i);
    }
    s->stream_cursor += size;
    return val;
}

/* Rate-limited indexed register logging */
static int nand_idx_log_count = 0;
#define NAND_IDX_LOG_MAX 50000
#define LEGACY_STATUS_IDX          0x07u
#define LEGACY_STATUS_ESCAPE_READS 64u
#define LEGACY_STATUS_IDLE_MASK    0xC0u
#define UART_TX_CONSOLE_PC         UINT32_C(0x80F03308)
#define UART_TX_CONSOLE_PC_CENET   UINT32_C(0x80F03350)
static int nand_stream_start_log_count = 0;
static int nand_stream_first_log_count = 0;
#define NAND_STREAM_LOG_MAX 8192

void nand_write(nand_state_t *s, uint32_t offset, unsigned size,
                uint64_t value, bool log, uint32_t pc)
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
            /* SPL traces show only subcommands 0x03/0x06 open address phases. */
            if (data_byte == 0x03u || data_byte == 0x06u)
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
             * Transfer mode register:
             *   0x00 = reset/idle
             *   0x01 = prepare for buffer read
             *   0x04 = 8-byte OOB/trailer into buffer regs
             *   0x05 = 520-byte stream burst via 0xB000
             */
            if (data_byte == 0x00u) {
                s->stream_active = false;
                s->stream_cursor = 0;
                s->xfer_buffer_valid = false;
                memset(s->xfer_buffer, 0, sizeof(s->xfer_buffer));
            } else if (data_byte == 0x01u) {
                s->xfer_buffer_valid = false;
            } else if (data_byte == 0x04u) {
                /* Mode 4: read 8 bytes into buffer registers.
                 * Activate stream from address if not already active. */
                if (!s->stream_active && s->xfer_addr_count >= 3) {
                    uint32_t row = (uint32_t)s->xfer_addr_bytes[1]
                                 | ((uint32_t)s->xfer_addr_bytes[2] << 8);
                    uint32_t col = (uint32_t)s->xfer_addr_bytes[0];
                    s->stream_page = row;
                    s->stream_col = col;
                    s->stream_base = row * NAND_PAGE_DATA + col;
                    s->stream_cursor = 0;
                    s->stream_active = true;
                }
                memset(s->xfer_buffer, 0xFF, sizeof(s->xfer_buffer));
                if (s->stream_active) {
                    for (int i = 0; i < 8; i++) {
                        int b = nand_stream_byte(s, s->stream_cursor + i);
                        if (b >= 0)
                            s->xfer_buffer[i] = (uint8_t)b;
                    }
                    s->stream_cursor += 8;
                }
                s->xfer_buffer_valid = true;
            } else if (data_byte == 0x05u) {
                uint32_t row = (uint32_t)s->xfer_addr_bytes[1]
                             | ((uint32_t)s->xfer_addr_bytes[2] << 8);
                uint32_t col = (uint32_t)s->xfer_addr_bytes[0];
                s->stream_page = row;
                s->stream_col = col;
                s->stream_base = row * NAND_PAGE_DATA + col;
                s->stream_cursor = 0;
                s->stream_active = true;
                if (log && nand_stream_start_log_count < NAND_STREAM_LOG_MAX) {
                    fprintf(stderr,
                            "[NAND_STREAM_START] pc=0x%08X mode=0x%02X"
                            " addr24=0x%06X bytes=[%02X %02X %02X]"
                            " row=0x%04X col=0x%02X base=0x%08X\n",
                            pc, data_byte,
                            (unsigned)s->xfer_addr24,
                            (unsigned)s->xfer_addr_bytes[0],
                            (unsigned)s->xfer_addr_bytes[1],
                            (unsigned)s->xfer_addr_bytes[2],
                            row, col, s->stream_base);
                    nand_stream_start_log_count++;
                }
            }
            s->ready = true;
        } else if (offset == NAND_REG_XFER_MISC) {
            /* WinCE writes 0x03FF repeatedly after mode/address setup.
             * Pre-fill buffer registers from stream if active. */
            if (s->stream_active && !s->xfer_buffer_valid) {
                memset(s->xfer_buffer, 0xFF, sizeof(s->xfer_buffer));
                for (int i = 0; i < 16; i++) {
                    int b = nand_stream_byte(s, s->stream_cursor + i);
                    if (b >= 0)
                        s->xfer_buffer[i] = (uint8_t)b;
                }
                s->xfer_buffer_valid = true;
            }
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
        if (log && nand_idx_log_count < NAND_IDX_LOG_MAX) {
            fprintf(stderr, "[NAND_IDX] SEL idx=0x%02X PC=0x%08X\n",
                    (unsigned)(value & 0xFF), pc);
            nand_idx_log_count++;
        }
        return;
    }

    /* Indexed legacy data register writes (D7F8 selects byte register). */
    if (offset == NAND_REG_DATA) {
        uint32_t idx = (uint8_t)(s->portctl & 0xFFu);
        for (unsigned i = 0; i < size; i++) {
            uint8_t ri = (uint8_t)(idx + i);
            uint8_t byte = (uint8_t)((value >> (8u * i)) & 0xFFu);
            bool was_dirty = s->legacy_dirty[ri];
            uint8_t old = s->legacy_regs[ri];

            s->legacy_regs[ri] = byte;
            s->legacy_dirty[ri] = true;

            if (ri == LEGACY_STATUS_IDX) {
                bool keep_read_count =
                    was_dirty && old == byte && (old & 0x1Bu) != 0;

                if (!keep_read_count)
                    s->legacy_read_since_write[ri] = 0;

                if ((byte & 0x1Bu) == 0) {
                    s->legacy_status7_event_reads = 0;
                    s->legacy_status7_ff_armed = false;
                }
            } else {
                s->legacy_read_since_write[ri] = 0;
            }

            /* Forward VRC4173 indexed UART data (idx 0x00) to stdout.
             * Normalize 0x80/0xA0 kseg aliases for the known console callsite. */
            uint32_t norm_pc = pc & ~UINT32_C(0x20000000);
            bool known_console_pc =
                norm_pc == UART_TX_CONSOLE_PC ||
                norm_pc == UART_TX_CONSOLE_PC_CENET;
            bool known_uart_idx = (ri == 0x00u || ri == 0xF0u);
            if (known_uart_idx &&
                known_console_pc &&
                (byte == 0x0A || byte == 0x0D || (byte >= 0x20 && byte <= 0x7E))) {
                host_io_emit_serial(byte);
                if (host_io_stdout_enabled()) {
                    putchar(byte);
                    fflush(stdout);
                }
            }
        }
        if (log && nand_idx_log_count < NAND_IDX_LOG_MAX) {
            fprintf(stderr, "[NAND_IDX] W idx=0x%02X val=0x%02X PC=0x%08X\n",
                    (unsigned)(idx & 0xFF), (unsigned)(value & 0xFF), pc);
            nand_idx_log_count++;
        }
        return;
    }

    /* Status/ready register write — some controllers allow clearing */
    if (offset == NAND_REG_READY) {
        return;
    }
}

uint64_t nand_read(nand_state_t *s, uint32_t offset, unsigned size, bool log, uint32_t pc)
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
        if (offset == NAND_REG_XFER_STATUS ||
            offset == NAND_REG_XFER_STATUS2) {
            val = s->ready ? 0x00000001u : 0u;
        } else if (offset >= NAND_REG_BUFFER_BASE &&
                   offset < NAND_REG_BUFFER_END) {
            /* WinCE reads 4 x 32-bit from the data buffer */
            uint32_t buf_off = offset - NAND_REG_BUFFER_BASE;
            val = 0;
            for (unsigned i = 0; i < size && (buf_off + i) < 16; i++)
                val |= (uint64_t)s->xfer_buffer[buf_off + i] << (i * 8);
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
        else {
            val = nand_stream_read(s, size);
            if (log && s->stream_cursor == size && nand_stream_first_log_count < NAND_STREAM_LOG_MAX) {
                uint32_t first_data_off = s->stream_page * NAND_PAGE_DATA + s->stream_col;
                fprintf(stderr,
                        "[NAND_STREAM_FIRST] pc=0x%08X page=0x%04X col=0x%02X"
                        " first_data_off=0x%08X size=%u val=0x%08" PRIX64 "\n",
                        pc, s->stream_page, s->stream_col,
                        first_data_off, size, val);
                nand_stream_first_log_count++;
            }
        }
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

        /* Legacy indexed register protocol:
         * - idx7 is treated as status and may transition over repeated reads.
         * - all non-idx7 registers are strict echo (especially idx0 command). */
        for (unsigned i = 0; i < size; i++) {
            uint8_t ri = (uint8_t)(idx + i);
            uint8_t raw = s->legacy_regs[ri];
            uint8_t byte_val = raw;
            uint16_t n = s->legacy_read_since_write[ri];

            /* Always track reads */
            if (n < UINT16_MAX)
                s->legacy_read_since_write[ri] = n + 1;

            if (ri != LEGACY_STATUS_IDX) {
                /* Non-status legacy indices are strict echo. */
                if (s->legacy_dirty[ri])
                    s->legacy_dirty[ri] = false;
            } else {
                bool has_events = (raw & 0x1Bu) != 0;
                bool idleish = !has_events &&
                               ((raw & LEGACY_STATUS_IDLE_MASK) != 0 || s->legacy_dirty[ri]);

                if (s->legacy_dirty[ri] && has_events) {
                    if (s->legacy_status7_event_reads < UINT16_MAX)
                        s->legacy_status7_event_reads++;

                    if (s->legacy_status7_event_reads >= LEGACY_STATUS_ESCAPE_READS) {
                        if (!s->legacy_status7_ff_armed) {
                            /* One-shot poll escape to satisfy loops waiting for 0xFF. */
                            byte_val = 0xFFu;
                            s->legacy_status7_ff_armed = true;
                            s->legacy_regs[ri] = LEGACY_STATUS_IDLE_MASK;
                        } else {
                            /* Subsequent read settles to idle and clears dirty state. */
                            byte_val = LEGACY_STATUS_IDLE_MASK;
                            s->legacy_regs[ri] = byte_val;
                            s->legacy_dirty[ri] = false;
                            s->legacy_read_since_write[ri] = 0;
                            s->legacy_status7_event_reads = 0;
                            s->legacy_status7_ff_armed = false;
                        }
                    } else if (n == 0) {
                        /* First read after write: unchanged. */
                        byte_val = raw;
                    } else if (n == 1) {
                        /* Second read: clear highest-priority event group. */
                        if (raw & 0x10u)
                            byte_val = raw & ~0x10u;
                        else if (raw & 0x05u)
                            byte_val = raw & ~0x05u;
                        else if (raw & 0x0Au)
                            byte_val = raw & ~0x0Au;
                        else
                            byte_val = raw & ~0x1Bu;
                        s->legacy_regs[ri] = byte_val;

                        if ((byte_val & 0x1Bu) == 0) {
                            byte_val = (byte_val & ~0x1Bu) | LEGACY_STATUS_IDLE_MASK;
                            s->legacy_regs[ri] = byte_val;
                            s->legacy_dirty[ri] = false;
                            s->legacy_read_since_write[ri] = 0;
                            s->legacy_status7_event_reads = 0;
                            s->legacy_status7_ff_armed = false;
                        }
                    } else {
                        /* Hold current status while waiting for either clear or escape. */
                        byte_val = raw;
                    }
                } else if (idleish) {
                    /* idx7 idle-complete state: present Ready semantics (bits 7 and 6). */
                    byte_val = (raw & ~0x1Bu) | LEGACY_STATUS_IDLE_MASK;
                    s->legacy_regs[ri] = byte_val;
                    s->legacy_dirty[ri] = false;
                    s->legacy_read_since_write[ri] = 0;
                    s->legacy_status7_event_reads = 0;
                    s->legacy_status7_ff_armed = false;
                } else {
                    /* Non-polling idx7 reads: strict echo and reset escape tracking. */
                    s->legacy_status7_event_reads = 0;
                    s->legacy_status7_ff_armed = false;
                }
            }

            if (ri == LEGACY_STATUS_IDX &&
                s->legacy_dirty[ri] == false &&
                s->legacy_status7_ff_armed == false &&
                (s->legacy_regs[ri] & 0x1Bu) == 0) {
                /* Ensure idle bit is retained after status completion. */
                if ((s->legacy_regs[ri] & LEGACY_STATUS_IDLE_MASK) == 0) {
                    byte_val = (s->legacy_regs[ri] & ~0x1Bu) | LEGACY_STATUS_IDLE_MASK;
                    s->legacy_regs[ri] = byte_val;
                }
            }

            val |= ((uint64_t)byte_val) << (8u * i);
        }
        if (log && nand_idx_log_count < NAND_IDX_LOG_MAX) {
            fprintf(stderr, "[NAND_IDX] R idx=0x%02X val=0x%02X PC=0x%08X\n",
                    (unsigned)(idx & 0xFF), (unsigned)(val & 0xFF), pc);
            nand_idx_log_count++;
        }
        goto out;
    }

    /* Status register */
    if (offset == NAND_REG_STATUS) {
        /* At the EBOOT NIC probe sites, return "not ready" to skip NE2000Init.
         * The SPL reads D7FE and tests bit 0x80 to decide whether to enter the
         * EDBG loop (infinite, no timeout). Clearing bit 0x80 makes it take
         * the "no board" path → falls through to NAND boot at 0x80F03B64. */
        uint32_t norm_pc = pc & ~UINT32_C(0x20000000);
        if (norm_pc == 0x80F022C4u ||
            norm_pc == 0x80F02324u ||
            norm_pc == 0x80F09E1Cu) {
            val = 0x00u;  /* bit 0x80 clear → no NIC */
        } else {
            val = s->ready ? 0x80u : 0x00u;
        }
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
