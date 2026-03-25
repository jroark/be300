#pragma once

#include <stddef.h>
#include <stdint.h>

#include "be300.h"

struct cpu;
struct machine;

void wince_boot_attach_machine(machine_t *m);
void wince_boot_detach_machine(machine_t *m);
void wince_boot_init(machine_t *m);
void wince_boot_note_spl_handoff(machine_t *m);
void wince_boot_note_cold_boot_redirect(machine_t *m, const char *detail);
void wince_boot_note_first_exception(machine_t *m, const char *detail);
void wince_boot_note_fatal_stop(machine_t *m, const char *reason);
void wince_boot_install_synthetic_low_vectors(machine_t *m,
    const uint32_t *handler, size_t word_count, const char *reason);
void wince_boot_apply_initial_seed(machine_t *m);
void wince_boot_apply_resume_seed(machine_t *m);
void wince_boot_on_vr41xx_tick(struct machine *gxm, struct cpu *cpu);
void wince_boot_note_timer_config(struct machine *gxm, struct cpu *cpu,
    uint64_t relative_addr, uint64_t value);
bool wince_boot_timer_irq_allowed(struct machine *gxm, struct cpu *cpu);
void wince_boot_note_low_vector_write(struct cpu *cpu, uint64_t paddr,
    size_t len);
void wince_boot_note_fb_oob(struct cpu *cpu, uint64_t paddr, size_t len);
