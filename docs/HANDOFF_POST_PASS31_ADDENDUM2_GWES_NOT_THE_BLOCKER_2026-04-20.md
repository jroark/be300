# Handoff Addendum 2 — gwes is NOT the Pass 32 blocker

**Date:** 2026-04-20
**Supersedes the gwes-stall speculation in:** previous addendum §"Where the real stall actually is" bullet 3.

## Finding

Direct instrumentation with probes at gwes's key init landmarks (added to `src/be300_probe.c` working copy, uncommitted per hygiene) proves gwes reaches its main message loop on BOTH boots (1st pre-reset, 2nd post-reset):

```text
gwes_winmain_entry       hits=8   @ 0x00016394  (multi-ASID; includes other processes whose .text is at same offset)
gwes_last_init_entry     hits=2   @ 0x00034b68  (FUN_04034b68, runs once per boot)
gwes_worker_thread_entry hits=2   @ 0x000348d4  (worker thread runs once per boot)
gwes_window_create_entry hits=59  @ 0x0002a4c4  (CreateDialogFromResource, ~30 per boot -- dialogs building)
gwes_message_loop_entry  hits=2   @ 0x00035928  (main message loop, 1 per boot)
```

ASIDs on the gwes-specific hits (`last_init_entry`, `worker_thread_entry`, `message_loop_entry`) are `0x00034804` / `0xb4004` = ASID `4`, consistent with gwes being process/slot assignment 4.

**Interpretation:** gwes.exe WinMain completes its full init chain (FUN_04034b68 creating events A/B/C and spawning the worker, the worker signalling event_B, the main thread unblocking and reaching `_DAT_000b41f8 = 1`), and enters the main message pump. Dialogs are being created (59 hits on `CreateDialogFromResource`). The system is not blocked in gwes.

## What this refutes

The previous addendum pointed at "gwes display-blank → user-mode redraw" as a stall candidate. That was speculation; the probe data disproves it — gwes is alive and processing messages. The window-create function at `0x0402a4c4` is not the blocker either (fires 59x). Ghidra labels updated to match:

- `0x04034b68` → `gwes_last_init_REACHED_OK_Pass32`
- `0x040348d4` → `gwes_worker_REACHED_OK_Pass32`
- `0x0402a4c4` → `gwes_CreateDialogFromResource_NOT_the_blocker`
- `0x04035928` → `gwes_main_message_loop_REACHED_OK_Pass32`

## What Pass 32 should target next

The real-HW "step 2 → step 3" transition (OAL splash → black screen → user-mode redraw) is not a single function call in gwes. It depends on:

1. **Display driver handover.** WinCE 3.0's OAL writes the splash via `dev_fb` at PA `0xAA200000`. The user-mode display driver (loaded by gwes via `HKLM\SYSTEM\GDI\DRIVERS`) is supposed to take over the framebuffer. If the display driver's `DllMain` or `DrvEnableDriver` blocks, the OAL splash remains visible even though gwes is running.
2. **PMU/backlight cycle.** CLAUDE.md §"Observed real-hardware framebuffer sequence" describes the backlight dimming and brightening between step 2 and step 4, suggesting PMU writes drive the visible transition. These are likely VR4131 PMU register writes (PA `0xAF0000C0..CC`) that the emulator latches but doesn't visually reflect.
3. **A missing input event.** gwes's message loop is running. If no input (touch, key) is ever delivered by the emulator, gwes never advances to the touch-calibration prompt (step 5).
4. **An inter-process signal.** gwes signalled `0x1E` ready, but some OTHER process may be waiting for a follow-up signal from gwes that isn't firing. The 8 spawned-but-never-ready processes could be waiting on this.

**Concrete Pass 32 actions:**

- Add MMIO probes on PA `0xAA200000..0xAA2FFFFF` (framebuffer) and PA `0xAF0000C0..0xAF0000CC` (PMU) to see what gwes writes after reaching the message loop. If the framebuffer is being written, the display transition IS happening — the issue is only that SDL's screenshot-on-shutdown captures the pre-transition state. If nothing is written, gwes hasn't issued a display redraw.
- Add a probe on each spawned process's `WaitForSingleObject` or `WaitForMultipleObjects` call site. Which of the 8 unsignaled processes is waiting, and on what handle? That waiting process is downstream of the real gap.
- Check whether touch events are being delivered. On real HW, touch calibration prompt = step 5. Emulator never injects touch events automatically; the user can click via SDL but no synthetic touch is generated during boot. If gwes is waiting for a touch event to dismiss the calibration dialog, it will wait forever in the emulator.
- Probe the display-driver's `DrvEnableDriver` entry. If it never fires, the OAL splash persists.

## Status of Pass 31 core fix

Unaffected. KjCMU warm-reset is functionally correct; Pass 31 remains committed at `dcab434b` + gxemul `09f835b`.

## Ghidra project state (persistent)

- NK program: pending NK rename of `0x80086884` to `RefreshKernelAlarm_kernel_side` (agent attempted but the MCP wasn't on NK; re-do by switching to NK program).
- coredll program: renames from previous addendum still stand (`RefreshKernelAlarm_trap_FFFFFB52` at `0x01F8DF9C`, etc.).
- gwes program: renames from this pass as listed above.
- filesys program: no renames from this pass (brief investigation confirmed filesys is not the post-0x3B Launch driver — it has no Launch/BuiltIn strings).
