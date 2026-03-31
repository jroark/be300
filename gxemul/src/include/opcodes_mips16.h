/*
 *  Copyright (C) 2003-2021  Anders Gavare.  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 *  OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 *  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 *  OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 *  SUCH DAMAGE.
 *
 *
 *  MIPS16 (MIPS16e) opcode definitions.
 *
 *  MIPS16 is a special encoding form for MIPS32/64 which uses 16-bit
 *  instruction words instead of 32-bit. Some instructions can be
 *  prefixed with an EXTEND word to form 32-bit extended instructions
 *  with larger immediate fields.
 *
 *  The 8 directly-addressable registers in MIPS16 map to MIPS32 GPRs:
 *    MIPS16 reg 0-7 -> MIPS32 $16, $17, $2, $3, $4, $5, $6, $7
 *
 *  Additionally, $24 (t8), $25 (t9), $29 (sp), $31 (ra) are
 *  implicitly accessible by certain instructions.
 */

#ifndef	OPCODES_MIPS16_H
#define	OPCODES_MIPS16_H


/*
 *  MIPS16-to-MIPS32 register mapping.
 *  MIPS16 reg 0..7 -> MIPS32 GPR $16, $17, $2, $3, $4, $5, $6, $7
 */
#define	MIPS16_REG_MAP	{ 16, 17, 2, 3, 4, 5, 6, 7 }


/*
 *  Primary opcodes (bits 15:11 of the 16-bit instruction word):
 */
#define	M16_OP_ADDIUSP		0x00	/*  ADDIU rx, sp, imm  */
#define	M16_OP_ADDIUPC		0x01	/*  ADDIU rx, pc, imm  */
#define	M16_OP_B		0x02	/*  B offset (unconditional branch)  */
#define	M16_OP_JAL		0x03	/*  JAL / JALX (32-bit instruction)  */
#define	M16_OP_BEQZ		0x04	/*  BEQZ rx, offset  */
#define	M16_OP_BNEZ		0x05	/*  BNEZ rx, offset  */
#define	M16_OP_SHIFT		0x06	/*  SLL, SRL, SRA  */
#define	M16_OP_LD		0x07	/*  LD ry, offset(rx) [64-bit]  */
#define	M16_OP_RRIA		0x08	/*  ADDIU ry, rx, imm (3-reg)  */
#define	M16_OP_ADDIU8		0x09	/*  ADDIU rx, imm8  */
#define	M16_OP_SLTI		0x0a	/*  SLTI rx, imm8  */
#define	M16_OP_SLTIU		0x0b	/*  SLTIU rx, imm8  */
#define	M16_OP_I8		0x0c	/*  I8 group  */
#define	M16_OP_LI		0x0d	/*  LI rx, imm8  */
#define	M16_OP_CMPI		0x0e	/*  CMPI rx, imm8  */
#define	M16_OP_SD		0x0f	/*  SD ry, offset(rx) [64-bit]  */
#define	M16_OP_LB		0x10	/*  LB ry, offset(rx)  */
#define	M16_OP_LH		0x11	/*  LH ry, offset(rx)  */
#define	M16_OP_LWSP		0x12	/*  LW rx, offset(sp)  */
#define	M16_OP_LW		0x13	/*  LW ry, offset(rx)  */
#define	M16_OP_LBU		0x14	/*  LBU ry, offset(rx)  */
#define	M16_OP_LHU		0x15	/*  LHU ry, offset(rx)  */
#define	M16_OP_LWPC		0x16	/*  LW rx, offset(pc)  */
#define	M16_OP_LWU		0x17	/*  LWU ry, offset(rx) [64-bit]  */
#define	M16_OP_SB		0x18	/*  SB ry, offset(rx)  */
#define	M16_OP_SH		0x19	/*  SH ry, offset(rx)  */
#define	M16_OP_SWSP		0x1a	/*  SW rx, offset(sp)  */
#define	M16_OP_SW		0x1b	/*  SW ry, offset(rx)  */
#define	M16_OP_RRR		0x1c	/*  RRR group  */
#define	M16_OP_RR		0x1d	/*  RR group  */
#define	M16_OP_EXTEND		0x1e	/*  EXTEND prefix  */
#define	M16_OP_I64		0x1f	/*  I64 group [64-bit]  */


