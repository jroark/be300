#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WINCE_VECTOR_WORDS 8u
#define WINCE_PC_RING_SIZE 512u
#define WINCE_SEC0_L1_SLOT_RING_SIZE 32u
#define WINCE_SEC0_TLB_RING_SIZE 16u

typedef struct {
    uint64_t instrs;
    uint32_t pc;
    uint32_t epc;
    uint32_t vaddr;
    uint32_t asid;
    uint32_t exccode;
} wince_sec0_tlb_entry_t;


typedef struct {
    uint64_t paddr;
    uint64_t instrs;
    uint32_t val;
    uint32_t pc;
    uint32_t ra;
    uint32_t asid;
    uint8_t  source;
    uint8_t  len;
} wince_sec0_l1_slot_entry_t;

#define WINCE_FB_SAMPLE_CHUNKS 8u
#define WINCE_FB_SAMPLE_CHUNK_BYTES 16u
#define WINCE_FB_SAMPLE_BYTES \
    (WINCE_FB_SAMPLE_CHUNKS * WINCE_FB_SAMPLE_CHUNK_BYTES)

typedef enum {
    WINCE_VECTOR_NONE = 0,
    WINCE_VECTOR_GUEST = 3,
} wince_vector_owner_t;

typedef struct {
    bool valid;
    bool code_valid;
    bool thread_valid;
    bool proc_valid;
    bool aky_valid;
    bool pc_valid;
    bool ra_valid;
    bool bva_valid;
    uint32_t code;
    uint32_t thread;
    uint32_t proc;
    uint32_t aky;
    uint32_t pc;
    uint32_t ra;
    uint32_t bva;
    char process_name[64];
} wince_serial_exception_record_t;

typedef struct {
    bool seen;
    bool logged;
    uint32_t probe_va;
    uint32_t fault_va;
    uint32_t section_idx;
    uint32_t section_val;
    uint32_t l2_off;
    uint32_t l2_val;
    uint32_t pte_off;
    uint32_t lo0;
    uint32_t lo1;
    uint32_t selected_lo;
    uint32_t entryhi;
    uint32_t asid;
    uint32_t tlb_match_entryhi;
    uint32_t tlb_match_lo;
    uint32_t tlb_match_page_vbase;
    uint32_t tlb_match_paddr;
    uint32_t tlb_match_page_size;
    int16_t tlb_match_idx;
    int8_t  tlb_match_page;
    bool odd_page;
    bool selected_valid;
    bool tlb_match_found;
    bool tlb_match_active;
    bool tlb_match_asid_match;
    bool tlb_match_global;
    bool tlb_match_valid;
} wince_hot_page_verdict_t;

