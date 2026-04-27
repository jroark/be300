# BE-300 Web Build

This repo can build a browser-only BE-300 frontend into `build-web/web/`.
The frontend cold-boots WinCE 3.0 from a NAND restore image
(`ce/restore_images/All_nand_300.bin`) — there is no Linux boot path. The
emulator core (`src/`, `gxemul/`) is unchanged; the web target only adds
`web/main_web.c` (Emscripten glue) plus the static assets in `web/`.

## One-command build

Use the helper script from the repo root:

```bash
./tools/build_web.sh
```

The script:

- creates `build-web/` if needed
- picks a Python 3.10+ interpreter for Emscripten, preferring the Homebrew
  Python used on this machine
- generates `build-web/.emscripten` on first run
- applies the Homebrew Emscripten environment overrides that were required on
  this repo
- configures `BUILD_WEB=ON`
- builds the `be300_web` target

## Optional overrides

The script respects these environment variables if you want to override the
defaults:

- `BUILD_DIR`
- `JOBS`
- `EMSDK_PYTHON`
- `EM_CONFIG`
- `EM_LLVM_ROOT`
- `EM_BINARYEN_ROOT`
- `EM_CACHE`

Example:

```bash
JOBS=8 ./tools/build_web.sh
```

## Verified manual fallback

If you want to run the commands yourself instead of the script, this is the
manual flow that was verified on this repo:

```bash
mkdir -p build-web

EMSDK_PYTHON=/opt/homebrew/bin/python3.14 \
EM_CONFIG="$PWD/build-web/.emscripten" \
emcc --generate-config

EMSDK_PYTHON=/opt/homebrew/bin/python3.14 \
EM_CONFIG="$PWD/build-web/.emscripten" \
EM_LLVM_ROOT=/opt/homebrew/Cellar/emscripten/5.0.4/libexec/llvm/bin \
EM_BINARYEN_ROOT=/opt/homebrew/Cellar/emscripten/5.0.4/libexec/binaryen \
EM_CACHE="$PWD/build-web/.emcache" \
emcmake cmake -S . -B build-web -DBUILD_WEB=ON

EMSDK_PYTHON=/opt/homebrew/bin/python3.14 \
EM_CONFIG="$PWD/build-web/.emscripten" \
EM_LLVM_ROOT=/opt/homebrew/Cellar/emscripten/5.0.4/libexec/llvm/bin \
EM_BINARYEN_ROOT=/opt/homebrew/Cellar/emscripten/5.0.4/libexec/binaryen \
EM_CACHE="$PWD/build-web/.emcache" \
cmake --build build-web --target be300_web -j4
```

## Output

The generated browser bundle is written to:

- `build-web/web/index.html`
- `build-web/web/app.js`
- `build-web/web/worker.js`
- `build-web/web/styles.css`
- `build-web/web/be300_frame.png`
- `build-web/web/favicon.svg`
- `build-web/web/be300_web.js`
- `build-web/web/be300_web.wasm`

## Local run

Copy the NAND image next to the served HTML so the page can fetch it on
boot, then serve the directory with any static file server:

```bash
cp ce/restore_images/All_nand_300.bin build-web/web/
python3 -m http.server --directory build-web/web 8000
```

Then open <http://127.0.0.1:8000> and click **Boot**. If
`./All_nand_300.bin` is not bundled with the page you can also upload one
via the file input on the controls panel; the worker writes it into MEMFS at
`/All_nand_300.bin` before calling `be300_create()`.

## Boot flow

1. The page fetches (or reads from upload) a 16,449,536-byte NAND restore
   image and posts the bytes to the worker.
2. The worker calls `_be300_web_load_nand(ptr, len)` which writes the bytes
   to `/All_nand_300.bin` in MEMFS.
3. The worker calls `_be300_web_create(sdramMb, targetMhz)`. That glue
   builds a `machine_config_t { .nand_path = "/All_nand_300.bin", ... }`,
   calls the existing public `be300_create()`, and flips `web_mode`,
   `use_builtin_ui`, `mirror_serial_to_stdout` on the returned
   `machine_t *`. No SDL window is opened.
4. The worker drives `_be300_step`, `_be300_copy_frame_rgba8888`,
   `_be300_drain_serial`, `_be300_set_touch`, `_be300_set_buttons`,
   `_be300_stop`, and `_be300_destroy` directly — those entry points are
   the existing public API in `src/be300.h`; the web build does not modify
   the emulator core.
