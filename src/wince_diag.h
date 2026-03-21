#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <unicorn/unicorn.h>

/* Forward declaration */
typedef struct machine_s machine_t;

/* ------------------------------------------------------------------ */
/* WinCE diagnostic PA region constants                                */
/* ------------------------------------------------------------------ */

#define WINCE_TRACE_VEC_PA_START UINT32_C(0x00000000)
#define WINCE_TRACE_VEC_PA_END   UINT32_C(0x00000400)
#define WINCE_TRACE_CTX_PA_START UINT32_C(0x00002000)
#define WINCE_TRACE_CTX_PA_END   UINT32_C(0x00002400)
#define WINCE_TRACE_CB_PA_START  UINT32_C(0x00051680)
#define WINCE_TRACE_CB_PA_END    UINT32_C(0x00051B00)
#define WINCE_TRACE_OBJPTR_PA_START UINT32_C(0x00660000)
#define WINCE_TRACE_OBJPTR_PA_END   UINT32_C(0x00660040)
#define WINCE_TRACE_OBJ_EXEC_PAGE_PA_START UINT32_C(0x00669000)
#define WINCE_TRACE_OBJ_EXEC_PAGE_PA_END   UINT32_C(0x0066A000)
#define WINCE_TRACE_OBJ_SLOT0_PA_START UINT32_C(0x0066BFC0)
#define WINCE_TRACE_OBJ_SLOT0_PA_END   UINT32_C(0x0066BFC4)
#define WINCE_TRACE_OBJ_HEADER_PA_START UINT32_C(0x0066BFC0)
#define WINCE_TRACE_OBJ_HEADER_PA_END   UINT32_C(0x0066BFE0)
#define WINCE_TRACE_OBJ_TAIL_STATS_PA_START UINT32_C(0x0067BFF4)
#define WINCE_TRACE_OBJ_TAIL_STATS_PA_END   UINT32_C(0x0067C000)
#define WINCE_TRACE_OBJ_PA_START UINT32_C(0x0066BFC0)
#define WINCE_TRACE_OBJ_PA_END   UINT32_C(0x00680000)
#define WINCE_TRACE_RESUME_GLOBAL_PA_START UINT32_C(0x006794EC)
#define WINCE_TRACE_RESUME_GLOBAL_PA_END   UINT32_C(0x00679514)
#define WINCE_TRACE_GATE_PA_START UINT32_C(0x000795E8)
#define WINCE_TRACE_GATE_PA_END   UINT32_C(0x00079604)
#define WINCE_TRACE_GATE_SRC_PA_START UINT32_C(0x00E18C00)
#define WINCE_TRACE_GATE_SRC_PA_END   UINT32_C(0x00E19000)
#define WINCE_TRACE_STACK_PA_START UINT32_C(0x00003780)
#define WINCE_TRACE_STACK_PA_END   UINT32_C(0x00003820)
#define WINCE_TRACE_CALLER_FRAME_PA_START UINT32_C(0x00001700)
#define WINCE_TRACE_CALLER_FRAME_PA_END   UINT32_C(0x000017C0)
#define WINCE_TRACE_S0_OBJ_PA_START UINT32_C(0x00001A80)
#define WINCE_TRACE_S0_OBJ_PA_END   UINT32_C(0x00001B00)
#define WINCE_TRACE_FPTBL_PA_START UINT32_C(0x00075580)
#define WINCE_TRACE_FPTBL_PA_END   UINT32_C(0x000755A0)
#define WINCE_TRACE_BOOTCTX_PA_START UINT32_C(0x00006000)
#define WINCE_TRACE_BOOTCTX_PA_END   UINT32_C(0x00007000)
#define WINCE_TRACE_BOOTPARAM0_PA_START UINT32_C(0x0001D000)
#define WINCE_TRACE_BOOTPARAM0_PA_END   UINT32_C(0x0001E000)
#define WINCE_TRACE_BOOTPARAM1_PA_START UINT32_C(0x0002D000)
#define WINCE_TRACE_BOOTPARAM1_PA_END   UINT32_C(0x0002E000)

#define WINCE_FAIL_CORRIDOR_START UINT32_C(0x80077FE4)
#define WINCE_FAIL_CORRIDOR_END   UINT32_C(0x80078C0C)

#define WINCE_NK_TRACE_BASE UINT32_C(0x80020000)
#define WINCE_NK_TRACE_END  UINT32_C(0x80F00000)

#define WINCE_STALL_SAMPLE_COUNT 3u
#define WINCE_SPL_VA_BASE UINT32_C(0x80F00000)
#define WINCE_SPL_VA_END  UINT32_C(0x80F10000)

static inline bool is_wince_spl_pc(uint32_t pc32)
{
    return pc32 >= WINCE_SPL_VA_BASE && pc32 < WINCE_SPL_VA_END;
}

/* ------------------------------------------------------------------ */
/* WinCE diagnostic initialization                                     */
/* ------------------------------------------------------------------ */

void init_wince_region_tracks(machine_t *m);

/* ------------------------------------------------------------------ */
/* Unicorn memory hook callbacks (registered from machine_create)      */
/* ------------------------------------------------------------------ */

bool wince_pa_watch_write_hook(uc_engine *uc, uc_mem_type type,
                               uint64_t address, int size,
                               int64_t value, void *user_data);
bool wince_despec_read_hook(uc_engine *uc, uc_mem_type type,
                            uint64_t address, int size,
                            int64_t value, void *user_data);
bool wince_fptbl_read_hook(uc_engine *uc, uc_mem_type type,
                           uint64_t address, int size,
                           int64_t value, void *user_data);

