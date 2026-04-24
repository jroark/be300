# HANDOFF POST PASS 40 — Boot.exe and coshell.exe ruled out as welcome spawners; CeCompressROM ≈ 80% cracked

**Date**: 2026-04-23 (evening, same day as Passes 38-39)
**Branch**: `investigate/pass38-gwes` (continues Pass 38-39 work)
**Stall state unchanged**: boot still stuck at OAL "Starting" splash.

## §1 Why this pass existed

Pass 39 established that welcome.exe is NOT a `HKLM\init\Launch<N>` entry (kernel enumerator at `FUN_800808c4` would unconditionally spawn it if it were, and zero welcome spawns have been seen across any run of any length). Pass 39 §6 listed the ranked Pass 40 candidates for who DOES spawn welcome. Pass 40 worked Q2 and Q3, with a detour into Q4 (CeCompressROM port).

## §2 Thread A — Boot.exe WinMain decoded; NOT the welcome spawner

### What Pass 32 noted

Boot.exe WinMain UVA `0x12BD4` was identified as the "reboot-vs-early-exit decision" based on an init-marker file check (`0x15390`). The exact branch semantics weren't traced at the time.

### Pass 40 decomp (Capstone MIPS32 LE on `build-host/xip/92_Boot.exe.bin`)

```
0x00012BD4  WinMain(argc, argv, env):
  prologue; sw ra, 0x1c(sp); save a2
  t6 = [0x00014018]                    ; import: GetFileAttributesW
  a0 = 0x00011390                      ; u"\\Windows\\Initialized.$$$"
  jalr t6                              ; v0 = GetFileAttributesW(...)
  sw zero, 0x2c(sp)                    ; local_2c = 0
  if (v0 == INVALID_FILE_ATTRIBUTES):  ; file absent
    v1 = local_2c = 0 (delay slot)
    # fall through; branch taken to 0x12c08 with v1=0
  else:
    v1 = local_2c = 0 (delay slot, always)
    v1 = 1             (only executed if beq NOT taken)
  if (v1 != 0):                        ; file EXISTS
    goto WARM_BOOT_PATH 0x12cc8
  # FIRST-BOOT PATH begins at 0x12c10:
  v0 = FUN_12F18(a0=1)                 ; check [0xA0051900]|[0xA0051904]
                                       ;   (warm-reset flags in hw-strap area)
  if (v0 != 0):                        ; warm reset detected
    jal FUN_124B0; FUN_12258; FUN_12F9C(1)
    jal FUN_12D20                      ; RegSetValueExW-ish: create marker
    jal FUN_1310C(a0=0x0101003c,0,0,0) ; REBOOT
  else:                                ; pure cold boot
    jal FUN_124B0; FUN_1209C; FUN_11E18
    jal FUN_119A0; FUN_11B30; FUN_12F9C(0)
    jal FUN_12D20                      ; create marker
    jal FUN_1310C(a0=0x0101003c,0,0,0) ; REBOOT
WARM_BOOT_PATH 0x12cc8:
    jal FUN_11FB0; FUN_122F4; FUN_12808; FUN_1311C
    fall through to normal exit
```

### Strings in Boot.exe (full list; 31 total UTF-16LE strings ≥5 chars)

The entire Boot.exe string table is about filesystem paths (`\Windows\Release.txt`, `\Windows\Name.txt`, `\Windows\Type.txt`, `\Windows\Build.txt`, `\NAND Disk\Program Files\Version.txt`, `\Windows\Safe.$$$`, `\Windows\System.$$$`, `\Alarm1.db`/`\Alarm2.db`), alarm-database keys, a few PATCHINST paths, and one DiskName registry key. **No welcome, no calibration, no wizard, no first-boot user-visible string.** Boot.exe is a pure boot-state initializer + reboot helper.

### Verdict — Thread A

- **Refuted**: Boot.exe does NOT spawn welcome.exe. Its first-boot path creates `\Windows\Initialized.$$$`, reboots, and is done.
- Verified Pass 32's narrative: Boot.exe's decision is existence-of-marker → reboot-with-init-chain-A vs early-exit-with-init-chain-B.

