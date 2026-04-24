# HANDOFF POST PASS 39 — kernel HKLM\init\Launch enumerator pinned; welcome NOT in the 5-entry list

**Date**: 2026-04-23 (evening, same day as Pass 38)
**Branch**: `investigate/pass38-gwes` (building on Pass 38 WIP)
**Stall state unchanged**: boot still stuck at OAL "Starting" splash, 99.67% pixel match to `Starting.bmp`.

## §1 Why this pass existed

Pass 38 refuted gwes `FUN_00017C40` as the welcome.exe spawn gate. The Pass 38 §4 next-probe list ranked `Q1 = decompress default.fdf (NK off 0x586598, CeCompressROM 0x5D6E/0x13146) and enumerate HKLM\init\LaunchXX`. Pass 39 attempted that + a parallel investigation into exactly which kernel code enumerates Launch entries.

## §2 Discovery: NK hardcodes the `HKLM\init\Launch<NN>` enumerator at `FUN_800808c4`

Scanning `docs/nk_decompressed.bin` for UTF-16LE strings reveals a tight cluster at VA `0x80074A68..0x80074B0C`:

```
VA 0x80074a68  u"Depend"           (Ghidra: u_Depend_80074a68)
VA 0x80074a78  u"Launch"           (Ghidra: u_Launch_80074a78)
VA 0x80074a88  u"init"
VA 0x80074a94  u"SystemPath"
VA 0x80074aa8  u"Loader"
VA 0x80074ab4  u"JITDebugger"
VA 0x80074aca  u"Debug"
VA 0x80074ad4  u"filesys.exe"
VA 0x80074ae8  u"nk.exe"
```

Ghidra xref-to `0x80074a78` identifies exactly one user: **`FUN_800808c4` at PC `0x80080908` [PARAM]**. The decomp shows this is the kernel's `HKLM\init` enumeration main loop — the code WinCE 3.0 uses at boot to collect all Launch entries and spawn them in dependency order. Key observations in the decomp:

- The outer loop calls `RegEnumValue(hKey, iVar2, ...)` via an imported function pointer (`&SUB_ffffabae`) to iterate every value under `HKLM\init`.
- Each value name is compared with `_wcsnicmp(name, u"Launch", 6)` at PC `0x80080908`. On match, the numeric suffix is parsed via `FUN_8009c1f8` and stored as the entry's launch-order ID at `unaff_s5 + count * 0x250`. Then the VALUE DATA (the executable name) is stored at offset `0x12 * 4 = 0x48` inside the entry via `FUN_8008e81c`.
- A second comparison with `u"Depend"` at `0x80074a68` handles dependency lists. `Depend<NN>` values are binary blobs of launch IDs that must be ready before `<NN>` can start. Max 32 Launch entries, max 32 bytes of dependency data per entry.
- After enumeration, a dispatch loop iterates each entry, calls `launcher_dependencies_satisfied_for_init_entry(uVar7)`, yields on `WaitForMultipleObjects` if not, or calls `FUN_8008690c(entry_base + 0x12, ...)` (= `CreateProcessW`) when satisfied.
- After all launches, `_DAT_8066aeec = 0` and a SIGNAL is sent via `(*_DAT_806697e0)(5, 0, 0)` — this is the kernel notifying user-mode that init is complete.

