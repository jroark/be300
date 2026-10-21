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
- `build-web/web/launch-config.mjs`
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
`build-web/web/nand/`; the page groups the Windows CE images together and
exposes a nine-image Linux matrix: Microwindows, OPIE, and PicoGUI on Linux
2.4.18, 2.6.8.1, and 4.2.9 (`mw-*.bin`, `opie-*.bin`, and
`picogui-*.bin`), plus Glaze (`glaze.bin`), a small microkernel OS with an
early-iPhone-OS-style UI built from <https://github.com/jroark/glaze-os>.
Choosing
**Your own image file…** in the **System image** dropdown boots a local NAND
image instead; the worker writes the selected bytes into MEMFS at
`/All_nand_300.bin` before calling `be300_create()`.

Release `20261020c` ships Glaze from commit `cccc78e` (plus local WinCE
compatibility updates), including demand-paged NAND romfs, WinCE app
compatibility, and the AIM package work. Its App Store (`appstore.glaze-os.com`,
which needs networking, i.e. the net bridge) offers Minesweeper, Solitaire,
Fifteen, Sketch, Task Manager and AIM; the factory image also has system
sounds and NTP time with named time zones.
Build its factory NAND from the sibling `glaze-os` checkout
with `./build.sh -s OUT=out/web-release-20261020c INIT=bin/springboard DYNAMIC=1
LD_TESTS=0 MEM_TESTS=0`, then copy the resulting `glaze.bin` to this repo's
`nand/glaze.bin` before building the web bundle. The 16 MiB release image has
SHA-256 `718d669ec649f5dfc69c514ceb22e954b18fe6a1d930b05fdeba3b50e5b53eb4`.
Keep development/test images outside `nand/`, since every `.bin` there is
packaged for hosting. Save any image containing user data before replacing it
with a fresh factory image.

**Boot** starts a fresh machine with the current settings. **Restart**
reboots the current machine and keeps its NAND and CF changes. **Stop** halts
it. Settings changed while running apply on the next boot. The serial
console is a collapsed drawer below the device; it shows a "new output"
marker when the guest prints while it is closed.

The device frame's buttons (Rocket, OK, Esc, Power and the D-pad) are
clickable. When the Stowaway keyboard is off, the arrow keys, Enter and Esc
press the D-pad, OK and Esc buttons. **Hold Rocket button** latches the
Rocket key.

Web boots initialize the guest RTC from the browser host's local date/time by
default, matching the native emulator's `--rtc-host-time` option. Hosted
boots default to NE2000 and the Targus / Stowaway serial keyboard dock, but
both can be disabled from the settings panel. All hosted images use the
BE-300's stock 16 MB SDRAM configuration. An uploaded image whose filename
contains `opie64` still selects the emulator-only 64 MB profile.

Casio `.cbea` application packages can be added with **Add .cbea
packages** or dropped anywhere on the page. Before boot they are queued and
applied to the staged NAND image when **Boot** is pressed. While the emulator
is running, accepting the reboot prompt first flushes guest NAND writes, then
installs the packages and reboots the updated image. Install diagnostics are
shown as `[INSTALL]` lines in the serial pane. See
[`../docs/INSTALLING_APPS.md`](../docs/INSTALLING_APPS.md) for supported INF
operations and limitations.

The Speed control (under **Advanced**) defaults to **Full speed** and can be changed while the
emulator is running. Full speed uses a low-latency worker scheduler and a
short time-budgeted execution slice. Non-zero values keep the older
timer-paced step-batch model; they are pacing units, not MHz.

## Launch links

Release `20261021a` adds URL settings and automatic boot. For example, this
link boots Glaze with the on-screen keyboard and no network card or helper:

<https://linux4be.com/?image=glaze&boot=1&card=none&keyboard=0>

Omit `boot=1` to preselect the configuration and wait for the **Boot** button.
Without URL settings, the existing image, card, keyboard and sound defaults
are unchanged. Parameters override only the controls explicitly named.

| Parameter | Accepted values |
| --- | --- |
| `image` | Hosted image ID: `300`, `beshell`, `expod`, `net`, `glaze`, or `mw-2.4`, `mw-2.6`, `mw-4.2`, `opie-2.4`, `opie-2.6`, `opie-4.2`, `picogui-2.4`, `picogui-2.6`, `picogui-4.2` |
| `boot` | `1` or `true` starts automatically; `0` or `false` waits for Boot |
| `card` | `none`, `ne2000`, or `cfblank` (an empty FAT16 card) |
| `cfSize` | Blank CF size in MB: `16`, `32`, `64`, `128`, `256`, `512` |
| `keyboard` | `1` / `true` attaches the Stowaway keyboard; `0` / `false` disables it |
| `audio` | `1` / `true` enables sound; `0` / `false` disables it |
| `speed` | An integer from `0` (full speed) through `64` (step-batch pacing units) |
| `mac` | Unicast MAC such as `10:20:30:00:00:10`; empty uses the existing default |
| `bridge` | A `ws://` or `wss://` bridge URL; `bridge=` selects the internal network |

Use URL encoding for bridge addresses containing query strings. For example:

```text
https://linux4be.com/?image=beshell&boot=1&card=ne2000&bridge=ws%3A%2F%2F127.0.0.1%3A8765&keyboard=1
https://linux4be.com/?image=glaze&boot=1&card=cfblank&cfSize=32&audio=0
```

