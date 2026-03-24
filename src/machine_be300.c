/*
 *  machine_be300.c — BE-300 machine setup and run loop for GXemul.
 *
 *  Replaces the old 7800-line Unicorn machine.c. GXemul handles CP0, TLB,
 *  exceptions, and address translation natively.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <inttypes.h>
#include <time.h>

#include "be300.h"
#include "loader.h"
#include "ui.h"

/* GXemul headers */
#include "cpu.h"
#include "cpu_mips.h"
#include "cop0.h"
#include "emul.h"
#include "device.h"
#include "devices.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"
#include "console.h"
#include "timer.h"
#include "settings.h"

/* GXemul externs */
extern volatile bool emul_shutdown;
extern volatile bool emul_executing;


machine_t *be300_create(const machine_config_t *cfg)
{
    static bool subsystems_initialized = false;
    machine_t *m = calloc(1, sizeof(machine_t));
    if (!m) return NULL;
    m->cfg = *cfg;

    /*
     * Initialize GXemul subsystems once per process.
     */
    if (!subsystems_initialized) {
        debugmsg_init();

        /* Initialize global settings before console_init needs them */
        extern void be300_init_global_settings(void);
        be300_init_global_settings();

        cpu_init();
        device_init();
        timer_init();
        console_init();

        /* Initialize machine type registry (registers hpcmips) */
        machine_init();
        
        subsystems_initialized = true;
    }

    /* Reset global execution flags for a new run */
    emul_executing = false;
    emul_shutdown = false;

    /*
     * Create the GXemul emul and machine objects.
     */
    m->emul = emul_new("be300");
    if (!m->emul) {
        fprintf(stderr, "[BE300] emul_new failed\n");
        free(m);
        return NULL;
    }

    struct machine *gxm = emul_add_machine(m->emul, "be300");
    if (!gxm) {
        fprintf(stderr, "[BE300] emul_add_machine failed\n");
        free(m);
        return NULL;
    }
    m->gxe_machine = gxm;

    /*
     * Configure as BE-300.
     */
    gxm->machine_type = MACHINE_HPCMIPS;
    gxm->machine_subtype = MACHINE_HPCMIPS_CASIO_BE300;
    gxm->cpu_name = strdup("VR4131");
    gxm->physical_ram_in_mb = cfg->sdram_size / (1024 * 1024);
    /* Enable prom emulation for Linux (sets up hpc_bootinfo, argc/argv),
     * but disable for NAND boot (SPL doesn't use NetBSD boot convention) */
    gxm->prom_emulation = cfg->nand_path ? 0 : 1;
    gxm->boot_kernel_filename = cfg->kernel_path ? strdup(cfg->kernel_path) : strdup("");
    gxm->boot_string_argument = cfg->cmdline ? strdup(cfg->cmdline) : strdup("");

    if (cfg->trace)
        gxm->instruction_trace = 1;

    /*
     * Manually set up the machine (adapted from emul_machine_setup).
     * We skip file_load since we use our own loader.
     */

    /* Create memory */
    uint64_t memory_amount = (uint64_t)gxm->physical_ram_in_mb * 1048576;
    gxm->memory = memory_new(memory_amount);

    /* Create CPU */
    gxm->ncpus = 1;
    gxm->cpus = malloc(sizeof(struct cpu *));
    gxm->cpus[0] = cpu_new(gxm->memory, gxm, 0, gxm->cpu_name);
    if (!gxm->cpus[0]) {
        fprintf(stderr, "[BE300] cpu_new failed for VR4131\n");
        free(m);
        return NULL;
    }
    gxm->bootstrap_cpu = 0;
    m->cpu = gxm->cpus[0];

    /*
     * Run the hpcmips machine setup function.
     * This calls dev_vr41xx_init(), adds the VRC4173 SIU (ns16550),
     * framebuffer, and configures hpc_bootinfo.
     *
     * Disable exit-on-error so missing devices (pcic, etc.) just
     * print a warning instead of crashing.
     */
    device_set_exit_on_error(0);
    machine_setup(gxm);
    device_set_exit_on_error(1);

    /*
     * Initialize our peripheral state structs.
     */
    bcu_init(&m->bcu);
    cmu_init(&m->cmu);
    pmu_init(&m->pmu);
    icu_init(&m->icu);
    siu_init(&m->siu);
    rtc_init(&m->rtc);
    gpio_init(&m->gpio);
    nand_init(&m->nand, NULL, 0);

    /*
     * Load kernel or NAND image.
     */
    if (cfg->kernel_path) {
        uint32_t entry_va = 0;
        uint32_t jiffies_pa = 0;

        if (loader_load_elf(m, cfg->kernel_path, &entry_va, &jiffies_pa) != 0) {
            fprintf(stderr, "[BE300] Failed to load kernel ELF\n");
            free(m);
            return NULL;
        }

        m->cpu->pc = (uint64_t)(int32_t)entry_va;
        fprintf(stderr, "[BE300] Kernel entry: PC=0x%08X\n", entry_va);

        if (cfg->cmdline && cfg->cmdline[0])
            fprintf(stderr, "[BE300] Kernel cmdline: %s\n", cfg->cmdline);

        if (cfg->ram_path)
            loader_load_ram(m, cfg->ram_path);

        /* Register input devices (touch + buttons) for Linux kernel boot.
         * These addresses are unclaimed for Linux boots. */
        extern void be300_register_input(struct machine *, machine_t *, bool);
        be300_register_input(gxm, m, cfg->log_mmio);

    } else if (cfg->nand_path) {
        uint32_t entry_va = 0;

        if (loader_load_nand(m, cfg->nand_path, &entry_va) != 0) {
            fprintf(stderr, "[BE300] Failed to load NAND image\n");
            free(m);
            return NULL;
        }

        m->cpu->pc = (uint64_t)(int32_t)entry_va;
        fprintf(stderr, "[BE300] NAND SPL entry: PC=0x%08X\n", entry_va);

        /* Re-initialize NAND controller with image data */
        if (m->nand_data) {
            nand_init(&m->nand, m->nand_data, m->nand_size);
        }

        /*
         * Map RAM at PA 0x1FC00000 for BEV=1 exception vectors.
         *
         * With prom_emulation=0 (NAND boot), CP0 Status has BEV=1.
         * The SPL doesn't clear BEV, so NK.exe starts with BEV=1.
         * Exception vectors (TLB refill, general, interrupt) go to
         * PA 0x1FC00000 + offset.  Real BE-300 hardware has 16K ROM
         * there (per docs/hardware.txt).  Without this RAM, exception
         * handlers can't be installed and WinCE can't set up virtual
         * memory or run the scheduler.
         */
        dev_ram_init(gxm, 0x1FC00000, 0x4000, DEV_RAM_RAM, 0, NULL);

        /*
         * Pre-fill BEV exception vectors with ERET (0x42000018) so
         * early exceptions during NK.exe init return cleanly instead
         * of executing NOPs into unmapped memory.
         *
         * BEV=1 vectors (R4000):
         *   0x000: TLB Refill (kseg0/1)
         *   0x100: Cache Error
         *   0x180: General Exception
         *   0x200: TLB Refill (64-bit, also used on 32-bit VR4131)
         *   0x280: XTLB Refill
         *   0x300: Cache Error (secondary)
         *   0x380: General Exception (alternate)
         */
        {
            /* ERET = 0x42000018 (COP0 | CO | func=ERET) */
            uint32_t eret = 0x42000018u;
            static const uint32_t vec_offsets[] = {
                0x000, 0x080, 0x100, 0x180,
                0x200, 0x280, 0x300, 0x380
            };
            for (unsigned i = 0; i < sizeof(vec_offsets)/sizeof(vec_offsets[0]); i++) {
                /* BEV=1 vectors at 0xBFC00000 only */
                uint64_t va = 0xffffffff80000000ULL |
                              (0x1FC00000ULL + vec_offsets[i]);
                store_32bit_word(m->cpu, va, eret);
                /* Normal vectors at 0x80000000 left as zeros —
                   init functions will install proper handlers */
            }
        }

        fprintf(stderr, "[BE300] Mapped 16K RAM at PA 0x1FC00000 with ERET stubs for BEV vectors\n");

        /*
         * Pre-load TLB with identity-mapped entries covering the
         * full 16MB SDRAM at both kseg2 (0xC0000000) and kuseg
         * (0x00000000).  WinCE uses kseg2 for kernel data and kuseg
         * for user-mode addresses.  Without TLB entries, every
         * access to these ranges causes a TLB refill exception which
         * sets EXL=1 permanently (no proper refill handler is
         * installed during the warm resume init path).
         *
         * Use 256KB pages: 32 entries × 2 pages × 256KB = 16MB.
         * Wire all entries to prevent TLBWR from evicting them.
         */
        /*
         * Install a minimal TLB refill handler at 0x80000000.
         * Identity maps VA → PA (PA = VA & 0x1FFFFFFF) using
         * 4KB pages.  This covers TLB misses during WinCE init
         * before the kernel installs its own handlers.
         *
         * MIPS code:
         *   mfc0 $k1, EntryHi    # get faulting VPN2
         *   srl  $k0, $k1, 7     # PFN in EntryLo position
         *   ori  $k0, $k0, 0x1F  # V=1,D=1,C=3,G=1
         *   mtc0 $k0, EntryLo0   # even page
         *   addiu $k1, $k0, 0x40 # odd page
         *   mtc0 $k1, EntryLo1
         *   tlbwr                # write TLB
         *   eret                 # return
         */
        {
            static const uint32_t tlb_handler[] = {
                0x401B5000,  /* mfc0 $k1, EntryHi    */
                0x001BD1C2,  /* srl  $k0, $k1, 7     */
                0x375A001F,  /* ori  $k0, $k0, 0x1F  */
                0x409A1000,  /* mtc0 $k0, EntryLo0   */
                0x275B0040,  /* addiu $k1, $k0, 0x40 */
                0x409B1800,  /* mtc0 $k1, EntryLo1   */
                0x42000006,  /* tlbwr                 */
                0x42000018,  /* eret                  */
            };
            for (unsigned ti = 0; ti < sizeof(tlb_handler)/4; ti++)
                store_32bit_word(m->cpu,
                    0xffffffff80000000ULL + ti * 4,
                    tlb_handler[ti]);

            /* Also install at general exception vector (0x180).
             * When EXL=1, ALL exceptions go to 0x180 instead of
             * the dedicated vectors.  TLB misses during init fire
             * here when EXL is already set from a previous exception. */
            for (unsigned ti = 0; ti < sizeof(tlb_handler)/4; ti++)
                store_32bit_word(m->cpu,
                    0xffffffff80000180ULL + ti * 4,
                    tlb_handler[ti]);

            /* Invalidate dyntrans caches so the handler code
             * is re-decoded from the updated memory.  */
            m->cpu->invalidate_translation_caches(m->cpu,
                0, /* everything */ INVALIDATE_ALL);

            fprintf(stderr, "[BE300] Installed TLB refill handler"
                " at 0x80000000 + 0x80000180 (identity map)\n");
        }

        /* Register VRC4173 latch (catch-all); pre-split to leave gaps for input device */
        extern void be300_register_vrc4173_latch(struct machine *, bool);
        be300_register_vrc4173_latch(gxm, cfg->log_mmio);

        /* Register NAND flash; pre-split to leave gap at 0x0A00A040 for input device */
        extern void be300_register_nand(struct machine *, nand_state_t *, bool);
        be300_register_nand(gxm, &m->nand, cfg->log_mmio);

        /* Register input devices AFTER latch/NAND (fills pre-carved gaps) */
        extern void be300_register_input(struct machine *, machine_t *, bool);
        be300_register_input(gxm, m, cfg->log_mmio);

    } else if (cfg->rom_path) {
        if (loader_load_rom(m, cfg->rom_path) != 0) {
            fprintf(stderr, "[BE300] Failed to load ROM image\n");
            free(m);
            return NULL;
        }
    }

    fprintf(stderr, "[BE300] Machine created: %u MB SDRAM, VR4131 CPU\n",
            cfg->sdram_size / (1024 * 1024));
    return m;
}


