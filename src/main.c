#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "be300.h"
#include "pcconnect_bridge.h"
#ifdef _WIN32
#include "win32_compat.h"
#endif
#ifdef BE300_HAVE_LAUNCHER
#include "launcher/launcher.h"
#endif

/* GXemul global verbosity gates — see gxemul/src/core/debugmsg.c.
 * debug() is silent while emul_executing unless verbose >= 1. */
extern int verbose;
extern int quiet_mode;

static bool parse_scale(const char *arg, double *scale_out)
{
    char *end = NULL;
    double scale;

    if (!arg || !*arg)
        return false;

    scale = strtod(arg, &end);
    if (end == arg || *end != '\0')
        return false;
    if (!(scale >= 1.0 && scale <= 4.0))
        return false;

    *scale_out = scale;
    return true;
}

static bool parse_fb_size(const char *arg, uint32_t *width_out,
    uint32_t *height_out, uint32_t *stride_out)
{
    unsigned width, height;
    char tail;

    if (!arg || !width_out || !height_out || !stride_out)
        return false;
    if (sscanf(arg, "%ux%u%c", &width, &height, &tail) != 2 &&
        sscanf(arg, "%uX%u%c", &width, &height, &tail) != 2)
        return false;

    if (width == 240u && height == 320u) {
        *width_out = 240u;
        *height_out = 320u;
        *stride_out = 256u;
        return true;
    }
    if (width == 480u && height == 640u) {
        *width_out = 480u;
        *height_out = 640u;
        *stride_out = 512u;
        return true;
    }

    return false;
}

static bool parse_mac(const char *arg, uint8_t mac[6])
{
    unsigned vals[6];
    char tail;

    if (!arg || !mac)
        return false;
    if (sscanf(arg, "%x:%x:%x:%x:%x:%x%c",
        &vals[0], &vals[1], &vals[2],
        &vals[3], &vals[4], &vals[5], &tail) != 6)
        return false;
    for (unsigned i = 0; i < 6; i++) {
        if (vals[i] > 0xFFu)
            return false;
        mac[i] = (uint8_t)vals[i];
    }
    if ((mac[0] & 1u) != 0)
        return false;
    return true;
}

