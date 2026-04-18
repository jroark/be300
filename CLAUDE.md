# CLAUDE.md — Working Rules for This Repository

## Emulation Philosophy

The goal of this project is **accurate cold boot of Windows CE 3.0** (`ce/restore_images/All_nand_300.bin`) on the Casio BE-300. The emulator must boot from the ROM reset vector through SPL, NK.exe decompression, and into WinCE 3.0 exactly as real hardware does after battery removal.

The Linux kernel boot path (`--kernel`), its ELF loader, and the `kernels/` tree have been removed. Non-3.0 WinCE images remain in the tree for reference only but are not tested or supported.

**Always prefer hardware-accurate emulation over workarounds.** Do not use seeds, patches, memory pre-population, guest binary modifications, or forced handoff shortcuts to work around emulation bugs. If a guest OS behavior fails, the root cause is a missing or incorrect hardware behavior in the emulator. Find and fix the emulator bug rather than patching the guest. The ROM and NK.exe binaries are captured from real hardware and must run unmodified.

Before adding any behavior that looks like a workaround (register value synthesis, TLB overrides, forced interrupt assertions, memory pre-population, etc.), cite the hardware source that *either* justifies it as real silicon behavior (VR4131 UM §X, `docs/hardware/hw_dump_*.txt` line Y, ROM disassembly at PC Z) *or* acknowledges it as a temporary patch with a dated TODO for root-cause investigation. Commits that add workarounds without citations will be reverted.

Do not reintroduce `resume_ctx` seeding, synthetic warm-boot state, or any other fake cold-start shortcut.

## Current Scope And Non-Goals

- Primary active target: WinCE 3.0 cold boot via `--nand ce/restore_images/All_nand_300.bin`
- Secondary code paths still present in the tree: CF recovery boot via `--restore --cf`, PPSH debug-shell probing via `--ppsh`
- Non-3.0 WinCE images remain in the tree for reference only

## Reference & Documentation

Available external reference environments:
- real BE-300 hardware
- eMbedded Visual C++ 3.0 VM with the BE-300 SDK
- Platform Builder 3.0 VM

Primary local references:
- `docs/hardware/Vr4131-um_200203.pdf` - NEC VR4131 SoC users manual
- `docs/hardware/U14579EJ2V0UM00.pdf` - NEC VRC4173 companion chip users manual
- `docs/hardware/hardware.txt` - notes from Linux4be project developers
- `docs/hardware/hw_dump_vr4131.txt` - VR4131 register dumps from real hardware
- `docs/hardware/hw_dump_vrc4173.txt` - VRC4173 register dumps from real hardware
- `docs/hardware/hw_dump_memory.txt` - SDRAM, exception vectors, boot params, and ROM dumps from real hardware
- `docs/hardware/hw_dump_tlb.txt` - TLB state snapshots from real hardware
- `docs/hardware/hw_dump_diffs.txt` - memory diffs, focus probes, and region summaries from real hardware
- `docs/hardware/BE300BootROM_v1.txt` - full 16 KB ROM dump at PA `0x1FC00000` with CRC32 `0xFA3B5582`
- `docs/hardware/be300_boot_rom.bin` - extracted ROM binary embedded into the emulator at build time
- `docs/ROM_SPL_HANDOFF.md` - ROM to SPL to NK handoff analysis
- `docs/HARDWARE_GROUND_TRUTH.md` - synthesized hardware reference notes
- `ce/bediag/` - BEDiag diagnostic tool source and output

## Source Code Layout

**Core emulation (`src/`)**
- `machine_be300.c` — machine setup, boot-mode selection, device registration, ROM and NAND image loading, main emulation loop, NK dumping, runtime shutdown
- `be300.h` — `be300_state_t`, `machine_config_t`, physical address map constants
- `ui.c` / `ui.h` — SDL2 display, input handling, screenshot capture
- `main.c` — CLI parsing for `--nand`, `--restore`, `--cf`, `--ppsh`, and other boot options
- `host_io.c` / `host_io.h` — host stdin/stdout handling
- `ppsh.h` — PPSH transport helpers
- `be300_devices.c` — VRC4173 latch, CF window, PPSH debug shell, WinCE aux device

