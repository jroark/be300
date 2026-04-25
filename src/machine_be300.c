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
#include "be300_probe.h"
#include "host_io.h"
#include "ppsh.h"
#include "ui.h"

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

static void be300_handle_stop_signal(int signum)
{
    (void)signum;
    emul_shutdown = true;
    __sync_synchronize();
}

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

static void be300_ppsh_poll_host_input(machine_t *m)
{
    unsigned char raw[128];
    uint8_t cooked[256];
    size_t raw_len;
    size_t cooked_len = 0;
    size_t queued;

    if (!m || !m->cfg.enable_ppsh || !be300_ppsh_transport_ready())
        return;

    raw_len = host_io_read_stdin_nonblocking(raw, sizeof(raw));
    if (raw_len == 0)
        return;

    for (size_t i = 0; i < raw_len && cooked_len < sizeof(cooked); i++) {
        unsigned char ch = raw[i];

        if (ch == '\n') {
            if (cooked_len == 0 || cooked[cooked_len - 1] != '\r')
                cooked[cooked_len++] = '\r';
        } else {
            cooked[cooked_len++] = ch;
        }
    }

    queued = be300_ppsh_queue_host_input(cooked, cooked_len);
    (void)queued;
}

static bool be300_read_phys_u32(machine_t *m, uint32_t pa, uint32_t *out)
{
    unsigned char *host;

    if (!m || !m->gxe_machine || !m->gxe_machine->memory || !out)
        return false;

    host = memory_paddr_to_hostaddr(m->gxe_machine->memory, pa, MEM_READ);
    if (!host)
        return false;

    memcpy(out, host, sizeof(*out));
    return true;
}

static void be300_maybe_apply_nk_override(machine_t *m)
{
    static const uint32_t mailbox_pa = 0x000024FCu;
    static const uint32_t nk_base_pa = 0x00060000u;
    static const uint32_t nk_base_va = 0x80060000u;
    static const uint32_t nk_max_va = 0x81000000u;
    uint32_t mailbox = 0;
    uint32_t norm_mailbox;
    unsigned char *dst;
    FILE *f;
    long fsize;
    char *buf;

    if (!m || !m->nk_override_path || m->nk_override_applied || m->nk_override_failed)
        return;

    if (!be300_read_phys_u32(m, mailbox_pa, &mailbox))
        return;

    norm_mailbox = mailbox & ~UINT32_C(0x20000000);
    if (norm_mailbox < nk_base_va || norm_mailbox >= nk_max_va)
        return;

    f = fopen(m->nk_override_path, "rb");
    if (!f) {
        fprintf(stderr, "[BE300] NK override open failed: %s\n", m->nk_override_path);
        m->nk_override_failed = true;
        return;
    }

    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    rewind(f);
    if (fsize <= 0) {
        fprintf(stderr, "[BE300] NK override is empty: %s\n", m->nk_override_path);
        fclose(f);
        m->nk_override_failed = true;
        return;
    }

    buf = malloc((size_t)fsize);
    if (!buf) {
        fprintf(stderr, "[BE300] OOM reading NK override (%ld bytes)\n", fsize);
        fclose(f);
        m->nk_override_failed = true;
        return;
    }
    if ((long)fread(buf, 1, (size_t)fsize, f) != fsize) {
        fprintf(stderr, "[BE300] Short read from NK override: %s\n", m->nk_override_path);
        free(buf);
        fclose(f);
        m->nk_override_failed = true;
        return;
    }
    fclose(f);

    dst = memory_paddr_to_hostaddr(m->gxe_machine->memory, nk_base_pa, MEM_WRITE);
    if (!dst) {
        fprintf(stderr, "[BE300] NK override target RAM is not writable yet\n");
        free(buf);
        return;
    }

    memcpy(dst, buf, (size_t)fsize);
    free(buf);

    /*
     * Direct host writes into emulated RAM bypass the normal MEM_WRITE path,
     * so flush any existing dyntrans blocks before guest execution resumes.
     */
    if (m->cpu)
        cpu_create_or_reset_tc(m->cpu);

    m->nk_override_applied = true;
    fprintf(stderr, "[BE300] Applied flat NK override at pc=%08" PRIx64 ": %s\n",
        m->cpu ? (uint64_t)m->cpu->pc : UINT64_C(0),
        m->nk_override_path);
}

