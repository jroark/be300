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
#include "host_io.h"
#include "loader.h"
#include "ui.h"
#include "wince_boot.h"

/* GXemul headers */
#include "interrupt.h"
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

static void be300_serial_ring_push(machine_t *m, int ch)
{
    if (!m)
        return;

    if (m->serial_count == BE300_SERIAL_RING_CAP) {
        m->serial_tail = (m->serial_tail + 1u) % BE300_SERIAL_RING_CAP;
        m->serial_count--;
    }

    m->serial_ring[m->serial_head] = (char)ch;
    m->serial_head = (m->serial_head + 1u) % BE300_SERIAL_RING_CAP;
    m->serial_count++;
}

static void be300_serial_sink(int ch, void *user_data)
{
    be300_serial_ring_push((machine_t *)user_data, ch);
}

static void be300_register_linux_input_if_needed(machine_t *m)
{
    if (m->input_registered)
        return;

    extern void be300_register_input(struct machine *, machine_t *, bool);
    be300_register_input(m->gxe_machine, m, m->cfg.log_mmio);
    m->input_registered = true;
}

static void be300_set_linux_boot_strings(machine_t *m,
                                         const char *kernel_name,
                                         const char *cmdline)
{
    struct machine *gxm = m->gxe_machine;
    const char *safe_kernel = kernel_name ? kernel_name : "vmlinux";
    const char *safe_cmdline = cmdline ? cmdline : "";

    free(gxm->boot_kernel_filename);
    gxm->boot_kernel_filename = strdup(safe_kernel);

    free(gxm->boot_string_argument);
    gxm->boot_string_argument = strdup(safe_cmdline);

    gxm->bootstr = gxm->boot_kernel_filename;
    gxm->bootarg = gxm->boot_string_argument[0] ? gxm->boot_string_argument : NULL;
}

static void be300_refresh_linux_bootinfo(machine_t *m)
{
    struct machine *gxm = m->gxe_machine;
    uint64_t ram_top;
    uint64_t argv_base;

    if (!gxm->prom_emulation)
        return;

    ram_top = 0x80000000ULL + ((uint64_t)gxm->physical_ram_in_mb << 20);
    argv_base = ram_top - 512;

    m->cpu->cd.mips.gpr[MIPS_GPR_A0] = 1;
    m->cpu->cd.mips.gpr[MIPS_GPR_A1] = argv_base;
    m->cpu->cd.mips.gpr[MIPS_GPR_A2] = ram_top - 256;

    store_32bit_word(m->cpu, argv_base, argv_base + 16);
    store_32bit_word(m->cpu, argv_base + 4, 0);
    store_32bit_word(m->cpu, argv_base + 8, 0);
    store_string(m->cpu, argv_base + 16, gxm->boot_kernel_filename);

    if (gxm->boot_string_argument && gxm->boot_string_argument[0]) {
        m->cpu->cd.mips.gpr[MIPS_GPR_A0]++;
        store_32bit_word(m->cpu, argv_base + 4, argv_base + 64);
        store_32bit_word(m->cpu, argv_base + 8, 0);
        store_string(m->cpu, argv_base + 64, gxm->boot_string_argument);
    }
}

static int be300_boot_linux_path(machine_t *m, const char *kernel_path,
                                 const char *cmdline, const char *ram_path)
{
    uint32_t entry_va = 0;
    uint32_t jiffies_pa = 0;

    if (!kernel_path)
        return -1;

    be300_set_linux_boot_strings(m, kernel_path, cmdline);
    be300_refresh_linux_bootinfo(m);

    if (loader_load_elf(m, kernel_path, &entry_va, &jiffies_pa) != 0) {
        fprintf(stderr, "[BE300] Failed to load kernel ELF\n");
        return -1;
    }

    m->cpu->pc = (uint64_t)(int32_t)entry_va;
    m->boot_mode = BE300_BOOT_LINUX_PATH;
    fprintf(stderr, "[BE300] Kernel entry: PC=0x%08X\n", entry_va);

    if (cmdline && cmdline[0])
        fprintf(stderr, "[BE300] Kernel cmdline: %s\n", cmdline);

    if (ram_path)
        loader_load_ram(m, ram_path);

    be300_register_linux_input_if_needed(m);
    return 0;
}

static int be300_boot_linux_memory_internal(machine_t *m,
                                            const void *kernel_data,
                                            size_t kernel_size,
                                            const char *cmdline)
{
    uint32_t entry_va = 0;
    uint32_t jiffies_pa = 0;

    if (!m || !kernel_data || kernel_size == 0 || !cmdline) {
        fprintf(stderr, "[BE300] Web boot requires a kernel image\n");
        return -1;
    }
    if (m->boot_mode != BE300_BOOT_NONE) {
        fprintf(stderr, "[BE300] Machine already has boot media loaded\n");
        return -1;
    }

    be300_set_linux_boot_strings(m, "vmlinux", cmdline);
    be300_refresh_linux_bootinfo(m);

    if (loader_load_elf_from_memory(m, kernel_data, kernel_size,
                                    &entry_va, &jiffies_pa) != 0) {
        fprintf(stderr, "[BE300] Failed to load in-memory kernel ELF\n");
        return -1;
    }

    m->cpu->pc = (uint64_t)(int32_t)entry_va;
    m->boot_mode = BE300_BOOT_LINUX_MEMORY;
    fprintf(stderr, "[BE300] In-memory kernel entry: PC=0x%08X\n", entry_va);
    fprintf(stderr, "[BE300] Kernel cmdline: %s\n", cmdline);

    be300_register_linux_input_if_needed(m);
    return 0;
}


