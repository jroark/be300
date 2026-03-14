/*
 * test_basic.c — Sanity tests for the BE-300 emulator
 *
 * Tests run without a ROM file by writing small MIPS32 programs
 * directly into the emulated address space.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unicorn/unicorn.h>

/* Physical addresses used by tests */
#define PA_ROM   0x1FC00000u   /* kseg1 VA 0xBFC00000 -> PA 0x1FC00000 */
#define PA_RAM   0x00000000u   /* kseg1 VA 0xA0000000 -> PA 0x00000000 */
#define VA_START 0xBFC00000u   /* MIPS reset vector (32-bit, for make_uc / MIPS32) */
/*
 * machine_create() now uses MIPS64 mode. In MIPS64, kseg0/kseg1 virtual
 * addresses must be sign-extended to 64 bits (0xFFFFFFFFBFC00000, etc.).
 * Use this when calling uc_emu_start on a machine_t engine.
 */
#define VA_START_64  ((uint64_t)(int32_t)(VA_START))

static int pass_count = 0;
static int fail_count = 0;

#define PASS(name) do { printf("[PASS] %s\n", name); pass_count++; } while(0)
#define FAIL(name, ...) do { printf("[FAIL] %s: ", name); \
    printf(__VA_ARGS__); printf("\n"); fail_count++; } while(0)

/* ------------------------------------------------------------------ */
/* Helper: create a minimal Unicorn MIPS32 LE instance                  */
/* ------------------------------------------------------------------ */

static uc_engine *make_uc(void)
{
    uc_engine *uc;
    uc_err err = uc_open(UC_ARCH_MIPS,
                         UC_MODE_MIPS32 | UC_MODE_LITTLE_ENDIAN, &uc);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_open: %s\n", uc_strerror(err));
        return NULL;
    }
    uc_ctl_set_cpu_model(uc, UC_CPU_MIPS32_4KC);

    /* Map 64 KB ROM region at PA_ROM */
    uc_mem_map(uc, PA_ROM, 0x10000, UC_PROT_ALL);
    /* Map 64 KB RAM region at PA_RAM */
    uc_mem_map(uc, PA_RAM, 0x10000, UC_PROT_ALL);

    return uc;
}

/* ------------------------------------------------------------------ */
/* Test 1: Basic ALU — LUI + ORI + SW, verify memory write              */
/* ------------------------------------------------------------------ */

static void test_alu_store(void)
{
    /*
     * MIPS32 LE little-endian program at VA_START:
     *   lui  $t0, 0xCAFE        # $t0 = 0xCAFE0000
     *   ori  $t0, $t0, 0xBABE   # $t0 = 0xCAFEBABE
     *   lui  $t1, 0xA000        # $t1 = 0xA0000000 (kseg1 base)
     *   sw   $t0, 0($t1)        # MEM[PA_RAM] = 0xCAFEBABE
     *   (stop here)
     */
    static const uint32_t prog[] = {
        0x3C08CAFEu,   /* lui  $t0, 0xCAFE  */
        0x3508BABEu,   /* ori  $t0, $t0, 0xBABE */
        0x3C09A000u,   /* lui  $t1, 0xA000  */
        0xAD280000u,   /* sw   $t0, 0($t1)  */
    };

    uc_engine *uc = make_uc();
    if (!uc) { FAIL("alu_store", "uc_open"); return; }

    uc_mem_write(uc, PA_ROM, prog, sizeof(prog));

    /* Run until after the SW (4 instructions) */
    uint64_t until = VA_START + sizeof(prog);
    uc_err err = uc_emu_start(uc, VA_START, until, 0, 0);
    if (err != UC_ERR_OK) {
        FAIL("alu_store", "uc_emu_start: %s", uc_strerror(err));
        uc_close(uc);
        return;
    }

    uint32_t val = 0;
    uc_mem_read(uc, PA_RAM, &val, 4);

    if (val == 0xCAFEBABEu)
        PASS("alu_store");
    else
        FAIL("alu_store", "expected 0xCAFEBABE, got 0x%08X", val);

    uc_close(uc);
}

/* ------------------------------------------------------------------ */
/* Test 2: Branch / jump — BEQ taken, result in register                */
/* ------------------------------------------------------------------ */

