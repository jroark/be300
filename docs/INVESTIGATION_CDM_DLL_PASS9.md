# Pass 9 Investigation Plan — cdm.dll Driver-Init Deadlock

**Status:** ready to pick up.
**Origin:** branched off Pass 9 (2026-04-19) as a parallel investigation
when the broader stall hypothesis pivoted to "HW-triggered reset
between splash screens" (separate plan owned elsewhere).
**Owner:** unassigned.

## Summary for new readers

The Casio BE-300 emulator (this repo) cold-boots WinCE 3.0 from
`ce/restore_images/All_nand_300.bin` to the first "Starting…" splash
and then stalls. Six guest threads are blocked. Pass 9 (2026-04-19)
identified the **likely trigger** of the stall: device.exe's driver
enumeration loads the PCMCIA driver chain, which loads `cdm.dll`, and
the stall begins inside cdm.dll's DllMain or its downstream
`XXX_Init`. This document is the plan for following that thread.

If you have not read these first, do so:

1. `CLAUDE.md` (repo root) — emulation philosophy, source layout,
   "WinCE Boot Facts" section, "Splash Screen Notes",
   "Current Investigation Guidance".
2. `docs/HANDOFF_POST_PPSH_STALL_2026-04-18.md` §1–§4, §6–§9 —
   thread enumeration, lock-inversion analysis (still current
   apart from §5C cadence theory which was refuted).
3. `docs/HANDOFF_POST_CADENCE_FIX_2026-04-19.md` — §3 (cadence-fix
   summary, now landed), §5.1 (the driver-identification work this
   doc continues).
4. `~/.claude/projects/-Users-jroark-src-be300/memory/project_post_ppsh_stall.md`
   — full passes 1–9 evidence chain.

## Reproduction

```bash
cd build-host && cmake .. && make -j$(sysctl -n hw.ncpu)
BE300_LIFECYCLE_PROBE=1 gtimeout 30s ./be300 \
  --nand ../ce/restore_images/All_nand_300.bin \
  > cold_stdout.log 2> cold_stderr.log
shasum screenshot_*.bmp
grep BE300_LIFECYCLE_LOAD cold_stderr.log
```

Expected: exit 124 (gtimeout, not an error), screenshot SHA
`e8a8c83cd66b9327f50fc1827eada71fb028b332` (= `Starting.bmp` at repo
root), and a `BE300_LIFECYCLE_LOAD` chronological log whose last
entries are cdm.dll loads.

## What Pass 9 established (do not re-derive)

- The probe at `src/be300_probe.c` instruments `module_load_core_dispatches_dllmain`
  (FUN_8008fd3c), `LoadLibrary_parse_filename_and_dispatch` (FUN_80091c90),
  and `LoadLibrary_kernel_acquire_loadercs_wrapper` (FUN_80093040).
  Trace from a 30 s cold-boot run shows:

  1. Hits 1–11: `coredll.dll`, `toolhelp.dll`, `COREDLL.dll`
     (process startup).
  2. **Hit 12: `PCMCIA.DLL`** — first and only `acquire_loadercs` hit
     in the entire run. `a0=0x00011348` (user-mode filename pointer),
     `ra=0x01F8EEB4` (user-mode coredll `LoadLibraryW` caller, in
     coredll range `0x01F00000..0x02000000`).
  3. Hits 13–20: `COREDLL.dll`, `cdm.dll`, `COREDLL.dll`, `cdm.dll`,
     `COREDLL.dll`, `cdm.dll`, `COREDLL.dll`, `cdm.dll` —
     PCMCIA's `DLL_PROCESS_ATTACH` chain pulling cdm.dll and its
     coredll dependencies.
  4. **After hit 20: NO more loader calls.** The stall begins inside
     cdm.dll's DllMain or downstream init.

- T5 thread struct dump (256 bytes from `0x80FD592C`) confirms T5 is
  parked with kernel `EPC = 0x80088230` (CS-wait function), waiting on
  the LoaderCS at `0x806695A0` (the same CS T3 holds). T5's user-mode
  saved EPC is `0x080AF000` in slot-3 ASID — the device.exe process
  slot, where PCMCIA / cdm.dll loaded.

