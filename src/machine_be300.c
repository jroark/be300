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
    gxm->prom_emulation = be300_uses_rom_boot(cfg) ? 0 : 1;
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
    cf_init(&m->cf);

    if (cfg->restore)
        m->btn_set2 = 0x80u;

    /*
     * Load kernel or NAND image.
     */
    if (cfg->kernel_path) {
        if (be300_boot_linux_path(m, cfg->kernel_path, cfg->cmdline,
                                  cfg->ram_path) != 0) {
            free(m);
            return NULL;
        }

    } else if (cfg->nand_path || cfg->restore) {
        if (cfg->nand_path) {
            if (loader_load_nand_image(m, cfg->nand_path) != 0) {
                fprintf(stderr, "[BE300] Failed to load NAND image\n");
                free(m);
                return NULL;
            }
        } else if (loader_create_blank_nand_image(m) != 0) {
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

        /* Register VRC4173 latch (catch-all); pre-split to leave gaps for input device */
        extern void be300_register_vrc4173_latch(struct machine *, machine_t *, bool, bool);
        be300_register_vrc4173_latch(gxm, m, cfg->log_mmio, cfg->enable_ppsh);

        /* Register NAND flash; pre-split to leave gap at 0x0A00A040 for input device */
        extern void be300_register_nand(struct machine *, nand_state_t *,
            cf_state_t *, bool);
        be300_register_nand(gxm, &m->nand, &m->cf, cfg->log_mmio);

        /* Register input devices AFTER latch/NAND (fills pre-carved gaps) */
        extern void be300_register_input(struct machine *, machine_t *, bool);
        be300_register_input(gxm, m, cfg->log_mmio);
        m->input_registered = true;

    } else if (cfg->rom_path) {
        if (loader_load_rom(m, cfg->rom_path) != 0) {
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

    if (m->use_builtin_ui && ui_should_quit(m))
        return false;

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

machine_t *be300_create_web(uint32_t sdram_mb, uint32_t target_mhz,
                            bool sfb_5bit_green)
{
    machine_config_t cfg = {
        .trace = false,
        .log_mmio = false,
        .sfb_5bit_green = sfb_5bit_green,
        .log_nand_legacy = false,
        .debug_serial = false,
        .enable_ppsh = false,
        .restore = false,
        .rom_path = NULL,
        .kernel_path = NULL,
        .cmdline = NULL,
        .ram_path = NULL,
        .nand_path = NULL,
        .cf_path = NULL,
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

    be300_sync_nand_image(m);
    cf_save_image(&m->cf);

    if (m->nand_data) {
        free(m->nand_data);
        m->nand_data = NULL;
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