static bool be300_uses_rom_boot(const machine_config_t *cfg)
{
    return cfg && (cfg->nand_path || cfg->restore);
}

static bool be300_nand_image_looks_bootable(const machine_t *m)
{
    static const uint8_t spl_sig[] = { 'B', '0', '0', '0', 'F', 'F', '\n' };

    if (!m || !m->nand_data || m->nand_size < 0x4000u + sizeof(spl_sig))
        return false;

    return memcmp(m->nand_data + 0x4000u, spl_sig, sizeof(spl_sig)) == 0;
}

static void be300_sync_nand_image(machine_t *m)
{
    FILE *f;

    if (!m || !m->nand_data || !m->cfg.nand_path || !m->nand.dirty)
        return;

    f = fopen(m->cfg.nand_path, "wb");
    if (!f) {
        fprintf(stderr, "[BE300] Failed to save NAND image: %s\n",
            m->cfg.nand_path);
        return;
    }
    if (fwrite(m->nand_data, 1, m->nand_size, f) != m->nand_size)
        fprintf(stderr, "[BE300] Short write while saving NAND image\n");
    fclose(f);
    m->nand.dirty = false;
}

/* ------------------------------------------------------------------ */
/* Image loaders — ROM and NAND                                        */
/* ------------------------------------------------------------------ */

