
/*  AUTOMATICALLY GENERATED! Do not edit.  */

#include <assert.h>
#include "debugger.h"
#define DYNTRANS_MAX_VPH_TLB_ENTRIES MIPS_MAX_VPH_TLB_ENTRIES
#define DYNTRANS_ARCH mips
#define DYNTRANS_MIPS
#ifndef DYNTRANS_32
#define DYNTRANS_L2N MIPS_L2N
#define DYNTRANS_L3N MIPS_L3N
#if !defined(MIPS_L2N) || !defined(MIPS_L3N)
#error arch_L2N, and arch_L3N must be defined for this arch!
#endif
#define DYNTRANS_L2_64_TABLE mips_l2_64_table
#define DYNTRANS_L3_64_TABLE mips_l3_64_table
#endif
#ifndef DYNTRANS_PAGESIZE
#define DYNTRANS_PAGESIZE 4096
#endif
#define DYNTRANS_IC mips_instr_call
#define DYNTRANS_IC_ENTRIES_PER_PAGE MIPS_IC_ENTRIES_PER_PAGE
#define DYNTRANS_INSTR_ALIGNMENT_SHIFT MIPS_INSTR_ALIGNMENT_SHIFT
#define DYNTRANS_TC_PHYSPAGE mips_tc_physpage
#define DYNTRANS_INVALIDATE_TLB_ENTRY mips_invalidate_tlb_entry
#define DYNTRANS_ADDR_TO_PAGENR MIPS_ADDR_TO_PAGENR
#define DYNTRANS_PC_TO_IC_ENTRY MIPS_PC_TO_IC_ENTRY
#define DYNTRANS_TC_ALLOCATE mips_tc_allocate_default_page
#define DYNTRANS_TC_PHYSPAGE mips_tc_physpage
#define DYNTRANS_PC_TO_POINTERS mips_pc_to_pointers
#define DYNTRANS_PC_TO_POINTERS_GENERIC mips_pc_to_pointers_generic
#define COMBINE_INSTRUCTIONS mips_combine_instructions
#define DISASSEMBLE mips_cpu_disassemble_instr

extern bool single_step;
extern bool about_to_enter_single_step;
extern int single_step_breakpoint;
extern int old_quiet_mode;
extern int quiet_mode;

/* instr uses the same names as in cpu_mips_instr.c */
#define instr(n) mips_instr_ ## n

#ifdef DYNTRANS_DUALMODE_32
#define instr32(n) mips32_instr_ ## n

#endif


#define X(n) void mips_instr_ ## n(struct cpu *cpu, \
 struct mips_instr_call *ic)

/*
 *  nothing:  Do nothing.
 *
 *  The difference between this function and a "nop" instruction is that
 *  this function does not increase the program counter.  It is used to "get out" of running in translated
 *  mode.
 */
X(nothing)
{
	cpu->cd.mips.next_ic --;
	cpu->ninstrs --;
}

static struct mips_instr_call nothing_call = { instr(nothing), {0,0,0} };

