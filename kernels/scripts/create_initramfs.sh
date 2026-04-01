#!/bin/bash
set -e

ROOTFS="./rootfs"
BB_INSTALL="../busybox-1.24.2/_install"

echo "--- Creating Directory Structure ---"
rm -rf $ROOTFS
mkdir -p $ROOTFS
cd $ROOTFS
mkdir -p bin sbin etc proc sys dev usr/bin usr/sbin root tmp var lib

echo "--- Copying BusyBox Files ---"
cp -a $BB_INSTALL/* .

echo "--- Creating Minimal /etc/fstab ---"
cat > etc/fstab <<EOF
proc            /proc        proc    defaults          0       0
sysfs           /sys         sysfs   defaults          0       0
devtmpfs        /dev         devtmpfs  defaults        0       0
EOF

echo "--- Creating basic /init script ---"
cat > init <<EOF
#!/bin/sh

# Mount essential filesystems
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

echo "--- Welcome to Tiny Core MIPS (Custom Build) ---"
echo "Booting from initramfs..."

# Start a shell on the console
exec /bin/sh
EOF

chmod +x init

echo "--- Packaging initramfs (rootfs.gz) ---"
find . | cpio -H newc -o | gzip > ../rootfs.gz

echo "--- SUCCESS: rootfs.gz created ---"
ls -l ../rootfs.gz
