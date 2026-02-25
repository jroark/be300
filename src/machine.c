#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "machine.h"
#include "bus.h"
#include "loader.h"
#include "macc.h"

/*
 * In MIPS64 mode, kseg0/kseg1 virtual addresses (0x80000000–0xBFFFFFFF)
 * must be sign-extended to 64 bits for Unicorn.  Plain 0x80000000 is kuseg
 * in 64-bit address space; 0xFFFFFFFF80000000 is the correct kseg0 base.
 */
static inline uint64_t mips_sext(uint32_t va32) {
    return (uint64_t)(int32_t)va32;
}

/*
 * Temporary timer shim:
 * linux4.be spins in calibrate_delay waiting for jiffies to change.
 * Until CP0 timer/interrupt delivery is fully emulated, bump jiffies
 * in RAM once per execution batch to keep early boot moving.
 */
static void tick_jiffies_hack(machine_t *m)
{
    static const uint32_t jiffies_pa = 0x0028D9E0u;
    uint32_t j = 0;
    if (uc_mem_read(m->uc, jiffies_pa, &j, sizeof(j)) == UC_ERR_OK) {
        j += 1;
        uc_mem_write(m->uc, jiffies_pa, &j, sizeof(j));
    }
}

/* Convert kseg0/kseg1 VA to physical; return false for non-direct-mapped VA. */
static bool va_to_pa_kseg(uint64_t va, uint64_t *pa_out)
{
    uint32_t va32 = (uint32_t)va;
    if ((va32 >= 0x80000000u && va32 <= 0xBFFFFFFFu) ||
        ((uint64_t)(int64_t)(int32_t)va32) == va) {
        *pa_out = (uint64_t)(va32 & 0x1FFFFFFFu);
        return true;
    }
    return false;
}

/*
 * Best-effort instruction read for hooks.
 * Unicorn sometimes reports virtual PC while uc_mem_read expects mapped PA.
 */
static bool read_insn_best_effort(uc_engine *uc, uint64_t address, uint32_t *insn)
{
    if (uc_mem_read(uc, address, insn, 4) == UC_ERR_OK)
        return true;

    uint64_t pa = 0;
    if (va_to_pa_kseg(address, &pa) && uc_mem_read(uc, pa, insn, 4) == UC_ERR_OK)
        return true;

    return false;
}

/* ------------------------------------------------------------------ */
/* Unicorn hooks                                                         */
/* ------------------------------------------------------------------ */

/*
 * Per-instruction trace hook.
 * Only installed when cfg.trace == true (significant performance impact).
 */
static void trace_hook(uc_engine *uc, uint64_t address,
                        uint32_t size, void *user_data)
{
    (void)size;
    machine_t *m = user_data;
    m->insn_count++;

    uint32_t insn = 0;
    if (!read_insn_best_effort(uc, address, &insn))
        insn = 0xFFFFFFFFu;

    /* Read a few key registers for context */
    uint32_t pc = (uint32_t)address;
    uint64_t at=0, v0=0, a0=0, sp=0, ra=0;
    uc_reg_read(uc, UC_MIPS_REG_AT, &at);
    uc_reg_read(uc, UC_MIPS_REG_V0, &v0);
    uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
    uc_reg_read(uc, UC_MIPS_REG_SP, &sp);
    uc_reg_read(uc, UC_MIPS_REG_RA, &ra);

    fprintf(stderr, "[T] %08X: %08X  at=%016" PRIX64 " v0=%016" PRIX64
                    " a0=%016" PRIX64 " sp=%016" PRIX64 " ra=%016" PRIX64 "\n",
            pc, insn, at, v0, a0, sp, ra);
}

/*
 * PRId intercept hook.
 *
 * Unicorn runs with the NEC VR5432 CPU model (PRId = 0x00000400), but the
 * linux4.be kernel identifies the platform by checking CP0 PRId against the
 * VR4131 value (0x00000C80).  Unicorn does not expose CP0_PRID as writable,
 * so we intercept every `mfc0 $rt, $15` instruction (CP0 reg 15 = PRId):
 *
 *   Encoding: 0x4000_7800 | (rt << 16)
 *   Mask:     0xFFE0_FFFF  (fix all bits except the 5-bit rt field)
 *
 * When detected:
 *   1. Write VR4131_PRID to the destination GPR.
 *   2. Advance PC by 4 to skip the instruction (Unicorn respects PC changes
 *      made inside UC_HOOK_CODE before the instruction executes).
 */
