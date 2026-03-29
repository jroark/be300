#!/bin/bash
# Phase 4: Create Qt/E ramdisk with musl runtime for BE-300 emulator
# Run inside mips-dev container:
#   docker compose run --rm mips-dev bash /work/opie-build/create_ramdisk_musl.sh
set -e

BUILDDIR=/work/opie-build
BRDIR=$BUILDDIR/buildroot/output/host
SYSROOT=$BRDIR/mipsel-buildroot-linux-musl/sysroot
ROOTFS=$BUILDDIR/rootfs
QTDIR=$BUILDDIR/qte-opie
RAMDISK=$BUILDDIR/ramdisk-qte.ext2

echo "=== Phase 4: Create Qt/E Ramdisk (musl) ==="

# Verify prerequisites
for f in "$BUILDDIR/hello_musl" "$QTDIR/lib/libqte.so.2.3.10" "$SYSROOT/lib/libc.so"; do
    if [ ! -f "$f" ]; then
        echo "ERROR: Missing $f"
        exit 1
    fi
done

# --- Step 1: Use pre-built BusyBox from pgui (MIPS-II, uClibc, dynamically linked) ---
echo "--- Using pgui BusyBox (MIPS-II) ---"
file "$BUILDDIR/busybox-pgui"

# --- Step 2: Create rootfs directory structure ---
echo "--- Creating rootfs ---"
rm -rf $ROOTFS
mkdir -p $ROOTFS/{bin,sbin,lib,dev,etc/init.d,proc,sys,tmp,var,root,opt/QtPalmtop/{bin,lib}}

# --- Step 3: Install BusyBox ---
echo "--- Installing BusyBox ---"
cp "$BUILDDIR/busybox-pgui" $ROOTFS/bin/busybox
chmod +x $ROOTFS/bin/busybox
cd $ROOTFS/bin
for cmd in sh ash init mount umount ls cat echo mkdir mknod sleep ps kill; do
    ln -sf busybox $cmd
done
cd $ROOTFS/sbin
ln -sf ../bin/busybox init

# --- Step 3b: Install uClibc runtime for BusyBox ---
echo "--- Installing uClibc runtime (for BusyBox) ---"
# Copy only the real library files (non-zero size), skip 0-byte symlink artifacts
for lib in $BUILDDIR/uclibc_*.so*; do
    [ ! -s "$lib" ] && continue  # skip 0-byte files
    BASENAME=$(basename "$lib" | sed 's/^uclibc_//')
    cp "$lib" "$ROOTFS/lib/$BASENAME"
