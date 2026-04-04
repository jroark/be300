#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WINCE_VECTOR_WORDS 8u

typedef enum {
    WINCE_VECTOR_NONE = 0,
    WINCE_VECTOR_SYNTHETIC = 1,
    WINCE_VECTOR_GUEST = 3,
} wince_vector_owner_t;

typedef struct {
    bool active;

    /* Checkpoint flags (one-shot logging) */
    bool spl_handoff_logged;
    bool cold_boot_redirected;
    bool timer_config_logged;
    bool first_exception_logged;
    bool fatal_exit_logged;
    bool timer_gate_logged;
    bool timer_kernel_gate_logged;
    bool timer_release_logged;
    bool low_vector_guest_write_logged;
    bool first_fb_oob_logged;
    bool vectors_ready;
    bool suppress_vector_write_observer;
    bool low_vector_observed_valid;
    bool low_vector_runtime_drift_logged;

    /* Cold boot state */
    bool cold_boot_wait_logged;
    bool cold_boot_copy_done;
    bool cold_boot_late_oal_wait_seen;
    uint32_t cold_boot_pc_probes_logged;

    /* Vector tracking */
    wince_vector_owner_t vector_owner;
    uint32_t synthetic_low_tlb[WINCE_VECTOR_WORDS];
    uint32_t synthetic_low_general[WINCE_VECTOR_WORDS];
    uint32_t observed_low_tlb[WINCE_VECTOR_WORDS];
    uint32_t observed_low_general[WINCE_VECTOR_WORDS];
} wince_boot_state_t;
