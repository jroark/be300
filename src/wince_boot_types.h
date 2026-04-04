#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WINCE_VECTOR_WORDS 8u
#define WINCE_REPLAY_SLOT_1AC0_WORDS 16u

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
    bool timer_kernel_gate_logged;
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
    bool replay_tlb_idx01_even_clone_applied;
    bool replay_tlb_b000_helper_installed;
    bool replay_tlb_b000_even_clone_applied;
    bool replay_exec_80000180_late_logged;
    bool replay_etimer_consumed;
    bool replay_exec_80000180_post_etimer_logged;
    bool replay_exec_80094f08_post_etimer_logged;
    bool replay_exec_80094f24_post_etimer_logged;
    bool replay_exec_80094f58_post_etimer_logged;
    bool replay_exec_8008b4f0_post_etimer_logged;
    bool replay_exec_8008b478_post_etimer_logged;
    bool replay_dispatch_page_runtime_refilled;
    bool replay_post_handler_return_armed;
    bool replay_post_handler_return_logged;
    bool replay_post_handler_mutation_logged;
    bool low_vector_observed_valid;
    bool low_vector_runtime_drift_logged;
    bool replay_slot_1ac0_baseline_valid;
    bool replay_slot_1ac0_drift_logged;
    bool cold_boot_wait_logged;
    bool cold_boot_copy_done;
    bool cold_boot_late_oal_wait_seen;
    uint32_t cold_boot_wait_count;
    uint32_t cold_boot_pc_probes_logged;
    bool cold_boot_oal_intercept_logged;

    uint32_t fault_site_logged_mask;
    uint32_t replay_region_drift_logged_mask;
    uint32_t replay_region_write_logged_mask;
    uint32_t replay_region_mismatch_logged_mask;
    uint64_t replay_pc_probe_logged_mask;
    uint32_t replay_exec_probe_logged_mask;
    uint32_t replay_bootctx_epc_probe_logged_mask;
    uint32_t replay_watch_write_logged_mask;
    uint32_t replay_mmio_watch_logged_mask;
    uint32_t replay_mmio_read_logged_mask;
    uint32_t replay_resume_halt_pc;
    uint32_t replay_resume_entry_pc;
    uint32_t replay_resume_target_pc;
    uint32_t replay_resume_stack_pointer;
    uint32_t replay_synthetic_ra;
    uint32_t replay_post_handler_return_pc;
    uint32_t hibernate_redirect_count;
    wince_vector_owner_t vector_owner;
    wince_resume_mode_t resume_mode;
    uint32_t synthetic_low_tlb[WINCE_VECTOR_WORDS];
    uint32_t synthetic_low_general[WINCE_VECTOR_WORDS];
    uint32_t observed_low_tlb[WINCE_VECTOR_WORDS];
    uint32_t observed_low_general[WINCE_VECTOR_WORDS];
    uint32_t replay_slot_1ac0_baseline[WINCE_REPLAY_SLOT_1AC0_WORDS];
} wince_boot_state_t;