static void test_branch(void)
{
    /*
     *   ori  $t0, $zero, 0       # $t0 = 0
     *   beq  $zero, $zero, +2    # branch forward 2 insns (offset=2)
     *   ori  $t0, $zero, 0xDEAD  # (skipped — in branch delay slot: executed but branch taken)
     *   ori  $t0, $zero, 0xBAD0  # (skipped by branch)
     *   ori  $t0, $zero, 0x600D  # $t0 = 0x600D (branch target)
     *
     * Wait — in MIPS, the delay slot IS executed.  The instruction after BEQ
     * always executes.  Then PC jumps to target.
     *
     * Layout:
     *   +0: ori  $t0, $zero, 0x0000
     *   +4: beq  $zero, $zero, 2    -> target = PC+4 + 2*4 = +16 (insn +4)
     *   +8: ori  $t0, $zero, 0xD5   <- delay slot (executed)
     *  +12: ori  $t0, $zero, 0xBAD  <- skipped
     *  +16: ori  $t0, $zero, 0x600D <- branch target, $t0 ends here
     */
    static const uint32_t prog[] = {
        0x34080000u,   /* ori  $t0, $zero, 0x0000  */
        0x10000002u,   /* beq  $zero, $zero, +2    */
        0x340800D5u,   /* ori  $t0, $zero, 0xD5    (delay slot, executes) */
        0x34080BADu,   /* ori  $t0, $zero, 0xBAD   (skipped) */
        0x3408600Du,   /* ori  $t0, $zero, 0x600D  (branch target) */
    };

    uc_engine *uc = make_uc();
    if (!uc) { FAIL("branch", "uc_open"); return; }

    uc_mem_write(uc, PA_ROM, prog, sizeof(prog));

    /* Run until after insn +4 (the final ORI) */
    uint64_t until = VA_START + sizeof(prog);
    uc_err err = uc_emu_start(uc, VA_START, until, 0, 0);
    if (err != UC_ERR_OK) {
        FAIL("branch", "uc_emu_start: %s", uc_strerror(err));
        uc_close(uc);
        return;
    }

    uint32_t t0 = 0;
    uc_reg_read(uc, UC_MIPS_REG_T0, &t0);

    if (t0 == 0x600Du)
        PASS("branch");
    else
        FAIL("branch", "expected $t0=0x600D, got 0x%08X", t0);

    uc_close(uc);
}

/* ------------------------------------------------------------------ */
/* Test 3: JAL / JR — call and return                                   */
/* ------------------------------------------------------------------ */

static void test_jal_jr(void)
{
    /*
     * Layout (VA_START = 0xBFC00000):
     *   +0  (0xBFC00000): jal  func         $ra = 0xBFC00008, target = 0xBFC00010
     *   +4  (0xBFC00004): nop               (delay slot — always executes)
     *   +8  (0xBFC00008): nop               (return point — stop here via 'until')
     *  +12  (0xBFC0000C): nop               (padding)
     *  +16  (0xBFC00010): ori $v0,$zero,0xBEEF  (func body)
     *  +20  (0xBFC00014): jr  $ra
     *  +24  (0xBFC00018): nop               (jr delay slot)
     *
     * JAL instr_index: target=0xBFC00010, (PC+4)[31:28]=0xB
     *   instr_index = (0xBFC00010 - 0xB0000000) / 4 = 0x0FC00010 / 4 = 0x03F00004
     *   JAL word = (0x03 << 26) | 0x03F00004 = 0x0FF00004
     */
    static const uint32_t prog[] = {
        0x0FF00004u,   /* jal  0xBFC00010  */
        0x00000000u,   /* nop (delay slot) */
        0x00000000u,   /* nop at +8 (return point) */
        0x00000000u,   /* nop padding  */
        0x3402BEEFu,   /* ori  $v0, $zero, 0xBEEF  (func at +16) */
        0x03E00008u,   /* jr   $ra */
        0x00000000u,   /* nop (jr delay slot) */
    };

    uc_engine *uc = make_uc();
    if (!uc) { FAIL("jal_jr", "uc_open"); return; }

    uc_mem_write(uc, PA_ROM, prog, sizeof(prog));

    /*
     * Stop when PC reaches the return point (0xBFC00008).
     * At that moment $v0 must already hold 0xBEEF from the function body.
     */
    uint64_t until = VA_START + 8;   /* 0xBFC00008 */
    uc_err err = uc_emu_start(uc, VA_START, until, 0, 0);
    if (err != UC_ERR_OK) {
        FAIL("jal_jr", "uc_emu_start: %s", uc_strerror(err));
        uc_close(uc);
        return;
    }

    uint32_t v0 = 0;
    uc_reg_read(uc, UC_MIPS_REG_V0, &v0);

    if (v0 == 0xBEEFu)
        PASS("jal_jr");
    else
        FAIL("jal_jr", "expected $v0=0xBEEF, got 0x%08X", v0);

    uc_close(uc);
}

