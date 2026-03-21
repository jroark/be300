/*
 * null_call.c — Null function pointer call recovery
 *
 * Handles CPU exceptions triggered by JALR to address 0 (null function
 * pointer calls). Both Linux and WinCE boot paths are supported with
 * different recovery strategies.
 *
 * Extracted from machine.c.
 */

#include "null_call.h"
#include "machine.h"
#include "wince_diag.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#define WINCE_NULL_RECOVER_CAP 64u

static inline bool is_null_call_class_intno(uint32_t intno)
{
    return intno == 20u || intno == 26u || intno == 27u;
}

void clear_pending_exception_state(machine_t *m)
{
    m->pending_epc          = 0;
    m->pending_excode       = 0;
    m->pending_cause        = 0;
    m->epc_was_written      = false;
    m->pending_cause_served = false;
    m->pending_epc_served   = false;
}

bool handle_linux_null_call_interrupt(machine_t *m, uc_engine *uc,
                                             uint32_t intno, uint64_t pc)
{
    (void)m;
    if ((uint32_t)pc != 0u || !is_null_call_class_intno(intno))
        return false;

    uint64_t ra = 0, t9 = 0, gp = 0, sp = 0;
    uint64_t a0 = 0, a1 = 0, a2 = 0, v0 = 0;
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
    uc_reg_read(uc, UC_MIPS_REG_T9, &t9);
    uc_reg_read(uc, UC_MIPS_REG_GP, &gp);
    uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
    uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
    uc_reg_read(uc, UC_MIPS_REG_A1, &a1);
    uc_reg_read(uc, UC_MIPS_REG_A2, &a2);
    uc_reg_read(uc, UC_MIPS_REG_V0, &v0);

    static uint32_t null_ptr_log = 0;
    if (null_ptr_log < 8u) {
        fprintf(stderr,
                "[NULL_CALL] PC=0x%08" PRIX64 " ra=0x%08" PRIX64
                " t9=0x%08" PRIX64 " gp=0x%08" PRIX64 " sp=0x%08" PRIX64
                " a0=0x%08" PRIX64 " a1=0x%08" PRIX64
                " a2=0x%08" PRIX64 " v0=0x%08" PRIX64 "\n",
                (uint64_t)(uint32_t)pc,
                (uint64_t)(uint32_t)ra, (uint64_t)(uint32_t)t9,
                (uint64_t)(uint32_t)gp, (uint64_t)(uint32_t)sp,
                (uint64_t)(uint32_t)a0, (uint64_t)(uint32_t)a1,
                (uint64_t)(uint32_t)a2, (uint64_t)(uint32_t)v0);
        null_ptr_log++;
    }

    uint32_t ra32 = (uint32_t)ra;
    if (ra32 == 0x80042F8Cu) {
        uint32_t saved_ra_lo = 0;
        uint64_t sp_val = 0;
        uc_reg_read(uc, UC_MIPS_REG_SP, &sp_val);
        uint32_t sp32   = (uint32_t)sp_val;
        uint64_t va_sp  = mips_sext(sp32);
        uc_mem_read(uc, va_sp + 16u, &saved_ra_lo, 4u);
        uint64_t new_sp = mips_sext(sp32 + 24u);
        uint64_t new_ra = mips_sext(saved_ra_lo);
        uint64_t neg2   = (uint64_t)(uint32_t)-2;
        fprintf(stderr,
                "[NULL_CALL_RECOVER] parse_one jalr t3 NULL;"
                " va_sp=0x%016" PRIX64 " saved_ra=0x%08" PRIX32
                " new_sp=0x%08" PRIX64 "\n",
                va_sp, saved_ra_lo, (uint64_t)(uint32_t)new_sp);
        if (saved_ra_lo >= 0x80000000u) {
            uc_reg_write(uc, UC_MIPS_REG_SP, &new_sp);
            uc_reg_write(uc, UC_MIPS_REG_RA, &new_ra);
            uc_reg_write(uc, UC_MIPS_REG_V0, &neg2);
            uc_reg_write(uc, UC_MIPS_REG_PC, &new_ra);
            return true;
        }
    }

    if (ra32 >= 0x80000000u) {
        uint64_t new_pc = mips_sext(ra32);
        uint64_t zero   = 0;
        uc_reg_write(uc, UC_MIPS_REG_PC, &new_pc);
        uc_reg_write(uc, UC_MIPS_REG_V0, &zero);
        static uint32_t gen_recover_log = 0;
        if (gen_recover_log < 16u) {
            fprintf(stderr,
                    "[NULL_CALL_RECOVER_GENERAL] PC=0 ra=0x%08" PRIX32
                    " -> redirecting to ra (v0=0)\n", ra32);
            gen_recover_log++;
        }
        return true;
    }

    return false;
}

