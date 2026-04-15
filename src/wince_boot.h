#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "be300.h"

struct cpu;
struct machine;

typedef enum {
    WINCE_RAM_SOURCE_UNKNOWN = 0,
    WINCE_RAM_SOURCE_FAST = 1,
    WINCE_RAM_SOURCE_SLOW = 2,
} wince_boot_ram_source_t;

void wince_boot_attach_machine(machine_t *m);
void wince_boot_detach_machine(machine_t *m);
void wince_boot_init(machine_t *m);
void wince_boot_note_spl_handoff(machine_t *m);
void wince_boot_note_cold_boot_redirect(machine_t *m, const char *detail);
void wince_boot_note_first_exception(machine_t *m, const char *detail);
void wince_boot_note_fatal_stop(machine_t *m, const char *reason);
void wince_boot_on_vr41xx_tick(struct machine *gxm, struct cpu *cpu);
void wince_boot_note_timer_config(struct machine *gxm, struct cpu *cpu,
    uint64_t relative_addr, uint64_t value);
void wince_boot_note_interrupt_exception(struct cpu *cpu, uint32_t exccode);
void wince_boot_note_tlb_exception(struct cpu *cpu, uint32_t exccode,
    uint32_t vaddr);
void wince_boot_note_tlb_exception_post(struct cpu *cpu, uint32_t exccode,
    uint32_t vaddr);
void wince_boot_note_eret(struct cpu *cpu);
bool wince_boot_timer_irq_allowed(struct machine *gxm, struct cpu *cpu);
void wince_boot_note_low_vector_write(struct cpu *cpu, uint64_t paddr,
    size_t len);
void wince_boot_note_fb_oob(struct cpu *cpu, uint64_t paddr, size_t len);
void wince_boot_check_dma_autocopy(struct cpu *cpu);
void wince_boot_pc_ring_activate(machine_t *m);
void wince_boot_pc_ring_dump(machine_t *m);
void wince_boot_crash_pc_dump(struct machine *gxm);
void wince_boot_note_pc(struct cpu *cpu, uint32_t pc32);
void wince_boot_note_ppsh_command(struct cpu *cpu, uint16_t cmd);
void wince_boot_note_ppsh_status_read(struct cpu *cpu, uint16_t status);
void wince_boot_note_ppsh_data_read(struct cpu *cpu, uint16_t word);
void wince_boot_note_serial_tx(struct cpu *cpu, unsigned char ch);
void wince_boot_log_summary(machine_t *m);
bool wince_boot_should_observe_fast_ram(struct cpu *cpu, uint64_t paddr,
    size_t len);
void wince_boot_note_ram_access(struct cpu *cpu, uint64_t paddr,
    const unsigned char *data, size_t len, bool is_write,
    wince_boot_ram_source_t source);
void wince_boot_note_mmio_access(struct machine *gxm, struct cpu *cpu,
    uint64_t paddr, size_t len, uint64_t value, bool is_write);
void wince_boot_note_usermode_entry(machine_t *m);
void wince_boot_note_idle_transition(struct cpu *cpu, const char *event,
    const char *mode, uint32_t status, uint32_t cause, uint32_t enabled,
    uint32_t mask, uint32_t raw_pending, uint32_t count, uint32_t compare,
    int compare_pending, int is_halted);
