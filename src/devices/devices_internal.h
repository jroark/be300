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
void be300_log_mmio(const char *name, int writeflag, uint32_t off,
    unsigned len, const unsigned char *data, uint32_t pc);

/* CLOCK_MONOTONIC reader, used for PIU timing and PCConnect deadlines. */
uint64_t be300_host_monotonic_ns(void);

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
void     piu_update_state(struct be300_input_device *d);
uint32_t button_girq0_source_bits(const struct be300_input_device *d);
void     button_ack_keyboard_source(struct be300_input_device *d);