/* ------------------------------------------------------------------ */
/* Test 4: Load / store byte and halfword                               */
/* ------------------------------------------------------------------ */

static void test_load_store(void)
{
    /*
     * Write 0xDEADBEEF to PA_RAM via SW, then read back SB/SH/LBU/LHU.
     *
     *   lui  $t1, 0xA000        # $t1 = 0xA0000000 (kseg1 scratch)
     *   lui  $t0, 0xDEAD
     *   ori  $t0, $t0, 0xBEEF
     *   sw   $t0, 0($t1)        # store 0xDEADBEEF
     *   lbu  $t2, 0($t1)        # load byte 0 (= 0xEF in LE)
     *   lhu  $t3, 0($t1)        # load halfword 0 (= 0xBEEF in LE)
     */
    static const uint32_t prog[] = {
        0x3C09A000u,   /* lui  $t1, 0xA000 */
        0x3C08DEADu,   /* lui  $t0, 0xDEAD */
        0x3508BEEFu,   /* ori  $t0, $t0, 0xBEEF */
        0xAD280000u,   /* sw   $t0, 0($t1) */
        0x912A0000u,   /* lbu  $t2, 0($t1) */
        0x952B0000u,   /* lhu  $t3, 0($t1) */
    };

    uc_engine *uc = make_uc();
    if (!uc) { FAIL("load_store", "uc_open"); return; }

    uc_mem_write(uc, PA_ROM, prog, sizeof(prog));

    uint64_t until = VA_START + sizeof(prog);
    uc_err err = uc_emu_start(uc, VA_START, until, 0, 0);
    if (err != UC_ERR_OK) {
        FAIL("load_store", "uc_emu_start: %s", uc_strerror(err));
        uc_close(uc);
        return;
    }

    uint32_t t2 = 0, t3 = 0;
    uc_reg_read(uc, UC_MIPS_REG_T2, &t2);
    uc_reg_read(uc, UC_MIPS_REG_T3, &t3);

    bool ok = (t2 == 0xEFu) && (t3 == 0xBEEFu);
    if (ok)
        PASS("load_store");
    else
        FAIL("load_store", "t2=0x%02X (exp 0xEF), t3=0x%04X (exp 0xBEEF)",
             t2, t3);

    uc_close(uc);
}

/* ------------------------------------------------------------------ */
/* Test 5: MMIO dispatch — BCU read returns revision ID                  */
/* ------------------------------------------------------------------ */

#include "bus.h"
#include "machine.h"

static void test_mmio_bcu(void)
{
    /*
     * Access BCU REVIDREG (PA 0x0F000010) via kseg1 (VA 0xAF000010).
     *
     *   lui  $t1, 0xAF00        # $t1 = 0xAF000000
     *   lhu  $t0, 0x10($t1)     # load halfword at 0xAF000010
     */
    static const uint32_t prog[] = {
        0x3C09AF00u,   /* lui  $t1, 0xAF00  */
        0x952A0010u,   /* lhu  $t0, 0x10($t1) */
    };

    /* Build a minimal machine so bus_init maps the MMIO region */
    machine_config_t cfg = {
        .trace      = false,
        .log_mmio   = false,
        .rom_path   = NULL,
        .ram_path   = NULL,
        .sdram_size = 16u * 1024u * 1024u,
    };
    machine_t *m = machine_create(&cfg);
    if (!m) { FAIL("mmio_bcu", "machine_create"); return; }

    /* Write test program into ROM region */
    uc_mem_write(m->uc, PA_ROM, prog, sizeof(prog));

    uint64_t until = VA_START_64 + sizeof(prog);
    uc_err err = uc_emu_start(m->uc, VA_START_64, until, 0, 0);
    if (err != UC_ERR_OK) {
        FAIL("mmio_bcu", "uc_emu_start: %s", uc_strerror(err));
        machine_destroy(m);
        return;
    }

    /* 0x952A0010 loads into $t2 (bits 20-16 = 01010 = 10 = $t2) */
    uint32_t t2 = 0;
    uc_reg_read(m->uc, UC_MIPS_REG_T2, &t2);

    if (t2 == BCU_REVID_VR4131)
        PASS("mmio_bcu");
    else
        FAIL("mmio_bcu", "expected BCU REVID 0x%04X, got 0x%08X",
             BCU_REVID_VR4131, t2);

    machine_destroy(m);
}

