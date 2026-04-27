#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "be300.h"

/* GXemul global verbosity gates — see gxemul/src/core/debugmsg.c.
 * debug() is silent while emul_executing unless verbose >= 1. */
extern int verbose;
extern int quiet_mode;

static bool parse_frame_scale(const char *arg, double *scale_out)
{
    char *end = NULL;
    double scale;

    if (!arg || !*arg)
        return false;

    scale = strtod(arg, &end);
    if (end == arg)
        return false;
    if (*end == 'x' || *end == 'X')
        end++;
    if (*end != '\0' || !(scale >= 1.0 && scale <= 4.0))
        return false;

    *scale_out = scale;
    return true;
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
        "  --cf <image>          Attach a FAT16 CF image\n"
        "  --restore             Enter CF recovery boot mode (requires --cf)\n"
        "  --sdram <MB>          SDRAM size in megabytes (default: 16)\n"
        "  --ppsh                Enable PPSH (parallel port debug shell) probe\n"
        "  --pcconnect-time-sync Enable experimental Casio PC Connect time-sync peer\n"
        "  --frame-2x            Show framed SDL display with a 480x640 LCD area\n"
        "  --frame-scale <N>     Framed SDL LCD scale, 1.0-4.0 (default: 1.0)\n"
        "  --speed <mhz>         Target CPU MHz (default: 166 = real hardware, 0 = unthrottled)\n"
        "  --mmio-coverage       First-hit log per (device, offset, op) + shutdown coverage table\n"
        "  --detect-stall        Emit [BE300_STALL] when guest spins on a tight PC set\n"
        "  --stall-window=N      Instructions per sampling bucket (default 10000)\n"
        "  --stall-threshold=K   Fire when unique PCs in window < K (default 64)\n"
        "  --stall-wall-secs=T   Sustained wall-seconds below threshold before firing (default 5)\n"
        "  -h, --help            Show this help\n"
        "\n"
        "ROM image (positional arg) is loaded at PA 0x1FC00000 (MIPS reset vector).\n"
        "--restore/--nand and rom.bin are mutually exclusive.\n",
        prog);
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    machine_config_t cfg = {
        .trace           = false,
        .log_mmio        = false,
        .log_nand_legacy = false,
        .enable_ppsh     = false,
        .enable_pcconnect_time_sync = false,
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
        .cf_path        = NULL,
        .sdram_size     = 16u * 1024u * 1024u,
        .target_mhz     = 166u,
        .frame_lcd_scale = 0.0,
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0) {
            cfg.trace = true;
        } else if (strcmp(argv[i], "--log-mmio") == 0) {
            cfg.log_mmio = true;
        } else if (strcmp(argv[i], "--ppsh") == 0) {
            cfg.enable_ppsh = true;
        } else if (strcmp(argv[i], "--pcconnect-time-sync") == 0) {
            cfg.enable_pcconnect_time_sync = true;
        } else if (strcmp(argv[i], "--frame-2x") == 0) {
            cfg.frame_lcd_scale = 2.0;
        } else if (strcmp(argv[i], "--frame-scale") == 0 && i + 1 < argc) {
            if (!parse_frame_scale(argv[++i], &cfg.frame_lcd_scale)) {
                fprintf(stderr, "Error: --frame-scale must be 1.0-4.0\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--nand") == 0 && i + 1 < argc) {
            cfg.nand_path = argv[++i];
        } else if (strcmp(argv[i], "--cf") == 0 && i + 1 < argc) {
            cfg.cf_path = argv[++i];
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
        fprintf(stderr, "Error: must specify --nand, --restore, or a ROM image\n");
        usage(argv[0]);
        return 1;
    }
    if (cfg.restore && !cfg.cf_path) {
        fprintf(stderr, "Error: --restore requires --cf <image>\n");
        usage(argv[0]);
        return 1;
    }
    {
        int boot_modes = (cfg.rom_path    ? 1 : 0) +
                         ((cfg.nand_path || cfg.restore) ? 1 : 0);
        if (boot_modes > 1) {
            fprintf(stderr, "Error: --restore/--nand and rom.bin are mutually exclusive\n");
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
