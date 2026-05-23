#!/usr/bin/env bash
# Build the BE300.app bundle into dist/. Re-runs CMake configure if needed.
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p build-host
cmake -S . -B build-host >/dev/null
cmake --build build-host -j --target be300
cmake --build build-host -j --target package_macos

echo
echo "Bundle written to:"
echo "  $(pwd)/dist/BE300.app"
echo
if ! command -v dylibbundler >/dev/null 2>&1; then
    echo "Tip: brew install dylibbundler"
    echo "     for a self-contained .app that runs on a clean Mac without Homebrew."
fi
