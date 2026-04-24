# Handoff — Diagnostic focus after Pass 31 / Addendum 11

**Date:** 2026-04-21
**State of tree:** no commits this session. `src/be300_probe.c` is
working-copy only (pre-Pass-33 probes preserved; session-added
Pass 33–47 probes removed as misdirected). No change to
`src/be300_devices.c`, `gxemul/`, or any committed file.

## 1. What the screen shows

The visible `Starting.bmp` (SHA `e8a8c83cd66b9327f50fc1827eada71fb028b332`)
was drawn by **OAL** during early cold boot. That is step 2 of the
real-hardware boot sequence in `CLAUDE.md`. The pixels on screen are
not the bug; they are the *starting point*.

## 2. What the bug actually is

The emulator never transitions from step 2 to step 3 of the
real-hardware sequence:

```
step 2: Starting... (OAL splash)    ← we are here, indefinitely
step 3: display blanks              ← never happens
step 4: Starting... (user-mode GDI) ← never happens
step 5: touchscreen-calibration UI  ← never happens
step 6: desktop                     ← never happens
```

Something on real hardware triggers the step 2 → step 3 transition
(display blank, then user-mode UI takeover). That trigger is absent
in the emulator.

## 3. What is confirmed working (do NOT re-investigate)

- ROM → SPL → NK → filesys → gwes cold-boot path is intact.
- gwes runs: WinMain fires, worker thread starts, message loop
  entered, 59 windows created, DC operations (brush/pen/font
  manipulation) fire throughout the run.
- ddi.dll is structurally complete: `DrvEnableDriver` populates the
  27-entry iFunc table; `DrvEnablePDEV` succeeds; ddi.dll calls
  `VirtualCopy` to map the framebuffer PA `0xAA200000` into user VA
  `0x001E0000`; `cached_pdev[0x6C]` stores that VA.
- VR4131 IRQs, VRC4173 latch, PIU, NAND, and Pass 31 KjCMU
  warm-reset wiring all work. No MMIO regressions.

The display driver / framebuffer stack is NOT where the bug is. 15
passes this session confirmed that exhaustively. The only artifact
from those passes worth preserving: *user-mode code never writes
to the framebuffer user-VA because no user-mode code ever tries* —
which means the problem is upstream of the display driver, not in it.

## 4. What is NOT confirmed — the real search space

The step 2 → step 3 transition on real hardware is orchestrated by
some combination of:

- A process spawn (NK's `CreateProcess` of an expected UI or
  display-handoff app)
- A kernel event signal (e.g., `EVENT_DISPLAY_READY` or
  `EVENT_GWES_INIT_DONE`) that unblocks a waiter
- A registry-driven launch entry (`HKLM\init\Launch<N>`) that NK's
  launcher dispatches
- An interrupt or timer (less likely, given the pattern is "the
  system is idle in a Wait")

None of these have been characterised in the emulator.

## 5. Existing probes that bear on this (pre-Pass-33, still in place)

- `spawn_module_createprocess_path` at `0x8008690C` — fires 7× in
  a 120 s run. **Unknown which 7 processes are spawned.** The
  probe's argument registers (a0/a1/a2/v0/ra) are captured but not
  decoded against the expected launch set.
- `launcher_module_ready_notify` at `0x80080D38` — fires 3×.
  **Unknown which 3 modules signal ready.** Likewise undecoded.
- `launcher_wait_loop` at `0x80080AA4` — fires 3×. The launcher
  enters a wait loop. **Unknown what it's waiting on.**
- `launcher_blocking_wait_call` at `0x80080CB4` — fires 3×. The
  `WaitForMultipleObjects`-style call inside the launcher.
  **Unknown which handles it waits on and whether any ever signal.**
- `gwes_message_loop_entry` (hits=1), `gwes_worker_thread_entry`
  (hits=1) — gwes enters these but we haven't characterised what
  it waits on inside the loop.

## 6. Next-step diagnostic targets (in priority order)

**Target A — Identify the 7 spawned processes.**
Modify the existing `spawn_module_createprocess_path` probe handler
(or add a new LIFECYCLE hook) to decode `a0` as a UTF-16 filename
pointer and log the image name for each of the 7 spawns. Compare
against the real-hardware expected launch set (`nk.exe`,
`filesys.exe`, `gwes.exe`, `device.exe`, `shell.exe`, likely one or
two more). **If a process is missing, that is almost certainly the
bug.** Expected finding: a UI or bring-up app (possibly
`BootInSafeModeWithPCC.exe`, `DevOSInstall.exe`, `Boot.exe`, or a
similar startup exe) does not appear in the 7 actually spawned.

**Target B — Identify the 3 ready-notifications and the
launcher's wait set.**
The launcher at `0x80080AA4` / `0x80080CB4` is a generic
module-readiness sync point. Dump its per-entry state (already
partially done by `be300_probe_dump_launcher_state` in the probe
file — review its output in any existing log, or re-run). Identify
which modules it's waiting on and which never signal. A module that
never signals ready is the proximate gate.

**Target C — Determine what event (if any) should cause the display
blank between step 2 and step 3.**
Real-hardware watch on MMIO regions is not available, but
introspection of NK's exception vectors and the OAL's display
dispatcher (`0x80078E10`, already probed) would reveal whether NK
itself is expected to blank the display before the user-mode UI
takeover. If so, find what triggers NK's blank call. If not, the
user-mode app is responsible and Target A is the root cause.

**Target D — Review registry launch entries (out-of-emulator).**
`HKLM\init\Launch*` in the BE-300 registry hive determines the
launch order. The dumped NK image contains the registry — extract
it and list the Launch entries to verify the expected boot
process is even present in the ROM.

## 7. What to avoid in the next session

- Do NOT re-investigate the display driver, framebuffer mapping,
  ddi.dll vtables, iFunc table, or cached_pdev fields. That stack
  is working. 15 passes confirmed it.
- Do NOT add probes at ddi.dll internals. The deletion of Pass
  33–47 probes was deliberate — they investigated the wrong layer.
- Do NOT assume the visible `Starting.bmp` is a paint-path failure.
  OAL already drew it; it is not being re-drawn by user-mode
  because user-mode has not taken over, not because the paint path
  is broken.

## 8. Committed ground truth to read first

In this order:
1. `CLAUDE.md` — emulation philosophy + real-hw boot sequence
2. `docs/HANDOFF_POST_PASS31_ADDENDUM11_VERIFIED_FACTS_ONLY_2026-04-21.md`
   — verified display-init facts (still correct and reusable)
3. `docs/wince_boot_reverse_engineering_report.md` — §2 Early
   Runtime Order and §4 Driver Activation Model, which describe
   the launch / ready-event pattern we need to trace
4. `docs/HANDOFF_POST_PASS30_BOOT_EXE_TRIGGERS_UNEMULATED_VRC4173_RESET_2026-04-20.md`
   and `docs/HANDOFF_POST_PASS31_KjCMU_reset_and_dyntrans.md`
   context — explains how Boot.exe's self-jal was resolved and
   what comes after

## 9. Suggested investigation shape

1 pass to run Target A. 1 pass (possibly combined) for Target B.
Depending on findings, Target C or D. The whole investigation
should take 2–4 passes, not 15. If it starts ballooning, stop and
reassess rather than drilling deeper into a specific dead-end.