**GXemul CPU engine (`gxemul/`)**
- `gxemul/src/cpus/` — VR4131 MIPS CPU execution, CP0, TLB, dyntrans, MIPS16 support
- `gxemul/src/core/` — memory, interrupts, timers, settings, console framework
- `gxemul/src/devices/` — `dev_vr41xx` (VR4131 ICU and timer), `dev_ns16550` (UART), `dev_fb`, `dev_ram`
- `gxemul/src/machines/machine_hpcmips.c` — BE-300 machine definition, framebuffer, SIU, bootinfo
- `gxemul/src/stubs.c` — minimal stubs for unused GXemul subsystems
- `gxemul/config.h` — MIPS-only build configuration

**Hardware peripherals (`src/hw/`)**
- `bcu.c/h` — Bus Control Unit
- `icu.c/h` — Interrupt Control Unit
- `rtc.c/h` — Real-Time Clock
- `siu.c/h` — Serial Interface Unit
- `nand.c/h` — NAND flash controller and NANDWRITER restore-engine support
- `cf.c/h` — CompactFlash image, ATA taskfile, CIS, and recovery-boot support
- `gpio.c/h` — GPIO
- `cmu.c/h` — Clock Management Unit
- `pmu.c/h` — Power Management Unit

## Build, Tooling, And Logs

### Host Build And Primary Test

```bash
# rebuild and cold-boot WinCE 3.0
mkdir -p build-host && cd build-host
cmake ..
make -j$(nproc)
gtimeout 60s ./be300 --nand ../ce/restore_images/All_nand_300.bin \
  > cold_stdout.log 2> cold_stderr.log
```

`gtimeout` exit `124` is expected for long-running boots. NK does not terminate on its own.

### Docker Reverse-Engineering Environment

- MIPS cross tooling lives in the Docker container `mips-dev`
- The local repository is mounted at `/work` inside the container
- Commit and push from the host only, not from inside Docker

Typical container entry:

```bash
docker compose build mips-dev
docker compose run --rm mips-dev /bin/bash
```

Useful toolchains in the container:
- `mipsel-linux-gnu-*` for MIPS ELF analysis
- `mipsel-pe-*` for Windows CE PE analysis

### Logs And Artifacts

- Always capture both stdout and stderr from emulator runs
- Serial output goes to stdout; emulator diagnostics go to stderr
- The emulator opens an SDL window on macOS regardless of `DISPLAY`
- Screenshots are saved during orderly runtime shutdown, including handled quit paths and handled stop signals such as `SIGTERM`
- Do not assume a screenshot exists after a crash or hard kill
- Check stderr for the saved screenshot path, for example:
  `[UI] Screenshot saved: screenshot_20260412_142904.bmp`

## Project Context

**Target:** Casio BE-300 using the NEC VR4131 MIPS little-endian SoC and VRC4173 companion chip.

**CPU engine:** GXemul 0.7.0 from the `jroark/GXemul` `be300` branch. Fresh clones need:

```bash
git submodule update --init
```

**WinCE restore images**
- Restore images are raw sector data in logical block order: `1004 blocks * 32 pages * 512 bytes = 16,449,536 bytes`
- `NANDWRITER.bin` writes them to NAND as-is with identity block mapping
- `ce/restore_images/RESTORE_IMAGES.md` documents the image set
- `ce/restore_images/All_nand_300.bin` is the sole active cold-boot target
- Other images such as `All_nand_Net.bin`, `org_CE_30.bin`, `BE500.bin`, and `CE_Net.bin` remain in the tree for reference only

**Restore image partition table** (block 0, 16-byte entries: `8 bytes 0xFF + start_sector(4) + size(4)`)
- Entry 0: sectors `0-31` -> boot metadata
- Entry 1: sectors `32-159` -> SPL / Kloader
- Entry 2: sectors `160-7583` -> compressed NK
- Entry 3: sectors `7584-32127` -> FAT16 filesystem
- The NK partition at offset `0x14000` includes a real leading `0xFF` byte before the `B000FF` signature

