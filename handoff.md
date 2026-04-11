# Session Handoff

Date: 2026-04-09
Branch: `main`

## Major accomplishments this session

### 1. Fixed Compare/IP7 timer wake coherency (GXemul gxemul@42b9ff5)

The permanent WAIT halt bug is fixed. Three changes:
- compare_interrupts_pending: unbounded counter → latched 0/1
- Timer frequency: `new_compare - old_compare` → `new_compare - current_count`
- Dyntrans Count crossing check: unconditional (removed emulated_hz gate)
- Timer callback directly unhalts CPU (be300 main loop too slow to poll)

Both WinCE 3.0 and .NET now reach "Starting..." screen.

### 2. Identified root cause of "Starting..." stall

**GDI (API set 7) never registers because gwes.exe is blocked.**

Boot ordering chain:
1. filesys.exe starts → blocks (unknown reason)
2. gwes.exe waits for "FileSys ready" event → never gets it
3. GDI API set stays as placeholder (name="FILE", 0 methods)
4. Some process calls GDI method 5 → 1390 retries → all fail
5. Without GDI, display init fails
6. WaitForSingleObject(handle=65, INFINITE) blocks forever

### 3. Decoded WinCE PSL trap address encoding

From exception handler at 0x8008B794:
```
encoding = (EPC + 0x3FE) >> 2  (arithmetic shift)
api_set  = (-encoding >> 8) & 0x3F
method   = (-encoding) & 0xFF
```

Key decoded stubs:
- 0xFFFFDFEE = API 7 (GDI), method 5 — the 1390× retry loop
- 0xFFFFF7D6 = API 1 (handles), method 11 — WaitForSingleObject (final block)
- 0xFFFFE3FA = API 6 (WMGR), method 2 — Window Manager

### 4. Confirmed timer ISR works correctly

- Timer fires at ~5000 Hz via SIGALRM, delivers ~650 interrupts/sec through dyntrans
- Timer ISR at 0x800A78E4 runs, increments counters, returns v0=0 (no reschedule)
- v0=0 is CORRECT — no threads have active timers (all waiting on events, not timeouts)
- Scheduler idle function (FUN_8007A3FC) correctly enters SUSPEND

## Module table from NK.exe (95 XIP modules)

Boot-critical process order: nk.exe → coredll.dll → filesys.exe → gwes.exe → device.exe → shell.exe

Key drivers: ddi.dll (display), touch.dll, nanddisk.dll, NANDAccess.dll, pcmcia.dll, wavedev.dll

## What blocks filesys.exe?

**UNKNOWN — this is the next investigation target.**

filesys.exe is the first user process to run. It starts, makes 3 kernel API calls, then blocks. After it blocks, gwes.exe can't start, GDI is unavailable, and the boot stalls.

On WinCE 3.0, filesys.exe's initialization:
1. Creates named events
2. Initializes object store from RAM (ulRAMStart=0x80660000, ulRAMEnd=0x81000000)
3. Loads registry
4. Signals "SYSTEM/FileSys" event

Step 2 or 3 likely fails or blocks. The object store is in RAM, so no hardware dependency should exist for cold boot. But something in the emulator might prevent this from completing.

## Recommended next steps

1. **Add process tracking** — log which WinCE process is running when PSL calls happen (read process name from KData/PROCESS structure)
2. **Trace filesys.exe entry** — find its entry point from the E32 header and set a breakpoint
3. **Check object store init** — does WinCE write/read the RAM at 0x80660000-0x81000000 during object store creation?
4. **Check if a kernel event is expected** — filesys.exe might wait for a kernel signal that cold-start code doesn't send

## Important artifacts

- `docs/nk_decompressed.bin` — local ignored WinCE 3.0 NK dump copy refreshed from `All_nand_300.bin` (base VA 0x80060000, SHA-256 `df55a2f89c3c9635d0cf4f8bf73fa32ced70e7137fb00fcf7872b70a77f0b15f`)
- `build-host/nk_decompressed.bin` — refreshed on WinCE NAND boots only, not Linux boots
- Latest logs in `build-host/`

## Current workspace state

No uncommitted changes. All probes committed and pushed.
