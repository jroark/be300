# HANDOFF POST PASS 41 — ALL user-mode spawn candidates ruled out; welcome.exe path exists only in default.fdf

**Date**: 2026-04-23 (late evening, Pass 38-41 series)
**Branch**: `investigate/pass38-gwes`
**Stall state unchanged**: "Starting" OAL splash, 99.67% pixel match.

## §1 Why this pass existed

Pass 40 left 4 suspect modules unexamined: `filesys.exe`, `shell.exe`, `device.exe`, `modmonitor.exe`. Pass 41 scanned each for welcome.exe references and, when those came up empty, widened the scan to NK kernel + all 95 XIP modules to definitively pin the welcome-spawn source.

## §2 Zero welcome.exe string literals across the entire ROM

### Scan matrix

| Target | `welcome` UTF-16 | `welcome.exe` UTF-16 | `Calib` UTF-16 | Other signals |
|---|---|---|---|---|
| 02_filesys.exe | 0 | 0 | 0 | clean |
| 04_device.exe  | 0 | 0 | 0 | clean |
| 06_shell.exe   | 1 (debug banner "Welcome to the Windows CE Shell.") | 0 | 0 | — |
| 50_coshell.exe | 0 | 0 | 0 | clean |
| 87_modmonitor.exe | 0 | 0 | 0 | clean |
| 03_gwes.exe    | 0 | 0 | 1 | imports `TouchCalibrate` ordinal |
| 51_welcome.exe | `WELCOME_EXE` (self-identity) | 0 | 0 | imports `TouchCalibrate` |
| ... (88 other modules) | 0 | 0 | 0 (scan coverage = 100%) | clean |
| NK kernel | 1 ("Welcome" @ 0x80212368 = shell debug banner), 1 ("WELCOME" @ 0x804e3000 = welcome.exe's own module-name) | 0 | 2 (@ 0x8014c0ea / 0x8014c23c = **touch.dll exports** `TouchCalibrate`, `TouchPanelCalibrateAPoint`, `TouchPanelSetCalibration`) | ASCII `welcome` @ 0x80330ff1 = TOC name pool |

**Result: there is NOT a single hardcoded `"welcome.exe"` or `"\\Windows\\welcome.exe"` string anywhere in the extracted ROM.** The only references to the word `welcome` are the shell debug banner, welcome.exe's own module-name self-identity (`WELCOME_EXE`), and the TOC name-pool lookup entry.

### TOC entry 51 for welcome.exe

Confirmed via struct unpack of NK TOC at offset `0x5F5CA4`, entry 51:

```
dwFileAttributes / loadVA = 0x8020D000
reserved / ???           = 0x00000007
FILETIME                  = 0x01C0D78F E219A300
nFileSize                 = 0x1600 (5504 bytes)
lpszFileName              = 0x80330FF1 ("welcome.exe")
ulE32Offset               = 0x80479C58
ulO32Offset               = 0x80479CC4
```

No obvious "AUTORUN" flag in the TOC entry. All .exe entries follow the same layout with distinct load addresses.

## §3 Consequence: welcome.exe's spawn path MUST come from the compressed registry `default.fdf`

Because:
1. Every spawn mechanism except kernel-side `FUN_8008690c` (the CreateProcessW thunk) has been eliminated.
2. `FUN_8008690c` is called 7 times in 60 s in our emulator; 15 times across 200 s warm-reset runs. None launch welcome.
3. The `char* image` argument to `CreateProcessW` must come from SOMEWHERE. If not from a hardcoded string in any extracted module (verified 0 hits), then it must come from a **runtime heap string loaded from the registry**.
4. The partial CeCompressROM decode of default.fdf (Pass 40) showed `"Launch"` × 4 substrings in page 19, confirming HKLM\init has at least 4 `Launch<N>` entries. Those entries contain the process filenames that the kernel launcher reads.

Pass 32's observed 5-entry launcher table (user VA `0x0203B4D0`, `0x250` stride) + the CreateProcessW census (filesys, device, gwes, shell, Boot = 5) accounts for exactly 5 HKLM\init Launch entries. Welcome.exe is NOT one of the 5 observed — either it's not in HKLM\init at all, or it IS listed but the kernel launcher skips it due to a missing dependency or failed condition check on our emulator.

## §4 The final surviving hypotheses for Pass 42

1. **welcome.exe IS a `Launch<N>` entry** in default.fdf but gated by a `Depend<N>` dependency blob that points at something our emulator doesn't complete. `FUN_800808c4` decomp showed the dispatch loop yields on `WaitForMultipleObjects(_DAT_8066AF00)` when dependencies aren't met; a stuck dependency would silently park welcome forever without a visible spawn attempt.

2. **welcome.exe is launched by a `HKLM\Services\...` auto-start entry** (a second spawn mechanism beyond HKLM\init). WinCE services are auto-started by `services.exe` which we DON'T see in the spawn list — so either services.exe is disabled, or it's cascaded from device.exe. Needs investigation.

3. **welcome.exe is launched by a `HKLM\Drivers\BuiltIn\xxx` IOCTL callback** — the device manager (device.exe) enumerates built-in drivers and calls their init routines. A driver could `CreateProcessW("welcome.exe")` in its init path. This would be a hidden-to-string-scan mechanism because the driver DLL would do a registry-read-then-CreateProcess sequence where the string comes from the registry value.

4. **welcome.exe is conditional on a hardware register** — e.g., battery-backed SRAM ("NV-RAM") state indicating "first boot". Our emulator may present the wrong NV-RAM state on cold boot. BE-300 has an RTC-backed CMOS region at `0x0B000000` / VRC4173 strap area. Needs emulator inspection.

## §5 Recommended Pass 42 priorities (ranked)

1. **P1 (highest definitive value)** — crack CeCompressROM. The partial 12+4 mm=2 variant produces page 19 at 324/326 bytes (2 bytes short). One or two more iterations of systematic variant-sweep OR a direct port from Microsoft's compchain.c (Windows Embedded Compact source leak) should nail it. Once HKLM\init is dumpable, hypothesis #1 is instantly verifiable.

2. **P2** — decompile `FUN_800808c4`'s `launcher_dependencies_satisfied_for_init_entry` function to see exactly what "dependency satisfied" means. If it's a `SetEvent(_DAT_8066AF00)` that gwes.exe or coshell.exe is supposed to signal after first-boot completion, we can probe that event state and see if welcome is STARVED waiting for it.

3. **P3** — scan NK and all modules for `HKLM\\Services` UTF-16LE — map out what services are registered and which (if any) could spawn welcome.

4. **P4** — in `be300_probe.c`, instrument every `FUN_8008690c` callsite (already probed) + CREATE_NEW_PROCESS / INTERNAL_SPAWN variants, AND instrument `_DAT_8066AF00` dependency event writes. Produces a ground-truth "who tried to spawn what when" timeline.

5. **P5** — check the BE-300's VRC4173 strap registers + RTC CMOS region for a "first boot" bit. Real hardware may read an NV-RAM flag that's 1 on first boot, 0 thereafter; the emulator might present it as 0 always (or 1 always), masking the path that normally triggers welcome.

## §6 Deliverables

- This handoff doc.
- No probe code changes this pass — pure static analysis.
- Memory updates: `project_pass41_all_candidates_ruled_out.md`, refresh `MEMORY.md` index.
- Branch `investigate/pass38-gwes` only; main untouched.

## §7 Cumulative candidate-elimination table (Passes 37-41)

| Candidate | How eliminated | Pass |
|---|---|---|
| gwes `FUN_00017C40` (Pass 37's hypothesis) | Decomp — silent RegCloseKey+free on fail | 38 |
| SPL rect-fill at PC 0x80F037CC as "callback" | Decomp — it's the SPL boot-decision `main`, one-shot | 38 |
| `HKLM\init\Launch<N>` kernel enumerator | Runtime — 5 entries observed, none welcome | 39 |
| Boot.exe WinMain | Decomp — init-marker + reboot; no welcome string | 40 |
| coshell.exe | Full string scan — desktop/taskbar only | 40 |
| filesys.exe, device.exe, shell.exe, modmonitor.exe | String scan of all 4 | **41** |
| All 88 other XIP modules | Exhaustive UTF-16LE + ASCII scan | **41** |
| NK kernel (for a hardcoded welcome path) | Exhaustive scan | **41** |

The investigation has now EXHAUSTED the "static code scan" approach. Pass 42+ must either crack the registry or instrument dependency/event state.
