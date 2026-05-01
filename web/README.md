# BE-300 Web Build

This repo can build a browser-only BE-300 frontend into `build-web/web/`.
The frontend cold-boots WinCE 3.0 from a NAND restore image
(`ce/restore_images/All_nand_300.bin`) — there is no Linux boot path. The
web target adds Emscripten glue in `web/main_web.c` plus static assets in
`web/`, and uses a small Emscripten-only NE2000 transmit hook in the CF
device so browser networking can be bridged.

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

Web boots initialize the guest RTC from the browser host's local date/time by
default, matching the native emulator's `--rtc-host-time` option.

The Speed control defaults to **Full speed** and can be changed while the
emulator is running. Non-zero values are web worker step-batch pacing units,
not MHz.

## Accessories

The controls panel can attach optional boot-time accessories:

- The primary PCMCIA dropdown chooses no card, CF slot 0, or NE2000. Choosing
  CF slot 0 opens the file picker immediately; the selected upload is written
  into MEMFS as `/cf0.img` before machine creation. CF slot 0 and NE2000 are
  mutually exclusive because both use the primary PCMCIA socket.
- CF slot 1 uploads are written into MEMFS as `/cf1.img`. Slot 1 can be
  staged, but the native emulator still warns that secondary socket MMIO is
  not decoded yet, so current WinCE visibility is limited.
- NE2000 attaches the existing PCMCIA NE2000 card model. The MAC and bridge
  fields are enabled only when NE2000 is selected. A MAC override may be
  supplied as a unicast `aa:bb:cc:dd:ee:ff` address.
- Targus / Stowaway keyboard enables the existing COM1 serial keyboard dock
  model and forwards browser keydown/keyup events through the same scancode
  table used by the SDL frontend. The dock probe handshake completes
  automatically; subsequent keypresses synthesize a CommMode modem event so
  the WinCE OAL dispatches GIRQ0-4-4 to serial.dll and the queued bytes
  reach the user-mode `WaitCommEvent` blocked thread (without this
  dispatch, bytes would sit in the UART RX queue forever — see
  `src/be300_devices.c:be300_stowaway_signal_uart_irq`).

If NE2000 is enabled without a bridge URL, the card uses GXemul's internal
IPv4 gateway model. If a `ws://` or `wss://` bridge URL is supplied, boot
still starts immediately with the internal gateway while the worker connects
to the bridge in the background. Once connected, the web build forwards raw
Ethernet frames to that WebSocket and injects binary frames received from the
socket back into the NE2000 receive path. Each WebSocket binary message is
exactly one Ethernet frame; text messages are ignored. The browser cannot
open TAP/raw sockets by itself, so real host LAN access requires a separate
bridge process implementing that protocol.

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
