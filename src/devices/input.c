/*
 *  src/devices/input.c — touchpanel and button input devices.
 *
 *  Touchpanel: VRC4173 PA 0x0A000300, size 0x60.
 *  Buttons:    VRC4173 PA 0x0A00A040, size 0x10.
 *
 *  Both share the be300_input_device struct and route through GIRQ0:
 *  GIU bit 8 -> GIRQ0 bit 0 (touch via PIUINTREG bit 9, buttons via bit 1).
 *
 *  Split out of src/be300_devices.c. Public API (declared in be300.h):
 *    be300_buttons_host_update, be300_touch_tick, be300_register_input.
 *  Internal callbacks consumed by the VRC4173 latch handler:
 *    piu_girq0_source_bits, button_girq0_source_bits,
 *    button_ack_keyboard_source.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"

#include "be300.h"
#include "be300_probe.h"
#include "devices.h"
#include "hw/piu.h"

#include "devices_internal.h"

struct be300_input_device {
    machine_t *m;
    bool       log_mmio;
    /* Button device state. */
    bool       button_irq_pending;
    bool       button_source_enabled;   /* AA000014 bit 3 */
    struct interrupt button_irq;  /* BE-300 GIRQ0 cascaded interrupt */
    bool       button_irq_connected;
    bool       button_irq_asserted;
    /* PIU state (touch device only). */
    piu_state_t piu;
    struct interrupt piu_irq;      /* BE-300 GIRQ0 cascaded interrupt */
    bool       piu_irq_connected;
    bool       piu_irq_asserted;
};

#define BUTTON_POWER_BIT        0x80u

/*
 *  PIU interrupt helper — idempotent assert/deassert through BE-300's
 *  cascaded GIU interrupt route.  The Linux4BE hardware notes describe
 *  touch as SYSINT1 bit 8 (GIU) -> GIUINTLREG bit 0 (GIRQ0) ->
 *  VRC4173 offset 0x0004 bit 9 (GIRQ0-9), then PIUINTREG at 0x0A000304.
 *  See docs/hardware/hardware.txt:15-19, :65-89, and :132-145.
 */
static void piu_irq_update(struct be300_input_device *d, bool want)
{
    if (!d->piu_irq_connected)
        return;
    if (want && !d->piu_irq_asserted) {
        INTERRUPT_ASSERT(d->piu_irq);
        d->piu_irq_asserted = true;
    } else if (!want && d->piu_irq_asserted) {
        INTERRUPT_DEASSERT(d->piu_irq);
        d->piu_irq_asserted = false;
    }
}

static void button_irq_update(struct be300_input_device *d, bool want)
{
    if (!d || !d->button_irq_connected)
        return;
    want = want && d->button_source_enabled;
    if (want && !d->button_irq_asserted) {
        INTERRUPT_ASSERT(d->button_irq);
        d->button_irq_asserted = true;
    } else if (!want && d->button_irq_asserted) {
        INTERRUPT_DEASSERT(d->button_irq);
        d->button_irq_asserted = false;
    }
}

uint32_t button_girq0_source_bits(const struct be300_input_device *d)
{
    /*
     * docs/hardware/hardware.txt:107-112 identifies GIRQ0-1 as the
     * keyboard SYSINTR source, and the real hardware idle dump keeps the
     * button block at 0x0A00A040 distinct from the generic NAND/latch
     * range (docs/hardware/hw_dump_vrc4173.txt:520).  Host button state
     * changes latch this cascaded source until the OAL writes the GIRQ0-1
     * acknowledge path at AA000014.
     */
    return (d && d->button_irq_pending && d->button_source_enabled) ?
        BUTTON_GIRQ0_SOURCE : 0;
}

void button_set_source_enabled(struct be300_input_device *d, bool enabled)
{
    if (!d)
        return;
    d->button_source_enabled = enabled;
    button_irq_update(d, d->button_irq_pending);
}

void button_ack_keyboard_source(struct be300_input_device *d)
{
    if (!d || !d->button_irq_pending)
        return;

    d->button_irq_pending = false;
    button_irq_update(d, false);
}

static bool button_change_is_power_release_only(uint8_t old_set1,
    uint8_t old_set2, uint8_t new_set1, uint8_t new_set2)
{
    return old_set1 == new_set1 &&
        ((old_set2 ^ new_set2) == BUTTON_POWER_BIT) &&
        (old_set2 & BUTTON_POWER_BIT) != 0 &&
        (new_set2 & BUTTON_POWER_BIT) == 0;
}

