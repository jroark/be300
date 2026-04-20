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

static bool nand_block_has_data(nand_state_t *s, uint32_t block)
{
    uint32_t start_off;
    uint32_t limit;

    if (block >= NAND_BLOCK_COUNT)
        return false;

    if (s->block_data_state[block] >= 0)
        return s->block_data_state[block] != 0;

    if (!s->image) {
        s->block_data_state[block] = 0;
        return false;
    }

    start_off = block * NAND_BLOCK_PAGES * NAND_PAGE_DATA;
    limit = NAND_BLOCK_PAGES * NAND_PAGE_DATA;
    if (start_off >= s->image_size) {
        s->block_data_state[block] = 0;
        return false;
    }
    if (limit > s->image_size - start_off)
        limit = (uint32_t)(s->image_size - start_off);

    for (uint32_t i = 0; i < limit; i++) {
        if (s->image[start_off + i] != 0xFFu) {
            s->block_data_state[block] = 1;
            return true;
        }
    }

    s->block_data_state[block] = 0;
    return false;
}

static bool nand_sector_has_fat16_bpb(const nand_state_t *s, uint32_t sector)
{
    uint32_t off = sector * NAND_PAGE_DATA;

    if (!s->image || off > s->image_size || s->image_size - off < NAND_PAGE_DATA)
        return false;

    return nand_image_byte(s, off + 0x1FEu) == 0x55u &&
           nand_image_byte(s, off + 0x1FFu) == 0xAAu &&
           nand_image_byte(s, off + 0x36u) == 'F' &&
           nand_image_byte(s, off + 0x37u) == 'A' &&
           nand_image_byte(s, off + 0x38u) == 'T' &&
           nand_image_byte(s, off + 0x39u) == '1' &&
           nand_image_byte(s, off + 0x3Au) == '6';
}

static bool nand_build_sector0_mbr(const nand_state_t *s,
                                   uint8_t out[NAND_PAGE_DATA],
                                   uint32_t *part_lba_out,
                                   uint32_t *part_size_out)
{
    uint32_t part_lba = 0;
    uint32_t part_size = 0;

    if (!s->image || s->image_size < NAND_PAGE_DATA)
        return false;

    if (nand_image_byte(s, 0x1FEu) == 0x55u &&
        nand_image_byte(s, 0x1FFu) == 0xAAu) {
        return false;
    }

    for (uint32_t off = 0; off + 8 <= 0x40u; off += 4) {
        uint32_t start;
        uint32_t size;
        uint64_t end_off;

        start = (uint32_t)nand_image_byte(s, off) |
                ((uint32_t)nand_image_byte(s, off + 1) << 8) |
                ((uint32_t)nand_image_byte(s, off + 2) << 16) |
                ((uint32_t)nand_image_byte(s, off + 3) << 24);
        size = (uint32_t)nand_image_byte(s, off + 4) |
               ((uint32_t)nand_image_byte(s, off + 5) << 8) |
               ((uint32_t)nand_image_byte(s, off + 6) << 16) |
               ((uint32_t)nand_image_byte(s, off + 7) << 24);

        if (start == 0 || size == 0 || start == 0xFFFFFFFFu || size == 0xFFFFFFFFu)
            continue;

        end_off = ((uint64_t)start + (uint64_t)size) * NAND_PAGE_DATA;
        if (end_off > s->image_size)
            continue;

        if (!nand_sector_has_fat16_bpb(s, start))
            continue;

        part_lba = start;
        part_size = size;
        break;
    }

    if (part_lba == 0 || part_size == 0)
        return false;

    memset(out, 0, NAND_PAGE_DATA);
    out[0x1BEu + 0] = 0x80u;  /* active partition */
    out[0x1BEu + 1] = 0xFFu;  /* CHS start (unused by ROM) */
    out[0x1BEu + 2] = 0xFFu;
    out[0x1BEu + 3] = 0xFFu;
    out[0x1BEu + 4] = 0x06u;  /* FAT16 */
    out[0x1BEu + 5] = 0xFFu;  /* CHS end */
    out[0x1BEu + 6] = 0xFFu;
    out[0x1BEu + 7] = 0xFFu;
    out[0x1BEu + 8] = (uint8_t)(part_lba & 0xFFu);
    out[0x1BEu + 9] = (uint8_t)((part_lba >> 8) & 0xFFu);
    out[0x1BEu + 10] = (uint8_t)((part_lba >> 16) & 0xFFu);
    out[0x1BEu + 11] = (uint8_t)((part_lba >> 24) & 0xFFu);
    out[0x1BEu + 12] = (uint8_t)(part_size & 0xFFu);
    out[0x1BEu + 13] = (uint8_t)((part_size >> 8) & 0xFFu);
    out[0x1BEu + 14] = (uint8_t)((part_size >> 16) & 0xFFu);
    out[0x1BEu + 15] = (uint8_t)((part_size >> 24) & 0xFFu);
    out[0x1FEu] = 0x55u;
    out[0x1FFu] = 0xAAu;

    if (part_lba_out)
        *part_lba_out = part_lba;
    if (part_size_out)
        *part_size_out = part_size;
    return true;
}

static bool nand_pc_in_boot_rom(uint32_t pc)
{
    uint32_t norm_pc = pc & ~UINT32_C(0x20000000);

    return norm_pc >= UINT32_C(0x9FC00000) &&
           norm_pc < UINT32_C(0x9FC04000);
}

static uint8_t nand_dma_byte(nand_state_t *s, uint32_t byte_off,
                             uint32_t pc)
{
    if (byte_off < NAND_PAGE_DATA && !nand_pc_in_boot_rom(pc)) {
        uint8_t mbr[NAND_PAGE_DATA];

        if (nand_build_sector0_mbr(s, mbr, NULL, NULL))
            return mbr[byte_off];
    }

    /*
     * DMA FIFO data read.  The caller supplies a fully resolved byte offset
     * into the NAND image (dma_nand_addr + dma_cursor), where dma_nand_addr
     * was converted from the page number in cmd[3..6] at trigger time.
     *
     * The ROM uses this path for ALL NAND regions: partition table (page 0),
     * SPL (page 0x20 = offset 0x4000), and later the FAT filesystem.
     */
    return nand_image_byte(s, byte_off);
}

