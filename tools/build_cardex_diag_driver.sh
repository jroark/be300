#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="${1:-build-host/cardex_diag}"
module_name="${2:-compdisk.dll}"

cd "$repo_root"
mkdir -p "$out_dir"

docker compose run --rm mips-dev /bin/bash -lc "\
  cd /work && \
  mipsel-linux-gnu-as -32 -EL -o ${out_dir}/cardex_diag_patch.o tools/cardex_diag.S && \
  mipsel-linux-gnu-objcopy -j .text -O binary ${out_dir}/cardex_diag_patch.o ${out_dir}/cardex_diag_patch.bin"

python3 tools/build_cardex_diag_image.py \
  --nk-image docs/nk_decompressed.bin \
  --patch-bin "${out_dir}/cardex_diag_patch.bin" \
  --module "$module_name" \
  --out-dir "$out_dir"