static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void be300_run(machine_t *m)
{
    struct machine *gxm = m->gxe_machine;
    struct emul *emul = m->emul;

    fprintf(stderr, "[BE300] Starting emulation at PC=0x%08" PRIx64 "\n",
            m->cpu->pc);

    cpu_run_init(gxm);
    m->cpu->running = true;

    timer_start();
    console_init_main(emul);

    ui_init(m);

    if (m->cfg.target_mhz > 0)
        fprintf(stderr, "[BE300] Throttle: targeting %u MHz\n", m->cfg.target_mhz);
    else
        fprintf(stderr, "[BE300] Throttle: disabled (unthrottled)\n");

    emul_executing = true;

    signal(SIGINT, SIG_DFL);

    /* Throttle state: track wall-clock origin and instruction origin for pacing */
    uint64_t throttle_target_ips  = (uint64_t)m->cfg.target_mhz * 1000000ULL;
    uint64_t throttle_wall_origin = 0;
    uint64_t throttle_instr_origin = 0;

    /*
     * Main emulation loop: machine_run() (GXemul) runs one batch
     * of ~8K instructions, then processes hardware tick functions.
     */
    int64_t last_report = 0;
    int loop_count = 0;
    while (1) {
        __sync_synchronize();
        if (emul_shutdown) {
            fprintf(stderr, "[BE300] Loop exit: emul_shutdown is true\n");
            break;
        }

        if (loop_count % 1000 == 0) {
            fprintf(stderr, "[BE300] Loop batch %d, PC=0x%08" PRIx64 "\n", loop_count, m->cpu->pc);
        }

        bool still_running = machine_run(gxm);
        if (!still_running) {
            fprintf(stderr, "[BE300] Loop exit: machine no longer running\n");
            break;
        }

        if (loop_count % 1000 == 0) {
            fprintf(stderr, "[BE300] Loop batch %d done\n", loop_count);
        }

        /* Throttle: sleep until wall time matches target CPU speed */
        if (throttle_target_ips > 0) {
            uint64_t now_ns = monotonic_ns();
            uint64_t instrs  = (uint64_t)m->cpu->ninstrs;

            if (throttle_wall_origin == 0) {
                /* First iteration: set reference point */
                throttle_wall_origin   = now_ns;
                throttle_instr_origin  = instrs;
            } else {
                uint64_t elapsed_instrs = instrs - throttle_instr_origin;
                uint64_t target_ns = (elapsed_instrs * 1000000000ULL)
                                     / throttle_target_ips;
                uint64_t actual_ns = now_ns - throttle_wall_origin;

                if (target_ns > actual_ns) {
                    uint64_t sleep_ns = target_ns - actual_ns;
                    /* Cap at 50ms to stay responsive to SDL events */
                    if (sleep_ns > 50000000ULL)
                        sleep_ns = 50000000ULL;
                    struct timespec ts = {
                        (time_t)(sleep_ns / 1000000000ULL),
                        (long)(sleep_ns % 1000000000ULL)
                    };
                    nanosleep(&ts, NULL);
                }

                /* Re-sync every ~1M instructions to prevent drift accumulation */
                if (elapsed_instrs >= 1000000ULL) {
                    throttle_wall_origin  = monotonic_ns();
                    throttle_instr_origin = instrs;
                }
            }
        }

        console_flush();
        ui_update(m);

        if (ui_should_quit(m)) {
            fprintf(stderr, "[BE300] Loop exit: ui_should_quit is true\n");
            break;
        }

        /* Periodic progress report to stderr */
        if (m->cpu->ninstrs - last_report >= 50000000LL) {
            fprintf(stderr, "[BE300] Progress: %" PRIi64 "M instrs, PC=0x%08" PRIx64 "\n",
                    m->cpu->ninstrs / 1000000LL, m->cpu->pc);
            last_report = m->cpu->ninstrs;
        }

        /*
         * WinCE cold-boot hibernate redirect.
         *
         * The OAL power-down code ends with a hibernate instruction.
         * After hibernate, the OAL has:
         *   +0x00-0x18: NOPs
         *   +0x1C-0x3C: resume path (loads saved CP0, JR to kernel)
         *   +0x40:      COLD BOOT init (JAL calls to full kernel init)
         *
         * On real hardware, the cold boot cycle is:
         *   ROM → SPL → NK.exe → OAL init → hibernate →
         *   RTC alarm wakes CPU → ROM → resume → kernel
         *
         * We shortcut: when the CPU halts at hibernate, skip 0x40
         * bytes forward to the cold boot init path.
         */
        if (m->cpu->is_halted) {
            static int cold_boot_count = 0;
            if (cold_boot_count < 5) {
                uint32_t norm = (uint32_t)m->cpu->pc & 0x1FFFFFFFu;
                if (norm >= 0x00060000u && norm < 0x00100000u) {
                    uint64_t old_pc = m->cpu->pc;
                    m->cpu->pc += 0x9C;  /* skip to warm init (JAL 0x80078BC0) at 0xA0079634 */
                    m->cpu->is_halted = false;
                    cold_boot_count++;
                    /*
                     * Set CP0 Status directly before cold boot init.
                     * Status needs: BEV=0, IE=0, EXL=0, IM=all enabled.
                     * IE stays 0 so interrupts don't fire until WinCE
                     * installs proper exception handlers and enables IE.
                     */
                    m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS] =
                        0x1000FF00u;  /* BEV=0, IM=0xFF, IE=0 */
                    m->cpu->cd.mips.coproc[0]->reg[COP0_WIRED] = 0;

                    /* Re-install TLB refill handler (may have been
                     * overwritten by NK.exe OAL init) and invalidate
                     * dyntrans caches so the new code takes effect. */
                    {
                        static const uint32_t tlb_h[] = {
                            0x401B5000, 0x001BD1C2, 0x375A001F,
                            0x409A1000, 0x275B0040, 0x409B1800,
                            0x42000006, 0x42000018,
                        };
                        for (unsigned ti = 0; ti < 8; ti++) {
                            store_32bit_word(m->cpu,
                                0xffffffff80000000ULL + ti * 4,
                                tlb_h[ti]);
                            store_32bit_word(m->cpu,
                                0xffffffff80000180ULL + ti * 4,
                                tlb_h[ti]);
                        }
                        m->cpu->invalidate_translation_caches(
                            m->cpu, 0, INVALIDATE_ALL);
                    }

                    fprintf(stderr,
                        "[BE300] Cold boot: skip hibernate+resume,"
                        " PC 0x%08" PRIx64 " → 0x%08" PRIx64
                        " Status=0x%08X\n",
                        old_pc, m->cpu->pc,
                        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS]);

                    /* Dump cold boot init function on first redirect */
                    if (cold_boot_count == 1) {
                        fprintf(stderr, "[COLD_INIT] Dumping 0x80079DF8:\n");
                        for (int i = 0; i < 128; i++) {
                            unsigned char buf[4];
                            uint64_t addr = 0xffffffff80079DF8ULL + i * 4;
                            if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                    addr, buf, 4, MEM_READ, CACHE_DATA)) {
                                uint32_t w = buf[0] | (buf[1]<<8) |
                                             (buf[2]<<16) | (buf[3]<<24);
                                fprintf(stderr, "[COLD_INIT] 0x%08" PRIx64
                                    ": %08X\n", addr, w);
                            }
                        }
                    }

                    /*
                     * Seed the CP0 save area at PA 0x00002280 with
                     * values from a real BE-300 (BEDiag hwseed dump).
                     * The warm resume code at 0x80079668 loads CP0
                     * registers from this area after the init functions
                     * have run.  Key: Status at offset 0xB0 must have
                     * BEV=0 and IE bits set for proper operation.
                     *
                     * From BEDiag resume_ctx at PA 0x00002200+0x80:
                     */
                    {
                        static const uint32_t cp0_seed[] = {
                            /* off 0x80: Index, Random, EntryLo0, EntryLo1 */
                            0x00000000, 0x00000007, 0x00000000, 0x00000000,
                            /* off 0x90: Context, PageMask, Wired, reserved */
                            0x00000000, 0x00001800, 0x00000000, 0x00000000,
                            /* off 0xA0: BadVAddr, Count, EntryHi, Compare */
                            0x00000000, 0x00000000, 0x00000000, 0x00000000,
                            /* off 0xB0: Status, Cause, EPC, PRId
                             * Status: BEV=0, IE=0 (interrupts off until
                             * WinCE installs proper exception handlers
                             * and explicitly enables interrupts).
                             * IM=0xFF (all masks ready for when IE is set).
                             */
                            0x1000FF00, 0x00000000, 0x00000000, 0x00000C80,
                            /* off 0xC0: Config, LLAddr, reserved, reserved */
                            0x00000000, 0x00000000, 0x00000000, 0x00000000,
                        };
                        uint64_t base = 0xffffffff80002280ULL;
                        for (unsigned si = 0; si < sizeof(cp0_seed)/4; si++)
                            store_32bit_word(m->cpu, base + si * 4,
                                cp0_seed[si]);
                    }

                    /* Dump exception vectors and crash area */
                    fprintf(stderr, "[EXC_DUMP] Exception vectors:\n");
                    static const uint64_t addrs[] = {
                        0xffffffff80000000ULL, /* TLB refill */
                        0xffffffff80000180ULL, /* General exception */
                        0xffffffff80000200ULL, /* TLB refill (alt) */
                        0xffffffff80000380ULL, /* General (alt) */
                        0xffffffff80002400ULL, /* crash address */
                    };
                    for (int a = 0; a < 5; a++) {
                        for (int i = 0; i < 8; i++) {
                            unsigned char buf[4];
                            uint64_t addr = addrs[a] + i * 4;
                            if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                    addr, buf, 4, MEM_READ, CACHE_DATA)) {
                                uint32_t w = buf[0] | (buf[1]<<8) |
                                             (buf[2]<<16) | (buf[3]<<24);
                                fprintf(stderr, "[EXC_DUMP] 0x%08" PRIx64
                                    ": %08X\n", addr, w);
                            }
                        }
                        fprintf(stderr, "[EXC_DUMP] ---\n");
                    }
                }
            }
        }

        /*
         * Fix stuck EXL: after WinCE kernel init installs exception
         * handlers and enters the scheduler, clear EXL and enable IE
         * so timer interrupts can fire and drive the scheduler.
         */
        /* EXL/IE management removed — rely on WinCE to set Status */

        if (++loop_count % 100 == 0) {
            // Optional: add extremely verbose logging here if needed
        }
    }

    /* Save framebuffer screenshot on exit */
    ui_save_screenshot(m);

    ui_destroy(m);

    emul_executing = false;
    cpu_run_deinit(gxm);

    fprintf(stderr, "[BE300] Emulation stopped after %" PRIi64 " instructions\n",
            m->cpu->ninstrs);
}


void be300_destroy(machine_t *m)
{
    if (!m) return;

    if (m->nand_data) {
        free(m->nand_data);
        m->nand_data = NULL;
    }

    free(m);
}
