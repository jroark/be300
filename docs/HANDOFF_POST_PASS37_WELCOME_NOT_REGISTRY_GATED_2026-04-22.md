# Handoff — Post Pass 37 · welcome.exe is not gated by registry; spawn mechanism remains unknown

**Date:** 2026-04-22
**Predecessor:** `docs/HANDOFF_POST_PASS36_WELCOME_NEVER_SPAWNS_2026-04-22.md` (Pass 36 — welcome.exe never spawns / paint pipeline dormant)

**State of tree (working-copy, uncommitted):**
- `src/be300.h` +5 lines (from Pass 34 — HIBERNATE declarations)
- `src/machine_be300.c` +127 lines (from Pass 34 — 0xAB000000 companion-window stub)

Pass 37 probe additions are **fully reverted** — `git diff src/be300_probe.c` is clean. No functional emulator changes in Pass 37.

## 1. Findings in one sentence

In 200 s of cold boot our emulator sees 10093 `RegOpenKeyExW` calls across 232 unique key paths and 10538 `RegQueryValueExW` calls across 100 unique value names — **none of which match any welcome/launch/runonce/firstboot/oobe/setup/calib/wizard pattern** — so welcome.exe's spawn trigger is NOT a registry lookup that we're currently reaching; and a critical Pass 36 framing error has been corrected (the blob at NK offset `0x2D0FF1` is the ROM TOC module name pool, not the packed registry).

## 2. Pass 36 framing correction

The Pass 36 handoff called NK offset `0x2D0FF1` "the packed ROM registry". **This is wrong.** Direct ROMHDR parsing (this pass) shows:

| Where | What | Size |
|-------|------|------|
| NK file off `0x5F5C54` (VA `0x80655C54`) | ROMHDR | 0x50 bytes |
| NK file off `0x5F5CA4` (VA `0x80655CA4`) | TOC array (95 × 32 B = `0xBE0`) | modules |
| NK file off `0x5F6884` | FILES table (20 × 28 B = `0x230`) | 20 ROM files |
| NK file off `0x5F6AB8` | COPY section (1 × 16 B) | init-data descriptors |

Critical fields from ROMHDR: `dllfirst=0x01870000`, `dlllast=0x02000000`, `physfirst=0x80060000`, `physlast=0x80656AC8`, `nummods=95`, `numfiles=20`.

Welcome.exe is TOC entry 51: name_va=`0x80330FF1` (pointing at `"welcome.exe"\0` at file off `0x2D0FF1`), `nFileSize=0x1600` (5632 B), `e32_va=0x80479C58`, `o32_va=0x80479CC4`, `load_va=0x8022F000`. So the "welcome.exe" string at `0x2D0FF1` is just a TOCentry name-pool entry for a normal XIP module — nothing more.

The actual packed ROM registry is the **`default.fdf`** file in the ROM FILES table:

| FILES idx | Name | attrs | Real size | Compressed size | Load VA |
|-----------|------|-------|-----------|-----------------|---------|
| 0 | `wince.nls` | `0x00000007` | `0x2349E` | `0x2349E` | `0x805C30F8` |
| 1 | `initobj.dat` | `0x00000807` | `0x1640` | `0x51E` | `0x80484584` |
| 2 | **`default.fdf`** | `0x00000807` | `0x13146` | **`0x5D6E`** | `0x805E6598` |
| 3 | `initdb.ini` | `0x00000807` | `0x85F` | `0x34A` | `0x80484AA2` |
| 4 | `usa.lex` | `0x00000007` | `0x20083` | `0x20083` | `0x805EC308` |
| 5 | `tahoma.ttf` | `0x00000007` | `0x19CBC` | `0x19CBC` | `0x8060C38C` |
| 6 | `cour.ttf` | `0x00000007` | `0x2A5AC` | `0x2A5AC` | `0x80626048` |
| 7–16 | `asterisk.wav`, `critical.wav`, `default.wav`, `exclam.wav`, `question.wav`, `asterisk.buz`, `critical.buz`, `default.buz`, `exclam.buz`, `question.buz` | sound files | | | |
| 17 | `Safe.reg` | `0x00000807` | `0x11C6F` | `0x5660` | `0x806505F4` |
| 18 | `tap.buz` | `0x00000807` | `0x6` | `0x6` | `0x80386FFA` |
| 19 | `button.buz` | `0x00000807` | `0x6` | `0x6` | `0x803F5FFA` |

