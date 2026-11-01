/*
 *  devices_internal.h — shared declarations for files split out of
 *  the original src/be300_devices.c. Included by src/be300_devices.c and
 *  the per-device files in src/devices/, and not anywhere else.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "be300.h"

struct cpu;
struct memory;
struct machine;
struct be300_vrc4173_latch;
struct be300_input_device;
struct be300_wince_aux;

/* Global state shared across the split-out device files. Defined in
 * src/be300_devices.c and seeded by the registration thunks. */
extern struct be300_vrc4173_latch *g_be300_vrc4173_latch;
extern machine_t                  *g_be300_machine;
extern struct be300_wince_aux     *g_be300_wince_aux;

/* GIRQ0-1 keyboard source bit. Real-hardware reference:
 *   docs/hardware/hardware.txt:107-112. */
#define BUTTON_GIRQ0_SOURCE  0x00000002u

/* MMIO trace helper (used by every device handler when --log-mmio is on). */
/*
 * Copy between a host buffer and guest physical memory through the CPU's
 * memory path: it crosses GXemul's 1 MiB RAM blocks, reaches devices (the
 * framebuffer), ignores addresses past RAM, and drops dyntrans translations
 * of the code it overwrites. Returns false if an access fails.
 */
bool be300_phys_copy(struct cpu *cpu, uint64_t pa, void *buf, size_t len,
    int writeflag);

void be300_log_mmio(const char *name, int writeflag, uint32_t off,
    unsigned len, const unsigned char *data, uint32_t pc);

/* CLOCK_MONOTONIC reader, for host-paced timing: the PC Connect insert
 * delay, a human-scale delay before the host link appears. */
/* Emulated time: the CPU's cycle count at the machine's emulated_hz. Use
 * it for guest-visible device timing, so host speed doesn't change it. */
uint64_t be300_emulated_ns(const machine_t *m);

/* Helpers that cross device boundaries. */
void be300_cf_irq_update(struct be300_vrc4173_latch *d);
void be300_pcconnect_reset_for_cpu_reset(struct be300_vrc4173_latch *d);

/* WinCE auxiliary / PPSH device register helper (definition in
 * src/devices/wince_aux.c). Sets up the parallel-port debug-shell device
 * at PA 0x0C000120: a real PPSH transport when enable_ppsh, otherwise an
 * idle-RAM stub that returns the "no debug host" status word. */
void be300_register_wince_aux(struct machine *gxm, bool log_mmio,
    bool enable_ppsh);

/* PIU/button cross-device callbacks (definitions in src/devices/input.c). */
uint32_t piu_girq0_source_bits(const struct be300_input_device *d);
uint32_t button_girq0_source_bits(const struct be300_input_device *d);
void     button_ack_keyboard_source(struct be300_input_device *d);
/* AA000014 bit 3, the GIRQ0-1 source enable, and whether the source can
 * drive GIU line 0 at all (be300_girq0_line_live, be300_devices.c). */
void     button_set_source_enabled(struct be300_input_device *d, bool enabled,
    bool line_live);
/* Touch's line follows AA000034 and AA000008 too; called when they change. */
void     be300_touch_irq_refresh(struct be300_input_device *d);
uint16_t be300_touch_adport(void *touch_device);   /* companion 0x0390 */
/* Bus stalls on gated companion units (src/devices/gate_stall.c). */
void     be300_gate_stall_install(struct machine *gxm);
void     be300_gate_stall_update(void);     /* after a gate write */
bool     be300_bus_stalled(void);
/* Log an access real hardware never completes and stop the CPU for good. */
void     be300_bus_stall(struct cpu *cpu, const char *unit, uint64_t pa,
                         int writeflag, const char *why);
/* Whether the clock gate at companion offset 0x1100-0x113C reads on. */
bool     be300_cmu_gate_on(uint32_t gate_off);
/* A GIRQ0 source word at en_off reaches GIU line 0 (bits 2-3 and AA000008). */
bool     be300_girq0_line_live(struct be300_vrc4173_latch *d, uint32_t en_off);

/* be300_devices.c: the ScCmcu watchdog's reset (hw/cmcu.h), from the
 * machine loop. */
void be300_cmcu_tick(machine_t *m);
