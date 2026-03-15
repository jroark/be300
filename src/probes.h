#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <unicorn/unicorn.h>

/* Forward declaration */
typedef struct machine_s machine_t;

/* Unicorn hook callbacks for kernel diagnostic probes.
 * Registered from machine_create() via uc_hook_add(). */

/* Register all probe hooks with Unicorn (called from machine_create). */
void probes_register_hooks(machine_t *m);

bool insn_invalid_hook(uc_engine *uc, void *user_data);

void checkpoint_hook(uc_engine *uc, uint64_t address,
                     uint32_t size, void *user_data);
void run_init_entry_trace_hook(uc_engine *uc, uint64_t address,
                               uint32_t size, void *user_data);
void initcall_trace_hook(uc_engine *uc, uint64_t address,
                         uint32_t size, void *user_data);
void rcu_probe_hook(uc_engine *uc, uint64_t address,
                    uint32_t size, void *user_data);
void init_execve_site_probe_hook(uc_engine *uc, uint64_t address,
                                 uint32_t size, void *user_data);
void pgui_probe_hook(uc_engine *uc, uint64_t address,
                     uint32_t size, void *user_data);
void irq_probe_hook(uc_engine *uc, uint64_t address,
                    uint32_t size, void *user_data);
void exception_vector_probe_hook(uc_engine *uc, uint64_t address,
                                 uint32_t size, void *user_data);
void page_fault_probe_hook(uc_engine *uc, uint64_t address,
                           uint32_t size, void *user_data);
void icu_etime_fixup_hook(uc_engine *uc, uint64_t address,
                          uint32_t size, void *user_data);
