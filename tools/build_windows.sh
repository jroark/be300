#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-windows}"
DIST_DIR="${DIST_DIR:-$ROOT_DIR/dist}"
PKG_DIR="$DIST_DIR/be300-windows-amd64"
SDL2_VERSION="${SDL2_VERSION:-2.30.12}"
SDL2_ARCHIVE="SDL2-devel-${SDL2_VERSION}-mingw.zip"
SDL2_URL="${SDL2_URL:-https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/${SDL2_ARCHIVE}}"
DEPS_DIR="${DEPS_DIR:-$BUILD_DIR/deps}"
SDL2_ROOT="${SDL2_MINGW_ROOT:-$DEPS_DIR/SDL2-${SDL2_VERSION}/x86_64-w64-mingw32}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"

if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
  if [[ -x /opt/homebrew/bin/x86_64-w64-mingw32-gcc ]]; then
    export PATH="/opt/homebrew/bin:$PATH"
  elif [[ -x /usr/local/bin/x86_64-w64-mingw32-gcc ]]; then
    export PATH="/usr/local/bin:$PATH"
  fi
fi

if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
  cat >&2 <<'EOF'
error: x86_64-w64-mingw32-gcc not found.

Install MinGW-w64 first. On macOS with Homebrew:
  brew install mingw-w64
EOF
  exit 1
fi

if [[ ! -f "$SDL2_ROOT/bin/SDL2.dll" ]]; then
  mkdir -p "$DEPS_DIR"
  if [[ ! -f "$DEPS_DIR/$SDL2_ARCHIVE" ]]; then
    echo "Downloading $SDL2_ARCHIVE"
    curl -L --fail "$SDL2_URL" -o "$DEPS_DIR/$SDL2_ARCHIVE"
  fi
  rm -rf "$DEPS_DIR/SDL2-${SDL2_VERSION}"
  unzip -q "$DEPS_DIR/$SDL2_ARCHIVE" -d "$DEPS_DIR"
fi

if [[ ! -f "$SDL2_ROOT/bin/SDL2.dll" ]]; then
  echo "error: SDL2.dll not found under $SDL2_ROOT" >&2
  echo "Set SDL2_MINGW_ROOT to an SDL2 x86_64-w64-mingw32 prefix." >&2
  exit 1
fi

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/tools/cmake/windows-mingw-x86_64.cmake" \
  -DCMAKE_PREFIX_PATH="$SDL2_ROOT" \
  -DSDL2_DIR="$SDL2_ROOT/lib/cmake/SDL2" \
  -DBE300_SDL2_MINGW_ROOT="$SDL2_ROOT" \
  -DBUILD_CLI=ON \
  -DBUILD_WEB=OFF \
  -DBUILD_ANDROID=OFF

cmake --build "$BUILD_DIR" --target be300 -j"$JOBS"

rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR"
cp "$BUILD_DIR/be300.exe" "$PKG_DIR/"
cp "$SDL2_ROOT/bin/SDL2.dll" "$PKG_DIR/"

cat > "$PKG_DIR/run-be300.bat" <<'EOF'
@echo off
setlocal
if "%~1"=="" (
  echo Usage: run-be300.bat path\to\All_nand_300.bin
  exit /b 2
)
"%~dp0be300.exe" --nand "%~1"
EOF

cat > "$PKG_DIR/README_WINDOWS.txt" <<'EOF'
BE-300 Emulator for Windows x86_64

This package contains the native Windows build of the BE-300 emulator.
Restore/NAND images are not bundled. Supply your local image when running:

  be300.exe --nand C:\path\to\All_nand_300.bin

The helper batch file accepts the NAND path as its first argument:

  run-be300.bat C:\path\to\All_nand_300.bin

Serial output is written to stdout and diagnostics to stderr.
EOF

(cd "$DIST_DIR" && zip -q -9 -r "be300-windows-amd64.zip" "be300-windows-amd64")

echo "Built $DIST_DIR/be300-windows-amd64.zip"

# Optional NSIS installer. Run `brew install makensis` (or apt install nsis)
# to enable. The .nsi pulls files out of build-windows/dist/be300-windows-amd64/
# (relative paths), so this step is happy whether makensis runs from the repo
# root or from the .nsi's directory.
if command -v makensis >/dev/null 2>&1; then
    NSI_VERSION="${BE300_VERSION:-0.1.1}"
    pushd "$ROOT_DIR/packaging/windows" >/dev/null
    makensis -DPRODUCT_VERSION="$NSI_VERSION" be300.nsi
    popd >/dev/null
    if [[ -f "$DIST_DIR/be300-setup.exe" ]]; then
        echo "Built $DIST_DIR/be300-setup.exe"
    fi
else
    echo "(skipping installer build: makensis not on PATH)"
fi