void be300_buttons_host_update(machine_t *m, uint8_t old_set1,
    uint8_t old_set2)
{
    struct be300_input_device *d;

    if (!m || !m->button_device)
        return;
    if (m->btn_set1 == old_set1 && m->btn_set2 == old_set2)
        return;

    d = (struct be300_input_device *)m->button_device;
    /*
     * The stock OAL has a separate SYSINT1 power-switch source
     * (docs/hardware/hardware.txt:15-18) and the keyboard source is used to
     * sample the active button bitmap.  Treating the power release as another
     * keyboard interrupt wakes SUSPEND immediately after the press powered the
     * guest down.  The live bit still clears; only the release edge is not a
     * keyboard wake source.
     */
    if (button_change_is_power_release_only(old_set1, old_set2,
        m->btn_set1, m->btn_set2))
        return;

    d->button_irq_pending = true;
    button_irq_update(d, true);
}

uint32_t piu_girq0_source_bits(const struct be300_input_device *d)
{
    if (!d)
        return 0;

    return piu_irq_pending(&d->piu) ? 0x00000200u : 0;
}

static void piu_refresh_irq(struct be300_input_device *d)
{
    piu_irq_update(d, piu_irq_pending(&d->piu));
}

/* The PIU's sample and conversion timing is guest-visible hardware time:
 * run it on emulated time, not the host clock. */
static uint64_t piu_now_ns(struct be300_input_device *d)
{
    return d ? be300_emulated_ns(d->m) : 0;
}

/*
 *  Touchpanel device — PA 0x0A000300, size 0x60
 *
 *  Register layout from docs/hardware/hardware.txt ISR analysis (GIRQ0-9):
 *
 *  +0x00 PIUCNTREG: bit 13 (0x2000) = pendown status
 *  +0x04 PIUINTREG: byte 0 (bits 7:0) = pending status (set by HW, W1C)
 *                   byte 1 (bits 15:8) = interrupt mask (R/W by driver)
 *                   ISR does: intr = W[304]; intr &= (intr >> 8);
 *                   Active bits: 0x5C -> SYSINTR_TOUCH, 0x01 -> SYSINTR_TOUCH_CHANGED
 *                   OAL masks delivered bits in the high byte; touch.dll
 *                   clears the low-byte pending bits with W1C writes.
 *  +0x20-0x2C, +0x50-0x5C: ADC coordinate buffers (y+, y-, x-, x+)
 */