- Ghidra renames already persisted (do not redo):
  `0x80093040 → LoadLibrary_kernel_acquire_loadercs_wrapper`,
  `0x80091c90 → LoadLibrary_parse_filename_and_dispatch`,
  `0x8008fd3c → module_load_core_dispatches_dllmain`,
  `0x80090a24 → LoadLibrary_increment_refcount_and_load`,
  `0x8008831c → WaitForMultipleObjects_kernel_syscall_shim`,
  `0x800998c0 → EnterCriticalSection_kernel`,
  `0x80099924 → LeaveCriticalSection_kernel`.

- Decompiler comments at `0x80093040` and `0x8008fd3c` capture the
  Pass 9 evidence.

## Hypothesis to verify or refute

Working hypothesis (Pass 9): "On real HW with no CompactFlash card
present, cdm.dll's DllMain (or PCMCIA.DLL's `XXX_Init` after cdm load)
detects the no-card condition early via VRC4173 PCC0 status registers
(`0x0F000200..0x0F0003FF`) and short-circuits without spawning a
worker. In our emulator, the VRC4173 PCC0 registers return 0 / a stale
value that the driver misinterprets as 'card present', sending it down
the long `XXX_Init` path that spawns T5. T5's worker then needs
LoaderCS (held by T3 still inside the LoadLibrary chain) and
deadlocks."

This is plausible but unverified. The investigation below tests it.

## Investigation steps

### Step A — Locate ActivateDeviceEx in NK and instrument it

ActivateDeviceEx is unnamed in the Ghidra project (Pass 9 verified —
search for `Activate` returns no functions). It must exist; device.exe
uses it to start each builtin driver from `HKLM\Drivers\BuiltIn`.

1. Use Ghidra MCP to find candidates:
   - `mcp__ghidra__list_strings` for `"ActivateDevice"`,
     `"\\Drivers\\BuiltIn"`, `"Init"`, `"DEVLOAD"`.
   - `mcp__ghidra__get_xrefs_to` on each found string —
     ActivateDeviceEx normally references the registry-path string.
   - Cross-reference with `mcp__ghidra__list_imports` if device.exe is
     analyzed too (`build-host/modules/04_device.exe.bin`).
2. Confirm by decompiling — the function should: take an
   `LPCWSTR registry-path` argument, open the registry key, read
   `Dll`/`Prefix`/`Index` values, `LoadLibraryW(dll)`, then call
   `<Prefix>_Init` via `GetProcAddress`.
3. Rename it (`mcp__ghidra__rename_function_by_address` →
   `ActivateDeviceEx_kernel_dispatch` or similar) and add a
   decompiler comment with the registry-path argument convention.
4. Add the address to `g_loader_watches[]` in `src/be300_probe.c`,
   following the existing pattern. Re-run probe and verify the log
   shows the registry-path strings of every driver loaded before the
   stall, confirming `cdm.dll` is in the chain.

### Step B — Locate and disassemble cdm.dll itself

cdm.dll is loaded by PCMCIA.DLL via LoadLibrary, but the binary is
not in the Ghidra project (which holds only nk.exe). Two paths to get
the binary:

