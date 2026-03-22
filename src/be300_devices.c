/*
 *  be300_devices.c — GXemul DEVICE_ACCESS wrappers for BE-300 peripherals.
 *
 *  Registers our hw/*.c peripheral state structs as GXemul memory-mapped
 *  devices at the VRC4173 companion chip address range.
 *
 *  The VR4131 internal I/O (BCU, CMU, PMU, ICU, RTC, GPIO, SIU) is handled
 *  by GXemul's native dev_vr41xx.c. This file handles the VRC4173 NAND
 *  flash controller which dev_vr41xx doesn't cover.
 */

#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"

#include "be300.h"
#include "hw/nand.h"

/*
 *  VRC4173 NAND flash controller device.
 *
 *  Covers the NAND register space at PA 0x0A00A000 - 0x0A00D800.
 *  This is registered with GXemul's memory subsystem so that memory
 *  accesses from the emulated CPU are dispatched here.
 */

struct be300_nand_device {
    nand_state_t *nand;
    bool          log_mmio;
};

DEVICE_ACCESS(be300_nand)
{
    struct be300_nand_device *d = (struct be300_nand_device *)extra;
    uint32_t offset = (uint32_t)relative_addr + 0xA000;  /* add base offset */
    uint32_t pc = (uint32_t)cpu->pc;

    if (writeflag == MEM_WRITE) {
        uint64_t val = memory_readmax64(cpu, data, len);
        nand_write(d->nand, offset, (unsigned)len, val, d->log_mmio, pc);
    } else {
        uint64_t val = nand_read(d->nand, offset, (unsigned)len, d->log_mmio, pc);
        memory_writemax64(cpu, data, len, val);
    }

    return 1;
}

/*
 *  VRC4173 catch-all latch device.
 *
 *  The VRC4173 companion chip has many register blocks. The SPL reads
 *  board ID, NAND status, NE2000 config, and other registers during init.
 *  This latch captures all VRC4173 accesses not handled by the NAND device
 *  or GXemul's ns16550 (SIU).  Writes are stored, reads return last value.
 *
 *  Special addresses:
 *    0x0A0C0 (board ID) returns 0x7100 (BE-300 identifier)
 */

#define VRC4173_LATCH_BASE   0x0A000000ULL
#define VRC4173_LATCH_SIZE   0x00020000     /* 128KB covers all VRC4173 space */

struct be300_vrc4173_latch {
    uint8_t  bytes[0x20000];
    bool     log_mmio;
};

struct be300_vrc4173_segment {
    struct be300_vrc4173_latch *latch;
    uint32_t offset_in_latch;    /* offset of this segment within the latch */
};

DEVICE_ACCESS(be300_vrc4173)
{
    struct be300_vrc4173_segment *seg = (struct be300_vrc4173_segment *)extra;
    struct be300_vrc4173_latch *d = seg->latch;
    uint32_t off = seg->offset_in_latch + (uint32_t)relative_addr;

    if (off + len > VRC4173_LATCH_SIZE)
        return 0;

    if (writeflag == MEM_WRITE) {
        uint64_t val = memory_readmax64(cpu, data, len);
        memcpy(&d->bytes[off], data, len);
        if (d->log_mmio)
            fprintf(stderr, "[VRC4173] W PA=0x%08X size=%zu val=0x%llX PC=0x%08X\n",
                    (uint32_t)(VRC4173_LATCH_BASE + off), len,
                    (unsigned long long)val, (uint32_t)cpu->pc);
    } else {
        memcpy(data, &d->bytes[off], len);
        if (d->log_mmio)
            fprintf(stderr, "[VRC4173] R PA=0x%08X size=%zu val=0x%llX PC=0x%08X\n",
                    (uint32_t)(VRC4173_LATCH_BASE + off), len,
                    (unsigned long long)memory_readmax64(cpu, data, len),
                    (uint32_t)cpu->pc);
    }

    return 1;
}


/*
 *  be300_register_nand():
 *
 *  Register the NAND flash controller as a GXemul device.
 *  Called from machine_be300.c after NAND image is loaded.
 */
void be300_register_nand(struct machine *gxm, nand_state_t *nand, bool log_mmio)
{
    struct be300_nand_device *d;
    CHECK_ALLOCATION(d = malloc(sizeof(struct be300_nand_device)));
    d->nand = nand;
    d->log_mmio = log_mmio;

    /*
     * NAND registers span offsets 0xA000-0xD800 from VRC4173 base.
     * VRC4173 base = PA 0x0A000000.
     * So NAND is at PA 0x0A00A000, length 0x3800 bytes.
     */
    memory_device_register(gxm->memory, "be300_nand",
        0x0A00A000ULL, 0x3800,
        dev_be300_nand_access, (void *)d, DM_DEFAULT, NULL);

    fprintf(stderr, "[BE300] Registered NAND controller at PA 0x0A00A000-0x0A00D800\n");
}


/*
 *  be300_register_vrc4173_latch():
 *
 *  Register the VRC4173 catch-all latch as two non-overlapping segments
 *  that avoid the NAND device range (0x0A00A000-0x0A00D800) and the
 *  ns16550 SIU range (~0x0A008680).
 *
 *  Segment A: 0x0A000000 - 0x0A008000 (below SIU/NAND)
 *  Segment B: 0x0A00E000 - 0x0A020000 (above NAND)
 */
void be300_register_vrc4173_latch(struct machine *gxm, bool log_mmio)
{
    struct be300_vrc4173_latch *latch;
    CHECK_ALLOCATION(latch = calloc(1, sizeof(struct be300_vrc4173_latch)));
    latch->log_mmio = log_mmio;

    /* Seed board ID: offset 0x0A0C0 = 0x7100 (BE-300 identifier) */
    uint32_t board_id = 0x00007100;
    memcpy(&latch->bytes[0x0A0C0], &board_id, 4);

    /* Segment A: 0x0A000000 - 0x0A008000 (below SIU) */
    struct be300_vrc4173_segment *seg_lo;
    CHECK_ALLOCATION(seg_lo = malloc(sizeof(struct be300_vrc4173_segment)));
    seg_lo->latch = latch;
    seg_lo->offset_in_latch = 0;
    memory_device_register(gxm->memory, "vrc4173_lo",
        VRC4173_LATCH_BASE, 0x8000,
        dev_be300_vrc4173_access, (void *)seg_lo, DM_DEFAULT, NULL);

    /* Segment B: 0x0A00E000 - 0x0A020000 (above NAND) */
    struct be300_vrc4173_segment *seg_hi;
    CHECK_ALLOCATION(seg_hi = malloc(sizeof(struct be300_vrc4173_segment)));
    seg_hi->latch = latch;
    seg_hi->offset_in_latch = 0xE000;
    memory_device_register(gxm->memory, "vrc4173_hi",
        VRC4173_LATCH_BASE + 0xE000, 0x12000,
        dev_be300_vrc4173_access, (void *)seg_hi, DM_DEFAULT, NULL);

    fprintf(stderr, "[BE300] Registered VRC4173 latch (0x0A000000-0x0A008000, 0x0A00E000-0x0A020000)\n");
}