static void prid_hook(uc_engine *uc, uint64_t address,
                      uint32_t size, void *user_data)
{
    (void)size; (void)user_data;

    uint32_t insn = 0;
    if (!read_insn_best_effort(uc, address, &insn))
        return;

    /* MFC0 $rt, $15 (PRId) */
    if ((insn & 0xFFE0FFFFu) == 0x40007800u) {
        uint32_t rt = (insn >> 16) & 0x1F;
        uint64_t prid = VR4131_PRID;
        /* UC_MIPS_REG_0 + rt gives the correct register enum for GPR $rt */
        uc_reg_write(uc, UC_MIPS_REG_0 + (int)rt, &prid);
        uint64_t next_pc = address + 4;
        uc_reg_write(uc, UC_MIPS_REG_PC, &next_pc);
    }
}

/*
 * Invalid instruction hook — handles VR4131 MACC instructions
 * (opcode 0x1C / SPECIAL2) which are not part of standard MIPS32.
 */
static bool insn_invalid_hook(uc_engine *uc, void *user_data)
{
    (void)user_data;
    uint64_t pc = 0;
    uc_reg_read(uc, UC_MIPS_REG_PC, &pc);

    uint32_t insn = 0;
    if (!read_insn_best_effort(uc, pc, &insn)) {
        fprintf(stderr, "[CPU] Cannot read insn at PC=0x%016" PRIX64 "\n", pc);
        return false;
    }

    /* Try MACC decode first */
    if (macc_execute(uc, insn))
        return true;

    /* Unknown instruction — log and stop */
    fprintf(stderr, "[CPU] Illegal instruction PC=0x%016" PRIX64 " insn=0x%08X\n",
            pc, insn);
    return false;
}

/* ------------------------------------------------------------------ */
/* Machine lifecycle                                                     */
/* ------------------------------------------------------------------ */