- **From the WinCE FAT16 partition** (`All_nand_300.bin` sectors
  7584–32127 per CLAUDE.md restore-image table). Mount or extract:
  `tools/extract_xip_modules.py` already exists for module dumping;
  see if it covers FAT16 partitions or extend it. cdm.dll is likely
  in `\Windows\` or `\Program Files\PCMCIA\` on the FAT16 image.
- **From the BE-300 SDK** (one of the reference VMs available per
  CLAUDE.md "Reference & Documentation"). The SDK ships drivers for
  reference. If `cdm.dll` is included, the symbol-rich version is
  more useful than the on-NAND copy.

Once obtained, load into Ghidra (or analyze with
`mipsel-pe-objdump` in the `mips-dev` Docker container — see
CLAUDE.md "Docker Reverse-Engineering Environment").

Targets to disassemble:

- `DllMain`. Identify the `DLL_PROCESS_ATTACH` branch.
- `<Prefix>_Init` exports (commonly `CDM_Init`, `CMM_Init`, `PCC_Init`).
- Any worker-thread spawn (`CreateThread`) followed by
  `WaitForSingleObject` on the new thread's handle.
- Every read of a VRC4173 PCC register (`0x0F000200..0x0F0003FF`).

### Step C — Audit VRC4173 PCC0 emulation

Files: `src/be300_devices.c` (VRC4173 latch around lines 318–959),
`gxemul/src/devices/dev_vr41xx.c` (companion ICU/PMU emulation, but
PCC is more likely on the VRC4173 side).

1. Identify which PCC0 registers cdm.dll reads (Step B output) and
   what values they return today (default-zero from the catch-all
   latch).
2. Compare against `docs/hardware/hw_dump_vrc4173.txt` for real-HW
   "no card" snapshot values, particularly the PCCISR (interrupt
   status / card-detect bits).
3. Hypothesis verification: if real HW returns a "no card" status
   pattern in PCCISR and our emulator returns zero (which the driver
   reads as "all sources possible / try again"), the driver loops or
   spawns a worker. Implement the correct "no card" status return
   in the VRC4173 device emulation, citing the VRC4173 UM section.
4. CLAUDE.md section "VRC4173 And NAND Facts" warns: "VRC4173
   `SYSINT1REG` at offset `0x060` is read-only on real hardware";
   "Interrupt status registers in the VRC4173 status ranges use
   write-1-to-clear semantics in the emulator". Stay consistent with
   that pattern when adding PCC register semantics.

### Step D — T3 stack dump (mirror of T5)

If Step C alone doesn't resolve the deadlock, mirror the T5 stack-dump
work (already in `src/be300_probe.c` as `be300_probe_dump_t5_state`)
for T3 (`0x80FF7B90`). Same approach: dump 256 bytes of T3's thread
struct + scan for kseg0/kseg1 candidate pointers + dump 256 bytes from
each. Goal: confirm T3's call chain is exactly
`kernel-LoadLibrary → DllMain dispatch → PCMCIA's DllMain → spawn T5
→ WFSO(T5_handle)`.

### Step E — Update memory + handoff

After each step, append findings to
`memory/project_post_ppsh_stall.md` under a new "Pass 9.A/9.B/9.C"
header. When a fix lands, mirror to `MEMORY.md` one-liner.

## Critical files

- `src/be300_probe.c` — probe additions (Step A: ActivateDeviceEx
  watch; Step D: T3 stack dump). All probe changes remain uncommitted
  per existing convention.
- `src/be300_devices.c` — VRC4173 latch; Step C may add PCC0
  semantics here.
- `tools/extract_xip_modules.py` — Step B may extend this to FAT16.
- `docs/hardware/hw_dump_vrc4173.txt` — authoritative no-card PCCISR
  values for Step C.
- `docs/hardware/U14579EJ2V0UM00.pdf` — VRC4173 UM, PCC chapter, for
  Step C citations.
- Ghidra project (via `mcp__ghidra__*`) — Step A renames, Step B
  cdm.dll if loaded there.

## Verification (per step)

- **Step A done when:** `BE300_LIFECYCLE_LOAD func=ActivateDeviceEx
  arg="\\Drivers\\BuiltIn\\..."` lines appear in the probe trace, the
  last one before the stall is the PCMCIA / card driver, and the
  Ghidra rename + decompiler comment are persisted.
- **Step B done when:** cdm.dll DllMain and exports are
  disassembled, and the specific code path that spawns T5 (or that
  blocks on memmgr CS) is identified, with line numbers.
- **Step C done when:** VRC4173 PCC0 emulation returns hardware-
  accurate "no card" status, with citation to VRC4173 UM and
  hw_dump_vrc4173.txt. Re-run probe; check whether cdm.dll's DllMain
  early-exits and the LOAD log continues past hit 20.
- **Step D done when:** T3's call chain is documented in the
  memory file alongside T5's.
- **Step E done when:** memory + MEMORY.md reflect the resolution
  (or current state if not yet resolved).

## Coordination with the parallel HW-reset plan

This plan and the HW-reset-hypothesis plan are not mutually
exclusive. If the HW-reset investigation finds that NK *does*
program a watchdog and waits for it during the first-Starting phase,
the cdm.dll chain may be the *intended* code path (NK / device.exe
proceeding normally, watchdog scheduled to fire and trigger the
display handoff). In that case, fixing only cdm.dll would be
insufficient — the missing watchdog is the larger issue and Step C's
PCC0 audit might still be a real bug worth fixing on its own.

If the HW-reset investigation refutes the watchdog hypothesis, then
this cdm.dll chain *is* the root cause and Steps A–D are the path to
the fix.

Either way, Step A (ActivateDeviceEx instrumentation) and Step C
(VRC4173 PCC0 hardware-accuracy audit) are independently valuable.