static uint8_t nand_stream_oob_byte(nand_state_t *s,
                                    uint32_t page, uint32_t oob_idx)
{
    uint32_t block = page / NAND_BLOCK_PAGES;
    uint32_t page_in_block = page % NAND_BLOCK_PAGES;

    if (oob_idx >= NAND_PAGE_OOB)
        return 0xFFu;

    /*
     * The restore images are data-only dumps (no physical OOB bytes).
     * The ROM/SPL logical-block mapper votes across the first five pages of a
     * block and expects the same tag on at least three of them, so mirror the
     * synthetic block metadata across that opening page set:
     *   OOB[0..1] = 0x55AA (little-endian: AA 55),
     *   OOB[2] = 0x0F, OOB[4..7] = logical block id.
     */
    if (page_in_block < 5 &&
        block < NAND_BLOCK_COUNT &&
        nand_block_has_data(s, block)) {
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

static uint8_t nand_stream_byte(nand_state_t *s, uint32_t cursor_pos)
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

static void nand_restore_seed_regs(nand_state_t *s)
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
        s->block_data_state[block] = -1;
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

static void nand_commit_program(nand_state_t *s)
{
    uint32_t page_off;
    uint32_t limit;
    uint32_t i;
    uint32_t block;

    if (!s->image || s->page_addr >= (NAND_BLOCK_COUNT * NAND_BLOCK_PAGES))
        return;

    page_off = s->page_addr * NAND_PAGE_DATA;
    limit = s->program_length;
    if (s->program_column + limit > NAND_PAGE_DATA)
        limit = (s->program_column < NAND_PAGE_DATA)
            ? (NAND_PAGE_DATA - s->program_column) : 0;

    for (i = 0; i < limit; i++)
        s->image[page_off + s->program_column + i] &= s->program_buffer[i];

    if (limit > 0) {
        s->dirty = true;
        block = s->page_addr / NAND_BLOCK_PAGES;
        if (block < NAND_BLOCK_COUNT)
            s->block_data_state[block] = -1;
    }
}

static void nand_commit_erase(nand_state_t *s)
{
    uint32_t block;
    uint32_t start_page;
    uint32_t start_off;

    if (!s->image)
        return;

    block = s->page_addr / NAND_BLOCK_PAGES;
    if (block >= NAND_BLOCK_COUNT)
        return;

    start_page = block * NAND_BLOCK_PAGES;
    start_off = start_page * NAND_PAGE_DATA;
    memset(s->image + start_off, 0xFF, NAND_BLOCK_PAGES * NAND_PAGE_DATA);
    s->dirty = true;
    s->block_data_state[block] = 0;
}

void nand_init(nand_state_t *s, uint8_t *image, size_t size)
{
    memset(s, 0, sizeof(*s));
    s->image      = image;
    s->image_size = size;
    s->ready      = true;
    s->state      = NAND_STATE_IDLE;
    s->status_value = 0xC0u;
    s->portctl    = 0;
    memset(s->block_data_state, -1, sizeof(s->block_data_state));
    memset(s->legacy_regs, 0x40, sizeof(s->legacy_regs));

    /*
     * Survey-backed companion/NAND strap block from real hardware.
     * WinCE samples 0x0A00A0E0 bit 0 here to decide whether RAM sizing
     * is 16 MB or 32 MB; leaving the window as zero makes it choose 32 MB
     * and clear beyond installed SDRAM.
     */
    {
        static const struct {
            uint16_t off;
            uint32_t val;
        } strap_init[] = {
            { 0xA060u, 0x000C000Cu }, { 0xA064u, 0x000C000Cu },
            { 0xA068u, 0x000C000Cu }, { 0xA06Cu, 0x000C000Cu },
            { 0xA070u, 0x000C000Cu }, { 0xA074u, 0x000C000Cu },
            { 0xA078u, 0x000C000Cu }, { 0xA07Cu, 0x000C000Cu },
            { 0xA080u, 0x00000000u }, { 0xA084u, 0x00000001u },
            { 0xA088u, 0x00000000u }, { 0xA08Cu, 0x00007100u },
            { 0xA090u, 0x00007100u }, { 0xA094u, 0x00007100u },
            { 0xA098u, 0x00007100u }, { 0xA09Cu, 0x00007100u },
            { 0xA0A0u, 0x00007100u }, { 0xA0A4u, 0x00007100u },
            { 0xA0A8u, 0x00007100u }, { 0xA0ACu, 0x00007100u },
            { 0xA0B0u, 0x00007100u }, { 0xA0B4u, 0x00007100u },
            { 0xA0B8u, 0x00007100u }, { 0xA0BCu, 0x00007100u },
            { 0xA0C0u, 0x00007100u }, { 0xA0C4u, 0x00000003u },
            { 0xA0C8u, 0x0000FFFFu }, { 0xA0CCu, 0x00000000u },
            { 0xA0D0u, 0x00000002u }, { 0xA0D4u, 0x00000000u },
            { 0xA0D8u, 0x00000000u }, { 0xA0DCu, 0x00000005u },
            { 0xA0E0u, 0x00000041u }, { 0xA0E4u, 0x0000000Fu },
            { 0xA0E8u, 0x00000000u }, { 0xA0ECu, 0x00000000u },
            { 0xA0F0u, 0x00000000u }, { 0xA0F4u, 0x00007100u },
            { 0xA0F8u, 0x00007100u }, { 0xA0FCu, 0x00007100u },
            { 0xA100u, 0x00000000u }, { 0xA104u, 0x00000000u },
            { 0xA108u, 0x00000000u }, { 0xA10Cu, 0x00000000u },
            { 0xA110u, 0x00000000u }, { 0xA114u, 0x00000000u },
            { 0xA118u, 0x00000000u }, { 0xA11Cu, 0x00000000u },
            { 0xA120u, 0x00000000u }, { 0xA124u, 0x00000000u },
            { 0xA128u, 0x00000000u }, { 0xA12Cu, 0x00000000u },
            { 0xA130u, 0x00000000u }, { 0xA134u, 0x00000000u },
            { 0xA138u, 0x00000000u }, { 0xA13Cu, 0x00000000u },
            { 0xA140u, 0x00000001u }, { 0xA144u, 0x00000000u },
            { 0xA148u, 0x00000000u }, { 0xA14Cu, 0x00000000u },
            { 0xA150u, 0x00000000u }, { 0xA154u, 0x00000000u },
            { 0xA158u, 0x00000000u }, { 0xA15Cu, 0x00000000u },
        };
        size_t i;

        for (i = 0; i < sizeof(strap_init) / sizeof(strap_init[0]); i++) {
            uint32_t rel = (uint32_t)(strap_init[i].off - 0xA060u);
            memcpy(&s->strap_regs[rel], &strap_init[i].val, sizeof(uint32_t));
        }
    }

    nand_restore_seed_regs(s);
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

static void nand_xfer_clear_data(nand_state_t *s)
{
    s->xfer_data_count = 0;
    s->xfer_data_pos = 0;
    memset(s->xfer_data_bytes, 0, sizeof(s->xfer_data_bytes));
}

static void nand_xfer_push_data(nand_state_t *s, uint8_t byte)
{
    if (s->xfer_data_count < sizeof(s->xfer_data_bytes))
        s->xfer_data_bytes[s->xfer_data_count++] = byte;
}

static uint32_t nand_xfer_row_addr(const nand_state_t *s)
{
    return (uint32_t)s->xfer_addr_bytes[1]
         | ((uint32_t)s->xfer_addr_bytes[2] << 8);
}

static uint32_t nand_xfer_col_addr(const nand_state_t *s)
{
    return (uint32_t)s->xfer_addr_bytes[0];
}

static uint32_t nand_xfer_block_addr(const nand_state_t *s)
{
    if (s->xfer_addr_count < 2)
        return s->page_addr / NAND_BLOCK_PAGES;

    uint32_t block = ((uint32_t)(s->xfer_addr_bytes[0] >> 5) & 0x7u)
                   | ((uint32_t)s->xfer_addr_bytes[1] << 3);

    if (s->xfer_addr_count >= 3)
        block |= ((uint32_t)s->xfer_addr_bytes[2] << 11);
    return block;
}

static void nand_xfer_latch_addr_state(nand_state_t *s)
{
    if (s->xfer_last_cmd == NAND_CMD_READ0 ||
        s->xfer_last_cmd == NAND_CMD_READ1 ||
        s->xfer_last_cmd == NAND_CMD_READOOB ||
        s->xfer_last_cmd == NAND_CMD_SEQIN) {
        if (s->xfer_addr_count >= 3) {
            s->page_addr = nand_xfer_row_addr(s);
            s->column = nand_xfer_col_addr(s);
        }
    } else if (s->xfer_last_cmd == NAND_CMD_ERASE1) {
        if (s->xfer_addr_count >= 2)
            s->page_addr = nand_xfer_block_addr(s) * NAND_BLOCK_PAGES;
    }
}

static void nand_xfer_prepare_readid(nand_state_t *s)
{
    nand_xfer_clear_data(s);
    nand_xfer_push_data(s, 0xECu);
    nand_xfer_push_data(s, 0x73u);
}

static void nand_xfer_prepare_status(nand_state_t *s)
{
    nand_xfer_clear_data(s);
    nand_xfer_push_data(s, s->status_value);
}

static void nand_xfer_commit_phase(nand_state_t *s)
{
    uint8_t byte = s->xfer_pending_byte;

    switch (s->xfer_phase) {
    case 0x03u:
        s->xfer_last_cmd = byte;
        switch (byte) {
        case NAND_CMD_READ0:
            s->state = NAND_STATE_READ_DATA;
            s->column = 0;
            s->xfer_addr_count = 0;
            memset(s->xfer_addr_bytes, 0, sizeof(s->xfer_addr_bytes));
            nand_xfer_clear_data(s);
            break;
        case NAND_CMD_READ1:
            s->state = NAND_STATE_READ_DATA;
            s->column = 256;
            s->xfer_addr_count = 0;
            memset(s->xfer_addr_bytes, 0, sizeof(s->xfer_addr_bytes));
            nand_xfer_clear_data(s);
            break;
        case NAND_CMD_READOOB:
            s->state = NAND_STATE_READ_OOB;
            s->xfer_addr_count = 0;
            memset(s->xfer_addr_bytes, 0, sizeof(s->xfer_addr_bytes));
            nand_xfer_clear_data(s);
            break;
        case NAND_CMD_READID:
            s->state = NAND_STATE_READ_ID;
            s->xfer_addr_count = 0;
            memset(s->xfer_addr_bytes, 0, sizeof(s->xfer_addr_bytes));
            nand_xfer_clear_data(s);
            s->ready = true;
            break;
        case NAND_CMD_STATUS:
            s->state = NAND_STATE_READ_STATUS;
            s->ready = true;
            nand_xfer_prepare_status(s);
            break;
        case NAND_CMD_SEQIN:
            s->state = NAND_STATE_PROGRAM_DATA;
            s->column = 0;
            s->program_column = 0;
            s->program_length = 0;
            s->xfer_addr_count = 0;
            memset(s->xfer_addr_bytes, 0, sizeof(s->xfer_addr_bytes));
            memset(s->program_buffer, 0xFF, sizeof(s->program_buffer));
            nand_xfer_clear_data(s);
            break;
        case NAND_CMD_ERASE1:
            s->state = NAND_STATE_ERASE_SETUP;
            s->xfer_addr_count = 0;
            memset(s->xfer_addr_bytes, 0, sizeof(s->xfer_addr_bytes));
            nand_xfer_clear_data(s);
            break;
        case NAND_CMD_PAGEPROG:
            if (s->state == NAND_STATE_PROGRAM_DATA)
                nand_commit_program(s);
            s->state = NAND_STATE_IDLE;
            s->status_value = 0xC0u;
            s->ready = true;
            nand_xfer_prepare_status(s);
            break;
        case NAND_CMD_ERASE2:
            if (s->state == NAND_STATE_ERASE_SETUP) {
                s->page_addr = nand_xfer_block_addr(s) * NAND_BLOCK_PAGES;
                nand_commit_erase(s);
            }
            s->state = NAND_STATE_IDLE;
            s->status_value = 0xC0u;
            s->ready = true;
            nand_xfer_prepare_status(s);
            break;
        case NAND_CMD_RESET:
            s->state = NAND_STATE_IDLE;
            s->ready = true;
            s->status_value = 0xC0u;
            nand_xfer_clear_data(s);
            break;
        default:
            nand_xfer_clear_data(s);
            s->ready = true;
            break;
        }
        break;
    case 0x06u:
        if (s->xfer_last_cmd == NAND_CMD_READID) {
            nand_xfer_prepare_readid(s);
        } else if (s->xfer_last_cmd == NAND_CMD_STATUS) {
            nand_xfer_prepare_status(s);
        } else if (s->xfer_last_cmd == NAND_CMD_READ0 ||
                   s->xfer_last_cmd == NAND_CMD_READ1 ||
                   s->xfer_last_cmd == NAND_CMD_READOOB ||
                   s->xfer_last_cmd == NAND_CMD_SEQIN) {
            s->page_addr = nand_xfer_row_addr(s);
            s->column = nand_xfer_col_addr(s);
        } else if (s->xfer_last_cmd == NAND_CMD_ERASE1) {
            s->page_addr = nand_xfer_block_addr(s) * NAND_BLOCK_PAGES;
        }
        s->ready = true;
        break;
    default:
        break;
    }
}

/* Stream read used by SPL transfer engine through 0xB000 window. */
static uint64_t nand_stream_read(nand_state_t *s, unsigned size)
{
    uint64_t val = 0;

    for (unsigned i = 0; i < size; i++) {
        uint8_t byte;

        if (s->stream_limit != 0 && (s->stream_cursor + i) >= s->stream_limit)
            byte = 0xFFu;
        else
            byte = nand_stream_byte(s, s->stream_cursor + i);
        val |= ((uint64_t)byte) << (8u * i);
    }
    s->stream_cursor += size;
    return val;
}

static void nand_boot_reset(nand_state_t *s)
{
    s->boot_ready = false;
    s->boot_mode = 0;
    s->boot_addr_count = 0;
    s->boot_ecc_count = 0;
    s->stream_active = false;
    s->stream_limit = 0;
    s->boot_regs[(NAND_REG_BOOT_STATUS - NAND_BOOT_BASE) >> 2] = 0;
    s->boot_regs[(NAND_REG_BOOT_STATUS2 - NAND_BOOT_BASE) >> 2] = 0;
}

static void nand_boot_latch_addr(nand_state_t *s, uint8_t data_byte)
{
    if (s->boot_addr_count < sizeof(s->boot_addr_bytes)) {
        s->boot_addr_bytes[s->boot_addr_count++] = data_byte;
    } else {
        memmove(s->boot_addr_bytes, s->boot_addr_bytes + 1,
                sizeof(s->boot_addr_bytes) - 1);
        s->boot_addr_bytes[sizeof(s->boot_addr_bytes) - 1] = data_byte;
    }
}

static void nand_boot_start_stream(nand_state_t *s)
{
    uint32_t page;
    uint32_t col;

    col = s->boot_addr_bytes[0];
    page = (uint32_t)s->boot_addr_bytes[1]
         | ((uint32_t)(s->boot_addr_bytes[2] & 0x7Fu) << 8);

    s->stream_page = page;
    s->stream_col = col;
    s->stream_base = page * NAND_PAGE_RAW + col;
    s->stream_cursor = 0;
    s->stream_limit = NAND_PAGE_RAW - col;
    s->stream_active = true;
    s->boot_ready = true;
    s->boot_regs[(NAND_REG_BOOT_STATUS - NAND_BOOT_BASE) >> 2] = 1;
    s->boot_regs[(NAND_REG_BOOT_STATUS2 - NAND_BOOT_BASE) >> 2] = 0;
}

static void nand_boot_finish_ecc_input(nand_state_t *s)
{
    /*
     * The real HW ECC engine computes syndrome = stored_ECC XOR computed_ECC.
     * Since the emulated NAND data is bit-perfect (no flash degradation),
     * the correct syndrome is always zero.  Output all zeros so the ROM's
     * software ECC correction (FUN_9fc01828) finds no errors and leaves the
     * data untouched.
     */
    s->boot_regs[(NAND_REG_BOOT_ECC_OUT_BASE - NAND_BOOT_BASE) >> 2] = 0;
    s->boot_regs[((NAND_REG_BOOT_ECC_OUT_BASE + 0x04u) - NAND_BOOT_BASE) >> 2] = 0;
    s->boot_regs[((NAND_REG_BOOT_ECC_OUT_BASE + 0x08u) - NAND_BOOT_BASE) >> 2] = 0;
    s->boot_regs[((NAND_REG_BOOT_ECC_OUT_BASE + 0x0Cu) - NAND_BOOT_BASE) >> 2] = 0;
    s->boot_regs[(NAND_REG_BOOT_STATUS2 - NAND_BOOT_BASE) >> 2] = 0;
}

/* Rate-limited indexed register logging */
#define LEGACY_STATUS_IDX          0x07u
#define LEGACY_STATUS_ESCAPE_READS 64u
#define LEGACY_STATUS_IDLE_MASK    0xC0u
#define UART_TX_CONSOLE_PC         UINT32_C(0x80F03308)
#define UART_TX_CONSOLE_PC_CENET   UINT32_C(0x80F03350)
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

void nand_write(nand_state_t *s, uint32_t offset, unsigned size,
                uint64_t value, uint32_t pc)
{

    /* Control/timing registers — latch + functional status */
    if (offset >= NAND_CTRL_BASE && offset < NAND_CTRL_END) {
        uint32_t idx = (offset - NAND_CTRL_BASE) >> 2;
        if (idx < 8) s->ctrl_regs[idx] = (uint32_t)value;

        /* Write to NFCNT (0xA000) controls the status register (0xA008).
         * On real hardware, enabling the controller sets NFSTAT to 0x2F9
         * (ready). The ROM polls NFSTAT after writing NFCNT. */
        if (offset == NAND_CTRL_BASE) {
            if (value & 1u)
                s->ctrl_regs[2] = 0x000002F9u;  /* ready (from HW dump) */
            else
                s->ctrl_regs[2] = 0;
        }
        return;
    }

    if (offset >= 0xA060u && offset < 0xA160u) {
        uint32_t rel = offset - 0xA060u;

        /*
         * Board-ID strap at 0xA0C0 is a read-only hardware strap on
         * real BE-300 (hw_survey_v8.txt:9 "BoardDetect: 0x7100";
         * hw_dump_vrc4173.txt:524 shows 0x7100 AFTER boot even though
         * NK's PPSH_reset_port_ac000124 writes 0xC to this offset as a
         * side-effect of probing 0xAC000124). Drop writes so pcmcia.dll's
         * FUN_C at UM 0x0198a9c8 can still read 0x7100 after NK's write.
         */
        for (unsigned i = 0; i < size && (rel + i) < sizeof(s->strap_regs); i++) {
            uint32_t abs_off = offset + i;
            if (abs_off >= 0xA0C0u && abs_off < 0xA0C4u)
                continue;
            s->strap_regs[rel + i] = (uint8_t)((value >> (8u * i)) & 0xFFu);
        }
        return;
    }

    /* SPL transfer engine registers (0xA400-0xA4FF). */
    if (offset >= NAND_XFER_BASE && offset < NAND_XFER_END) {
        uint32_t idx = (offset - NAND_XFER_BASE) >> 2;
        uint8_t data_byte = (uint8_t)(value & 0xFFu);

        if (idx < (sizeof(s->xfer_regs) / sizeof(s->xfer_regs[0])))
            s->xfer_regs[idx] = (uint32_t)value;

        if (offset == NAND_REG_XFER_CMD) {
            s->xfer_phase = data_byte;
            /* SPL traces show only subcommands 0x03/0x06 open command/address phases. */
            if (data_byte == 0x03u || data_byte == 0x06u)
                s->xfer_addr_count = 0;
        } else if (offset == NAND_REG_XFER_ADDR) {
            s->xfer_pending_byte = data_byte;

            if (s->xfer_phase == 0x06u) {
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

                nand_xfer_latch_addr_state(s);
            }
        } else if (offset == NAND_REG_XFER_ACK) {
            nand_xfer_commit_phase(s);
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
             *   0x06 = final 8-byte OOB/ECC write window via 0xB000
             *   0x07 = 520-byte program burst (512 data + first 8 OOB)
             */
            if (data_byte == 0x00u || data_byte == 0x01u) {
                s->xfer_ecc_count = 0;
                memset(s->xfer_buffer, 0x00, sizeof(s->xfer_buffer));
                s->xfer_regs[(NAND_REG_XFER_STATUS2 - NAND_XFER_BASE) >> 2] = 0;
                s->stream_base = 0;
                s->stream_cursor = 0;
                s->stream_limit = 0;
                s->stream_active = false;
            } else if (data_byte == 0x04u) {
                /*
                 * Mode-4 fills the small result buffer from the current
                 * stream position.  The ROM/SPL may request this before NK.exe,
                 * so this must be hardware state rather than a WinCE phase gate.
                 */
                if (!s->stream_active && s->xfer_addr_count >= 3) {
                    uint32_t row = (uint32_t)s->xfer_addr_bytes[1]
                                 | ((uint32_t)s->xfer_addr_bytes[2] << 8);
                    uint32_t col = (uint32_t)s->xfer_addr_bytes[0];
                    s->stream_page = row;
                    s->stream_col = col;
                    s->stream_base = row * NAND_PAGE_RAW + col;
                    s->stream_cursor = 0;
                    s->stream_active = true;
                }
                memset(s->xfer_buffer, 0x00, sizeof(s->xfer_buffer));
                if (s->stream_active) {
                    for (int i = 0; i < 16; i++)
                        s->xfer_buffer[i] = nand_stream_byte(s,
                            s->stream_cursor + i);
                }
            } else if (data_byte == 0x05u) {
                uint32_t row = (uint32_t)s->xfer_addr_bytes[1]
                             | ((uint32_t)s->xfer_addr_bytes[2] << 8);
                uint32_t col = (uint32_t)s->xfer_addr_bytes[0];
                s->stream_page = row;
                s->stream_col = col;
                s->stream_base = row * NAND_PAGE_RAW + col;
                s->stream_cursor = 0;
                s->stream_limit = NAND_PAGE_RAW - col;
                s->stream_active = true;
            } else if (data_byte == 0x06u) {
                s->stream_base = NAND_PAGE_DATA + 8u;
                s->stream_cursor = 0;
                s->stream_limit = 8u;
                s->stream_active = false;
            } else if (data_byte == 0x07u) {
                nand_xfer_latch_addr_state(s);
                s->stream_base = 0;
                s->stream_cursor = 0;
                s->stream_limit = NAND_PAGE_DATA + 8u;
                s->stream_active = false;
            }
            s->ready = true;
        } else if (offset == NAND_REG_XFER_MISC) {
            /*
             * The SPL ECC path writes six 10-bit stored ECC words to A468
             * after reading a page.  The hardware exposes correction
             * syndromes through the buffer registers; for a bit-perfect
             * emulated image those syndromes are zero.
             */
            if (s->xfer_regs[(NAND_REG_XFER_MODE - NAND_XFER_BASE) >> 2] == 0x01u) {
                if (s->xfer_ecc_count < 6)
                    s->xfer_ecc_count++;
                if (s->xfer_ecc_count >= 6) {
                    memset(s->xfer_buffer, 0x00, sizeof(s->xfer_buffer));
                    s->xfer_regs[(NAND_REG_XFER_STATUS2 - NAND_XFER_BASE) >> 2] = 0;
                    s->ready = true;
                }
            }
        }
        return;
    }

    /* ROM transfer engine registers (0xC000-0xC0FF). */
    if (offset >= NAND_BOOT_BASE && offset < NAND_BOOT_END) {
        uint32_t idx = (offset - NAND_BOOT_BASE) >> 2;
        uint8_t data_byte = (uint8_t)(value & 0xFFu);

        if (idx < (sizeof(s->boot_regs) / sizeof(s->boot_regs[0])))
            s->boot_regs[idx] = (uint32_t)value;

        if (offset == NAND_REG_BOOT_CTRL) {
            if ((value & 1u) == 0)
                nand_boot_reset(s);
        } else if (offset == NAND_REG_BOOT_CMD) {
            if (data_byte == 0x03u || data_byte == 0x06u)
                s->boot_addr_count = 0;
            else if (data_byte == 0x00u)
                nand_boot_reset(s);
        } else if (offset == NAND_REG_BOOT_ADDR) {
            nand_boot_latch_addr(s, data_byte);
        } else if (offset == NAND_REG_BOOT_KICK) {
            s->boot_ready = false;
            s->boot_regs[(NAND_REG_BOOT_STATUS - NAND_BOOT_BASE) >> 2] = 0;
        } else if (offset == NAND_REG_BOOT_MODE) {
            s->boot_mode = data_byte;
            if (data_byte == 0x05u && s->boot_addr_count >= 3) {
                nand_boot_start_stream(s);
            } else if (data_byte == 0x04u) {
                s->boot_ready = true;
                s->boot_regs[(NAND_REG_BOOT_STATUS - NAND_BOOT_BASE) >> 2] = 1;
            } else if (data_byte == 0x00u || data_byte == 0x01u) {
                s->boot_ecc_count = 0;
                if (data_byte == 0x00u) {
                    s->boot_regs[(NAND_REG_BOOT_STATUS2 - NAND_BOOT_BASE) >> 2] = 0;
                }
            }
        } else if (offset == NAND_REG_BOOT_ECC_IN) {
            if (s->boot_mode == 0x01u && s->boot_ecc_count < 6) {
                s->boot_ecc_words[s->boot_ecc_count++] = (uint16_t)(value & 0x03FFu);
                if (s->boot_ecc_count == 6)
                    nand_boot_finish_ecc_input(s);
            }
        }
        return;
    }

    /* Enable register */
    if (offset >= NAND_ENABLE_BASE && offset < NAND_ENABLE_END) {
        s->enabled = (value & 1) != 0;
        return;
    }

    /* DMA command block (0xC170-0xC177): ROM writes address + command */
    if (offset >= NAND_DMA_BASE && offset < NAND_DMA_END) {
        uint32_t old_cursor = s->dma_cursor;
        uint32_t old_total = s->dma_total_bytes;
        uint32_t old_addr = s->dma_nand_addr;
        bool old_active = s->dma_active;
        for (unsigned i = 0; i < size; i++) {
            uint32_t idx = (offset - NAND_DMA_BASE) + i;
            if (idx < 8)
                s->dma_cmd[idx] = (uint8_t)((value >> (8u * i)) & 0xFFu);
        }
        /* Byte 7 write triggers transfer setup.
         * The ROM uses at least two command-byte values here:
         *   0x20 = main sector-read path used by FUN_9fc010ec
         *   0xEC = scratch-buffer read path used by FUN_9fc01360
         * Both need to activate the FIFO over the already-latched
         * page-count/address block. Treating only 0xEC as the trigger
         * leaves the scratch helper with the only live transfer and the
         * later parser sees zeros from 0xAA00C170. */
        uint32_t last_byte = (offset - NAND_DMA_BASE) + size - 1;
        if (last_byte >= 7) {
            uint8_t cmd7 = s->dma_cmd[7];
            if (cmd7 == 0x20u || cmd7 == 0xECu) {
                /* Read trigger: decode page number and activate FIFO.
                 *
                 * FUN_9fc01318 splits a 32-bit page number across
                 * cmd[3..6], with 0xE0 OR'd into cmd[6] (ATA drive/head
                 * convention).  The page number × 512 gives the byte
                 * offset into the NAND image. */
                uint32_t page = (uint32_t)s->dma_cmd[3]
                              | ((uint32_t)s->dma_cmd[4] << 8)
                              | ((uint32_t)s->dma_cmd[5] << 16)
                              | ((uint32_t)(s->dma_cmd[6] & 0x0Fu) << 24);
                uint32_t pages = s->dma_cmd[2];
                if (pages == 0) pages = 1;
                s->dma_nand_addr = page * NAND_PAGE_DATA;
                s->dma_page_count = pages;
                s->dma_total_bytes = pages * NAND_PAGE_DATA;
                s->dma_cursor = 0;
                s->dma_active = true;
            }
        }
        return;
    }

    /* DMA control/acknowledge (0xC376)
     *
     * The ROM MIPS16 init path writes 0 here and then waits for status
     * bit 6 ("idle") to become true before issuing the real read trigger
     * via cmd byte 7 = 0xEC.  So 0xC376 is not itself the page-start
     * trigger; treating every write as "start DMA now" leaves status at
     * 0xD8 forever and wedges FUN_9fc013f0 in its pre-transfer wait. */
    if (offset == NAND_DMA_CTRL) {
        uint8_t ctrl = (uint8_t)(value & 0xFFu);

        if (ctrl == 0) {
            s->dma_active = false;
            s->dma_cursor = 0;
            s->dma_total_bytes = 0;
        }
        (void)ctrl;
        return;
    }

    /* Direct I/O control register (D002): sets CLE/ALE mode for D000 */
    if (offset == NAND_REG_DIO_CTRL) {
        s->dio_mode = (uint8_t)(value & 0xFFu);
        return;
    }

    /* Direct I/O data register (D000): routes to NAND cmd/addr/data */
    if (offset == NAND_REG_DIO_DATA) {
        uint8_t data_byte = (uint8_t)(value & 0xFFu);
        s->dio_last_write = data_byte;  /* latch for echo-back reads */
        if (s->dio_mode & 0x80u) {
            /* CLE mode: data_byte is a NAND command */
            s->last_cmd    = data_byte;
            s->addr_cycle  = 0;
            s->xfer_cursor = 0;
            s->ready       = false;
            switch (data_byte) {
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
            case NAND_CMD_STATUS:
                s->state = NAND_STATE_READ_STATUS;
                s->ready = true;
                break;
            case NAND_CMD_SEQIN:
                s->state = NAND_STATE_PROGRAM_DATA;
                s->column = 0;
                s->program_column = 0;
                s->program_length = 0;
                memset(s->program_buffer, 0xFF, sizeof(s->program_buffer));
                break;
            case NAND_CMD_ERASE1:
                s->state = NAND_STATE_ERASE_SETUP;
                break;
            case NAND_CMD_PAGEPROG:
                if (s->state == NAND_STATE_PROGRAM_DATA)
                    nand_commit_program(s);
                s->state = NAND_STATE_IDLE;
                s->ready = true;
                s->status_value = 0xC0u;
                break;
            case NAND_CMD_ERASE2:
                if (s->state == NAND_STATE_ERASE_SETUP)
                    nand_commit_erase(s);
                s->state = NAND_STATE_IDLE;
                s->ready = true;
                s->status_value = 0xC0u;
                break;
            case NAND_CMD_RESET:
                s->state = NAND_STATE_IDLE;
                s->ready = true;
                break;
            default:
                /* Unknown command — latch for echo-back via DIO read.
                 * The ROM writes a test byte (0x43) to D000 and reads it
                 * back to verify the NAND I/O path is functional. */
                s->state = NAND_STATE_IDLE;
                s->dio_last_write = data_byte;
                s->ready = true;
                break;
            }
        } else if (s->dio_mode & 0x01u) {
            /* ALE mode: data_byte is an address byte */
            switch (s->addr_cycle) {
            case 0:
                if (s->state == NAND_STATE_READ_DATA)
                    s->column = (s->column & ~0xFFu) | data_byte;
                break;
            case 1:
                s->page_addr = (s->page_addr & ~0xFFu) | data_byte;
                break;
            case 2:
                s->page_addr = (s->page_addr & ~0xFF00u) | ((uint32_t)data_byte << 8);
                break;
            case 3:
                s->page_addr = (s->page_addr & ~0xFF0000u) | ((uint32_t)data_byte << 16);
                break;
            }
            s->addr_cycle++;
            if (s->addr_cycle >= 3 &&
                (s->state == NAND_STATE_READ_DATA ||
                 s->state == NAND_STATE_READ_OOB)) {
                nand_setup_transfer(s);
                s->ready = true;
            } else if (s->addr_cycle == 1 && s->state == NAND_STATE_PROGRAM_DATA) {
                s->program_column = s->column;
            }
        } else if (s->state == NAND_STATE_PROGRAM_DATA) {
            for (unsigned i = 0; i < size; i++) {
                if ((s->program_column + s->program_length) >= sizeof(s->program_buffer))
                    break;
                s->program_buffer[s->program_length++] =
                    (uint8_t)((value >> (8u * i)) & 0xFFu);
            }
        }
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
        case NAND_CMD_STATUS:
            s->state = NAND_STATE_READ_STATUS;
            s->ready = true;
            break;
        case NAND_CMD_SEQIN:
            s->state = NAND_STATE_PROGRAM_DATA;
            s->column = 0;
            s->program_column = 0;
            s->program_length = 0;
            memset(s->program_buffer, 0xFF, sizeof(s->program_buffer));
            break;
        case NAND_CMD_ERASE1:
            s->state = NAND_STATE_ERASE_SETUP;
            break;
        case NAND_CMD_PAGEPROG:
            if (s->state == NAND_STATE_PROGRAM_DATA)
                nand_commit_program(s);
            s->state = NAND_STATE_IDLE;
            s->ready = true;
            s->status_value = 0xC0u;
            break;
        case NAND_CMD_ERASE2:
            if (s->state == NAND_STATE_ERASE_SETUP)
                nand_commit_erase(s);
            s->state = NAND_STATE_IDLE;
            s->ready = true;
            s->status_value = 0xC0u;
            break;
        case NAND_CMD_RESET:
            s->state = NAND_STATE_IDLE;
            s->ready = true;
            break;
        default:
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
        if (s->addr_cycle >= 3 &&
            (s->state == NAND_STATE_READ_DATA ||
             s->state == NAND_STATE_READ_OOB)) {
            nand_setup_transfer(s);
            s->ready = true;
        } else if (s->addr_cycle == 1 && s->state == NAND_STATE_PROGRAM_DATA) {
            s->program_column = s->column;
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
        if (s->state == NAND_STATE_PROGRAM_DATA) {
            for (unsigned i = 0; i < size; i++) {
                if ((s->program_column + s->program_length) >= sizeof(s->program_buffer))
                    break;
                s->program_buffer[s->program_length++] =
                    (uint8_t)((value >> (8u * i)) & 0xFFu);
            }
            return;
        }

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
        return;
    }

    /* Legacy/program stream window (0xB000): sequential page/OOB writes. */
    if (offset >= NAND_STREAM_BASE && offset < NAND_STREAM_END) {
        uint32_t mode = s->xfer_regs[(NAND_REG_XFER_MODE - NAND_XFER_BASE) >> 2];

        if ((mode == 0x06u || mode == 0x07u) && s->stream_limit != 0) {
            for (unsigned i = 0; i < size; i++) {
                uint32_t idx;

                if (s->stream_cursor >= s->stream_limit)
                    break;

                idx = s->stream_base + s->stream_cursor;
                if (idx < sizeof(s->program_buffer))
                    s->program_buffer[idx] = (uint8_t)((value >> (8u * i)) & 0xFFu);
                if (idx < NAND_PAGE_DATA) {
                    uint32_t used = idx + 1u;
                    if (used > s->program_length)
                        s->program_length = used;
                }
                s->stream_cursor++;
            }
            return;
        }
    }

    /* Status/ready register write — some controllers allow clearing */
    if (offset == NAND_REG_READY) {
        return;
    }

}

uint64_t nand_read(nand_state_t *s, uint32_t offset, unsigned size,
                   uint32_t pc)
{
    uint64_t val = 0;

    /* Control/timing registers — return latched value */
    if (offset >= NAND_CTRL_BASE && offset < NAND_CTRL_END) {
        uint32_t idx = (offset - NAND_CTRL_BASE) >> 2;
        val = (idx < 8) ? s->ctrl_regs[idx] : 0;
        goto out;
    }

    if (offset >= 0xA060u && offset < 0xA160u) {
        uint32_t rel = offset - 0xA060u;

        for (unsigned i = 0; i < size && (rel + i) < sizeof(s->strap_regs); i++)
            val |= (uint64_t)s->strap_regs[rel + i] << (8u * i);
        goto out;
    }

    /* SPL transfer engine registers (0xA400-0xA4FF). */
    if (offset >= NAND_XFER_BASE && offset < NAND_XFER_END) {
        uint32_t idx = (offset - NAND_XFER_BASE) >> 2;
        if (offset == NAND_REG_XFER_STATUS) {
            val = s->ready ? 0x00000001u : 0u;
        } else if (offset == NAND_REG_XFER_ACK) {
            nand_xfer_commit_phase(s);
            val = 0;
        } else if (offset == NAND_REG_XFER_ADDR &&
                   s->xfer_data_pos < s->xfer_data_count) {
            val = s->xfer_data_bytes[s->xfer_data_pos++];
        } else if (offset == NAND_REG_XFER_STATUS2) {
            val = s->xfer_regs[idx];
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

    /* ROM transfer engine registers (0xC000-0xC0FF). */
    if (offset >= NAND_BOOT_BASE && offset < NAND_BOOT_END) {
        uint32_t idx = (offset - NAND_BOOT_BASE) >> 2;
        if (offset == NAND_REG_BOOT_STATUS) {
            val = s->boot_ready ? 0x00000001u : 0u;
        } else if (idx < (sizeof(s->boot_regs) / sizeof(s->boot_regs[0]))) {
            val = s->boot_regs[idx];
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
        }
        goto out;
    }

    /* Direct I/O data register (D000): read NAND data or status */
    if (offset == NAND_REG_DIO_DATA) {
        if (s->dio_last_write != 0) {
            /* Echo-back: ROM writes a test byte and reads it back to
             * verify the NAND controller I/O path is functional. */
            val = s->dio_last_write;
            s->dio_last_write = 0;
            goto out;
        }
        if (s->state == NAND_STATE_READ_STATUS) {
            val = s->status_value;
            goto out;
        }
        if (s->state == NAND_STATE_READ_DATA ||
            s->state == NAND_STATE_READ_OOB ||
            s->state == NAND_STATE_READ_ID) {
            if (s->xfer_length > 0 && s->image && s->xfer_cursor < s->xfer_length) {
                if (s->state == NAND_STATE_READ_ID) {
                    /* NAND ID bytes: maker=0xEC (Samsung), device=0x73 (K9F2808U0B 16MB) */
                    static const uint8_t id_bytes[] = {0xEC, 0x73};
                    val = (s->xfer_cursor < 4) ? id_bytes[s->xfer_cursor] : 0xFF;
                    s->xfer_cursor++;
                } else if (s->state == NAND_STATE_READ_OOB) {
                    val = nand_stream_oob_byte(s, s->page_addr,
                                               s->xfer_cursor);
                    s->xfer_cursor++;
                } else {
                    uint32_t img_off = (s->page_addr * NAND_PAGE_DATA) + s->column + s->xfer_cursor;
                    val = nand_image_byte(s, img_off);
                    s->xfer_cursor++;
                }
            } else {
                val = 0xFF;
            }
        } else {
            val = 0xFF;
        }
        goto out;
    }

    /* Direct I/O control register (D002): return latched mode */
    if (offset == NAND_REG_DIO_CTRL) {
        val = s->dio_mode;
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

    /* Device ID: Samsung K9F2808U0B (16MB, 3.3V) = maker 0xEC, device 0x73 */
    if (offset == NAND_REG_DEVID) {
        val = 0x73ECu;  /* little-endian: maker=0xEC, device=0x73 */
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

        if (s->state == NAND_STATE_READ_STATUS) {
            val = s->status_value;
            goto out;
        }

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
        goto out;
    }

    /* Status register */
    if (offset == NAND_REG_STATUS) {
        if (s->state == NAND_STATE_READ_STATUS) {
            val = s->status_value;
            goto out;
        }
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

    /* DMA command/status/FIFO registers (0xC170-0xC177).
     * Offset 0 (0xC170): FIFO data port — sequential NAND page reads.
     * Offset 7 (0xC177): Status byte — bit 6=idle/complete, bit 3=data ready.
     * Other offsets: return latched command block bytes. */
    if (offset >= NAND_DMA_BASE && offset < NAND_DMA_END) {
        uint32_t reg_off = offset - NAND_DMA_BASE;
        if (reg_off == 0 && s->dma_active) {
            /* FIFO data port: return sequential NAND page data */
            val = 0;
            for (unsigned i = 0; i < size; i++) {
                uint8_t byte;
                if (s->dma_cursor < s->dma_total_bytes)
                    byte = nand_dma_byte(s, s->dma_nand_addr + s->dma_cursor,
                                         pc);
                else
                    byte = 0xFFu;
                val |= (uint64_t)byte << (8u * i);
                s->dma_cursor++;
            }
            /*
             * Once the final FIFO byte has been consumed, transition back to
             * the ROM's idle state. Returning a sticky synthetic "complete"
             * status causes the ROM to recurse back through 0x9FC003DC and
             * leak stack frames until execution falls into low RAM.
             */
            if (s->dma_cursor >= s->dma_total_bytes)
            if (s->dma_cursor >= s->dma_total_bytes)
                s->dma_active = false;
            goto out;
        }
        /* Status and command block readback */
        val = 0;
        for (unsigned i = 0; i < size && (reg_off + i) < 8; i++) {
            uint8_t byte;
            if ((reg_off + i) == 7) {
                /* Status byte at offset 7 (0xC177):
                 *   !dma_active             -> 0x50 (idle / transfer drained)
                 *   dma_active             -> 0x58 (busy, data available)
                 *
                 * The old sticky 0x5F "complete" state was wrong for the ROM
                 * path: bit0 and low nibble 0xF make the MIPS16 dispatcher
                 * re-enter 0x9FC003DC, which accumulates recursive frames.
                 * Clearing dma_active when the FIFO drains makes subsequent
                 * status reads collapse back to 0x50 instead. */
                if (!s->dma_active) {
                    byte = 0x50u;        /* idle: no transfer in progress */
                } else {
                    byte = 0x58u;        /* busy: data available */
                }
            } else {
                byte = s->dma_cmd[reg_off + i];
            }
            val |= (uint64_t)byte << (8u * i);
        }
        goto out;
    }

    /* Command/address range reads — return 0 */
    if (offset >= NAND_CMD_BASE && offset < NAND_CMD_END) {
        val = 0;
        goto out;
    }

out:

    return val;
}