bool handle_wince_null_call_interrupt(machine_t *m, uc_engine *uc,
                                             uint32_t intno, uint64_t pc,
                                             uint64_t status)
{
    if ((uint32_t)pc != 0u || !is_null_call_class_intno(intno))
        return false;

    uint64_t ra = 0;
    uint64_t v0 = 0;
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
    uc_reg_read(uc, UC_MIPS_REG_V0, &v0);

    /*
     * SPL handoff site:
     *   0xA0F024F8: jalr a1   ; returns stage entry in v0
     *   0xA0F02500: move t0,v0
     *   0xA0F02504: jr t0
     *
     * If v0 is unexpectedly 0, jr t0 jumps to 0. Recover by
     * redirecting to the stage entry block at 0xA0F0250C.
     */
    if ((uint32_t)ra == 0xA0F02500u) {
        uint32_t stage32 = 0xA0F0250Cu;
        uint32_t saved_stage32 = 0;
        uc_err saved_stage_err = uc_mem_read(uc, mips_sext(0xA00024FCu),
                                             &saved_stage32, sizeof(saved_stage32));
        if (saved_stage_err != UC_ERR_OK)
            saved_stage_err = uc_mem_read(uc, UINT64_C(0x000024FC),
                                          &saved_stage32, sizeof(saved_stage32));
        if (saved_stage_err == UC_ERR_OK && is_kseg_va32(saved_stage32))
            stage32 = saved_stage32;
        uint64_t stage = mips_sext(stage32);
        uc_reg_write(uc, UC_MIPS_REG_V0, &stage);
        uc_reg_write(uc, UC_MIPS_REG_T0, &stage);
        uc_reg_write(uc, UC_MIPS_REG_PC, &stage);
        m->wince_null_last_ra = 0;
        m->wince_null_last_intno = 0;
        m->wince_null_consecutive = 0;

        static uint32_t wince_spl_recover_log = 0;
        if (wince_spl_recover_log < 32u) {
            fprintf(stderr,
                    "[WINCE_NULL_RECOVER] intno=%u pc=0x%08" PRIX64
                    " ra=0x%08" PRIX64 " v0=0x%08" PRIX64
                    " saved_stage=0x%08" PRIX32 " saved_stage_err=%d"
                    " -> stage=0x%08" PRIX32 "\n",
                    intno,
                    (uint64_t)(uint32_t)pc,
                    (uint64_t)(uint32_t)ra,
                    (uint64_t)(uint32_t)v0,
                    saved_stage32,
                    (int)saved_stage_err,
                    stage32);
            wince_spl_recover_log++;
        }
        return true;
    }

    uint32_t ra32 = (uint32_t)ra;
    if (ra32 >= 0x80000000u) {
        if (m->wince_null_last_ra == ra32 &&
            m->wince_null_last_intno == intno) {
            if (m->wince_null_consecutive < UINT32_MAX)
                m->wince_null_consecutive++;
        } else {
            m->wince_null_last_ra = ra32;
            m->wince_null_last_intno = intno;
            m->wince_null_consecutive = 1u;
        }

        if (m->wince_null_consecutive == 2u) {
            static uint32_t wince_null_burst_log = 0;
            if (wince_null_burst_log < 64u) {
                fprintf(stderr,
                        "[WINCE_NULL_BURST] intno=%u ra=0x%08X"
                        " pending_excode=%u pending_epc=0x%08" PRIX64
                        " pending_cause=0x%08X STATUS=0x%08" PRIX64 "\n",
                        intno, ra32, m->pending_excode,
                        (uint64_t)(uint32_t)m->pending_epc,
                        m->pending_cause,
                        (uint64_t)(uint32_t)status);
                wince_null_burst_log++;
            }
        }

        if (m->wince_null_consecutive > WINCE_NULL_RECOVER_CAP) {
            fprintf(stderr,
                    "[WINCE_NULL_BAILOUT] intno=%u ra=0x%08X count=%u cap=%u"
                    " STATUS=0x%08" PRIX64 "\n",
                    intno, ra32, m->wince_null_consecutive,
                    WINCE_NULL_RECOVER_CAP, (uint64_t)(uint32_t)status);
            log_wince_pa_watch_nonzero(m, "NULL_BAILOUT");
            log_wince_pa_watch_summary(m, "NULL_BAILOUT");
            log_wince_region_track_summary(m, "NULL_BAILOUT");
            log_wince_div_hist_summary(m, "NULL_BAILOUT", 32u);
            log_wince_div_call_trace_summary(m, "NULL_BAILOUT");
            log_wince_div_stack_watch_summary(m, uc, "NULL_BAILOUT");
            log_wince_null_mmio_tail(m, 24u);
            log_wince_producer_summary(m, "NULL_BAILOUT");
            log_wince_callback_arm_summary(m, uc, "NULL_BAILOUT");
            log_wince_future_frame_summary(m, uc, "NULL_BAILOUT");
            log_wince_resume_global_summary(m, uc, "NULL_BAILOUT");
            log_wince_delay_call_summary(m, "NULL_BAILOUT");
            log_wince_midbody_9c_state(m, uc, "NULL_BAILOUT", m->last_exec_pc);
            machine_stop(m);
            uc_emu_stop(uc);
            return true;
        }

        uint64_t next_pc = mips_sext(ra32);
        uint64_t zero = 0;
        uint64_t new_status = status & ~(uint64_t)0x2u; /* clear EXL only */
        clear_pending_exception_state(m);
        uc_reg_write(uc, UC_MIPS_REG_CP0_STATUS, &new_status);
        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
        uc_reg_write(uc, UC_MIPS_REG_V0, &zero);

        static uint32_t wince_general_recover_log = 0;
        if (wince_general_recover_log < 128u) {
            fprintf(stderr,
                    "[WINCE_NULL_RECOVER] intno=%u ra=0x%08X count=%u"
                    " STATUS=0x%08" PRIX64 " -> PC=0x%08X\n",
                    intno, ra32, m->wince_null_consecutive,
                    (uint64_t)(uint32_t)status, ra32);
            wince_general_recover_log++;
        }

        /* One-shot code dump around the null-call site for NK addresses.
         * JALR is at RA-8, delay slot at RA-4, return at RA. */
        static uint32_t null_call_dumped_ras[8] = {0};
        static unsigned null_call_dump_count = 0;
        bool already_dumped = false;
        for (unsigned dd = 0; dd < null_call_dump_count; dd++)
            if (null_call_dumped_ras[dd] == ra32) { already_dumped = true; break; }
        if (!already_dumped && null_call_dump_count < 8 &&
            !(ra32 >= UINT32_C(0x80F00000) && ra32 < UINT32_C(0x80F10000))) {
            null_call_dumped_ras[null_call_dump_count++] = ra32;
            uint32_t dump_base = (ra32 - 64u) & ~UINT32_C(0x3);
            fprintf(stderr, "[WINCE_NULL_CODE] dumping 128 bytes around RA=0x%08X:\n", ra32);
            for (uint32_t off = 0; off < 128; off += 64) {
                uint8_t chunk[64];
                uint64_t pa = (uint64_t)((dump_base + off) & 0x1FFFFFFFu);
                uc_err merr = uc_mem_read(uc, pa, chunk, 64);
                if (merr != UC_ERR_OK) {
                    fprintf(stderr, "[WINCE_NULL_CODE] chunk at 0x%08X: READ FAILED\n",
                            dump_base + off);
                    continue;
                }
                fprintf(stderr, "[WINCE_NULL_CODE] base=0x%08X off=0x%02X hex=",
                        dump_base, off);
                for (int b = 0; b < 64; b++)
                    fprintf(stderr, "%02X", chunk[b]);
                fprintf(stderr, "\n");
            }
            /* Also dump GPRs for context */
            uint64_t t9v = 0, a0v = 0, a1v = 0, s0v = 0;
            uc_reg_read(uc, UC_MIPS_REG_T9, &t9v);
            uc_reg_read(uc, UC_MIPS_REG_A0, &a0v);
            uc_reg_read(uc, UC_MIPS_REG_A1, &a1v);
            uc_reg_read(uc, UC_MIPS_REG_S0, &s0v);
            fprintf(stderr,
                    "[WINCE_NULL_CODE] t9=0x%08X a0=0x%08X a1=0x%08X s0=0x%08X\n",
                    (uint32_t)t9v, (uint32_t)a0v, (uint32_t)a1v, (uint32_t)s0v);

            /* Dump the JAL target function (RA-8 is the JAL, extract target) */
            uint32_t jal_insn = 0;
            uint64_t jal_pa = (uint64_t)((ra32 - 8u) & 0x1FFFFFFFu);
            if (uc_mem_read(uc, jal_pa, &jal_insn, 4) == UC_ERR_OK) {
                uint32_t jal_op = jal_insn >> 26;
                if (jal_op == 3u) { /* JAL */
                    uint32_t jal_target = ((jal_insn & 0x03FFFFFFu) << 2) |
                                          (ra32 & 0xF0000000u);
                    fprintf(stderr, "[WINCE_NULL_CODE] JAL target=0x%08X, dumping:\n",
                            jal_target);
                    uint32_t tgt_pa = jal_target & 0x1FFFFFFFu;
                    for (uint32_t off = 0; off < 512; off += 64) {
                        uint8_t tc[64];
                        if (uc_mem_read(uc, (uint64_t)(tgt_pa + off), tc, 64) == UC_ERR_OK) {
                            fprintf(stderr, "[WINCE_NULL_CODE] tgt=0x%08X off=0x%02X hex=",
                                    jal_target, off);
                            for (int b = 0; b < 64; b++)
                                fprintf(stderr, "%02X", tc[b]);
                            fprintf(stderr, "\n");
                        }
                    }
                }
            }

            /* Also dump nested function at 0x80079CE4 (integrity check) */
            {
                uint32_t nested_pa = UINT32_C(0x00079CE4);
                fprintf(stderr, "[WINCE_NULL_CODE] nested func 0x80079CE4:\n");
                for (uint32_t off = 0; off < 128; off += 64) {
                    uint8_t nc[64];
                    if (uc_mem_read(uc, (uint64_t)(nested_pa + off), nc, 64) == UC_ERR_OK) {
                        fprintf(stderr, "[WINCE_NULL_CODE] 0x%08X off=0x%02X hex=",
                                0x80079CE4u + off, off);
                        for (int b = 0; b < 64; b++)
                            fprintf(stderr, "%02X", nc[b]);
                        fprintf(stderr, "\n");
                    }
                }
                /* Also dump PA 0x6020 area and VA 0x800748F0 area */
                uint32_t ctx_area[4] = {0};
                uint32_t const_area[4] = {0};
                uc_mem_read(uc, UINT64_C(0x00006020), ctx_area, 16);
                uc_mem_read(uc, UINT64_C(0x000748F0), const_area, 16);
                fprintf(stderr, "[WINCE_NULL_CODE] PA_6020: %08X %08X %08X %08X\n",
                        ctx_area[0], ctx_area[1], ctx_area[2], ctx_area[3]);
                fprintf(stderr, "[WINCE_NULL_CODE] PA_748F0: %08X %08X %08X %08X\n",
                        const_area[0], const_area[1], const_area[2], const_area[3]);
            }
        }
        return true;
    }

    fprintf(stderr, "[WINCE_INTR] NULL call detected (ra=0x%08" PRIX64
            ") last_exec_pc=0x%08" PRIX64 " — stopping\n",
            (uint64_t)(uint32_t)ra,
            (uint64_t)(uint32_t)m->last_exec_pc);
    log_wince_pa_watch_nonzero(m, "NULL_POSTMORTEM");
    log_wince_pa_watch_summary(m, "NULL_POSTMORTEM");
    log_wince_region_track_summary(m, "NULL_POSTMORTEM");
    log_wince_ctrl_hist_summary(m, "NULL_POSTMORTEM", 128u);
    log_wince_div_hist_summary(m, "NULL_POSTMORTEM", 64u);
    log_wince_div_call_trace_summary(m, "NULL_POSTMORTEM");
    log_wince_div_stack_watch_summary(m, uc, "NULL_POSTMORTEM");
    log_wince_null_mmio_tail(m, 24u);
    log_wince_producer_summary(m, "NULL_POSTMORTEM");
    log_wince_callback_arm_summary(m, uc, "NULL_POSTMORTEM");
    log_wince_ctx_saved_summary(m, uc, "NULL_POSTMORTEM");
    log_wince_future_frame_summary(m, uc, "NULL_POSTMORTEM");
    log_wince_resume_global_summary(m, uc, "NULL_POSTMORTEM");
    log_wince_obj_bootstrap_summary(m, "NULL_POSTMORTEM");
    log_wince_delay_call_summary(m, "NULL_POSTMORTEM");

    /* Dump code around last known execution PC for post-mortem analysis */
    {
        uint32_t lpc = (uint32_t)m->last_exec_pc;
        uint32_t dump_base = (lpc >= 32u) ? (lpc - 32u) & ~UINT32_C(0x3) : 0u;
        uint32_t dump_pa = dump_base & UINT32_C(0x1FFFFFFF);
        fprintf(stderr, "[WINCE_NULL_POSTMORTEM] last_exec_pc=0x%08X dump:\n", lpc);
        for (uint32_t off = 0; off < 128; off += 64) {
            uint8_t chunk[64];
            if (uc_mem_read(uc, (uint64_t)(dump_pa + off), chunk, 64) == UC_ERR_OK) {
                fprintf(stderr, "[WINCE_NULL_POSTMORTEM] base=0x%08X off=0x%02X hex=",
                        dump_base, off);
                for (int b = 0; b < 64; b++)
                    fprintf(stderr, "%02X", chunk[b]);
                fprintf(stderr, "\n");
            }
        }
        /* Dump exception vectors and context block */
        struct { uint32_t pa; const char *name; } mem_regions[] = {
            { 0x00000000, "exception_vectors" },
            { 0x00000100, "exception_vectors+0x100" },
            { 0x00000180, "exception_vectors+0x180" },
            { 0xA0002000, "ctx_tlb_seed_0xA0002000" },
            { 0x00002200, "context_block" },
            { 0x800748D0, "ctx_boot_magic_0x800748D0" },
            { 0x800794C0, "ctx_caller_0x800794C8" },
            { 0x80079580, "ctx_caller_0x80079580" },
            { 0x800795C0, "ctx_caller_0x800795C0" },
            { 0x80079600, "ctx_caller_0x80079600" },
            { 0x80079640, "ctx_path_0x80079640" },
            { 0x80079680, "ctx_path_0x80079680" },
            { 0x80079700, "ctx_path_0x80079700" },
            { 0x80079780, "ctx_path_0x80079780" },
            { 0x80079840, "ctx_path_0x80079840" },
            { 0x80079880, "ctx_path_0x80079880" },
            { 0x80096780, "ctx_caller_0x80096790" },
            { 0x80096880, "ctx_caller_0x80096894" },
            { 0x80077DC0, "ctx_caller_0x80077DC0" },
            { 0x80077CA0, "ctx_caller_0x80077CC0" },
            { 0x80077D00, "ctx_caller_0x80077D20" },
            { 0x80077E28, "ctx_caller_0x80077E28" },
            { 0x800781C0, "ctx_caller_0x800781C0" },
            { 0x80078200, "ctx_caller_0x80078200" },
            { 0x80078BA0, "ctx_caller_0x80078BC0" },
            { 0x80077FC0, "ctx_caller_0x80077FE4" },
            { 0x80079A80, "ctx_caller_0x80079AC4" },
            { 0x80079DD0, "ctx_caller_0x80079DF8" },
            { 0x8007AF80, "ctx_caller_0x8007AFA8" },
            { 0x800A7D40, "ctx_caller_0x800A7D64" },
            { 0x800323B0, "ctx_caller_0x800323B8" },
            { 0x800324F0, "ctx_caller_0x800324F8" },
            { 0x80032510, "ctx_caller_0x80032518" },
            { 0x80032530, "ctx_path_0x80032530" },
            { 0x80032570, "ctx_path_0x80032570" },
            { 0x80032600, "ctx_path_0x80032600" },
            { 0x80032680, "ctx_path_0x80032680" },
            { 0x80032700, "ctx_path_0x80032700" },
            { 0x80032740, "ctx_path_0x80032740" },
            { 0x80032780, "ctx_path_0x80032780" },
            { 0x806794D0, "ctx_cbslot_0x806794D0" },
            { 0x80660000, "ctx_objptr_0x80660000" },
            { 0x8066BFC0, "ctx_obj_0x8066BFC0" },
            { 0x8067BFC0, "ctx_obj_0x8067BFC0" },
            { 0x80078260, "ctx_obj_cb0_0x80078260" },
            { 0x80078300, "ctx_obj_cb1_0x80078308" },
            { 0x80078340, "ctx_obj_cb2_0x80078354" },
            { 0x80078440, "ctx_obj_cb4_0x8007846C" },
            { 0x800784D0, "ctx_obj_cb5_0x800784F4" },
            { 0x8007A650, "ctx_callee_0x8007A65C" },
            { 0x8007A100, "ctx_callee_0x8007A110" },
            { 0x8007A120, "ctx_callee_0x8007A128" },
            { 0x8007AAA0, "ctx_callee_0x8007AAAC" },
            { 0x800A5E60, "ctx_callee_0x800A5E70" },
            { 0x800A5FE0, "ctx_callee_0x800A5FEC" },
            { 0x800A7640, "ctx_callee_0x800A7650" },
            { 0x800A7DC0, "ctx_callee_0x800A7DCC" },
            { 0xA0051680, "ctx_table_0xA0051680" },
            { 0xA0060000, "ctx_all_nand_base_0xA0060000" },
            { 0xA0061000, "ctx_all_nand_blk_0xA0061000" },
            { 0xA0061680, "ctx_all_nand_tbl_0xA0061680" },
            { 0xA0061900, "ctx_all_nand_ptr_0xA0061900" },
            { 0x007E9000, "ctx_src_0x007E9000" },
            { 0xA001D000, "ctx_bootparam_0xA001D000" },
            { 0xA002D000, "ctx_bootparam_0xA002D000" },
            { 0xA0006000, "ctx_bootctx_0xA0006000" },
            { 0xA0002600, "ctx_bootflags_0xA0002600" },
            { 0xA0003800, "ctx_stack_0xA0003800" },
        };
        size_t region_count = sizeof(mem_regions) / sizeof(mem_regions[0]);
        for (size_t r = 0; r < region_count; r++) {
            size_t dump_len = 64u;
            if (mem_regions[r].pa == 0x8007A650u ||
                mem_regions[r].pa == 0x8007AAA0u ||
                mem_regions[r].pa == 0x800A5E60u ||
                mem_regions[r].pa == 0x800A5FE0u ||
                mem_regions[r].pa == 0x800A7640u ||
                mem_regions[r].pa == 0x800A7DC0u ||
                mem_regions[r].pa == 0xA0002000u ||
                mem_regions[r].pa == 0x80078BA0u ||
                mem_regions[r].pa == 0x80077FC0u ||
                mem_regions[r].pa == 0x80077DC0u ||
                mem_regions[r].pa == 0x80077CA0u ||
                mem_regions[r].pa == 0x80077D00u ||
                mem_regions[r].pa == 0x80077E28u ||
                mem_regions[r].pa == 0x800781C0u ||
                mem_regions[r].pa == 0x80078200u ||
                mem_regions[r].pa == 0x800794C0u ||
                mem_regions[r].pa == 0x80079A80u ||
                mem_regions[r].pa == 0x80079DD0u ||
                mem_regions[r].pa == 0x8007AF80u ||
                mem_regions[r].pa == 0x800A7D40u ||
                mem_regions[r].pa == 0x800323B0u ||
                mem_regions[r].pa == 0x800324F0u ||
                mem_regions[r].pa == 0x80032510u ||
                mem_regions[r].pa == 0x80032530u ||
                mem_regions[r].pa == 0x80032570u ||
                mem_regions[r].pa == 0x80032600u ||
                mem_regions[r].pa == 0x80032680u ||
                mem_regions[r].pa == 0x80032700u ||
                mem_regions[r].pa == 0x80032740u ||
                mem_regions[r].pa == 0x80032780u ||
                mem_regions[r].pa == 0x80096780u ||
                mem_regions[r].pa == 0x80096880u ||
                mem_regions[r].pa == 0x80078260u ||
                mem_regions[r].pa == 0x80078300u ||
                mem_regions[r].pa == 0x80078340u ||
                mem_regions[r].pa == 0x80078440u ||
                mem_regions[r].pa == 0x800784D0u ||
                mem_regions[r].pa == 0x8007A100u ||
                mem_regions[r].pa == 0x8007A120u) {
                dump_len = 512u;
            } else if (mem_regions[r].pa >= 0x80000000u) {
                dump_len = 128u;
            }
            uint8_t mc[512];
            if (uc_mem_read(uc, (uint64_t)mem_regions[r].pa, mc, dump_len) == UC_ERR_OK) {
                fprintf(stderr, "[WINCE_NULL_POSTMORTEM] %s PA=0x%08X hex=",
                        mem_regions[r].name, mem_regions[r].pa);
                for (size_t b = 0; b < dump_len; b++)
                    fprintf(stderr, "%02X", mc[b]);
                fprintf(stderr, "\n");
            }
        }

        /* Dump all GPRs */
        static const int gpr_ids[32] = {
            UC_MIPS_REG_ZERO, UC_MIPS_REG_AT, UC_MIPS_REG_V0, UC_MIPS_REG_V1,
            UC_MIPS_REG_A0,   UC_MIPS_REG_A1, UC_MIPS_REG_A2, UC_MIPS_REG_A3,
            UC_MIPS_REG_T0,   UC_MIPS_REG_T1, UC_MIPS_REG_T2, UC_MIPS_REG_T3,
            UC_MIPS_REG_T4,   UC_MIPS_REG_T5, UC_MIPS_REG_T6, UC_MIPS_REG_T7,
            UC_MIPS_REG_S0,   UC_MIPS_REG_S1, UC_MIPS_REG_S2, UC_MIPS_REG_S3,
            UC_MIPS_REG_S4,   UC_MIPS_REG_S5, UC_MIPS_REG_S6, UC_MIPS_REG_S7,
            UC_MIPS_REG_T8,   UC_MIPS_REG_T9, UC_MIPS_REG_K0, UC_MIPS_REG_K1,
            UC_MIPS_REG_GP,   UC_MIPS_REG_SP, UC_MIPS_REG_FP, UC_MIPS_REG_RA
        };
        static const char *gpr_names[32] = {
            "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
            "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
            "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
            "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra"
        };
        for (int i = 0; i < 32; i++) {
            uint64_t gv = 0;
            uc_reg_read(uc, gpr_ids[i], &gv);
            fprintf(stderr, "[WINCE_NULL_POSTMORTEM] $%-4s = 0x%08X\n",
                    gpr_names[i], (uint32_t)gv);
        }
        uint64_t cp0_status_v = 0, cp0_cause_v = 0;
        uc_reg_read(uc, UC_MIPS_REG_CP0_STATUS, &cp0_status_v);
        fprintf(stderr, "[WINCE_NULL_POSTMORTEM] STATUS=0x%08X CAUSE=0x%08X EPC=0x%08X\n",
                (uint32_t)cp0_status_v, m->pending_cause, (uint32_t)m->pending_epc);
    }

    machine_stop(m);
    uc_emu_stop(uc);
    return true;
}

bool handle_null_call_interrupt(machine_t *m, uc_engine *uc,
                                       uint32_t intno, uint64_t pc,
                                       uint64_t status)
{
    if ((uint32_t)pc != 0u || !is_null_call_class_intno(intno))
        return false;
    if (is_wince_boot_machine(m))
        return handle_wince_null_call_interrupt(m, uc, intno, pc, status);
    return handle_linux_null_call_interrupt(m, uc, intno, pc);
}

bool handle_wince_interrupt_passthrough(machine_t *m, uint32_t intno,
                                               uint64_t pc, uint64_t status)
{
    if (!is_wince_boot_machine(m))
        return false;

    m->wince_null_last_ra = 0;
    m->wince_null_last_intno = 0;
    m->wince_null_consecutive = 0;

    static uint32_t wince_intr_log = 0;
    if (wince_intr_log < 32u) {
        fprintf(stderr, "[WINCE_INTR] intno=%u PC=0x%08" PRIX64
                " STATUS=0x%08" PRIX64 "\n",
                intno, (uint64_t)(uint32_t)pc, (uint64_t)(uint32_t)status);
        wince_intr_log++;
    }
    return true;
}
