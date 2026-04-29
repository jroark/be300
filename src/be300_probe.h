#pragma once

#include <stdbool.h>
#include <stdint.h>

struct cpu;
struct machine;

/*
 * Coverage class tags for be300_probe_note_mmio. The class records how the
 * emulator answered a given access so the shutdown coverage table can be
 * sorted by "forgiveness": KNOWN (register is actually modeled) vs STUBBED
 * (handler returns a synthesized constant / instant-completion stub) vs
 * LATCHED (catch-all store/return, pretending to be RAM) vs DEFAULT (fell
 * through a handler's switch without a real case).
 */
enum be300_mmio_class {
    BE300_MMIO_CLASS_KNOWN   = 0,
    BE300_MMIO_CLASS_STUBBED = 1,
    BE300_MMIO_CLASS_LATCHED = 2,
    BE300_MMIO_CLASS_DEFAULT = 3,
};

void be300_probe_attach(struct machine *machine);
void be300_probe_detach(struct machine *machine);

/*
 * Runtime options — set by machine_be300.c after be300_probe_attach, based on
 * CLI flags in machine_config_t. Called once per run.
 */
void be300_probe_set_options(bool mmio_coverage,
                             bool detect_stall,
                             uint32_t stall_window,
                             uint32_t stall_unique_threshold,
                             uint32_t stall_wall_secs);

void be300_probe_note_exec(struct cpu *cpu, uint64_t pc);

/*
 * First-hit MMIO coverage logger. Handlers call this on every read/write; the
 * implementation returns immediately if --mmio-coverage is off.
 *   dev: stable string label (compiler-deduped literal) identifying the device
 *   off: offset within the device's range
 *   op:  'R' or 'W'
 *   len: access width in bytes
 *   pc:  guest PC that issued the access (for first-hit line)
 *   mclass: one of enum be300_mmio_class
 */
void be300_probe_note_mmio(const char *dev, uint32_t off, char op,
    uint32_t len, uint64_t pc, int mclass);
