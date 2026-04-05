# be300 emulator

Casio BE-300 (NEC VR4131 MIPS little-endian) emulator using a modified fork of
the [GXemul](http://gavare.se/gxemul/) 0.7.0 CPU engine. The emulator boots
Linux 2.4, 2.6, and 4.2.9 kernels to userspace, and cold-boots Windows CE 3.0/4.0
from a real 16KB boot ROM dumped from hardware.

Available as a native desktop app (macOS, Linux), a
[web app](https://linux4be.com), and mobile apps for Android and iOS.

## Screenshots

### Kernel 2.6
![Kernel 2.6](vmlinux-2.6.png)

### Kernel 2.4 vmlinux-pgui-demo
![Kernel 2.4](vmlinux-pgui-demo.png)

### WinCE 3.0
![WinCE 3.0](All_nand_300.png)

## Supported Kernels

| Kernel | Version | Notes |
|--------|---------|-------|
| `vmlinux-pgui-demo` | Linux 2.4.18 | Known good kernel and ramdisk (PicoGUI demo) |
| `vmlinux` | Linux 2.4.18 | Statically linked, not stripped |
| `vmlinux-mw` | Linux 2.4.18 | |
| `vmlinux-pgui-test1` | Linux 2.4.18 | |
| `vmlinux-pgui-fbtest` | Linux 2.4.18 | Framebuffer test |
| `vmlinux_sdlregtest` | Linux 2.4.18 | SDL register test |
| `vmlinux-qte` | Linux 2.4.18 | Qt/Embedded |
| `vmlinux-2.6` | Linux 2.6.8.1 | Use `--sfb-5bit-green` for correct colors |
| `vmlinux-4.2.9` | Linux 4.2.9 | Multi-page userspace support |

All kernels boot to userspace on both real hardware and the emulator. The 2.4
kernels use `--cmdline` for boot parameters; the 2.6 kernel ignores `--cmdline`
(hardcoded command line in prom_init).

## WinCE NAND Images

| Image | Version | SPL |
|-------|---------|-----|
| `All_nand_300.bin` | WinCE 3.0 | v0.52 |
| `org_CE_30.bin` | WinCE 3.0 | v0.60 |
| `BE500.bin` | BE-500 variant | v0.62 |
| `CE_Net.bin` | WinCE 4.0 | v0.62 |

WinCE boots via cold boot from the ROM reset vector (0xBFC00000). The ROM reads
NAND, loads the SPL bootloader, SPL decompresses NK.exe into RAM, then the ROM's
boot dispatcher hands off to the WinCE kernel.

## Prerequisites

- CMake 3.10+
- C11 compiler (GCC or Clang)
- SDL2 (optional -- builds headless without it)

## Building

```bash
git clone --recurse-submodules <repo-url>
cd be300-framebuffer
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

If you already cloned without submodules:
```bash
git submodule update --init
```

## Usage

```
Usage: be300 [options] [rom.bin]

Options:
  --kernel <vmlinux>    Load ELF kernel directly
  --cmdline <string>    Kernel command line passed via bootinfo
  --nand <image>        Boot WinCE from NAND dump (B000FF SPL loader)
  --sfb-5bit-green      Use 5-bit green expansion for 2.6 sfb.c
  --ram <file>          Preload a raw RAM image at PA 0x00000000
  --sdram <MB>          SDRAM size in megabytes (default: 16)
  --speed <mhz>         Target CPU MHz (default: 166 = real hardware, 0 = unthrottled)
  --trace               Print each executed instruction to stderr
  --log-mmio            Log all MMIO register reads/writes
  -h, --help            Show this help
```

`--kernel`, `--nand`, and a positional ROM image are mutually exclusive.

### Running Linux 2.4

```bash
./be300 --kernel ../kernels/vmlinux-pgui-demo \
  --cmdline "console=tty0 root=/dev/ram"
```

### Running Linux 2.6

```bash
./be300 --kernel ../kernels/vmlinux-2.6 --sfb-5bit-green
```

### Running Windows CE

```bash
./be300 --nand ../ce/restore_images/All_nand_300.bin
```

## Cross-Development Toolchain (Docker)

A Docker container provides MIPS cross-tools for analyzing kernels and WinCE
binaries:

```bash
docker compose build mips-dev
docker compose run --rm mips-dev /bin/bash
```

Includes `mipsel-linux-gnu-*` (Linux ELF) and `mipsel-pe-*` (Windows CE PE)
toolchains.
