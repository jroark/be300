#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/build-web/web/downloads}"
TMP_DIR="${TMP_DIR:-$ROOT_DIR/build-web/net-bridge-packages}"

if ! command -v go >/dev/null 2>&1; then
  echo "error: go not found in PATH" >&2
  exit 1
fi

rm -rf "$TMP_DIR"
mkdir -p "$OUT_DIR" "$TMP_DIR"

build_one() {
  local goos="$1"
  local goarch="$2"
  local archive="$3"
  local exe="be300-net-bridge"
  local ext=""

  if [[ "$goos" == "windows" ]]; then
    ext=".exe"
  fi

  local work="$TMP_DIR/${goos}-${goarch}"
  mkdir -p "$work"

  (cd "$ROOT_DIR" && GO111MODULE=off GOOS="$goos" GOARCH="$goarch" CGO_ENABLED=0 \
    go build -trimpath -ldflags="-s -w" \
    -o "$work/${exe}${ext}" ./tools/net_bridge)

  if [[ "$archive" == *.zip ]]; then
    (cd "$work" && zip -q -9 "$OUT_DIR/$archive" "${exe}${ext}")
  else
    tar -C "$work" -czf "$OUT_DIR/$archive" "${exe}${ext}"
  fi
}

build_one darwin amd64 be300-net-bridge-macos-amd64.tar.gz
build_one darwin arm64 be300-net-bridge-macos-arm64.tar.gz
build_one linux amd64 be300-net-bridge-linux-amd64.tar.gz
build_one windows amd64 be300-net-bridge-windows-amd64.zip

echo "Built network bridge downloads in $OUT_DIR"
