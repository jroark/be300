#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <unicorn/unicorn.h>

/* Forward declaration */
typedef struct machine_s machine_t;

/* Boot-time seeding & warm profiles (called from machine_create). */
void apply_wince_warm_profile(machine_t *m, const char *marker);

/* Deferred PA seeding (called from prid_hook when probe PA is first written). */
void seed_wince_probe_deferred(machine_t *m, uint32_t pc32);

/* Per-instruction WinCE boot helpers (called from prid_hook). */
bool maybe_emulate_wince_objptr_init_call(machine_t *m, uc_engine *uc,
                                          uint32_t pc32, uint32_t insn);
bool maybe_skip_wince_bootmode_delay_call(machine_t *m, uc_engine *uc,
                                          uint32_t pc32, uint32_t insn);
