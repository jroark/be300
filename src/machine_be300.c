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

#include "be300.h"
#include "loader.h"

/* GXemul headers */
#include "cpu.h"
#include "cpu_mips.h"
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
extern bool emul_shutdown;
extern bool emul_executing;


machine_t *be300_create(const machine_config_t *cfg)
{
    machine_t *m = calloc(1, sizeof(machine_t));
    if (!m) return NULL;
    m->cfg = *cfg;

    /*
     * Initialize GXemul subsystems.
     */
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

        /* Register VRC4173 latch (catch-all) BEFORE NAND so NAND takes priority */
        extern void be300_register_vrc4173_latch(struct machine *, bool);
        be300_register_vrc4173_latch(gxm, cfg->log_mmio);

        /* Register NAND flash as a GXemul device (overlays latch) */
        extern void be300_register_nand(struct machine *, nand_state_t *, bool);
        be300_register_nand(gxm, &m->nand, cfg->log_mmio);

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

    emul_executing = true;
    emul_shutdown = false;

    signal(SIGINT, SIG_DFL);

    /*
     * Main emulation loop: machine_run() (GXemul) runs one batch
     * of ~8K instructions, then processes hardware tick functions.
     */
    int64_t last_report = 0;
    while (!emul_shutdown) {
        bool still_running = machine_run(gxm);
        if (!still_running)
            break;

        console_flush();

        /* Periodic progress report to stderr */
        if (m->cpu->ninstrs - last_report >= 50000000LL) {
            fprintf(stderr, "[BE300] Progress: %" PRIi64 "M instrs, PC=0x%08" PRIx64 "\n",
                    m->cpu->ninstrs / 1000000LL, m->cpu->pc);
            last_report = m->cpu->ninstrs;
        }
    }

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
