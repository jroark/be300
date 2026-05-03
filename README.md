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
- SDL2 for the interactive framebuffer window and host buzzer audio
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
An SDL window opens for the framebuffer when SDL is available. The Casio
piezo buzzer is rendered through SDL audio; set `BE300_AUDIO=0` to mute host
audio without changing guest-visible hardware state. Touch input coalesces
rapid host click events into one guest pen session. `BE300_TOUCH_MIN_DWELL_MS`
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

## Secondary Paths

These paths remain implemented for targeted investigation, but they are not the
primary regression target:

- `--restore --cf` — CompactFlash recovery path using a local NANDWRITER tool
- `--ppsh` — WinCE PPSH debug-shell probe path
- `--ne2000` — PCMCIA NE2000 Ethernet card model
- `--rtc-host-time` — initialize the guest RTC from host local time
- `--pcconnect-time-sync` — experimental PC Connect serial time-sync peer
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