machine_t *be300_create(const machine_config_t *cfg)
{
    static bool subsystems_initialized = false;
    machine_t *m = calloc(1, sizeof(machine_t));
    if (!m) return NULL;
    m->cfg = *cfg;
    m->boot_mode = BE300_BOOT_NONE;
    m->use_builtin_ui = true;
    m->save_exit_screenshot = true;
    m->mirror_serial_to_stdout = true;
    m->fb_width = 240;
    m->fb_height = 320;
    m->fb_stride = 256;
    wince_boot_init(m);

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
        if (be300_boot_linux_path(m, cfg->kernel_path, cfg->cmdline,
                                  cfg->ram_path) != 0) {
            free(m);
            return NULL;
        }

    } else if (cfg->nand_path) {
        uint32_t entry_va = 0;

        if (loader_load_nand(m, cfg->nand_path, &entry_va) != 0) {
            fprintf(stderr, "[BE300] Failed to load NAND image\n");
            free(m);
            return NULL;
        }

        m->cpu->pc = (uint64_t)(int32_t)entry_va;
        m->boot_mode = BE300_BOOT_NAND;
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
         * Load the real BE-300 boot ROM into PA 0x1FC00000.
         * This 16KB masked ROM contains the reset vector, BEV
         * exception handlers, and utility routines that NK.exe
         * may call during initialization.  Captured from real
         * hardware via BEDiag (CRC32=0xFA3B5582).
         *
         * Falls back to ERET stubs if the ROM file is not found.
         */
        {
            const char *rom_paths[] = {
                "be300_boot_rom.bin",
                "../hardware_survey/be300_boot_rom.bin",
                NULL
            };
            FILE *rom_fp = NULL;
            for (int pi = 0; rom_paths[pi] && !rom_fp; pi++)
                rom_fp = fopen(rom_paths[pi], "rb");

            if (rom_fp) {
                uint8_t rom_buf[0x4000];
                size_t rom_read = fread(rom_buf, 1, sizeof(rom_buf), rom_fp);
                fclose(rom_fp);
                if (rom_read == sizeof(rom_buf)) {
                    uint64_t base_va = 0xffffffffBFC00000ULL;
                    for (size_t i = 0; i < sizeof(rom_buf); i += 4) {
                        uint32_t w = rom_buf[i] | (rom_buf[i+1] << 8) |
                                     (rom_buf[i+2] << 16) | (rom_buf[i+3] << 24);
                        store_32bit_word(m->cpu, base_va + i, w);
                    }
                    fprintf(stderr, "[BE300] Loaded real boot ROM"
                        " (%zu bytes) at PA 0x1FC00000\n", rom_read);
                } else {
                    fprintf(stderr, "[BE300] ROM file truncated"
                        " (%zu bytes), using ERET stubs\n", rom_read);
                    goto eret_stubs;
                }
            } else {
eret_stubs:
                ;
                uint32_t eret = 0x42000018u;
                static const uint32_t vec_offsets[] = {
                    0x000, 0x080, 0x100, 0x180,
                    0x200, 0x280, 0x300, 0x380
                };
                for (unsigned i = 0; i < sizeof(vec_offsets)/sizeof(vec_offsets[0]); i++) {
                    uint64_t va = 0xffffffff80000000ULL |
                                  (0x1FC00000ULL + vec_offsets[i]);
                    store_32bit_word(m->cpu, va, eret);
                }
                fprintf(stderr, "[BE300] Boot ROM not found,"
                    " using ERET stubs at PA 0x1FC00000\n");
            }
        }

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
        if (!cfg->wince_cold_boot) {
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
            wince_boot_install_synthetic_low_vectors(m, tlb_handler,
                sizeof(tlb_handler) / sizeof(tlb_handler[0]),
                "nand-setup");
            fprintf(stderr, "[BE300] Installed TLB refill handler"
                " at 0x80000000 + 0x80000180 (identity map)\n");
        } else {
            fprintf(stderr, "[BE300] Cold boot: skipping synthetic"
                " TLB handler (kernel installs its own)\n");
        }

        if (!cfg->wince_cold_boot) {
            wince_boot_apply_initial_seed(m);
        } else {
            /*
             * Seed the hibernate signature and flags so NK.exe's
             * pre-WAIT init takes the state-save path.  Without
             * these, the hibernate check at 0x76E68 fails and
             * NK.exe skips installing exception handlers, saving
             * CPU state, and setting up page tables.
             *
             * On a real BE-300, the factory process pre-hibernates
             * with these values set.  A true virgin cold boot never
             * happens in normal use.
             */
            /*
             * PA 0x2400: NK.exe version marker.  The init code at
             * 0x76CBC checks PA 0x2400 against 0x03020100.  If it
             * doesn't match, it clears PA 0x254C (hibernate flags).
             * NK.exe writes its own version (0x03020101) later, but
             * the check runs first, so we need the expected value.
             */
            store_32bit_word(m->cpu, 0xffffffffa0002400ULL,
                UINT32_C(0x03020100));
            store_32bit_word(m->cpu, 0xffffffffa0002524ULL,
                UINT32_C(0x32100000));
            store_32bit_word(m->cpu, 0xffffffffa000254cULL,
                UINT32_C(0x00000003));
            fprintf(stderr, "[BE300] Cold boot: seeded hibernate"
                " state (PA 0x2400=0x03020100,"
                " PA 0x2524=0x32100000,"
                " PA 0x254C=0x00000003)\n");
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
        m->input_registered = true;
        wince_boot_note_spl_handoff(m);

    } else if (cfg->rom_path) {
        if (loader_load_rom(m, cfg->rom_path) != 0) {
            fprintf(stderr, "[BE300] Failed to load ROM image\n");
            free(m);
            return NULL;
        }
        m->boot_mode = BE300_BOOT_ROM;
    }

    wince_boot_attach_machine(m);

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

static void be300_runtime_start(machine_t *m)
{
    struct machine *gxm = m->gxe_machine;
    struct emul *emul = m->emul;

    if (m->runtime_initialized)
        return;

    fprintf(stderr, "[BE300] Starting emulation at PC=0x%08" PRIx64 "\n",
            m->cpu->pc);

    cpu_run_init(gxm);
    m->cpu->running = true;

    if (m->web_mode)
        timer_reset_state();
    else
        timer_start();
    console_init_main(emul);

    if (m->use_builtin_ui)
        ui_init(m);

    if (m->cfg.target_mhz > 0) {
        if (m->web_mode)
            fprintf(stderr, "[BE300] Web pacing target: %u MHz-equivalent\n",
                    m->cfg.target_mhz);
        else
            fprintf(stderr, "[BE300] Throttle: targeting %u MHz\n",
                    m->cfg.target_mhz);
    } else {
        fprintf(stderr, "[BE300] Throttle: disabled (unthrottled)\n");
    }

    m->throttle_target_ips = (uint64_t)m->cfg.target_mhz * 1000000ULL;
    m->throttle_wall_origin = 0;
    m->throttle_instr_origin = 0;
    m->last_report = 0;
    m->loop_count = 0;
    m->runtime_initialized = true;
    m->runtime_stopped = false;
    m->runtime_finalized = false;
    emul_executing = true;

    if (!m->web_mode)
        signal(SIGINT, SIG_DFL);

    host_io_set_serial_sink(be300_serial_sink, m);
    host_io_set_stdout_enabled(m->mirror_serial_to_stdout);
}

static void be300_runtime_finalize(machine_t *m)
{
    if (!m || !m->runtime_initialized || m->runtime_finalized)
        return;

    if (m->save_exit_screenshot)
        ui_save_screenshot(m);

    if (m->use_builtin_ui)
        ui_destroy(m);

    timer_stop();
    host_io_set_serial_sink(NULL, NULL);
    host_io_set_stdout_enabled(true);

    emul_executing = false;
    cpu_run_deinit(m->gxe_machine);
    m->runtime_stopped = true;
    m->runtime_finalized = true;

    fprintf(stderr, "[BE300] Emulation stopped after %" PRIi64 " instructions\n",
            m->cpu->ninstrs);
}

static bool be300_run_batch(machine_t *m)
{
    struct machine *gxm = m->gxe_machine;

    __sync_synchronize();
    if (emul_shutdown) {
        fprintf(stderr, "[BE300] Loop exit: emul_shutdown is true\n");
        return false;
    }

    if (m->loop_count % 1000 == 0) {
        fprintf(stderr, "[BE300] Loop batch %d, PC=0x%08" PRIx64 "\n",
                m->loop_count, m->cpu->pc);
    }

    /* Detect user-mode execution (PC in kuseg: 0x00000000-0x7FFFFFFF) */
    {
        static int usermode_logged = 0;
        uint64_t pc = m->cpu->pc;
        if (!usermode_logged && pc < 0x80000000ULL && pc > 0x1000ULL) {
            fprintf(stderr, "[BE300] *** USER MODE DETECTED: PC=0x%08" PRIx64 " ***\n", pc);
            usermode_logged = 1;
        }
    }

    if (!machine_run(gxm)) {
        wince_boot_note_fatal_stop(m, "machine-no-longer-running");
        fprintf(stderr, "[BE300] Loop exit: machine no longer running\n");

        /* Dump full CPU state at crash point */
        if (m->cfg.wince_cold_boot) {
            uint32_t pc = (uint32_t)m->cpu->pc;
            fprintf(stderr, "[COLD_CRASH] PC=0x%08X\n", pc);
            fprintf(stderr, "[COLD_CRASH] CP0: Status=0x%08X"
                " Cause=0x%08X EPC=0x%08X BadVA=0x%08X\n",
                (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS],
                (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE],
                (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC],
                (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR]);
            fprintf(stderr, "[COLD_CRASH] GPR:"
                " at=%08X v0=%08X v1=%08X a0=%08X\n",
                (uint32_t)m->cpu->cd.mips.gpr[1],
                (uint32_t)m->cpu->cd.mips.gpr[2],
                (uint32_t)m->cpu->cd.mips.gpr[3],
                (uint32_t)m->cpu->cd.mips.gpr[4]);
            fprintf(stderr, "[COLD_CRASH] GPR:"
                " t0=%08X t1=%08X t2=%08X t3=%08X\n",
                (uint32_t)m->cpu->cd.mips.gpr[8],
                (uint32_t)m->cpu->cd.mips.gpr[9],
                (uint32_t)m->cpu->cd.mips.gpr[10],
                (uint32_t)m->cpu->cd.mips.gpr[11]);
            fprintf(stderr, "[COLD_CRASH] GPR:"
                " sp=%08X fp=%08X ra=%08X gp=%08X\n",
                (uint32_t)m->cpu->cd.mips.gpr[29],
                (uint32_t)m->cpu->cd.mips.gpr[30],
                (uint32_t)m->cpu->cd.mips.gpr[31],
                (uint32_t)m->cpu->cd.mips.gpr[28]);
            fprintf(stderr, "[COLD_CRASH] GPR:"
                " s0=%08X s1=%08X s2=%08X s3=%08X"
                " s4=%08X s5=%08X s6=%08X s7=%08X\n",
                (uint32_t)m->cpu->cd.mips.gpr[16],
                (uint32_t)m->cpu->cd.mips.gpr[17],
                (uint32_t)m->cpu->cd.mips.gpr[18],
                (uint32_t)m->cpu->cd.mips.gpr[19],
                (uint32_t)m->cpu->cd.mips.gpr[20],
                (uint32_t)m->cpu->cd.mips.gpr[21],
                (uint32_t)m->cpu->cd.mips.gpr[22],
                (uint32_t)m->cpu->cd.mips.gpr[23]);

            /* Dump instructions around crash PC */
            fprintf(stderr, "[COLD_CRASH] Code around PC:\n");
            for (int i = -4; i < 8; i++) {
                unsigned char buf[4];
                uint64_t va = (uint64_t)(int32_t)pc + i * 4;
                if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                        va, buf, 4, MEM_READ, CACHE_DATA)) {
                    uint32_t w = buf[0] | (buf[1]<<8) |
                                 (buf[2]<<16) | (buf[3]<<24);
                    fprintf(stderr, "[COLD_CRASH]   0x%08" PRIx64
                        ": %08X%s\n", va, w,
                        (int64_t)va == (int64_t)(int32_t)pc
                            ? "  <-- PC" : "");
                }
            }
        }

        return false;
    }

    if (m->web_mode)
        timer_tick_manual();

    if (m->loop_count % 1000 == 0) {
        fprintf(stderr, "[BE300] Loop batch %d done\n", m->loop_count);
    }

    if (!m->web_mode && m->throttle_target_ips > 0) {
        uint64_t now_ns = monotonic_ns();
        uint64_t instrs  = (uint64_t)m->cpu->ninstrs;

        if (m->throttle_wall_origin == 0) {
            m->throttle_wall_origin = now_ns;
            m->throttle_instr_origin = instrs;
        } else {
            uint64_t elapsed_instrs = instrs - m->throttle_instr_origin;
            uint64_t target_ns = (elapsed_instrs * 1000000000ULL)
                                 / m->throttle_target_ips;
            uint64_t actual_ns = now_ns - m->throttle_wall_origin;

            if (target_ns > actual_ns) {
                uint64_t sleep_ns = target_ns - actual_ns;
                struct timespec ts;

                if (sleep_ns > 50000000ULL)
                    sleep_ns = 50000000ULL;

                ts.tv_sec = (time_t)(sleep_ns / 1000000000ULL);
                ts.tv_nsec = (long)(sleep_ns % 1000000000ULL);
                nanosleep(&ts, NULL);
            }

            if (elapsed_instrs >= 1000000ULL) {
                m->throttle_wall_origin = monotonic_ns();
                m->throttle_instr_origin = instrs;
            }
        }
    }

    console_flush();
    if (m->use_builtin_ui)
        ui_update(m);

    if (m->use_builtin_ui && ui_should_quit(m)) {
        fprintf(stderr, "[BE300] Loop exit: ui_should_quit is true\n");
        return false;
    }

    if (m->cpu->ninstrs - m->last_report >= 50000000LL) {
        fprintf(stderr, "[BE300] Progress: %" PRIi64 "M instrs, PC=0x%08" PRIx64 "\n",
                m->cpu->ninstrs / 1000000LL, m->cpu->pc);
        m->last_report = m->cpu->ninstrs;
    }

    if (m->cpu->is_halted) {
        if (m->cfg.wince_cold_boot) {
            uint32_t wait_pc = (uint32_t)m->cpu->pc;
            uint32_t norm = wait_pc & 0x1FFFFFFFu;

            if (!m->wince.cold_boot_wait_logged) {
                /* First WAIT: skip past it to start OAL init */
                m->cpu->is_halted = false;
                m->cpu->pc += 4;
                m->wince.cold_boot_wait_logged = true;
                fprintf(stderr,
                    "[COLD_BOOT] WAIT at PC=0x%08X (PA=0x%08X),"
                    " skipping to 0x%08" PRIx64 "\n",
                    wait_pc, norm, m->cpu->pc);

                /*
                 * Save live CPU state to the resume_ctx table at
                 * PA 0x2200 so the OAL GPR/CP0 restore at 0x79668
                 * gets valid values instead of zeros.
                 *
                 * CP0 offset mapping (from disassembly of 0x79670-0x79714):
                 *   0x80:Index 0x84:Random 0x88:EntryLo0 0x8C:EntryLo1
                 *   0x90:Context 0x94:PageMask 0x98:Wired 0x9C:Count
                 *   0xA0:EntryHi 0xA4:Compare 0xA8:STATUS 0xAC:Cause
                 *   0xB0:EPC 0xB4:Config 0xB8:LLAddr ...
                 * Status is loaded LAST (at 0x19714) for atomicity.
                 *
                 * GPR mapping: 0x00-0x18:$at-$a3, 0x1C-0x68:$t1-$gp
                 * (skips $t0), 0x6C:SP, 0x70:FP, 0x74:RA, 0x78:HI, 0x7C:LO
                 */
                {
                    uint64_t base = 0xffffffffa0002200ULL;
                    /* GPR 1-7 (at through a3) */
                    for (int r = 1; r <= 7; r++)
                        store_32bit_word(m->cpu, base + (r-1)*4,
                            (uint32_t)m->cpu->cd.mips.gpr[r]);
                    /* GPR 9-28 (t1 through gp, skipping t0) */
                    for (int r = 9; r <= 28; r++)
                        store_32bit_word(m->cpu, base + 0x1C + (r-9)*4,
                            (uint32_t)m->cpu->cd.mips.gpr[r]);
                    /* SP, FP, RA */
                    store_32bit_word(m->cpu, base + 0x6C,
                        (uint32_t)m->cpu->cd.mips.gpr[29]);
                    store_32bit_word(m->cpu, base + 0x70,
                        (uint32_t)m->cpu->cd.mips.gpr[30]);
                    store_32bit_word(m->cpu, base + 0x74,
                        (uint32_t)m->cpu->cd.mips.gpr[31]);
                    /* HI, LO */
                    store_32bit_word(m->cpu, base + 0x78,
                        (uint32_t)m->cpu->cd.mips.hi);
                    store_32bit_word(m->cpu, base + 0x7C,
                        (uint32_t)m->cpu->cd.mips.lo);
                    /* CP0 Status at offset 0xA8.
                       - Clear BEV (bit 22) so exceptions go to PA 0x0000
                         (our synthetic handler) instead of the boot ROM's
                         MIPS16 handlers that GXemul can't execute
                       - Set IE=1 and IM[7]=1 so timer interrupt can wake
                         the CPU from subsequent WAITs */
                    {
                        uint32_t status =
                            (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS];
                        status &= ~(1u << 22);  /* clear BEV */
                        status |= 0x8001u;       /* IE=1, IM[7]=1 */
                        store_32bit_word(m->cpu, base + 0xA8, status);
                    }
                    /* CP0 EPC at offset 0xB0 */
                    store_32bit_word(m->cpu, base + 0xB0,
                        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC]);
                    /* CP0 Config at offset 0xB4 */
                    store_32bit_word(m->cpu, base + 0xB4,
                        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CONFIG]);
                    /* Push $t0 onto stack for the LW $t0,0($sp) epilogue */
                    {
                        uint32_t sp = (uint32_t)m->cpu->cd.mips.gpr[29];
                        if (sp >= 4) {
                            store_32bit_word(m->cpu,
                                0xffffffff80000000ULL | (sp - 4),
                                (uint32_t)m->cpu->cd.mips.gpr[8]);
                            store_32bit_word(m->cpu, base + 0x6C, sp - 4);
                        }
                    }

                    /* Install synthetic TLB handler at PA 0x0000 and
                       0x0180 — the OAL restore sets BEV=0, so TLB
                       misses go here, not to the boot ROM. */
                    {
                        static const uint32_t tlb_h[] = {
                            0x401B5000, 0x001BD1C2, 0x375A001F,
                            0x409A1000, 0x275B0040, 0x409B1800,
                            0x42000006, 0x42000018,
                        };
                        for (unsigned j = 0; j < 8; j++) {
                            store_32bit_word(m->cpu,
                                0xffffffff80000000ULL + j*4, tlb_h[j]);
                            store_32bit_word(m->cpu,
                                0xffffffff80000180ULL + j*4, tlb_h[j]);
                        }
                    }

                    fprintf(stderr, "[COLD_BOOT] Saved live state"
                        " + TLB handler (SP=0x%08X RA=0x%08X"
                        " Status=0x%08X)\n",
                        (uint32_t)m->cpu->cd.mips.gpr[29],
                        (uint32_t)m->cpu->cd.mips.gpr[31],
                        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS]);
                }

                /* Dump decompressed NK.exe binary for analysis */
                {
                    const uint32_t nk_pa = 0x00060000u;
                    const uint32_t nk_size = 0x005F6AC8u;
                    FILE *nk_fp = fopen("nk_decompressed.bin", "wb");
                    if (nk_fp) {
                        uint8_t page[4096];
                        uint32_t off = 0;
                        while (off < nk_size) {
                            uint32_t chunk = nk_size - off;
                            if (chunk > sizeof(page)) chunk = sizeof(page);
                            uint64_t va = 0xffffffff80000000ULL | (nk_pa + off);
                            if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                    va, page, chunk, MEM_READ, CACHE_DATA))
                                fwrite(page, 1, chunk, nk_fp);
                            else
                                break;
                            off += chunk;
                        }
                        fclose(nk_fp);
                        fprintf(stderr,
                            "[COLD_BOOT] Dumped NK.exe (%u bytes)"
                            " to nk_decompressed.bin\n", off);
                    }
                }

                /* Dump instructions around WAIT for disassembly */
                fprintf(stderr, "[COLD_BOOT] Code around WAIT:\n");
                for (int i = -8; i <= 40; i++) {
                    unsigned char buf[4];
                    uint64_t va = (uint64_t)(int32_t)wait_pc + i * 4;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            va, buf, 4, MEM_READ, CACHE_DATA)) {
                        uint32_t w = buf[0] | (buf[1]<<8) |
                                     (buf[2]<<16) | (buf[3]<<24);
                        fprintf(stderr, "[COLD_BOOT]   0x%08" PRIx64
                            ": %08X%s\n", va, w,
                            (int64_t)va == (int64_t)(int32_t)wait_pc
                                ? "  <-- WAIT" : "");
                    }
                }

                /* Dump CP0 table at PA 0x2200 (resume_ctx) */
                fprintf(stderr, "[COLD_BOOT] resume_ctx at PA 0x2200:\n");
                for (int i = 0; i < 64; i++) {
                    unsigned char buf[4];
                    uint64_t va = 0xffffffffa0002200ULL + i * 4;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            va, buf, 4, MEM_READ, CACHE_DATA)) {
                        uint32_t w = buf[0] | (buf[1]<<8) |
                                     (buf[2]<<16) | (buf[3]<<24);
                        if (w != 0)
                            fprintf(stderr, "[COLD_BOOT]   PA 0x%04X"
                                ": %08X\n", 0x2200 + i * 4, w);
                    }
                }

                /* Dump exception vectors at PA 0x0000 */
                fprintf(stderr, "[COLD_BOOT] Low vectors (PA 0x0000):\n");
                for (int i = 0; i < 16; i++) {
                    unsigned char buf[4];
                    uint64_t va = 0xffffffff80000000ULL + i * 4;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            va, buf, 4, MEM_READ, CACHE_DATA)) {
                        uint32_t w = buf[0] | (buf[1]<<8) |
                                     (buf[2]<<16) | (buf[3]<<24);
                        if (w != 0)
                            fprintf(stderr, "[COLD_BOOT]   PA 0x%04X"
                                ": %08X\n", i * 4, w);
                    }
                }

                /* Dump CP0 state */
                fprintf(stderr, "[COLD_BOOT] CP0: Status=0x%08X"
                    " Cause=0x%08X EPC=0x%08X\n",
                    (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS],
                    (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE],
                    (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC]);
                fprintf(stderr, "[COLD_BOOT] GPR: SP=0x%08X"
                    " RA=0x%08X S0=0x%08X\n",
                    (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_SP],
                    (uint32_t)m->cpu->cd.mips.gpr[MIPS_GPR_RA],
                    (uint32_t)m->cpu->cd.mips.gpr[16]);

                /* Dump memory the three check functions will read */
                fprintf(stderr, "[COLD_BOOT] Check data:\n");
                /* PA 0x250C (check 3 arg) */
                {
                    unsigned char buf[4];
                    uint64_t va = 0xffffffffa000250cULL;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            va, buf, 4, MEM_READ, CACHE_DATA)) {
                        uint32_t w = buf[0] | (buf[1]<<8) |
                                     (buf[2]<<16) | (buf[3]<<24);
                        fprintf(stderr, "[COLD_BOOT]   PA 0x250C"
                            " (check3 arg target): %08X\n", w);
                    }
                }
                /* Dump PA 0x2500-0x2520 for context */
                for (int i = 0; i < 8; i++) {
                    unsigned char buf[4];
                    uint64_t va = 0xffffffffa0002500ULL + i * 4;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            va, buf, 4, MEM_READ, CACHE_DATA)) {
                        uint32_t w = buf[0] | (buf[1]<<8) |
                                     (buf[2]<<16) | (buf[3]<<24);
                        if (w != 0)
                            fprintf(stderr, "[COLD_BOOT]   PA 0x%04X"
                                ": %08X\n", 0x2500 + i * 4, w);
                    }
                }

                /*
                 * Arm PC-based probes for the three check return points.
                 * We'll log when we hit the BNE/BEQ after each check.
                 */
                /* Dump the three check function prologues */
                fprintf(stderr, "[COLD_BOOT] check1 @ 0x80079AC4:\n");
                for (int i = 0; i < 24; i++) {
                    unsigned char buf[4];
                    uint64_t va = 0xffffffff80079AC4ULL + i * 4;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            va, buf, 4, MEM_READ, CACHE_DATA)) {
                        uint32_t w = buf[0] | (buf[1]<<8) |
                                     (buf[2]<<16) | (buf[3]<<24);
                        fprintf(stderr, "[COLD_BOOT]   0x%08" PRIx64
                            ": %08X\n", va, w);
                    }
                }
                fprintf(stderr, "[COLD_BOOT] check2 @ 0x8007AFA8:\n");
                for (int i = 0; i < 24; i++) {
                    unsigned char buf[4];
                    uint64_t va = 0xffffffff8007AFA8ULL + i * 4;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            va, buf, 4, MEM_READ, CACHE_DATA)) {
                        uint32_t w = buf[0] | (buf[1]<<8) |
                                     (buf[2]<<16) | (buf[3]<<24);
                        fprintf(stderr, "[COLD_BOOT]   0x%08" PRIx64
                            ": %08X\n", va, w);
                    }
                }

                /*
                 * Dump code at the last known good PC range before
                 * the crash (0x80079750+) to trace control flow.
                 */
                fprintf(stderr, "[COLD_BOOT] Code at 0x80079750"
                    " (post-ICU, before crash):\n");
                for (int i = 0; i < 32; i++) {
                    unsigned char buf[4];
                    uint64_t va = 0xffffffff80079750ULL + i * 4;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            va, buf, 4, MEM_READ, CACHE_DATA)) {
                        uint32_t w = buf[0] | (buf[1]<<8) |
                                     (buf[2]<<16) | (buf[3]<<24);
                        fprintf(stderr, "[COLD_BOOT]   0x%08" PRIx64
                            ": %08X\n", va, w);
                    }
                }

                /* Also dump what's at/around PA 0x2400 (crash site) */
                fprintf(stderr, "[COLD_BOOT] Code/data at crash"
                    " site 0x80002400:\n");
                for (int i = -4; i < 8; i++) {
                    unsigned char buf[4];
                    uint64_t va = 0xffffffff80002400ULL + i * 4;
                    if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                            va, buf, 4, MEM_READ, CACHE_DATA)) {
                        uint32_t w = buf[0] | (buf[1]<<8) |
                                     (buf[2]<<16) | (buf[3]<<24);
                        fprintf(stderr, "[COLD_BOOT]   PA 0x%04X"
                            ": %08X\n",
                            0x2400 + i * 4, w);
                    }
                }
            } else {
                /*
                 * Subsequent WAITs: the OAL init restored Status
                 * with IE=0 (from the zeroed resume_ctx table), so
                 * the timer can't wake the CPU.  Fix Status to
                 * enable interrupts, then leave halted.  The pending
                 * timer interrupt (Cause IP[7] already set) will
                 * immediately fire and wake the CPU via GXemul's
                 * interrupt mechanism.
                 */
                m->wince.cold_boot_wait_count++;
                {
                    uint32_t status =
                        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS];
                    if (!(status & 0x1u)) {
                        /* IE not set — enable it plus IM[7] for timer */
                        status |= 0x8001u;
                        m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS] =
                            status;
                    }
                }
                if (m->wince.cold_boot_wait_count <= 5) {
                    fprintf(stderr,
                        "[COLD_BOOT] WAIT #%u at PC=0x%08" PRIx64
                        ", halted with IE=1"
                        " (Status=0x%08X Cause=0x%08X"
                        " EPC=0x%08X BadVA=0x%08X)\n",
                        m->wince.cold_boot_wait_count + 1,
                        m->cpu->pc,
                        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS],
                        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_CAUSE],
                        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_EPC],
                        (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_BADVADDR]);
                }
                /* On first subsequent WAIT, dump the exception handlers
                   that OAL init installed at PA 0x0000 */
                if (m->wince.cold_boot_wait_count == 1) {
                    fprintf(stderr,
                        "[COLD_BOOT] Exception handlers after"
                        " OAL init:\n");
                    fprintf(stderr, "[COLD_BOOT] TLB Refill"
                        " (PA 0x0000):\n");
                    for (int i = 0; i < 32; i++) {
                        unsigned char buf[4];
                        uint64_t va = 0xffffffff80000000ULL + i*4;
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                va, buf, 4, MEM_READ, CACHE_DATA)) {
                            uint32_t w = buf[0] | (buf[1]<<8) |
                                         (buf[2]<<16) | (buf[3]<<24);
                            fprintf(stderr, "[COLD_BOOT]   0x%04X"
                                ": %08X\n", i * 4, w);
                        }
                    }
                    fprintf(stderr, "[COLD_BOOT] General Exception"
                        " (PA 0x0180):\n");
                    for (int i = 0; i < 32; i++) {
                        unsigned char buf[4];
                        uint64_t va = 0xffffffff80000180ULL + i*4;
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                va, buf, 4, MEM_READ, CACHE_DATA)) {
                            uint32_t w = buf[0] | (buf[1]<<8) |
                                         (buf[2]<<16) | (buf[3]<<24);
                            fprintf(stderr, "[COLD_BOOT]   0x%04X"
                                ": %08X\n", 0x180 + i * 4, w);
                        }
                    }
                    /* Dump resume_ctx to see what OAL wrote */
                    fprintf(stderr, "[COLD_BOOT] resume_ctx"
                        " after OAL init:\n");
                    for (int i = 0; i < 64; i++) {
                        unsigned char buf[4];
                        uint64_t va = 0xffffffffa0002200ULL + i*4;
                        if (m->cpu->memory_rw(m->cpu, m->cpu->mem,
                                va, buf, 4, MEM_READ, CACHE_DATA)) {
                            uint32_t w = buf[0] | (buf[1]<<8) |
                                         (buf[2]<<16) | (buf[3]<<24);
                            if (w != 0)
                                fprintf(stderr,
                                    "[COLD_BOOT]   PA 0x%04X"
                                    ": %08X\n", 0x2200 + i*4, w);
                        }
                    }
                }
            }
        } else if (m->wince.hibernate_redirect_count < 5) {
            uint32_t norm = (uint32_t)m->cpu->pc & 0x1FFFFFFFu;
            if (norm >= 0x00060000u && norm < 0x00100000u) {
                uint64_t old_pc = m->cpu->pc;
                static const uint32_t tlb_h[] = {
                    0x401B5000, 0x001BD1C2, 0x375A001F,
                    0x409A1000, 0x275B0040, 0x409B1800,
                    0x42000006, 0x42000018,
                };

                wince_boot_install_synthetic_low_vectors(m, tlb_h,
                    sizeof(tlb_h) / sizeof(tlb_h[0]),
                    m->cfg.wince_resume_replay
                        ? "resume-replay"
                        : "cold-boot-redirect");

                if (m->cfg.wince_resume_replay) {
                    uint32_t target_pc = 0;
                    uint32_t target_sp = 0;

                    if (!wince_boot_prepare_resume_replay(m,
                        (uint32_t)old_pc, &target_pc, &target_sp)) {
                        wince_boot_note_fatal_stop(m,
                            "resume-replay-prepare-failed");
                        fprintf(stderr,
                            "[BE300] Resume replay prepare failed"
                            " at halt PC=0x%08" PRIx64 "\n",
                            old_pc);
                        return false;
                    }

                    m->cpu->pc = (uint64_t)(int32_t)target_pc;
                    if (target_sp != 0)
                        m->cpu->cd.mips.gpr[MIPS_GPR_SP] = target_sp;
                    m->cpu->cd.mips.coproc[0]->reg[COP0_EPC] =
                        m->wince.replay_resume_target_pc != 0
                            ? m->wince.replay_resume_target_pc
                            : target_pc;
                    m->cpu->is_halted = false;
                    m->wince.hibernate_redirect_count++;

                    fprintf(stderr,
                        "[BE300] Resume replay: PC 0x%08" PRIx64
                        " -> 0x%08X EPC=0x%08X SP=%s0x%08X\n",
                        old_pc,
                        target_pc,
                        m->wince.replay_resume_target_pc != 0
                            ? m->wince.replay_resume_target_pc
                            : target_pc,
                        target_sp != 0 ? "" : "(keep) ",
                        target_sp);
                    wince_boot_note_cold_boot_redirect(
                        m, "hibernate-to-resume-replay");
                    return true;
                }

                m->cpu->pc += 0x9C;
                m->cpu->is_halted = false;
                m->wince.hibernate_redirect_count++;
                m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS] = 0x1000FF00u;
                m->cpu->cd.mips.coproc[0]->reg[COP0_WIRED] = 0;

                fprintf(stderr,
                    "[BE300] Cold boot: skip hibernate+resume,"
                    " PC 0x%08" PRIx64 " → 0x%08" PRIx64
                    " Status=0x%08X\n",
                    old_pc, m->cpu->pc,
                    (uint32_t)m->cpu->cd.mips.coproc[0]->reg[COP0_STATUS]);

                if (m->cfg.log_wince_stall
                    && m->wince.hibernate_redirect_count == 1) {
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

                {
                    static const uint32_t cp0_seed[] = {
                        0x00000000, 0x00000007, 0x00000000, 0x00000000,
                        0x00000000, 0x00001800, 0x00000000, 0x00000000,
                        0x00000000, 0x00000000, 0x00000000, 0x00000000,
                        0x1000FF00, 0x00000000, 0x00000000, 0x00000C80,
                        0x00000000, 0x00000000, 0x00000000, 0x00000000,
                    };
                    uint64_t base = 0xffffffff80002280ULL;
                    for (unsigned si = 0; si < sizeof(cp0_seed)/4; si++)
                        store_32bit_word(m->cpu, base + si * 4,
                            cp0_seed[si]);
                }
                wince_boot_apply_resume_seed(m);
                wince_boot_note_cold_boot_redirect(
                    m, "hibernate-to-cold-boot");
            }
        }
    }

    if (++m->loop_count % 100 == 0) {
        /* keep hook point for extremely verbose diagnostics */
    }

    return true;
}