**Flash tools** (`ce/restore_images/`)
- `NANDWRITER.bin` — CF-to-NAND recovery tool, WinCE B000FF app, version `0.67`, base `0x80E00000`
- `KLOADER.bin` — standalone SPL used by `NANDWRITER.bin`, version `0.62`
- `DevOSInstall.exe` — on-device OS installer
- `Setup.exe` — host-side ActiveSync upgrade coordinator
- `DevRestore.exe` / `DevBackup.exe` — user-data backup and restore
- `BootInSafeModeWithPCC.exe` — safe-mode boot utility

Recovery boot through `--restore --cf` is still implemented for NANDWRITER-path work, but it is not the primary regression target.

## WinCE Boot Facts

### Boot ROM

- 16 KB masked ROM at PA `0x1FC00000` (VA `0xBFC00000` in kseg1, `0x9FC00000` in kseg0)
- Dumped from real hardware: `docs/hardware/be300_boot_rom.bin`
- Reset vector: NOP -> LUI/ORI/JR to `0xBFC002F0`
- ROM initializes CP0, SDRAM timing, clocks, NAND, and SPL loading
- Original ROM has no real BEV TLB refill handler at `+0x200`
- The original bytes at the BEV general-exception vector overlap boot code continuation rather than a general exception handler
- The emulator patches the ROM at load time with MIPS32 BEV handlers so exceptions during early boot are routed correctly
- The ROM contains about 5.5 KB of MIPS16 code from `0x0C20` to `0x219B`, and GXemul executes it natively
- NK.exe is fully MIPS32

**ROM layout**
```text
0x0000-0x00FF: Reset vector and exception stubs
0x0100-0x0C1F: Initialization and setup code
0x0C20-0x219B: MIPS16 function library
0x219C-0x224F: Function metadata and address table
0x2250-0x3FFF: Unused padding
```

**Important MIPS32 ROM helpers called from MIPS16**
- `0x9FC00464` — mailbox writer for `0x24FC` and `0x2400`
- `0x9FC00834` — memcpy-like helper
- `0x9FC00888` — memset-like helper
- `0x9FC00980` — context save helper
- `0x9FC009BC` — context helper
- `0x9FC00BC0` — additional helper
- `0x9FC00C04` — trampoline back into MIPS32

### Cold-Boot Sequence

Cold boot through `--nand` starts at the ROM reset vector `0xBFC00000`, matching real hardware after battery removal. The ROM reads NAND, loads the SPL, the SPL decompresses NK into RAM, then the ROM MIPS16 boot dispatcher populates callback tables and jumps into NK via the mailbox at PA `0x24FC`.

**Observed real-hardware framebuffer sequence**
1. `Initializing...` with progress bar
2. `Starting...`
3. touch calibration
4. WinCE desktop

Reference screenshots of the splash stages are committed at the repo root as `Initializing.bmp`, `Starting.bmp`, and `All_nand_300.png`. Use them for visual regression when comparing the emulator's saved screenshot (per `[UI] Screenshot saved:` in stderr) against expected real-hardware output.

### Splash Screen Notes

- `Initializing...` and `Starting...` are rendered at runtime, not stored as ASCII, UTF-16, or bitmaps in the image
- OAL display function `0x80078E10` acts as a blit dispatcher
- `a0 = 10` clears the framebuffer at `0xAA200000`
- `a0 = 0` blits the splash buffer at VA `0x80061188`
- `a0 = 6` blits the `240x160` progress buffer at VA `0x80061CD0`
- The displayed text is drawn by WinCE graphics code into buffers that are zero-filled in the static image

### SPL B000FF Details

- Signature: `B000FF\n` at NAND offset `0x4000`
- Format: 7-byte signature, image start, image length, then repeated `addr + len + cksum + data`
- Load address: VA `0x80F00000` = PA `0x00F00000`
- Entry point jumps from `0x80F00000` to `0x80F02404`
- SPL writes CP0 `Config` and `TagLo`, switches to kseg1, and performs hardware init
- SPL does not install its own exception vectors

### ROM To NK Mailbox Facts

- PA `0x24FC` is the next-stage jump target mailbox
- PA `0x2400` is a state/version marker mailbox
- The ROM-side helper at `0x9FC00464` writes the jump target to `0x24FC` and `0x03020101` to `0x2400`
- NK later expects `0x03020100` at `0x2400` during its version checks

### NK Boot Notes

