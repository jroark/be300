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
 *  MIPS16 slow interpreter.
 *
 *  This is a slow interpreter for the MIPS16 (compact 16-bit encoding)
 *  instruction set, following the same pattern as ARM Thumb support in
 *  cpu_arm.c.  When the CPU is in MIPS16 mode, the dyntrans execution
 *  loop calls this interpreter instead of the normal IC-based execution.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "cpu.h"
#include "cop0.h"
#include "cpu_mips.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"
#include "opcodes_mips16.h"
#include "symbol.h"


static const int mips16_reg_map[8] = MIPS16_REG_MAP;

#define	M16REG(x)	cpu->cd.mips.gpr[mips16_reg_map[(x) & 7]]

/*  Sign-extend a value with 'bits' significant bits  */
#define	SIGN_EXTEND(val, bits)	\
	((int32_t)((val) << (32 - (bits))) >> (32 - (bits)))

/*  Regnames, for disassembly trace output  */
static const char *regnames[] = MIPS_REGISTER_NAMES;


/*
 *  mips_cpu_disassemble_instr_mips16():
 *
 *  Disassemble a MIPS16 instruction.
 */
int mips_cpu_disassemble_instr_mips16(struct cpu *cpu, unsigned char *ib,
	int running, uint64_t dumpaddr)
{
	uint16_t iw;
	int op, rx, ry, rz, sa, imm, func;

	if (cpu->byte_order == EMUL_LITTLE_ENDIAN)
		iw = ib[0] | (ib[1] << 8);
	else
		iw = ib[1] | (ib[0] << 8);

	op = (iw >> 11) & 0x1f;
	rx = (iw >> 8) & 0x7;
	ry = (iw >> 5) & 0x7;
	rz = (iw >> 2) & 0x7;
	sa = (iw >> 2) & 0x7;
	imm = iw & 0xff;
	func = iw & 0x1f;

	debug("%04x\t", iw);

	switch (op) {
	case M16_OP_ADDIUSP:
		debug("addiu\t$%s, $sp, %i\n",
		    regnames[mips16_reg_map[rx]], (imm << 2));
		break;
	case M16_OP_ADDIUPC:
		debug("addiu\t$%s, $pc, %i\n",
		    regnames[mips16_reg_map[rx]], (imm << 2));
		break;
	case M16_OP_B:
		{
			int offset = SIGN_EXTEND(iw & 0x7ff, 11) << 1;
			debug("b\t0x%" PRIx64 "\n",
			    (uint64_t)(dumpaddr + 2 + offset));
		}
		break;
	case M16_OP_JAL:
		/*  32-bit instruction; need next halfword  */
		debug("jal/jalx (32-bit)\n");
		return 4;
	case M16_OP_BEQZ:
		{
			int offset = SIGN_EXTEND(imm, 8) << 1;
			debug("beqz\t$%s, 0x%" PRIx64 "\n",
			    regnames[mips16_reg_map[rx]],
			    (uint64_t)(dumpaddr + 2 + offset));
		}
		break;
	case M16_OP_BNEZ:
		{
			int offset = SIGN_EXTEND(imm, 8) << 1;
			debug("bnez\t$%s, 0x%" PRIx64 "\n",
			    regnames[mips16_reg_map[rx]],
			    (uint64_t)(dumpaddr + 2 + offset));
		}
		break;
	case M16_OP_SHIFT:
		{
			int type = iw & 0x3;
			if (sa == 0)
				sa = 8;
			switch (type) {
			case M16_SHIFT_SLL:
				debug("sll\t$%s, $%s, %i\n",
				    regnames[mips16_reg_map[rx]],
				    regnames[mips16_reg_map[ry]], sa);
				break;
			case M16_SHIFT_SRL:
				debug("srl\t$%s, $%s, %i\n",
				    regnames[mips16_reg_map[rx]],
				    regnames[mips16_reg_map[ry]], sa);
				break;
			case M16_SHIFT_SRA:
				debug("sra\t$%s, $%s, %i\n",
				    regnames[mips16_reg_map[rx]],
				    regnames[mips16_reg_map[ry]], sa);
				break;
			default:
				debug("UNIMPLEMENTED shift type %i\n", type);
			}
		}
		break;
	case M16_OP_RRIA:
		{
			int imm4 = SIGN_EXTEND((iw >> 0) & 0xf, 4);
			debug("addiu\t$%s, $%s, %i\n",
			    regnames[mips16_reg_map[ry]],
			    regnames[mips16_reg_map[rx]], imm4);
		}
		break;
	case M16_OP_ADDIU8:
		debug("addiu\t$%s, %i\n",
		    regnames[mips16_reg_map[rx]],
		    SIGN_EXTEND(imm, 8));
		break;
	case M16_OP_SLTI:
		debug("slti\t$%s, %i\n",
		    regnames[mips16_reg_map[rx]], imm);
		break;
	case M16_OP_SLTIU:
		debug("sltiu\t$%s, %i\n",
		    regnames[mips16_reg_map[rx]], imm);
		break;
	case M16_OP_I8:
		{
			int i8func = (iw >> 8) & 0x7;
			switch (i8func) {
			case M16_I8_BTEQZ:
				debug("bteqz\t0x%" PRIx64 "\n",
				    (uint64_t)(dumpaddr + 2 +
				    (SIGN_EXTEND(imm, 8) << 1)));
				break;
			case M16_I8_BTNEZ:
				debug("btnez\t0x%" PRIx64 "\n",
				    (uint64_t)(dumpaddr + 2 +
				    (SIGN_EXTEND(imm, 8) << 1)));
				break;
			case M16_I8_SWRASP:
				debug("sw\t$ra, %i($sp)\n", (imm << 2));
				break;
			case M16_I8_ADJSP:
				{
					int adj = SIGN_EXTEND(imm, 8) << 3;
					debug("addiu\t$sp, %i\n", adj);
				}
				break;
			case M16_I8_MOV32R:
				{
					int r32 = ((iw >> 3) & 0x1f);
					int rz16 = (iw & 0x7);
					debug("mov32r\t$%s, $%s\n",
					    regnames[r32],
					    regnames[mips16_reg_map[rz16]]);
				}
				break;
			case M16_I8_MOVR32:
				{
					int r32 = iw & 0x1f;
					debug("movr32\t$%s, $%s\n",
					    regnames[mips16_reg_map[ry]],
					    regnames[r32]);
				}
				break;
			default:
				debug("UNIMPLEMENTED I8 func %i\n", i8func);
			}
		}
		break;
	case M16_OP_LI:
		debug("li\t$%s, %i\n",
		    regnames[mips16_reg_map[rx]], imm);
		break;
	case M16_OP_CMPI:
		debug("cmpi\t$%s, %i\n",
		    regnames[mips16_reg_map[rx]], imm);
		break;
	case M16_OP_LB:
		debug("lb\t$%s, %i($%s)\n",
		    regnames[mips16_reg_map[ry]],
		    SIGN_EXTEND((iw & 0x1f), 5),
		    regnames[mips16_reg_map[rx]]);
		break;
	case M16_OP_LH:
		debug("lh\t$%s, %i($%s)\n",
		    regnames[mips16_reg_map[ry]],
		    SIGN_EXTEND((iw & 0x1f), 5) << 1,
		    regnames[mips16_reg_map[rx]]);
		break;
	case M16_OP_LWSP:
		debug("lw\t$%s, %i($sp)\n",
		    regnames[mips16_reg_map[rx]], (imm << 2));
		break;
	case M16_OP_LW:
		debug("lw\t$%s, %i($%s)\n",
		    regnames[mips16_reg_map[ry]],
		    (iw & 0x1f) << 2,
		    regnames[mips16_reg_map[rx]]);
		break;
	case M16_OP_LBU:
		debug("lbu\t$%s, %i($%s)\n",
		    regnames[mips16_reg_map[ry]],
		    (iw & 0x1f),
		    regnames[mips16_reg_map[rx]]);
		break;
	case M16_OP_LHU:
		debug("lhu\t$%s, %i($%s)\n",
		    regnames[mips16_reg_map[ry]],
		    (iw & 0x1f) << 1,
		    regnames[mips16_reg_map[rx]]);
		break;
	case M16_OP_LWPC:
		debug("lw\t$%s, %i($pc)\n",
		    regnames[mips16_reg_map[rx]], (imm << 2));
		break;
	case M16_OP_SB:
		debug("sb\t$%s, %i($%s)\n",
		    regnames[mips16_reg_map[ry]],
		    SIGN_EXTEND((iw & 0x1f), 5),
		    regnames[mips16_reg_map[rx]]);
		break;
	case M16_OP_SH:
		debug("sh\t$%s, %i($%s)\n",
		    regnames[mips16_reg_map[ry]],
		    SIGN_EXTEND((iw & 0x1f), 5) << 1,
		    regnames[mips16_reg_map[rx]]);
		break;
	case M16_OP_SWSP:
		debug("sw\t$%s, %i($sp)\n",
		    regnames[mips16_reg_map[rx]], (imm << 2));
		break;
	case M16_OP_SW:
		debug("sw\t$%s, %i($%s)\n",
		    regnames[mips16_reg_map[ry]],
		    (iw & 0x1f) << 2,
		    regnames[mips16_reg_map[rx]]);
		break;
	case M16_OP_RRR:
		{
			int rrr_func = iw & 0x3;
			switch (rrr_func) {
			case M16_RRR_ADDU:
				debug("addu\t$%s, $%s, $%s\n",
				    regnames[mips16_reg_map[rz]],
				    regnames[mips16_reg_map[rx]],
				    regnames[mips16_reg_map[ry]]);
				break;
			case M16_RRR_SUBU:
				debug("subu\t$%s, $%s, $%s\n",
				    regnames[mips16_reg_map[rz]],
				    regnames[mips16_reg_map[rx]],
				    regnames[mips16_reg_map[ry]]);
				break;
			default:
				debug("UNIMPLEMENTED RRR func %i\n", rrr_func);
			}
		}
		break;
	case M16_OP_RR:
		switch (func) {
		case M16_RR_JR:
			if (rx == 0)
				debug("jr\t$ra\n");
			else
				debug("jr\t$%s\n",
				    regnames[mips16_reg_map[rx]]);
			break;
		case M16_RR_JALR:
			debug("jalr\t$%s\n",
			    regnames[mips16_reg_map[rx]]);
			break;
		case M16_RR_SLT:
			debug("slt\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_SLTU:
			debug("sltu\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_SLLV:
			debug("sllv\t$%s, $%s\n",
			    regnames[mips16_reg_map[ry]],
			    regnames[mips16_reg_map[rx]]);
			break;
		case M16_RR_BREAK:
			debug("break\t%i\n", (iw >> 5) & 0x3f);
			break;
		case M16_RR_SRLV:
			debug("srlv\t$%s, $%s\n",
			    regnames[mips16_reg_map[ry]],
			    regnames[mips16_reg_map[rx]]);
			break;
		case M16_RR_SRAV:
			debug("srav\t$%s, $%s\n",
			    regnames[mips16_reg_map[ry]],
			    regnames[mips16_reg_map[rx]]);
			break;
		case M16_RR_CMP:
			debug("cmp\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_NEG:
			debug("neg\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_AND:
			debug("and\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_OR:
			debug("or\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_XOR:
			debug("xor\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_NOT:
			debug("not\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_MFHI:
			debug("mfhi\t$%s\n",
			    regnames[mips16_reg_map[rx]]);
			break;
		case M16_RR_MFLO:
			debug("mflo\t$%s\n",
			    regnames[mips16_reg_map[rx]]);
			break;
		case M16_RR_MULT:
			debug("mult\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_MULTU:
			debug("multu\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_DIV:
			debug("div\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_DIVU:
			debug("divu\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		default:
			debug("UNIMPLEMENTED RR func 0x%02x\n", func);
		}
		break;
	case M16_OP_EXTEND:
		debug("extend\t(prefix)\n");
		return 2;
	default:
		debug("UNIMPLEMENTED MIPS16 opcode 0x%02x\n", op);
	}

	return 2;
}


/*
 *  Helper: do a load/store via memory_rw.
 *  Returns 1 on success, 0 on failure (exception).
 */
static int m16_memory_rw(struct cpu *cpu, uint64_t addr,
	unsigned char *buf, int len, int writeflag)
{
	return cpu->memory_rw(cpu, cpu->mem, addr, buf, len,
	    writeflag ? MEM_WRITE : MEM_READ,
	    writeflag ? CACHE_DATA : CACHE_DATA);
}


/*
 *  Helper: read a 16-bit instruction halfword from memory.
 *  Returns the instruction word, or sets *ok = 0 on failure.
 */
static uint16_t m16_fetch(struct cpu *cpu, uint64_t addr, int *ok)
{
	uint8_t ib[2];
	if (!cpu->memory_rw(cpu, cpu->mem, addr, ib, sizeof(ib),
	    MEM_READ, CACHE_INSTRUCTION)) {
		*ok = 0;
		return 0;
	}
	*ok = 1;
	if (cpu->byte_order == EMUL_LITTLE_ENDIAN)
		return ib[0] | (ib[1] << 8);
	else
		return ib[1] | (ib[0] << 8);
}


/*
 *  Helper: read/write a 32-bit word from/to memory, handling endianness.
 */
static uint32_t m16_load_word(struct cpu *cpu, uint64_t addr, int *ok)
{
	uint8_t buf[4];
	if (!m16_memory_rw(cpu, addr, buf, 4, 0)) {
		*ok = 0;
		return 0;
	}
	*ok = 1;
	if (cpu->byte_order == EMUL_LITTLE_ENDIAN)
		return buf[0] | (buf[1] << 8) | (buf[2] << 16) |
		    (buf[3] << 24);
	else
		return buf[3] | (buf[2] << 8) | (buf[1] << 16) |
		    (buf[0] << 24);
}

static int m16_store_word(struct cpu *cpu, uint64_t addr, uint32_t val)
{
	uint8_t buf[4];
	if (cpu->byte_order == EMUL_LITTLE_ENDIAN) {
		buf[0] = val;
		buf[1] = val >> 8;
		buf[2] = val >> 16;
		buf[3] = val >> 24;
	} else {
		buf[3] = val;
		buf[2] = val >> 8;
		buf[1] = val >> 16;
		buf[0] = val >> 24;
	}
	return m16_memory_rw(cpu, addr, buf, 4, 1);
}

static uint32_t m16_load_half(struct cpu *cpu, uint64_t addr, int *ok)
{
	uint8_t buf[2];
	if (!m16_memory_rw(cpu, addr, buf, 2, 0)) {
		*ok = 0;
		return 0;
	}
	*ok = 1;
	if (cpu->byte_order == EMUL_LITTLE_ENDIAN)
		return buf[0] | (buf[1] << 8);
	else
		return buf[1] | (buf[0] << 8);
}

static int m16_store_half(struct cpu *cpu, uint64_t addr, uint16_t val)
{
	uint8_t buf[2];
	if (cpu->byte_order == EMUL_LITTLE_ENDIAN) {
		buf[0] = val;
		buf[1] = val >> 8;
	} else {
		buf[1] = val;
		buf[0] = val >> 8;
	}
	return m16_memory_rw(cpu, addr, buf, 2, 1);
}

static uint32_t m16_load_byte(struct cpu *cpu, uint64_t addr, int *ok)
{
	uint8_t buf[1];
	if (!m16_memory_rw(cpu, addr, buf, 1, 0)) {
		*ok = 0;
		return 0;
	}
	*ok = 1;
	return buf[0];
}

static int m16_store_byte(struct cpu *cpu, uint64_t addr, uint8_t val)
{
	return m16_memory_rw(cpu, addr, &val, 1, 1);
}


/*
 *  mips_cpu_interpret_mips16_SLOW():
 *
 *  Interpret a single MIPS16 instruction.  Called from the dyntrans
 *  execution loop when cpu->cd.mips.mips16 is set.
 *
 *  Returns 1 on success, 0 on failure.
 */
int mips_cpu_interpret_mips16_SLOW(struct cpu *cpu)
{
	uint16_t iw;
	uint64_t addr = cpu->pc;
	int op, rx, ry, rz, sa, func;
	int ok = 1;
	int extended = 0;
	uint16_t extend_word = 0;

	if (!cpu->cd.mips.mips16) {
		fatal("mips_cpu_interpret_mips16_SLOW called when not in "
		    "MIPS16 mode?\n");
		cpu->running = 0;
		return 0;
	}


	/*  Fetch the instruction  */
	iw = m16_fetch(cpu, addr, &ok);
	if (!ok) {
		fatal("mips_cpu_interpret_mips16_SLOW(): could not read "
		    "the instruction at 0x%" PRIx64 "\n", addr);
		cpu->running = 0;
		return 0;
	}

	op = (iw >> 11) & 0x1f;

	/*  Check for EXTEND prefix  */
	if (op == M16_OP_EXTEND) {
		extend_word = iw;
		addr += 2;
		iw = m16_fetch(cpu, addr, &ok);
		if (!ok) {
			fatal("mips_cpu_interpret_mips16_SLOW(): could not "
			    "read extended instruction\n");
			cpu->running = 0;
			return 0;
		}
		op = (iw >> 11) & 0x1f;
		extended = 1;
	}

	/*  Instruction trace  */
	if (cpu->machine->instruction_trace) {
		uint64_t offset;
		char *symbol = get_symbol_name(&cpu->machine->symbol_context,
		    cpu->pc, &offset);
		if (symbol != NULL && offset == 0)
			debug("<%s>\n", symbol);
		if (cpu->machine->ncpus > 1)
			debug("cpu%i:\t", cpu->cpu_id);
		debug("%08" PRIx64 ":  ", cpu->pc);
		if (extended) {
			uint8_t ib[2];
			ib[0] = extend_word & 0xff;
			ib[1] = (extend_word >> 8) & 0xff;
			if (cpu->byte_order != EMUL_LITTLE_ENDIAN) {
				ib[0] = (extend_word >> 8) & 0xff;
				ib[1] = extend_word & 0xff;
			}
			mips_cpu_disassemble_instr_mips16(cpu, ib, 1, cpu->pc);
		}
		{
			uint8_t ib[2];
			if (cpu->byte_order == EMUL_LITTLE_ENDIAN) {
				ib[0] = iw & 0xff;
				ib[1] = (iw >> 8) & 0xff;
			} else {
				ib[0] = (iw >> 8) & 0xff;
				ib[1] = iw & 0xff;
			}
			mips_cpu_disassemble_instr_mips16(cpu, ib, 1,
			    extended ? cpu->pc + 2 : cpu->pc);
		}
	}

	cpu->ninstrs ++;

	rx = (iw >> 8) & 0x7;
	ry = (iw >> 5) & 0x7;
	rz = (iw >> 2) & 0x7;
	sa = (iw >> 2) & 0x7;
	func = iw & 0x1f;

	switch (op) {

	case M16_OP_ADDIUSP:
		{
			int imm8 = iw & 0xff;
			if (extended) {
				imm8 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				imm8 = SIGN_EXTEND(imm8, 16);
			} else {
				imm8 <<= 2;
			}
			M16REG(rx) = (int32_t)(
			    (int32_t)cpu->cd.mips.gpr[MIPS_GPR_SP] + imm8);
		}
		break;

	case M16_OP_ADDIUPC:
		{
			int imm8 = iw & 0xff;
			if (extended) {
				imm8 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				imm8 = SIGN_EXTEND(imm8, 16);
			} else {
				imm8 <<= 2;
			}
			M16REG(rx) = (int32_t)(
			    (int32_t)((cpu->pc + 2) & ~3) + imm8);
		}
		break;

	case M16_OP_B:
		{
			int offset = iw & 0x7ff;
			if (extended) {
				offset = ((extend_word & 0x1f) << 11) |
				    (iw & 0x7ff);
				offset = SIGN_EXTEND(offset, 16);
			} else {
				offset = SIGN_EXTEND(offset, 11);
			}
			offset <<= 1;
			cpu->pc = cpu->pc + 2 + offset;
			return 1;
		}

	case M16_OP_JAL:
		{
			/*
			 *  JAL/JALX: This is a 32-bit instruction.
			 *  The first halfword has been consumed as the EXTEND
			 *  check above, but JAL uses a different encoding.
			 *  We need to re-decode: the full 32 bits are:
			 *    [15:11]=00011 [10:0]=target_hi
			 *    [15:0]=target_lo
			 *
			 *  Actually for JAL, the first word IS the JAL opcode
			 *  (not EXTEND).  So if we got here via EXTEND, that's
			 *  wrong.  JAL is always 32-bit.
			 */
			uint16_t hi_word, lo_word;
			uint32_t target;
			int jalx;

			if (extended) {
				/*  We consumed EXTEND then got JAL as next -
				    this shouldn't happen in valid code.  */
				fatal("EXTEND + JAL: invalid\n");
				cpu->running = 0;
				return 0;
			}

			/*  First halfword is iw (the JAL opcode word)  */
			hi_word = iw;
			/*  Fetch second halfword  */
			lo_word = m16_fetch(cpu, cpu->pc + 2, &ok);
			if (!ok) {
				fatal("mips_cpu_interpret_mips16_SLOW(): "
				    "could not read JAL second halfword\n");
				cpu->running = 0;
				return 0;
			}

			/*  Bit 10 of hi_word = X bit (JALX vs JAL)  */
			jalx = (hi_word >> 10) & 1;
			/*
			 *  The 26-bit target is encoded across both
			 *  halfwords with bits [20:16] and [25:21]
			 *  swapped relative to the standard MIPS J
			 *  encoding:
			 *
			 *  hi_word[9:5]  -> target[20:16]
			 *  hi_word[4:0]  -> target[25:21]
			 *  lo_word[15:0] -> target[15:0]
			 */
			target = ((uint32_t)(hi_word & 0x1f) << 21) |
			    ((uint32_t)((hi_word >> 5) & 0x1f) << 16) |
			    lo_word;

			/*  Link: RA = address of instruction after JAL (PC+4,
			 *  since JAL is 4 bytes).  Set bit 0 to indicate
			 *  MIPS16 mode for the return.  */
			cpu->cd.mips.gpr[MIPS_GPR_RA] =
			    (cpu->pc + 4) | 1;

			/*  Compute target: 26-bit field shifted left 2  */
			target <<= 2;
			cpu->pc = ((cpu->pc + 2) & 0xf0000000) | target;

			if (jalx) {
				/*  Switch to MIPS32 mode  */
				cpu->cd.mips.mips16 = 0;
			}

			if (cpu->machine->show_trace_tree)
				cpu_functioncall_trace(cpu, cpu->pc);
			return 1;
		}

	case M16_OP_BEQZ:
		{
			int offset = iw & 0xff;
			if (extended) {
				offset = ((extend_word & 0x1f) << 11) |
				    (iw & 0x7ff);
				offset = SIGN_EXTEND(offset, 16);
			} else {
				offset = SIGN_EXTEND(offset, 8);
			}
			offset <<= 1;
			if (M16REG(rx) == 0) {
				cpu->pc = cpu->pc + 2 + offset;
				return 1;
			}
		}
		break;

	case M16_OP_BNEZ:
		{
			int offset = iw & 0xff;
			if (extended) {
				offset = ((extend_word & 0x1f) << 11) |
				    (iw & 0x7ff);
				offset = SIGN_EXTEND(offset, 16);
			} else {
				offset = SIGN_EXTEND(offset, 8);
			}
			offset <<= 1;
			if (M16REG(rx) != 0) {
				cpu->pc = cpu->pc + 2 + offset;
				return 1;
			}
		}
		break;

	case M16_OP_SHIFT:
		{
			int type = iw & 0x3;
			int amount = sa;
			if (extended) {
				amount = (extend_word >> 6) & 0x1f;
			} else {
				if (amount == 0)
					amount = 8;
			}
			switch (type) {
			case M16_SHIFT_SLL:
				M16REG(rx) = (int32_t)(
				    (uint32_t)M16REG(ry) << amount);
				break;
			case M16_SHIFT_SRL:
				M16REG(rx) = (int32_t)(
				    (uint32_t)M16REG(ry) >> amount);
				break;
			case M16_SHIFT_SRA:
				M16REG(rx) = (int32_t)(
				    (int32_t)M16REG(ry) >> amount);
				break;
			default:
				fatal("UNIMPLEMENTED MIPS16 shift type %i "
				    "at 0x%" PRIx64 "\n", type, cpu->pc);
				cpu->running = 0;
				return 0;
			}
		}
		break;

	case M16_OP_RRIA:
		{
			int imm4;
			if (extended) {
				imm4 = ((extend_word & 0xf) << 11) |
				    (iw & 0xf);
				imm4 = SIGN_EXTEND(imm4, 15);
			} else {
				imm4 = SIGN_EXTEND(iw & 0xf, 4);
			}
			M16REG(ry) = (int32_t)(
			    (int32_t)M16REG(rx) + imm4);
		}
		break;

	case M16_OP_ADDIU8:
		{
			int imm8;
			if (extended) {
				imm8 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				imm8 = SIGN_EXTEND(imm8, 16);
			} else {
				imm8 = SIGN_EXTEND(iw & 0xff, 8);
			}
			M16REG(rx) = (int32_t)(
			    (int32_t)M16REG(rx) + imm8);
		}
		break;

	case M16_OP_SLTI:
		{
			int imm8;
			if (extended) {
				imm8 = ((extend_word & 0x1f) << 11) |
				    (iw & 0xff);
				imm8 = SIGN_EXTEND(imm8, 16);
			} else {
				imm8 = iw & 0xff;
			}
			cpu->cd.mips.gpr[MIPS_GPR_T8] =
			    ((int32_t)M16REG(rx) < imm8) ? 1 : 0;
		}
		break;

	case M16_OP_SLTIU:
		{
			uint32_t imm8;
			if (extended) {
				imm8 = ((extend_word & 0x1f) << 11) |
				    (iw & 0xff);
				imm8 = SIGN_EXTEND(imm8, 16);
			} else {
				imm8 = iw & 0xff;
			}
			cpu->cd.mips.gpr[MIPS_GPR_T8] =
			    ((uint32_t)M16REG(rx) < imm8) ? 1 : 0;
		}
		break;

	case M16_OP_I8:
		{
			int i8func = (iw >> 8) & 0x7;
			int imm8 = iw & 0xff;
			switch (i8func) {
			case M16_I8_BTEQZ:
				{
					int offset;
					if (extended) {
						offset = ((extend_word & 0x1f)
						    << 11) | (iw & 0x7ff);
						offset = SIGN_EXTEND(offset, 16);
					} else {
						offset = SIGN_EXTEND(imm8, 8);
					}
					offset <<= 1;
					if (cpu->cd.mips.gpr[MIPS_GPR_T8]
					    == 0) {
						cpu->pc = cpu->pc + 2 +
						    offset;
						return 1;
					}
				}
				break;
			case M16_I8_BTNEZ:
				{
					int offset;
					if (extended) {
						offset = ((extend_word & 0x1f)
						    << 11) | (iw & 0x7ff);
						offset = SIGN_EXTEND(offset, 16);
					} else {
						offset = SIGN_EXTEND(imm8, 8);
					}
					offset <<= 1;
					if (cpu->cd.mips.gpr[MIPS_GPR_T8]
					    != 0) {
						cpu->pc = cpu->pc + 2 +
						    offset;
						return 1;
					}
				}
				break;
			case M16_I8_SWRASP:
				{
					int offset8;
					if (extended) {
						offset8 = ((extend_word & 0x1f)
						    << 11) |
						    ((extend_word >> 5) & 0x3f)
						    << 5 | (iw & 0x1f);
						offset8 = SIGN_EXTEND(
						    offset8, 16);
					} else {
						offset8 = imm8 << 2;
					}
					uint64_t a =
					    cpu->cd.mips.gpr[MIPS_GPR_SP] +
					    offset8;
					if (!m16_store_word(cpu, a,
					    cpu->cd.mips.gpr[MIPS_GPR_RA])) {
						cpu->running = 0;
						return 0;
					}
				}
				break;
			case M16_I8_ADJSP:
				{
					int adj;
					if (extended) {
						adj = ((extend_word & 0x1f)
						    << 11) |
						    ((extend_word >> 5) & 0x3f)
						    << 5 | (iw & 0x1f);
						adj = SIGN_EXTEND(adj, 16);
					} else {
						adj = SIGN_EXTEND(imm8, 8)
						    << 3;
					}
					cpu->cd.mips.gpr[MIPS_GPR_SP] =
					    (int32_t)(
					    (int32_t)cpu->cd.mips.
					    gpr[MIPS_GPR_SP] + adj);
				}
				break;
			case M16_I8_MOV32R:
				{
					/*
					 *  MOV32R: move MIPS16 reg to
					 *  MIPS32 reg.
					 *  r32 = bits [7:5,2:0] = rz | ry
					 *  Actually: r32 = bits 4:0
					 *  combined from different fields.
					 */
					int r32 = ((iw >> 3) & 0x1f);
					int rz16 = iw & 0x7;
					cpu->cd.mips.gpr[r32] =
					    M16REG(rz16);
				}
				break;
			case M16_I8_MOVR32:
				{
					/*  MOVR32: move MIPS32 reg to
					 *  MIPS16 reg.  */
					int r32 = iw & 0x1f;
					M16REG(ry) = cpu->cd.mips.gpr[r32];
				}
				break;
			default:
				fatal("UNIMPLEMENTED MIPS16 I8 func %i "
				    "at 0x%" PRIx64 "\n", i8func, cpu->pc);
				cpu->running = 0;
				return 0;
			}
		}
		break;

	case M16_OP_LI:
		{
			int imm8 = iw & 0xff;
			if (extended) {
				imm8 = ((extend_word & 0x1f) << 11) |
				    (iw & 0x7ff);
				imm8 = SIGN_EXTEND(imm8, 16);
			}
			M16REG(rx) = (int32_t)imm8;
		}
		break;

	case M16_OP_CMPI:
		{
			int imm8 = iw & 0xff;
			if (extended) {
				imm8 = ((extend_word & 0x1f) << 11) |
				    (iw & 0x7ff);
				imm8 = SIGN_EXTEND(imm8, 16);
			}
			cpu->cd.mips.gpr[MIPS_GPR_T8] =
			    ((uint32_t)M16REG(rx) ^ (uint32_t)imm8);
		}
		break;

	case M16_OP_LB:
		{
			int offset5 = iw & 0x1f;
			uint64_t a;
			uint32_t val;
			if (extended) {
				offset5 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				offset5 = SIGN_EXTEND(offset5, 16);
			} else {
				offset5 = SIGN_EXTEND(offset5, 5);
			}
			a = (uint64_t)((int64_t)(int32_t)M16REG(rx) +
			    offset5);
			val = m16_load_byte(cpu, a, &ok);
			if (!ok) { cpu->running = 0; return 0; }
			M16REG(ry) = (int32_t)(int8_t)val;
		}
		break;

	case M16_OP_LH:
		{
			int offset5 = iw & 0x1f;
			uint64_t a;
			uint32_t val;
			if (extended) {
				offset5 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				offset5 = SIGN_EXTEND(offset5, 16);
			} else {
				offset5 = SIGN_EXTEND(offset5, 5) << 1;
			}
			a = (uint64_t)((int64_t)(int32_t)M16REG(rx) +
			    offset5);
			val = m16_load_half(cpu, a, &ok);
			if (!ok) { cpu->running = 0; return 0; }
			M16REG(ry) = (int32_t)(int16_t)val;
		}
		break;

	case M16_OP_LWSP:
		{
			int imm8 = iw & 0xff;
			uint64_t a;
			uint32_t val;
			if (extended) {
				imm8 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				imm8 = SIGN_EXTEND(imm8, 16);
			} else {
				imm8 <<= 2;
			}
			a = (uint64_t)((int64_t)(int32_t)
			    cpu->cd.mips.gpr[MIPS_GPR_SP] + imm8);
			val = m16_load_word(cpu, a, &ok);
			if (!ok) { cpu->running = 0; return 0; }
			M16REG(rx) = (int32_t)val;
		}
		break;

	case M16_OP_LW:
		{
			int offset5 = iw & 0x1f;
			uint64_t a;
			uint32_t val;
			if (extended) {
				offset5 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				offset5 = SIGN_EXTEND(offset5, 16);
			} else {
				offset5 <<= 2;
			}
			a = (uint64_t)((int64_t)(int32_t)M16REG(rx) +
			    offset5);
			val = m16_load_word(cpu, a, &ok);
			if (!ok) { cpu->running = 0; return 0; }
			M16REG(ry) = (int32_t)val;
		}
		break;

	case M16_OP_LBU:
		{
			int offset5 = iw & 0x1f;
			uint64_t a;
			uint32_t val;
			if (extended) {
				offset5 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				offset5 = SIGN_EXTEND(offset5, 16);
			} else {
				/*  unsigned offset, not shifted  */
			}
			a = (uint64_t)((int64_t)(int32_t)M16REG(rx) +
			    offset5);
			val = m16_load_byte(cpu, a, &ok);
			if (!ok) { cpu->running = 0; return 0; }
			M16REG(ry) = val;
		}
		break;

	case M16_OP_LHU:
		{
			int offset5 = iw & 0x1f;
			uint64_t a;
			uint32_t val;
			if (extended) {
				offset5 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				offset5 = SIGN_EXTEND(offset5, 16);
			} else {
				offset5 <<= 1;
			}
			a = (uint64_t)((int64_t)(int32_t)M16REG(rx) +
			    offset5);
			val = m16_load_half(cpu, a, &ok);
			if (!ok) { cpu->running = 0; return 0; }
			M16REG(ry) = val;
		}
		break;

	case M16_OP_LWPC:
		{
			int imm8 = iw & 0xff;
			uint64_t a;
			uint32_t val;
			if (extended) {
				imm8 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				imm8 = SIGN_EXTEND(imm8, 16);
			} else {
				imm8 <<= 2;
			}
			a = ((cpu->pc + 2) & ~3) + imm8;
			val = m16_load_word(cpu, a, &ok);
			if (!ok) { cpu->running = 0; return 0; }
			M16REG(rx) = (int32_t)val;
		}
		break;

	case M16_OP_SB:
		{
			int offset5 = iw & 0x1f;
			uint64_t a;
			if (extended) {
				offset5 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				offset5 = SIGN_EXTEND(offset5, 16);
			} else {
				offset5 = SIGN_EXTEND(offset5, 5);
			}
			a = (uint64_t)((int64_t)(int32_t)M16REG(rx) +
			    offset5);
			if (!m16_store_byte(cpu, a, M16REG(ry))) {
				cpu->running = 0;
				return 0;
			}
		}
		break;

	case M16_OP_SH:
		{
			int offset5 = iw & 0x1f;
			uint64_t a;
			if (extended) {
				offset5 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				offset5 = SIGN_EXTEND(offset5, 16);
			} else {
				offset5 = SIGN_EXTEND(offset5, 5) << 1;
			}
			a = (uint64_t)((int64_t)(int32_t)M16REG(rx) +
			    offset5);
			if (!m16_store_half(cpu, a, M16REG(ry))) {
				cpu->running = 0;
				return 0;
			}
		}
		break;

	case M16_OP_SWSP:
		{
			int imm8 = iw & 0xff;
			uint64_t a;
			if (extended) {
				imm8 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				imm8 = SIGN_EXTEND(imm8, 16);
			} else {
				imm8 <<= 2;
			}
			a = (uint64_t)((int64_t)(int32_t)
			    cpu->cd.mips.gpr[MIPS_GPR_SP] + imm8);
			if (!m16_store_word(cpu, a, M16REG(rx))) {
				cpu->running = 0;
				return 0;
			}
		}
		break;

	case M16_OP_SW:
		{
			int offset5 = iw & 0x1f;
			uint64_t a;
			if (extended) {
				offset5 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				offset5 = SIGN_EXTEND(offset5, 16);
			} else {
				offset5 <<= 2;
			}
			a = (uint64_t)((int64_t)(int32_t)M16REG(rx) +
			    offset5);
			if (!m16_store_word(cpu, a, M16REG(ry))) {
				cpu->running = 0;
				return 0;
			}
		}
		break;

	case M16_OP_RRR:
		{
			int rrr_func = iw & 0x3;
			switch (rrr_func) {
			case M16_RRR_ADDU:
				M16REG(rz) = (int32_t)(
				    (uint32_t)M16REG(rx) +
				    (uint32_t)M16REG(ry));
				break;
			case M16_RRR_SUBU:
				M16REG(rz) = (int32_t)(
				    (uint32_t)M16REG(rx) -
				    (uint32_t)M16REG(ry));
				break;
			default:
				fatal("UNIMPLEMENTED MIPS16 RRR func %i "
				    "at 0x%" PRIx64 "\n", rrr_func, cpu->pc);
				cpu->running = 0;
				return 0;
			}
		}
		break;

	case M16_OP_RR:
		switch (func) {
		case M16_RR_JR:
			{
				uint64_t target;
				if (rx == 0) {
					/*  JR $ra  */
					target = cpu->cd.mips.gpr[MIPS_GPR_RA];
				} else {
					target = M16REG(rx);
				}
				if (target & 1) {
					/*  Stay in MIPS16 mode  */
					cpu->pc = target & ~(uint64_t)1;
				} else {
					/*  Switch to MIPS32 mode  */
					cpu->cd.mips.mips16 = 0;
					cpu->pc = target;
				}
				if (rx == 0 &&
				    cpu->machine->show_trace_tree)
					cpu_functioncall_trace_return(cpu);
				return 1;
			}
		case M16_RR_JALR:
			{
				uint64_t target = M16REG(rx);
				/*  Link: RA = PC + 2, with MIPS16 bit  */
				cpu->cd.mips.gpr[MIPS_GPR_RA] =
				    (cpu->pc + 2) | 1;
				if (target & 1) {
					cpu->pc = target & ~(uint64_t)1;
				} else {
					cpu->cd.mips.mips16 = 0;
					cpu->pc = target;
				}
				if (cpu->machine->show_trace_tree)
					cpu_functioncall_trace(cpu, cpu->pc);
				return 1;
			}
		case M16_RR_SLT:
			cpu->cd.mips.gpr[MIPS_GPR_T8] =
			    ((int32_t)M16REG(rx) < (int32_t)M16REG(ry))
			    ? 1 : 0;
			break;
		case M16_RR_SLTU:
			cpu->cd.mips.gpr[MIPS_GPR_T8] =
			    ((uint32_t)M16REG(rx) < (uint32_t)M16REG(ry))
			    ? 1 : 0;
			break;
		case M16_RR_SLLV:
			M16REG(ry) = (int32_t)(
			    (uint32_t)M16REG(ry) <<
			    (M16REG(rx) & 0x1f));
			break;
		case M16_RR_BREAK:
			mips_cpu_exception(cpu, EXCEPTION_BP, 0, 0,
			    0, 0, 0, 0);
			return 1;
		case M16_RR_SRLV:
			M16REG(ry) = (int32_t)(
			    (uint32_t)M16REG(ry) >>
			    (M16REG(rx) & 0x1f));
			break;
		case M16_RR_SRAV:
			M16REG(ry) = (int32_t)(
			    (int32_t)M16REG(ry) >>
			    (M16REG(rx) & 0x1f));
			break;
		case M16_RR_CMP:
			cpu->cd.mips.gpr[MIPS_GPR_T8] =
			    ((uint32_t)M16REG(rx) ^
			    (uint32_t)M16REG(ry));
			break;
		case M16_RR_NEG:
			M16REG(rx) = (int32_t)(-(int32_t)M16REG(ry));
			break;
		case M16_RR_AND:
			M16REG(rx) = M16REG(rx) & M16REG(ry);
			break;
		case M16_RR_OR:
			M16REG(rx) = M16REG(rx) | M16REG(ry);
			break;
		case M16_RR_XOR:
			M16REG(rx) = M16REG(rx) ^ M16REG(ry);
			break;
		case M16_RR_NOT:
			M16REG(rx) = ~M16REG(ry);
			break;
		case M16_RR_MFHI:
			M16REG(rx) = cpu->cd.mips.hi;
			break;
		case M16_RR_MFLO:
			M16REG(rx) = cpu->cd.mips.lo;
			break;
		case M16_RR_MULT:
			{
				int64_t result = (int64_t)(int32_t)M16REG(rx) *
				    (int64_t)(int32_t)M16REG(ry);
				cpu->cd.mips.lo = (int32_t)result;
				cpu->cd.mips.hi = (int32_t)(result >> 32);
			}
			break;
		case M16_RR_MULTU:
			{
				uint64_t result =
				    (uint64_t)(uint32_t)M16REG(rx) *
				    (uint64_t)(uint32_t)M16REG(ry);
				cpu->cd.mips.lo = (int32_t)result;
				cpu->cd.mips.hi = (int32_t)(result >> 32);
			}
			break;
		case M16_RR_DIV:
			if ((int32_t)M16REG(ry) != 0) {
				cpu->cd.mips.lo = (int32_t)(
				    (int32_t)M16REG(rx) /
				    (int32_t)M16REG(ry));
				cpu->cd.mips.hi = (int32_t)(
				    (int32_t)M16REG(rx) %
				    (int32_t)M16REG(ry));
			}
			break;
		case M16_RR_DIVU:
			if ((uint32_t)M16REG(ry) != 0) {
				cpu->cd.mips.lo = (int32_t)(
				    (uint32_t)M16REG(rx) /
				    (uint32_t)M16REG(ry));
				cpu->cd.mips.hi = (int32_t)(
				    (uint32_t)M16REG(rx) %
				    (uint32_t)M16REG(ry));
			}
			break;
		default:
			fatal("UNIMPLEMENTED MIPS16 RR func 0x%02x "
			    "at 0x%" PRIx64 "\n", func, cpu->pc);
			cpu->running = 0;
			return 0;
		}
		break;

	default:
		fatal("UNIMPLEMENTED MIPS16 opcode 0x%02x "
		    "at 0x%" PRIx64 "\n", op, cpu->pc);
		cpu->running = 0;
		return 0;
	}

	/*  Advance PC past this instruction  */
	if (extended)
		cpu->pc += 4;
	else
		cpu->pc += 2;

	return 1;
}
