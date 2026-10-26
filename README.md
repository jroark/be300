# BE-300 Emulator

Casio Cassiopeia BE-300 emulator using a lightly integrated GXemul 0.7.0 MIPS
CPU core. The active target is accurate cold boot of Windows CE 3.0 from the
real masked boot ROM and a raw NAND restore image.

NAND restore images, Casio backup images, restore-package binaries, and vendor
PDFs are not distributed in this repository. Place local copies at the expected
paths before running WinCE boot tests; see
[`docs/LOCAL_ASSETS.md`](docs/LOCAL_ASSETS.md).

The emulator must boot the unmodified ROM and NAND image through the same chain
as hardware after battery removal:

```text
ROM reset vector -> SPL/Kloader from NAND -> NK.exe decompression -> WinCE 3.0
```

## Current Status

The primary boot target is:

```bash
./be300 --nand ../ce/restore_images/All_nand_300.bin
```

Current implementation status and investigation guidance live in
[`docs/CURRENT_STATUS.md`](docs/CURRENT_STATUS.md).

Historical pass handoffs and superseded investigation notes are archived in
[`docs/archive/`](docs/archive/).

## Requirements

- CMake 3.10+
- C11 compiler
- Git submodules for GXemul, Dear ImGui, and native file dialogs
- SDL2 for the default native launcher, interactive framebuffer window, and
  host audio. An asset-free headless CLI can instead be built with
  `-DBUILD_LAUNCHER=OFF` when SDL2 is unavailable.
- libpng is optional; it enables the rendered BE-300 bezel in SDL builds
- `gtimeout` or GNU `timeout` for bounded boot smoke tests

The GXemul CPU engine is a submodule. Fresh clones need:

```bash
git submodule update --init
```

## Build

```bash
mkdir -p build-host
cd build-host
cmake ..
make -j$(nproc)
```

On macOS, replace `$(nproc)` with the number of jobs you want to run, or use a
shell that provides `nproc`.

Run `./build-host/be300` with no arguments to open the native VM manager. It
creates reusable `.be300vm` bundles, remembers display and accessory settings,
and can boot a selected NAND image without assembling the CLI options by hand.
Pass `--nand` and other options to bypass the launcher and run directly.

### Windows x86_64 ZIP

From macOS, the MinGW cross-build helper produces a portable Windows package:

```bash
brew install mingw-w64
./tools/build_windows.sh
```

The output is `dist/be300-windows-amd64.zip`. It contains `be300.exe`,
`SDL2.dll`, and a `run-be300.bat` helper. The WinCE NAND image is not bundled;
copy your local image to the Windows machine and run:

```cmd
be300.exe --nand C:\path\to\All_nand_300.bin
```

## Run WinCE 3.0

Provide a local NAND image first:

```text
ce/restore_images/All_nand_300.bin
```

Then run:

```bash
cd build-host
./be300 --nand ../ce/restore_images/All_nand_300.bin
```

Serial output is written to stdout. Emulator diagnostics are written to stderr.
An SDL window opens for the framebuffer when SDL is available. Panel and
backlight power control presentation; see [LCD model](docs/BE300_LCD.md).
Set `BE300_AUDIO=0` to mute host audio without changing guest-visible
hardware state. The separate piezo buzzer's register behavior remains
unverified. Touch input coalesces rapid host click events into one guest
pen session. `BE300_TOUCH_MIN_DWELL_MS`
sets the minimum guest pen-down time in milliseconds; set it to `0` for direct
host timing.

Native cold boot leaves the RTC in the first-boot/default state so WinCE can
prompt for date/time like real hardware after battery removal. For interactive
application or network testing, add `--rtc-host-time` to initialize the guest
RTC from host local time.

For a bounded smoke test:

```bash
cd build-host
gtimeout 60s ./be300 --nand ../ce/restore_images/All_nand_300.bin \
  > cold_stdout.log 2> cold_stderr.log
```

Exit code `124` from `gtimeout` is expected for a long-running boot; WinCE does
not exit on its own.

## Install Applications Offline

`be300-install` applies Casio `.cbea` application packages directly to a copy
of the NAND image, without a Windows VM or PC Connect session:

```bash
./build-host/be300-install \
  --output /tmp/All_nand_300_apps.bin \
  ce/restore_images/All_nand_300.bin \
  path/to/package.cbea
./build-host/be300 --nand /tmp/All_nand_300_apps.bin
```

The web frontend provides the same operation through its **Install apps
(.cbea)** picker and file-drop support. Package formats, transactional behavior,
and current limitations are documented in
[`docs/INSTALLING_APPS.md`](docs/INSTALLING_APPS.md).

## Regression Tests

The asset-free unit suite covers the RTC, PMU, ROM/SPL NAND path, USB function
controller, host bridges, launcher configuration, and `.cbea` installer:

```bash
cmake -S . -B build-host -DBUILD_TESTING=ON
cmake --build build-host -j4
ctest --test-dir build-host --output-on-failure -LE local
```

To register the full cold-boot regression, configure with the local WinCE 3.0
image. The test copies the image before running, verifies that the source hash
does not change, and checks the serial milestones through the Casio shell:

```bash
cmake -S . -B build-host \
  -DBUILD_TESTING=ON \
  -DBE300_TEST_NAND="$PWD/ce/restore_images/All_nand_300.bin"
cmake --build build-host -j4
ctest --test-dir build-host --output-on-failure -R cold_boot
```

Logs and the final screenshot are written under
`build-host/cold-boot-artifacts/`. Public CI intentionally runs only the
asset-free suite because the restore image cannot be redistributed.

## Secondary Paths

These paths remain implemented for targeted investigation, but they are not the
primary regression target:

- `--restore --cf` — CompactFlash recovery path using a local NANDWRITER tool
- `--ppsh` — WinCE PPSH debug-shell probe path
- `--ne2000` — PCMCIA NE2000 Ethernet card model
- `--xi825` — Z-Com XI-825 wireless CF card with a virtual access point;
  requires the local vendor driver. See [XI-825 setup](docs/XI825.md).
- `--rtc-host-time` — initialize the guest RTC from host local time
- `--audio` — route emulated wave-audio output to SDL
- `--stowaway-keyboard` — attach the Targus/Think Outside serial keyboard dock
- `--serial0-bridge` / `--serial1-bridge` — expose either emulated UART to a
  host TCP, Unix-socket, or PTY chardev
- `--pcconnect-bridge` — bridge the dock cable to a host chardev for real PC
  Connect, carrying serial bytes by default or usbredir packets with
  `--pcconnect-dock usb-sync`
- `--trace`, `--log-mmio`, `--mmio-coverage`, `--detect-stall` — diagnostic
  options for controlled investigations

Run `./be300 --help` for the current option list.

## Reference Material

Primary hardware and boot references:

- NEC VR4131 users manual (`Vr4131-um_200203.pdf`, local copy)
- NEC VRC4173 users manual (`U14579EJ2V0UM00.pdf`, local copy)
- [`docs/hardware/hardware.txt`](docs/hardware/hardware.txt)
- [`docs/hardware/hw_dump_vr4131.txt`](docs/hardware/hw_dump_vr4131.txt)
- [`docs/hardware/hw_dump_vrc4173.txt`](docs/hardware/hw_dump_vrc4173.txt)
- [`docs/HARDWARE_GROUND_TRUTH.md`](docs/HARDWARE_GROUND_TRUTH.md)
- [`docs/HARDWARE_SURVEY_SYNTHESIS.md`](docs/HARDWARE_SURVEY_SYNTHESIS.md)
- [`docs/ROM_SPL_HANDOFF.md`](docs/ROM_SPL_HANDOFF.md)
- [`docs/LEGITIMATE_FIXES_NOT_APPLIED.md`](docs/LEGITIMATE_FIXES_NOT_APPLIED.md)

The WinCE 3.0 restore image reference is
[`ce/restore_images/RESTORE_IMAGES.md`](ce/restore_images/RESTORE_IMAGES.md).

## Development Notes

- Keep guest binaries unmodified. Fix hardware emulation bugs instead of
  patching WinCE, pre-seeding RAM, or forcing handoff state.
- Commit only functional changes or maintained documentation. Do not commit
  one-off probes, local logs, extracted modules, screenshots, or build outputs.
- Use the Docker `mips-dev` environment for MIPS and WinCE binary analysis:

```bash
docker compose build mips-dev
docker compose run --rm mips-dev /bin/bash
```
