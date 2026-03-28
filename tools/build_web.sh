#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-web}"

if command -v nproc >/dev/null 2>&1; then
  JOBS="${JOBS:-$(nproc)}"
elif command -v sysctl >/dev/null 2>&1; then
  JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
else
  JOBS="${JOBS:-4}"
fi

if ! command -v emcc >/dev/null 2>&1; then
  echo "error: emcc not found in PATH" >&2
  exit 1
fi

if ! command -v emcmake >/dev/null 2>&1; then
  echo "error: emcmake not found in PATH" >&2
  exit 1
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "error: cmake not found in PATH" >&2
  exit 1
fi

python_candidates=()

if [[ -n "${EMSDK_PYTHON:-}" ]]; then
  python_candidates+=("$EMSDK_PYTHON")
fi

for candidate in \
  /opt/homebrew/bin/python3.14 \
  /opt/homebrew/bin/python3.13 \
  /opt/homebrew/bin/python3.12
do
  if [[ -x "$candidate" ]]; then
    python_candidates+=("$candidate")
  fi
done

if command -v python3 >/dev/null 2>&1; then
  python_candidates+=("$(command -v python3)")
fi

EMSDK_PYTHON=""
for candidate in "${python_candidates[@]}"; do
  if "$candidate" -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)' \
    >/dev/null 2>&1; then
    EMSDK_PYTHON="$candidate"
    break
  fi
done

if [[ -z "$EMSDK_PYTHON" ]]; then
  echo "error: could not find a Python 3.10+ interpreter for Emscripten" >&2
  exit 1
fi

if [[ -z "${EM_LLVM_ROOT:-}" || -z "${EM_BINARYEN_ROOT:-}" ]]; then
  if command -v brew >/dev/null 2>&1; then
    EMSCRIPTEN_PREFIX="$(brew --prefix emscripten 2>/dev/null || true)"
    if [[ -n "${EMSCRIPTEN_PREFIX}" ]]; then
      : "${EM_LLVM_ROOT:=$EMSCRIPTEN_PREFIX/libexec/llvm/bin}"
      : "${EM_BINARYEN_ROOT:=$EMSCRIPTEN_PREFIX/libexec/binaryen}"
    fi
  fi
fi

: "${EM_CONFIG:=$BUILD_DIR/.emscripten}"
: "${EM_CACHE:=$BUILD_DIR/.emcache}"

mkdir -p "$BUILD_DIR"

env_args=(
  "EMSDK_PYTHON=$EMSDK_PYTHON"
  "EM_CONFIG=$EM_CONFIG"
  "EM_CACHE=$EM_CACHE"
)

if [[ -n "${EM_LLVM_ROOT:-}" ]]; then
  env_args+=("EM_LLVM_ROOT=$EM_LLVM_ROOT")
fi

if [[ -n "${EM_BINARYEN_ROOT:-}" ]]; then
  env_args+=("EM_BINARYEN_ROOT=$EM_BINARYEN_ROOT")
fi

if [[ ! -f "$EM_CONFIG" ]]; then
  env "${env_args[@]}" emcc --generate-config >/dev/null
fi

env "${env_args[@]}" emcmake cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DBUILD_WEB=ON
env "${env_args[@]}" cmake --build "$BUILD_DIR" --target be300_web -j"$JOBS"

echo "Built web bundle in $BUILD_DIR/web"
