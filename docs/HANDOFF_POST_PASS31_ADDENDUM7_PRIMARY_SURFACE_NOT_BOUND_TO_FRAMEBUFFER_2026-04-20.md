# Handoff Addendum 7 — DrvEnableDriver succeeds but primary surface isn't bound to the framebuffer

**Date:** 2026-04-20
**Extends and concludes:** addendums 3-6. Pass 32 now has the precise failure signature.

## The nail-down

Function-entry probes pinned inside ddi.dll (native vbase `0x01A50000`):

| Probe                          | PC         | Hits / 60 s | Args observed (ASID 4 = gwes) |
|--------------------------------|------------|-------------|-------------------------------|
| `ddi_DrvEnableDriver_export`   | `0x01A540B8` | 2           | `a0=0x00020001` (DDI_DRIVER_VERSION), `a1=0x6C` (sizeof DRVENABLEDATA), `a2=0x000B6A00` (ppdded, in gwes), `ra=0x00049D50` (gwes caller) |
| `ddi_DrvEnableDriver_impl`     | `0x01A5C43C` | 2           | same args (tail-call from export) |
| `ddi_blit_dispatcher_entry`    | `0x01A5BF00` | 27          | `a0=0x00140258..0x00141Bxx` (surface objects in gwes heap) |

### Interpretation

1. Gwes calls `ddi.dll!DrvEnableDriver` correctly on both boots with the
   canonical WinCE 3.0 signature. The impl populates the GDI callback
   table at `DAT_01A637F8..01A63828` and fills in `ppdded->pdrvfn`
   from the internal driver-function table.
2. Blits happen — 27 dispatches in 60 s — but every `param_1` is a
   surface pointer at `0x00140000..0x00142000` in gwes's process
   heap. No dispatch ever receives a surface whose backing is
   `PA 0x0A200000` (the BE-300 framebuffer).
3. Those 27 blits are gwes building its user-mode UI (dialog
   framebuffers, cursor cache, etc.) into off-screen memory.
   They never flush to the real display because the surface that
   would represent the screen never appears.

### Conclusion

**The primary surface (the one whose `pvBits` should point to
`PA 0x0A200000`) was never created, or was created with the wrong
backing.** In WinCE 3.0 GDI terms, this means one of:

1. **`DrvEnableSurface` (INDEX 4) was never called by GDI.** After
   `DrvEnableDriver` returns the function table, GDI normally
   calls the DDI function list in order: `DrvEnablePDEV` →
   `DrvCompletePDEV` → `DrvEnableSurface`. If any fail or return
   null, the pipeline halts before the primary surface exists.
2. **`DrvEnableSurface` was called but internally failed to map
   `PA 0x0A200000`.** It's the function that would call
   `EngCreateDeviceSurface` or similar, and set up a surface
   backed by `VirtualCopy`-mapped framebuffer memory. If the
   emulator doesn't correctly advertise `PA 0x0A200000` as
   physical memory to `VirtualCopy` / `OEMMapMemoryAddr`, this
   call returns NULL and GDI has no primary surface.
3. **Casio uses a custom OEM hook for framebuffer mapping.** BE-300
   is a Casio product, and the display-mapping path may go
   through a Casio-specific kernel extension rather than standard
   WinCE `VirtualCopy`. The hook may be missing in the emulator.

## Why ruling #2 in is likely correct

`gxemul/src/machines/machine_hpcmips.c` registers the framebuffer
as a `dev_fb` device at PA `0x0A200000`. `dev_fb` intercepts CPU
writes for rendering purposes but may NOT be registered as
"physical memory" from `VirtualCopy`'s perspective — i.e., when a
user-mode process asks the kernel "map PA `0x0A200000` to my user
VA", the kernel's memory manager may refuse because the address
isn't in its list of legitimate physical memory ranges.

## Concrete Pass 32 fix path

1. **Prove hypothesis:** probe NK's `VirtualCopy` implementation
   (PSL trap ~`0xFFFFFxxx`). Observe whether gwes (or ddi.dll's
   DrvEnableSurface) ever calls it with `PA 0x0A200000` as the
   physical address. If yes, check the return value. If NULL,
   that's the failure point.
2. **Look at gwes `FUN_00049D50 - 8` (or thereabouts).** That's
   the caller of `DrvEnableDriver`. Decompile the caller: it's
   the GDI init code. Does it proceed to call `DrvEnablePDEV`
   (iFunc 0) → `DrvEnableSurface` (iFunc 3) from the received
   function table? If it stops after DrvEnableDriver, find out
   why.
3. **If the emulator's `VirtualCopy` refuses `PA 0x0A200000`,
   extend `gxemul/src/cpus/memory_mips.c` or the relevant
   VirtualCopy-handler to permit mapping that PA range.** The
   standard WinCE VirtualCopy handler validates PAs against the
   OAL-provided memory map; if BE-300's OAL advertises the
   framebuffer as valid physical memory (check
   `docs/hardware/hw_dump_memory.txt` for any clues about the
   OAL memory map), add the range to the emulator's memory
   validation.

## What this session established (Pass 31 + 7 addendums)

- Pass 31 KjCMU warm-reset fix: committed `dcab434b`, gxemul
  `09f835b` — boot now passes Boot.exe.
- Gwes is alive; message loop reached.
- Ddi.dll is loaded, `DrvEnableDriver` runs, blit dispatcher
  runs.
- No DMA; blits target user-heap surfaces only.
- Primary framebuffer surface is never created or not bound to
  `PA 0x0A200000`.
- Next: trace the GDI init sequence past DrvEnableDriver (in
  gwes) and DrvEnableSurface (in ddi.dll) to find where the
  framebuffer mapping fails.

## Persistent Ghidra renames this pass

coredll+ddi program:

- `FUN_01A5C43C` → `DrvEnableDriver_impl`
- `FUN_01A5BF00` → `ddi_blit_dispatcher_1Hz_hotspot`
- Existing coredll renames from earlier addendums still stand.

## Working-copy probe set (uncommitted)

`src/be300_probe.c` now contains ~30 watches: `gwes_*`,
`fb_topleft_*`, `ddi_dll_text_*`, `ddi_DrvEnableDriver_*`,
`ddi_blit_dispatcher_entry`, `nk_loadlib_return_path`,
`vrc4173_dma_range_k1`, `vr4131_dmac_range_k1`, candidate
back-buffer VAs. All reusable for Pass 32's
VirtualCopy / DrvEnableSurface investigation.
