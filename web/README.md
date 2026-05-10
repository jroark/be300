# BE-300 Web Build

This repo can build a browser-only BE-300 frontend into `build-web/web/`.
The frontend cold-boots hosted NAND images from `nand/*.bin`; there is no
kernel ELF boot path. The web target adds Emscripten glue in
`web/main_web.c` plus static assets in `web/`, and uses a small
Emscripten-only NE2000 transmit hook in the CF device so browser networking
can be bridged.

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
- packages native `be300-net-bridge` helper downloads for macOS, Windows,
  and Linux

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

EMSCRIPTEN_PREFIX="$(brew --prefix emscripten)"

EMSDK_PYTHON=/opt/homebrew/bin/python3.14 \
EM_CONFIG="$PWD/build-web/.emscripten" \
emcc --generate-config

EMSDK_PYTHON=/opt/homebrew/bin/python3.14 \
EM_CONFIG="$PWD/build-web/.emscripten" \
EM_LLVM_ROOT="$EMSCRIPTEN_PREFIX/libexec/llvm/bin" \
EM_BINARYEN_ROOT="$EMSCRIPTEN_PREFIX/libexec/binaryen" \
EM_CACHE="$PWD/build-web/.emcache" \
emcmake cmake -S . -B build-web -DBUILD_WEB=ON

EMSDK_PYTHON=/opt/homebrew/bin/python3.14 \
EM_CONFIG="$PWD/build-web/.emscripten" \
EM_LLVM_ROOT="$EMSCRIPTEN_PREFIX/libexec/llvm/bin" \
EM_BINARYEN_ROOT="$EMSCRIPTEN_PREFIX/libexec/binaryen" \
EM_CACHE="$PWD/build-web/.emcache" \
cmake --build build-web --target be300_web -j4

