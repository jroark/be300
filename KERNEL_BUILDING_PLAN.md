# Plan: Build Linux 2.4.18 Kernel for BE-300 Emulator

## Context
The BE-300 emulator has pre-built vmlinux kernels (Linux 2.4.18, GCC 3.0.x) that boot successfully. We want to build a kernel from source. The existing source tree at `kernels/src/linux-2.4.18/` has all BE-300/VR4131-specific code but is **missing all Makefiles** (linux-mips.org CVS export). We'll merge it with vanilla 2.4.18 from kernel.org and build with modern GCC + compat flags.

## Step 1: Download vanilla Linux 2.4.18
In Docker container:
```bash
cd /work/kernels/build
wget https://cdn.kernel.org/pub/linux/kernel/v2.4/linux-2.4.18.tar.gz
tar xzf linux-2.4.18.tar.gz
mv linux-2.4.18 linux-2.4.18-be300
```
This provides all Makefiles, Rules.make, linker scripts, and the complete 2.4 build system.

## Step 2: Overlay linux-mips.org BE-300 files

### Full directory overlays (replace vanilla with linux-mips.org versions):
- `arch/mips/` — VR41xx CPU support, BE-300 board support, MIPS boot, linker scripts
- `include/asm-mips/` — VR4131 register defs, linux_logo_be300.h, linux_logo.h

### Individual BE-300 files to copy into vanilla tree:
| Source file | Description |
|---|---|
| `drivers/video/sfb.c` | Simple framebuffer (240x320, 16bpp, base 0xaa200000) |
| `drivers/char/serial_be300.c` | BE-300 UART driver (base 0xAA008680) |
| `drivers/char/serial_be300.h` | UART config constants |
| `drivers/char/scan_be300.c` | BE-300 keyboard scanner |

### Vanilla files to overwrite with linux-mips.org versions:
| File | What changes |
|---|---|
| `drivers/char/tty_io.c` | Adds `be300_console_init()` (line ~2267, gated by `CONFIG_SERIAL_BE300_CONSOLE`) and `be300_serial_init()` (line ~2361, gated by `CONFIG_SERIAL_BE300`) |
| `drivers/char/Config.in` | Adds `CONFIG_SERIAL_BE300` and `CONFIG_SERIAL_BE300_CONSOLE` menu entries (~line 76) |
| `include/asm-mips/linux_logo.h` | Conditional BE-300 logo when `CONFIG_CASIO_BE300` is set |

## Step 3: Framebuffer integration (Config.in, Makefile, fbmem.c)

The config `linux4be-cf.config` uses `CONFIG_FB_SIMPLE=y` which does NOT exist in vanilla 2.4.18's `drivers/video/Config.in`. Three files need editing:

### 3a: `drivers/video/Config.in` — Add sfb config entry
Add after the `CONFIG_FB_TX3912` block (~line 199), before the `CONFIG_FB_VIRTUAL` entry:
```
if [ "$CONFIG_CASIO_BE300" = "y" ]; then
   bool '  Simple frame buffer' CONFIG_FB_SIMPLE
   if [ "$CONFIG_FB_SIMPLE" = "y" ]; then
      hex '    Video RAM base address' CONFIG_FB_SIMPLE_MEMBASE aa200000
      hex '    Video RAM size (0=auto)' CONFIG_FB_SIMPLE_MEMSIZE 0
   fi
fi
```
Also add `CONFIG_FB_SIMPLE` to the "Guess what we need" CFB16 auto-select block (~line 296).

### 3b: `drivers/video/Makefile` — Add sfb build rule
Add to the obj rules section:
```
obj-$(CONFIG_FB_SIMPLE)           += sfb.o
```

### 3c: `drivers/video/fbmem.c` — Register sfb in fb_drivers[] init table
Add extern declarations (~line 131):
```c
extern int sfb_init(void);
extern int sfb_setup(char*);
```
Add to `fb_drivers[]` array:
```c
#ifdef CONFIG_FB_SIMPLE
	{ "sfb", sfb_init, sfb_setup },
#endif
```

## Step 4: Create missing board-level Makefiles

Vanilla 2.4.18 won't have Makefiles for linux-mips.org's VR41xx additions:

| Makefile path | Contents |
|---|---|
| `arch/mips/vr41xx/Makefile` | `subdir-$(CONFIG_VR4122) += vr4122`; `subdir-$(CONFIG_VR4131) += vr4131` |
| `arch/mips/vr41xx/vr4131/Makefile` | `subdir-$(CONFIG_CASIO_BE300) += casio-be300` |
| `arch/mips/vr41xx/vr4131/casio-be300/Makefile` | `O_TARGET = be300.o`; `obj-y = setup.o`; `obj-$(CONFIG_BLK_DEV_IDE) += ide-be300.o` |
| `arch/mips/vr41xx/vr4122/Makefile` | `subdir-y += common` |
| `arch/mips/vr41xx/vr4122/common/Makefile` | Build common VR4122 objects |

Also check/update:
- `arch/mips/Makefile` — ensure VR41xx/casio-be300 entries exist in SUBDIRS and core-y
- `drivers/char/Makefile` — ensure `serial_be300.o` and `scan_be300.o` rules exist

## Step 5: Fix missing headers
- `include/asm-mips/vr4122/vr4122.h` — create stub if not present (VR4131 is superset)
- Any other missing headers discovered during build

## Step 6: Configure the kernel
Use `linux4be-cf.config` (NOT `config_be300` — the latter has `CONFIG_SERIAL_BE300` unset, breaking serial console output).

```bash
cd /work/kernels/build/linux-2.4.18-be300
cp /work/kernels/src/linux-2.4.18/linux4be-cf.config .config
make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- oldconfig
make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- dep
```

Key config settings: `CONFIG_CASIO_BE300=y`, `CONFIG_CPU_VR41XX=y`, `CONFIG_SERIAL_BE300=y`, `CONFIG_SERIAL_BE300_CONSOLE=y`, `CONFIG_FB=y`, `CONFIG_FB_SIMPLE=y`, `CONFIG_BLK_DEV_INITRD=y`

## Step 7: Build with modern GCC + compat flags
Add to top-level Makefile CFLAGS:
```
-fgnu89-inline -fcommon -fno-strict-aliasing -Wno-error -w
```
Then:
```bash
make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- vmlinux -j$(nproc)
```
Expect iterative fixes for GCC 13 incompatibilities (inline asm, type conflicts, etc.).

## Step 8: Insert ramdisk
```bash
python3 /work/kernels/scripts/ramdisk_tool.py insert \
  /work/kernels/build/linux-2.4.18-be300/vmlinux \
  /work/kernels/ramdisk-pgui-full.gz
```
Uses `__rd_start`/`__rd_end` symbols from `arch/mips/ld.script.in`.

## Step 9: Test in the emulator
```bash
# From host, after copying vmlinux out of Docker
gtimeout 20s ./build-host/be300 \
  --cmdline "console=tty0 console=ttyS0,9600 root=/dev/ram" \
  --kernel kernels/build/linux-2.4.18-be300/vmlinux \
  > custom_stdout.log 2> custom_stderr.log
```
Compare serial output and framebuffer against known-good `vmlinux-pgui-demo`.

## Key Files
- `kernels/src/linux-2.4.18/linux4be-cf.config` — config with serial + FB enabled
- `kernels/src/linux-2.4.18/arch/mips/vr41xx/vr4131/casio-be300/setup.c` — board init
- `kernels/src/linux-2.4.18/drivers/char/serial_be300.c` — serial driver
- `kernels/src/linux-2.4.18/drivers/video/sfb.c` — framebuffer driver (exports `sfb_init()`, `sfb_setup()`)
- `kernels/src/linux-2.4.18/drivers/char/tty_io.c` — modified tty with BE-300 hooks
- `kernels/src/linux-2.4.18/drivers/video/fbmem.c` — needs `sfb` entry in `fb_drivers[]`
- `kernels/src/linux-2.4.18/drivers/video/Config.in` — needs `CONFIG_FB_SIMPLE` entry
- `kernels/scripts/ramdisk_tool.py` — ramdisk insert tool
- `Dockerfile` / `docker-compose.yml` — mips-dev container

## Fallback: GCC Compatibility
If GCC 13 produces too many hard errors even with compat flags:
1. Install `gcc-10` in the container
2. Create a separate Docker container with GCC 3.x (Debian Sarge based)