Launcher-entry struct stride = `0x250` bytes (confirms Pass 32's launcher-table observation). Image name field = offset `0x48` inside the struct.

## §3 Implication: welcome.exe is NOT a `HKLM\init\Launch<N>` entry

Pass 32 observed the launcher table at user VA `0x0203B4D0` with exactly **5 entries** (0x250 stride). Pass 38's 60-second run logged exactly **7 CreateProcessW calls** (`filesys.exe`, `SystemPatchModule.exe`, `shell.exe`, `device.exe`, `gwes.exe`, `Boot.exe`, `modmonitor.exe`) — some of which are cascaded spawns from other user-mode processes (shell.exe spawns SystemPatchModule, modmonitor spawns itself on warm reset). Pass 37's 200-second run saw **15 kernel spawns across multiple warm-reset cycles, with `coshell.exe` added on warm boot** — still zero welcome.

If welcome.exe were a Launch entry, the kernel enumerator would see it and CreateProcessW it. It doesn't. Therefore:

- **welcome.exe is not a kernel-level HKLM\init\Launch entry.** Pass 38 §4 Q1 (decompress default.fdf to find the Launch<N>=welcome.exe key) would likely NOT find one.

This changes the Pass 40 strategy. The welcome.exe launch trigger must live in **user-mode first-boot detection logic** somewhere in one of the 7–8 processes we already spawn. The strongest candidates:

1. **coshell.exe** — owns the desktop. Per Pass 37, it's spawned on warm reset only (not the first cold boot). Real-HW sequence has desktop painting BEFORE the calibration wizard (user observation, per `project_wince_boot_sequence.md`), which means coshell runs first and THEN triggers welcome when it detects unset calibration.
2. **Boot.exe** — Pass 32 identified it as having a "reboot-vs-early-exit decision" at WinMain UVA 0x12bd4 based on init-marker file 0x15390. If the init-marker doesn't exist, Boot.exe forces a reboot (observed: SDRAM survives warm-reset). That decision tree is a natural place for welcome to be spawned on a "fresh" boot.
3. **shell.exe** — spawns SystemPatchModule.exe (observed). Could have additional spawn logic gated on registry state.

## §4 Why `default.fdf` CeCompressROM decompression didn't crack

Attempted formats (all failed to cleanly round-trip all 20 pages):

- Classic Okumura LZSS with ring buffer 4096, start pos 0xFEE, flag bit 1 = literal (reused `tools/nk_lzss.py decode_lzss`). Produced mostly zeros because the ring starts empty and backrefs hit the uninitialized ring.
- Flipped-flag LZSS (0 = literal, 1 = backref), various ring starts, min-match 2 or 3: partial decode. Page 19 produced the literal UTF-16LE string `"Launch"` at offset 0xef03 of the combined output — unambiguous evidence this IS the registry file — but no config decompressed all 20 pages cleanly (best: 1/20 page-exact).
- Offset/length bitfield permutations (12-bit offset + 4-bit length, various orderings; 13-bit offset + 3-bit length; high-nibble-as-length vs low-nibble): 0–1 / 20 page-exact.
- zlib / gzip / raw deflate: all rejected (invalid header).

Page layout IS confirmed: 3-byte `total_uncompressed_size` (0x13146) at blob start, followed by 20 × 3-byte `page_end_offset` (relative to start of blob), header total 63 bytes. Compressed page boundaries match the end-offsets exactly. Each page decompresses independently to 4096 bytes (except page 19 = 326 bytes = 0x13146 - 19 × 4096).

The per-page compression algorithm is CeCompressROM's LZ77 variant, but the exact bit encoding needs a reference implementation. Candidates to try in Pass 40:

1. **cegcc source tree** (`cegcc/src/pecoff/`) — Microsoft's leaked or reverse-engineered tool chain.
2. **nlitsme/xipdump** on github — known to handle ROM-compressed sections.
3. **viewcer** / **ceimagex** — various WinCE OSS dump tools.
4. **reference**: the algorithm is also sometimes called "LZROM" or "CE-LZ77" in documentation.

Evidence the file is CeCompressROM and not some other format:
- `nFileSize = 0x13146`, `nCompFileSize = 0x5D6E` in the FILES table entry for `default.fdf`
- Paged 4KB structure with end-offset table matches the documented CeCompressROM layout
- Partial decompress reveals UTF-16LE registry-style strings (`Launch`)

## §5 What probe evidence Pass 39 captured

Ghidra-side (no new runtime probe needed — the enumerator decomp was the diagnostic):

- `FUN_800808c4` — kernel's HKLM\init\Launch enumerator entry. Should be renamed via `mcp__ghidra__rename_function_by_address`.
- `FUN_80080640(s, u_Launch_80074a78, 6)` — this IS `_wcsnicmp(s, L"Launch", 6)`. Candidate rename: `_wcsnicmp_probably`.
- `FUN_8009c1f8(&buf)` — atoi-style parse of the decimal suffix of `Launch<NN>`. Candidate rename: `atoi_wide_buf`.
- `FUN_8008e81c(entry_base + 0x48, name)` — installs the executable-name string in the launch entry. Candidate rename: `install_launch_entry_name`.
- `FUN_8008690c(name, parent, 0, 0)` — kernel CreateProcessW. Already known as `spawn_module_createprocess_path` via the existing `be300_probe.c` probe.
- `(*_DAT_806697e0)(5, 0, 0)` — post-init notification callback. Candidate rename: `notify_init_complete`.
- `_DAT_8066aee8` — launch-entry count (0..32). Candidate rename: `g_launch_entry_count`.
- `_DAT_8066af00` — event handle used by `WaitForMultipleObjects` to gate launches on dependencies. Candidate rename: `g_launch_dependency_event`.

Runtime-side: Pass 38 probe hits corroborate (`launcher_wait_loop` hits=3, `launcher_blocking_wait_call` hits=3, `launcher_module_ready_notify` hits=3 — these are inside the dispatch loop AFTER the enumerator completes, which is why the enumerator itself doesn't need a dedicated probe).

## §6 Ranked next-probes for Pass 40

1. **Q1 (highest signal):** Run a 200+ second `BE300_LIFECYCLE_PROBE=1` cold boot capturing **every** RegQueryValueExW key-path/value-name pair, and post-filter for anything queried AFTER `launcher_module_ready_notify hit=3` (= post-init). That is the window where user-mode first-boot detection runs. Look for any queried path containing `welcome`, `Calibration`, `RunOnce`, `BootCount`, `FirstBoot`, `Setup`, `OOBE`, `Wizard`, or `Init`. Pass 37 did this for 200 s but only reported misses — re-run with the exact filter and dump the full key/value list sorted by time-first-queried to spot any newly-reached path.
2. **Q2 (medium):** Static decomp of `build-host/xip/50_coshell.exe.bin` (vbase `0x00010000`, .text at file offset ~0x1000). Focus areas:
   - `strings -a -e l 50_coshell.exe.bin | grep -iE 'welcome|calib|runonce|firstboot|setup|init'`
   - Import table — does coshell import `CreateProcessW` / `ShellExecuteEx` / `RegQueryValueExW`?
   - Entry point + any first-boot-gated branches.
3. **Q3 (medium):** Static decomp of `build-host/xip/92_Boot.exe.bin` WinMain at UVA `0x00012BD4`. Per Pass 32, this function checks init-marker file `0x15390` and reboots if absent. What is the init-marker file name, where is it stored, and does the emulator create it correctly?
4. **Q4 (implementation):** Port a CeCompressROM decompressor to `tools/decompress_cecompress.py`. Once cracked, dump the full `HKLM\init` key with all values — the authoritative source of which Launch<N> entries exist.

## §7 Deliverables

- This handoff doc.
- `investigate/pass38-gwes` branch (still no push to main). Pass 38 probe code carries forward; no new probes added this pass (the Ghidra decomp was sufficient).
- Memory updates (separate commit): add `project_pass39_nk_launcher_at_800808c4.md`, refresh `MEMORY.md` index.

## §8 Methodology notes

Pass 38's lesson — "always decompile the containing function, not just the probe PC" — continued to pay off here. The key finding (identifying `FUN_800808c4` as the launcher) came from following a SINGLE Ghidra xref from the `u"Launch"` string literal to its one consumer. No runtime probes were needed.

The pairing of strings-in-NK search with Ghidra xrefs is now established as the preferred triage method: grep `docs/nk_decompressed.bin` for UTF-16LE / ASCII substrings of interest, cross-reference to the one or two code sites that use each string, decomp those. This converges much faster than runtime instrumentation when the question is "what code handles X?"