/* ------------------------------------------------------------------ */
/* Test 6: SIU write outputs a character                                 */
/* ------------------------------------------------------------------ */

static void test_siu_putchar(void)
{
    /*
     * Write 'A' (0x41) to SIU THR (PA 0x0F000800, VA 0xAF000800).
     *
     *   lui  $t1, 0xAF00        # $t1 = 0xAF000000
     *   ori  $t0, $zero, 0x41   # $t0 = 'A'
     *   sh   $t0, 0x800($t1)    # store halfword -> SIU THR
     */
    static const uint32_t prog[] = {
        0x3C09AF00u,   /* lui  $t1, 0xAF00  */
        0x34080041u,   /* ori  $t0, $zero, 0x41 */
        0xA5280800u,   /* sh   $t0, 0x800($t1) */
    };

    machine_config_t cfg = {
        .trace      = false,
        .log_mmio   = false,
        .sdram_size = 16u * 1024u * 1024u,
    };
    machine_t *m = machine_create(&cfg);
    if (!m) { FAIL("siu_putchar", "machine_create"); return; }

    uc_mem_write(m->uc, PA_ROM, prog, sizeof(prog));

    uint64_t until = VA_START_64 + sizeof(prog);
    uc_err err = uc_emu_start(m->uc, VA_START_64, until, 0, 0);
    /* A character should have been printed to stdout */
    if (err == UC_ERR_OK)
        PASS("siu_putchar");
    else
        FAIL("siu_putchar", "uc_emu_start: %s", uc_strerror(err));

    machine_destroy(m);
}

/* ------------------------------------------------------------------ */
/* Test 7: BEQL taken — callee prologue must execute exactly once        */
/* ------------------------------------------------------------------ */

/*
 * Hook callback context for counting visits to key PCs.
 */
typedef struct {
    uint32_t callee_entry;    /* +0x10 */
    uint32_t beql_site;       /* +0x20 */
    uint32_t epilogue;        /* +0x70 */
    uint32_t return_point;    /* +0x08 */
    uint32_t body_marker;     /* +0x28 */
} beql_hook_counts_t;

static void beql_code_hook(uc_engine *uc, uint64_t address, uint32_t size,
                           void *user_data)
{
    (void)uc; (void)size;
    beql_hook_counts_t *c = user_data;
    uint32_t pc = (uint32_t)address;

    if (pc == VA_START + 0x10) c->callee_entry++;
    else if (pc == VA_START + 0x20) c->beql_site++;
    else if (pc == VA_START + 0x70) c->epilogue++;
    else if (pc == VA_START + 0x08) c->return_point++;
    else if (pc == VA_START + 0x28) c->body_marker++;
}

/*
 * Build the test program with either BEQL (opcode=0x50) or BEQ (opcode=0x10)
 * at the branch site.  Layout mirrors the NK callee at 0x80096790.
 *
 * +0x00: jal callee       +0x04: nop (delay)   +0x08: nop (return point)
 * +0x0C: nop (pad)
 * +0x10: addiu sp,sp,-0x28  (callee_entry)
 * +0x14: sw s0,0x18(sp)   +0x18: or s0,a0,zero  +0x1C: sw ra,0x1C(sp)
 * +0x20: beql/beq a3,zero,+0x13  (→ +0x70)
 * +0x24: lw s0,0x18(sp)   (delay slot)
 * +0x28: ori v0,zero,0xBAD (body — should not execute if branch taken)
 * +0x2C..+0x6C: nops
 * +0x70: lw ra,0x1C(sp)   (epilogue)
 * +0x74: jr ra             +0x78: addiu sp,sp,+0x28
 */
