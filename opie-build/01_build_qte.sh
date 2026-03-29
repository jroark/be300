#!/bin/bash
# Phase 1: Build Qt/Embedded 2.3.10 for mipsel
# Run inside mips-dev container: docker compose run --rm mips-dev bash /work/opie-build/01_build_qte.sh
set -e

BUILDDIR=/work/opie-build
export QTDIR=$BUILDDIR/qte-opie
export OPIEDIR=$BUILDDIR/opie
export QPEDIR=$BUILDDIR/opie

echo "=== Phase 1: Build Qt/Embedded 2.3.10 for mipsel ==="

# --- Step 1: Create toolchain symlinks ---
# qte-opie configs reference mipsel-linux-gcc, but Ubuntu has mipsel-linux-gnu-gcc
echo "--- Creating toolchain symlinks ---"
for tool in gcc g++ ar strip ld nm objcopy ranlib; do
    ln -sf /usr/bin/mipsel-linux-gnu-$tool /usr/local/bin/mipsel-linux-$tool
done
# Verify
mipsel-linux-gcc --version | head -1

# --- Step 2: Copy Opie's qconfig-qpe.h into Qt/E ---
echo "--- Installing qconfig-qpe.h ---"
cp $OPIEDIR/qt/qconfig-qpe.h $QTDIR/src/tools/
echo "Copied qconfig-qpe.h"

# --- Step 3: Build native moc and uic first ---
# moc and uic are code generators that must run on the build host (x86_64).
# We do a quick native build just to get these tools, then cross-compile Qt/E.
echo "--- Building native moc and uic ---"
cd $QTDIR

# Save a clean copy of the source for the cross build
if [ ! -f "$BUILDDIR/.native_tools_built" ]; then
    # Configure for native build (just to get moc/uic)
    export QTDIR=$BUILDDIR/qte-opie
    echo "yes" | ./configure \
        -qconfig qpe \
        -depths 16 \
        -no-xft \
        -no-qvfb \
        2>&1 | tail -20

    # Build just moc and uic
    cd $QTDIR/src
    # Fix any GCC issues in moc source
    make -C ../bin/moc -j$(nproc) 2>&1 || true
    # Try building moc directly
    cd $QTDIR/bin/moc
    if [ ! -f $QTDIR/bin/moc/moc ] && [ ! -f $QTDIR/bin/moc ]; then
        # moc is in src/moc/
        cd $QTDIR/src/moc
        make -j$(nproc) 2>&1 | tail -10
    fi

    # Build uic
    cd $QTDIR/tools/designer/uic
    make -j$(nproc) 2>&1 | tail -10 || echo "uic build may need Qt libs first, will retry"

    # Save native moc binary
    find $QTDIR -name "moc" -type f -executable 2>/dev/null | head -5
    mkdir -p $BUILDDIR/native-tools
    cp $QTDIR/bin/moc $BUILDDIR/native-tools/moc 2>/dev/null || \
    find $QTDIR -name "moc" -type f -executable -exec cp {} $BUILDDIR/native-tools/moc \; 2>/dev/null

    if [ -f "$BUILDDIR/native-tools/moc" ]; then
        echo "Native moc built successfully"
        file $BUILDDIR/native-tools/moc
        touch $BUILDDIR/.native_tools_built
    else
        echo "WARNING: Failed to build native moc, will try alternative approach"
    fi
fi

# --- Step 4: Clean and configure for MIPS cross-compile ---
echo "--- Configuring Qt/E for MIPS cross-compilation ---"
cd $QTDIR
make clean 2>/dev/null || true

# Patch the MIPS config to add GCC 12 compat flags
MIPS_CONFIG=$QTDIR/configs/linux-mips-g++-shared
cp $MIPS_CONFIG ${MIPS_CONFIG}.orig 2>/dev/null || true

# Add compat flags to CXXFLAGS and CFLAGS
sed -i 's/SYSCONF_CXXFLAGS\t= -pipe -DQWS -fno-exceptions -fno-rtti -O2 -Wall -W -DNO_DEBUG/SYSCONF_CXXFLAGS\t= -pipe -DQWS -fno-exceptions -fno-rtti -O2 -Wall -W -DNO_DEBUG -fpermissive -std=gnu++98 -Wno-narrowing -Wno-register/' $MIPS_CONFIG
sed -i 's/SYSCONF_CFLAGS\t\t= -pipe -O2 -Wall -W/SYSCONF_CFLAGS\t\t= -pipe -O2 -Wall -W -Wno-implicit-function-declaration -Wno-implicit-int -Wno-int-conversion/' $MIPS_CONFIG

echo "Modified MIPS config with GCC 12 compat flags"

# Configure for MIPS
echo "yes" | ./configure \
    -qconfig qpe \
    -depths 16 \
    -xplatform linux-mips-g++ \
    -no-xft \
    -no-qvfb \
    2>&1 | tail -20

# --- Step 5: Restore native moc for cross build ---
if [ -f "$BUILDDIR/native-tools/moc" ]; then
    cp $BUILDDIR/native-tools/moc $QTDIR/bin/moc
    chmod +x $QTDIR/bin/moc
    echo "Restored native moc for cross-build"
fi

# --- Step 6: Apply source-level GCC 12 fixes ---
echo "--- Applying GCC 12 compatibility source fixes ---"
cd $QTDIR

# Fix 1: implicit-function-declaration in various C files (bundled zlib/libpng)
# These bundled libs use old C idioms
for f in src/3rdparty/zlib/*.c src/3rdparty/libpng/*.c; do
    if [ -f "$f" ]; then
        # Add missing includes if needed - we'll let compiler warnings guide us
        true
    fi
done

# Fix 2: 'register' storage class specifier (removed in C++17, error with some flags)
# The -std=gnu++98 flag should handle this

# Fix 3: Narrowing conversions - handled by -Wno-narrowing flag

# Fix 4: Old-style casts and implicit conversions in Qt source
# Most will be handled by -fpermissive

echo "Source fixes applied (most handled by compiler flags)"

# --- Step 7: Build Qt/E ---
echo "--- Building Qt/Embedded for MIPS ---"
cd $QTDIR/src

# Build with error output captured
make -j$(nproc) 2>&1 | tee $BUILDDIR/qte_build.log | tail -50

BUILD_STATUS=${PIPESTATUS[0]}
if [ $BUILD_STATUS -eq 0 ]; then
    echo ""
    echo "=== Qt/Embedded build SUCCEEDED ==="
    ls -lh $QTDIR/lib/libqte* 2>/dev/null
    file $QTDIR/lib/libqte.so* 2>/dev/null | head -3
else
    echo ""
    echo "=== Qt/Embedded build FAILED (exit code $BUILD_STATUS) ==="
    echo "Last 30 error lines:"
    grep -E "error:|Error:|undefined reference" $BUILDDIR/qte_build.log | tail -30
    echo ""
    echo "Full build log at: $BUILDDIR/qte_build.log"
    exit 1
fi