Image IDs select only the hosted catalogue. Links cannot supply arbitrary
NAND URLs, local uploads, CF image uploads, or `.cbea` packages; choose those
files interactively. Unknown query parameters (such as campaign tags) are
ignored. Invalid or repeated recognized parameters show an error and prevent
automatic boot, so a misspelled image never silently boots the default OS.
The valid settings still appear in the controls for manual adjustment.

Automatic boot uses the same validation and worker boot path as the Boot
button. Sound starts on the first pointer or keyboard interaction, subject to
the browser's audio policy; it does not hold up boot. Restart and Stop retain
their normal behavior and do not reapply URL settings.

Run the dependency-free parser and frontend integration tests with Node 20+:

```bash
node --experimental-vm-modules --test tests/web_launch_config.mjs
```

## Accessories

The settings panel attaches optional boot-time accessories:

- The **Card slot** dropdown chooses no card, an NE2000 network card, a CF
  card from an image file, or a new blank CF card. It defaults to NE2000, and
  only the selected card's options are shown. Choosing the image file opens
  the file picker immediately; the selected upload is
  written into MEMFS as `/cf0.img` before machine creation. CF and NE2000 are
  mutually exclusive because both use the primary PCMCIA socket.
- "New blank CF card" builds a card in the browser: pick a size
  (16-512 MB), then add files, a folder (its structure is kept), or drop
  files/folders on the panel. At boot the worker formats a bare FAT16 volume
  with `_be300_web_cf_create` (the same layout as `tools/make_cf_image.sh`),
  writes each file with `_be300_web_cf_add_file`, and stages it with
  `_be300_web_cf_commit`. The files appear under `\Storage Card`. Names are
  stored as 8-bit FAT long names, so characters outside Latin-1 and the FAT
  reserved set become `_`.
- "Download card image" (shown while a CF card is attached) saves the slot-0 card (uploaded or blank) as
  `be300-cf.img`, including the guest's writes (`_be300_web_cf_flush` pushes
  them to MEMFS first). Restart and running `.cbea` installs reboot with the
  guest's current CF contents rather than the originally selected bytes.
- Second CF slot uploads (under **Advanced**) are written into MEMFS as `/cf1.img`. Slot 1 can be
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

- **Sound** defaults on and can be toggled while running. The buzzer and the
  AIU wave output (the native `--audio` path) are written as 44100 Hz mono
  S16 into a ring in `src/ui.c` instead of an SDL device, because Web Workers
  have no `AudioContext`. `worker.js` drains it with `_be300_web_audio_drain`
  after every slice and posts the chunks to `app.js`, which schedules them on
  an `AudioContext` created from the Boot click (browser autoplay rules).

If NE2000 is enabled without a bridge URL, the card uses GXemul's internal
IPv4 gateway model. The controls default to `ws://127.0.0.1:8765`, matching
the downloadable native `be300-net-bridge` helpers linked from the page.
Download the helper for your operating system, extract it, and run the
binary before booting:

```bash
./be300-net-bridge
```

On Windows, run `be300-net-bridge.exe`.

When a bridge URL is configured, the worker connects to the bridge before
starting the guest CPU so DHCP OFFER and ACK packets come from one network
backend. Once connected, the web build forwards raw Ethernet frames to that
WebSocket and injects binary frames received from the socket back into the
NE2000 receive path. Each WebSocket binary message is exactly one Ethernet
frame; text messages are ignored. The helper listens on loopback by default
and provides user-mode DHCP/ARP/DNS plus outbound TCP/UDP proxying through
ordinary host sockets, so it does not need TAP drivers or administrator
privileges, and it does not require Node.js. It does not provide inbound LAN
access to guest services.

## Boot flow

1. The page fetches (or reads from upload) a 16,449,536-byte NAND restore
   image and posts the bytes plus any optional CF images to the worker.
2. The worker calls `_be300_web_load_nand(ptr, len)` to write the bytes to
   `/All_nand_300.bin` in MEMFS. Queued packages are then applied with
   `_be300_web_install_cbea`; selected CF uploads are staged with
   `_be300_web_load_cf(slot, ptr, len)`, and a blank card is built with
   `_be300_web_cf_create` / `_be300_web_cf_add_file` / `_be300_web_cf_commit`.
3. The worker calls `_be300_web_create_ex2(sdramMb, targetMhz, flags, mac,
   fbWidth, fbHeight)`. The flags select optional CF slots, NE2000, the
   Stowaway dock, host audio, and an explicit MAC address. The `targetMhz`
   name is legacy; in web mode the same value is used by the worker as the
   step-batch pacing value. The glue builds a `machine_config_t`, calls
   `be300_create()`, and flips `web_mode`, `use_builtin_ui`, and
   `mirror_serial_to_stdout` on the returned `machine_t *`. No SDL window is
   opened.
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
  --exclude "_backups/*" --exclude "index.html"
aws s3 cp build-web/web/index.html s3://linux4be.com/index.html \
  --content-type "text/html" --cache-control "no-cache"
aws cloudfront create-invalidation --distribution-id E2LF6QBIW7PZR \
  --paths "/*"
```

`index.html` is the only unversioned entry point, so it is served with
`Cache-Control: no-cache`; everything it loads carries the `?v=` asset tag.
Bump `ASSET_VERSION` in `web/app.js` together with the matching `?v=` tags
in `web/index.html` and `web/worker.js` on every release, or browsers can
pair a cached `be300_web.js` with a new `be300_web.wasm` and fail with a
`LinkError`.