static bool parse_pcconnect_dock(const char *arg,
    be300_pcconnect_dock_mode_t *dock_out)
{
    if (!arg || !dock_out)
        return false;

    if (strcmp(arg, "rs232") == 0) {
        *dock_out = BE300_PCC_DOCK_RS232;
        return true;
    }
    if (strcmp(arg, "usb-sync") == 0 || strcmp(arg, "usb-vcom") == 0) {
        *dock_out = BE300_PCC_DOCK_USB_SYNC;
        return true;
    }

    return false;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] [rom.bin]\n"
        "\n"
        "Options:\n"
        "  --trace               Print each executed instruction to stderr\n"
        "  --log-mmio            Log all MMIO register reads/writes to stderr\n"
        "  --nand <image>        Boot WinCE from NAND dump (B000FF SPL loader)\n"
        "  --cf <image>          Attach a FAT16 CF image (may be specified twice;\n"
        "                        with --ne2000, one --cf uses the secondary socket)\n"
        "  --ne2000              Attach a PCMCIA NE2000 Ethernet card\n"
        "  --net-mac <mac>       Override NE2000 MAC address (aa:bb:cc:dd:ee:ff)\n"
        "  --audio               Enable Casio AIU audio path on the VRC4173 latch\n"
        "                        (default off; pulls PCM from wavedev DMA pointers\n"
        "                         and plays through SDL; BE300_AUDIO=0 mutes host audio)\n"
        "  --restore             Enter CF recovery boot mode (requires --cf)\n"
        "  --sdram <MB>          SDRAM size in megabytes (default: 16)\n"
        "  --ppsh                Enable PPSH (parallel port debug shell) probe\n"
        "  --pcconnect-bridge <S> Pipe the VRC4173 SIU UART to a host chardev so a\n"
        "                        UTM Windows VM running PCConnect can talk to the guest.\n"
        "                        S = tcp:HOST:PORT | tcp-listen:PORT[@ADDR]\n"
        "                          | unix:/PATH | unix-listen:/PATH | pty:auto\n"
        "  --pcconnect-tee <P>   Write both directions of the bridged byte stream to P\n"
        "                        (annotated text). Requires --pcconnect-bridge.\n"
        "  --pcconnect-baud <N>  Throttle guest->host bridge transmit to N baud (8N1).\n"
        "                        Default 115200 to match real serial; 0 = unlimited.\n"
        "  --pcconnect-dock <D>  Guest-visible dock socket: rs232 or usb-sync\n"
        "                        (default rs232; usb-sync requires --pcconnect-bridge).\n"
        "  --serial0-bridge <S>  Pipe the VR4131 main SIU (Linux ttyVR0) to a host chardev.\n"
        "                        S = same forms as --pcconnect-bridge (tcp/tcp-listen/unix/...\n"
        "                        unix-listen/pty:auto). Plain Linux-mode (no PCConnect quirks).\n"
        "  --serial0-tee <P>     Tee both directions of the serial0 stream to P.\n"
        "  --serial0-baud <N>    Throttle serial0 transmit to N baud (default 115200).\n"
        "  --serial1-bridge <S>  Pipe the VRC4173 companion SIU (Linux ttyS0 / WinCE dock)\n"
        "                        to a host chardev in plain-Linux mode (no PCConnect quirks).\n"
        "                        Mutually exclusive with --pcconnect-bridge and\n"
        "                        --stowaway-keyboard (all share the same UART).\n"
        "  --serial1-tee <P>     Tee both directions of the serial1 stream to P.\n"
        "  --serial1-baud <N>    Throttle serial1 transmit to N baud (default 115200).\n"
        "  --rtc-host-time       Initialize the guest RTC from host local time\n"
        "  --stowaway-keyboard   Feed host key events to the Stowaway serial dock on COM1:\n"
        "  --fb-size <WxH>       Experimental framebuffer size: 240x320 or 480x640\n"
        "                        (default: 240x320; 480x640 requires a patched NAND)\n"
        "  --frame               Show the BE-300 bezel around the LCD (default: hidden)\n"
        "  --scale <N>           Render scale, 1.0-4.0 (default: 1.0; applies with or without --frame)\n"
        "  --speed <N>           Throttle target in million guest instructions/sec\n"
        "                        (default: 166, 0 = unthrottled)\n"
        "  --mmio-coverage       First-hit log per (device, offset, op) + shutdown coverage table\n"
        "  --detect-stall        Emit [BE300_STALL] when guest spins on a tight PC set\n"
        "  --stall-window=N      Instructions per sampling bucket (default 10000)\n"
        "  --stall-threshold=K   Fire when unique PCs in window < K (default 64)\n"
        "  --stall-wall-secs=T   Sustained wall-seconds below threshold before firing (default 5)\n"
        "  -h, --help            Show this help\n"
        "\n"
        "ROM image (positional arg) is loaded at PA 0x1FC00000 (MIPS reset vector).\n"
        "Choose exactly one boot source: --restore, --nand, or rom.bin.\n",
        prog);
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

#ifdef _WIN32
    if (!be300_win32_init())
        return 1;
    atexit(be300_win32_shutdown);
#endif

#ifdef BE300_HAVE_LAUNCHER
    /* Filter out macOS LaunchServices process-serial-number args
     * (-psn_0_12345) that AppKit prepends when an .app is double-clicked.
     * After filtering, if argv carries no payload (or just a single
     * .be300vm bundle path), defer to the launcher UI. */
    {
        int filtered_argc = 0;
        char *filtered_argv[64];
        if (argc > 0)
            filtered_argv[filtered_argc++] = argv[0];
        for (int i = 1; i < argc && filtered_argc < 64; i++) {
            if (strncmp(argv[i], "-psn_", 5) == 0)
                continue;
            filtered_argv[filtered_argc++] = argv[i];
        }
        bool defer_to_launcher = false;
        if (filtered_argc <= 1) {
            defer_to_launcher = true;
        } else if (filtered_argc == 2) {
            const char *a = filtered_argv[1];
            size_t n = strlen(a);
            if (n >= 8 && strcmp(a + n - 8, ".be300vm") == 0)
                defer_to_launcher = true;
        }
        if (defer_to_launcher)
            return launcher_main(filtered_argc, filtered_argv);
    }
