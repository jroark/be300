#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "machine.h"

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
        "  --ram <file>          Preload a raw RAM image at PA 0x00000000\n"
        "  --sdram <MB>          SDRAM size in megabytes (default: 16)\n"
        "  -h, --help            Show this help\n"
        "\n"
        "ROM image (positional arg) is loaded at PA 0x1FC00000 (MIPS reset vector).\n"
        "--kernel and rom.bin are mutually exclusive.\n",
        prog);
}

int main(int argc, char *argv[])
{
    machine_config_t cfg = {
        .trace       = false,
        .log_mmio    = false,
        .rom_path    = NULL,
        .kernel_path = NULL,
        .cmdline     = NULL,
        .ram_path    = NULL,
        .sdram_size  = 16u * 1024u * 1024u,
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

    if (!cfg.kernel_path && !cfg.rom_path) {
        fprintf(stderr, "Error: must specify either --kernel <vmlinux> or a ROM image\n");
        usage(argv[0]);
        return 1;
    }
    if (cfg.kernel_path && cfg.rom_path) {
        fprintf(stderr, "Error: --kernel and rom.bin are mutually exclusive\n");
        usage(argv[0]);
        return 1;
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