Files with attrs bit `0x800` set are compressed (initobj.dat, default.fdf, initdb.ini, the .wav/.buz, Safe.reg, tap.buz, button.buz). **The packed registry `default.fdf` lives at file off `0x586598` and is `0x5D6E` bytes compressed / `0x13146` bytes uncompressed.** The `tools/nk_lzss.py` in the repo is the B000FF/SPL stream decompressor; it is not obviously the same format as these in-ROM file blobs. The ASCII fragment visible in the compressed `initdb.ini` ("Copyright (c) 1995-2000 Microsoft Corporation…") shows one-byte literal/backref tokens interspersed with text — consistent with WinCE's `CeCompressROM` LZ77 variant, not LZSS. **No tool in the repo currently decompresses this format.**

## 3. welcome.exe import table

Parsed welcome.exe's E32 header + O32 section table + IMAGE_IMPORT_DESCRIPTOR directly from NK (file off `0x419C58` for E32). welcome.exe imports **only from `COREDLL.dll`** — 21 functions, all by ordinal:

| Ordinal | Function | RVA in coredll | Purpose |
|---------|----------|----------------|---------|
| 246 | CreateWindowExW | `0x000145F8` | Create the calibration window |
| 463 | RegQueryValueExW | `0x00032E84` | **Read registry values** |
| 516 | GetLastError | `0x0000BDC8` | Error reporting |
| 461 | RegOpenKeyExW | `0x00032DB8` | **Open registry keys** |
| 455 | RegCloseKey | `0x00032BE4` | Registry cleanup |
| 286 | FindWindowW | `0x00012BDC` | Find an existing window (uniqueness check) |
| 545 | NKDbgPrintfW | `0x0000CD60` | Debug print |
| 861 | GetMessageW | `0x0001448C` | Message loop |
| 870 | TranslateMessage | `0x00014430` | Message translation |
| 859 | DispatchMessageW | `0x00014340` | Message dispatch |
| 95 | RegisterClassW | `0x00032F68` | Register window class |
| 497 | WaitForSingleObject | `0x0000B9F8` | Synchronization |
| 865 | PostMessageW | `0x0001458C` | Post to message queue |
| 866 | PostQuitMessage | `0x0001456C` | Exit the message loop |
| 264 | DefWindowProcW | `0x000123F8` | Default window proc (confirms welcome uses default, augmenting via subclass perhaps) |
| **877** | **TouchCalibrate** | **`0x00011640`** | **The calibration primitive ✓** |
| 876 | KillTimer | `0x00012B9C` | Timer teardown |
| 1097 | swprintf | `0x000425AC` | String formatting |
| 493 | **CreateProcessW** | `0x0000F89C` | **Spawns a follow-up process** |
| 875 | SetTimer | `0x00012BBC` | Timer setup |
| 36 | LocalFree | `0x0001FB08` | Memory cleanup |

**Notable absences:** `RegSetValueExW`, `ShellExecuteEx`, `CreateFileW`, `SetCalibrationData` — so welcome.exe **does not persist calibration results to the registry itself**, does not use Shell APIs, and does not touch the filesystem. It opens/queries registry values, calls `TouchCalibrate`, registers a window class, runs a message loop, and spawns something via `CreateProcessW`. The calibration-persistence mechanism has to sit elsewhere — probably in whatever `TouchCalibrate` calls through to (likely a touch driver IOCTL that updates `Drivers\CASIO\Keybd` or similar).

## 4. Runtime registry / spawn observations (200 s cold boot)

Instrumented `be300_probe.c` (reverted before commit) to log:
- `RegOpenKeyExW` (PC `0x01FB2DB8`) — a1 decoded as UTF-16LE subkey path
- `RegQueryValueExW` (PC `0x01FB2E84`) — a1 decoded as UTF-16LE value name
- `ShellExecuteEx` (PC `0x01FB4B30`) — `a0->lpFile` (offset +0x18) decoded
- User-mode `CreateProcessW` (PC `0x01F8F89C`) — a0 decoded
- Post-return hooks at gwes PC `0x00017D58` (CalibrationData callsite) and `0x00020114` (CalVKey callsite)

### 4.1 Kernel CreateProcessW (via existing `spawn_module_createprocess_path` at PC `0x8008690C`)

15 spawns over 200 s, exactly matching Pass 36:

```
Boot 1: filesys → SystemPatchModule → shell → device → gwes → Boot → modmonitor
         <KjCMU warm reset fires>
Boot 2: filesys → SystemPatchModule → shell → device → gwes → Boot → modmonitor → coshell
```

**welcome.exe is never spawned — confirmed.**

### 4.2 User-mode CreateProcessW (PC `0x01F8F89C`)