static void run_branch_likely_test(const char *name, uint32_t branch_opcode)
{
    /*
     * JAL target = 0xBFC00010.  instr_index = (0xBFC00010 & 0x0FFFFFFF)/4
     *            = 0x0FC00010/4 = 0x03F00004.  JAL = (3<<26)|0x03F00004
     */
    uint32_t prog[31]; /* +0x00 .. +0x78  = 31 words */
    memset(prog, 0, sizeof(prog));

    prog[0]  = 0x0FF00004u;            /* +0x00: jal 0xBFC00010           */
    prog[1]  = 0x00000000u;            /* +0x04: nop (delay slot)         */
    prog[2]  = 0x00000000u;            /* +0x08: nop (return point/stop)  */
    prog[3]  = 0x00000000u;            /* +0x0C: nop (pad)                */
    prog[4]  = 0x27BDFFD8u;            /* +0x10: addiu sp,sp,-0x28        */
    prog[5]  = 0xAFB00018u;            /* +0x14: sw s0,0x18(sp)           */
    prog[6]  = 0x00808025u;            /* +0x18: or s0,a0,zero            */
    prog[7]  = 0xAFBF001Cu;            /* +0x1C: sw ra,0x1C(sp)           */
    /* +0x20: beql/beq a3,zero,+0x13.  Encoding: [op(6)|rs=a3=7(5)|rt=0(5)|imm16=0x0013] */
    prog[8]  = (branch_opcode << 26) | (7u << 21) | (0u << 16) | 0x0013u;
    prog[9]  = 0x8FB00018u;            /* +0x24: lw s0,0x18(sp) (delay)   */
    prog[10] = 0x3402BADu;             /* +0x28: ori v0,zero,0xBAD (body) */
    /* +0x2C..+0x6C: nops (indices 11..27, already zero) */
    prog[28] = 0x8FBF001Cu;            /* +0x70: lw ra,0x1C(sp) (epilogue)*/
    prog[29] = 0x03E00008u;            /* +0x74: jr ra                    */
    prog[30] = 0x27BD0028u;            /* +0x78: addiu sp,sp,+0x28        */

    uc_engine *uc = make_uc();
    if (!uc) { FAIL(name, "uc_open"); return; }

    uc_mem_write(uc, PA_ROM, prog, sizeof(prog));

    /* Set a3 = 0 (branch taken), sp to a sane value in mapped RAM */
    uint32_t a3_val = 0;
    uint32_t sp_val = 0xA0008000u;  /* kseg1, well within mapped 64KB RAM */
    uc_reg_write(uc, UC_MIPS_REG_A3, &a3_val);
    uc_reg_write(uc, UC_MIPS_REG_SP, &sp_val);

    /* Install code hook */
    beql_hook_counts_t counts = {0};
    uc_hook hk;
    uc_hook_add(uc, &hk, UC_HOOK_CODE, beql_code_hook, &counts,
                VA_START, VA_START + sizeof(prog));

    /* Run until return point +0x08 */
    uint64_t until = VA_START + 0x08;
    uc_err err = uc_emu_start(uc, VA_START, until, 0, 0);
    if (err != UC_ERR_OK) {
        FAIL(name, "uc_emu_start: %s", uc_strerror(err));
        uc_close(uc);
        return;
    }

    /* Read back SP, RA, and PC */
    uint32_t sp_after = 0, ra_after = 0, pc_after = 0;
    uc_reg_read(uc, UC_MIPS_REG_SP, &sp_after);
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra_after);
    uc_reg_read(uc, UC_MIPS_REG_PC, &pc_after);

    bool ok = true;
    if (counts.callee_entry != 1) {
        FAIL(name, "callee_entry_count=%u (expected 1) — prologue replayed!",
             counts.callee_entry);
        ok = false;
    }
    if (counts.beql_site != 1) {
        FAIL(name, "beql_site_count=%u (expected 1)", counts.beql_site);
        ok = false;
    }
    if (counts.epilogue != 1) {
        FAIL(name, "epilogue_count=%u (expected 1)", counts.epilogue);
        ok = false;
    }
    /* uc_emu_start stops before executing 'until', so check PC instead */
    if (pc_after != (VA_START + 0x08)) {
        FAIL(name, "PC after return=0x%08X (expected 0x%08X)", pc_after, VA_START + 0x08);
        ok = false;
    }
    if (counts.body_marker != 0) {
        FAIL(name, "body_marker_count=%u (expected 0 — body should be skipped)",
             counts.body_marker);
        ok = false;
    }
    if (sp_after != sp_val) {
        FAIL(name, "SP drift: before=0x%08X after=0x%08X", sp_val, sp_after);
        ok = false;
    }
    if (ra_after != (VA_START + 0x08)) {
        FAIL(name, "RA=0x%08X (expected 0x%08X)", ra_after, VA_START + 0x08);
        ok = false;
    }
    if (ok)
        PASS(name);

    uc_close(uc);
}

