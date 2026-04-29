#!/usr/bin/env bash
# Build a FAT16-formatted raw CF image of arbitrary size, optionally
# pre-populated from a staging directory whose contents become the image
# root. Uses mtools (mformat + mcopy) — no mounting, no root, portable
# across macOS and Linux.
#
# Usage:
#   tools/make_cf_image.sh OUTPUT_IMG SIZE_MB [STAGING_DIR] [LABEL]
#
# Example:
#   stage=$(mktemp -d)
#   mkdir -p "$stage/Program Files"
#   cp linux4be/loader.exe linux4be/vmlinux-pgui-demo linux4be/cyacecfg.txt \
#       "$stage/Program Files/"
#   tools/make_cf_image.sh linux4be/cf.img 16 "$stage" CF
#   rm -rf "$stage"

set -euo pipefail

usage() {
    cat <<EOF
Usage: $0 OUTPUT_IMG SIZE_MB [STAGING_DIR] [LABEL]

  OUTPUT_IMG    path to the image file to (re)create
  SIZE_MB       integer MB; >=4 (FAT16 minimum), <=2048 (FAT16 maximum)
  STAGING_DIR   optional; its CONTENTS are copied to the image root
  LABEL         optional FAT volume label (<=11 chars, default "CF")
EOF
    exit 1
}

[[ $# -lt 2 || $# -gt 4 ]] && usage

OUT=$1
SIZE_MB=$2
STAGE=${3:-}
LABEL=${4:-CF}

if [[ ! "$SIZE_MB" =~ ^[0-9]+$ ]] || (( SIZE_MB < 4 )) || (( SIZE_MB > 2048 )); then
    echo "error: SIZE_MB must be an integer in [4, 2048]" >&2
    exit 1
fi
if [[ -n "$STAGE" && ! -d "$STAGE" ]]; then
    echo "error: staging dir not found: $STAGE" >&2
    exit 1
fi
if (( ${#LABEL} > 11 )); then
    echo "error: LABEL must be <=11 chars" >&2
    exit 1
fi

for tool in mformat mcopy; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "error: $tool not found in PATH (install mtools)" >&2
        exit 1
    }
done

# mtools refuses to operate on a plain file unless told to skip the
# device-type check.
export MTOOLS_SKIP_CHECK=1

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=1m count="$SIZE_MB" status=none
echo "[+] created $OUT ($((SIZE_MB * 1024 * 1024)) bytes)"

# Geometry (16 heads, 32 sectors/track, 4 sectors/cluster, 512 root
# entries) matches what newfs_msdos -F 16 produced for the original
# 16 MB CF image. -T forces total-sector count from SIZE_MB so mformat
# picks FAT16 for the supported size range.
mformat -i "$OUT" \
    -v "$LABEL" \
    -h 16 -s 32 \
    -c 4 -r 512 \
    -T $((SIZE_MB * 2048)) ::
echo "[+] formatted FAT16 (label=$LABEL)"

if [[ -n "$STAGE" ]]; then
    shopt -s dotglob nullglob
    entries=( "$STAGE"/* )
    if (( ${#entries[@]} > 0 )); then
        mcopy -i "$OUT" -s -Q -- "${entries[@]}" ::
    fi
    echo "[+] copied contents of $STAGE/ to image root"
fi

echo "[+] image ready: $OUT"