static int be300_load_rom_image(machine_t *m, const char *path)
{
    FILE *f;
    long fsize;
    void *buf;
    uint64_t max = PA_ROM_BASE + PA_ROM_SIZE - PA_RESET_VECTOR;
    uint64_t kseg0_addr;

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[BE300] Cannot open ROM: %s\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    rewind(f);

    if (fsize <= 0) {
        fprintf(stderr, "[BE300] ROM is empty\n");
        fclose(f);
        return -1;
    }
    if ((uint64_t)fsize > max) {
        fprintf(stderr, "[BE300] ROM too large (%ld bytes, region %llu bytes)\n",
                fsize, (unsigned long long)max);
        fclose(f);
        return -1;
    }

    buf = malloc((size_t)fsize);
    if (!buf) {
        fprintf(stderr, "[BE300] OOM reading ROM\n");
        fclose(f);
        return -1;
    }

    if ((long)fread(buf, 1, (size_t)fsize, f) != fsize) {
        fprintf(stderr, "[BE300] Short read from ROM\n");
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    kseg0_addr = 0xffffffff80000000ULL | (uint64_t)PA_RESET_VECTOR;
    store_buf(m->cpu, kseg0_addr, (const char *)buf, (size_t)fsize);
    free(buf);
    return 0;
}

static int be300_load_nand_image_file(machine_t *m, const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data;
    size_t alloc_size;
    size_t bytes_read;
    long fsize;

    if (!f) {
        fprintf(stderr, "[BE300] Cannot open NAND image: %s\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    rewind(f);

    if (fsize < 0) {
        fclose(f);
        return -1;
    }
    if ((uint64_t)fsize > NAND_IMAGE_SIZE) {
        fprintf(stderr, "[BE300] NAND image too large (%ld bytes)\n", fsize);
        fclose(f);
        return -1;
    }

    alloc_size = NAND_IMAGE_SIZE;
    data = malloc(alloc_size);
    if (!data) {
        fprintf(stderr, "[BE300] OOM reading NAND (%zu bytes)\n", alloc_size);
        fclose(f);
        return -1;
    }

    memset(data, 0xFF, alloc_size);
    bytes_read = fread(data, 1, (size_t)fsize, f);
    if ((long)bytes_read != fsize) {
        fprintf(stderr, "[BE300] Short read from NAND image\n");
        free(data); fclose(f); return -1;
    }
    fclose(f);

    m->nand_data = data;
    m->nand_size = alloc_size;
    m->nand_file_size = (size_t)fsize;
    return 0;
}

static int be300_create_blank_nand(machine_t *m)
{
    uint8_t *data;

    if (!m)
        return -1;

    data = malloc(NAND_IMAGE_SIZE);
    if (!data)
        return -1;

    memset(data, 0xFF, NAND_IMAGE_SIZE);
    m->nand_data = data;
    m->nand_size = NAND_IMAGE_SIZE;
    m->nand_file_size = 0;
    return 0;
}


machine_t *be300_create(const machine_config_t *cfg)
{
    static bool subsystems_initialized = false;
    machine_t *m = calloc(1, sizeof(machine_t));
    const char *nk_override_env;
    const char *autostop_env;
    if (!m) return NULL;
    m->cfg = *cfg;
    m->boot_mode = BE300_BOOT_NONE;
    m->use_builtin_ui = true;
    m->save_exit_screenshot = true;
    m->mirror_serial_to_stdout = true;

    nk_override_env = getenv("BE300_NK_OVERRIDE");
    if (nk_override_env && nk_override_env[0] != '\0')
        m->nk_override_path = strdup(nk_override_env);

    autostop_env = getenv("BE300_AUTOSTOP_SEC");
    if (autostop_env && autostop_env[0] != '\0') {
        char *endptr = NULL;
        unsigned long long seconds = strtoull(autostop_env, &endptr, 10);
        if (endptr != autostop_env && endptr && *endptr == '\0' && seconds > 0) {
            m->autostop_after_ns = seconds * 1000000000ULL;
        } else {
            fprintf(stderr, "[BE300] Ignoring invalid BE300_AUTOSTOP_SEC=%s\n",
                autostop_env);
        }
    }
    m->fb_width = 240;
    m->fb_height = 320;
    m->fb_stride = 256;

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
    /* NAND/ROM boot does not use the NetBSD prom_emulation boot convention. */
    gxm->prom_emulation = be300_uses_rom_boot(cfg) ? 0 : 1;
    gxm->boot_kernel_filename = strdup("");
    gxm->boot_string_argument = strdup("");

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
    icu_init(&m->icu);
    siu_init(&m->siu);
    rtc_init(&m->rtc);
    gpio_init(&m->gpio);
    nand_init(&m->nand, NULL, 0, 0);
    cf_init(&m->cf);

    if (cfg->restore)
        m->btn_set2 = 0x80u;

    /*
     * Load NAND image or ROM image.
     */
    if (cfg->nand_path || cfg->restore) {
        if (cfg->nand_path) {
            if (be300_load_nand_image_file(m, cfg->nand_path) != 0) {
                fprintf(stderr, "[BE300] Failed to load NAND image\n");
                free(m);
                return NULL;
            }
        } else if (be300_create_blank_nand(m) != 0) {
            fprintf(stderr, "[BE300] Failed to create blank NAND image\n");
            free(m);
            return NULL;
        }

        if (cfg->cf_path && cf_load_image(&m->cf, cfg->cf_path) != 0) {
            fprintf(stderr, "[BE300] Failed to load CF image\n");
            free(m);
            return NULL;
        }

        if (cfg->cf_path) {
            bool nand_bootable = be300_nand_image_looks_bootable(m);
            bool cf_boot_visible = cfg->restore || !nand_bootable;

            cf_set_boot_visibility(&m->cf, cf_boot_visible);
        }

        /* True cold boot: start at ROM reset vector.
         * The ROM will read NAND, load the SPL, run the MIPS16
         * section copier and boot dispatcher, then jump to NK.exe. */
        m->cpu->pc = 0xffffffffBFC00000ULL;
        m->boot_mode = cfg->restore ? BE300_BOOT_RESTORE : BE300_BOOT_NAND;

        /* Re-initialize NAND controller with image data */
        if (m->nand_data) {
            nand_init(&m->nand, m->nand_data, m->nand_size,
                      m->nand_file_size);
        }

        /*
         * Map RAM at PA 0x1FC00000 for BEV=1 exception vectors.
         *
         * With prom_emulation=0 (NAND boot), CP0 Status has BEV=1.
         * The SPL doesn't clear BEV, so NK.exe starts with BEV=1.
         * Exception vectors (TLB refill, general, interrupt) go to
         * PA 0x1FC00000 + offset.  Real BE-300 hardware has 16K ROM
         * there (per docs/hardware/hardware.txt).  Without this RAM, exception
         * handlers can't be installed and WinCE can't set up virtual
         * memory or run the scheduler.
         */
        dev_ram_init(gxm, 0x1FC00000, 0x4000, DEV_RAM_RAM, 0, NULL);

        /*
         * Load the real BE-300 boot ROM into PA 0x1FC00000.
         * The image is used verbatim; if the emulator ever takes an
         * unexpected early BEV exception, the single-shot trace above
         * should capture it instead of patching the ROM in place.
         */
        {
#include "boot_rom_embedded.h"
            {
                    uint64_t base_va = 0xffffffffBFC00000ULL;
                    for (size_t i = 0; i < be300_boot_rom_len; i += 4) {
                        uint32_t w = be300_boot_rom[i] | (be300_boot_rom[i+1] << 8) |
                                     (be300_boot_rom[i+2] << 16) | (be300_boot_rom[i+3] << 24);
                        store_32bit_word(m->cpu, base_va + i, w);
                    }
                }
        }

        extern void be300_register_cf_window(struct machine *, machine_t *, bool);
        be300_register_cf_window(gxm, m, cfg->log_mmio);

        /* Register VR4131 SIU/DSIU (0x0F000800/0x0F000820 per hardware.txt) */
        extern void be300_register_vr4131_siu(struct machine *, siu_state_t *, bool);
        be300_register_vr4131_siu(gxm, &m->siu, cfg->log_mmio);

        /* Register VRC4173 latch (catch-all); pre-split to leave gaps for input device */
        extern void be300_register_vrc4173_latch(struct machine *, machine_t *, bool, bool);
        be300_register_vrc4173_latch(gxm, m, cfg->log_mmio, cfg->enable_ppsh);

        /*
         * Companion-chip secondary decode window at PA 0x0B000000.
         * docs/hardware/hardware.txt:204 lists 0xab000060 (CMU CLKMSK) and
         * 0xab00011c (GIU_PODATL) as "on companion", i.e. additional
         * VRC4173 registers accessible through a kseg1-aliased window at
         * 0xAB000000 distinct from the primary 0xAA000000 window. The
         * original boot (with unmodified card_ex.dll) and the user's diag
         * build both reach NK OAL code at pc=0x8007B1D4..0x8007B314 that
         * writes offsets 0x104, 0x108, 0x10C, 0x110, 0x138, 0x13C, 0x204,
         * 0x208, 0x520, 0x524 in this window. Without a backing device
         * the emulator logs "non-existant paddr" and drops the writes,
         * so any readback (e.g., a card-presence status poll) fails and
         * the driver stalls. A RAM-backed stub remembers writes so at
         * least readback of just-written values behaves sensibly; exact
         * semantics per offset are TBD from card_ex.dll reverse-eng.
         */
        dev_ram_init(gxm, 0x0B000000ULL, 0x10000ULL,
            DEV_RAM_RAM, 0, "be300_companion_ab_window");

        /*
         * PCMCIA "no card" stub at PA 0x0B400000-0x0B700000.
         * Returns 0xFF on reads (= CISTPL_END per PC Card spec §3.2.10),
         * terminating pcmcia.dll's CIS scan after one byte instead of
         * spinning forever on unmapped reads. See src/be300_devices.c
         * `be300_register_pcmcia_no_card` for the rationale + TODO.
         */
        extern void be300_register_pcmcia_no_card(struct machine *);
        be300_register_pcmcia_no_card(gxm);

        /* Register NAND flash; pre-split to leave gap at 0x0A00A040 for input device */
        extern void be300_register_nand(struct machine *, nand_state_t *,
            cf_state_t *, bool);
        be300_register_nand(gxm, &m->nand, &m->cf, cfg->log_mmio);

        /* Register input devices AFTER latch/NAND (fills pre-carved gaps) */
        extern void be300_register_input(struct machine *, machine_t *, bool);
        be300_register_input(gxm, m, cfg->log_mmio);
        m->input_registered = true;

    } else if (cfg->rom_path) {
        if (be300_load_rom_image(m, cfg->rom_path) != 0) {
            fprintf(stderr, "[BE300] Failed to load ROM image\n");
            free(m);
            return NULL;
        }
        m->boot_mode = BE300_BOOT_ROM;
    }

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

    be300_probe_attach(gxm);
    be300_probe_set_options(m->cfg.mmio_coverage,
                            m->cfg.detect_stall,
                            m->cfg.stall_window,
                            m->cfg.stall_unique_threshold,
                            m->cfg.stall_wall_secs);
    cpu_run_init(gxm);
    m->cpu->running = true;

    if (m->web_mode)
        timer_reset_state();
    else
        timer_start();
    console_init_main(emul);

    if (m->use_builtin_ui)
        ui_init(m);

    m->throttle_target_ips = (uint64_t)m->cfg.target_mhz * 1000000ULL;
    m->throttle_wall_origin = 0;
    m->throttle_instr_origin = 0;
    m->autostop_start_ns = monotonic_ns();
    m->last_report = 0;
    m->loop_count = 0;
    m->runtime_initialized = true;
    m->runtime_stopped = false;
    m->runtime_finalized = false;
    emul_executing = true;

    signal(SIGTERM, be300_handle_stop_signal);
    if (!m->web_mode)
        signal(SIGINT, be300_handle_stop_signal);

    host_io_reset_stdin_state();
    host_io_set_serial_sink(be300_serial_sink, m);
    host_io_set_stdout_enabled(m->mirror_serial_to_stdout);
    host_io_set_console_stdin_enabled(!m->cfg.enable_ppsh);
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
    host_io_set_console_stdin_enabled(true);

    emul_executing = false;
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    cpu_run_deinit(m->gxe_machine);
    be300_probe_detach(m->gxe_machine);
    console_deinit_main();
    m->runtime_stopped = true;
    m->runtime_finalized = true;
}

static bool be300_run_batch(machine_t *m)
{
    struct machine *gxm = m->gxe_machine;

    __sync_synchronize();
    if (emul_shutdown)
        return false;

    if (!machine_run(gxm))
        return false;

    be300_maybe_apply_nk_override(m);

    if (m->web_mode)
        timer_tick_manual();

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
    be300_ppsh_poll_host_input(m);
    if (m->use_builtin_ui)
        ui_update(m);

    be300_touch_tick(m);

    if (m->use_builtin_ui && ui_should_quit(m))
        return false;

    if (m->autostop_after_ns > 0 && m->autostop_start_ns > 0) {
        uint64_t now_ns = monotonic_ns();
        if (now_ns - m->autostop_start_ns >= m->autostop_after_ns) {
            fprintf(stderr,
                "[BE300] Autostop reached after %" PRIu64 " s\n",
                m->autostop_after_ns / 1000000000ULL);
            emul_shutdown = true;
            __sync_synchronize();
            return false;
        }
    }

    m->loop_count++;
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

            g = (pixel >> 5) & 0x3Fu;
            g = (g << 2) | (g >> 4);

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
    be300_touch_tick(m);
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

    be300_sync_nand_image(m);
    cf_save_image(&m->cf);

    if (m->nand_data) {
        free(m->nand_data);
        m->nand_data = NULL;
    }
    if (m->nk_override_path) {
        free(m->nk_override_path);
        m->nk_override_path = NULL;
    }
    cf_destroy(&m->cf);

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
