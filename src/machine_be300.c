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

#include <errno.h>

#include "be300.h"
#include "file_util.h"
#include "be300_probe.h"
#include "host_io.h"
#include "host_time.h"
#include "pcconnect.h"
#include "pcconnect_bridge.h"
#include "usbredir_dev.h"
#include "ppsh.h"
#include "serial_backends.h"
#include "stowaway.h"
#include "ui.h"
#include "devices/devices_internal.h"

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
#include "net.h"
#include "console.h"
#include "timer.h"
#include "settings.h"

/* GXemul externs */
extern volatile bool emul_shutdown;
extern volatile bool emul_executing;

#define BE300_FB_DEFAULT_WIDTH       240u
#define BE300_FB_DEFAULT_HEIGHT      320u
#define BE300_FB_DEFAULT_STRIDE      256u
#define BE300_FB_DEFAULT_MEM_HEIGHT  324u
#define BE300_FB_HIRES_WIDTH         480u
#define BE300_FB_HIRES_HEIGHT        640u
#define BE300_FB_HIRES_STRIDE        512u
#define BE300_FB_HIRES_MEM_HEIGHT    640u

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

static bool be300_uses_rom_boot(const machine_config_t *cfg)
{
    return cfg && (cfg->nand_path || cfg->restore);
}

static bool be300_select_framebuffer_geometry(machine_t *m)
{
    uint32_t width;
    uint32_t height;
    uint32_t stride;

    if (!m)
        return false;

    width = m->cfg.fb_width ? m->cfg.fb_width : BE300_FB_DEFAULT_WIDTH;
    height = m->cfg.fb_height ? m->cfg.fb_height : BE300_FB_DEFAULT_HEIGHT;
    stride = m->cfg.fb_stride ? m->cfg.fb_stride : BE300_FB_DEFAULT_STRIDE;

    if (width == BE300_FB_DEFAULT_WIDTH &&
        height == BE300_FB_DEFAULT_HEIGHT &&
        stride == BE300_FB_DEFAULT_STRIDE) {
        m->fb_width = width;
        m->fb_height = height;
        m->fb_stride = stride;
        m->fb_mem_height = BE300_FB_DEFAULT_MEM_HEIGHT;
        return true;
    }

    if (width == BE300_FB_HIRES_WIDTH &&
        height == BE300_FB_HIRES_HEIGHT &&
        stride == BE300_FB_HIRES_STRIDE) {
        m->fb_width = width;
        m->fb_height = height;
        m->fb_stride = stride;
        m->fb_mem_height = BE300_FB_HIRES_MEM_HEIGHT;
        if (m->cfg.sdram_size < 64u * 1024u * 1024u) {
            fprintf(stderr,
                "[BE300] Warning: 480x640 framebuffer mode is experimental; "
                "--sdram 64 is recommended\n");
        }
        fprintf(stderr,
            "[BE300] Experimental framebuffer mode: %ux%u stride=%u\n",
            m->fb_width, m->fb_height, m->fb_stride);
        return true;
    }

    fprintf(stderr,
        "[BE300] Unsupported framebuffer geometry: %ux%u stride=%u\n",
        width, height, stride);
    return false;
}