4 hits total:
- `"\Windows\SystemPatchModule.exe"` from shell.exe PC `0x0001284C` (×2, once per boot)
- `"modmonitor.exe"` from modmonitor PC `0x018A184C` (×2, self-spawn once per boot)

None of the 4 is welcome.exe.

### 4.3 ShellExecuteExW

**0 hits** over 200 s. Welcome is not reached via ShellExecute either.

### 4.4 Registry key / value audit

- **232 unique `RegOpenKeyExW` subkey paths** across 10093 calls. Dominant patterns: `Drivers\BuiltIn\*`, `Drivers\CASIO\*`, `Drivers\Active\NN`, `Comm\*`, `SYSTEM\GWE\*`, `ControlPanel\*`, `Tcpip\*`, and `SOFTWARE\CASIO\COShell\AppCategory\00` (queried once). Full list saved to `/tmp/pass37_keys.txt` during probing.
- **100 unique `RegQueryValueExW` value names** across 10538 calls. Interesting ones: `CalibrationData`, `CalVKey`, `ExeName`, `ImagePath`, `Dll`, `Entry`, `AutoOOM`, `Helpers`, `BackupFolder`, plus many driver properties.
- **No key / value name matches** `welcome|launch|run|firstboot|first.boot|calib|oobe|setup|home|desktop|startup|autorun|wizard|touch` (regex applied case-insensitive).

So: **nothing in our emulator queries any registry key or value that would plausibly point to welcome.exe**. Either the gating lookup happens but doesn't use a key/value name we'd expect, or the gating happens outside the registry entirely.

### 4.5 CalibrationData anomaly

The most interesting registry event by a wide margin:

```
[BE300_LIFECYCLE_REGVAL] hit=5179 value="CalVKey"          ra=0x00020114
[BE300_LIFECYCLE_PC]     label=gwes_post_calvkey_query     pc=0x00020114 v0=0x00000000
[BE300_LIFECYCLE_REGVAL] hit=5204 value="CalibrationData"  ra=0x00017D58
[BE300_LIFECYCLE_PC]     label=gwes_post_calibrationdata_query pc=0x00017D58 v0=0x00000057
```

- `CalVKey` → returns `v0=0` (ERROR_SUCCESS) — the value exists and is readable.
- `CalibrationData` → returns `v0=0x57` (**ERROR_INVALID_DATA / ERROR_INVALID_PARAMETER** depending on interpretation, both map to decimal 87). The value exists in the registry but is either the wrong type/size or malformed.

Both hits come from gwes.exe (vbase `0x00010000`, ASID 0x04, PC `0x00017D58` is at gwes file offset `0x7D58`). The same two queries repeat in identical sequence on the second boot (hit 10440 + 10465, same PCs, same v0=0x57).

**This is likely a latent registry data issue in our decompressed NK (default.fdf) — the CalibrationData record is present but malformed from our perspective.** On real HW with a battery-removed cold boot, `CalibrationData` should be absent (ERROR_FILE_NOT_FOUND, v0=2), which presumably triggers a different gwes code path that calls `TouchCalibrate` or spawns welcome.

But here's the kicker: **even with v0=0x57 (invalid data) in our emulator, welcome still never spawns.** So welcome.exe is also not triggered by gwes's CalibrationData failure path — at least not by any mechanism that surfaces through CreateProcessW / ShellExecute / another instrumented API.

## 5. Stall chain, updated

```
Correct ROM layout:
  TOC name pool at NK off 0x2D0FF1 (not registry) — contains welcome.exe's filename
  Packed registry at NK off 0x586598, file name default.fdf, compressed (CeCompressROM)

Kernel spawn chain in 200s:
  filesys → SPM → shell → device → gwes → Boot → modmonitor  <KjCMU>  (all again + coshell)

Inside gwes startup:
  RegOpenKeyExW("Drivers\CASIO\Keybd" or similar)          → OK
  RegQueryValueExW("CalVKey")                              → v0=0      (SUCCESS)
  RegQueryValueExW("CalibrationData")                      → v0=0x57   (INVALID_DATA)
  post-query code at gwes PC 0x00017D58...  [UNKNOWN BRANCH]
  (something in here SHOULD trigger calibration in real HW;
   in our emulator, whatever happens does NOT spawn welcome.exe
   and does NOT call TouchCalibrate user-mode)

Result:
  coshell.exe eventually spawns, its desktop HWND's WM_PAINT goes to
  DefWindowProcW stub (Pass 36 §2.2), no user-mode pixel ever lands in
  PA 0x0A200000, OAL's "Starting" splash at PC 0x80F037CC stays visible.
```