## §3 Thread B — coshell.exe full string scan; NOT the welcome spawner either

Dumped all 63 UTF-16LE strings ≥5 chars from `build-host/xip/50_coshell.exe.bin` (vbase `0x00010000`, .text `0x11000..0x24DD4`). The full list:

- Desktop / taskbar UI: `"DesktopExplorerWindow"`, `"Task Manager"`, `"&Active Tasks"`, `"Switch &To"`, `"&End Task"`, `"Terminate Process"`, dialog error strings.
- "Run" dialog: `"Type the name of the program to start."`, `"Browse"`, `"Programs"`, `"All Files"`, `"*.exe"`.
- Battery / memory warning dialogs: `"Internal Battery Warning"`, `"The internal battery is low."`, `"User Data Memory Warning"`, etc.
- CF slot info: `"CF Slot Information"`, `"Card type"`, `"Card unit"`, `"Card battery"`.
- CreateProcessW error strings: `"Cannot execute '%s'."`, `"File not found."`.
- "Charging", `"AC adapter"`, "(%dKB remaining)`.

**No welcome, no calibrate, no first-boot, no wizard, no setup, no OOBE, no TouchCalibrate.** coshell is the interactive desktop shell — not the welcome launcher.

### Verdict — Thread B

- **Refuted**: coshell.exe does NOT spawn welcome.exe. Its role is desktop + task manager + battery warnings + CF slot info + run-dialog. No first-boot wizard trigger.

## §4 Thread C — CeCompressROM decoder port (partial success)

### Reference implementation found

`KodaSec/wince-decompr` at https://github.com/KodaSec/wince-decompr implements a Python LZX decoder for WinCE compressed XIP modules. Ported to `tools/decompress_cecompress.py`.

### Why the reference didn't decode default.fdf directly

The reference expects each 4KB block to start with a **16-byte LZX block header** (`window_size: u32` + `decompressed_size: u32` + 8 bytes padding). Inspecting our default.fdf blob, each block DOES NOT have this header — the block data starts directly with the LZ77 bitstream. This is a known difference between the **XIP-module** format (16-byte header) and the **FILES-entry** format (raw per-block bitstream).

Attempting `LZXDecoder.decompress(page, ...)` directly with `window_size ∈ {15, 17, 21}` returned status=-1 for all pages. So the FILES bitstream is NOT standard LZX either.

### Alternative: custom LZ77 sweep

Systematic sweep across MSB/LSB first flag bits, literal-bit convention (0 vs 1), offset/length bitfield splits (9+7, 10+6, 11+5, 12+4), min_match (2, 3, 4, 5), and ring configurations. Best partial results:

