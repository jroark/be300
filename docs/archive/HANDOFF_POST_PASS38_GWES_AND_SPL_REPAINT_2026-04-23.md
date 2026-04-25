# HANDOFF POST PASS 38 — gwes `0x00017D58` not a welcome gate; SPL repaint is benign splash drawer

**Date**: 2026-04-23
**Branch**: `investigate/pass38-gwes` (probe WIP + this handoff; nothing merged to `main`)
**Image**: `ce/restore_images/All_nand_300.bin` via `--nand`
**Stall state unchanged**: screenshot `screenshot_20260423_191512.bmp` is 99.67% pixel-identical (229,653 / 230,400 bytes) to `Starting.bmp` — still at OAL "Starting" splash. `Starting.bmp` SHA-1 `e8a8c83cd66b9327f50fc1827eada71fb028b332`.

## §1 Committed-state ledger

- **Last `main` commit**: `733da373` (Pass 31 addendum 7, 2026-04-20).
- **This branch's WIP commit**: `b24a6b86` — Pass 34–38 probe infrastructure (`src/be300_probe.{c,h}` + wiring). Scope-capped to ~15 new exec watches for this pass.
- **Pass 38-specific probe additions** (all gated on `BE300_LIFECYCLE_PROBE`):
  - `gwes_apply_saved_calib_entry`         @ `0x00017C40`
  - `gwes_apply_saved_calib_query_ret`     @ `0x00017D58` (the Pass 37 PC)
  - `gwes_apply_saved_calib_ok`            @ `0x00017D5C`
  - `gwes_apply_saved_calib_loop`          @ `0x00017D7C`
  - `gwes_apply_saved_calib_fail`          @ `0x00017E40`
  - `gwes_apply_saved_calib_cleanup`       @ `0x00017E70`

## §2 Thread A — gwes `0x00017D58` is NOT the welcome gate (hypothesis refuted)

### What Pass 37 claimed

> "gwes queries CalibrationData at PC `0x00017D58` → v0=0x57 INVALID_DATA; welcome still never spawns. Pass 38 priority = decompile gwes at `0x00017D58`."

The implication was that the INVALID_DATA branch should — on real hardware — eventually fire CreateProcessW("welcome.exe"), and our emulator is missing some state that normally causes that branch to launch the wizard.

### What Pass 38 static decomp found

Extracted `build-host/xip/03_gwes.exe.bin` (vbase `0x00010000`, .text `0x00011000..0x000B24EA`) via `tools/extract_xip_modules.py` and disassembled with Capstone MIPS32 LE.

**PC `0x00017D58` is inside function `FUN_00017C40`**, whose full semantics are:

```
0x00017C40  prologue: push ra/s0..s5, reserve 0x98 bytes of locals
0x00017C64  jal 0x00017BD4        ; helper: GetCalibrationPointCount(&count_out)
0x00017C6C  if ($v0 == 0) goto FINAL_RET            ; helper failed → bail
0x00017C78  jal 0xb1014           ; LocalAlloc(count*16)   ; alloc #1
0x00017C80  if ($v0 == 0) goto FINAL_RET
0x00017CC0  jal 0xb1014           ; LocalAlloc(...)        ; alloc #2
0x00017CC8  if ($v0 == 0) goto CLEANUP_FAIL
0x00017CD0  lw  $t9, 0xB3050($at) ; = RegOpenKeyExW fptr
0x00017CE0  a0=HKEY_LOCAL_MACHINE, a1=[0x00011338] (subkey name)
0x00017D08  jalr $t9              ; RegOpenKeyExW(HKLM, subkey, 0, 0, &hKey, &dispo)
0x00017D10  if ($v0 != 0) goto CLEANUP_FAIL
0x00017D1C  if (dispo == 1) goto CLEANUP_FAIL        ; existing_key only
0x00017D38  set reg_open_flag = 1
0x00017D28  lw  $t5, 0xB303C($at) ; = RegQueryValueExW fptr
0x00017D40  a1=[0x00011340] (valueName = "CalibrationData")
0x00017D50  jalr $t5              ; RegQueryValueExW(hKey, "CalibrationData", 0, &type, lpData, &cbData)
0x00017D58  bnez $v0, CLEANUP_FAIL   ; ← PASS 37 PC
0x00017D5C  lw $a0, 0x88($sp)     ; count  (SUCCESS path continues)
0x00017D64  if (count <= 0) goto FINAL_RET
0x00017D7C  LOOP: jal 0x00017AB0 ; process one calibration point
0x00017DE4  loop while i < count
0x00017DEC  maybe call 0xB31D0(0x1F) logging callback
0x00017E38  jalr $v0              ; optional commit callback
CLEANUP_FAIL:
0x00017E40  if (reg_open_flag) RegCloseKey(hKey)
0x00017E60  LocalFree(alloc #2)
0x00017E68  LocalFree(alloc #1)
FINAL_RET:
0x00017E70  restore regs, jr $ra
```

