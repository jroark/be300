# Plan: MIPS-II Toolchain + Qt/Embedded on BE-300 Emulator

## Context

We have Qt/Embedded 2.3.10 cross-compiled and a working framebuffer pipeline — a static MIPS-II binary successfully draws color bars to `/dev/fb0` on the emulated BE-300. However, the Qt/E library and hello app were built with Ubuntu's `mipsel-linux-gnu` toolchain which produces glibc/libstdc++ containing **actual MIPS32r2 instructions**. These crash with "Illegal instruction" on the MIPS-II VR4131 CPU.

**Goal:** Get a MIPS-II compatible C/C++ toolchain, rebuild Qt/E against it, and render the Qt hello world on the framebuffer.

## What Works Already

- Linux 2.4.18 boots to userspace with BusyBox shell (uClibc MIPS-II)
- Static MIPS-II binaries write to `/dev/fb0` successfully
- Qt/E 2.3.10 source compiles (libqte.so 4.7MB) — just linked against wrong libc
- Kernel with CONFIG_BLK_DEV_RAM_SIZE=16384 built and tested
- Correct cmdline: `console=tty0 root=/dev/ram mem=32M` (NO ttyS0)

## Lessons Learned

- `console=ttyS0,9600` hangs all userspace — serial driver is incomplete
- ELF `EF_MIPS_ARCH` flags: kernel rejects MIPS64 (0x70000000), accepts MIPS32 (0x60000000)
- glibc MIPS32r2 instructions genuinely crash — not just a flag issue
- Ramdisk must fit within CONFIG_BLK_DEV_RAM_SIZE
- BusyBox from pgui ramdisk (uClibc 0.9.15, MIPS-II) works perfectly

---

## Approach: Buildroot with musl, targeting MIPS-II

Buildroot explicitly supports `-march=mips2` and musl libc with libstdc++. It produces a complete sysroot we can use to rebuild Qt/E.

### Phase 1: Build Buildroot MIPS-II musl Toolchain

Run inside the mips-dev Docker container.

```bash
cd /work/opie-build
git clone --depth 1 https://git.buildroot.net/buildroot
cd buildroot
```

Configure for MIPS-II little-endian with musl + C++:

```
Target options:
  Target Architecture: MIPS (little endian)
  Target Architecture Variant: mips 2

Toolchain:
  C library: musl
  Enable C++ support: YES
  Build cross GDB for host: NO (save time)

Target packages:
  (nothing — we only need the toolchain/sysroot)

Build options:
  Number of jobs: $(nproc)
```

Save config and build:
```bash
make -j$(nproc)
```

**Output:** `output/host/bin/mipsel-linux-musl-gcc` and sysroot at `output/host/mipsel-buildroot-linux-musl/sysroot/`

**Expected build time:** 30-60 minutes

### Phase 2: Rebuild Qt/Embedded 2.3.10 Against musl Sysroot

The Qt/E source is already cloned and patched at `/work/opie-build/qte-opie/`. We already fixed `moc`, applied GCC 12 compat flags, and generated allmoc.cpp.

1. Create a new MIPS config in `configs/linux-mips-musl-g++-shared` pointing to the Buildroot toolchain
2. Set `CROSS_COMPILE` to `mipsel-linux-musl-`
3. Add `-march=mips2` to CFLAGS/CXXFLAGS
4. Reconfigure and rebuild:
   ```bash
   make clean
   echo yes | ./configure -qconfig qpe -depths 16 -xplatform linux-mips-musl-g++ -no-xft -no-qvfb
   # Restore native moc
   cp /work/opie-build/native-tools/moc bin/moc
   make -j$(nproc)
   ```

**Key output:** `libqte.so.2.3.10` linked against musl, MIPS-II instructions only

### Phase 3: Build Hello App Against musl

```bash
mipsel-linux-musl-g++ -march=mips2 -o hello \
    -I$QTDIR/include -L$QTDIR/lib \
    -Wl,-rpath,/opt/QtPalmtop/lib \
    -DQWS hello.cpp -lqte -lm
```

### Phase 4: Assemble Ramdisk

16MB ext2 ramdisk containing:
- BusyBox (pgui version, uClibc MIPS-II) — for init/shell
- musl libc runtime (`ld-musl-mipsel.so.1`, `libc.so`) — for Qt/E + hello
- musl libstdc++ — for C++
- libqte.so.2 (rebuilt against musl)
- hello binary

Init script (NO ttyS0):
```bash
#!/bin/sh
echo "BE-300 Qt/Embedded Demo"
export LD_LIBRARY_PATH=/opt/QtPalmtop/lib:/lib
export QWS_DISPLAY=LinuxFb:/dev/fb0
export QWS_MOUSE_PROTO=none
export QWS_KEYBOARD=none
/opt/QtPalmtop/bin/hello -qws
```

### Phase 5: Link Kernel and Test