static int be300_touch_handle(struct cpu *cpu, struct memory *mem,
    uint64_t relative_addr, unsigned char *data, size_t len, int writeflag,
    void *extra)
{
    struct be300_input_device *d = (struct be300_input_device *)extra;
    uint32_t off = (uint32_t)relative_addr;
    uint64_t val;


    be300_probe_note_mmio("vrc4173-piu-touch", off,
        writeflag == MEM_WRITE ? 'W' : 'R',
        (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
        BE300_MMIO_CLASS_KNOWN);

    if (writeflag == MEM_WRITE) {
        val = memory_readmax64(cpu, data, len);
        piu_write(&d->piu, off, (uint16_t)val, piu_now_ns(d));
        piu_refresh_irq(d);
        return 1;
    }

    val = piu_read(&d->piu, off, piu_now_ns(d));
    piu_refresh_irq(d);
    memory_writemax64(cpu, data, len, val);
    return 1;
}

/* --log-mmio logs after the access, so a read shows the value returned. */
DEVICE_ACCESS(be300_touch)
{
    struct be300_input_device *d = (struct be300_input_device *)extra;
    uint32_t pc = (uint32_t)cpu->pc;
    int r = be300_touch_handle(cpu, mem, relative_addr, data, len, writeflag,
        extra);

    if (r && d->log_mmio)
        be300_log_mmio("be300_touch", writeflag, (uint32_t)relative_addr, (unsigned)len, data, pc);
    return r;
}

/*
 *  Called once per BE-300 run-loop batch after host input is sampled to
 *  detect pen state changes and generate PIU interrupts when the scan
 *  sequencer is active.
 */
void be300_touch_tick(machine_t *m)
{
    struct be300_input_device *d;

    if (!m || !m->touch_device)
        return;
    d = (struct be300_input_device *)m->touch_device;
    piu_set_touch(&d->piu, m->touch_down, m->touch_x, m->touch_y,
        piu_now_ns(d));
    piu_refresh_irq(d);
}

/* Button register device — PA 0x0A00A040, size 0x10 */
static int be300_buttons_handle(struct cpu *cpu, struct memory *mem,
    uint64_t relative_addr, unsigned char *data, size_t len, int writeflag,
    void *extra)
{
    struct be300_input_device *d = (struct be300_input_device *)extra;
    machine_t *m = d->m;
    uint8_t reg[0x10] = { 0 };
    uint64_t val = 0;


    be300_probe_note_mmio("vrc4173-buttons", (uint32_t)relative_addr,
        writeflag == MEM_WRITE ? 'W' : 'R',
        (uint32_t)len, (uint64_t)(uint32_t)cpu->pc,
        BE300_MMIO_CLASS_KNOWN);

    if (writeflag == MEM_WRITE)
        return 1;

    /*
     * Real-hardware idle dump:
     *   docs/hardware/hw_dump_vrc4173.txt:520
     *   0x0A00A040: 00009EFF ...
     * Keep the low halfword at its quiescent value.  The stock WinCE
     * keybddr.dll initializes that halfword to 0x9EFF, then its interrupt
     * path performs a dummy lhu at 0x0A00A042 and samples the active-high
     * button bitmap at 0x0A00A044 masked by 0x9EFF
     * (keybddr.dll PCs 0x01A32460 and 0x01A3411C-0x01A34128).
     */
    reg[0x00] = 0xffu;
    reg[0x01] = 0x9eu;
    reg[0x04] = m->btn_set1;
    reg[0x05] = m->btn_set2;

    if ((uint32_t)relative_addr >= sizeof(reg))
        val = 0;
    else {
        uint32_t off = (uint32_t)relative_addr;
        unsigned n = (unsigned)len;
        if (n > 8)
            n = 8;
        for (unsigned i = 0; i < n && off + i < sizeof(reg); i++)
            val |= (uint64_t)reg[off + i] << (8u * i);
    }

    memory_writemax64(cpu, data, len, val);
    return 1;
}

/* --log-mmio logs after the access, so a read shows the value returned. */
DEVICE_ACCESS(be300_buttons)
{
    struct be300_input_device *d = (struct be300_input_device *)extra;
    uint32_t pc = (uint32_t)cpu->pc;
    int r = be300_buttons_handle(cpu, mem, relative_addr, data, len, writeflag,
        extra);

    if (r && d->log_mmio)
        be300_log_mmio("be300_buttons", writeflag, (uint32_t)relative_addr, (unsigned)len, data, pc);
    return r;
}

/*
 *  be300_register_input():
 *
 *  Register touch and button input devices.  Called for all boot modes
 *  after any latch/NAND device registration (address ranges are pre-carved).
 */
void be300_register_input(struct machine *gxm, machine_t *m, bool log_mmio)
{
    struct be300_input_device *touch_d, *btn_d;

    CHECK_ALLOCATION(touch_d = calloc(1, sizeof(struct be300_input_device)));
    touch_d->m = m;
    touch_d->log_mmio = log_mmio;
    piu_init(&touch_d->piu);

    /* Connect BE-300 touch through the cascaded GIU route documented in
     * docs/hardware/hardware.txt:15-19, :65-89, and :132-145:
     * SYSINT1 bit 8 (GIU) -> GIUINTLREG bit 0 (GIRQ0) ->
     * VRC4173 offset 0x0004 bit 9 (GIRQ0-9) -> PIUINTREG. */
    {
        char tmps[300];
        snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i.giu.0.girq0.%i",
            gxm->path, gxm->bootstrap_cpu, 8, 9);
        INTERRUPT_CONNECT(tmps, touch_d->piu_irq);
        touch_d->piu_irq_connected = true;
        touch_d->piu_irq_asserted = false;
    }

    m->touch_device = touch_d;
    g_be300_machine = m;

    memory_device_register(gxm->memory, "be300_touch",
        0x0A000300ULL, 0x60,
        dev_be300_touch_access, (void *)touch_d, DM_DEFAULT, NULL);

    CHECK_ALLOCATION(btn_d = calloc(1, sizeof(struct be300_input_device)));
    btn_d->m = m;
    btn_d->log_mmio = log_mmio;
    /* docs/hardware/hardware.txt:65-112 routes keyboard buttons as
     * SYSINT1 bit 8 (GIU) -> GIUINTLREG bit 0 (GIRQ0) ->
     * VRC4173 offset 0x0004 bit 1 (GIRQ0-1). */
    {
        char tmps[300];
        snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i.giu.0.girq0.%i",
            gxm->path, gxm->bootstrap_cpu, 8, 1);
        INTERRUPT_CONNECT(tmps, btn_d->button_irq);
        btn_d->button_irq_connected = true;
        btn_d->button_irq_asserted = false;
    }
    m->button_device = btn_d;
    memory_device_register(gxm->memory, "be300_buttons",
        0x0A00A040ULL, 0x10,
        dev_be300_buttons_access, (void *)btn_d, DM_DEFAULT, NULL);
}