done
# Create symlinks for the dynamic linker and libc
ln -sf ld-uClibc-0.9.15.so "$ROOTFS/lib/ld-uClibc.so.0"
ln -sf libuClibc-0.9.15.so "$ROOTFS/lib/libc.so.0"
# Dynamic linkers must be executable
chmod +x "$ROOTFS/lib/ld-uClibc-0.9.15.so" "$ROOTFS/lib/libuClibc-0.9.15.so" 2>/dev/null || true
echo "uClibc libs:"
ls -lh $ROOTFS/lib/*uClibc* $ROOTFS/lib/ld-uClibc* $ROOTFS/lib/libc.so.0 2>/dev/null || true

# --- Step 4: Install musl runtime (for Qt/E + hello) ---
echo "--- Installing musl runtime ---"

# musl dynamic linker + libc (they're the same file in musl)
# The ELF interpreter is /lib/ld-musl-mipsel-sf.so.1
# NEEDED entries reference libc.so
# Both names must resolve to the same musl libc binary.
cp "$SYSROOT/lib/libc.so" "$ROOTFS/lib/libc.so"
ln -sf libc.so "$ROOTFS/lib/ld-musl-mipsel-sf.so.1"
echo "Dynamic linker: ld-musl-mipsel-sf.so.1 -> libc.so"

# libgcc_s from buildroot
BRGCC=$(find "$BRDIR/mipsel-buildroot-linux-musl/lib" -maxdepth 1 -name "libgcc_s.so.1" | head -1)
if [ -n "$BRGCC" ]; then
    cp "$BRGCC" "$ROOTFS/lib/libgcc_s.so.1"
    echo "Installed libgcc_s from: $BRGCC"
fi

# libstdc++ from buildroot (exclude the -gdb.py file!)
BRSTDCXX=$(find "$BRDIR/mipsel-buildroot-linux-musl/lib" -maxdepth 1 -name "libstdc++.so.6.0.*" -not -name "*gdb*" | head -1)
if [ -n "$BRSTDCXX" ]; then
    cp "$BRSTDCXX" "$ROOTFS/lib/"
    BASENAME=$(basename "$BRSTDCXX")
    ln -sf "$BASENAME" "$ROOTFS/lib/libstdc++.so.6"
    ln -sf "$BASENAME" "$ROOTFS/lib/libstdc++.so"
    echo "Installed libstdc++ from: $BRSTDCXX"
fi

# Strip all libs (use buildroot strip for musl libs)
"$BRDIR/bin/mipsel-linux-strip" --strip-unneeded $ROOTFS/lib/ld-musl* $ROOTFS/lib/libgcc_s* $ROOTFS/lib/libstdc++* 2>/dev/null || true

echo "--- Runtime libraries ---"
ls -lh $ROOTFS/lib/

# --- Step 5: Install Qt/Embedded ---
echo "--- Installing Qt/Embedded ---"
"$BRDIR/bin/mipsel-linux-strip" -o $ROOTFS/opt/QtPalmtop/lib/libqte.so.2.3.10 $QTDIR/lib/libqte.so.2.3.10
cd $ROOTFS/opt/QtPalmtop/lib
ln -sf libqte.so.2.3.10 libqte.so.2
ln -sf libqte.so.2.3.10 libqte.so

# --- Step 5b: Install Qt/E fonts ---
echo "--- Installing Qt/E fonts ---"
mkdir -p $ROOTFS/opt/QtPalmtop/lib/fonts
# Install pre-rendered QPF fonts (helvetica + fixed, normal weight, key sizes)
for f in helvetica_100_50.qpf helvetica_120_50.qpf helvetica_140_50.qpf \
         helvetica_180_50.qpf helvetica_240_50.qpf helvetica_80_50.qpf \
         fixed_120_50.qpf fixed_70_50.qpf; do
    cp "$QTDIR/lib/fonts/$f" "$ROOTFS/opt/QtPalmtop/lib/fonts/" 2>/dev/null || true
done
# Install fontdir (Qt/E needs this to discover fonts)
cp "$QTDIR/lib/fonts/fontdir" "$ROOTFS/opt/QtPalmtop/lib/fonts/"
echo "Fonts installed:"
ls -lh $ROOTFS/opt/QtPalmtop/lib/fonts/

# --- Step 6: Install hello demo ---
echo "--- Installing hello demo ---"
cp "$BUILDDIR/hello_musl" $ROOTFS/opt/QtPalmtop/bin/hello
"$BRDIR/bin/mipsel-linux-strip" $ROOTFS/opt/QtPalmtop/bin/hello

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

export QTDIR=/opt/QtPalmtop
export HOME=/root
export LD_LIBRARY_PATH=/opt/QtPalmtop/lib:/lib
export QWS_MOUSE_PROTO=none
export QWS_KEYBOARD=none
export QWS_DISPLAY=LinuxFb:/dev/fb0
export QWS_SIZE=240x320

# Debug: check if libraries load
echo "Checking libs..."
ls -l /opt/QtPalmtop/lib/fonts/
ls -l /lib/ld-musl* /lib/libc.so /lib/libstdc++* /lib/libgcc_s*

echo "Starting hello -qws..."
/opt/QtPalmtop/bin/hello -qws 2>&1 &
HELLO_PID=$!
echo "hello PID=$HELLO_PID"

# Give it a moment then check if still alive
sleep 2
if kill -0 $HELLO_PID 2>/dev/null; then
    echo "hello is running"
else
    echo "hello CRASHED (exit=$?)"
fi
RCS
chmod +x $ROOTFS/etc/init.d/rcS

# --- Step 8: Size check ---
echo "--- Rootfs size ---"
du -sh $ROOTFS
du -sh $ROOTFS/lib
du -sh $ROOTFS/opt

# --- Step 9: Create ext2 image using genext2fs (no loop mount needed) ---
echo "--- Creating 16MB ext2 image ---"

# Install genext2fs if not present
which genext2fs >/dev/null 2>&1 || apt-get update -qq && apt-get install -y -qq genext2fs >/dev/null 2>&1

# Create device table for genext2fs
cat > /tmp/devtable.txt << 'DEVTABLE'
/dev		d	755	0	0	-	-	-	-	-
/dev/console	c	600	0	0	5	1	-	-	-
/dev/null	c	666	0	0	1	3	-	-	-
/dev/zero	c	666	0	0	1	5	-	-	-
/dev/tty	c	666	0	0	5	0	-	-	-
/dev/tty0	c	666	0	0	4	0	-	-	-
/dev/tty1	c	666	0	0	4	1	-	-	-
/dev/fb0	c	666	0	0	29	0	-	-	-
/dev/mem	c	640	0	0	1	1	-	-	-
/dev/ttyS0	c	666	0	0	4	64	-	-	-
DEVTABLE

genext2fs -b 16384 -d $ROOTFS -D /tmp/devtable.txt -U $RAMDISK

echo "--- Image contents (debugfs) ---"
debugfs -R "ls -l /" $RAMDISK 2>/dev/null | head -20
debugfs -R "ls -l /dev" $RAMDISK 2>/dev/null | head -15
debugfs -R "ls -l /lib" $RAMDISK 2>/dev/null | head -20

# Create /dev/fb symlink
debugfs -w -R "symlink /dev/fb fb0" $RAMDISK 2>/dev/null || true

# Compress
gzip -9 -f $RAMDISK
ls -lh ${RAMDISK}.gz

echo "=== Ramdisk created: ${RAMDISK}.gz ==="
