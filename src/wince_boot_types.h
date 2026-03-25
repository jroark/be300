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
    bool active;
    bool log_stall;
    bool use_hw_seed;

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

    wince_vector_owner_t vector_owner;
    uint32_t synthetic_low_tlb[WINCE_VECTOR_WORDS];
    uint32_t synthetic_low_general[WINCE_VECTOR_WORDS];
} wince_boot_state_t;