machine_t *machine_create(const machine_config_t *cfg)
{
    machine_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;

    m->cfg = *cfg;
    if (m->cfg.sdram_size == 0)
        m->cfg.sdram_size = 16u * 1024u * 1024u;   /* 16 MB default */

    /*
     * Open the Unicorn engine: MIPS64 little-endian.
     *
     * The linux4.be vmlinux uses 133k+ MIPS-III 64-bit instructions (LD, SD,
     * DADDIU, LDL, LDR, BEQL, BNEL, …).  MIPS32 mode raises Reserved Instruction
     * exceptions for all of these.  MIPS64 with the NEC VR5432 CPU model supports
     * the full MIPS-III 64-bit ISA.
     *
     * The kernel runs in 32-bit address mode (CP0 Status KX/SX/UX = 0), which
     * keeps kseg0/kseg1 segment translation identical to MIPS32.
     */
    uc_err err = uc_open(UC_ARCH_MIPS,
                         UC_MODE_MIPS64 | UC_MODE_LITTLE_ENDIAN,
                         &m->uc);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "[MACHINE] uc_open failed: %s\n", uc_strerror(err));
        free(m);
        return NULL;
    }

    /*
     * Map physical memory BEFORE setting the CPU model.
     * uc_ctl_set_cpu_model() for MIPS32_4KC creates internal QEMU machine
     * regions (including a boot-ROM stub at/near PA 0) that can overlap with
     * our SDRAM range.  By calling bus_init first we win the region claim.
     */
    bus_init(m);

    /*
     * Select the NEC VR5432 CPU model (MIPS IV, NEC VR series).
     * This is the closest available Unicorn MIPS64 model to the VR4131 and
     * supports the full MIPS-III 64-bit instruction set the kernel relies on.
     */
    uc_ctl_set_cpu_model(m->uc, UC_CPU_MIPS64_VR5432);

    /* Set CP0 Status for reset state: BEV=1 (boot vectors), ERL=1 */
    uint64_t status = 0x00400004u;
    uc_reg_write(m->uc, UC_MIPS_REG_CP0_STATUS, &status);
    bcu_init(&m->bcu);
    cmu_init(&m->cmu);
    pmu_init(&m->pmu);
    icu_init(&m->icu);
    siu_init(&m->siu);
    rtc_init(&m->rtc);
    gpio_init(&m->gpio);

    /* Always install the invalid-instruction hook (for MACC) */
    uc_hook hk;
    uc_hook_add(m->uc, &hk, UC_HOOK_INSN_INVALID,
                insn_invalid_hook, m, 1, 0);

    /*
     * PRId intercept hook — fires for every instruction, checks for
     * MFC0 $rt, $15 and substitutes VR4131_PRID (0x00000C80).
     * This hook is always installed (low per-instruction cost: one read +
     * one compare; only acts on the rare MFC0 PRId instruction).
     */
    uc_hook_add(m->uc, &hk, UC_HOOK_CODE, prid_hook, m, 1, 0);

    /* Trace hook (only when requested — high overhead) */
    if (cfg->trace)
        uc_hook_add(m->uc, &hk, UC_HOOK_CODE, trace_hook, m, 1, 0);

    /* Kernel direct-boot mode: load ELF and set entry point */
    if (cfg->kernel_path) {
        uint32_t entry_va = 0;
        if (loader_load_elf(m, cfg->kernel_path, &entry_va) != 0) {
            fprintf(stderr, "[MACHINE] Kernel ELF load failed\n");
            machine_destroy(m);
            return NULL;
        }
        m->kernel_entry = mips_sext(entry_va);

        /*
         * MIPS Linux prom_init() calling convention (linux4.be / arch/mips/vr41xx):
         *   $a0 = argc  (0 = no args, or 1 if cmdline provided)
         *   $a1 = argv  (pointer to cmdline string in low SDRAM, or 0)
         *   $a2 = 0     (envp, unused by prom_init)
         *   $a3 = 0     (reserved / memory descriptor ptr — stub 0)
         *
         * The cmdline string (if any) is placed at a well-known low-SDRAM
         * address (0x00000200) so prom_init can find it.
         */
        uint64_t a0 = 0, a1 = 0;
        if (cfg->cmdline && cfg->cmdline[0]) {
            const char *cl = cfg->cmdline;
            uint32_t   cl_pa = 0x00000200u;   /* physical backing in low SDRAM */
            uint32_t   cl_va = 0xA0000200u;   /* kseg1 virtual pointer seen by kernel */
            const char *arg0 = "be300";
            uint32_t arg0_pa = 0x00000240u;
            uint32_t arg0_va = 0xA0000240u;
            uc_mem_write(m->uc, cl_pa, cl, strlen(cl) + 1);
            uc_mem_write(m->uc, arg0_pa, arg0, strlen(arg0) + 1);
            /* Build argv[] in RAM and pass virtual address in $a1. */
            uint32_t argv_pa = 0x000001F0u;
            uint32_t argv_va = 0xA00001F0u;
            uint32_t argv_words[3] = { arg0_va, cl_va, 0 };
            uc_mem_write(m->uc, argv_pa, argv_words, sizeof(argv_words));
            a0 = 2;
            a1 = mips_sext(argv_va);
        }
        uint64_t a2 = 0, a3 = 0;
        uc_reg_write(m->uc, UC_MIPS_REG_A0, &a0);
        uc_reg_write(m->uc, UC_MIPS_REG_A1, &a1);
        uc_reg_write(m->uc, UC_MIPS_REG_A2, &a2);
        uc_reg_write(m->uc, UC_MIPS_REG_A3, &a3);

        /*
         * For direct kernel boot, relax CP0 Status to kernel mode:
         * BEV=0 (normal exception vectors), EXL=0, ERL=0, KSU=00 (kernel).
         * IM bits all enabled so the kernel can configure them itself.
         */
        uint64_t kstatus = 0x0000FF00u; /* IM7-IM0 all set, no EXL/ERL/BEV */
        uc_reg_write(m->uc, UC_MIPS_REG_CP0_STATUS, &kstatus);

        /*
         * Pre-fill BCU CLKSPEEDREG for VR4131-style encoding:
         *   - CLKSP   bits [4:0]  : core clock step
         *   - VTDIV   bits [10:8] : VT clock divisor selector (must be non-zero)
         *   - TDIVMODE bit  [12]  : 0=/2, 1=/4
         *
         * The previous value (0x004A) left VTDIV=0 and linux4.be executed a
         * guarded DIVU with divisor 0, triggering BREAK.
         */
        m->bcu.clkspeedreg = 0x1108u; /* TDIV=/4, VTDIV=1, CLKSP=8 */

        /* Verify entry-point bytes are correctly loaded */
        {
            uint32_t pa_entry = entry_va & 0x1FFFFFFFu;
            uint32_t insns[4] = {0};
            uc_mem_read(m->uc, pa_entry, insns, sizeof(insns));
            fprintf(stderr, "[MACHINE] Entry PA=0x%08X  insns: %08X %08X %08X %08X\n",
                    pa_entry, insns[0], insns[1], insns[2], insns[3]);
        }

    } else {
        /* ROM boot mode: load flat binary at reset vector */
        m->kernel_entry = mips_sext(VA_RESET_VECTOR);

        if (cfg->rom_path) {
            if (loader_load_rom(m, cfg->rom_path) != 0) {
                fprintf(stderr, "[MACHINE] ROM load failed\n");
                machine_destroy(m);
                return NULL;
            }
        }
    }

    /* Optionally preload RAM (both modes) */
    if (cfg->ram_path)
        loader_load_ram(m, cfg->ram_path);

    return m;
}

