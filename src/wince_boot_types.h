#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WINCE_VECTOR_WORDS 8u
#define WINCE_PC_RING_SIZE 512u

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
    bool timer_release_logged;
    bool low_vector_guest_write_logged;
    bool first_fb_oob_logged;
    bool vectors_ready;
    bool suppress_vector_write_observer;
    bool suppress_watch_observer;
    bool low_vector_observed_valid;
    bool low_vector_runtime_drift_logged;

    /* Cold boot state */
    bool cold_boot_copy_done;
    uint32_t cold_boot_pc_probes_logged;
    uint32_t boot_path_probe_mask;

    /* Vector tracking */
    wince_vector_owner_t vector_owner;
    uint32_t synthetic_low_tlb[WINCE_VECTOR_WORDS];
    uint32_t synthetic_low_general[WINCE_VECTOR_WORDS];
    uint32_t observed_low_tlb[WINCE_VECTOR_WORDS];
    uint32_t observed_low_general[WINCE_VECTOR_WORDS];

    /* PC ring buffer — sampled every device tick for crash analysis */
    uint32_t pc_ring[WINCE_PC_RING_SIZE];
    uint32_t pc_ring_sp[WINCE_PC_RING_SIZE];
    uint32_t pc_ring_status[WINCE_PC_RING_SIZE];
    uint32_t pc_ring_idx;
    bool     pc_ring_active;

    /* Targeted RAM/MMIO watch counters */
    uint16_t ram_watch_read_count;
    uint16_t ram_watch_write_count;
    uint16_t mmio_watch_read_count;
    uint16_t mmio_watch_write_count;
} wince_boot_state_t;
