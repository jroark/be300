#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "machine.h"
#include "ui.h"

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] [rom.bin]\n"
        "\n"
        "Options:\n"
        "  --kernel <vmlinux>    Load ELF kernel directly (bypass ROM boot)\n"
        "  --cmdline <string>    Kernel command line passed via $a1\n"
        "  --trace               Print each executed instruction to stderr\n"
        "  --log-mmio            Log all MMIO register reads/writes\n"
        "  --sfb-5bit-green      Use 5-bit green expansion for 2.6 sfb.c (non-standard\n"
        "                        R5X1G5B5 layout); default is pass-through (true RGB565,\n"
        "                        correct for Linux 2.4 kernels)\n"
        "  --kuseg-hotpath-populate\n"
        "                        Experimental: in LOAD_EMU/STORE_EMU kuseg accesses,\n"
        "                        attempt shadow-TLB-backed VA->PA population before\n"
        "                        falling back to empty block mapping\n"
        "  --log-nand-legacy     Log D7F8/D7FC indexed register traffic (with PC)\n"
        "  --log-wince-stall     Log WinCE post-NAND stall diagnostics\n"
        "  --wince-delay-skip    Experimental: replay old WinCE bootmode delay skip\n"
        "  --wince-hw-seed       Experimental: replay captured BE-300 WinCE RAM state\n"
        "  --wince-hw-seed-clear-callback-slot\n"
        "                        Experimental: after WinCE HW seed, clear PA 0x006694F4\n"
        "  --wince-hw-seed-clear-callback-target\n"
        "                        Experimental: after WinCE HW seed, clear PA 0x006694EC\n"
        "  --wince-hw-seed-clear-caller-restore-s0\n"
        "                        Experimental: after WinCE HW seed, clear PA 0x000017B0\n"
        "  --wince-hw-seed-force-alt-entry-prologue\n"
        "                        Experimental: after WinCE HW seed, force ctx saved RA to 0x80096800\n"
        "  --wince-hw-seed-skip-caller-frame\n"
        "                        Experimental: do not replay the caller_frame seed region\n"
        "  --wince-hw-seed-clear-future-frame\n"
        "                        Experimental: after WinCE HW seed, clear PA 0x00001760/64\n"
        "  --wince-collapse-double-helper-entry\n"
        "                        Experimental: skip duplicate WinCE helper entry at 0x80096790\n"
        "  --wince-obj-bootstrap Experimental: seed narrow WinCE objptr/header words\n"
        "  --trace-user-handoff  Debug first user handoff fault/map details (verbose)\n"
        "  --ram <file>          Preload a raw RAM image at PA 0x00000000\n"
        "  --nand <image>        Boot WinCE from NAND dump (B000FF SPL loader)\n"
        "  --sdram <MB>          SDRAM size in megabytes (default: 16)\n"
        "  -h, --help            Show this help\n"
        "\n"
        "ROM image (positional arg) is loaded at PA 0x1FC00000 (MIPS reset vector).\n"
        "--kernel, --nand, and rom.bin are mutually exclusive.\n",
        prog);
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    machine_config_t cfg = {
        .trace          = false,
        .log_mmio       = false,
        .sfb_5bit_green = false,
        .kuseg_hotpath_populate = false,
        .log_wince_stall = false,
        .wince_delay_skip = false,
        .wince_hw_seed = false,
        .wince_hw_seed_clear_callback_slot = false,
        .wince_hw_seed_clear_callback_target = false,
        .wince_hw_seed_clear_caller_restore_s0 = false,
        .wince_hw_seed_force_alt_entry_prologue = false,
        .wince_hw_seed_skip_caller_frame = false,
        .wince_hw_seed_clear_future_frame = false,
        .wince_collapse_double_helper_entry = false,
        .wince_obj_bootstrap = false,
        .trace_user_handoff = false,
        .rom_path       = NULL,
        .kernel_path    = NULL,
        .cmdline        = NULL,
        .ram_path       = NULL,
        .nand_path      = NULL,
        .sdram_size     = 16u * 1024u * 1024u,
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--kernel") == 0 && i + 1 < argc) {
            cfg.kernel_path = argv[++i];
        } else if (strcmp(argv[i], "--cmdline") == 0 && i + 1 < argc) {
            cfg.cmdline = argv[++i];
        } else if (strcmp(argv[i], "--trace") == 0) {
            cfg.trace = true;
        } else if (strcmp(argv[i], "--log-mmio") == 0) {
            cfg.log_mmio = true;
        } else if (strcmp(argv[i], "--sfb-5bit-green") == 0) {
            cfg.sfb_5bit_green = true;
        } else if (strcmp(argv[i], "--kuseg-hotpath-populate") == 0) {
            cfg.kuseg_hotpath_populate = true;
        } else if (strcmp(argv[i], "--trace-user-handoff") == 0) {
            cfg.trace_user_handoff = true;
        } else if (strcmp(argv[i], "--log-nand-legacy") == 0) {
            cfg.log_nand_legacy = true;
        } else if (strcmp(argv[i], "--log-wince-stall") == 0) {
            cfg.log_wince_stall = true;
        } else if (strcmp(argv[i], "--wince-delay-skip") == 0) {
            cfg.wince_delay_skip = true;
        } else if (strcmp(argv[i], "--wince-hw-seed") == 0) {
            cfg.wince_hw_seed = true;
        } else if (strcmp(argv[i], "--wince-hw-seed-clear-callback-slot") == 0) {
            cfg.wince_hw_seed_clear_callback_slot = true;
        } else if (strcmp(argv[i], "--wince-hw-seed-clear-callback-target") == 0) {
            cfg.wince_hw_seed_clear_callback_target = true;
        } else if (strcmp(argv[i], "--wince-hw-seed-clear-caller-restore-s0") == 0) {
            cfg.wince_hw_seed_clear_caller_restore_s0 = true;
        } else if (strcmp(argv[i], "--wince-hw-seed-force-alt-entry-prologue") == 0) {
            cfg.wince_hw_seed_force_alt_entry_prologue = true;
        } else if (strcmp(argv[i], "--wince-hw-seed-skip-caller-frame") == 0) {
            cfg.wince_hw_seed_skip_caller_frame = true;
        } else if (strcmp(argv[i], "--wince-hw-seed-clear-future-frame") == 0) {
            cfg.wince_hw_seed_clear_future_frame = true;
        } else if (strcmp(argv[i], "--wince-collapse-double-helper-entry") == 0) {
            cfg.wince_collapse_double_helper_entry = true;
        } else if (strcmp(argv[i], "--wince-obj-bootstrap") == 0) {
            cfg.wince_obj_bootstrap = true;
        } else if (strcmp(argv[i], "--nand") == 0 && i + 1 < argc) {
            cfg.nand_path = argv[++i];
        } else if (strcmp(argv[i], "--ram") == 0 && i + 1 < argc) {
            cfg.ram_path = argv[++i];
        } else if (strcmp(argv[i], "--sdram") == 0 && i + 1 < argc) {
            unsigned mb = (unsigned)atoi(argv[++i]);
            if (mb == 0 || mb > 64) {
                fprintf(stderr, "Error: --sdram must be 1–64 MB\n");
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

    if (!cfg.kernel_path && !cfg.rom_path && !cfg.nand_path) {
        fprintf(stderr, "Error: must specify --kernel, --nand, or a ROM image\n");
        usage(argv[0]);
        return 1;
    }
    {
        int boot_modes = (cfg.kernel_path ? 1 : 0) +
                         (cfg.rom_path    ? 1 : 0) +
                         (cfg.nand_path   ? 1 : 0);
        if (boot_modes > 1) {
            fprintf(stderr, "Error: --kernel, --nand, and rom.bin are mutually exclusive\n");
            usage(argv[0]);
            return 1;
        }
    }

    machine_t *m = machine_create(&cfg);
    if (!m) {
        fprintf(stderr, "Failed to create machine\n");
        return 1;
    }

    machine_run(m);
    machine_destroy(m);
    return 0;
}