The kernel is built from `kernels/src/linux4be-2.4.18-20021129` using GCC 3.2.1 in the `kernel-build` Docker container. Object files persist in `kernels/build/linux4be-gcc3/`. To swap a new ramdisk, we only need to rebuild `ramdisk.o` and relink — no full kernel recompile needed.

**Prerequisites:**
- `CONFIG_BLK_DEV_RAM_SIZE=16384` must be in the kernel config and `include/linux/autoconf.h`. This was already done; the recompiled `rd.o` and `block.o` are in the build tree.

**Ramdisk swap and relink (in kernel-build container):**
```bash
docker compose run --rm kernel-build bash -c '
export PATH=/toolchain/usr/bin:$PATH
export GCC_EXEC_PREFIX=/toolchain/usr/lib/gcc-lib/mipsel-alt-linux/3.2.1/
cd /work/kernels/build/linux4be-gcc3

# Replace ramdisk
cp /work/opie-build/ramdisk-qte.ext2.gz arch/mips/ramdisk/ramdisk.gz

# Rebuild ramdisk.o
cd arch/mips/ramdisk
mipsel-linux-ld -r -T ld.script -b binary -o ramdisk.o ramdisk.gz
cd /work/kernels/build/linux4be-gcc3

# Relink vmlinux (all other .o files are unchanged)
mipsel-linux-ld -G 0 -static -T arch/mips/ld.script \
    arch/mips/kernel/head.o arch/mips/kernel/init_task.o \
    init/main.o init/version.o \
    --start-group \
    arch/mips/kernel/kernel.o arch/mips/mm/mm.o kernel/kernel.o mm/mm.o fs/fs.o ipc/ipc.o \
    arch/mips/math-emu/fpu_emulator.o arch/mips/ramdisk/ramdisk.o \
    drivers/char/char.o drivers/block/block.o drivers/misc/misc.o drivers/net/net.o \
    drivers/media/media.o drivers/video/video.o net/network.o \
    arch/mips/lib/lib.a lib/lib.a \
    arch/mips/vr41xx/common/vr41xx.o arch/mips/vr41xx/vr4122/common/vr4122.o \
    arch/mips/vr41xx/vr4131/casio-be300/casio-be300.o \
    --end-group -o vmlinux

cp vmlinux /work/kernels/vmlinux-qte
'
```

**Key notes:**
- The GCC 3.2.1 toolchain is at `/toolchain/usr/bin/` — needs `GCC_EXEC_PREFIX` set to find `cc1`
- If `rd.o` or `block.o` need recompiling (e.g., CONFIG_BLK_DEV_RAM_SIZE change), use:
  ```bash
  mipsel-linux-gcc -B/toolchain/usr/lib/gcc-lib/mipsel-alt-linux/3.2.1/ \
      -B/toolchain/usr/mipsel-alt-linux/bin/ \
      -isystem /toolchain/usr/lib/gcc-lib/mipsel-alt-linux/3.2.1/include \
      -I include/asm/gcc -D__KERNEL__ -I include \
      [flags from make --dry-run] -c -o drivers/block/rd.o drivers/block/rd.c
  ```
- The link command was obtained via `make ARCH=mips CROSS_COMPILE=mipsel-linux- vmlinux --dry-run`

**For the pgui-demo kernel (4MB ramdisk limit):**
Ramdisks under 744KB can be swapped using `ramdisk_tool.py`:
```bash
python3 kernels/scripts/ramdisk_tool.py insert kernels/vmlinux-pgui-demo opie-build/ramdisk.ext2.gz
```

**Boot (user runs manually):**
```bash
./build-host/be300 --kernel kernels/vmlinux-qte \
  --cmdline "console=tty0 root=/dev/ram mem=32M" --sdram 32
```

---

## Critical Files

| File | Role |
|------|------|
| `opie-build/qte-opie/` | Qt/E source (already cloned + patched) |
| `opie-build/native-tools/moc` | Working native moc binary |
| `opie-build/qte-opie/src/moc/moc_compat.h` | moc GCC 12 compat fixes |
| `opie-build/hello.cpp` | Qt/E demo app source |
| `opie-build/busybox-pgui` | Working MIPS-II BusyBox |
| `opie-build/uclibc_*.so` | uClibc libs for BusyBox |
| `kernels/build/linux4be-gcc3/` | Kernel build tree for relinking |
| `kernels/scripts/ramdisk_tool.py` | Ramdisk insert tool |

## Verification

1. Check Buildroot toolchain produces MIPS-II: `mipsel-linux-musl-gcc -march=mips2 -o test test.c && readelf -A test | grep ISA`
2. Check libqte links against musl: `readelf -d libqte.so.2 | grep NEEDED` (should show musl, not glibc)
3. Check no MIPS32r2 in the binary: `readelf -h hello | grep Flags` (should show mips2, not mips32r2)
4. Boot and see Qt hello on framebuffer
