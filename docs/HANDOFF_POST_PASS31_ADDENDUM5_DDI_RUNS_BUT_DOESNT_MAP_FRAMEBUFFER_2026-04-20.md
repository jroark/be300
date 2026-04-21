# Handoff Addendum 5 — DDI.DLL's code runs but doesn't map the framebuffer

**Date:** 2026-04-20
**Corrects:** addendum 4's inference that "no code in DDI-family DLLs runs". That was wrong — the first-byte-of-.text probe missed DllMain.

## Finding

Sampled DDI.DLL's .text at 0x1000 intervals:

| Probe offset | Exec hits / 60 s |
|--------------|------------------|
| `0x01A51000` (+0)       | 0 |
| `0x01A52000` (+0x1000)  | 0 |
| `0x01A54000` (+0x3000)  | 4 |
| `0x01A58000` (+0x7000)  | 6 |
| `0x01A5C000` (+0xB000)  | **54** |
| `0x01A60000` (+0xF000)  | 0 |
| `0x01A64000` (+0x13000) | 0 |

Code IS running inside DDI.DLL. The hot spot at `0x01A5C000` (~1 Hz)
is consistent with a refresh-timer or WM_PAINT dispatcher.

Meanwhile `nk_loadlib_return_path` probe at NK `0x80090A78` confirms
142 LoadLibrary returns in 60 s, every `v0` a valid NK-space module
handle (e.g. `0x80fffea8`, `0x80fd5dc4`, `0x80fcbf18`). **Zero failure
returns observed.** The load chain succeeds, DllMain runs, and DDI.DLL
code is active.

Meanwhile the framebuffer-write probe from addendum 3 still shows:

```text
fb_topleft_kseg1  reads=0  writes=64  range=0xAA200000..0xAA200010
```

All 64 writes remain kernel-mode (SPL `0x80F037CC`, NK
`0xA0079130`/`0xA0079294`). Zero user-mode writes. So DDI.DLL's
drawing code is executing but NOT writing to PA `0xAA200000`.

## Interpretation

DDI.DLL has been loaded and is being driven (WM_PAINT / refresh-timer
callbacks), but it's drawing to somewhere other than the real
framebuffer. The most plausible cause is that the display driver's
framebuffer **physical-VA mapping** has never been established:

- WinCE 3.0 display drivers call `VirtualCopy` (or a kernel OEM hook)
  to map the physical framebuffer address into user-space. DDI.DLL
  would need to map `PA 0xAA200000` to a user VA so its drawing
  routines can `store` pixels.
- If that mapping fails silently or returns a different address, the
  driver's pixel writes go to a valid but wrong page (probably a
  zero-initialised heap page). The driver continues to function from
  a correctness standpoint — it thinks it's drawing — but nothing
  reaches the visible framebuffer.
- The 54 periodic hits at offset `0x01A5C000` match a hidden-buffer
  rendering loop: 1 Hz ticks consistent with a WM_TIMER cadence,
  minimal hits (54 rather than thousands) consistent with writes
  going to a cold buffer nothing flushes.

## Concrete Pass 32 attack

1. **Probe PA `0xAA200000` memory map in the emulator.** Verify the
   framebuffer is registered in a way that user-mode `VirtualCopy`
   can observe it. Check `gxemul/src/machines/machine_hpcmips.c`
   for framebuffer device registration and confirm it's exposed as
   a legitimate physical memory page (not just an MMIO device).
2. **Probe the `VirtualCopy` / `OEMMapMemoryAddr` call chain.** In
   coredll, `VirtualCopy` is a coredll export. Find its stub RVA,
   map to PSL trap, probe the NK-side handler to see what
   parameters user-mode passes. If DDI.DLL calls `VirtualCopy(dstVA,
   PA 0xAA200000, len, flags)` and the return is non-NULL, the
   mapping should be working. If it returns 0 / fails, that's the
   smoking gun.
3. **Probe DDI.DLL's write targets.** Instead of watching only PA
   `0xAA200000`, watch broad user-VA ranges near where DDI.DLL's
   allocated buffers would live (slot 0 heap, `0x00100000..0x00FFFFFF`,
   or slot 2 = gwes's process private area at slot 4
   `0x08010000..0x09FFFFFF`). If DDI.DLL writes lots of pixels but
   to the wrong VA, we'll see that cluster.
4. **Check BE-300 OEM hook layer.** WinCE 3.0 OEMs provide
   `OEMInit`, `OEMGetRealTime`, etc. A Casio-specific OEM hook could
   be the mechanism by which DDI.DLL requests a framebuffer mapping.
   Check if NK exports an OEM-area entry that isn't being called
   correctly in our emulator.

## Hypothesis ordering (refined)

1. **Framebuffer VirtualCopy/mapping returns wrong address.** DDI.DLL
   asks for the FB, gets a heap page, draws forever to the heap.
2. **Missing OEM hook.** A Casio-specific call that DDI.DLL expects
   to return the FB mapping isn't implemented.
3. **GDI display-sharing handshake.** Some other process (e.g. gwes's
   display thread) is supposed to flush DDI.DLL's back buffer to the
   real FB. That flush never fires.

## What's been ruled out (from addenda 2, 3, 4, 5)

- [ruled out] gwes WinMain stuck — message loop reached both boots
- [ruled out] DDI load chain fails — DllMain runs, code periodically fires
- [ruled out] `SignalBootReady` PSL never invoked — NK `0x80086884` is RefreshKernelAlarm
- [ruled out] `boot_trampoline_wfm_call` is a stall gate — it's the alarm scheduler
- [ruled out] filesys drives post-0x3B Launch — filesys has no Launch strings

## What's confirmed

- [confirmed] Pass 31 KjCMU warm-reset fix is correct
- [confirmed] gwes runs the message loop on both boots
- [confirmed] DDI.DLL is loaded and its code executes periodically
- [confirmed] `LoadLibrary` does NOT fail for DDI/ddstub/ddcore
- [confirmed] PA `0xAA200000` (framebuffer) gets zero user-mode writes

The stall is in the framebuffer-mapping handshake between DDI.DLL and
the emulator. Pass 32's proper target is the `VirtualCopy` /
OEMMapMemoryAddr code path, not any of the prior candidates.

## Working-copy state

`src/be300_probe.c` remains uncommitted per hygiene. It now contains:

- `fb_topleft_kseg1/kseg0` (framebuffer watches)
- `ddi_dll_text_entry` + `ddi_dll_text_*` (6 mid-range probes)
- `ddhel_dll_text_entry`, `ddstub_dll_text_entry`, `ddcore_dll_text_entry`, `ddraw_dll_text_entry`
- `nk_loadlib_return_path` (LoadLibrary return path)
- `gwes_*` (5 probes from addendum 2)

All re-usable for Pass 32's VirtualCopy investigation.