typedef struct {
    bool active;

    /* Checkpoint flags (one-shot logging) */
    bool spl_handoff_logged;
    bool cold_boot_redirected;
    bool timer_config_logged;
    bool first_exception_logged;
    bool fatal_exit_logged;
    bool tlb_fault_snapshot_logged;
    bool irq_exception_snapshot_logged;
    bool eret_snapshot_logged;
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
    uint64_t boot_path_probe_mask;

    /* Vector tracking */
    wince_vector_owner_t vector_owner;
    uint32_t observed_low_tlb[WINCE_VECTOR_WORDS];
    uint32_t observed_low_general[WINCE_VECTOR_WORDS];

    /* PC ring buffer — sampled every device tick for crash analysis */
    uint32_t pc_ring[WINCE_PC_RING_SIZE];
    uint32_t pc_ring_sp[WINCE_PC_RING_SIZE];
    uint32_t pc_ring_status[WINCE_PC_RING_SIZE];
    uint32_t pc_ring_idx;
    bool     pc_ring_active;
    bool     fb_watch_armed;
    bool     fb_watch_baseline_valid;
    bool     fb_watch_pc_ring_dumped;
    bool     toc_dumped;
    uint16_t fb_watch_report_count;
    uint16_t fb_write_diag_count;
    uint16_t process_map_diag_count;
    uint32_t fb_watch_arm_pc;
    uint8_t  fb_watch_baseline[WINCE_FB_SAMPLE_BYTES];

    /* Targeted RAM/MMIO watch counters */
    uint16_t ram_watch_read_count;
    uint16_t ram_watch_write_count;
    uint16_t mmio_watch_read_count;
    uint16_t mmio_watch_write_count;
    uint16_t idle_diag_count;
    uint16_t section_write_diag_count;
    uint16_t l2_write_diag_count;
    uint16_t tlb_post_diag_count;
    uint32_t section_table_shadow[64];
    uint32_t diag_shared_l2_table;
    uint32_t diag_process_l2_table;
    uint32_t last_tlb_post_vaddr;
    uint32_t last_tlb_post_pc;
    uint16_t last_tlb_post_repeat;

    /* PPSH loop attribution diagnostics */
    bool     ppsh_trace_armed;
    bool     ppsh_seq_active;
    bool     ppsh_seq_cap_logged;
    bool     ppsh_poll_active;
    uint16_t ppsh_cmd_seq_count;
    uint16_t ppsh_seq_status_reads;
    uint16_t ppsh_seq_data_reads;
    uint16_t ppsh_seq_read_budget;
    uint16_t ppsh_flow_diag_count;
    uint16_t ppsh_send_entry_count;
    uint16_t ppsh_read_entry_count;
    uint16_t ppsh_poll_episode_count;
    uint16_t ppsh_poll_exit_count;
    uint16_t ppsh_debug_msg_count;
    uint16_t ppsh_debug_dump_count;
    uint16_t ppsh_flag_transition_count;
    uint16_t ppsh_flag_write_count;
    uint16_t ppsh_flag_set_count;
    uint16_t ppsh_flag_clear_count;
    uint32_t ppsh_seq_cmd;
    uint32_t ppsh_seq_start_pc;
    uint32_t ppsh_seq_last_status;
    uint32_t ppsh_seq_last_data;
    uint32_t ppsh_poll_iters;
    uint32_t ppsh_poll_last_iters;
    uint32_t ppsh_poll_next_milestone;
    uint32_t ppsh_poll_entry_ra;
    uint32_t ppsh_poll_entry_sp;
    uint32_t ppsh_poll_exit_pc;
    uint32_t ppsh_flag_prev;
    uint32_t ppsh_last_debug_callsite;
    uint32_t ppsh_serial_first_pc;
    uint32_t ppsh_serial_first_ra;
    uint32_t ppsh_serial_last_pc;
    uint32_t ppsh_serial_last_ra;
    uint32_t ppsh_serial_first_sp;
    uint32_t ppsh_serial_first_a0;
    uint32_t ppsh_serial_first_a1;
    uint32_t ppsh_serial_first_a2;
    uint32_t ppsh_serial_first_a3;
    uint32_t ppsh_serial_first_v0;
    uint32_t ppsh_serial_first_v1;
    uint32_t ppsh_serial_first_s0;
    uint32_t ppsh_serial_first_s1;
    uint32_t ppsh_serial_first_s2;
    uint32_t ppsh_serial_first_s3;
    uint32_t ppsh_serial_first_s4;
    uint32_t ppsh_serial_first_t9;
    uint32_t ppsh_serial_stack0;
    uint32_t ppsh_serial_stack1;
    uint32_t ppsh_serial_stack2;
    uint32_t ppsh_serial_stack3;
    uint16_t ppsh_serial_line_len;
    uint16_t ppsh_serial_msg_count;
    uint16_t ppsh_helper_dump_count;
    uint16_t ppsh_flag_dump_count;
    uint16_t ppsh_obj_write_count;
    uint32_t ppsh_last_helper_pc;
    uint32_t ppsh_last_flag_pc;
    uint32_t ppsh_exact_pc_mask;
    bool     ppsh_timeout_path_dumped;
    uint32_t ppsh_last_callsite_fmt;
    uint32_t ppsh_last_serial_dump_sp;
    char     ppsh_serial_line[160];

    /* Serial exception and post-PPSH fault diagnostics */
    uint16_t serial_exception_diag_count;
    uint16_t serial_exception_corr_count;
    uint16_t systempatch_context_diag_count;
    uint16_t systempatch_thread_ctx_diag_count;
    uint16_t systempatch_thread_write_count;
    uint16_t callback_slot_diag_count;
    uint16_t callback_slot_write_count;
    uint16_t callback_obj_diag_count;
    uint16_t callback_obj_write_count;
    uint16_t hot_user_l2_write_count;
    uint16_t hot_l2_alloc_probe_count;
    uint16_t hot_fault_probe_count;
    uint16_t section0_focus_diag_count;
    uint16_t section0_hot_slot_diag_count;
    uint16_t section0_source_probe_count;
    uint16_t section3_focus_diag_count;
    uint16_t section3_raw_diag_count;
    uint16_t section3_page_diag_count;
    uint16_t section3_install_probe_count;
    uint16_t section3_owner_write_count;
    uint16_t section3_desc_write_count;
    uint16_t section3_desc_read_count;
    uint16_t section3_callback_probe_count;
    uint16_t section3_head_probe_count;
    uint16_t section3_head_write_count;
    uint16_t section3_order_probe_count;
    uint16_t section3_gate_probe_count;
    uint16_t section3_caller_probe_count;
    uint16_t section3_source_probe_count;
    uint16_t section3_obj_probe_count;
    uint16_t section3_worker_probe_count;
    uint16_t section3_queue_probe_count;
    uint16_t section3_focusobj_write_count;
    uint16_t section3_pool_write_count;
    uint16_t section3_ctor_probe_count;
    uint16_t section3_type4_probe_count;
    uint16_t type4_gate_probe_count;
    uint16_t type4_state_probe_count;
    uint16_t section3_retobj_write_count;
    uint16_t section0_focus_slot_read_count;
    uint16_t section0_focus_slot_write_count;
    bool     section3_page_watch_armed;
    bool     section3_retobj_watch_armed;
    uint16_t type4_order_next;
    uint16_t type4_order_ctor_seq;
    uint16_t type4_order_link_seq;
    uint16_t type4_order_state_seq;
    uint16_t type4_order_payload_seq;
    uint16_t type4_order_enqueue_seq;
    uint16_t type4_order_cleanup_seq;
    uint16_t type4_ready_write_count;
    uint32_t type4_wrap_watch_va;
    uint32_t type4_payload_watch_va;
    uint32_t type4_handle_watch_va;
    uint32_t section3_owner_pc_mask;
    uint32_t section3_callback_pc_mask;
    uint32_t section3_retobj_watch_va;
    bool     systempatch_seen;
    bool     systempatch_first_exception_logged;
    bool     systempatch_process_logged;
    bool     systempatch_thread_ctx_valid;
    bool     callback_slot_watch_armed;
    bool     hot_fault_code_dumped;
    uint32_t callback_slot_watch_pa;
    bool     exc_slot1_dumped;
    bool     cb_rearm_logged;
    bool     hot_l2w_probe_dumped;
    bool     free_helper_entry_dumped;
    uint8_t  free_helper_call_count;
    uint8_t  loop_prejal_count;
    bool     teardown_section_entry_dumped;
    uint8_t  verify_helper_entry_count;
    uint8_t  verify_helper_ret_count;
    uint8_t  walker_entry_count;
    uint8_t  alloc_helper_count;
    uint16_t hot_l2p_write_count;
    uint8_t  l1_slot_write_count;
    uint8_t  teardown_trace_count;
    uint8_t  teardown_caller_count;
    uint8_t  sec0_track_count;
    bool     teardown_hot_logged;
    bool     teardown_tail_dumped;
    bool     publish_scan_call_dumped;
    bool     publish_scan_cont_dumped;
    bool     state_bit_setter_dumped;
    bool     sec0_first_bad_transition_logged;
    uint8_t  cb_slot_first_seen;
    uint8_t  cb_slot_zero_seen;
    uint32_t cb_slot_first_pc[4];
    uint32_t cb_slot_first_ra[4];
    uint32_t cb_slot_first_val[4];
    uint32_t cb_slot_zero_pc[4];
    uint32_t cb_slot_zero_ra[4];
    uint64_t cb_slot_first_instr[4];
    uint16_t sec0_hot_map_diag_count;
    uint16_t sec0_derived_l1_write_diag_count;
    uint16_t sec0_derived_pte_write_diag_count;
    uint32_t systempatch_last_cur_thrd;
    uint32_t systempatch_last_obj00;
    uint32_t systempatch_last_obj04;
    uint32_t systempatch_last_obj0c;
    uint32_t systempatch_last_obj14;
    uint32_t systempatch_last_obj24;
    uint32_t systempatch_last_obj3c;
    uint32_t systempatch_last_ctx08;
    uint32_t systempatch_last_ctx0c;
    uint32_t systempatch_last_src_slot;
    uint32_t systempatch_last_sec0;
    uint32_t systempatch_last_sec1;
    uint32_t systempatch_last_sec3;
    uint32_t sec0_track_tables[8];
    uint32_t sec0_track_l2[8];
    uint32_t sec0_track_lo0[8];
    uint32_t sec0_track_lo1[8];
    uint32_t sec0_first_bad_old_table;
    uint32_t sec0_first_bad_new_table;
    uint32_t sec0_first_bad_old_l2;
    uint32_t sec0_first_bad_new_l2;
    uint32_t sec0_first_bad_old_lo0;
    uint32_t sec0_first_bad_old_lo1;
    uint32_t sec0_first_bad_new_lo0;
    uint32_t sec0_first_bad_new_lo1;
    uint32_t sec0_first_bad_pc;
    uint32_t sec0_first_bad_ra;
    uint32_t sec0_first_bad_asid;
    bool     sec0_first_bad_source_recorded;
    bool     sec0_first_bad_l1_write_recorded;
    bool     sec0_first_bad_pte_write_recorded;
    uint8_t  sec0_first_bad_publish_source_mask;
    uint8_t  sec0_first_bad_write_source_mask;
    uint8_t  sec0_first_bad_l1_source;
    uint8_t  sec0_first_bad_pte_source;
    uint32_t sec0_first_bad_source_obj;
    uint32_t sec0_first_bad_source_obj0c;
    uint32_t sec0_first_bad_source_obj14;
    uint32_t sec0_first_bad_source_obj24;
    uint32_t sec0_first_bad_source_ctx08;
    uint32_t sec0_first_bad_source_ctx0c;
    uint32_t sec0_first_bad_source_src_off;
    uint32_t sec0_first_bad_source_src_idx;
    uint32_t sec0_first_bad_source_src_slot;
    uint32_t sec0_first_bad_l1_pc;
    uint32_t sec0_first_bad_l1_ra;
    uint32_t sec0_first_bad_l1_asid;
    uint32_t sec0_first_bad_l1_new_l2;
    uint32_t sec0_first_bad_pte_pc;
    uint32_t sec0_first_bad_pte_ra;
    uint32_t sec0_first_bad_pte_asid;
    uint32_t sec0_first_bad_pte_new_lo0;
    uint32_t sec0_first_bad_pte_new_lo1;
    uint64_t sec0_first_bad_ninstrs;
    uint32_t sec0_l1_slot_ring_total;
    uint8_t  sec0_l1_slot_ring_head;
    uint8_t  sec0_l1_slot_ring_filled;
    wince_sec0_l1_slot_entry_t sec0_l1_slot_ring[WINCE_SEC0_L1_SLOT_RING_SIZE];
    uint8_t  sec0_tlb_ring_filled;
    wince_sec0_tlb_entry_t sec0_tlb_ring[WINCE_SEC0_TLB_RING_SIZE];
    wince_serial_exception_record_t serial_exc_pending;
    wince_serial_exception_record_t serial_exc_last;
    wince_hot_page_verdict_t hot_page_01fe6550;
    wince_hot_page_verdict_t hot_page_01f8f8f8;
    wince_hot_page_verdict_t hot_page_01f94b50;
    wince_hot_page_verdict_t hot_page_0204fe48;
    wince_hot_page_verdict_t hot_page_02041fa8;

} wince_boot_state_t;
