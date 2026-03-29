#!/bin/bash
# Phase 5: Relink Linux 2.4.18 kernel with Qt/E musl ramdisk
# Run inside kernel-build container:
#   docker compose run --rm kernel-build bash /work/opie-build/relink_kernel_qte.sh
set -e

WORK=/work

echo "=== Phase 5: Relink kernel with Qt/E ramdisk ==="

# Verify ramdisk exists
if [ ! -f "$WORK/opie-build/ramdisk-qte.ext2.gz" ]; then
    echo "ERROR: ramdisk-qte.ext2.gz not found. Run create_ramdisk_musl.sh first."
    exit 1
fi

# Verify kernel build tree exists
KBUILD=$WORK/kernels/build/linux4be-gcc3
if [ ! -d "$KBUILD" ]; then
    echo "ERROR: Kernel build tree not found at $KBUILD"
    exit 1
fi

export PATH=/toolchain/usr/bin:$PATH
export GCC_EXEC_PREFIX=/toolchain/usr/lib/gcc-lib/mipsel-alt-linux/3.2.1/

echo "--- Replacing ramdisk ---"
cd $KBUILD/arch/mips/ramdisk
cp $WORK/opie-build/ramdisk-qte.ext2.gz ramdisk.gz
ls -lh ramdisk.gz

echo "--- Rebuilding ramdisk.o ---"
mipsel-linux-ld -r -T ld.script -b binary -o ramdisk.o ramdisk.gz
ls -lh ramdisk.o

echo "--- Relinking vmlinux ---"
cd $KBUILD
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

echo "--- Verify ---"
ls -lh vmlinux
mipsel-linux-nm vmlinux | grep "__rd_start\|__rd_end"
strings vmlinux | grep "Linux version" | head -1

# Copy to final location
cp vmlinux $WORK/kernels/vmlinux-qte
echo ""
echo "=== Kernel built: $WORK/kernels/vmlinux-qte ==="
ls -lh $WORK/kernels/vmlinux-qte

echo ""
echo "To boot on the emulator (run from host terminal):"
echo "  ./build-host/be300 --kernel kernels/vmlinux-qte \\"
echo "    --cmdline \"console=tty0 root=/dev/ram mem=32M\" --sdram 32"
