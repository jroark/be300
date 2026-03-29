#!/bin/bash
# Phase 5: Build Linux 2.4.18 kernel with Qt/E ramdisk (16MB)
# Run inside mips-dev container: docker compose run --rm mips-dev bash /work/opie-build/03_build_kernel.sh
#
# This modifies build_2418_kernel.sh to use:
# - autobuild-qte.config (CONFIG_BLK_DEV_RAM_SIZE=16384)
# - ramdisk-qte.ext2.gz (Qt/Embedded ramdisk)
set -e

WORK=/work

echo "=== Phase 5: Build kernel with Qt/E ramdisk ==="

# Verify ramdisk exists
if [ ! -f "$WORK/opie-build/ramdisk-qte.ext2.gz" ]; then
    echo "ERROR: ramdisk-qte.ext2.gz not found. Run 02_create_ramdisk.sh first."
    exit 1
fi

# Temporarily swap the config and ramdisk for the build script
SRCOVERLAY=$WORK/kernels/src/linux-2.4.18

# Backup originals
cp $SRCOVERLAY/autobuild.config $SRCOVERLAY/autobuild.config.bak 2>/dev/null || true

# Use Qt/E config
cp $SRCOVERLAY/autobuild-qte.config $SRCOVERLAY/autobuild.config

# Run the standard kernel build script
bash $WORK/kernels/scripts/build_2418_kernel.sh

# The build script uses ramdisk-pgui-full.gz. We need to replace it.
# Actually, let's patch the ramdisk into the built kernel using ramdisk_tool.py
BUILDDIR=$WORK/kernels/build/linux-2.4.18-be300

# Rebuild the ramdisk object with the Qt/E ramdisk
cd $BUILDDIR/arch/mips/ramdisk
cp $WORK/opie-build/ramdisk-qte.ext2.gz ramdisk.gz
mipsel-linux-gnu-ld -r -T ld.script -b binary -o ramdisk.o ramdisk.gz
echo "Ramdisk object rebuilt with Qt/E ramdisk"
ls -lh ramdisk.gz ramdisk.o

# Relink the kernel with the new ramdisk
cd $BUILDDIR
make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- vmlinux -j$(nproc)

echo "=== Verify ==="
ls -lh vmlinux
mipsel-linux-gnu-nm vmlinux | grep "__rd_start\|__rd_end"
strings vmlinux | grep "Linux version"

# Copy to final location
cp vmlinux $WORK/kernels/vmlinux-qte
echo "=== Kernel built: $WORK/kernels/vmlinux-qte ==="
ls -lh $WORK/kernels/vmlinux-qte

# Restore original config
cp $SRCOVERLAY/autobuild.config.bak $SRCOVERLAY/autobuild.config 2>/dev/null || true

echo ""
echo "To boot on the emulator (run from host terminal):"
echo "  ./build-host/be300 --kernel kernels/vmlinux-qte \\"
echo "    --cmdline \"console=tty0 console=ttyS0,9600 root=/dev/ram mem=32M\" \\"
echo "    --sdram 32"
