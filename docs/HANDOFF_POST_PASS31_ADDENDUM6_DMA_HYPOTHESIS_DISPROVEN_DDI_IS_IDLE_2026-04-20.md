# Handoff Addendum 6 — DMA hypothesis disproven; DDI.DLL is loaded but idle

**Date:** 2026-04-20
**Extends:** addendum 5 (DDI.DLL code runs but doesn't reach the framebuffer).

## The question

Could DDI.DLL be bypassing my CPU-store probe by using DMA? The
probe in `src/be300_probe.c` is wired to the dyntrans load/store
hook in `gxemul/src/cpus/cpu_mips_instr_loadstore.c`, which only
catches CPU stores. A memory-to-memory DMA from an off-screen
back-buffer to `PA 0x0A200000` would produce zero CPU writes at
that address while still driving the display.

## The test

Added probes on both DMA engines available on BE-300:

| Probe | Range (PA) | Writes / 60 s |
|-------|-----------|----------------|
| `vrc4173_dma_range_k1` | `0x0A001C00..0x0A002000` | **1** (SPL init zero, not runtime) |
| `vr4131_dmac_range_k1` | `0x0F000200..0x0F000280` | **0** |

Plus candidate back-buffer single-byte watches:

| Probe | VA | Writes |
|-------|-----|---------|
| `maybe_backbuf_at_2mb`  | `0x00200000` | 0 |
| `maybe_backbuf_at_4mb`  | `0x00400000` | 0 |
| `maybe_backbuf_at_8mb`  | `0x00800000` | 0 |
| `maybe_backbuf_at_16mb` | `0x01000000` | 0 |

## The answer

DMA is **NOT** in use. DDI.DLL is not programming either DMA engine,
not writing to the VRC4173 DMA control range (the 1 write observed
is SPL's one-shot init), and not bulk-writing pixels to any of the
tested user-VA candidate back-buffer locations.

## What this leaves us with

DDI.DLL is loaded and its code runs periodically (Pass 32 addendum
5: hits at `0x01A5C000` = ~1 Hz), but it is **not actively
rendering** by any mechanism I've tested. Candidate explanations:

1. **`DrvEnableDriver` was never called.** DDI.DLL's DllMain
   completed, but the chain from gwes → "enable the display" never
   fired. DDI.DLL's 1 Hz activity is a DllMain-spawned housekeeping
   thread or message-pump idle callback, not a drawing loop.
2. **Display is in "off" power-state.** WinCE 3.0 GDI has display
   power states (on / standby / suspend / off). If something
   initialises the display to "off" (because the user-mode splash
   transition hasn't fired), drawing is gated behind a "turn-on"
   event that never arrives.
3. **Drawing commands queue but don't dispatch.** gwes sends
   `WM_PAINT` messages into DDI.DLL's queue but the dispatcher
   isn't running. The 1 Hz code could be a queue-drain attempt
   that always finds the queue empty because messages go to the
   wrong target.
4. **Visual output goes via a different API.** BE-300's CASIO
   OEM layer might use a bespoke `OEMDrawXxx` hook instead of
   standard WinCE drawing. If that hook is NULL in our emulator
   build of NK, nothing draws.

## Pass 32 next concrete step

Dig into what `0x01A5C000` actually does. Decompile the function
that starts at or near that offset in DDI.DLL's Ghidra project
(DDI.DLL would need to be imported as a separate program, similar
to how coredll is imported — native `vbase 0x01A50000`, vsize
`0x01A000`, sections per `build-host/modules/73_... wait no`,
`build-host/modules/62_ddi.dll.*` per the index).

Once decompiled, three questions answer themselves:

- Is `0x01A5C000` a drawing-loop entry (`OEMDrv*` caller, framebuffer
  access) or a housekeeping function (heartbeat, idle callback, timer
  maintenance)?
- What does it branch on (i.e. "do drawing" path vs "skip" path)?
- Does it call `VirtualCopy` / `OEMMapMemoryAddr` to establish or
  refresh the framebuffer mapping?

Concurrently: **rename DDI.DLL's hot function in Ghidra** once
imported, so the next investigator can see the lifecycle picture.

## Status summary

- [confirmed] Pass 31 KjCMU fix works
- [confirmed] gwes runs message loop
- [confirmed] DDI.DLL loads (LoadLibrary returns valid handles)
- [confirmed] DDI.DLL executes periodic code (~1 Hz)
- [confirmed] No user-mode CPU writes to framebuffer
- [confirmed] No DMA programming (neither VR4131 DMAC nor VRC4173 DMA)
- [confirmed] No heavy bulk writes to tested user-VA back-buffer candidates

**Net:** DDI.DLL is in a quiescent / standby state. Pass 32 needs to
identify the event that should transition it to active rendering,
and why that event never fires.

## Working-copy probe state

`src/be300_probe.c` now contains (still uncommitted):
- `gwes_*` (5 probes) — WinMain/init/worker/window/message-loop
- `fb_topleft_kseg1/kseg0` — framebuffer write catch
- `ddi_dll_text_entry` + 6 mid-DLL sample points (`_1k`, `_4k`, `_8k`,
  `_c000`, `_10000`, `_14000`)
- `ddhel/ddstub/ddcore/ddraw_dll_text_entry`
- `nk_loadlib_return_path` — LoadLibrary return value observation
- `vrc4173_dma_range_k1`, `vr4131_dmac_range_k1` — DMA engine catches
- `maybe_backbuf_at_{2,4,8,16}mb` — candidate back-buffer VAs

Reusable for Pass 32's next iteration.
