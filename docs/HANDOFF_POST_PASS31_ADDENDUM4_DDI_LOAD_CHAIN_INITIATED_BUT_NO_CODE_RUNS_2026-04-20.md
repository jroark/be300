# Handoff Addendum 4 — DDI.DLL load chain initiated but no code executes

**Date:** 2026-04-20
**Extends:** addendum 3 (user-mode display driver never takes over the framebuffer).

## Finding

Added exec watches at the first instruction of every DDI-family DLL in
`build-host/modules/index.txt`:

| Module | vbase | .text start | Pass 32 probe hits |
|--------|--------|--------------|-------------------|
| ddi.dll    | `0x01A50000` | `0x01A51000` | **0** |
| ddhel.dll  | `0x01B90000` | `0x01B91000` | **0** |
| ddstub.dll | `0x01BC0000` | `0x01BC1000` | **0** |
| ddcore.dll | `0x01BD0000` | `0x01BD1000` | **0** |
| ddraw.dll  | `0x01C20000` | `0x01C21000` | **0** |

60 s cold-boot run. Zero hits on every DDI-family DLL's first instruction.

Meanwhile the NK module loader's `loadlib_parse_filename` probe at NK
`0x80091C90` DOES show the load chain initiating:

```text
hit=133 ra=0x80090a78 arg="DDI.DLL"       <-- requested by gwes (or another user-mode process)
hit=135 ra=0x80090a78 arg="ddstub.dll"    <-- name pulled from DDI.DLL's .rdata at 0x01A52090
hit=136 ra=0x80090bbc arg="DDCORE.dll"
```

`a0=0x01a52090` for `ddstub.dll` is itself significant: it is inside
DDI.DLL's range (`0x01A50000..0x01A6A000`), specifically in what would
be DDI.DLL's .rdata import-name table. That tells us DDI.DLL's pages
**are mapped into memory** (the loader read its import table), but
none of DDI.DLL's code has executed. The kernel is resolving DDI.DLL's
imports (it imports ddstub.dll and DDCORE.dll), which is normal PE
loader behaviour before calling DllMain.

## Interpretation

One of two things:

1. **DDI.DLL's dependency chain fails before any DllMain runs.** The
   loader maps DDI.DLL → recursively tries ddstub.dll / DDCORE.dll →
   one of those dependency resolutions fails → LoadLibrary returns
   NULL → gwes sees the failure and either silently continues (no
   display driver installed, stub no-op path) or skips the
   display-subsystem bringup entirely.
2. **DDI.DLL's DllMain is somewhere past byte 0 of .text.** The
   `ddi_dll_text_entry` probe is at `0x01A51000` (the first byte of
   .text per the sidecar). If DllMain sits at a different RVA (PE
   entry point field — common), the probe misses the DllMain
   activation. However, *all* DDI-family DLLs probed show zero hits
   at first-byte-of-.text, which is statistically very unlikely if
   any of them had a running DllMain (their first function after
   .text start would usually be reached during module init).

Occam's razor: the load chain fails early. Likely missing dependency
or missing initialisation hardware in the emulator.

## Concrete Pass 32 attack plan

1. **Extend the exec-watch set to multiple PCs per DDI.DLL.** Add
   probes every ~0x1000 bytes inside DDI.DLL (e.g. 0x01A51000,
   0x01A52000, 0x01A53000, ... 0x01A69000). If ANY of these hit,
   DDI.DLL's code is executing and DllMain is somewhere in the
   module. If *all* of them are zero, no code in DDI.DLL runs.
2. **Probe NK's LoadLibrary return-path.** `0x80090a78` (the `ra` in
   the loadlib probe) is the instruction after the LoadLibrary call
   inside NK. Observe `v0` at that PC for each DDI-family load: if
   `v0 == 0`, LoadLibrary failed. Use `mcp__ghidra__decompile_function`
   on the caller to understand what NK does on failure.
3. **Check import-resolution failure path.** If DDI.DLL imports a
   function that ddstub.dll / DDCORE.dll doesn't export (or that
   doesn't exist in the emulator's coredll because of missing
   OEM hooks), the whole chain bails. Use coredll's Ghidra project
   to list what DDI.DLL needs.
4. **Check `HKLM\SYSTEM\GDI\DRIVERS\DISPLAY` registry value.** In
   `initobj.dat` (the pre-baked WinCE registry), what DLL is
   registered as the display driver? If it's a Casio custom name
   (e.g. `casiodisp.dll`, not in the module index), that DLL
   isn't present and LoadLibrary fails with FILE_NOT_FOUND.

## Hypothesis ordering (from most to least likely)

1. **Missing hardware dependency** — the BE-300 display driver
   requires accessing an MMIO register the emulator doesn't
   implement (e.g. a KjCMU display-clock register or an LCD
   controller config register). DllMain reads the hardware, finds
   a zero/wrong value, returns failure.
2. **Missing Casio custom display driver** — `HKLM\SYSTEM\GDI\DRIVERS\DISPLAY`
   points to a DLL that doesn't exist in our XIP dump (either
   because it's on the NAND filesystem and filesys hasn't mounted
   that partition yet, or because the Casio driver name is being
   looked up by alias).
3. **Import mismatch** — DDI.DLL imports a coredll function that
   exists in our coredll but has a wrong trap number, causing the
   recursive load to bail.

## Working-copy probes

Added to `src/be300_probe.c` (uncommitted per hygiene):

- `ddi_dll_text_entry @ 0x01A51000`
- `ddhel_dll_text_entry @ 0x01B91000`
- `ddstub_dll_text_entry @ 0x01BC1000`
- `ddcore_dll_text_entry @ 0x01BD1000`
- `ddraw_dll_text_entry @ 0x01C21000`
- `fb_topleft_kseg1/kseg0 @ 0xAA200000/0x8A200000` (from addendum 3)
- `gwes_*` probes (from addendum 2)

Re-running Pass 32 with `BE300_LIFECYCLE_PROBE=1` after adding
these gives immediate signal on which hypothesis to chase.

## Status of Pass 31 core fix

Unchanged. Pass 31's KjCMU warm-reset + dyntrans resync remain
functionally correct and committed.