#endif

    machine_config_t cfg = {
        .trace           = false,
        .log_mmio        = false,
        .log_nand_legacy = false,
        .enable_ppsh     = false,
        .enable_rtc_host_time = false,
        .enable_stowaway_keyboard = false,
        .enable_ne2000   = false,
        .enable_audio    = false,
        .net_mac_set     = false,
        .restore         = false,
        .mmio_coverage   = false,
        .detect_stall    = false,
        /* Defaults calibrated against the current ddi.dll post-splash spin
         * (Pass 34): a 2-instruction tight loop at PC 0x01a5382c reading
         * VRC4173 latch offset 0x234. window=10000 + threshold=64 discriminates
         * tight loops (< 64 unique PCs) from kernel/scheduler activity
         * (>> 64 unique PCs per 10K-instruction window). */
        .stall_window            = 10000u,
        .stall_unique_threshold  = 64u,
        .stall_wall_secs         = 5u,
        .rom_path       = NULL,
        .nand_path      = NULL,
        .cf_paths       = { NULL },
        .cf_count       = 0,
        .net_mac        = { 0 },
        .sdram_size     = 16u * 1024u * 1024u,
        .target_mhz     = 166u,
        .fb_width       = 0,
        .fb_height      = 0,
        .fb_stride      = 0,
        .frame_visible  = false,
        .scale          = 1.0,
        .pcconnect_bridge = NULL,
        .pcconnect_tee    = NULL,
        .pcconnect_baud   = 115200u,
        .pcconnect_dock   = BE300_PCC_DOCK_RS232,
        .serial0_bridge   = NULL,
        .serial0_tee      = NULL,
        .serial0_baud     = 115200u,
        .serial1_bridge   = NULL,
        .serial1_tee      = NULL,
        .serial1_baud     = 115200u,
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0) {
            cfg.trace = true;
        } else if (strcmp(argv[i], "--log-mmio") == 0) {
            cfg.log_mmio = true;
        } else if (strcmp(argv[i], "--ppsh") == 0) {
            cfg.enable_ppsh = true;
        } else if (strcmp(argv[i], "--pcconnect-bridge") == 0 && i + 1 < argc) {
            cfg.pcconnect_bridge = argv[++i];
        } else if (strcmp(argv[i], "--pcconnect-tee") == 0 && i + 1 < argc) {
            cfg.pcconnect_tee = argv[++i];
        } else if (strcmp(argv[i], "--pcconnect-baud") == 0 && i + 1 < argc) {
            char *end = NULL;
            unsigned long b = strtoul(argv[++i], &end, 0);
            if (!end || *end != '\0' || b > 1000000u) {
                fprintf(stderr,
                    "Error: --pcconnect-baud must be 0..1000000 (got '%s')\n",
                    argv[i]);
                return 1;
            }
            cfg.pcconnect_baud = (uint32_t)b;
        } else if (strcmp(argv[i], "--serial0-bridge") == 0 && i + 1 < argc) {
            cfg.serial0_bridge = argv[++i];
        } else if (strcmp(argv[i], "--serial0-tee") == 0 && i + 1 < argc) {
            cfg.serial0_tee = argv[++i];
        } else if (strcmp(argv[i], "--serial0-baud") == 0 && i + 1 < argc) {
            char *end = NULL;
            unsigned long b = strtoul(argv[++i], &end, 0);
            if (!end || *end != '\0' || b > 1000000u) {
                fprintf(stderr,
                    "Error: --serial0-baud must be 0..1000000 (got '%s')\n",
                    argv[i]);
                return 1;
            }
            cfg.serial0_baud = (uint32_t)b;
        } else if (strcmp(argv[i], "--serial1-bridge") == 0 && i + 1 < argc) {
            cfg.serial1_bridge = argv[++i];
        } else if (strcmp(argv[i], "--serial1-tee") == 0 && i + 1 < argc) {
            cfg.serial1_tee = argv[++i];
        } else if (strcmp(argv[i], "--serial1-baud") == 0 && i + 1 < argc) {
            char *end = NULL;
            unsigned long b = strtoul(argv[++i], &end, 0);
            if (!end || *end != '\0' || b > 1000000u) {
                fprintf(stderr,
                    "Error: --serial1-baud must be 0..1000000 (got '%s')\n",
                    argv[i]);
                return 1;
            }
            cfg.serial1_baud = (uint32_t)b;
        } else if (strcmp(argv[i], "--pcconnect-dock") == 0 && i + 1 < argc) {
            if (!parse_pcconnect_dock(argv[++i], &cfg.pcconnect_dock)) {
                fprintf(stderr,
                    "Error: --pcconnect-dock must be rs232 or usb-sync (got '%s')\n",
                    argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--rtc-host-time") == 0) {
            cfg.enable_rtc_host_time = true;
        } else if (strcmp(argv[i], "--stowaway-keyboard") == 0) {
            cfg.enable_stowaway_keyboard = true;
        } else if (strcmp(argv[i], "--fb-size") == 0 && i + 1 < argc) {
            if (!parse_fb_size(argv[++i], &cfg.fb_width, &cfg.fb_height,
                    &cfg.fb_stride)) {
                fprintf(stderr,
                    "Error: --fb-size must be 240x320 or experimental 480x640\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--ne2000") == 0) {
            cfg.enable_ne2000 = true;
        } else if (strcmp(argv[i], "--audio") == 0) {
            cfg.enable_audio = true;
        } else if (strcmp(argv[i], "--net-mac") == 0 && i + 1 < argc) {
            if (!parse_mac(argv[++i], cfg.net_mac)) {
                fprintf(stderr,
                    "Error: --net-mac must be an unicast MAC like 10:20:30:00:00:10\n");
                return 1;
            }
            cfg.net_mac_set = true;
        } else if (strcmp(argv[i], "--frame") == 0) {
            cfg.frame_visible = true;
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            if (!parse_scale(argv[++i], &cfg.scale)) {
                fprintf(stderr, "Error: --scale must be 1.0-4.0\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--nand") == 0 && i + 1 < argc) {
            cfg.nand_path = argv[++i];
        } else if (strcmp(argv[i], "--cf") == 0 && i + 1 < argc) {
            if (cfg.cf_count >= BE300_MAX_CF_SLOTS) {
                fprintf(stderr, "Error: at most %u --cf images are supported\n",
                    (unsigned)BE300_MAX_CF_SLOTS);
                return 1;
            }
            cfg.cf_paths[cfg.cf_count++] = argv[++i];
        } else if (strcmp(argv[i], "--restore") == 0) {
            cfg.restore = true;
        } else if (strcmp(argv[i], "--speed") == 0 && i + 1 < argc) {
            cfg.target_mhz = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mmio-coverage") == 0) {
            cfg.mmio_coverage = true;
        } else if (strcmp(argv[i], "--detect-stall") == 0) {
            cfg.detect_stall = true;
        } else if (strncmp(argv[i], "--stall-window=", 15) == 0) {
            cfg.stall_window = (uint32_t)strtoul(argv[i] + 15, NULL, 0);
            if (cfg.stall_window < 1000u) cfg.stall_window = 1000u;
        } else if (strncmp(argv[i], "--stall-threshold=", 18) == 0) {
            cfg.stall_unique_threshold = (uint32_t)strtoul(argv[i] + 18, NULL, 0);
        } else if (strncmp(argv[i], "--stall-wall-secs=", 18) == 0) {
            cfg.stall_wall_secs = (uint32_t)strtoul(argv[i] + 18, NULL, 0);
        } else if (strcmp(argv[i], "--sdram") == 0 && i + 1 < argc) {
            unsigned mb = (unsigned)atoi(argv[++i]);
            if (mb == 0 || mb > 64) {
                fprintf(stderr, "Error: --sdram must be 1-64 MB\n");
                return 1;
            }
            cfg.sdram_size = mb * 1024u * 1024u;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            cfg.rom_path = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!cfg.rom_path && !cfg.nand_path && !cfg.restore) {
        fprintf(stderr,
            "Error: must specify --nand, --restore, or a ROM image\n");
        usage(argv[0]);
        return 1;
    }
    if (cfg.restore && cfg.enable_ne2000) {
        fprintf(stderr, "Error: --restore requires the primary PCMCIA socket, so it cannot be combined with --ne2000\n");
        usage(argv[0]);
        return 1;
    }
    if (cfg.restore && cfg.cf_count == 0) {
        fprintf(stderr, "Error: --restore requires --cf <image>\n");
        usage(argv[0]);
        return 1;
    }
    if (cfg.enable_ne2000 && cfg.rom_path) {
        fprintf(stderr, "Error: --ne2000 is supported only with --nand boots\n");
        usage(argv[0]);
        return 1;
    }
    if (cfg.enable_ne2000 && cfg.cf_count > 0) {
        if (cfg.cf_count > 1) {
            fprintf(stderr,
                "Error: only one --cf image can be combined with --ne2000; "
                "it is attached to the secondary PCMCIA socket\n");
            usage(argv[0]);
            return 1;
        }
        cfg.cf_paths[1] = cfg.cf_paths[0];
        cfg.cf_paths[0] = NULL;
        cfg.cf_count = 2;
    }
    if (cfg.net_mac_set && !cfg.enable_ne2000) {
        fprintf(stderr, "Error: --net-mac requires --ne2000\n");
        usage(argv[0]);
        return 1;
    }
    /* --pcconnect-bridge, --serial1-bridge, and --stowaway-keyboard all
     * claim the VRC4173 companion SIU. At most one may be active. */
    {
        int vrc4173_claims = (cfg.pcconnect_bridge ? 1 : 0) +
                             (cfg.serial1_bridge ? 1 : 0) +
                             (cfg.enable_stowaway_keyboard ? 1 : 0);
        if (vrc4173_claims > 1) {
            fprintf(stderr,
                "Error: --pcconnect-bridge, --serial1-bridge, and --stowaway-keyboard "
                "all claim the VRC4173 dock UART; pick one\n");
            usage(argv[0]);
            return 1;
        }
    }
    if (cfg.pcconnect_tee && !cfg.pcconnect_bridge) {
        fprintf(stderr,
            "Error: --pcconnect-tee requires --pcconnect-bridge\n");
        usage(argv[0]);
        return 1;
    }
    if (cfg.serial0_tee && !cfg.serial0_bridge) {
        fprintf(stderr,
            "Error: --serial0-tee requires --serial0-bridge\n");
        usage(argv[0]);
        return 1;
    }
    if (cfg.serial1_tee && !cfg.serial1_bridge) {
        fprintf(stderr,
            "Error: --serial1-tee requires --serial1-bridge\n");
        usage(argv[0]);
        return 1;
    }
    if (cfg.pcconnect_dock == BE300_PCC_DOCK_USB_SYNC &&
        !cfg.pcconnect_bridge) {
        fprintf(stderr,
            "Error: --pcconnect-dock usb-sync requires --pcconnect-bridge\n");
        usage(argv[0]);
        return 1;
    }
    if (cfg.pcconnect_bridge) {
        pcc_bridge_config_t br = {0};
        if (!pcconnect_bridge_parse_spec(cfg.pcconnect_bridge, &br)) {
            usage(argv[0]);
            return 1;
        }
    }
    if (cfg.serial0_bridge) {
        pcc_bridge_config_t br = {0};
        if (!pcconnect_bridge_parse_spec(cfg.serial0_bridge, &br)) {
            usage(argv[0]);
            return 1;
        }
    }
    if (cfg.serial1_bridge) {
        pcc_bridge_config_t br = {0};
        if (!pcconnect_bridge_parse_spec(cfg.serial1_bridge, &br)) {
            usage(argv[0]);
            return 1;
        }
    }
    {
        int boot_modes = (cfg.rom_path    ? 1 : 0) +
                         (cfg.nand_path   ? 1 : 0) +
                         (cfg.restore     ? 1 : 0);
        if (boot_modes > 1) {
            fprintf(stderr,
                "Error: choose exactly one of --restore, --nand, or rom.bin\n");
            usage(argv[0]);
            return 1;
        }
    }

    if (cfg.trace) {
        verbose = 1;
        quiet_mode = 0;
    }

    machine_t *m = be300_create(&cfg);
    if (!m) {
        fprintf(stderr, "Failed to create machine\n");
        return 1;
    }

    be300_run(m);
    be300_destroy(m);
    return 0;
}
