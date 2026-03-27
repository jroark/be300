#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WINCE_VECTOR_WORDS 8u

typedef enum {
    WINCE_VECTOR_NONE = 0,
    WINCE_VECTOR_SYNTHETIC = 1,
    WINCE_VECTOR_SEEDED = 2,
    WINCE_VECTOR_GUEST = 3,
} wince_vector_owner_t;

typedef enum {
    WINCE_RESUME_MODE_INIT_SEED = 0,
    WINCE_RESUME_MODE_REPLAY = 1,
} wince_resume_mode_t;

typedef struct {
    const char *name;
    uint32_t    pa;
    uint32_t    size;
    uint32_t    crc32;
    const uint8_t *data;
} wince_pa_seed_region_t;

typedef struct {
    const char *name;
    uint32_t    va;
    uint32_t    size;
    uint32_t    crc32;
    const uint8_t *data;
} wince_va_seed_region_t;

typedef struct {
    const char *name;
    uint32_t    pa;
    uint32_t    size;
    uint32_t    word_count;
    const uint32_t *words;
    const uint8_t *valid_words;
} wince_resume_region_t;

typedef struct {
    const char *name;
    uint32_t    reg;
    uint32_t    value;
} wince_resume_cp0_field_t;

typedef struct {
    uint32_t    resume_target_pc;
    uint32_t    resume_stack_pointer;
    uint32_t    synthetic_ra;
    const wince_resume_region_t *regions;
    uint32_t    region_count;
    const wince_resume_cp0_field_t *cp0_fields;
    uint32_t    cp0_field_count;
} wince_resume_snapshot_t;

typedef struct {
    bool active;
    bool log_stall;
    bool use_hw_seed;
    bool use_resume_replay;

    bool spl_handoff_logged;
    bool cold_boot_redirected;
    bool timer_config_logged;
    bool first_exception_logged;
    bool fatal_exit_logged;
    bool timer_gate_logged;
    bool timer_release_logged;
    bool low_vector_guest_write_logged;
    bool first_fb_oob_logged;
    bool initial_seed_applied;
    bool resume_seed_applied;
    bool vectors_ready;
    bool suppress_vector_write_observer;
    bool replay_snapshot_applied;
    bool replay_snapshot_logged;
    bool replay_synthetic_ra_attempted;
    bool replay_callback_redirect_attempted;
    bool replay_helper_tlb_installed;
    bool replay_bootctx_stub_poststate_logged;
    bool replay_resume_entry_logged;
    bool replay_resume_exit_logged;
    bool replay_vrc4173_1b50_synth_logged;
    bool replay_dispatch_page_runtime_refilled;

    uint32_t fault_site_logged_mask;
    uint32_t replay_region_drift_logged_mask;
    uint32_t replay_region_write_logged_mask;
    uint32_t replay_region_mismatch_logged_mask;
    uint32_t replay_pc_probe_logged_mask;
    uint32_t replay_bootctx_epc_probe_logged_mask;
    uint32_t replay_watch_write_logged_mask;
    uint32_t replay_mmio_watch_logged_mask;
    uint32_t replay_mmio_read_logged_mask;
    uint32_t replay_resume_halt_pc;
    uint32_t replay_resume_entry_pc;
    uint32_t replay_resume_target_pc;
    uint32_t replay_resume_stack_pointer;
    uint32_t replay_synthetic_ra;
    uint32_t hibernate_redirect_count;
    wince_vector_owner_t vector_owner;
    wince_resume_mode_t resume_mode;
    uint32_t synthetic_low_tlb[WINCE_VECTOR_WORDS];
    uint32_t synthetic_low_general[WINCE_VECTOR_WORDS];
} wince_boot_state_t;
