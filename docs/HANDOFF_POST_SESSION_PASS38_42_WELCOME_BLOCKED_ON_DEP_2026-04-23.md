# HANDOFF — 2026-04-23 session close (Passes 38–42)

**Session span:** Passes 38 through 42, same day.
**Branch:** `investigate/pass38-gwes` (local only; **never pushed; `main` untouched**).
**Stall state at session start:** boot stuck at OAL "Starting" splash.
**Stall state at session close:** same splash (99.67 % pixel match to `Starting.bmp` after 60 s).
**Net forward progress:** the welcome.exe spawn mechanism has been localized to the kernel `HKLM\init\Launch<N>` enumerator. welcome.exe is almost certainly a Launch entry with a `Depend<N>` on a process that never signals ready in our emulator.

## 1. TL;DR — one-screen summary of today's result

- welcome.exe never spawns in the emulator. Pass 37 thought this was gated by gwes at PC `0x00017D58`. That was refuted in Pass 38.
- The REAL gate is WinCE's `HKLM\init\Launch<N>` enumerator in NK at `FUN_800808c4` (PC `0x80080908` for the `u"Launch"` comparison). Identified in Pass 39. 5 entries are observed (Pass 32 count + our own probe data match).
- Pass 40 decompiled Boot.exe WinMain (init-marker+reboot helper, not a welcome launcher) and scanned coshell.exe strings (desktop shell, not a welcome launcher) — both refuted.
- Pass 41 exhaustively scanned all 95 XIP modules + NK kernel for ANY hardcoded `"welcome.exe"` spawn-path string. **ZERO hits anywhere.** The string CreateProcessW uses on real hardware must be loaded from the registry at runtime.
- Pass 42 decoded the kernel's dependency check (`FUN_800805A8`) and correlated with Pass 38 runtime data: **only 3 of the 5 Launch entries ever signal "ready"** — `shell`, `device`, `gwes`. `Boot.exe` reboots via `FUN_1310C(0x0101003c)` before signaling; `coshell.exe` only spawns on warm reset. Any Launch entry whose `Depend<N>` list references the other two stays parked forever in `WaitForMultipleObjects(_DAT_8066AF00)`.
- CeCompressROM decompression of `default.fdf` is NOT cracked yet (best variant gives page 19 at 324/326 with `"Launch"` substrings visible, but no full-round-trip).

**Falsifiable hypothesis for the next session:** welcome.exe IS a `Launch<N>` entry in `HKLM\init` with `Depend<N>` containing either Boot's or coshell's launch-order ID. Cracking `default.fdf` OR probing the `entries[*].started` array at user VA `0x0203B4D0` confirms or refutes this directly.

## 2. Per-pass links (authoritative handoff chain)

