# Handoff Addendum 3 — User-mode display driver never takes over the framebuffer

**Date:** 2026-04-20
**Supersedes:** the "display-driver handover" hypothesis (bullet 1 of the addendum-2 target list) is now the confirmed stall location.

## Finding

Added a framebuffer-write probe at PA `0xAA200000..0xAA200010` (top-left 16 bytes of the BE-300 display, per `docs/hardware/hardware.txt:193`). 60 s cold-boot run with `BE300_LIFECYCLE_PROBE=1` produced:

```text
[BE300_LIFECYCLE_SUMMARY] mem label=fb_topleft_kseg1  reads=0  writes=64  range=0xaa200000..0xaa200010
[BE300_LIFECYCLE_SUMMARY] mem label=fb_topleft_kseg0  reads=0  writes=0   range=0x8a200000..0x8a200010
```

All 64 writes come from **kernel-mode PCs only**:

- `pc=0x80F037CC` — SPL (base `0x80F00000`)
- `pc=0xA0079294`, `pc=0xA0079130` — NK at kseg1 alias of `0x80079xxx` (OAL display dispatcher region)
- `pc=0x80079130` — NK at kseg0 alias

**Zero writes come from user-mode** (no PCs in the `0x01xxxxxx` shared-DLL range or the `0x00010000..0x01FFFFFF` process-private code range). Every write value is `0x0000` — these are clear operations by OAL's display dispatcher at NK `0x80078E10` (reached twice, per `splash_caller_a060a0 hits=2`, consistent with one OAL splash render per boot).

## Interpretation

Gwes is running its message loop (Pass 32 addendum 2) but the user-mode display driver is not taking over the framebuffer. On real hardware, a loaded display driver would:

1. Load its DLL via `HKLM\SYSTEM\GDI\DRIVERS\DISPLAY` registry lookup (per prior Ghidra analysis of gwes at strings `0x04011A70`/`0x04011A98`).
2. Call `DrvEnableDriver` → allocate DDI contexts → map the framebuffer → begin drawing.

If any step fails silently, gwes keeps running (message loop is intact) but no pixel ever flows from user-mode to PA `0xAA200000`. The OAL splash remains visible because there's no one to overwrite it.

This matches the observed-HW step 2 → step 3 gap precisely:
- Step 2: OAL draws "Starting" → SPL/NK writers account for all emulator framebuffer writes
- Step 3 (missing): black-screen transition from display-driver handover — never happens because handover never happens
- Step 4+ (missing): user-mode render of "Starting" again — never happens

## What Pass 32 should investigate next

**Confirmed narrow target:** the path from gwes message-loop → display-driver `LoadLibrary` → `DrvEnableDriver`. Candidates for the display-driver DLL (from `build-host/modules/index.txt`):

- `62 ddi.dll` @ `0x01CA0000` — most likely (DDI = WinCE Display Driver Interface)
- `37 ddcore.dll`, `36 ddraw.dll` — DirectDraw support DLLs (2D acceleration layer)

Concrete steps:

1. **Add a probe at `ddi.dll`'s entry range** (runtime PC `0x01CA0000..0x01CB0000`). If never hit, gwes isn't loading ddi.dll at all. If hit, narrow to the DllMain → DrvEnableDriver path.
2. **Check `HKLM\SYSTEM\GDI\DRIVERS` registry population.** filesys processes `initobj.dat` into the runtime registry. If the GDI\DRIVERS subtree is missing, gwes has no driver to load. On BE-300 this subtree should map the `DISPLAY` key to `ddi.dll` (or Casio custom name).
3. **Check for fallback / stub behaviour in gwes.** If gwes can't find the display driver, it may silently proceed with a null-DDI stub that no-ops draws. That would match the symptom: gwes runs, pumps messages, but no pixels flow.
4. **Compare against real-HW:** `docs/hardware/hw_dump_memory.txt` has post-init memory dumps. If it contains PA `0xAA2xxxxx` content non-zero outside the top-left, the splash content is drawn there on real HW too — confirming the OAL splash is preserved until user-mode takes over. Our emulator should match this state at corresponding lifecycle points.

## Why this is the right target

- Pass 31 verified: the KjCMU warm-reset, ROM re-run, launcher advancing through 0x3B, gwes init, and gwes message loop all work.
- Pass 32 addendum 2 verified: gwes is alive and pumping messages.
- Pass 32 addendum 3 (this): user-mode never writes to the framebuffer. Only kernel-mode OAL does.

Everything between "gwes is alive" and "pixels flow from user-mode" is in the display-driver loading chain. Narrowing to a ~3-DLL candidate list with specific probes should pin the exact failure in 1-2 runs.

## Probe left in working-copy src/be300_probe.c

The `fb_topleft_kseg1`/`fb_topleft_kseg0` watches remain in the uncommitted working copy. Useful baseline: any future change that succeeds should produce many more writes from user-mode PCs (thousands per second of framebuffer refresh). Re-use the same probe to validate Pass 32's fix.

## Status of Pass 31 and Ghidra project

Unchanged. Pass 31 core fix (`dcab434b` + gxemul `09f835b`) stands. Ghidra renames from addendum 2 stand. NK rename of `0x80086884` to `RefreshKernelAlarm_kernel_side` still pending (do it when NK is the active Ghidra program).
