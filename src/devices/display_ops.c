/*
 *  src/devices/display_ops.c — the companion's display engine as seen
 *  through the VRC4173 latch: the 0x0A00 block copy and the 0x0200-0x0234
 *  fill/copy operations ddi.dll triggers. Split out of src/be300_devices.c.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "machine.h"
#include "memory.h"

#include "devices.h"
#include "devices_internal.h"
#include "vrc4173_latch.h"

static bool be300_reg_addr_to_pa(uint32_t addr, uint64_t *pa_out)
{
    if (!pa_out)
        return false;

    if (addr >= 0x80000000u && addr < 0xC0000000u)
        *pa_out = (uint64_t)(addr & 0x1FFFFFFFu);
    else
        *pa_out = (uint64_t)addr;

    return true;
}

static void be300_fb_mark_dirty(machine_t *m, uint32_t off, uint32_t len)
{
    struct vfb_data *fb;
    uint32_t bpp, stride, start_y, end_y, x1, x2;

    if (!m || !m->gxe_machine || !m->gxe_machine->fb || len == 0)
        return;

    fb = m->gxe_machine->fb;
    if (fb->bit_depth <= 0 || fb->bytes_per_line <= 0 ||
        fb->xsize <= 0 || fb->ysize <= 0)
        return;

    bpp = (uint32_t)((fb->bit_depth + 7) / 8);
    stride = (uint32_t)fb->bytes_per_line;
    if (bpp == 0 || stride == 0)
        return;

    start_y = off / stride;
    end_y = (off + len - 1u) / stride;
    if (start_y >= (uint32_t)fb->ysize)
        return;
    if (end_y >= (uint32_t)fb->ysize)
        end_y = (uint32_t)fb->ysize - 1u;

    if (start_y == end_y) {
        x1 = (off % stride) / bpp;
        x2 = ((off + len - 1u) % stride) / bpp;
    } else {
        x1 = 0;
        x2 = (uint32_t)fb->xsize - 1u;
    }

    if (x1 >= (uint32_t)fb->xsize)
        x1 = (uint32_t)fb->xsize - 1u;
    if (x2 >= (uint32_t)fb->xsize)
        x2 = (uint32_t)fb->xsize - 1u;

    if (fb->update_x1 > (int)x1) fb->update_x1 = (int)x1;
    if (fb->update_y1 > (int)start_y) fb->update_y1 = (int)start_y;
    if (fb->update_x2 < (int)x2) fb->update_x2 = (int)x2;
    if (fb->update_y2 < (int)end_y) fb->update_y2 = (int)end_y;
}

void be300_vrc4173_a00_blit_maybe(struct cpu *cpu,
    struct be300_vrc4173_latch *d)
{
    machine_t *m = g_be300_machine;
    struct vfb_data *fb;
    uint32_t status, command, count_words, src_reg, dst_reg;
    uint32_t dst_off;
    uint64_t src_pa;
    size_t len, fb_size;
    unsigned char *staging;

    if (!cpu || !cpu->mem || !d || !m || !m->gxe_machine ||
        !m->gxe_machine->fb)
        return;

    status = be300_latch_peek_u32(d, 0x0A00);
    if ((status & 1u) == 0)
        return;

    command = be300_latch_peek_u32(d, 0x0A04);
    if ((command & 0x81u) != 0x81u)
        return;

    count_words = be300_latch_peek_u32(d, 0x0A08);
    src_reg = be300_latch_peek_u32(d, 0x0A10);
    dst_reg = be300_latch_peek_u32(d, 0x0A14);

    /*
     * VRC4173/Casio graphics copy engine at PA 0x0A000A00.
     *
     * docs/hardware/hw_dump_vrc4173.txt:1085-1086 show this block
     * populated after boot (`0x0A000A00: 00000000 00000001 00000018 ...`,
     * `0x0A000A10: 00005A00 00027D7C ...`): idle status, mode 1, word
     * count, source scratch address, and framebuffer byte offset.
     * ddi.dll's row blitter at UM 0x01A53AD0..0x01A53D04 programs the
     * same block, stages a source row into low SDRAM (0x5800/0x5A00),
     * writes command 0x81, then triggers by writing bit 0 at 0xA00.
     *
     * Model the observed command as an immediate SDRAM-to-framebuffer copy.
     * TODO 2026-04-25: confirm the full bit assignments and completion
     * timing with a BEDiag hardware trace; this currently covers the only
     * command shape seen on the WinCE 3.0 boot path.
     */
    if (count_words == 0)
        goto complete;

    if (!be300_reg_addr_to_pa(src_reg, &src_pa))
        goto complete;

    fb = m->gxe_machine->fb;
    if (!fb->framebuffer || fb->framebuffer_size == 0)
        goto complete;

    fb_size = fb->framebuffer_size;
    if ((uint64_t)dst_reg >= PA_VRC4173_FB &&
        (uint64_t)dst_reg < PA_VRC4173_FB + (uint64_t)fb_size) {
        dst_off = dst_reg - PA_VRC4173_FB;
    } else if (((uint64_t)dst_reg & 0x1FFFFFFFu) >= PA_VRC4173_FB &&
        ((uint64_t)dst_reg & 0x1FFFFFFFu) <
            PA_VRC4173_FB + (uint64_t)fb_size) {
        dst_off = (uint32_t)(((uint64_t)dst_reg & 0x1FFFFFFFu) -
            PA_VRC4173_FB);
    } else {
        dst_off = dst_reg;
    }

    if ((uint64_t)dst_off >= (uint64_t)fb_size)
        goto complete;

    /* 64-bit so a huge word count can't wrap size_t on 32-bit hosts. */
    if ((uint64_t)count_words * 4u > (uint64_t)(fb_size - (size_t)dst_off))
        len = fb_size - (size_t)dst_off;
    else
        len = (size_t)count_words * 4u;
    if (len == 0)
        goto complete;

    /*
     * The source is low SDRAM (ddi.dll's staging rows) or, for GDI
     * scroll/redraw, the framebuffer itself. Read it through the memory
     * path, which spans RAM blocks and reaches dev_fb, into a staging
     * buffer, so an overlapping framebuffer-to-framebuffer copy is safe.
     */
    staging = malloc(len);
    if (!staging)
        goto complete;
    if (be300_phys_copy(cpu, src_pa, staging, len, MEM_READ)) {
        memcpy(fb->framebuffer + dst_off, staging, len);
        be300_fb_mark_dirty(m, dst_off, (uint32_t)len);
    }
    free(staging);