| Pass | Doc | Key finding |
|---|---|---|
| 38 | `docs/HANDOFF_POST_PASS38_GWES_AND_SPL_REPAINT_2026-04-23.md` | gwes `FUN_00017C40` (containing Pass 37's PC) = apply-saved-calibration, silent RegCloseKey+free on fail. **Pass 37 hypothesis REFUTED.** SPL rect-fill at PC `0x80F037CC` is benign splash drawer via `FUN_80f02284→FUN_80f03320→FUN_80f0334c→FUN_80f03734`, not a callback. |
| 39 | `docs/HANDOFF_POST_PASS39_KERNEL_LAUNCHER_AT_FUN_800808C4_2026-04-23.md` | Kernel `HKLM\init\Launch<N>` enumerator pinned at `FUN_800808c4` (PC `0x80080908` for the `u"Launch"` cmp). UTF-16LE strings `u"Depend" / u"Launch" / u"init" / u"filesys.exe" / u"nk.exe"` cluster at VA `0x80074A68..0x80074B0C`. Struct stride 0x250, image-name offset 0x48 match Pass 32. |
| 40 | `docs/HANDOFF_POST_PASS40_BOOT_COSHELL_RULED_OUT_2026-04-23.md` | Boot.exe WinMain = init-marker+reboot helper. coshell.exe strings = desktop/taskbar only. KodaSec LZX reference ported to `tools/decompress_cecompress.py`; FILES-entry format doesn't have the 16-byte per-block LZX header. Custom 12+4 LZ77 gives page 19 at 324/326 bytes. |
| 41 | `docs/HANDOFF_POST_PASS41_ALL_CANDIDATES_RULED_OUT_2026-04-23.md` | All 4 remaining user-mode candidates (filesys, shell, device, modmonitor) + all 88 other XIP modules + NK kernel have ZERO hardcoded `"welcome.exe"` spawn-path strings. |
| 42 | `docs/HANDOFF_POST_PASS42_DEPENDENCY_CHAIN_DECODED_2026-04-23.md` | `launcher_dependencies_satisfied_for_init_entry` @ `FUN_800805A8` decoded. Runtime: `launcher_module_ready_notify hits=3` = only shell/device/gwes signal; Boot reboots, coshell doesn't spawn on first boot. Any entry with Depend on Boot/coshell parks forever. HKLM\Services scan clean. |

## 3. Cumulative candidate-elimination table

| Candidate | How eliminated | Pass |
|---|---|---|
| gwes `FUN_00017C40` (Pass 37 hypothesis) | Static decomp — silent return on fail | 38 |
| SPL rect-fill at 0x80F037CC as "callback" | Decomp — it's the SPL boot `main`, one-shot | 38 |
| Boot.exe WinMain | Decomp — init-marker + reboot; no welcome string | 40 |
| coshell.exe | Full string scan — desktop/taskbar only | 40 |
| filesys / shell / device / modmonitor | String scan — zero hits | 41 |
| All 88 other XIP modules | Exhaustive scan — zero hits | 41 |
| NK kernel (for a hardcoded welcome path) | Exhaustive scan — zero hits | 41 |
| HKLM\Services second-mechanism | Scan — only repllog (ActiveSync), unrelated | 42 |

**What's LEFT:** welcome.exe IS in the TOC (entry 51), IS on disk as `build-host/xip/51_welcome.exe.bin`, BUT its spawn path string exists only as a dynamic value LOADED FROM `default.fdf` at boot. The runtime-loaded string then flows into `FUN_8008690c` (kernel CreateProcessW).

## 4. Key code addresses (for Pass 43+ reference)

### NK kernel (vbase 0x80060000)

| Name | Address | Role |
|---|---|---|
| `u_Depend` | `0x80074A68` | UTF-16LE `"Depend"` |
| `u_Launch` | `0x80074A78` | UTF-16LE `"Launch"` (sole xref = `FUN_800808c4` @ `0x80080908`) |
| `u_init`, `u_SystemPath`, etc. | `0x80074A88..0x80074B0C` | init-sequence string table |
| **launcher main loop** | **`FUN_800808c4`** | reads HKLM\init, collects Launch+Depend, dispatches |
| **dep check** | **`FUN_800805A8`** | returns 1 if all deps satisfied, else 0 |
| CreateProcessW kernel thunk | `FUN_8008690c` | probed as `spawn_module_createprocess_path` |
| launcher_wait_loop | `0x80080AA4` | probed |
| launcher_blocking_wait_call | `0x80080CB4` | probed |
| launcher_module_ready_notify | `0x80080D38` | 3 hits in 60s |
| `_DAT_8066AEE8` | global | launch-entry count (0..32, observed 5) |
| `_DAT_8066AEEC` | global | base of entry struct array |
| `_DAT_8066AF00` | global | dependency event handle |

### User-mode launcher table (observed in Pass 32/38)

- Base: user VA `0x0203B4D0` (in filesys's address space after it owns the init tree)
- Stride: `0x250` bytes per entry
- Fields known: `+0x00 order_id` (u32), `+0x04 started` (u32 0/1), `+0x08` onwards = u16 Depend<NN> list terminated by 0, `+0x48` = image-name UTF-16LE buffer
- Count: 5 entries

### gwes.exe (vbase 0x00010000, loaded at NK VA `0x0007xxxx+something`)

| Name | Address | Role |
|---|---|---|
| `apply_saved_calibration` | `0x00017C40` | reads `HKLM\…\CalibrationData`; silent return on fail |
| CalibrationData query | `0x00017D58` | `bnez $v0, 0x17E40` — the Pass 37 PC |
| fail cleanup | `0x00017E40` | RegCloseKey + LocalFree + return |

### Boot.exe (vbase 0x00010000)

| Name | Address | Role |
|---|---|---|
| WinMain | `0x00012BD4` | GetFileAttributesW("\Windows\Initialized.$$$") → reboot or exit |
| init-marker string | `0x00011390` | `u"\Windows\Initialized.$$$"` |
| reboot thunk | `0x0001310C` | takes a0 = reboot code |

### SPL (loaded at VA 0x80F00000 from NAND)

| Name | Address | Role |
|---|---|---|
| SPL main (boot decision) | `0x80F02284` | Eboot/Pboot/NK chooser, one-shot |
| splash init | `0x80F03320` | calls display init + backlight + splash-draw |
| splash draw (decompress+blit) | `0x80F0334C` | clears FB, decompresses splash bitmap, blits |
| rect-fill primitive | `0x80F03734` | 240×320 stride 0x200, base `0xAA200000` |
| rect-fill inner store | `0x80F037CC` | the `sh` instruction inside the loop |

## 5. Committed probe infrastructure (on `investigate/pass38-gwes` branch, NOT on main)

Branch history:

```
fa4f7abb  pass 42 handoff
31af6b7d  pass 41 handoff
9437c8d0  pass 40 handoff + tools/decompress_cecompress.py (LZX port)
eb156c7a  pass 39 handoff
cd5d9702  pass 38 handoff + 6 gwes_apply_saved_calib_* probes in be300_probe.c
b24a6b86  WIP: Pass 38 probe infrastructure (do not merge)
```

`b24a6b86` consolidated the Pass 34–38 probe infrastructure from the working tree (src/be300_probe.{c,h}, CMakeLists.txt, wiring in machine_be300.c and be300_devices.c). Pass 38 added 6 new exec_watches at gwes PCs `0x17C40`, `0x17D58`, `0x17D5C`, `0x17D7C`, `0x17E40`, `0x17E70`.

Infrastructure is gated on `BE300_LIFECYCLE_PROBE=1`. When unset, baseline emulator behavior is byte-identical to main.

**Do NOT merge this branch to main.** Pass 43+ will cherry-pick only functional emulator fixes after evidence is conclusive.

## 6. Pass 43 ranked next-probes

### P1 (highest expected value) — runtime memwatch on `entries[*].started`

For each of the 5 launcher entries at `0x0203B4D0 + i*0x250 + 0x04`, add a `mem_watch_t` with `log_writes=true, log_reads=false`. Run `BE300_LIFECYCLE_PROBE=1 gtimeout 120s build-host/be300 --nand ce/restore_images/All_nand_300.bin`. The slots that show `started=1` are fully launched; the slots stuck at `started=0` are the candidates welcome is depending on.

Effort: 20 lines of probe code, one run. Produces direct runtime evidence for the hypothesis.

### P2 — dump blocking dep_id

In `FUN_800805A8`, add exec_watches at the TWO `return 0` instructions. Capture `$v0` / `$a0` / stack state at each call that returns 0. The dep_id at each failure, cross-referenced with the launcher table's `order_id` field, names exactly which entry is blocking.

Effort: 3-4 probe lines + trivial log interpretation.

### P3 — hand-trace CeCompressROM page 19

Pass 40's 12+4 mm=2 variant gave page 19 at 324/326 bytes. Page 19 contains `u"Launch"` substrings. The next UTF-16LE code unit after each `Launch` must be an ASCII digit (`'0'..'9' = XX 00` UTF-16LE). Match my decoder's OUTPUT at those bit positions against this constraint; the miss tells us exactly how the length/offset encoding needs to shift.

Effort: 1–2 hours of careful bit-level trace.

### P4 (dangerous — debug-only) — force coshell to spawn on first boot

Temporarily patch the launcher to ignore dependencies for coshell, or force its `started=1` at init. Observe whether welcome then spawns. Fast hypothesis test but violates CLAUDE.md's "no guest patches / no synthetic handoff shortcuts" rule. For diagnosis only — revert before committing anything.

## 7. What NOT to re-investigate

From Passes 38–42 (and earlier, preserved for continuity):

- gwes `0x17D58` / `FUN_00017C40` — silent cleanup path only; no spawn logic anywhere in this function.
- SPL's persistent FB repaint — benign splash drawer; not a callback/ISR.
- Boot.exe for a welcome branch — it only reboots.
- coshell.exe, filesys, shell, device, modmonitor (strings only); **92 of 95 XIP modules scanned clean**.
- `HKLM\Services` as a second spawn mechanism — doesn't exist on BE-300.
- Classic/flipped LZSS + 9/10/11/12-bit offsets × 2-5 min_match × LSB/MSB flag bits for CeCompressROM — systematically swept; none round-trip.

## 8. Environment notes

- `tools/decompress_cecompress.py` added (port of KodaSec/wince-decompr LZX). Useful for XIP-module decompression later; not suitable for FILES-entry format as-is.
- `build-host/xip/` holds all 95 extracted modules (`python3 tools/extract_xip_modules.py docs/nk_decompressed.bin build-host/xip` regenerates them).
- `build-host/default_fdf_10o_6l.bin` / `default_fdf_12o_4l.bin` are partial decompressions of `default.fdf` (NOT authoritative; for string-search only).
- `build-host/pass38_gwes_stderr.log` and `pass38_gwes_stdout.log` are the Pass 38 full-run captures with `BE300_LIFECYCLE_PROBE=1`; preserve for Pass 43 re-analysis.
- 60-second cold boots are the standard test; `gtimeout` exit `124` is expected and NOT an error.
- `build-host/screenshot_20260423_191512.bmp` is the latest captured frame (99.67 % pixel match to `Starting.bmp`).

## 9. Methodology notes accumulated this session

- **Pass 38's lesson:** always decompile the containing function, not just the probe PC. The "apply" vs "launch" distinction is invisible at single-PC granularity, and Pass 37's inference from nearby registry-call names was what cost a whole pass.
- **Pass 39's lesson:** string-grep NK + Ghidra xref-to is the single fastest triage technique for "what code handles X?" Converges in minutes.
- **Pass 40's lesson:** reference implementations for WinCE ROM formats split into "XIP-module" (16-byte LZX header) vs "FILES-entry" (raw bitstream, no header). The KodaSec XIP decoder is wrong for FILES and returns status=-1.
- **Pass 41's lesson:** exhaustive scan covering 95 modules is cheap and definitive. If you can't find the string, it's loaded at runtime from the registry; that's falsifiable.
- **Pass 42's lesson:** runtime probes (even already-in-place ones) have interpretive value beyond their original target. `launcher_module_ready_notify hits=3` was collected for a different question; it turns out to directly count satisfied dependency sources.

## 10. One-line bottom line

**Welcome.exe is almost certainly a `HKLM\init\Launch<N>` entry whose `Depend<N>` points at Boot.exe or coshell.exe — neither of which signals "ready" on a cold boot in our emulator — so welcome's dispatcher loop parks indefinitely in `WaitForMultipleObjects(_DAT_8066AF00)`.** Pass 43 P1 or P2 directly confirms or refutes this without needing to crack `default.fdf`.
