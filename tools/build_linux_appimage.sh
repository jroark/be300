#!/usr/bin/env bash
# Build a Linux AppImage from a freshly-built be300 binary using linuxdeploy.
# Requires `linuxdeploy` on PATH (https://github.com/linuxdeploy/linuxdeploy).
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

if ! command -v linuxdeploy >/dev/null 2>&1; then
    echo "linuxdeploy not found on PATH." >&2
    echo "Get it from https://github.com/linuxdeploy/linuxdeploy/releases" >&2
    exit 1
fi

mkdir -p build-host dist
cmake -S . -B build-host >/dev/null
cmake --build build-host -j --target be300

APPDIR="$ROOT/build-host/AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/icons/hicolor/256x256/apps" \
         "$APPDIR/usr/share/applications" "$APPDIR/usr/share/mime/packages"

cp "$ROOT/build-host/be300"                "$APPDIR/usr/bin/be300"
cp "$ROOT/packaging/linux/be300.desktop"   "$APPDIR/usr/share/applications/be300.desktop"
cp "$ROOT/packaging/linux/be300.xml"       "$APPDIR/usr/share/mime/packages/be300.xml"
cp "$ROOT/packaging/linux/be300.png"       "$APPDIR/usr/share/icons/hicolor/256x256/apps/be300.png"

linuxdeploy \
    --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/be300" \
    --desktop-file "$APPDIR/usr/share/applications/be300.desktop" \
    --icon-file    "$APPDIR/usr/share/icons/hicolor/256x256/apps/be300.png" \
    --output appimage

mv BE-300_VM_Manager-*.AppImage "$ROOT/dist/BE300-x86_64.AppImage" 2>/dev/null || \
    mv ./*.AppImage "$ROOT/dist/BE300-x86_64.AppImage"

chmod +x "$ROOT/dist/BE300-x86_64.AppImage"
echo
echo "AppImage written to $ROOT/dist/BE300-x86_64.AppImage"
