#include <stdio.h>
#include <stdint.h>
#include "macc.h"

/*
 * VR4131 MACC instruction emulation.
 *
 * All MACC variants read rs and rt as 32-bit values, multiply to produce
 * a 64-bit product, then add/subtract that product to/from the 64-bit
 * accumulator in HI:LO (AC0).  Saturating variants clamp on overflow.
 *
 * Unicorn represents HI:LO as a single 64-bit register UC_MIPS_REG_AC0
 * where AC0[63:32] = HI and AC0[31:0] = LO.
 */

#define OP(i)    (((i) >> 26) & 0x3F)
#define RS(i)    (((i) >> 21) & 0x1F)
#define RT(i)    (((i) >> 16) & 0x1F)
#define FUNCT(i) (((i)      ) & 0x3F)

/* Unicorn GPR index -> UC_MIPS_REG_n */
static inline uc_mips_reg gpr_reg(unsigned n)
{
    return (uc_mips_reg)(UC_MIPS_REG_0 + n);
}

/*
 * UC_MIPS_REG_AC0 is a no-op in Unicorn 2.x.
 * The VR4131 MACC accumulates into the standard MIPS HI:LO pair,
 * so we read/write those directly as two 32-bit registers.
 */
static inline int64_t read_acc(uc_engine *uc)
{
    uint32_t hi = 0, lo = 0;
    uc_reg_read(uc, UC_MIPS_REG_HI, &hi);
    uc_reg_read(uc, UC_MIPS_REG_LO, &lo);
    return (int64_t)(((uint64_t)hi << 32) | lo);
}

static inline void write_acc(uc_engine *uc, int64_t v)
{
    uint32_t hi = (uint32_t)((uint64_t)v >> 32);
    uint32_t lo = (uint32_t)((uint64_t)v & 0xFFFFFFFFu);
    uc_reg_write(uc, UC_MIPS_REG_HI, &hi);
    uc_reg_write(uc, UC_MIPS_REG_LO, &lo);
}

static inline int32_t read_gpr32(uc_engine *uc, unsigned n)
{
    uint32_t v = 0;
    uc_reg_read(uc, gpr_reg(n), &v);
    return (int32_t)v;
}

/* Signed 64-bit saturating add */
static int64_t sat_add64(int64_t a, int64_t b)
{
    int64_t r = (int64_t)((uint64_t)a + (uint64_t)b);
    /* overflow if signs of a and b are equal but differ from result */
    if (!((a ^ b) & INT64_MIN) && ((a ^ r) & INT64_MIN))
        r = (a < 0) ? INT64_MIN : INT64_MAX;
    return r;
}

static int64_t sat_sub64(int64_t a, int64_t b)
{
    int64_t r = (int64_t)((uint64_t)a - (uint64_t)b);
    if (((a ^ b) & INT64_MIN) && ((a ^ r) & INT64_MIN))
        r = (a < 0) ? INT64_MIN : INT64_MAX;
    return r;
}

bool macc_execute(uc_engine *uc, uint32_t insn)
{
    if (OP(insn) != 0x1C)
        return false;

    unsigned funct = FUNCT(insn);
    if (funct < MACC_MAC || funct > MACC_MSACSU)
        return false;

    int32_t  rs_s = read_gpr32(uc, RS(insn));
    int32_t  rt_s = read_gpr32(uc, RT(insn));
    uint32_t rs_u = (uint32_t)rs_s;
    uint32_t rt_u = (uint32_t)rt_s;

    int64_t  acc  = read_acc(uc);
    int64_t  prod_s = (int64_t)rs_s * (int64_t)rt_s;
    int64_t  prod_su = (int64_t)rs_s * (int64_t)(uint64_t)rt_u;  /* MACSU */
    uint64_t prod_u = (uint64_t)rs_u * (uint64_t)rt_u;

    int64_t result;
    switch (funct) {
    case MACC_MAC:    result = acc + prod_s;                       break;
    case MACC_MACS:   result = sat_add64(acc, prod_s);             break;
    case MACC_MSAC:   result = acc - prod_s;                       break;
    case MACC_MSACS:  result = sat_sub64(acc, prod_s);             break;
    case MACC_MACU:   result = (int64_t)((uint64_t)acc + prod_u);  break;
    case MACC_MACSU:  result = acc + prod_su;                      break;
    case MACC_MSACU:  result = (int64_t)((uint64_t)acc - prod_u);  break;
    case MACC_MSACSU: result = acc - prod_su;                      break;
    default:
        fprintf(stderr, "[MACC] Unknown funct 0x%02X\n", funct);
        return false;
    }

    write_acc(uc, result);

    /* Advance PC past this instruction */
    uint32_t pc = 0;
    uc_reg_read(uc, UC_MIPS_REG_PC, &pc);
    pc += 4;
    uc_reg_write(uc, UC_MIPS_REG_PC, &pc);

    return true;
}
