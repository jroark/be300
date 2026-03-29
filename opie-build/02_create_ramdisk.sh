#!/bin/bash
# Phase 4: Create Qt/E ramdisk for BE-300 emulator
# Run inside mips-dev container: docker compose run --rm mips-dev bash /work/opie-build/02_create_ramdisk.sh
set -e

BUILDDIR=/work/opie-build
ROOTFS=$BUILDDIR/rootfs
QTDIR=$BUILDDIR/qte-opie
RAMDISK=$BUILDDIR/ramdisk-qte.ext2

for tool in gcc g++ ar strip ld nm objcopy ranlib; do
    ln -sf /usr/bin/mipsel-linux-gnu-$tool /usr/local/bin/mipsel-linux-$tool
done

echo "=== Phase 4: Create Qt/E Ramdisk ==="

# --- Step 1: Build static BusyBox ---
echo "--- Building BusyBox ---"
BBVER=1.24.2
BBDIR=$BUILDDIR/busybox-$BBVER
if [ ! -f "$BBDIR/busybox" ]; then
    cd $BUILDDIR
    if [ ! -f busybox-${BBVER}.tar.bz2 ]; then
        wget -q http://busybox.net/downloads/busybox-${BBVER}.tar.bz2
    fi
    tar xf busybox-${BBVER}.tar.bz2
    cd $BBDIR
    make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- defconfig
    # Enable static build
    sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
    # Disable RPC (missing headers)
    sed -i 's/CONFIG_FEATURE_INETD_RPC=y/# CONFIG_FEATURE_INETD_RPC is not set/' .config
    # Use internal crypt
    sed -i 's/CONFIG_USE_BB_CRYPT=y/CONFIG_USE_BB_CRYPT=y/' .config
    sed -i 's/CONFIG_USE_BB_CRYPT_SHA=y/CONFIG_USE_BB_CRYPT_SHA=y/' .config
    make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- -j$(nproc) 2>&1 | tail -5
    echo "BusyBox built:"
    ls -lh busybox
    file busybox
else
    echo "BusyBox already built"
fi

# --- Step 2: Create rootfs directory structure ---
echo "--- Creating rootfs ---"
rm -rf $ROOTFS
mkdir -p $ROOTFS/{bin,sbin,lib,dev,etc/init.d,proc,sys,tmp,var,root,opt/QtPalmtop/{bin,lib}}

# --- Step 3: Install BusyBox ---
echo "--- Installing BusyBox ---"
cp $BBDIR/busybox $ROOTFS/bin/
cd $ROOTFS/bin
for cmd in sh ash init mount umount ls cat echo mkdir mknod sleep ps kill; do
    ln -sf busybox $cmd
done
cd $ROOTFS/sbin
ln -sf ../bin/busybox init

# --- Step 4: Install glibc runtime ---
echo "--- Installing glibc runtime ---"
SYSROOT=/usr/mipsel-linux-gnu/lib
for lib in ld.so.1 libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0; do
    src=$(readlink -f $SYSROOT/$lib)
    cp "$src" $ROOTFS/lib/
    # Create soname symlinks
    soname=$(mipsel-linux-gnu-readelf -d "$src" 2>/dev/null | grep SONAME | sed 's/.*\[\(.*\)\]/\1/')
    [ -n "$soname" ] && [ "$soname" != "$(basename $src)" ] && ln -sf "$(basename $src)" "$ROOTFS/lib/$soname"
done
# Also need ld.so.1 symlink
[ ! -f $ROOTFS/lib/ld.so.1 ] && ln -sf ld-linux-mipsel.so.1 $ROOTFS/lib/ld.so.1

# libgcc_s and libstdc++ from cross-compiler
for lib in libgcc_s.so.1 libstdc++.so.6; do
    src=$(find /usr/mipsel-linux-gnu/lib -name "$lib*" -not -type l | head -1)
    [ -f "$src" ] && cp "$src" $ROOTFS/lib/
    # Symlink
    [ "$lib" = "libstdc++.so.6" ] && ln -sf "$(basename $src)" "$ROOTFS/lib/libstdc++.so.6"
