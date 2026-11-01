# Current Status

Last updated: 2026-10-31

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

Since 2026-10-31, a cold boot stops at the touch-screen calibration prompt
("Align Screen"), the real battery-pull sequence's step 5 (CLAUDE.md). It used
to go straight to the shell. The change comes from `PMUINTREG` taking the VR4131
UM's layout (`src/hw/pmu.h`): `RTCRST`, which reports the power-on, is bit 4,
not bit 0. CE now sees a power-on, where before it saw bit 0, which is
`POWERSWINTR`. With `RTCRST` put back at bit 0 and the CMCU watchdog modelled,
CE showed its "Restart: Due to an OS problem..." dialog over the shell instead.
Tapping through calibration headless isn't scripted yet.

Secondary implemented paths, used for targeted investigation only:

- `--restore --cf` for the local NANDWRITER/CompactFlash recovery path
- `--ppsh` for the WinCE debug-shell probe path
- `--ne2000` for the PCMCIA NE2000 Ethernet card path
- `--wl54cf` for the native AmbiCom WL54-CF station model and Glaze MIPS
  driver; open virtual `BE300` AP, with local firmware upload; see
  `docs/WL54CF.md`. The supplied ARM CE 4.1 driver cannot run on WinCE 3.0.
- `--xi825` for the native Z-Com XI-825 wireless CF card and virtual `BE300`
  access point; see `docs/XI825.md` for the vendor driver and one-time
  WinCE driver/LAN-profile registration required by the minimal CIS
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
  SDL audio device; the browser frontend routes the same PCM through Web Audio

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

- LCD presentation follows the recovered panel enable, clock gates and
  backlight power state. Cold starts no longer inherit a post-WinCE LCD
  snapshot. Native display/screenshots and web/Android frame copies share
  blanking without modifying guest VRAM. Contrast serial commands are
  captured; optical curves and timing fields remain unmeasured. The old
  BLG-to-buzzer heuristic was removed because it interpreted backlight
  initialization as audio. See `docs/BE300_LCD.md` for evidence and limits.

- The actual piezo channel at `09A0/9A4/9A8` now follows the recovered
  `bltinbuz.dll` PWM sequence, with shared BLG config/clock and independent
  host mixing alongside stereo PCM. The old backlight-triggered pulses
  remain removed; see `docs/BE300_BUZZER.md`.

- Host button input tracks keyboard and pointer ownership separately, so one
  release cannot clear another held source. SDL focus loss and browser
  blur/hidden-page events release device buttons, touch and Stowaway keys.
  Web frame buttons accept multiple pointers. F12 drives the existing Power
  input in both frontends; arrows remain device buttons with the keyboard
  accessory off. `ui_input` tests the real SDL event loop and browser tests
  cover overlapping sources, chords and dwell timers. Display/EEPROM driver
  findings and the next capture targets are in `docs/WINCE_DISPLAY_CONTROLS.md`.

- XI-825 (2026-10-24): the unmodified vendor `nb825be.dll` 1.1.0 initializes
  the station controller, and Wireless LAN Setting shows `BE300`, channel 3,
  11 Mbps and BSSID `02:42:45:33:30:30`. After normal driver registration
  and LAN-profile setup, reboot binds the driver automatically, DHCP assigns
  `10.0.0.1` with gateway/DNS `10.0.0.254`, and the WinCE browser displays
  a host-served HTTP test page and resolves/loads `http://example.com/`.
  A captured XI-825 CIS is still needed to
  match the vendor INF's initial card-ID checksum without manual selection.

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
- Emulated time runs at the 165.888 MHz PClock (one instruction per cycle),
  matching the default `--speed 166` throttle, and CP0 Count advances at
  TClock/4 = 10.368 MHz, the rate WinCE's 1 ms tick (`Compare = Count + 10368`)
  and Linux's vr41xx timer code assume. A Count rate 8x too fast made
  GetTickCount run ~6x fast, so Stowaway keys auto-repeated after ~60 ms.
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

- Host audio preserves stereo L/R in both SDL and Web Audio, matching the
  service manual's headphone specification (printed pp. 2/5/52). Mono PCM and
  host-generated tone pulses are duplicated to both channels; guest buzzer
  register behavior remains unverified. Interpolation history resets
  at a new DMA stream or format change; browser queue capacities/drain results
  are stereo frames. Recording-sink, resampler/ring, muted-DMA and browser tests
  cover these paths. See `docs/SERVICE_MANUAL.md` for the board evidence and
  remaining EEPROM, power, LCD and reset validation prerequisites.

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

### Emulator hacks still in place (2026-10-21)

- **KjCMU warm reset** (`src/devices/nand_window.c`, the 7 to `0xA0C4` then
  10 to `0xA0C8` sequence). It cold-resets only the CPU and the serial-dock
  state; every other device keeps its state. Which units the real reset
  reaches depends on how the Kj companion drives the VR4131 reset input
  (RSTSW or a full system reset), and there is no hardware evidence for
  either. WinCE's second boot after `Boot.exe` relies on what survives, so
  the scope has not been widened.
- **D7FE "NIC probe" override** (`src/hw/nand.c`, three SPL PCs before the
  SPL entry). None of the regression images executes these PCs. They look
  like an EDBG/debug-boot path, possibly reached by the NANDWRITER/KLOADER
  `--restore` path, which can't be tested without NANDWRITER. Remove the
  override once that path can be tested.
- **Console capture of SPL writes to D7FC** (`src/hw/nand.c`). This is
  host-side logging only; guest-visible state is latched as usual.

### CE.Net white screen after idle power-off (reference image, open)

The CE.Net reference image (`net.bin`) boots to its shell, then after its
idle power-off timeout turns the LCD white and stops responding. (Before the
Count rate fix of 2026-10-21 the tick ran about 6x fast and this came about
45 s after the shell; it now comes within 7 minutes.) It happens with or without
`--stowaway-keyboard` and `--ne2000`. Traced 2026-10-21:

- CE.Net's idle power-off runs each driver's PowerDown in kernel mode on the
  KData stack (device.exe's handler at slot-0 `0x1302C`, kernel stack
  `0xFFFFD730`, CurAKey `0x5`, ASID 2).
- The last driver called is the third-party `Stowaway.dll` (Think Outside,
  Sep 2001), loaded at `0x01F2xxxx`. Its PowerDown is
  `EscapeCommFunction(g_hCom = NULL, CLRDTR)`, a system call that drivers
  must not make from PowerDown.
- coredll's DeviceIoControl stub (`0x03F8D73C`) reads the current thread's
  TLS through UserKData (`[0x5800] - 20` = `0x0A1FFEEC`). That page is in
  slot 5 (access key `0x10`), which CurAKey `0x5` excludes. The refill
  handler at `0x80000000` therefore sends the miss to the general exception
  path.
- There is no thread context during power-off, so the kernel's fault handler
  dereferences NULL at `0x8003EF4C` (the `LOW reference` warning). The OAL
  then loops at `0x8006B288`.
- The access only succeeds if a TLB entry for `0x0A1FE000` with ASID 2 is
  still resident from before power-off. In the emulator, that entry is
  evicted by TLBWR refills before Stowaway runs. A diagnostic that pins the
  entry lets CE.Net power off and resume normally, so this is the only
  blocker on that path.

**Open questions:**
- Why the entry survives on hardware, if it does. TLBWR picks its slot with
  host `random()` rather than the VR4131 Random register. A
  per-instruction Random model at dyntrans-chunk granularity did not change
  the outcome.
- Whether a real device auto-suspends here at all. AC/battery state isn't
  modeled, so CE.Net may be choosing its battery idle timeout.

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