int be300_step(machine_t *m, uint32_t max_batches)
{
    if (!m)
        return -1;
    if (m->boot_mode == BE300_BOOT_NONE) {
        fprintf(stderr, "[BE300] Cannot run before boot media is loaded\n");
        return -1;
    }
    if (m->runtime_finalized)
        return 0;
    if (max_batches == 0)
        max_batches = 1;

    be300_runtime_start(m);

    for (uint32_t i = 0; i < max_batches; i++) {
        if (!be300_run_batch(m)) {
            be300_runtime_finalize(m);
            return 0;
        }
    }

    return 1;
}

void be300_run(machine_t *m)
{
    while (be300_step(m, 1) > 0) {
    }
}

machine_t *be300_create_web(uint32_t sdram_mb, uint32_t target_mhz,
                            bool sfb_5bit_green)
{
    machine_config_t cfg = {
        .trace = false,
        .log_mmio = false,
        .sfb_5bit_green = sfb_5bit_green,
        .log_nand_legacy = false,
        .log_wince_stall = false,
        .wince_hw_seed = false,
        .wince_resume_replay = false,
        .wince_resume_replay_full = false,
        .rom_path = NULL,
        .kernel_path = NULL,
        .cmdline = NULL,
        .ram_path = NULL,
        .nand_path = NULL,
        .sdram_size = (sdram_mb ? sdram_mb : 16u) * 1024u * 1024u,
        .target_mhz = target_mhz,
    };
    machine_t *m = be300_create(&cfg);
    if (!m)
        return NULL;

    m->web_mode = true;
    m->use_builtin_ui = false;
    m->save_exit_screenshot = false;
    m->mirror_serial_to_stdout = false;
    return m;
}

