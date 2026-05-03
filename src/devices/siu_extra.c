/*
 *  src/devices/siu_extra.c — VR4131 SIU and DSIU extension windows.
 *
 *  GXemul's dev_vr41xx caps internal I/O at offset 0x800 and registers an
 *  ns16550 for SIU (base+0x800, 8 bytes) and DSIU (base+0x820, 8 bytes).
 *  Real silicon and the WinCE drivers reach further: SIU exposes
 *  registers up to +0x1E, and DSIU up to +0x5F. This file fills those
 *  extension windows: SIU forwards to siu.c, DSIU is stubbed.
 *
 *  Split out of src/be300_devices.c. Public API (declared in be300.h):
 *    be300_register_vr4131_siu.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "cpu.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"

#include "be300.h"
#include "be300_probe.h"
#include "devices.h"
#include "hw/siu.h"

#include "devices_internal.h"

/*
 *  VR4131 SIU / DSIU extension registers (hardware.txt:201-202,
 *  VR4131 UM §20 / §22).
 *
 *  hardware.txt lists:
 *      0xaf000800  SIU Base on vr4131
 *      0xaf000820  DSIU Base on vr4131
 *
 *  dev_vr41xx.c caps its own claimed range at DEV_VR41XX_LENGTH=0x800
 *  and then registers two ns16550 devices (at baseaddr+0x800 for SIU
 *  and baseaddr+0x820 for DSIU). Both are DEV_NS16550_LENGTH=8 with
 *  default addr_mult=1, so the ns16550s cover only offsets 0x00..0x07
 *  of each block.
 *
 *  The VR4131 SIU/DSIU actually extend to offset 0x0E (SCR) on SIU and
 *  further on DSIU (per VR4131 UM). gwes.exe driver chain at UM
 *  0x0194xxxx pokes MCR at SIU+0x08, LSR at SIU+0x0A, and DSIU+0x38 —
 *  all beyond the ns16550 8-byte window — and gets "non-existent paddr"
 *  warnings from gxemul core.
 *
 *  Pass 22 fix: register our own device windows for the ranges the
 *  ns16550 does not cover. SIU extensions (+0x08..+0x1F) are dispatched
 *  to siu_read/siu_write (which uses 2-byte stride — see
 *  `offset &= ~1u` in siu.c). DSIU extensions (+0x08..+0x5F) are
 *  stubbed: reads return 0, writes are accepted silently. If a driver
 *  polls a DSIU status bit that doesn't naturally clear, a later pass
 *  can expand beyond a stub.
 */

struct be300_siu_device {
    siu_state_t *siu;
    uint32_t     base_offset; /* added to relative_addr before siu_read/write */
    bool         log_mmio;
};

DEVICE_ACCESS(be300_vr4131_siu)
{
    struct be300_siu_device *d = (struct be300_siu_device *)extra;
    uint32_t siu_offset = d->base_offset + (uint32_t)relative_addr;
    uint32_t pc = (uint32_t)cpu->pc;

    if (d->log_mmio)
        be300_log_mmio("be300_vr4131_siu", writeflag, siu_offset,
            (unsigned)len, data, pc);

    be300_probe_note_mmio("vr4131-siu-ext", siu_offset,
        writeflag == MEM_WRITE ? 'W' : 'R',
        (uint32_t)len, (uint64_t)pc, BE300_MMIO_CLASS_KNOWN);

    if (writeflag == MEM_WRITE) {
        uint64_t val = memory_readmax64(cpu, data, len);
        siu_write(d->siu, siu_offset, (unsigned)len, (uint32_t)val);
    } else {
        uint64_t val = siu_read(d->siu, siu_offset, (unsigned)len);
        memory_writemax64(cpu, data, len, val);
    }

    return 1;
}

DEVICE_ACCESS(be300_vr4131_dsiu)
{
    struct be300_siu_device *d = (struct be300_siu_device *)extra;
    uint32_t offset = d->base_offset + (uint32_t)relative_addr;
    uint32_t pc = (uint32_t)cpu->pc;

    if (d->log_mmio)
        be300_log_mmio("be300_vr4131_dsiu", writeflag, offset,
            (unsigned)len, data, pc);

    be300_probe_note_mmio("vr4131-dsiu-stub", offset,
        writeflag == MEM_WRITE ? 'W' : 'R',
        (uint32_t)len, (uint64_t)pc, BE300_MMIO_CLASS_STUBBED);

    if (writeflag == MEM_WRITE) {
        /* Accept silently. */
    } else {
        memory_writemax64(cpu, data, len, 0);
    }

    return 1;
}

void be300_register_vr4131_siu(struct machine *gxm, siu_state_t *siu,
                               bool log_mmio)
{
    struct be300_siu_device *siu_dev, *dsiu_dev;

    /* SIU extension window: 0x0F000808..0x0F00081F (ns16550 already
     * covers 0x0F000800..0x0F000807). siu_read/write expects offset
     * from SIU base, so base_offset=0x08. */
    CHECK_ALLOCATION(siu_dev = malloc(sizeof(struct be300_siu_device)));
    siu_dev->siu = siu;
    siu_dev->base_offset = 0x08;
    siu_dev->log_mmio = log_mmio;
    memory_device_register(gxm->memory, "be300_vr4131_siu_ext",
        0x0F000808ULL, 0x18,
        dev_be300_vr4131_siu_access, (void *)siu_dev, DM_DEFAULT, NULL);

    /* DSIU extension window: 0x0F000828..0x0F00087F (ns16550 covers
     * 0x0F000820..0x0F000827). Stubbed. */
    CHECK_ALLOCATION(dsiu_dev = malloc(sizeof(struct be300_siu_device)));
    dsiu_dev->siu = siu;
    dsiu_dev->base_offset = 0x08;
    dsiu_dev->log_mmio = log_mmio;
    memory_device_register(gxm->memory, "be300_vr4131_dsiu_ext",
        0x0F000828ULL, 0x58,
        dev_be300_vr4131_dsiu_access, (void *)dsiu_dev, DM_DEFAULT, NULL);
}
