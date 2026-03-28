# BE-300 Web Build

This repo can build a browser-only BE-300 frontend into `build-web/web/`.

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
- `build-web/web/be300_web.js`
- `build-web/web/be300_web.wasm`

## Local run

Serve the built directory with any static file server. Example:

```bash
python3 -m http.server --directory build-web/web 8000
```

Then open <http://127.0.0.1:8000>.