int be300_boot_linux_from_memory(machine_t *m,
                                 const void *kernel_data,
                                 size_t kernel_size,
                                 const char *cmdline)
{
    return be300_boot_linux_memory_internal(m, kernel_data, kernel_size, cmdline);
}

int be300_copy_frame_rgba8888(machine_t *m, uint8_t *dst, size_t dst_len,
                              uint32_t *width_out, uint32_t *height_out)
{
    const uint16_t *src;
    uint32_t width;
    uint32_t height;

    if (!m || !dst || !width_out || !height_out)
        return -1;

    width = m->fb_width ? m->fb_width : 240u;
    height = m->fb_height ? m->fb_height : 320u;
    if (dst_len < (size_t)width * height * 4u)
        return -1;

    if (!m->fb_data) {
        if (!m->gxe_machine || !m->gxe_machine->fb || !m->gxe_machine->fb->framebuffer)
            return 0;
        m->fb_data = m->gxe_machine->fb->framebuffer;
    }

    src = (const uint16_t *)m->fb_data;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint16_t pixel = src[y * m->fb_stride + x];
            uint32_t r = (pixel >> 11) & 0x1Fu;
            uint32_t b = pixel & 0x1Fu;
            uint32_t g;
            size_t off = ((size_t)y * width + x) * 4u;

            if (m->cfg.sfb_5bit_green) {
                g = (pixel >> 5) & 0x1Fu;
                g = (g << 3) | (g >> 2);
            } else {
                g = (pixel >> 5) & 0x3Fu;
                g = (g << 2) | (g >> 4);
            }

            r = (r << 3) | (r >> 2);
            b = (b << 3) | (b >> 2);

            dst[off + 0] = (uint8_t)r;
            dst[off + 1] = (uint8_t)g;
            dst[off + 2] = (uint8_t)b;
            dst[off + 3] = 0xFFu;
        }
    }

    *width_out = width;
    *height_out = height;
    return 1;
}