**Both branches of `bnez $v0, 0x17E40` terminate in plain RegCloseKey + LocalFree + return.** Neither branch calls CreateProcessW, SetEvent, ShellExecuteEx, TouchCalibrate, or anything that would launch a wizard. The function's role is apply-saved-calibration (read the previously-saved coordinate transform from registry, feed it to the touch driver via the per-point loop). When the registry value is absent, the function simply returns.

### Corroborating string-search (0/95 modules reference welcome.exe)

Scanned every one of the 95 XIP modules in `build-host/xip/*.bin` for UTF-16LE or ASCII substrings `welcome.exe`, `WELCOME.EXE`, `Welcome.exe`. **Zero matches.** The only `welcome` hit anywhere in the tree is the debug banner `"Welcome to the Windows CE Shell. Type ? for help."` in `06_shell.exe.bin` — unrelated.

`Boot.exe` (`92_Boot.exe.bin`, 0x6000 bytes) references only `\Windows\PATCHINST.EXE` — not a calibration launcher either.

This rules out the "welcome.exe is hard-coded in some executable" hypothesis. The name must live in the **packed registry** (`default.fdf` at NK offset `0x586598`, CeCompressROM-compressed per Pass 37 §3.2) or be constructed at runtime.

### Runtime corroboration

60-second cold boot with `BE300_LIFECYCLE_PROBE=1` (see `build-host/pass38_gwes_stderr.log`):

```
label=gwes_apply_saved_calib_entry     hits=1  pc=0x00017c40
label=gwes_apply_saved_calib_query_ret hits=1  pc=0x00017d58  v0=0x00000057
label=gwes_apply_saved_calib_fail      hits=1  pc=0x00017e40
label=gwes_apply_saved_calib_cleanup   hits=1  pc=0x00017e70
label=gwes_apply_saved_calib_ok        hits=0
label=gwes_apply_saved_calib_loop      hits=0
```

Predicted pattern `{ entry=1, fail=1, cleanup=1, ok=0, loop=0 }` matches exactly. `v0=0x57` at the `bnez` confirms Pass 37's original probe reading.

**CreateProcessW census** for the full 60 s run (7 hits total, none welcome):

```
hit=1  image="filesys.exe"
hit=2  image="\Windows\SystemPatchModule.exe"
hit=3  image="shell.exe"
hit=4  image="device.exe"
hit=5  image="gwes.exe"
hit=6  image="Boot.exe"
hit=7  image="modmonitor.exe"
```

### Verdict — Thread A

- **Refuted**: Pass 37's hypothesis that PC `0x00017D58` (or its containing function `0x00017C40`) gates welcome.exe.
- **New understanding**: `FUN_00017C40` is `gwes_apply_saved_calibration`. Its job is to load a previously-saved transform from registry and apply it via per-point callbacks. When no saved data exists, it silently returns. No process is ever spawned from it, on either branch.
- **Implication**: the welcome.exe launch mechanism is **not gated inside gwes at all**. We have been looking in the wrong module.

## §3 Thread B — SPL rect-fill is the splash drawer, not a callback

### What the memory note implied

> "100% of post-boot FB repaints (382K writes / 60 s) come from SPL rect-fill at VA `0x80F037CC`, called via `0x80F03734 → 0x80F0334C → 0x80F03320 → 0x80F02284`, with NO direct jal callers (function-pointer/ISR invocation)."

The phrasing suggested `FUN_80f02284` was installed as a callback in a dispatch table somewhere, since its only xrefs in Ghidra are two data-slot references at `0x80F0D004` and `0x80F0D014`.

### What Ghidra decomp actually shows