bash tools/build_net_bridge.sh
```

## Output

The generated browser bundle is written to:

- `build-web/web/index.html`
- `build-web/web/app.js`
- `build-web/web/worker.js`
- `build-web/web/styles.css`
- `build-web/web/be300_frame.png`
- `build-web/web/buttons_dpad_bw_mask.png`
- `build-web/web/favicon.svg`
- `build-web/web/be300_web.js`
- `build-web/web/be300_web.wasm`
- `build-web/web/downloads/be300-net-bridge-macos-amd64.tar.gz`
- `build-web/web/downloads/be300-net-bridge-macos-arm64.tar.gz`
- `build-web/web/downloads/be300-net-bridge-linux-amd64.tar.gz`
- `build-web/web/downloads/be300-net-bridge-windows-amd64.zip`
- `build-web/web/nand/*.bin`

## Local run

Serve the generated directory with any static file server:

```bash
python3 -m http.server --directory build-web/web 8000
```

Then open <http://127.0.0.1:8000>, choose a hosted NAND image, and click
**Boot**. The build copies every local `nand/*.bin` file into
`build-web/web/nand/`; the page currently exposes `300.bin`, `beshell.bin`,
`expod.bin`, `mw.bin`, `net.bin`, `opie.bin`, and `picogui.bin`. You can
also upload a local override through the file input; the worker writes the
selected bytes into MEMFS at `/All_nand_300.bin` before calling
`be300_create()`.

Web boots initialize the guest RTC from the browser host's local date/time by
default, matching the native emulator's `--rtc-host-time` option. Hosted
boots default to NE2000 and the Targus / Stowaway serial keyboard dock, but
both can be disabled from the controls panel. `opie.bin` boots with 64 MB
SDRAM; all other hosted images use 16 MB.

The Speed control defaults to **Full speed** and can be changed while the
emulator is running. Full speed uses a low-latency worker scheduler and a
short time-budgeted execution slice. Non-zero values keep the older
timer-paced step-batch model; they are pacing units, not MHz.

## Accessories

The controls panel attaches optional boot-time accessories:

- The primary PCMCIA dropdown chooses no card, CF slot 0, or NE2000. It
  defaults to NE2000. Choosing CF slot 0 opens the file picker immediately;
  the selected upload is written into MEMFS as `/cf0.img` before machine
  creation. CF slot 0 and NE2000 are mutually exclusive because both use the
  primary PCMCIA socket.
- CF slot 1 uploads are written into MEMFS as `/cf1.img`. Slot 1 can be
  staged, but the native emulator still warns that secondary socket MMIO is
  not decoded yet, so current WinCE visibility is limited.
- Targus / Stowaway keyboard defaults on. It uses the existing COM1 serial
  keyboard dock model and forwards browser keydown/keyup events through the same scancode
  table used by the SDL frontend. The dock probe handshake completes
  automatically; subsequent keypresses synthesize a CommMode modem event so
  the WinCE OAL dispatches GIRQ0-4-4 to serial.dll and the queued bytes
  reach the user-mode `WaitCommEvent` blocked thread (without this
  dispatch, bytes would sit in the UART RX queue forever — see
  `src/be300_devices.c:be300_stowaway_signal_uart_irq`).

If NE2000 is enabled without a bridge URL, the card uses GXemul's internal
IPv4 gateway model. The controls default to `ws://127.0.0.1:8765`, matching
the downloadable native `be300-net-bridge` helpers linked from the page.
Download the helper for your operating system, extract it, and run the
binary:

```bash
./be300-net-bridge
```

On Windows, run `be300-net-bridge.exe`.

Boot still starts immediately while the worker connects to the bridge in the
background. Once connected, the web build forwards raw Ethernet frames to
that WebSocket and injects binary frames received from the socket back into
the NE2000 receive path. Each WebSocket binary message is exactly one Ethernet
frame; text messages are ignored. The helper listens on loopback by default
and provides user-mode DHCP/ARP/DNS plus outbound TCP/UDP proxying through
ordinary host sockets, so it does not need TAP drivers or administrator
privileges, and it does not require Node.js. It does not provide inbound LAN
access to guest services.

## Boot flow

1. The page fetches (or reads from upload) a 16,449,536-byte NAND restore
   image and posts the bytes plus any optional CF images to the worker.
2. The worker calls `_be300_web_load_nand(ptr, len)` which writes the bytes
   to `/All_nand_300.bin` in MEMFS, and calls `_be300_web_load_cf(slot, ptr,
   len)` for selected CF uploads.
3. The worker calls `_be300_web_create_ex(sdramMb, targetMhz, flags, mac)`.
   The `targetMhz` name is legacy; in web mode the same value is used by the
   worker as the step-batch pacing value. That glue builds a
   `machine_config_t`, calls `be300_create()`, and flips `web_mode`,
   `use_builtin_ui`, and `mirror_serial_to_stdout` on the returned
   `machine_t *`. No SDL window is opened.
4. The worker drives `_be300_step`, `_be300_copy_frame_rgba8888`,
   `_be300_drain_serial`, `_be300_set_touch`, `_be300_set_buttons`,
   `_be300_web_stowaway_key`, `_be300_web_net_rx`, `_be300_web_net_tx_pop`,
   `_be300_stop`, and `_be300_destroy` directly.

## Publish to linux4be.com

The production site is served from S3 bucket `s3://linux4be.com` through
CloudFront distribution `E2LF6QBIW7PZR`.

Back up the current site before overwriting it:

```bash
BACKUP_PREFIX="_backups/$(date -u +%Y%m%dT%H%M%SZ)"
aws s3 sync s3://linux4be.com "s3://linux4be.com/$BACKUP_PREFIX" \
  --exclude "_backups/*"
```

Publish the freshly built bundle while preserving backups:

```bash
aws s3 sync build-web/web/ s3://linux4be.com/ --delete \
  --exclude "_backups/*"
aws cloudfront create-invalidation --distribution-id E2LF6QBIW7PZR \
  --paths "/*"
```
