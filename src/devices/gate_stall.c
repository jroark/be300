/*
 *  src/devices/gate_stall.c — bus stalls on gated companion units.
 *
 *  On a real BE-300 a read or write of a companion unit whose clock gate
 *  (0x0A001100-0x0A00113C) is off never completes: the screen freezes, no
 *  interrupt or button gets through, and only a battery pull recovers it.
 *  A power-on leaves every gate off but 111C (glaze-os
 *  docs/NAND_BOOT_AUDIT.md, "Power-on findings"), and three boots hung
 *  exactly so:
 *
 *    framebuffer (PA 0x0A200000)  with 1100 or 110C off
 *    companion SIU (PA 0x0A008680) with 1108, 112C, 1138 and 113C all off
 *
 *  Only these two are modelled: which gate clocks which other unit isn't
 *  known yet, and the SIU is known to work once all four of its candidate
 *  gates are on, not which one it needs. The access that stalls is logged
 *  and the CPU stops for good; the window and screenshot keep working. The
 *  emulated CPU may finish the dyntrans block it is in (at most ~120
 *  instructions) before stopping, where the hardware stops at once.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"

#include "be300.h"
#include "devices_internal.h"

struct gated_device {
    const char *unit;
    uint64_t base;
    bool installed;
    int (*f)(struct cpu *, struct memory *, uint64_t, unsigned char *,
        size_t, int, void *);
    void *extra;
    int flags;          /* the device's own, restored while it is clocked */
    bool clocked;
};

static struct gated_device gated_fb = { "framebuffer", 0x0A200000ULL };
static struct gated_device gated_siu = { "companion SIU", 0x0A008680ULL };
static struct memory *gated_mem;
static struct cpu *gated_cpu;
static bool stalled;

static bool fb_clocked(void)
{
    return be300_cmu_gate_on(0x1100u) && be300_cmu_gate_on(0x110Cu);
}

static bool siu_clocked(void)
{
    return be300_cmu_gate_on(0x1108u) || be300_cmu_gate_on(0x112Cu) ||
        be300_cmu_gate_on(0x1138u) || be300_cmu_gate_on(0x113Cu);
}

bool be300_bus_stalled(void)
{
    return stalled;
}

static void stall(struct cpu *cpu, const struct gated_device *g,
    uint64_t addr, int writeflag, const char *gates)
{
    if (!stalled)
        fprintf(stderr, "[BUS] stall: %s %s at PA 0x%08" PRIx64 " (pc 0x%08"
            PRIx32 ") with clock gate %s off; a real BE-300 hangs here\n",
            writeflag == MEM_WRITE ? "write to" : "read of", g->unit,
            g->base + addr, (uint32_t)cpu->pc, gates);
    stalled = true;
    cpu->running = false;
    cpu->n_translated_instrs += N_DYNTRANS_IDLE_BREAK;  /* end this batch */
}

static int gated_fb_access(struct cpu *cpu, struct memory *mem,
    uint64_t addr, unsigned char *data, size_t len, int writeflag, void *extra)
{
    if (stalled || !fb_clocked()) {
        stall(cpu, &gated_fb, addr, writeflag, "1100/110C");
        if (writeflag != MEM_WRITE)
            memset(data, 0xFF, len);
        return 1;
    }
    return gated_fb.f(cpu, mem, addr, data, len, writeflag, extra);
}

static int gated_siu_access(struct cpu *cpu, struct memory *mem,
    uint64_t addr, unsigned char *data, size_t len, int writeflag, void *extra)
{
    if (stalled || !siu_clocked()) {
        stall(cpu, &gated_siu, addr, writeflag, "1108/112C/1138/113C");
        if (writeflag != MEM_WRITE)
            memset(data, 0xFF, len);
        return 1;
    }
    return gated_siu.f(cpu, mem, addr, data, len, writeflag, extra);
}

/* Looked up each time: GXemul reallocates its device array as devices are
 * registered. */
static struct memory_device *find(const struct gated_device *g)
{
    for (int i = 0; gated_mem && i < gated_mem->n_mmapped_devices; i++)
        if (gated_mem->devices[i].baseaddr == g->base)
            return &gated_mem->devices[i];
    return NULL;
}

static bool wrap(struct gated_device *g,
    int (*f)(struct cpu *, struct memory *, uint64_t, unsigned char *,
        size_t, int, void *))
{
    struct memory_device *dev = find(g);

    if (dev) {
        g->installed = true;
        g->f = dev->f;
        g->extra = dev->extra;
        g->flags = dev->flags;
        g->clocked = true;
        dev->f = f;
        return true;
    }
    fprintf(stderr, "[BUS] no %s device at PA 0x%08" PRIx64
        "; its gate is not modelled\n", g->unit, g->base);
    return false;
}

/*
 * The framebuffer is mapped straight into the translation cache, which
 * would bypass its access function: keep it out while its gates are off
 * and flush translations whenever that changes.
 */
void be300_gate_stall_update(void)
{
    struct gated_device *g = &gated_fb;
    struct memory_device *dev;
    bool clocked;

    if (!g->installed || !(dev = find(g)))
        return;
    clocked = fb_clocked();
    if (clocked == g->clocked)
        return;
    g->clocked = clocked;
    dev->flags = clocked ? g->flags :
        g->flags & ~(DM_DYNTRANS_OK | DM_DYNTRANS_WRITE_OK);
    if (gated_cpu && gated_cpu->invalidate_translation_caches)
        gated_cpu->invalidate_translation_caches(gated_cpu, 0, INVALIDATE_ALL);
}

void be300_gate_stall_install(struct machine *gxm)
{
    gated_mem = gxm->memory;
    gated_cpu = gxm->cpus[gxm->bootstrap_cpu];
    stalled = false;
    wrap(&gated_fb, gated_fb_access);
    wrap(&gated_siu, gated_siu_access);
    be300_gate_stall_update();
}
