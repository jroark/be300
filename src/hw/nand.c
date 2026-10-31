#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "host_io.h"
#include "nand.h"
#include "nand_internal.h"

static uint8_t nand_image_byte(const nand_state_t *s, uint32_t off)
{
    if (!s->image || off >= s->image_size)
        return 0xFFu;
    return s->image[off];
}

static bool nand_block_is_allocated(const nand_state_t *s, uint32_t block)
{
    if (block >= NAND_BLOCK_COUNT)
        return false;

    return s->block_data_state[block] > 0;
}

uint8_t nand_stream_oob_byte(nand_state_t *s,
                             uint32_t page, uint32_t oob_idx)
{
    uint32_t block = page / NAND_BLOCK_PAGES;
    uint32_t page_in_block = page % NAND_BLOCK_PAGES;

    if (oob_idx >= NAND_PAGE_OOB || page >= NAND_BLOCK_COUNT * NAND_BLOCK_PAGES)
        return 0xFFu;
    if (s->oob) return s->oob[page * NAND_PAGE_OOB + oob_idx];

    /*
     * The restore images are data-only dumps (no physical OOB bytes;
     * see ce/restore_images/RESTORE_IMAGES.md), so reconstruct the
     * per-block tags that NANDWRITER would have stored in the spare area.
     * The ROM/SPL logical-block mapper votes across the first five pages
     * of a block and expects the same tag on at least three of them, so
     * mirror the synthetic block metadata across that opening page set.
     *
     * Two OOB formats are recognised — both share the 0x55AA magic in
     * OOB[0..1]; OOB[2] and the block-id word distinguish them:
     *   Allocated:  OOB[2]=0x0F, OOB[4..7]=block_id
     *   Erased:     OOB[2]=0xFF, OOB[4..7]=0xFFFFFFFF
     *
     * Allocation is determined by image coverage, not payload contents:
     * an all-0xFF logical block in the data-only image still has allocated
     * spare metadata on real NAND.  nanddisk.dll's NandColdBoot builds
     * the BAT from these tags at UM 0x019A3B24..0x019A3DCC; mislabelling
     * image-backed blank payload blocks as erased makes it overrun the
     * blank-block table at UM 0x019A3D14.
     */
    if (page_in_block < 5 && block < NAND_BLOCK_COUNT) {
        bool allocated = nand_block_is_allocated(s, block);

        switch (oob_idx) {
        case 0: return 0xAAu;
        case 1: return 0x55u;
        case 2: return allocated ? 0x0Fu : 0xFFu;
        case 4: return allocated ? (uint8_t)(block & 0xFFu) : 0xFFu;
        case 5: return allocated ? (uint8_t)((block >> 8) & 0xFFu) : 0xFFu;
        case 6: return allocated ? 0x00u : 0xFFu;
        case 7: return allocated ? 0x00u : 0xFFu;
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

void nand_stream_read_dma(nand_state_t *s, uint8_t *dest, uint32_t len)
{
    if (!s || !dest || !s->stream_active)
        return;

    for (uint32_t i = 0; i < len; i++)
        dest[i] = nand_stream_byte(s, s->stream_cursor + i);
    s->stream_cursor += len;
}


bool nand_physical_bad(const nand_state_t *s, uint32_t block)
{
    if (!s->oob || block >= NAND_BLOCK_COUNT) return false;
    for (unsigned p = 0; p < 2; p++) {
        const uint8_t *o = s->oob + (block*NAND_BLOCK_PAGES+p)*NAND_PAGE_OOB;
        if (o[5] != 0xFF && !(o[0] == 0xAA && o[1] == 0x55 && o[2] == 0x0F)) return true;
    }
    return false;
}

/* Physical-page RS remainder for the ROM's MODE-1 result path. Synthetic
 * data-only images retain their legacy no-parity/clean behavior. */
static uint8_t nand_bitrev(uint8_t b)
{
    uint8_t r=0;
    for (unsigned i=0;i<8;i++) r=(uint8_t)((r<<1)|((b>>i)&1));
    return r;
}
static uint16_t nand_xtime(uint16_t a)
{
    a=(uint16_t)(a<<1);
    return (a&0x400u) ? (uint16_t)(a^0x409u) : a;
}
static uint16_t nand_gmul(uint16_t a,uint16_t b)
{
    uint16_t r=0;
    for(unsigned i=0;i<10;i++,b>>=1,a=nand_xtime(a)) if(b&1) r^=a;
    return r;
}
static void nand_physical_ecc(nand_state_t *s)
{
    memset(s->xfer_buffer,0,sizeof(s->xfer_buffer));
    unsigned status=(NAND_REG_XFER_STATUS2-NAND_XFER_BASE)>>2;
    s->xfer_regs[status]=0;
    if (!s->oob || s->stream_page >= NAND_BLOCK_COUNT*NAND_BLOCK_PAGES) return;
    const uint8_t *o=s->oob+s->stream_page*NAND_PAGE_OOB;
    uint16_t r[6], syndrome[6];
    /* ROM 9FC01FE0 feeds reversed 10-bit fields. Use the values supplied
     * by the guest, so incorrect ECC inputs cannot silently appear clean. */
    for(unsigned i=0;i<6;i++) {
        uint16_t v=s->xfer_ecc_words[i];r[i]=0;
        for(unsigned j=0;j<10;j++)r[i]=(uint16_t)((r[i]<<1)|((v>>j)&1));
    }
    uint16_t g[7]={1}, rem[6]={0}, root=1;
    for(unsigned k=0;k<6;k++,root=nand_xtime(root)) {
        for(int i=(int)k+1;i>0;i--) g[i]=g[i-1]^nand_gmul(g[i],root);
        g[0]=nand_gmul(g[0],root);
    }
    for(unsigned i=0;i<520;i++) {
        uint8_t byte=i<512 ? s->image[s->stream_page*512+i] : o[i-512];
        uint16_t feedback=((uint16_t)nand_bitrev(byte)<<2)^rem[5];
        for(int j=5;j>0;j--) rem[j]=rem[j-1]^nand_gmul(feedback,g[j]);
        rem[0]=nand_gmul(feedback,g[0]);
    }
    /* The controller returns polynomial coefficients, NOT S0..S5. ROM
     * 9FC01C30 evaluates this degree-five remainder at x^0..x^5 before
     * its error locator runs. Correct stored parity makes it zero. */
    for(unsigned i=0;i<6;i++) {
        syndrome[i]=r[i]^rem[5-i];
        if(syndrome[i]) s->xfer_regs[status]=1;
    }
    /* Same 60-bit packing as physical parity, presented in four low
     * halfwords at A4A0/A4A4/A4A8/A4AC. ROM 9FC01FA0 copies those pairs. */
    uint16_t *v=syndrome;
    uint8_t packed[8]={ (uint8_t)(v[5]>>2), (uint8_t)((v[5]<<6)|(v[4]>>4)),
        (uint8_t)((v[4]<<4)|(v[3]>>6)), (uint8_t)((v[3]<<2)|(v[2]>>8)),
        (uint8_t)v[2], (uint8_t)(v[1]>>2), (uint8_t)((v[1]<<6)|(v[0]>>4)), (uint8_t)(v[0]<<4) };
    for (unsigned i=0;i<8;i++) s->xfer_buffer[(i/2)*4+i%2]=nand_bitrev(packed[i]);
}

static void nand_commit_program(nand_state_t *s)
{
    uint32_t page_off;
    uint32_t limit;
    uint32_t i;
    uint32_t block;

    if (!s->image || s->page_addr >= (NAND_BLOCK_COUNT * NAND_BLOCK_PAGES))
        return;

    if (nand_physical_bad(s, s->page_addr / NAND_BLOCK_PAGES)) {
        s->status_value = 0xC1u; return;
    }
    page_off = s->page_addr * NAND_PAGE_DATA;
    if (s->oob) {
        for (uint32_t j = 0; j < s->program_length && s->program_column+j < NAND_PAGE_RAW; j++) {
            uint32_t col = s->program_column+j;
            if (col >= NAND_PAGE_DATA) s->oob[s->page_addr*NAND_PAGE_OOB+col-NAND_PAGE_DATA] &= s->program_buffer[j];
        }
        if (s->program_length) s->dirty = true;
    }
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
            s->block_data_state[block] = 1;
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

    if (nand_physical_bad(s, block)) { s->status_value = 0xC1u; return; }
    start_page = block * NAND_BLOCK_PAGES;
    if (s->oob) memset(s->oob + start_page*NAND_PAGE_OOB, 0xFF, NAND_BLOCK_PAGES*NAND_PAGE_OOB);
    start_off = start_page * NAND_PAGE_DATA;
    memset(s->image + start_off, 0xFF, NAND_BLOCK_PAGES * NAND_PAGE_DATA);
    s->dirty = true;
    s->block_data_state[block] = 0;
}

void nand_init(nand_state_t *s, uint8_t *image, size_t size,
               size_t valid_size)
{
    memset(s, 0, sizeof(*s));
    s->image      = image;
    s->image_size = size;
    s->image_valid_size = valid_size <= size ? valid_size : size;
    s->ready      = true;
    s->state      = NAND_STATE_IDLE;
    s->status_value = 0xC0u;
    s->portctl    = 0;
    for (uint32_t block = 0; block < NAND_BLOCK_COUNT; block++) {
        uint64_t start_off = (uint64_t)block * NAND_BLOCK_PAGES * NAND_PAGE_DATA;
        s->block_data_state[block] =
            (image && start_off < s->image_valid_size) ? 1 : 0;
    }
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

void nand_dma_state_written(nand_state_t *s)
{
    s->burst_520 = false;
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

static void nand_xfer_reset_addr(nand_state_t *s)
{
    s->xfer_addr_count = 0;
    memset(s->xfer_addr_bytes, 0, sizeof(s->xfer_addr_bytes));
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
            nand_xfer_reset_addr(s);
            nand_xfer_clear_data(s);
            break;
        case NAND_CMD_READ1:
            s->state = NAND_STATE_READ_DATA;
            s->column = 256;
            nand_xfer_reset_addr(s);
            nand_xfer_clear_data(s);
            break;
        case NAND_CMD_READOOB:
            s->state = NAND_STATE_READ_OOB;
            nand_xfer_reset_addr(s);
            nand_xfer_clear_data(s);
            break;
        case NAND_CMD_READID:
            s->state = NAND_STATE_READ_ID;
            nand_xfer_reset_addr(s);
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
            nand_xfer_reset_addr(s);
            memset(s->program_buffer, 0xFF, sizeof(s->program_buffer));
            nand_xfer_clear_data(s);
            break;
        case NAND_CMD_ERASE1:
            s->state = NAND_STATE_ERASE_SETUP;
            nand_xfer_reset_addr(s);
            nand_xfer_clear_data(s);
            break;
        case NAND_CMD_PAGEPROG:
            s->status_value = 0xC0u;
            if (s->state == NAND_STATE_PROGRAM_DATA)
                nand_commit_program(s);
            s->state = NAND_STATE_IDLE;
            s->ready = true;
            nand_xfer_prepare_status(s);
            break;
        case NAND_CMD_ERASE2:
            s->status_value = 0xC0u;
            if (s->state == NAND_STATE_ERASE_SETUP) {
                s->page_addr = nand_xfer_block_addr(s) * NAND_BLOCK_PAGES;
                nand_commit_erase(s);
            }
            s->state = NAND_STATE_IDLE;
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

/*
 * Legacy command/address ports: the direct I/O data port at 0xD000 (CLE/ALE
 * picked by 0xD002) and the 0xAC00/0xAC04 command/address registers drive
 * the same NAND command and address-cycle sequence.
 */
static void nand_legacy_command(nand_state_t *s, uint8_t cmd)
{
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
        s->status_value = 0xC0u;
        if (s->state == NAND_STATE_PROGRAM_DATA)
            nand_commit_program(s);
        s->state = NAND_STATE_IDLE;
        s->ready = true;
        break;
    case NAND_CMD_ERASE2:
        s->status_value = 0xC0u;
        if (s->state == NAND_STATE_ERASE_SETUP)
            nand_commit_erase(s);
        s->state = NAND_STATE_IDLE;
        s->ready = true;
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
}

/* One address cycle; returns true when it completes a read setup. */
static bool nand_legacy_address(nand_state_t *s, uint8_t abyte)
{
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
        return true;
    }
    if (s->addr_cycle == 1 && s->state == NAND_STATE_PROGRAM_DATA)
        s->program_column = s->column;
    return false;
}

/* Program data bytes, least significant first, up to the page buffer. */
static void nand_program_append(nand_state_t *s, uint64_t value, unsigned size)
{
    for (unsigned i = 0; i < size; i++) {
        if ((s->program_column + s->program_length) >= sizeof(s->program_buffer))
            break;
        s->program_buffer[s->program_length++] =
            (uint8_t)((value >> (8u * i)) & 0xFFu);
    }
}

/* Stream read used by SPL transfer engine through 0xB000 window. */
static uint64_t nand_stream_read(nand_state_t *s, unsigned size)
{
    uint64_t val = 0;

    for (unsigned i = 0; i < size; i++) {
        uint8_t byte;

        if (s->stream_limit != 0 && (s->stream_cursor + i) >= s->stream_limit) {
            if (s->burst_520 &&
                s->xfer_regs[(NAND_REG_XFER_MODE - NAND_XFER_BASE) >> 2] == 0x05u)
                s->stream_stall = true;
            byte = 0xFFu;
        }
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
            if (data_byte == 0x01u)
                s->burst_520 = true;    /* the ROM's ECC result path */
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
                /* The rest of the page, past a 520-byte burst. */
                if (s->stream_active)
                    s->stream_limit = NAND_PAGE_RAW - s->stream_col;
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
                s->stream_limit = (s->burst_520 ? NAND_PAGE_DATA + 8u : NAND_PAGE_RAW) - col;
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
                if (s->xfer_ecc_count < 6) {
                    s->xfer_ecc_words[s->xfer_ecc_count] = (uint16_t)(value & 0x3FFu);
                    s->xfer_ecc_count++;
                }
                if (s->xfer_ecc_count >= 6) {
                    nand_physical_ecc(s);
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

    /*
     * CF ATA taskfile (0xC170-0xC177) and alternate status (0xC376) with
     * no ATA card answering: nothing latches a write
     * (devices/nand_window.c routes them to the card when one is).
     */
    if ((offset >= NAND_ATA_TF_BASE && offset < NAND_ATA_TF_END) ||
        offset == NAND_ATA_ALT_STATUS)
        return;

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
            /* CLE mode: data_byte is a NAND command. An unknown one
             * stays latched for echo-back: the ROM writes a test byte
             * (0x43) to D000 and reads it back to verify the NAND I/O
             * path. */
            nand_legacy_command(s, data_byte);
        } else if (s->dio_mode & 0x01u) {
            /* ALE mode: data_byte is an address byte */
            if (nand_legacy_address(s, data_byte))
                s->dio_last_write = 0;
        } else if (s->state == NAND_STATE_PROGRAM_DATA) {
            nand_program_append(s, value, size);
        }
        return;
    }

    /* Command register */
    if (offset == NAND_REG_CMD) {
        nand_legacy_command(s, (uint8_t)(value & 0xFF));
        return;
    }

    /* Address byte register */
    if (offset == NAND_REG_ADDR) {
        nand_legacy_address(s, (uint8_t)(value & 0xFF));
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
            nand_program_append(s, value, size);
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
                if (idx < NAND_PAGE_RAW) {
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
                    val = (s->xfer_cursor < sizeof(id_bytes)) ?
                        id_bytes[s->xfer_cursor] : 0xFF;
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

    /*
     * CF ATA taskfile (0xC170-0xC177) and alternate status (0xC376) with
     * no ATA card answering. The socket's data lines float high, and an
     * all-ones status byte is the ATA "no device" reading, which is how
     * the ROM's CF probe sees an empty socket. These reads used to serve
     * the NAND image as a phantom disk with a fabricated MBR; no boot in
     * the regression set reaches them without a card.
     */
    if ((offset >= NAND_ATA_TF_BASE && offset < NAND_ATA_TF_END) ||
        offset == NAND_ATA_ALT_STATUS) {
        val = 0;
        for (unsigned i = 0; i < size; i++)
            val |= (uint64_t)0xFFu << (8u * i);
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