static void be300_apply_rtc_host_time(machine_t *m)
{
    time_t now;
    struct tm tm_local;
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;

    if (!m || !m->cfg.enable_rtc_host_time)
        return;

    now = time(NULL);
    if (now == (time_t)-1) {
        fprintf(stderr, "[BE300] Failed to read host time for RTC\n");
        return;
    }

#if defined(_WIN32)
    if (localtime_s(&tm_local, &now) != 0) {
#else
    if (localtime_r(&now, &tm_local) == NULL) {
#endif
        fprintf(stderr, "[BE300] Failed to convert host time for RTC\n");
        return;
    }

    year = tm_local.tm_year + 1900;
    month = tm_local.tm_mon + 1;
    day = tm_local.tm_mday;
    hour = tm_local.tm_hour;
    minute = tm_local.tm_min;
    second = tm_local.tm_sec;

    if (dev_vr41xx_set_rtc_ymdhms(m->gxe_machine, year, month, day,
            hour, minute, second) != 0) {
        fprintf(stderr, "[BE300] Failed to initialize VR4131 RTC from host time\n");
        return;
    }
    fprintf(stderr,
        "[BE300] RTC initialized from host local time: "
        "%04d-%02d-%02d %02d:%02d:%02d\n",
        year, month, day, hour, minute, second);
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
    size_t len;

    if (!m || !m->nand_data || !m->cfg.nand_path || !m->nand.dirty)
        return;

    /*
     * Keep the image's own length (a 16,449,536-byte restore image, or a
     * short one) unless the guest programmed pages past it; the loader
     * filled the rest of the chip with erased 0xFF.
     */
    len = m->nand_size;
    if (m->nand_file_size > 0 && m->nand_file_size < m->nand_size) {
        size_t tail = m->nand_file_size;

        while (tail < m->nand_size && m->nand_data[tail] == 0xFFu)
            tail++;
        if (tail == m->nand_size)
            len = m->nand_file_size;
    }
    if (be300_write_file_atomic(m->cfg.nand_path, m->nand_data, len) != 0) {
        fprintf(stderr, "[BE300] Failed to save NAND image %s: %s\n",
            m->cfg.nand_path, strerror(errno));
        return;
    }
    m->nand.dirty = false;
}

/* ------------------------------------------------------------------ */
/* Image loaders — ROM and NAND                                        */
/* ------------------------------------------------------------------ */

int be300_register_boot_rom(struct machine *gxm, const uint8_t *image,
    size_t len);

static int be300_load_rom_image(machine_t *m, const char *path)
{
    FILE *f;
    long fsize;
    void *buf;
    uint64_t max = PA_ROM_BASE + PA_ROM_SIZE - PA_RESET_VECTOR;

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

    be300_register_boot_rom(m->gxe_machine, buf, (size_t)fsize);
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


/*
 * Undo a partly built machine: be300_create arms the host bridges, the
 * usbredir device and the Stowaway dock first, and machine_setup registers
 * GXemul interrupt handlers by name, so a failure that only freed m left
 * them armed and made the next be300_create in this process fail (bind:
 * address in use) or abort on duplicate handler names.
 */
static machine_t *be300_create_fail(machine_t *m)
{
    free(m->nand_data);
    for (unsigned i = 0; i < BE300_MAX_CF_SLOTS; i++)
        cf_destroy(&m->cf[i]);
    pcconnect_bridge_shutdown();
    usbredir_dev_shutdown();
    stowaway_configure(false);
    interrupt_handler_clear_all();
    timer_remove_all();
    free(m);
    return NULL;
}

machine_t *be300_create(const machine_config_t *cfg)
{
    static bool subsystems_initialized = false;
    machine_t *m = calloc(1, sizeof(machine_t));
    const char *autostop_env;
    if (!m) return NULL;
    m->cfg = *cfg;
    if (cfg->pcconnect_bridge &&
        cfg->pcconnect_dock == BE300_PCC_DOCK_USB_SYNC) {
        /*
         * The USB cable does not use the SIU: the BE-300 is a USB device
         * (docs/BE300_USB_FUNCTION.md), so the host link carries usbredir
         * for QEMU's usb-redir instead of raw serial bytes.
         */
        usbredir_dev_config_t ur = {0};
        const char *v = getenv("BE300_USBREDIR_TRACE");

        if (!host_chardev_parse_spec(cfg->pcconnect_bridge, &ur.kind,
                                     &ur.target)) {
            return be300_create_fail(m);
        }
        ur.tee_path = cfg->pcconnect_tee;
        ur.trace = v && *v && strcmp(v, "0") != 0;
        if (!usbredir_dev_configure(&ur)) {
            return be300_create_fail(m);
        }
    } else if (cfg->pcconnect_bridge) {
        pcc_bridge_config_t br = {0};
        if (!pcconnect_bridge_parse_spec(cfg->pcconnect_bridge, &br)) {
            return be300_create_fail(m);
        }
        br.uart_id = PCC_UART_VRC4173_SIU;
        br.wince_mode = true;
        br.tee_path = cfg->pcconnect_tee;
        br.uart_name = NULL;        /* default vrc4173siu */
        br.trace = (getenv("BE300_PCC_TRACE") != NULL);
        br.baud = cfg->pcconnect_baud;
        if (!pcconnect_bridge_configure(&br)) {
            return be300_create_fail(m);
        }
    }
    if (cfg->serial0_bridge) {
        pcc_bridge_config_t br = {0};
        if (!pcconnect_bridge_parse_spec(cfg->serial0_bridge, &br)) {
            return be300_create_fail(m);
        }
        br.uart_id = PCC_UART_VR4131_SIU;
        br.wince_mode = false;
        br.tee_path = cfg->serial0_tee;
        br.uart_name = NULL;        /* default "siu" */
        br.trace = (getenv("BE300_SERIAL0_TRACE") != NULL);
        br.baud = cfg->serial0_baud;
        if (!pcconnect_bridge_configure(&br)) {
            return be300_create_fail(m);
        }
    }
    if (cfg->serial1_bridge) {
        pcc_bridge_config_t br = {0};
        if (!pcconnect_bridge_parse_spec(cfg->serial1_bridge, &br)) {
            return be300_create_fail(m);
        }
        br.uart_id = PCC_UART_VRC4173_SIU;
        br.wince_mode = false;
        br.tee_path = cfg->serial1_tee;
        br.uart_name = NULL;        /* default "vrc4173siu" */
        br.trace = (getenv("BE300_SERIAL1_TRACE") != NULL);
        br.baud = cfg->serial1_baud;
        if (!pcconnect_bridge_configure(&br)) {
            return be300_create_fail(m);
        }
    }
    stowaway_configure(cfg->enable_stowaway_keyboard);
    be300_serial_backends_install();
    m->boot_mode = BE300_BOOT_NONE;
    m->use_builtin_ui = true;
    m->save_exit_screenshot = true;
    m->mirror_serial_to_stdout = true;

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
    if (!be300_select_framebuffer_geometry(m)) {
        return be300_create_fail(m);
    }

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
        return be300_create_fail(m);
    }

    struct machine *gxm = emul_add_machine(m->emul, "be300");
    if (!gxm) {
        fprintf(stderr, "[BE300] emul_add_machine failed\n");
        return be300_create_fail(m);
    }
    m->gxe_machine = gxm;

    /*
     * Configure as BE-300.
     */
    gxm->machine_type = MACHINE_HPCMIPS;
    gxm->machine_subtype = MACHINE_HPCMIPS_CASIO_BE300;
    gxm->cpu_name = strdup("VR4131");
    gxm->physical_ram_in_mb = cfg->sdram_size / (1024 * 1024);
    gxm->hpcmips_fb_width = (int)m->fb_width;
    gxm->hpcmips_fb_height = (int)m->fb_height;
    gxm->hpcmips_fb_stride = (int)m->fb_stride;
    gxm->hpcmips_fb_mem_height = (int)m->fb_mem_height;
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
    gxm->cpus = calloc(1, sizeof(struct cpu *));
    if (gxm->cpus)
        gxm->cpus[0] = cpu_new(gxm->memory, gxm, 0, gxm->cpu_name);
    if (!gxm->memory || !gxm->cpus || !gxm->cpus[0]) {
        fprintf(stderr, "[BE300] cpu_new failed for VR4131\n");
        return be300_create_fail(m);
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

    /* Initialize board-owned peripheral state. VR4131 internal devices are
     * initialized by dev_vr41xx during machine_setup() above. */
    siu_init(&m->siu);
    nand_init(&m->nand, NULL, 0, 0);
    for (unsigned i = 0; i < BE300_MAX_CF_SLOTS; i++)
        cf_init(&m->cf[i]);
    aiu_init(&m->aiu, cfg->log_mmio);
    be300_apply_rtc_host_time(m);

    if (cfg->restore)
        m->btn_set2 = 0x80u;

    /*
     * Load NAND image or ROM image.
     */
    if (cfg->nand_path || cfg->restore) {
        if (cfg->nand_path) {
            if (be300_load_nand_image_file(m, cfg->nand_path) != 0) {
                fprintf(stderr, "[BE300] Failed to load NAND image\n");
                return be300_create_fail(m);
            }
        } else if (be300_create_blank_nand(m) != 0) {
            fprintf(stderr, "[BE300] Failed to create blank NAND image\n");
            return be300_create_fail(m);
        }

        bool cf_image_loaded = false;
        for (unsigned i = 0; i < BE300_MAX_CF_SLOTS; i++) {
            if (!cfg->cf_paths[i])
                continue;
            /* --cf-insert-after: the card arrives later (be300_cf_hotplug). */
            if (i == BE300_PRIMARY_CF_SLOT && cfg->cf_insert_after_s &&
                (!cfg->cf_eject_after_s || cfg->cf_eject_after_s > cfg->cf_insert_after_s))
                continue;
            if (cf_load_image(&m->cf[i], cfg->cf_paths[i]) != 0) {
                fprintf(stderr, "[BE300] Failed to load CF image %u\n",
                    i + 1);
                return be300_create_fail(m);
            }
            cf_image_loaded = true;
        }

        if (cfg->enable_ne2000 || cfg->enable_xi825) {
            if ((cfg->enable_ne2000 && cfg->enable_xi825) || cfg->restore ||
                cfg->rom_path || cf_present(&m->cf[BE300_PRIMARY_CF_SLOT])) {
                fprintf(stderr, "[BE300] Conflicting primary PCMCIA socket configuration\n");
                return be300_create_fail(m);
            }
            m->emul->net = net_init(m->emul, NET_INIT_FLAG_GATEWAY,
                NULL, NET_DEFAULT_IPV4_MASK, NET_DEFAULT_IPV4_LEN,
                NULL, 0, 0, NULL);
            if (!m->emul->net) {
                fprintf(stderr, "[BE300] Failed to initialize emulated network\n");
                return be300_create_fail(m);
            }
            int (*attach)(cf_state_t *, struct machine *, struct net *, const uint8_t *) =
                cfg->enable_xi825 ? cf_attach_xi825 : cf_attach_ne2000;
            if (attach(&m->cf[BE300_PRIMARY_CF_SLOT], gxm,
                    m->emul->net,
                    cfg->net_mac_set ? cfg->net_mac : NULL) != 0) {
                fprintf(stderr, "[BE300] Failed to attach network card\n");
                return be300_create_fail(m);
            }
        }

        if (cf_image_loaded) {
            bool nand_bootable = be300_nand_image_looks_bootable(m);
            bool cf_boot_visible = cfg->restore || !nand_bootable;

            if (!cf_is_network(&m->cf[BE300_PRIMARY_CF_SLOT]))
                cf_set_boot_visibility(&m->cf[BE300_PRIMARY_CF_SLOT],
                    cf_boot_visible);
            for (unsigned i = 1; i < BE300_MAX_CF_SLOTS; i++)
                cf_set_boot_visibility(&m->cf[i], false);
            if (cf_present(&m->cf[1])) {
                fprintf(stderr,
                    "[BE300] Secondary CF image loaded; secondary socket "
                    "MMIO is not decoded yet, so only slot 0 is "
                    "guest-visible\n");
            }
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
         * The real BE-300 boot ROM at PA 0x1FC00000, verbatim and
         * read-only (src/devices/boot_rom.c). With Status.BEV set the
         * exception vectors are ROM code too.
         */
        {
#include "boot_rom_embedded.h"
            be300_register_boot_rom(gxm, be300_boot_rom, be300_boot_rom_len);
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
         * the driver stalls. The device preserves guest-written latches
         * and overlays the known PCMCIA no-media status bytes when no CF
         * image is attached.
         */
        extern void be300_register_companion_ab_window(struct machine *,
            machine_t *, bool);
        be300_register_companion_ab_window(gxm, m, cfg->log_mmio);

        /*
         * PCMCIA card attribute-memory window at PA 0x0B400000-0x0B700000.
         * No card attached: returns 0xFF (= CISTPL_END per PC Card
         * Standard Release 8 §3.2.10) so pcmcia.dll's CIS scan exits
         * after one byte instead of spinning forever.
         * Card attached (`--cf <image>`): routes reads into the CF
         * device's seeded CIS so pcmcia.dll can identify the card.
         * See `be300_register_pcmcia_attr_window` in src/be300_devices.c.
         */
        extern void be300_register_pcmcia_attr_window(struct machine *,
            machine_t *);
        be300_register_pcmcia_attr_window(gxm, m);

        /* Register NAND flash; pre-split to leave gap at 0x0A00A040 for input device */
        extern void be300_register_nand(struct machine *, nand_state_t *,
            cf_state_t *, bool);
        be300_register_nand(gxm, &m->nand,
            &m->cf[BE300_PRIMARY_CF_SLOT], cfg->log_mmio);

        /* Register input devices AFTER latch/NAND (fills pre-carved gaps) */
        extern void be300_register_input(struct machine *, machine_t *, bool);
        be300_register_input(gxm, m, cfg->log_mmio);
        m->input_registered = true;

    } else if (cfg->rom_path) {
        if (be300_load_rom_image(m, cfg->rom_path) != 0) {
            fprintf(stderr, "[BE300] Failed to load ROM image\n");
            return be300_create_fail(m);
        }
        m->boot_mode = BE300_BOOT_ROM;
    }

    return m;
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
    m->autostop_start_ns = be300_host_monotonic_ns();
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

/* --cf-eject-after / --cf-insert-after: hot plug of the primary card, so
 * a guest's insertion handling (pcmcia.dll, Glaze's card driver) can be
 * traced and tested. Each fires once, on host time since start. */
static void be300_cf_hotplug(machine_t *m)
{
    static bool ejected, inserted;
    cf_state_t *cf = &m->cf[BE300_PRIMARY_CF_SLOT];
    uint64_t s;

    if ((!m->cfg.cf_eject_after_s || ejected) && (!m->cfg.cf_insert_after_s || inserted))
        return;
    s = (be300_host_monotonic_ns() - m->autostop_start_ns) / UINT64_C(1000000000);
    if (m->cfg.cf_eject_after_s && !ejected && s >= m->cfg.cf_eject_after_s) {
        ejected = true;
        cf_eject(cf);
        be300_cf_irq_update(g_be300_vrc4173_latch);
        fprintf(stderr, "[CF] card ejected after %" PRIu64 " s\n", s);
    }
    if (m->cfg.cf_insert_after_s && !inserted && s >= m->cfg.cf_insert_after_s &&
        (!m->cfg.cf_eject_after_s || ejected) && m->cfg.cf_paths[BE300_PRIMARY_CF_SLOT]) {
        inserted = true;
        if (cf_load_image(cf, m->cfg.cf_paths[BE300_PRIMARY_CF_SLOT]) == 0)
            fprintf(stderr, "[CF] card inserted after %" PRIu64 " s\n", s);
        be300_cf_irq_update(g_be300_vrc4173_latch);
    }
}

static bool be300_run_batch(machine_t *m)
{
    struct machine *gxm = m->gxe_machine;

    __sync_synchronize();
    if (emul_shutdown)
        return false;

    if (!machine_run(gxm))
        return false;

    be300_probe_note_batch(m->cpu);

#ifdef _WIN32
    timer_tick_manual();
#else
    if (m->web_mode)
        timer_tick_manual();
#endif

    if (!m->web_mode && m->throttle_target_ips > 0) {
        uint64_t now_ns = be300_host_monotonic_ns();
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
                m->throttle_wall_origin = be300_host_monotonic_ns();
                m->throttle_instr_origin = instrs;
            }
        }
    }

    console_flush();
    be300_ppsh_poll_host_input(m);
    if (m->use_builtin_ui)
        ui_update(m);

    be300_touch_tick(m);
    cf_ne2000_tick(&m->cf[BE300_PRIMARY_CF_SLOT]);
    cf_xi825_tick(&m->cf[BE300_PRIMARY_CF_SLOT]);
    be300_vrc4173_update_cf_irq();
    aiu_tick(m);
    if (m->cfg.enable_stowaway_keyboard || m->cfg.pcconnect_bridge ||
        m->cfg.serial0_bridge || m->cfg.serial1_bridge)
        be300_pcconnect_poll();
    if (m->cfg.pcconnect_bridge || m->cfg.serial0_bridge ||
        m->cfg.serial1_bridge)
        pcconnect_bridge_tick();
    usbredir_dev_tick();

    if (m->use_builtin_ui && ui_should_quit(m))
        return false;

    be300_cf_hotplug(m);

    if (m->autostop_after_ns > 0 && m->autostop_start_ns > 0) {
        uint64_t now_ns = be300_host_monotonic_ns();
        if (now_ns - m->autostop_start_ns >= m->autostop_after_ns) {
            fprintf(stderr,
                "[BE300] Autostop reached after %" PRIu64 " s\n",
                m->autostop_after_ns / UINT64_C(1000000000));
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
    bool was_down;

    if (!m)
        return;

    was_down = m->touch_down;
    if (was_down && !down) {
        /*
         * Host input can deliver a short click only at an emulator batch
         * boundary. Give the PIU one final tick in the old pen-down state
         * so any matured coordinate conversion is queued before release
         * stops the scan sequencer.
         */
        m->touch_x = x;
        m->touch_y = y;
        __sync_synchronize();
        be300_touch_tick(m);
    }

    m->touch_down = down;
    m->touch_x = x;
    m->touch_y = y;
    __sync_synchronize();
    be300_touch_tick(m);
}

void be300_set_buttons(machine_t *m, uint8_t btn_set1, uint8_t btn_set2)
{
    uint8_t old_set1, old_set2;

    if (!m)
        return;

    old_set1 = m->btn_set1;
    old_set2 = m->btn_set2;
    if (old_set1 == btn_set1 && old_set2 == btn_set2)
        return;

    m->btn_set1 = btn_set1;
    m->btn_set2 = btn_set2;
    __sync_synchronize();
    be300_buttons_host_update(m, old_set1, old_set2);
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
    for (unsigned i = 0; i < BE300_MAX_CF_SLOTS; i++)
        cf_save_image(&m->cf[i]);

    if (m->nand_data) {
        free(m->nand_data);
        m->nand_data = NULL;
    }
    for (unsigned i = 0; i < BE300_MAX_CF_SLOTS; i++)
        cf_destroy(&m->cf[i]);
    pcconnect_bridge_shutdown();
    usbredir_dev_shutdown();
    stowaway_configure(false);

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
