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