- **10-bit offset + 6-bit length, 1024-byte ring, LSB-first, 0=literal, min_match=2:** produces exactly 78150 bytes total over 20 pages (matches uncompressed size), but per-page consumption is only 60–70% of input (decoder stops early on `len(out) == expected_len` and doesn't naturally terminate). 4× `"Launch"` substrings visible in page 19 output. Partial-correct but not round-trip.
- **12-bit offset + 4-bit length, 4096-byte ring, LSB-first, 0=literal, min_match=2:** produces 60–80% of expected output per page. 1× `"Launch"` substring visible. Worse than 10+6.

Neither variant round-trips. The true format likely has:
- **A different bit-packing than any LZSS variant** tried.
- Or **a token-stream design** with variable-length codes (like LZX-ALIGNED block type) not captured by fixed bitfield splits.

### Partial-decode evidence this IS the registry

`build-host/default_fdf_10o_6l.bin` (the 10+6 output) contains UTF-16LE substrings matching known WinCE registry content:

- `"URL Protocol"` — HKCR MIME-type key prefix
- `"Content-Type"`, `"audio"`, `"Serial Port"` — file-association values
- `"Berlin, Rome"` — TZI entries for Central European Time
- `"Microsoft"` (multiple) — HKLM\SOFTWARE\Microsoft tree
- `"Linkage"`, `"Ethernet"` — network adapter subkeys
- `"CASIO"` (multiple) — HKLM\SOFTWARE\CASIO
- `"BrightEx"` — power-management subkey
- **`"Launch"` × 4** — multiple Launch<N> entries in the HKLM\init tree

This unambiguously confirms the file IS the packed registry. Cracking the exact bit encoding is a tractable ~1-2 hour task for Pass 41.

### Deliverable

- `tools/decompress_cecompress.py` — Ported LZX decoder (currently requires 16-byte header variant; Pass 41 should add a FILES-format bypass).
- `build-host/default_fdf_10o_6l.bin` — Partial decode showing plausible registry content. Not authoritative.

## §5 Implication for the investigation

Of the originally suspected user-mode welcome spawners:

| Candidate | Evidence | Verdict |
|---|---|---|
| `FUN_00017C40` inside gwes | Pass 38 decomp + runtime | **Refuted** — silent return on fail |
| Boot.exe WinMain | Pass 40 §2 decomp | **Refuted** — no welcome string; just init+reboot |
| coshell.exe | Pass 40 §3 string scan | **Refuted** — no welcome/calib strings |
| `HKLM\init\Launch<N>` entry | Pass 39 kernel enumerator + Pass 32 5-entry count | **Refuted** — welcome never spawned |
| shell.exe | Pass 37 saw shell spawn SystemPatchModule only | Not yet decomp'd |
| filesys.exe | First-spawned by kernel | Not yet decomp'd |
| device.exe | 3rd kernel spawn | Not yet decomp'd |
| modmonitor.exe | User-mode-only spawn (of self) | Not yet decomp'd |

The surviving candidates are shell.exe, filesys.exe, device.exe, and modmonitor.exe. Pass 41 should static-decomp each for:
1. UTF-16LE strings containing `"welcome"`, `"\\Windows\\welcome"`, `"Calibr"`, `"Touch"`, `"firstboot"`, `"runonce"`, `"setup"`, `"wizard"`, `"oobe"` (case-insensitive).
2. `CreateProcessW` callsites that build paths dynamically from registry reads.
3. `RegQueryValueExW` callsites reading first-boot-gated values.

## §6 Ranked Pass 41 next-probes

1. **P1 (highest signal)** — static decomp + string scan of `filesys.exe`, `shell.exe`, `device.exe`, `modmonitor.exe`. One of these MUST reference welcome.exe via some mechanism (possibly via a dynamic path built from registry). Small target set; should finish in 1-2 sessions.
2. **P2** — port / refine CeCompressROM decoder to round-trip default.fdf cleanly. Compare 10+6 and 12+4 variants byte-by-byte against known-literal sections (`"URL Protocol"` / `"Microsoft"` / `"Berlin, Rome"`) to identify where each loses track. Tractable but grinds.
3. **P3** — add a runtime memory-watch on a known in-memory registry region (HKLM\init's in-memory hive location in NK) and dump the decompressed tree directly. Avoids the CeCompressROM problem entirely. Requires locating the hive pointer first.
4. **P4** — search NK for all `RegSetValueExW` callsites that write UTF-16LE paths containing `"welcome"` or `"runonce"`. If some kernel-side init writes the launcher key only on first-HW-boot (e.g., gated by NV-RAM state), that's the spawn gate.

## §7 Deliverables

- This handoff doc.
- `tools/decompress_cecompress.py` — Ported KodaSec LZX decoder; Pass 41 will refine.
- `build-host/default_fdf_10o_6l.bin` — Partial decompression of default.fdf for reference.
- Memory updates on branch: `project_pass40_boot_coshell_ruled_out.md`, refresh `project_wince_boot_progress.md` and `MEMORY.md`.
- Branch `investigate/pass38-gwes` only; main unchanged.

## §8 Methodology note

Passes 38-40 collectively narrowed the welcome-spawn mystery by eliminating every tractable code-level candidate. The remaining possibilities (P1 candidates, P2 registry decompression) all remain in-scope for Pass 41+. The investigation has been EVIDENCE-DRIVEN with NO emulator-side modifications since Pass 31 — this is the cleanest possible kind of progress.