**Screenshot SHA for every Pass 37 run: `e8a8c83cd66b9327f50fc1827eada71fb028b332`** (the OAL "Starting" splash). No UI progression.

## 6. Ranked next steps for Pass 38

### Option Q (highest signal) — Decompile gwes.exe around PC `0x00017D58` in Ghidra

The v0=0x57 return from `CalibrationData` is the loudest signal we have. Decompile the gwes function containing PC `0x00017D58` and trace the branches for v0 != 0. Real HW should take a branch that either spawns welcome or invokes the calibration subsystem; our emulator must take a branch that silently skips.

**Approach:**
1. Load `build-host/modules/03_gwes.exe.bin` (732 KB, vbase `0x00010000`) into Ghidra.
2. Jump to `0x00017D58`. Identify the containing function and its caller(s).
3. Also inspect `0x00020114` (CalVKey callsite) — the close proximity of these two PCs suggests they live in the same init routine.
4. Trace post-CalibrationData branches. Specifically look for:
   - A branch on `v0 != 0` that calls `CreateProcessW` with any string (welcome is imported-by-ordinal so the target string may be built at runtime from pieces).
   - A branch that calls a kernel callback (e.g., an OAL `SetCalibrationNeeded` flag).
   - A loop that waits on an event we might not be signaling.

**Effort:** medium. **Signal:** highest.

### Option R — Decode the packed registry (default.fdf) statically

Write a `CeCompressROM` (LZ77 variant) decompressor to extract default.fdf. Once uncompressed, dump the registry tree and confirm whether `CalibrationData` is present-but-malformed, absent, or correctly populated.

**Approach:**
1. Write `tools/decompress_cecompress.py` — research the WinCE 3.0 `CeCompressROM` algorithm (Microsoft has documented it via `compress`/`decompress` APIs in `imagehlp.dll`). The format is typically: flag byte (8 bits, 1 = next-is-literal, 0 = next-is-backref), then either a literal byte or a 2-byte backref `(offset<<4) | (length-3)` with offset in a 4096-byte sliding window.
2. Decompress `default.fdf` (blob at NK off `0x586598..0x586598+0x5D6E`).
3. Write `tools/parse_wince_registry.py` — WinCE 3.0 ROM registry format: key tree with 4-byte-aligned records, variable-length UTF-16LE key names, `REG_SZ`/`REG_DWORD`/`REG_BINARY`/`REG_MULTI_SZ` values.
4. Dump the full registry tree. Grep for welcome, Launch, Calibrate, Wizard, Touch. Inspect `Drivers\CASIO\*` and `SOFTWARE\CASIO\COShell\AppCategory\*` subkeys.

**Effort:** high (two non-trivial Python tools). **Signal:** high — gives us ground truth on what registry the guest sees.

### Option S — Decompile coshell.exe's startup

coshell spawns at hit=15 (after the warm reset) and according to Pass 36 it paints WM_PAINT via DefWindowProcW. But coshell's AppCategory (we see `SOFTWARE\CASIO\COShell\AppCategory\00` queried) might contain a first-boot wizard entry. If `AppCategory\00` or `AppCategory\NN` values include welcome, coshell enumerates it on startup.

**Approach:** Load `build-host/modules/50_coshell.exe.bin` (132 KB) in Ghidra. Find the function that reads `SOFTWARE\CASIO\COShell\AppCategory`. Trace what it spawns.

**Effort:** medium. **Signal:** medium — could identify the Casio-specific shell launcher, but unlikely to spawn welcome (welcome is a Microsoft wizard, not a Casio app).

### Option T — Decompile Boot.exe (original plan Option E)

Still valid. Pass 32 partial decode showed Boot.exe at WinMain UVA `0x00012BD4` checks an init-marker file at UVA `0x00015390`. The branch that doesn't reboot might spawn welcome.

**Effort:** medium. **Signal:** lower than Q — Boot.exe's 6 KB doesn't obviously contain a welcome spawner (only 4 user-mode CreateProcessW calls in 200s, none from Boot.exe).

### Recommended sequence

**Q first** (decompile gwes at PC 0x00017D58). Then **R** if Q doesn't localize. Skip S/T unless Q+R both silent.

### Out of scope / refuted (do NOT revisit)

- VRC4173 LCDC / VBLANK IRQ (Pass 35)
- VirtualCopy failure for primary FB (Pass 35)
- ddi.dll reading stale VRC4173 values (Pass 35)
- AIU audio driver (Pass 33)
- gwes_worker WFSO(0x000B6834) block (Pass 34 — real HW same behavior)
- "NK off `0x2D0FF1` is the packed registry" (Pass 36 — corrected this pass)
- Boot.exe as the welcome spawner (very low prior after 200 s of probing — but kept as Option T for completeness)
- Any RAM seed / `resume_ctx` / guest-binary patch shortcut (CLAUDE.md §Emulation Philosophy)