- NK entry starts at VA `0x80076B50`
- The cold-start kernel entry of interest is `0x8007B398`
- Kernel main init of interest is `0x800947C8`
- ROMHDR `pTOC` is `0x80655C54`
- Key low SDRAM structures:
  - `0x2200-0x22FF` — `resume_ctx`
  - `0x2400` — version marker
  - `0x2524` — hibernate signature
  - `0x254C` — hibernate flags

Historical reverse-engineering found that NK reaches warm-resume-style logic during cold-boot analysis. Treat that as analysis context only, not implementation guidance. Current project policy forbids solving cold boot by pre-populating `resume_ctx` or any other synthetic state.

### VRC4173 And NAND Facts

- VRC4173 `SYSINT1REG` at offset `0x060` is read-only on real hardware
- Interrupt status registers in the VRC4173 status ranges use write-1-to-clear semantics in the emulator
- `dev_vr41xx.c` must not force-enable ETIMER in `MSYSINT1`
- NAND controller buffer registers `0xA4A0-0xA4AC` and `STATUS2` at `0xA4C0` only return meaningful data when a stream transfer is active
- The ROM-era NAND ECC path expects zero syndromes when the emulated NAND data is bit-perfect

### PPSH

- PPSH is a WinCE debug interface, not a companion MCU
- Data register: PA `0x0C000120`
- Status / command register: PA `0x0C000520`
- Emulated in `src/be300_devices.c` as `be300_wince_aux`
- Default probe result is absent -> normal GUI boot
- `--ppsh` returns the expected status bits and routes WinCE to the debug shell instead of the normal GUI path

### ROM NAND Boot Helpers

- `FUN_9fc015f4` — multi-page reader
- `FUN_9fc01710` — logical-block translation and OOB voting
- `FUN_9fc019fc` — block reader
- `FUN_9fc01a4c` — single-page reader and ECC path
- `FUN_9fc01828` — software ECC correction
- `FUN_9fc016b4` — partition descriptor reader
- `FUN_9fc015dc` — partition entry 1 reader used for SPL discovery

### NK Analysis Tools

- The emulator dumps decompressed NK to `nk_decompressed.bin` in the working directory during WinCE NAND boots once PA `0x24FC` points into NK
- `docs/nk_decompressed.bin` is a local ignored convenience copy of the verified WinCE 3.0 NK dump
- `build-host/nk_decompressed.bin` reflects the most recent verified NAND boot
- `tools/scan_nk_producers.py` scans for stores to interesting VAs
- `tools/disasm_nk_ctx.py` disassembles selected NK regions
- In Docker:

```bash
mipsel-linux-gnu-objdump -D -b binary -m mips:3000 -EL nk_decompressed.bin
```

## Current Investigation Guidance

- Primary active target: WinCE 3.0 cold boot via `--nand ce/restore_images/All_nand_300.bin`
- Secondary implemented paths: `--restore --cf` and `--ppsh`
- Do not solve cold boot with guest patches, RAM seeds, `resume_ctx` seeds, or forced handoff shortcuts
- For legitimate upstream GXemul 0.7.0 bugs that are *not* on the current boot path but may become relevant for future investigations, see:
  - `docs/LEGITIMATE_FIXES_NOT_APPLIED.md` — catalog of real bugs left unpatched, each with citation, warning signs that would justify re-applying, and a "do not reconsider" list of genuinely non-legitimate drops

## Key Files For Current WinCE Work

- `src/main.c` — CLI parsing for `--nand`, `--restore`, `--cf`, `--ppsh`, and related boot options
- `src/be300.h` — NAND, CF, and machine configuration state
- `src/machine_be300.c` — boot-mode setup, ROM and NAND boot path, image loaders, NK dump logic, runtime finalization, screenshot-on-shutdown behavior
- `src/hw/nand.c` — NAND controller and NANDWRITER restore-engine behavior
- `src/hw/cf.c` — CompactFlash emulation and recovery-boot support
- `src/hw/rtc.c` — RTC elapsed time, RTCL1 timer, and interrupt-clear behavior
- `src/hw/bcu.c` — BCU register behavior and noisy-probe suppression
- `src/be300_devices.c` — VRC4173 latch, CF window, PPSH device, WinCE aux device
- `gxemul/src/devices/dev_vr41xx.c` — VR4131 ICU and timer behavior
- `gxemul/src/devices/dev_ns16550.c` — SIU UART emulation

