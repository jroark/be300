# Current Status

Last updated: 2026-10-13

## Active Target

The primary target is accurate cold boot of Windows CE 3.0 from:

```bash
./be300 --nand ../ce/restore_images/All_nand_300.bin
```

The NAND image is a local prerequisite and is not redistributed in Git; see
`docs/LOCAL_ASSETS.md`.

The emulator should run the real masked ROM, load the SPL from NAND, decompress
NK.exe, start WinCE, and reach the first-boot UI without guest patches, RAM
seeds, or synthetic handoff state.

Secondary implemented paths, used for targeted investigation only:

- `--restore --cf` for the local NANDWRITER/CompactFlash recovery path
- `--ppsh` for the WinCE debug-shell probe path
- `--ne2000` for the PCMCIA NE2000 Ethernet card path
- `--rtc-host-time` for interactive boots that should start with the host
  local date/time instead of the first-boot default RTC value
- `--pcconnect-bridge` connects the selected RS-232 or USB-sync dock path to a
  host TCP/Unix/PTY chardev so real `PCConnect.exe` can talk to the guest;
  USB-sync presents the emulated USB function controller through usbredir,
  while the default RS-232 mode pipes the VRC4173 SIU byte stream;
  `--serial0-bridge`/`--serial1-bridge` expose the VR4131 main SIU / VRC4173
  companion SIU as plain host serial bridges; see
  `docs/PC_CONNECT_BRIDGE.md`
- `--stowaway-keyboard` attaches the Targus/Think Outside Stowaway dock
- `--audio` routes the always-active Kj companion wave-DMA model to the host
  SDL audio device; the browser frontend routes the same PCM plus buzzer audio
  through Web Audio

A portable Windows x86_64 build is produced from macOS by the MinGW
cross-build helper `tools/build_windows.sh` (output
`dist/be300-windows-amd64.zip`).

Casio `.cbea` applications can be applied directly to a NAND image with
`be300-install`, or queued through the web frontend's package picker/drop
target. The installer updates the FAT16 filesystem and compact registry
snapshot transactionally; see `docs/INSTALLING_APPS.md`.

## Regression Baseline

- Public CI performs an asset-free headless build and runs the maintained CTest
  suite. The suite directly covers the active RTC, PMU, ROM/SPL NAND, USB,
  bridge, configuration, and package-install paths.
- A local-only `cold_boot` CTest is registered when CMake receives
  `-DBE300_TEST_NAND=/absolute/path/All_nand_300.bin`. It boots a temporary copy
  and requires ordered KLOADER, NK, WinCE kernel, and Casio shell serial
  milestones. The source NAND hash must remain unchanged.
- When SDL is available, the local test also requires an orderly-shutdown
  screenshot that is nonblank and no longer matches `Initializing.bmp` or
  `Starting.bmp`. Artifacts live in `build-host/cold-boot-artifacts/`.
- The BE-300 machine object owns only board/companion device state. VR4131 RTC,
  CMU, PMU, ICU, BCU, and GPIO state live on the active GXemul `dev_vr41xx`
  path rather than in unused shadow structs.

## Current Behavior

- Cold boot reaches the WinCE shell / first-boot UI path. The historical
  `Starting...` splash stall and Welcome.exe/launcher dependency theories are
  superseded.
- User-mode display rendering is known to reach the VRC4173 framebuffer mapping.
- Touch input is usable enough to complete calibration and reach applications.
  If touch behavior regresses, investigate PIU handshaking as a regression from
  the current baseline rather than as the primary boot blocker.
- Without `--cf`, the PCMCIA/CF socket model matches real hardware with the
  adapter/socket present but no media inserted: the CF icon can be present, CF
  Slot Information reports `Card type: None` / `Card unit: Set`, and the
  pulled-up card-memory window is not detected as an unknown PCCard. The
  VRC4173 hardware dump was captured with a CF memory card inserted, so
  inserted-card CF companion values are not no-card defaults.
- The PIU model follows the VRC4173 two-page coordinate buffer model, uses
  hardware-captured timing defaults from `docs/hardware/hw_dump_vrc4173.txt`,
  and schedules conversion completion against emulated CP0 Count cycles instead
  of host wall-clock time.
- GXemul CP0 Compare delivery is now driven by emulated instruction progress,
  not host SIGALRM cadence. This removes the trace/logging dependency where
  WinCE scheduler ticks arrived only when diagnostics slowed the emulator.
- Native boots default to the cold-boot RTC presentation, which lets WinCE ask
  the user to set date/time. `--rtc-host-time` initializes the VR4131 RTC from
  host local time for interactive testing. The RTC elapsed counter uses WinCE's
  1850 epoch; using a 2001 epoch made the guest display 1875 dates.
- Web boots enable the same host-time RTC path by default.
- The `Internet.exe` low-address exception seen with `--ne2000` was reproduced
  with the wrong RTC epoch and is gone after the 1850 epoch correction. Treat
  future NE2000 failures with a correct guest date as network/adapter binding
  issues, not as the old date-corruption crash.
- The USB function controller is connected to stock QEMU/UTM `usb-redir` via
  `--pcconnect-dock usb-sync`. The unmodified guest `usb.dll` supplies the
  `VID_07CF&PID_2002` descriptors, control handling, and bulk traffic; no
  custom QEMU dock device is required.
- The Kj companion wave-audio model services the guest's ping-pong DMA and
  SYSINTR `0x14`. Native `--audio` and the web Sound toggle control only host
  playback, not the guest-visible DMA/interrupt behavior.

## Current Investigation Focus

There is no current mandatory boot-blocker investigation in this file. For a
new regression, first confirm the baseline command and guest date:

```bash
./be300 --nand ../ce/restore_images/All_nand_300.bin --rtc-host-time
```

If touch calibration regresses, focus on PIU hardware behavior and the WinCE
touch driver handshake:

- PIUINTREG pending/mask/W1C semantics
- page 0/page 1 buffer validity, ordering, and data-lost behavior when both
  page buffers remain valid
- PADSTATE transitions in WaitPenTouch, PenDataScan, and IntervalNextScan
- whether touch.dll consumes at least three stable coordinate pages per held
  calibration press

If NE2000 connectivity regresses with a correct guest date, focus on adapter
binding/IP configuration, PCMCIA I/O window routing, and NE2000 IRQ delivery.
Do not chase the resolved `Internet.exe` date/epoch crash unless the guest
clock is wrong again.

Do not revisit older Welcome.exe, launcher dependency, VirtualCopy, or
framebuffer-alias hypotheses unless a new trace contradicts the current PIU
evidence. Those investigations are archived under `docs/archive/`.

## Authoritative References

- `Vr4131-um_200203.pdf` (local NEC VR4131 users manual copy)
- `U14579EJ2V0UM00.pdf` (local NEC VRC4173 users manual copy)
- `docs/hardware/hardware.txt`
- `docs/hardware/hw_dump_vr4131.txt`
- `docs/hardware/hw_dump_vrc4173.txt`
- `docs/HARDWARE_GROUND_TRUTH.md`
- `docs/HARDWARE_SURVEY_SYNTHESIS.md`
- `docs/ROM_SPL_HANDOFF.md`
- `docs/LEGITIMATE_FIXES_NOT_APPLIED.md`