## 7. Supersedes

- Supersedes `HANDOFF_POST_PASS36_*_2026-04-22.md §1` (the "packed ROM registry" framing). The gating mechanism is not "which registry key points at welcome.exe" — it's whatever the gwes code at PC `0x00017D58` does with `v0=0x57` from the CalibrationData query, OR a separate mechanism we haven't hit yet.
- Supersedes Pass 36 §4 Option A (decode the packed ROM registry around welcome.exe). Welcome.exe's only appearance in NK is as a TOC name; the packed registry is a different blob (`default.fdf`) and does not reference welcome by name in any form we can find.
- Updates `project_pass36_welcome_and_touchcal` memory entry — welcome.exe **is imported-link-target-absent** from every .bin hard-coded pool, in both ASCII and UTF-16, so the spawn cannot be by hard-coded image name. The spawn mechanism sits in compiled code that builds the name at runtime, or in a kernel/driver side that references it by TOC index.

## 8. Pending commit decisions

Pass 37 produced no functional emulator change — probe additions in `src/be300_probe.c` are fully reverted. The only files to commit are:

1. **This handoff doc** — yes, commit.
2. **Pass 34's two uncommitted changes** (HIBERNATE in gxemul submodule + `0xAB000000` companion-window stub in `src/machine_be300.c`) — still pending. These have hardware citations (VR4131 UM §6.1.3; `hardware.txt:204`) and move the project forward. Pass 36 handoff §8 already recommended committing them independently. The recommendation stands. Commit Pass 34's changes in a separate commit from the handoff.

## 9. Key facts cached for Pass 38 reuse

### ROMHDR at VA `0x80655C54` (file off `0x5F5C54`)
```
dllfirst     = 0x01870000     ulRAMStart     = 0x80660000
dlllast      = 0x02000000     ulRAMFree      = 0x8066D000
physfirst    = 0x80060000     ulRAMEnd       = 0x81000000
physlast     = 0x80656AC8     ulCopyEntries  = 1
nummods      = 95             ulCopyOffset   = 0x80656AB8
numfiles     = 20             usCPUType      = 0x0166 (MIPS R4000)
```

### TOC entry layout (32 bytes each), starting VA `0x80655CA4`
```
+0x00 dwAttrs          +0x10 nFileSize
+0x04 ??? (4 bytes)    +0x14 lpszName (VA)
+0x08 FILETIME         +0x18 ulE32Offset (VA)
                       +0x1C ulO32Offset (VA)
```

### welcome.exe TOC entry 51 at file off `0x5F6304`
```
attrs=0x00000007, name_va=0x80330FF1 ("welcome.exe"),
nFileSize=0x1600, e32_va=0x80479C58, o32_va=0x80479CC4
vbase=0x00010000, vsize=0x5000, 4 O32 sections.
```

### gwes.exe post-CalibrationData site
```
gwes.exe vbase 0x00010000, vsize ~0xB7000 (732 KB).
RegQueryValueExW("CalibrationData") returns v0=0x57 (ERROR_INVALID_DATA).
Return site: PC 0x00017D58 inside gwes — investigate in Ghidra.
CalVKey query at close-by PC 0x00020114 returns v0=0 (success).
ASID 0x04 during this phase.
```

### Registry API VAs (reuse from Pass 36 §6, verified in Pass 37 probe)
```
RegOpenKeyExW     VA=0x01FB2DB8  (coredll ord 461)
RegQueryValueExW  VA=0x01FB2E84  (coredll ord 463)
RegCloseKey       VA=0x01FB2BE4  (coredll ord 455)
RegCreateKeyExW   VA=0x01FB2C70
ShellExecuteEx    VA=0x01FB4B30  (0 hits in 200s cold boot)
CreateProcessW    VA=0x01F8F89C  (4 user-mode hits, all shell/modmonitor)
TouchCalibrate    VA=0x01F91640  (0 hits — calibration never invoked)
```

### Packed registry `default.fdf`
```
ROM FILES index 2, compressed with CeCompressROM (LZ77-variant).
File offset in nk_decompressed.bin: 0x586598
Compressed size: 0x5D6E
Uncompressed size: 0x13146
Load VA on guest: 0x805E6598
No decompressor in repo. tools/nk_lzss.py handles a different (B000FF/SPL)
stream and likely won't work here without adaptation.
```