complete:
    d->bytes[0x0A00] &= ~(uint8_t)1u;   /* trigger/busy complete */
    d->bytes[0x0A04] &= ~(uint8_t)0x80u; /* command bit self-clears */
}

void be300_vrc4173_200_display_op_maybe(
    struct be300_vrc4173_latch *d)
{
    machine_t *m = g_be300_machine;
    struct vfb_data *fb;
    uint32_t mode, dst_off, width, height, color, src_off, bit_off;
    uint32_t stride, bpp;

    if (!d || !m || !m->gxe_machine || !m->gxe_machine->fb)
        return;

    fb = m->gxe_machine->fb;
    if (!fb->framebuffer || fb->framebuffer_size == 0 ||
        fb->bit_depth != 16 || fb->bytes_per_line <= 0)
        goto complete;

    if ((be300_latch_peek_u32(d, 0x0234) & 1u) == 0)
        return;

    mode = be300_latch_peek_u32(d, 0x0200);
    dst_off = (be300_latch_peek_u32(d, 0x0210) & 0xFFFFu)
        | ((be300_latch_peek_u32(d, 0x0214) & 0xFFFFu) << 16);
    width = be300_latch_peek_u32(d, 0x0208);
    height = be300_latch_peek_u32(d, 0x020C);
    color = be300_latch_peek_u32(d, 0x0204) & 0xFFFFu;
    stride = (uint32_t)fb->bytes_per_line;
    bpp = 2;

