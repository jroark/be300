#!/usr/bin/env bash
# Regenerate packaging/macos/be300.icns from packaging/macos/icon.iconset/.
# Run after editing the iconset sources.
set -euo pipefail

cd "$(dirname "$0")/.."
ICONSET=packaging/macos/icon.iconset
OUT=packaging/macos/be300.icns

if [ ! -d "$ICONSET" ]; then
    echo "No iconset at $ICONSET" >&2
    exit 1
fi
iconutil -c icns "$ICONSET" -o "$OUT"
echo "Wrote $OUT"
