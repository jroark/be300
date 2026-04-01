#!/bin/bash
set -e

BUSYBOX_VERSION="1.24.2"
TCL_BB_SOURCE="http://distro.ibiblio.org/tinycorelinux/7.x/x86/release/src/busybox/busybox-$BUSYBOX_VERSION.tar.bz2"
TCL_BB_CONFIG="http://distro.ibiblio.org/tinycorelinux/7.x/x86/release/src/busybox/busybox-1.24.1_config_suid"

echo "--- Downloading BusyBox $BUSYBOX_VERSION ---"
if [ ! -f busybox-$BUSYBOX_VERSION.tar.bz2 ]; then
    wget $TCL_BB_SOURCE
fi

echo "--- Extracting BusyBox ---"
if [ ! -d busybox-$BUSYBOX_VERSION ]; then
    tar xjf busybox-$BUSYBOX_VERSION.tar.bz2
fi

cd busybox-$BUSYBOX_VERSION

echo "--- Fetching Tiny Core BusyBox Config ---"
wget -O .config $TCL_BB_CONFIG

echo "--- Preparing Config for MIPS ---"
# Force static build
sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
sed -i 's/CONFIG_STATIC=n/CONFIG_STATIC=y/' .config

# FIX 1: Disable RPC (missing in modern glibc)
sed -i 's/CONFIG_FEATURE_MOUNT_NFS=y/CONFIG_FEATURE_MOUNT_NFS=n/' .config
sed -i 's/CONFIG_FEATURE_HAVE_RPC=y/CONFIG_FEATURE_HAVE_RPC=n/' .config
sed -i 's/CONFIG_FEATURE_INETD_RPC=y/CONFIG_FEATURE_INETD_RPC=n/' .config

# FIX 2: Use BusyBox internal crypt implementation to avoid libcrypt dependency
sed -i 's/CONFIG_USE_BB_CRYPT=n/CONFIG_USE_BB_CRYPT=y/' .config
sed -i 's/^# CONFIG_USE_BB_CRYPT is not set/CONFIG_USE_BB_CRYPT=y/' .config
sed -i 's/CONFIG_USE_BB_CRYPT_SHA=n/CONFIG_USE_BB_CRYPT_SHA=y/' .config
sed -i 's/^# CONFIG_USE_BB_CRYPT_SHA is not set/CONFIG_USE_BB_CRYPT_SHA=y/' .config

# Use 'yes "" | make oldconfig'
yes "" | make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- oldconfig

echo "--- Starting BusyBox Build ---"
make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- -j$(nproc)

echo "--- Installing BusyBox to ./_install ---"
make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- install