## Commit, Branch, And Push Rules

Commit when you have a **functional change backed by evidence** that moves the project forward: a real fix, a platform integration, or a clean refactor. Do **not** commit:
- Investigation probes, PC rings, watchpoints, one-time debug prints, or any `fprintf(stderr, …)` added for a specific debugging session
- Wrong-semantics stepping stones. If your first attempt had an incorrect model (e.g., wrong exception EPC semantics), revert it before trying again — don't leave superseded commits on `main`
- Dump-to-file debug output
- Files mixing diagnostic code with functional fixes. If you find yourself writing both in the same edit, split them: commit only the functional change, discard the diagnostics
- Unrelated files or build artifacts

Each commit message should still cover: what was tried, outcome, what was learned, next step — but only when there *is* something worth committing.

Use double quotes for `git commit -m "..."`. For multi-line messages, prefer a heredoc or a `/tmp/msg.txt` file and `git commit -F /tmp/msg.txt`.

Stay on the current branch. Do not create PRs.

Push with:

```bash
git push -u origin <current branch>
```

## Instrumentation Hygiene

When you need temporary diagnostics:

- Add them to a local working copy only. Revert before committing.
- If an investigation genuinely needs more than one session of probe code, create a dedicated `investigate/<topic>` branch and keep it local. Do not push. When you find the fix, extract *only* the functional change onto `main` and discard the investigation branch.
- Never add `wince_boot_*` / `idle_diag_*` style hook call sites to GXemul core files (`gxemul/src/cpus/`, `gxemul/src/core/`, `gxemul/src/devices/`). That pattern — host-side diagnostic code reaching into the CPU/memory/device layers via hook includes — is how `wince_boot.c` grew to 13 KLOC before it was deleted. If a behavior needs observation, use the existing `--trace` flag, GXemul's built-in single-step disassembly, or a throwaway `fprintf` in exactly one call site that you revert before committing.
- `docs/LEGITIMATE_FIXES_NOT_APPLIED.md` lists upstream GXemul 0.7.0 bugs that are not on the current boot path. Consult it before adding a workaround for a symptom that matches one of its "warning signs."

## GXemul Submodule Hygiene

The `gxemul/` submodule points at the `be300-minimal` branch of `jroark/GXemul`. Keep it thin.

- Current layout: pristine GXemul 0.7.0 (`c35c056`) + a squash adding MIPS16 interpreter and BE-300 platform glue (`3c3d5cb`) + a full VR4131 RTC/ETIMER/ECMP implementation (`53c5910`) + MIPS AdEL on misaligned jr/jalr (`8b8449d`) + VR41xx COP0 SUSPEND → wait-for-interrupt (`3250bb8`, VR4131 UM §4.3.3) + VR41xx RTCL1 timer SYSINT1-ack + edge-pulse IRQ (`8775b00`, VR4131 UM §11.2.1 / §13.2.3) + debug()/fatal()/debugmsg() routed to stderr instead of stdout so guest serial and emulator diagnostics don't interleave (`100bd72`, convention-alignment, not a hardware-accuracy fix). Six delta commits on top of upstream. Do not accumulate more without first documenting the justification.
- Before adding a gxemul-side fix, **diff against 0.7.0**: `git -C gxemul show c35c056:<path>` versus the current file. Confirm the change actually improves on upstream behavior. Several historical "fixes" on the prior `be300` branch were identity transformations of existing 0.7.0 code.
- Before adding a gxemul-side fix, **cite the VR4131 UM section or `docs/hardware/hw_dump_*.txt` line** that justifies it. If you can't, the fix might be a workaround for a different emulator bug — keep root-causing.
- One functional change per gxemul commit. Do not bundle platform integration, behavioral fixes, and diagnostics together (the prior `be300` branch had a commit that bundled a MIPS16 decode fix with BCU latch machinery and diagnostic fprintfs — it was unsplittable for cherry-pick later).
- If a commit resolves a warning sign listed in `docs/LEGITIMATE_FIXES_NOT_APPLIED.md`, update that file in the same or adjacent commit.