size_t be300_drain_serial(machine_t *m, char *dst, size_t dst_len)
{
    size_t count = 0;

    if (!m || !dst || dst_len == 0)
        return 0;

    while (count < dst_len && m->serial_count > 0) {
        dst[count++] = m->serial_ring[m->serial_tail];
        m->serial_tail = (m->serial_tail + 1u) % BE300_SERIAL_RING_CAP;
        m->serial_count--;
    }

    return count;
}

void be300_set_touch(machine_t *m, bool down, uint16_t x, uint16_t y)
{
    if (!m)
        return;
    m->touch_down = down;
    m->touch_x = x;
    m->touch_y = y;
    __sync_synchronize();
}

void be300_set_buttons(machine_t *m, uint8_t btn_set1, uint8_t btn_set2)
{
    if (!m)
        return;
    m->btn_set1 = btn_set1;
    m->btn_set2 = btn_set2;
    __sync_synchronize();
}

void be300_stop(machine_t *m)
{
    (void)m;
    emul_shutdown = true;
    __sync_synchronize();
}


void be300_destroy(machine_t *m)
{
    if (!m) return;

    be300_stop(m);
    be300_runtime_finalize(m);

    wince_boot_detach_machine(m);

    if (m->nand_data) {
        free(m->nand_data);
        m->nand_data = NULL;
    }

    free(m);

    /*
     * Clear GXemul global state so that be300_create() can be called again.
     * interrupt_handler_register() aborts with exit(1) on duplicate names,
     * and timer_add() appends to a global list — both must be reset between
     * machine instantiations (e.g. kernel switch from the Android UI).
     */
    interrupt_handler_clear_all();
    timer_remove_all();
}