/*
 *  RR function codes (bits 4:0 when primary opcode = M16_OP_RR):
 */
#define	M16_RR_JR_RA		0x00	/*  JR $ra  (rx field = 0)  */
#define	M16_RR_JR		0x00	/*  JR rx   (rx field != 0)  */
#define	M16_RR_JALR		0x01	/*  JALR $ra, rx  (ry=0)  */
#define	M16_RR_JRRA		0x01	/*  JR $ra  (alias when rx=0,ry=0)  */
#define	M16_RR_SLT		0x02	/*  SLT  */
#define	M16_RR_SLTU		0x03	/*  SLTU  */
#define	M16_RR_SLLV		0x04	/*  SLLV  */
#define	M16_RR_BREAK		0x05	/*  BREAK  */
#define	M16_RR_SRLV		0x06	/*  SRLV  */
#define	M16_RR_SRAV		0x07	/*  SRAV  */
#define	M16_RR_CMP		0x0a	/*  CMP rx, ry  */
#define	M16_RR_NEG		0x0b	/*  NEG rx, ry  */
#define	M16_RR_AND		0x0c	/*  AND rx, ry  */
#define	M16_RR_OR		0x0d	/*  OR rx, ry  */
#define	M16_RR_XOR		0x0e	/*  XOR rx, ry  */
#define	M16_RR_NOT		0x0f	/*  NOT rx, ry  */
#define	M16_RR_MFHI		0x10	/*  MFHI rx  */
#define	M16_RR_MFLO		0x12	/*  MFLO rx  */
#define	M16_RR_MULT		0x18	/*  MULT rx, ry  */
#define	M16_RR_MULTU		0x19	/*  MULTU rx, ry  */
#define	M16_RR_DIV		0x1a	/*  DIV rx, ry  */
#define	M16_RR_DIVU		0x1b	/*  DIVU rx, ry  */


/*
 *  RRR function codes (bits 1:0 when primary opcode = M16_OP_RRR):
 */
#define	M16_RRR_ADDU		0x01	/*  ADDU rz, rx, ry  */
#define	M16_RRR_SUBU		0x03	/*  SUBU rz, rx, ry  */


/*
 *  SHIFT sub-function (bits 1:0 when primary opcode = M16_OP_SHIFT):
 */
#define	M16_SHIFT_SLL		0x00	/*  SLL rx, ry, sa  */
#define	M16_SHIFT_SRL		0x02	/*  SRL rx, ry, sa  */
#define	M16_SHIFT_SRA		0x03	/*  SRA rx, ry, sa  */


/*
 *  I8 function codes (bits 10:8 when primary opcode = M16_OP_I8):
 */
#define	M16_I8_BTEQZ		0x00	/*  BTEQZ offset  */
#define	M16_I8_BTNEZ		0x01	/*  BTNEZ offset  */
#define	M16_I8_SWRASP		0x02	/*  SW $ra, offset(sp)  */
#define	M16_I8_ADJSP		0x03	/*  ADDIU sp, imm  */
#define	M16_I8_SVRS		0x04	/*  SAVE/RESTORE (MIPS16e)  */
#define	M16_I8_MOV32R		0x05	/*  MOV32R r32, rz  */
#define	M16_I8_MOVR32		0x07	/*  MOVR32 ry, r32  */


/*
 *  RRIA sub-function (bit 4 when primary opcode = M16_OP_RRIA):
 */
#define	M16_RRIA_ADDIU		0	/*  ADDIU ry, rx, imm4  */
#define	M16_RRIA_DADDIU		1	/*  DADDIU ry, rx, imm4 [64-bit]  */


#endif	/*  OPCODES_MIPS16_H  */
