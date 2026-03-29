#!/bin/bash
# Phase 1: Build Buildroot MIPS-II musl toolchain
# Run inside mips-dev container:
#   docker compose run --rm mips-dev bash /work/opie-build/build_musl_toolchain.sh
set -e

BUILDDIR=/work/opie-build

echo "=== Phase 1: Buildroot MIPS-II musl Toolchain ==="

# --- Step 1: Clone Buildroot ---
if [ ! -d "$BUILDDIR/buildroot" ]; then
    echo "--- Cloning Buildroot ---"
    cd "$BUILDDIR"
    git clone --depth 1 https://git.buildroot.net/buildroot
else
    echo "--- Buildroot already cloned ---"
fi

# --- Step 2: Configure ---
echo "--- Configuring Buildroot ---"
cd "$BUILDDIR/buildroot"

# Clean any previous partial build
make clean 2>/dev/null || true

cp "$BUILDDIR/buildroot_defconfig" configs/be300_defconfig
make be300_defconfig

# Post-config fixup: force MIPS-II ISA for the entire toolchain
# BR2_GCC_TARGET_ARCH is derived from BR2_mips_32 and defaults to "mips32".
# GCC's --with-arch sets the default ISA for the cross-compiler.
# We change it to "mips2" so GCC, musl, and libgcc are all built for MIPS-II.
sed -i 's/^BR2_GCC_TARGET_ARCH="mips32"/BR2_GCC_TARGET_ARCH="mips2"/' .config

echo "--- Config summary ---"
grep -E "^BR2_(mips|GCC_TARGET_ARCH|TOOLCHAIN|TARGET_OPTIM|INIT|PACKAGE_BUSYBOX|TARGET_ROOTFS)" .config | head -25

# --- Step 3: Build ---
echo "--- Building toolchain (this will take 30-60 minutes) ---"
make -j$(nproc) toolchain 2>&1 | tail -5

# --- Step 4: Verify ---
echo ""
echo "=== Verification ==="
TCBIN="$BUILDDIR/buildroot/output/host/bin"

if [ -f "$TCBIN/mipsel-linux-gcc" ]; then
    echo "Toolchain found at: $TCBIN"
    $TCBIN/mipsel-linux-gcc --version | head -1
    echo "Default arch:"
    $TCBIN/mipsel-linux-gcc -Q --help=target 2>&1 | grep march || true

    # Test compile - should produce MIPS-II
    cat > /tmp/test_mips2.c << 'EOF'
int main() { return 0; }
EOF
    $TCBIN/mipsel-linux-gcc -static -o /tmp/test_mips2 /tmp/test_mips2.c
    echo "--- Test binary (default flags, should be mips2) ---"
    file /tmp/test_mips2
    mipsel-linux-gnu-readelf -h /tmp/test_mips2 | grep Flags

    # Check musl libc.so for MIPS32 instructions (CLZ is a good marker)
    echo "--- Checking musl libc.so for MIPS32 instructions ---"
    SYSROOT="$BUILDDIR/buildroot/output/host/mipsel-buildroot-linux-musl/sysroot"
    MUSL_LIBC=$(find "$SYSROOT" -name "libc.so" | head -1)
    if [ -n "$MUSL_LIBC" ]; then
        mipsel-linux-gnu-readelf -h "$MUSL_LIBC" | grep Flags
        # Look for clz instruction (MIPS32 only)
        CLZ_COUNT=$(mipsel-linux-gnu-objdump -d "$MUSL_LIBC" | grep -c "clz" || true)
        echo "CLZ instructions in musl libc.so: $CLZ_COUNT (should be 0 for MIPS-II)"
    fi

    echo ""
    echo "Sysroot: $SYSROOT/"
    ls "$SYSROOT/lib/" | head -10
    echo ""
    echo "=== Phase 1 COMPLETE ==="
else
    echo "ERROR: Toolchain not found!"
    ls "$TCBIN/" 2>/dev/null | head -20
    exit 1
fi
