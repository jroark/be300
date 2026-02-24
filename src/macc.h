#pragma once
#include <stdbool.h>
#include <unicorn/unicorn.h>

/*
 * VR4131 MACC (Multiply-ACCumulate) instructions — SPECIAL2 (opcode 0x1C)
 *
 * These are NEC VR4100-series extensions not present in standard MIPS32.
 * Unicorn will raise an invalid-instruction event for them; we decode and
 * emulate them here, then advance PC past the instruction.
 *
 * Instruction format (R-type):
 *   [31:26] op    = 0x1C (SPECIAL2)
 *   [25:21] rs
 *   [20:16] rt
 *   [15:11] rd    (ignored for MACC; result always goes to HI:LO)
 *   [10: 6] shamt (ignored)
 *   [ 5: 0] funct
 *
 * All variants accumulate into/from the standard MIPS HI:LO register pair.
 */

/* funct field values */
#define MACC_MAC    0x20u   /* HI:LO +=  rs * rt  (signed)              */
#define MACC_MACS   0x21u   /* HI:LO +=  rs * rt  (signed, saturating)  */
#define MACC_MSAC   0x22u   /* HI:LO -=  rs * rt  (signed)              */
#define MACC_MSACS  0x23u   /* HI:LO -=  rs * rt  (signed, saturating)  */
#define MACC_MACU   0x24u   /* HI:LO +=  rs * rt  (unsigned)            */
#define MACC_MACSU  0x25u   /* HI:LO +=  rs * rt  (rs signed, rt unsigned) */
#define MACC_MSACU  0x26u   /* HI:LO -=  rs * rt  (unsigned)            */
#define MACC_MSACSU 0x27u   /* HI:LO -=  rs * rt  (rs signed, rt unsigned) */

/*
 * Called from the UC_HOOK_INSN_INVALID handler.
 * Decodes 'insn' as a MACC instruction, executes it against the current
 * Unicorn register state, advances PC by 4, and returns true.
 * Returns false if 'insn' is not a recognised MACC encoding.
 */
bool macc_execute(uc_engine *uc, uint32_t insn);