void machine_destroy(machine_t *m)
{
    if (!m) return;
    if (m->uc) uc_close(m->uc);
    free(m);
}

/*
 * Main execution loop.
 *
 * Run in batches of BATCH_SIZE instructions.  Between batches we check
 * for ICU interrupt state and advance the RTC — this is the point where
 * future interrupt injection will be added.
 *
 * Unicorn with MIPS softmmu handles TLB misses, CP0 exceptions, ERET, etc.
 * internally; they are transparent to this loop.
 */
#define BATCH_SIZE 100000u

void machine_run(machine_t *m)
{
    m->running    = true;
    m->insn_count = 0;

    fprintf(stderr, "[MACHINE] Starting execution at VA 0x%016" PRIX64 "\n",
            m->kernel_entry);

    /* Set PC to the entry point (sign-extended 64-bit VA for MIPS64 mode) */
    uc_reg_write(m->uc, UC_MIPS_REG_PC, &m->kernel_entry);

    while (m->running) {
        uint64_t pc = 0;
        uc_reg_read(m->uc, UC_MIPS_REG_PC, &pc);

        uc_err err = uc_emu_start(m->uc, pc, 0, 0, BATCH_SIZE);

        if (err != UC_ERR_OK) {
            uint64_t bad_pc = 0;
            uc_reg_read(m->uc, UC_MIPS_REG_PC, &bad_pc);
            uint64_t status = 0;
            uint64_t sp = 0, ra = 0;
            uint32_t insn = 0;
            uc_reg_read(m->uc, UC_MIPS_REG_CP0_STATUS, &status);
            uc_reg_read(m->uc, UC_MIPS_REG_SP, &sp);
            uc_reg_read(m->uc, UC_MIPS_REG_RA, &ra);
            if (!read_insn_best_effort(m->uc, bad_pc, &insn))
                insn = 0xFFFFFFFFu;
            fprintf(stderr,
                    "[MACHINE] uc_emu_start error at PC=0x%016" PRIX64 ": %s\n",
                    bad_pc, uc_strerror(err));
            fprintf(stderr,
                    "[MACHINE] State: STATUS=0x%016" PRIX64
                    " SP=0x%016" PRIX64 " RA=0x%016" PRIX64 " INSN=0x%08X\n",
                    status, sp, ra, insn);
            m->running = false;
            break;
        }

        if (!m->cfg.trace)
            m->insn_count += BATCH_SIZE;

        /* Advance RTC: treat one batch as ~1 ms worth of ticks.
         * The VR4131 RTC runs at ~32.768 kHz; 33 ticks ≈ 1 ms. */
        rtc_tick(&m->rtc, 33);
        tick_jiffies_hack(m);
    }

    fprintf(stderr, "[MACHINE] Stopped after %" PRIu64 " instructions\n",
            m->insn_count);
}

void machine_stop(machine_t *m)
{
    m->running = false;
    uc_emu_stop(m->uc);
}