done

# Strip all libs
mipsel-linux-gnu-strip --strip-unneeded $ROOTFS/lib/*.so* 2>/dev/null || true

# --- Step 5: Install Qt/Embedded ---
echo "--- Installing Qt/Embedded ---"
mipsel-linux-gnu-strip -o $ROOTFS/opt/QtPalmtop/lib/libqte.so.2.3.10 $QTDIR/lib/libqte.so.2.3.10
cd $ROOTFS/opt/QtPalmtop/lib
ln -sf libqte.so.2.3.10 libqte.so.2
ln -sf libqte.so.2.3.10 libqte.so

# --- Step 6: Install hello world demo ---
echo "--- Installing demo app ---"
cp $BUILDDIR/hello $ROOTFS/opt/QtPalmtop/bin/hello
mipsel-linux-gnu-strip $ROOTFS/opt/QtPalmtop/bin/hello

# --- Step 7: Create init scripts ---
echo "--- Creating init scripts ---"
cat > $ROOTFS/etc/inittab << 'INITTAB'
::sysinit:/etc/init.d/rcS
::respawn:-/bin/sh
INITTAB

cat > $ROOTFS/etc/init.d/rcS << 'RCS'
#!/bin/sh
mount -t proc none /proc
mount -t sysfs none /sys 2>/dev/null

echo "BE-300 Qt/Embedded Demo"
echo "Starting Qt/Embedded on /dev/fb0..."

export QTDIR=/opt/QtPalmtop
export HOME=/root
export LD_LIBRARY_PATH=/opt/QtPalmtop/lib:/lib
export QWS_MOUSE_PROTO=none
export QWS_KEYBOARD=none
export QWS_DISPLAY=LinuxFb:/dev/fb0
export QWS_SIZE=240x320

/opt/QtPalmtop/bin/hello -qws &
RCS
chmod +x $ROOTFS/etc/init.d/rcS

# --- Step 8: Create device nodes ---
echo "--- Noting device nodes (will create in ext2) ---"
# Device nodes can't be created in the host filesystem without root
# We'll create them using debugfs after creating the ext2 image

# --- Step 9: Size check ---
echo "--- Rootfs size ---"
du -sh $ROOTFS
du -sh $ROOTFS/lib
du -sh $ROOTFS/opt

# --- Step 10: Create ext2 image ---
echo "--- Creating ext2 image ---"
dd if=/dev/zero of=$RAMDISK bs=1M count=16
mkfs.ext2 -F -b 1024 -I 128 -m 0 $RAMDISK

# Mount and populate (needs root, but in Docker we are root)
MNTDIR=$(mktemp -d)
mount -o loop $RAMDISK $MNTDIR
cp -a $ROOTFS/* $MNTDIR/

# Create device nodes
mknod $MNTDIR/dev/console c 5 1
mknod $MNTDIR/dev/null c 1 3
mknod $MNTDIR/dev/zero c 1 5
mknod $MNTDIR/dev/tty c 5 0
mknod $MNTDIR/dev/tty0 c 4 0
mknod $MNTDIR/dev/tty1 c 4 1
mknod $MNTDIR/dev/fb0 c 29 0
mknod $MNTDIR/dev/mem c 1 1
mknod $MNTDIR/dev/ttyS0 c 4 64
ln -sf fb0 $MNTDIR/dev/fb

echo "--- Image contents ---"
ls -la $MNTDIR/
ls -la $MNTDIR/dev/
du -sh $MNTDIR/

umount $MNTDIR
rmdir $MNTDIR

# Compress
gzip -9 -f $RAMDISK
ls -lh ${RAMDISK}.gz

echo "=== Ramdisk created: ${RAMDISK}.gz ==="
