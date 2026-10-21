/*
 *  src/devices/boot_rom.c — boot ROM at PA 0x1FC00000.
 *
 *  The BE-300's 16 KB mask ROM (docs/hardware/BE300BootROM_v1.txt, dumped
 *  with a stable CRC across survey passes) is read-only. It used to be a
 *  plain dev_ram filled through CPU stores, so a stray guest store would
 *  have patched the ROM in place. Here dyntrans maps it read-only
 *  (DM_DYNTRANS_OK without DM_DYNTRANS_WRITE_OK, so stores come back to
 *  the access function), and stores are dropped and reported once.
 *
 *  Public API: be300_register_boot_rom (declared where machine_be300.c
 *  uses it, like the other device registrations).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"

#include "be300.h"
#include "devices.h"

#include "devices_internal.h"

#define BE300_BOOT_ROM_MIN 0x4000u
#define BE300_BOOT_ROM_PAGE 0x1000u

struct be300_boot_rom {
    unsigned char *data;    /* page-aligned view into storage */
    void *storage;
    size_t size;
    unsigned long writes;
};

DEVICE_ACCESS(be300_boot_rom)
{
    struct be300_boot_rom *d = (struct be300_boot_rom *)extra;

    (void)mem;
    if (relative_addr + len > d->size)
        return 0;

    if (writeflag == MEM_READ) {
        memcpy(data, d->data + relative_addr, len);
        return 1;
    }

    if (d->writes++ == 0)
        fprintf(stderr, "[BE300] Boot ROM is read-only: dropped a %zu-byte "
            "store to PA 0x%08llx (pc=0x%08x)\n", len,
            (unsigned long long)(PA_RESET_VECTOR + relative_addr),
            cpu ? (uint32_t)cpu->pc : 0u);
    return 1;
}

int be300_register_boot_rom(struct machine *gxm, const uint8_t *image,
    size_t len)
{
    struct be300_boot_rom *d;
    size_t size = len < BE300_BOOT_ROM_MIN ? BE300_BOOT_ROM_MIN : len;
    uintptr_t p;

    size = (size + BE300_BOOT_ROM_PAGE - 1u) & ~(size_t)(BE300_BOOT_ROM_PAGE - 1u);
    CHECK_ALLOCATION(d = calloc(1, sizeof(*d)));
    CHECK_ALLOCATION(d->storage = malloc(size + BE300_BOOT_ROM_PAGE));
    p = ((uintptr_t)d->storage + BE300_BOOT_ROM_PAGE - 1u) &
        ~(uintptr_t)(BE300_BOOT_ROM_PAGE - 1u);
    d->data = (unsigned char *)p;
    d->size = size;
    memset(d->data, 0xFF, size);
    memcpy(d->data, image, len);

    memory_device_register(gxm->memory, "be300_boot_rom",
        PA_RESET_VECTOR, size, dev_be300_boot_rom_access, d,
        DM_DYNTRANS_OK | DM_READS_HAVE_NO_SIDE_EFFECTS, d->data);
    return 0;
}
