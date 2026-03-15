#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <unicorn/unicorn.h>

/* Forward declaration */
typedef struct machine_s machine_t;

/* Null function pointer call recovery.
 * Handles intno 20/26/27 (null JALR) for both Linux and WinCE boots.
 * Extracted from machine.c. */

/* Clear pending exception state after recovery. */
void clear_pending_exception_state(machine_t *m);

/* Main null-call interrupt dispatcher (called from intr_hook). */
bool handle_null_call_interrupt(machine_t *m, uc_engine *uc,
                                uint32_t intno, uint64_t pc,
                                uint64_t status);

/* WinCE interrupt passthrough (non-null-call WinCE exceptions). */
bool handle_wince_interrupt_passthrough(machine_t *m, uint32_t intno,
                                        uint64_t pc, uint64_t status);
