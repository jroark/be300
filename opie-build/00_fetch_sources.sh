#!/bin/bash
# Phase 0: Fetch sources for Opie build
# Run inside mips-dev container: docker compose run --rm mips-dev bash /work/opie-build/00_fetch_sources.sh
set -e

BUILDDIR=/work/opie-build

echo "=== Phase 0: Source Acquisition ==="

# Clone qte-opie (Qt/Embedded 2.3.x fork for Opie)
if [ ! -d "$BUILDDIR/qte-opie" ]; then
    echo "--- Cloning qte-opie ---"
    git clone --depth 1 https://github.com/opieproject/qte-opie "$BUILDDIR/qte-opie"
else
    echo "--- qte-opie already cloned ---"
fi

# Clone Opie
if [ ! -d "$BUILDDIR/opie" ]; then
    echo "--- Cloning opie ---"
    git clone --depth 1 https://github.com/opieproject/opie "$BUILDDIR/opie"
else
    echo "--- opie already cloned ---"
fi

# Create staging and rootfs directories
mkdir -p "$BUILDDIR/staging"/{lib,include,bin}
mkdir -p "$BUILDDIR/rootfs"

echo ""
echo "=== Source tree summary ==="
echo "qte-opie: $(ls $BUILDDIR/qte-opie/configure 2>/dev/null && echo 'OK' || echo 'MISSING')"
echo "opie:     $(ls $BUILDDIR/opie/configure 2>/dev/null && echo 'OK (has configure)' || echo 'no configure (uses qmake)')"
echo ""
ls -la "$BUILDDIR/qte-opie/configs/" | grep mips || echo "No MIPS configs found in qte-opie"
echo ""
echo "=== Opie qt/ patch directory ==="
ls "$BUILDDIR/opie/qt/" 2>/dev/null || echo "No opie/qt/ directory"
echo ""
echo "=== Phase 0 complete ==="