/* ------------------------------------------------------------------ */
/* Per-instruction diagnostic hooks (called from prid_hook)            */
/* ------------------------------------------------------------------ */

void wince_div_call_trace_step(machine_t *m, uc_engine *uc,
                               uint32_t pc32, uint32_t insn);
void wince_div_stack_watch_step(machine_t *m, uc_engine *uc,
                                uint32_t pc32, uint32_t insn);
void wince_div_hist_record(machine_t *m, uc_engine *uc,
                           uint32_t pc32, uint32_t insn);
void maybe_record_wince_ctrl_event(machine_t *m, uc_engine *uc,
                                   uint32_t pc32, uint32_t insn,
                                   uint32_t op, uint32_t rs, uint32_t rt,
                                   uint32_t rd, uint32_t sel);
void maybe_probe_wince_ctx_path(machine_t *m, uc_engine *uc,
                                uint32_t pc32, uint32_t insn,
                                uint32_t op, uint32_t rs,
                                uint32_t rt, uint32_t rd, uint32_t sel);
void maybe_probe_wince_bootctx_gate(machine_t *m, uc_engine *uc,
                                    uint32_t pc32);
void maybe_probe_wince_live_call_targets(machine_t *m, uc_engine *uc,
                                         uint32_t pc32);
void maybe_probe_wince_gate_path_hits(machine_t *m, uc_engine *uc,
                                      uint32_t pc32);
void maybe_probe_wince_bootmode(machine_t *m, uc_engine *uc,
                                uint32_t pc32, uint32_t insn);
void maybe_probe_wince_obj_dispatch(machine_t *m, uc_engine *uc,
                                    uint32_t pc32, uint32_t insn);
void maybe_probe_wince_bootmode_sp_flow(machine_t *m, uc_engine *uc,
                                        uint32_t pc32, uint32_t insn);
void maybe_probe_wince_spl_memcpy(machine_t *m, uc_engine *uc,
                                  uint32_t pc32, uint32_t insn);

/* ------------------------------------------------------------------ */
/* De-speculation first-touch diagnostics                              */
/* ------------------------------------------------------------------ */

void wince_despec_first_touch_check(machine_t *m, bool is_write,
                                    uint32_t pa, uint32_t size,
                                    uint64_t value, uint32_t pc);

/* ------------------------------------------------------------------ */
/* Diagnostic summaries & logging                                      */
/* ------------------------------------------------------------------ */

void log_wince_stall_dump(machine_t *m, uint32_t pc32);
void log_wince_null_mmio_tail(const machine_t *m, uint32_t max_lines);
void log_wince_pa_watch_summary(const machine_t *m, const char *reason);
void log_wince_pa_watch_nonzero(const machine_t *m, const char *reason);
void log_wince_region_track_summary(const machine_t *m, const char *reason);
void log_wince_ctrl_hist_summary(const machine_t *m,
                                 const char *reason, uint32_t max_entries);
void log_wince_div_hist_summary(const machine_t *m,
                                const char *reason, uint32_t max_entries);
void log_wince_div_call_trace_summary(const machine_t *m, const char *reason);
void log_wince_div_stack_watch_summary(machine_t *m, uc_engine *uc,
                                       const char *reason);

/* ------------------------------------------------------------------ */
/* Producer reachability diagnostics                                    */
/* ------------------------------------------------------------------ */

void init_wince_producer_attrs(machine_t *m);
void wince_producer_record_write(machine_t *m, uint32_t pa, uint32_t pc,
                                 unsigned size, uint64_t value);
void wince_producer_record_read(machine_t *m, uint32_t pa, uint32_t pc,
                                unsigned size, uint64_t value);
void maybe_probe_wince_producer_pcs(machine_t *m, uc_engine *uc,
                                    uint32_t pc32, uint32_t insn);
void maybe_probe_wince_77FE4_internals(machine_t *m, uc_engine *uc,
                                        uint32_t pc32, uint32_t insn);
void maybe_probe_wince_dispatcher_corridor(machine_t *m, uc_engine *uc,
                                           uint32_t pc32, uint32_t insn);
void maybe_probe_wince_obj_late_writer(machine_t *m, uc_engine *uc,
                                       uint32_t pc32, uint32_t insn);
void maybe_probe_wince_ctx_launch(machine_t *m, uc_engine *uc,
                                  uint32_t pc32, uint32_t insn);
void maybe_probe_wince_ctx_save_writer(machine_t *m, uc_engine *uc,
                                       uint32_t pc32, uint32_t insn);
void maybe_probe_wince_delay_call_frame(machine_t *m, uc_engine *uc,
                                        uint32_t pc32, uint32_t insn);
void maybe_record_wince_delay_touch(machine_t *m, bool is_write,
                                    bool is_mmio, uint32_t pa,
                                    unsigned size, uint64_t value,
                                    uint32_t pc);
void log_wince_producer_summary(const machine_t *m, const char *reason);
void log_wince_obj_bootstrap_summary(const machine_t *m, const char *reason);
void log_wince_delay_call_summary(const machine_t *m, const char *reason);
void log_wince_resume_global_summary(const machine_t *m, uc_engine *uc,
                                     const char *reason);
void log_wince_callback_arm_summary(machine_t *m, uc_engine *uc,
                                    const char *reason);
void log_wince_future_frame_summary(machine_t *m, uc_engine *uc,
                                    const char *reason);
void log_wince_midbody_9c_state(machine_t *m, uc_engine *uc,
                                const char *reason, uint32_t pc_hint);