| Addr | Role |
|---|---|
| `FUN_80f02284` | **SPL main boot-decision function** (one-shot). Chooses Eboot vs Pboot vs default NK handoff. On success, writes PA `0x024FC` / `0x02400` version markers and jumps to NK via `FUN_80f02550(kaddr \| 0x20000000)`. On failure, paints the red error rect (`FUN_80f03734(0, 0x14000F0, 0xF800)`), sleeps, and calls `FUN_80f02530` (likely a halt/retry). |
| `FUN_80f03320` | **Splash initializer.** Calls `FUN_80f038dc` (display init) + `FUN_80f034a0` (LCD backlight / power) + `FUN_80f0334c(0)`. |
| `FUN_80f0334c` | **Decompress-and-blit splash.** First calls `FUN_80f03734(0, 0x00F00140, param_1)` to fill the full 240×320 framebuffer with `param_1` (clear to black when called as `FUN_80f0334c(0)`). Then reads dimensions/size from `DAT_80f012a0..a4`, RLE-decompresses the splash image from `DAT_80f012a8` via `FUN_80f039e4`, and blits the bitmap via `FUN_80f03638`. |
| `FUN_80f03734` | **Rect-fill primitive**, 240×320, 2-byte writes, stride `0x200`, base `0xAA200000` (`-0x55e00000` in decomp). Store instruction at PC `0x80F037CC` inside the double-loop. |

The call chain direction in the memory note was inverted; the real graph is `FUN_80f02284 → FUN_80f03320 → FUN_80f0334c → FUN_80f03734`. Every arrow is a direct `jal`, not a function-pointer invocation.

### The two data-slot xrefs explained

Ghidra shows `0x80F02284` is referenced as data from:
- `0x80F0D004`
- `0x80F0D014`

`get_xrefs_to 0x80F0D000` in turn shows three code-adjacent data references at `0x80F0C4B4`, `0x80F0C4C8`, `0x80F0C4CC` — these are literal-pool slots that hold the pointer `0x80F0D000`. The `0x80F0D000..0x80F0D020` region is **static SPL `.data`** (likely a jump-table / banner-array used once during SPL main to lay out printk strings and default entry points). It is not a runtime callback dispatch table — no code path walks it to `jalr` through entries.

Conclusion: the "no direct jal callers" phrase was correct in that *inside Ghidra's auto-analyzed callgraph* `FUN_80f02284` has no explicit call site, but that is only because SPL's entry vector is reached from the ROM-to-SPL jump at `FUN_80f02550` / reset-vector chain, not from within the SPL module's own code.

### Runtime corroboration

```
fb_body_kseg1_writes total hits in 60 s: 4096 (memory-watch cap)
  all hits:  pc=0x80F037CC, ra=0x80F0336C, data=0x0000
  addr progression: 0xAA200010, 0xAA200012, ... (consecutive 2-byte stores)
```

`ra=0x80F0336C` is the return-address inside `FUN_80f0334c` immediately after its `FUN_80f03734(0, 0xF00140, 0)` call — confirming every observed FB write is the black-clear phase of the splash drawer. `data=0x0000` (black) tells us none of these writes are the red error-rect (that would be `0xF800`), so SPL did NOT take the boot-failure branch.

### Verdict — Thread B

- **Benign.** The 382K writes/60s pattern (larger than the 4096-cap window we sampled) is explained entirely by the SPL splash drawer being invoked on a legitimate boot path. FUN_80f02284 is a one-shot SPL main; its use of the framebuffer is the expected OAL-splash-keeper behaviour that persists because no user-mode paint ever overwrites it.
- Nothing to fix here. If the 382K count really reflects many repeat invocations (vs. one invocation × 76,800 stores × some small multiplier), the multiplicity is caused by an upstream boot-loop or warm-reset cycle triggered by Pass 31 KjCMU logic, not by a callback registration inside SPL `.data`.
- Removes Thread B from the critical-path investigation. No emulator-side work required.

## §4 Ranked next-probe list for Pass 39

The welcome.exe launch mechanism is **not inside gwes**. It is either in the packed registry (probable) or in a kernel-side init script (possible). Pass 39 should pivot accordingly.

### Q1 (HIGHEST — decompress default.fdf and inspect HKLM\init) — ~2-3 hr