    /*
     * VRC4173/Casio display fill engine at PA 0x0A000200.
     *
     * The real-hardware dump identifies this as display-related:
     * docs/hardware/hw_dump_vrc4173.txt:977-982 shows the block populated,
     * including PA 0x0A000220 = 0x1E0 (240 visible pixels * 2 bytes).
     * ddi.dll's solid-fill path at UM 0x01A53EC8..0x01A53FA0 programs
     * destination byte offset at 0x210/0x214, width/height at 0x208/0x20C,
     * RGB565 color at 0x204, mode 0 at 0x200, then starts the operation by
     * writing 1 to 0x234. Model that mode as an immediate framebuffer fill.
     *
     * The same dump also captures mode 2 immediately below the seed row
     * (PA 0x0A000200 = 2, 0x0208/0x020C = 4x9, 0x0210 = 0xB414,
     * 0x0220 = 0x1E0, 0x0238 = 1). ddi.dll's glyph path at UM
     * 0x01A53848..0x01A53AAC programs mode 2, stages 1-bpp glyph rows into
     * the framebuffer row padding at byte offset 0x1E0, then starts the
     * block via 0x0234. Model that observed mode as transparent mono
     * expansion from the staged padding rows into the visible destination.
     *
     * Mode 1 is FB-to-FB block copy. ddi.dll's copy path at
     * UM 0x01A53D44..0x01A53EBC programs destination at 0x210/0x214,
     * source at 0x218/0x21C, width/height at 0x208/0x20C, then
     * starts through 0x234. The mode register is 1 plus optional
     * 0x400/0x800 start-corner bits; when those bits are set the driver
     * writes the right/bottom corner, and the hardware iterates from that
     * corner so overlapping scrolls preserve source pixels. Browser page
     * scrolling uses this path for 32-row framebuffer moves.
     *
     * TODO 2026-05-02: capture BEDiag traces for any other non-zero modes;
     * unobserved ROP modes remain deliberately unimplemented.
     */
    if (width == 0 || height == 0)
        goto complete;
    if ((mode & 0xFFu) != 1u &&
        (uint64_t)dst_off >= (uint64_t)fb->framebuffer_size)
        goto complete;

