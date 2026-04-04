#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TIMEOUT_SECS="${1:-60}"
RUN_TAG="${2:-$(date +%Y%m%d_%H%M%S)_${TIMEOUT_SECS}s}"

OUT_DIR="${ROOT_DIR}/build-docker/wince-divergence/${RUN_TAG}"
CONTAINER_OUT_DIR="/work/build-docker/wince-divergence/${RUN_TAG}"
mkdir -p "${OUT_DIR}"

docker compose run --rm mips-dev /bin/bash -lc "
cd /work && mkdir -p build-docker && cd build-docker &&
cmake .. && make -j\$(nproc) &&
set +e
timeout ${TIMEOUT_SECS}s ./be300 --nand ../ce/restore_images/All_nand_300.bin --log-mmio \
  > ${CONTAINER_OUT_DIR}/stdout.log 2> ${CONTAINER_OUT_DIR}/stderr.log
rc=\$?
set -e
printf '%s\n' \"\$rc\" > ${CONTAINER_OUT_DIR}/exit_code.txt
"

python3 "${ROOT_DIR}/tools/wince_divergence_report.py" \
  --stdout "${OUT_DIR}/stdout.log" \
  --stderr "${OUT_DIR}/stderr.log" \
  --exit-code-file "${OUT_DIR}/exit_code.txt" \
  > "${OUT_DIR}/report.txt"

cat "${OUT_DIR}/report.txt"
echo "report_path: ${OUT_DIR}/report.txt"