Pass 37 §3.2 identified the real registry at `default.fdf` — FILES index 2, NK offset `0x586598`, CeCompressROM-compressed size `0x5D6E`, decompressed `0x13146`. Decompress it and enumerate `HKLM\init\LaunchXX` keys. WinCE's `filesys.exe` reads these at boot and spawns each listed executable in order. If there is **no** `Launch80=welcome.exe` (or similar) entry, then welcome.exe was never on real hardware's boot spawn list either — meaning the calibration wizard is spawned by a different mechanism (e.g., `coshell.exe` on first-boot when it detects an unset `HKLM\HARDWARE\DEVICEMAP\TOUCH\CalibrationData` value and invokes welcome via a fixed path like `\Windows\welcome.exe`).

Pseudocode:
```python
# pass39_decompress_default_fdf.py
nk = open('nk_decompressed.bin','rb').read()
blob = nk[0x586598:0x586598 + 0x5D6E]      # CeCompressROM input
plain = cecompressrom_decode(blob, out_size=0x13146)
# scan for UTF-16LE "Launch", "welcome", "Calibrate"
```

### Q2 (welcome.exe imports — who calls it?) — ~1 hr

`build-host/xip/51_welcome.exe.bin` exists. Extract its import table (IMAGE_IMPORT_DESCRIPTOR) and confirm its entry point. If welcome.exe imports `TouchCalibrate` from `touch.dll` (per Pass 36 memory) and exports only `WinMain`, then it is a leaf process — nobody calls it, but something launches it. The import list confirms what **welcome itself** expects, not who launches it. But a `strings` pass over welcome.exe may reveal the path string the wizard uses to invoke touch.dll, which narrows where calibration data would normally be committed.

### Q3 (coshell first-boot detection) — ~1-2 hr

coshell.exe is the post-gwes UI process. It is CreateProcessW'd by the launcher at position 4 (per Pass 32 memory, launcher-table entry at user VA `0x0203b4d0`). coshell is the most likely candidate to own the first-boot detection because:
- It owns the desktop/shell repaint loop
- It routes WM_PAINT to DefWindowProcW (Pass 35 — stub, so no desktop paint)
- It is positioned AFTER gwes (so it runs post-calibration-query, can see the "no calibration" state)
- On real HW, step 5 of the boot sequence (calibration wizard) happens after step 4 (second Starting splash drawn by user-mode gwes + coshell)

Probe: add `exec_watch_t` entries for coshell's entry point (vbase `0x00010000` per `build-host/xip/50_coshell.exe.txt`) and its CreateProcessW callsites. Look for any spawn attempt. Also search `50_coshell.exe.bin` for UTF-16LE substrings `Calibrat`, `welcome`, `\\Windows\\`.

### Q4 (if Q1-Q3 all dry — look at filesys.exe boot-launch handler) — fallback

If the registry doesn't have `Launch80=welcome.exe` and coshell doesn't invoke it, the next candidate is `filesys.exe`'s first-boot handler. filesys is the first user-mode process (CreateProcessW hit=1) and owns the registry API. It would be the natural place to detect first-boot state and invoke a wizard. Static decomp of filesys.exe with focus on `RegQueryValueExW`-gated CreateProcessW calls.

## §5 Deliverables (what this pass leaves behind)

- This handoff doc.
- Probe code on `investigate/pass38-gwes` branch (`b24a6b86` + the Pass-38-specific additions at `0x17C40`/`0x17D58`/etc.). Nothing pushed; nothing on `main`.
- Memory updates (see separate commit on branch): refresh `project_pass37_welcome_not_registry_gated.md` with the refutation; add `project_pass38_gwes_calibration_not_welcome_gate.md` noting the apply-saved-calibration role; update `project_wince_boot_progress.md`.

## §6 Methodology note (for the next session)

Pass 37's claim was derived from **pattern-matching a PC to a nearby RegQueryValueExW call that returns INVALID_DATA**. It was not derived from a branch-trace of the containing function. This is a reminder: when a probe observes an unexpected status code (here `v0=0x57`), the next step is to decompile the branch that **consumes** that status, not to assume its role from the call site alone. Pass 37's mistake cost one pass; Pass 38 recovered it at the cost of extracting and disassembling the module, which took ~30 min of real work.

The Pass 22+ handoff methodology (scan → pin → cross-reference) is still sound; the failure mode to avoid is "pin without cross-reference." Pass 38's result adds: **always decompile the containing function, not just the probe PC.** The apply-vs-launch distinction is invisible at single-PC granularity.