    if (mode == 0) {
        for (uint32_t y = 0; y < height; y++) {
            uint64_t row_off = (uint64_t)dst_off + (uint64_t)y * stride;
            if (row_off >= (uint64_t)fb->framebuffer_size)
                break;

            size_t row_left = fb->framebuffer_size - (size_t)row_off;
            uint32_t row_width = width;
            if ((uint64_t)row_width * bpp > row_left)
                row_width = (uint32_t)(row_left / bpp);

            uint16_t *row = (uint16_t *)(void *)(fb->framebuffer + row_off);
            for (uint32_t x = 0; x < row_width; x++)
                row[x] = (uint16_t)color;
        }

        {
            uint64_t len64 = (uint64_t)(height - 1u) * stride
                + (uint64_t)width * bpp;
            uint32_t len = len64 > UINT32_MAX ? UINT32_MAX : (uint32_t)len64;
            be300_fb_mark_dirty(m, dst_off, len);
        }
    } else if (mode == 2) {
        src_off = (be300_latch_peek_u32(d, 0x0220) & 0xFFFFu)
            | ((be300_latch_peek_u32(d, 0x0224) & 0xFFFFu) << 16);
        bit_off = be300_latch_peek_u32(d, 0x0228) & 7u;

        for (uint32_t y = 0; y < height; y++) {
            uint64_t dst_row_off = (uint64_t)dst_off + (uint64_t)y * stride;
            uint64_t src_row_off = (uint64_t)src_off + (uint64_t)y * stride;
            uint32_t row_width = width;
            uint8_t *src_row;
            uint16_t *dst_row;

            if (dst_row_off >= (uint64_t)fb->framebuffer_size ||
                src_row_off >= (uint64_t)fb->framebuffer_size)
                break;
            if ((uint64_t)row_width * bpp >
                (uint64_t)fb->framebuffer_size - dst_row_off)
                row_width = (uint32_t)(((uint64_t)fb->framebuffer_size -
                    dst_row_off) / bpp);

            src_row = fb->framebuffer + src_row_off;
            dst_row = (uint16_t *)(void *)(fb->framebuffer + dst_row_off);
            for (uint32_t x = 0; x < row_width; x++) {
                uint32_t bit = bit_off + x;
                uint64_t src_byte_off = src_row_off + (bit >> 3);
                uint8_t mask;

                if (src_byte_off >= (uint64_t)fb->framebuffer_size)
                    break;

                mask = (uint8_t)(0x80u >> (bit & 7u));
                if (src_row[bit >> 3] & mask)
                    dst_row[x] = (uint16_t)color;
            }
        }

        {
            uint64_t len64 = (uint64_t)(height - 1u) * stride
                + (uint64_t)width * bpp;
            uint32_t len = len64 > UINT32_MAX ? UINT32_MAX : (uint32_t)len64;
            be300_fb_mark_dirty(m, dst_off, len);
        }
    } else if ((mode & 0xFFu) == 1u) {
        uint32_t dst_byte = dst_off;
        uint32_t src_byte = (be300_latch_peek_u32(d, 0x0218) & 0xFFFFu)
            | ((be300_latch_peek_u32(d, 0x021C) & 0xFFFFu) << 16);
        bool h_start_right = (mode & 0x400u) != 0;
        bool v_start_bottom = (mode & 0x800u) != 0;
        bool reverse_y;
        size_t fb_size = fb->framebuffer_size;
        size_t row_bytes;

        if (h_start_right) {
            uint64_t back = (uint64_t)(width - 1u) * bpp;
            if ((uint64_t)src_byte < back || (uint64_t)dst_byte < back)
                goto complete;
            src_byte = (uint32_t)((uint64_t)src_byte - back);
            dst_byte = (uint32_t)((uint64_t)dst_byte - back);
        }
        if (v_start_bottom) {
            uint64_t back = (uint64_t)(height - 1u) * stride;
            if ((uint64_t)src_byte < back || (uint64_t)dst_byte < back)
                goto complete;
            src_byte = (uint32_t)((uint64_t)src_byte - back);
            dst_byte = (uint32_t)((uint64_t)dst_byte - back);
        }

        if ((uint64_t)src_byte >= (uint64_t)fb_size ||
            (uint64_t)dst_byte >= (uint64_t)fb_size)
            goto complete;

        row_bytes = (size_t)width * bpp;
        reverse_y = dst_byte > src_byte;
        for (uint32_t yi = 0; yi < height; yi++) {
            uint32_t y = reverse_y ? (height - 1u - yi) : yi;
            uint64_t src_row_off = (uint64_t)src_byte + (uint64_t)y * stride;
            uint64_t dst_row_off = (uint64_t)dst_byte + (uint64_t)y * stride;
            size_t this_row_bytes = row_bytes;

            if (src_row_off >= (uint64_t)fb_size ||
                dst_row_off >= (uint64_t)fb_size)
                continue;
            if (this_row_bytes > fb_size - (size_t)src_row_off)
                this_row_bytes = fb_size - (size_t)src_row_off;
            if (this_row_bytes > fb_size - (size_t)dst_row_off)
                this_row_bytes = fb_size - (size_t)dst_row_off;
            memmove(fb->framebuffer + dst_row_off,
                fb->framebuffer + src_row_off, this_row_bytes);
        }

        {
            uint64_t len64 = (uint64_t)(height - 1u) * stride
                + (uint64_t)width * bpp;
            uint32_t len = len64 > UINT32_MAX ? UINT32_MAX : (uint32_t)len64;
            be300_fb_mark_dirty(m, dst_byte, len);
        }
    }

complete:
    d->bytes[0x0234] &= ~(uint8_t)1u;   /* trigger/busy complete */
}