static void test_beql_taken(void)
{
    run_branch_likely_test("beql_taken", 0x14);  /* BEQL opcode = 010100 = 0x14 */
}

static void test_beq_taken(void)
{
    run_branch_likely_test("beq_taken_control", 0x04);  /* BEQ opcode = 000100 = 0x04 */
}

/* ------------------------------------------------------------------ */
/* Test 8: MACC instruction emulation (direct API test)                  */
/* ------------------------------------------------------------------ */

#include "macc.h"

static void test_macc(void)
{
    /*
     * IMPORTANT: MIPS32 4Kc (used by Unicorn) interprets SPECIAL2
     * funct=0x20 as CLZ, not as MAC.  The invalid-instruction hook
     * will therefore NOT fire when running through Unicorn.
     *
     * We test macc_execute() directly:
     *   1. Open a Unicorn engine (for register state)
     *   2. Set $t0 = 6, $t1 = 7, AC0 = 0, PC = VA_START
     *   3. Call macc_execute() with a hand-crafted MAC instruction word
     *   4. Verify AC0 = 42 (6 * 7)
     *
     * MAC $0, $t0, $t1 (rs=$t0=8, rt=$t1=9, funct=0x20):
     *   op[31:26]=0x1C  rs=8  rt=9  rd=0  shamt=0  funct=0x20
     *   = (0x1C<<26)|(8<<21)|(9<<16)|0x20 = 0x71090020
     */
    uc_engine *uc = make_uc();
    if (!uc) { FAIL("macc", "uc_open"); return; }

    /* Set up register state */
    uint32_t t0_val = 6, t1_val = 7;
    uint32_t hi_val = 0, lo_val = 0;
    uint32_t pc_val  = VA_START;

    uc_reg_write(uc, UC_MIPS_REG_T0, &t0_val);
    uc_reg_write(uc, UC_MIPS_REG_T1, &t1_val);
    uc_reg_write(uc, UC_MIPS_REG_HI, &hi_val);
    uc_reg_write(uc, UC_MIPS_REG_LO, &lo_val);
    uc_reg_write(uc, UC_MIPS_REG_PC, &pc_val);

    /* Write the MAC instruction to ROM so macc_execute can read it */
    uint32_t mac_insn = 0x71090020u;  /* MAC $0, $t0, $t1 */
    uc_mem_write(uc, PA_ROM, &mac_insn, 4);

    /* Call macc_execute directly */
    bool ok = macc_execute(uc, mac_insn);
    if (!ok) {
        FAIL("macc", "macc_execute returned false");
        uc_close(uc);
        return;
    }

    /* PC should have advanced by 4 */
    uint32_t new_pc = 0;
    uc_reg_read(uc, UC_MIPS_REG_PC, &new_pc);
    if (new_pc != VA_START + 4) {
        FAIL("macc", "PC not advanced: expected 0x%08X got 0x%08X",
             VA_START + 4, new_pc);
        uc_close(uc);
        return;
    }

    /* HI:LO should be 6 * 7 = 42 (fits in LO, HI = 0) */
    uint32_t hi_result = 0, lo_result = 0;
    uc_reg_read(uc, UC_MIPS_REG_HI, &hi_result);
    uc_reg_read(uc, UC_MIPS_REG_LO, &lo_result);
    uint64_t acc_result = ((uint64_t)hi_result << 32) | lo_result;

    if (acc_result == 42u)
        PASS("macc");
    else
        FAIL("macc", "expected HI:LO=42, got %" PRIu64
             " (HI=0x%08X LO=0x%08X)", acc_result, hi_result, lo_result);

    uc_close(uc);
}

/* ------------------------------------------------------------------ */
/* main                                                                  */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=== BE-300 emulator basic tests ===\n\n");

    test_alu_store();
    test_branch();
    test_jal_jr();
    test_load_store();
    test_mmio_bcu();
    test_siu_putchar();
    test_macc();
    test_beql_taken();
    test_beq_taken();

    printf("\n=== Results: %d passed, %d failed ===\n",
           pass_count, fail_count);
    return fail_count ? 1 : 0;
}
