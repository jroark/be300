# Handoff Addendum — Correction to Pass 31: `boot_trampoline_wfm_call` is not a boot-ready gate

**Date:** 2026-04-20
**Supersedes §2.1 of:** `docs/HANDOFF_POST_PASS31_KJCMU_WARM_RESET_WIRED_2026-04-20.md`

## Correction

Pass 31's handoff (§2.1) called out the `boot_trampoline_wfm_call` at NK
`0x80082618` as the Pass 32 target, citing the Ghidra-renamed
`SignalBootReady_pulses_DAT_806698CC` at NK `0x80086884` as the
signaller that is never invoked.

**Both identifications are wrong.** The prior Ghidra rename was
speculative. Decoding the SH_WIN32 PSL method table in NK against
coredll.dll's export table confirms the kernel function at NK
`0x80086884` is **`RefreshKernelAlarm`**, not `SignalBootReady`.

### Proof

1. NK PSL table base: `0x800753D8` (SH_WIN32, descriptor at
   `0x80075670` with signature `"Wn32"` — agent analysis earlier
   this pass).
2. `0x80086884` occupies slot `0x80075488 = 0x800753D8 + 0xB0`,
   method index `0x2C` (44).
3. Trap-encoding formula derived from known slot ↔ trap pair
   (Pass 29: method `0x63` at `0x80075564` ↔ trap `0xFFFFFA76`):
   ```
   trap = 0xFFFFFC02 - method_id * 4
   ```
4. For method `0x2C`: `trap = 0xFFFFFC02 - 0xB0 = 0xFFFFFB52`.
5. Scan of `build-host/modules/01_coredll.dll.bin` for the MIPS
   literal `addiu $v0, $zero, 0xFB52` + `jalr $v0` finds exactly
   one stub at coredll VA `0x01F8DFC0` (function prologue at
   `0x01F8DF9C`, RVA `0xDF9C`).
6. coredll's PE export directory (file offset `0x5CE70`,
   `NumberOfFunctions=2053`, `NumberOfNames=1393`) has exactly one
   export whose function RVA equals `0xDF9C`:
   **`RefreshKernelAlarm` (ordinal 586)**.
7. Ghidra on coredll.dll confirms the sibling stub at coredll VA
   `0x01F8D910` is `SetKernelAlarm` (trap `0xFFFFFB8E`, method
   `0x2D` — the adjacent slot), and callers pair
   `SetKernelAlarm` + `RefreshKernelAlarm` in the standard WinCE
   "time changed → re-sort pending alarms" sequence at
   `SetLocalTime`/`SetSystemTime` wrapper functions
   (`FUN_01FA0C04`, `FUN_01FA0D10`).

### Implication

- NK `0x80086884` should be renamed `RefreshKernelAlarm_kernel_side`
  (or similar). Kernel global `0x806698CC` should be renamed
  `g_hKernelAlarmEvent`.
- The boot thread at NK `0x80082300` is **the kernel alarm
  scheduler thread** (or a thread that hosts alarm-wait logic), not
  the "wait for user-mode boot done" thread. WFM'ing forever on
  `0x806698CC` when no alarm is set is **expected behaviour** on
  real hardware, not a stall.
- There is no "boot ready" handshake missing. The earlier theory
  was a dead end.

## Where the real stall actually is

Still unresolved, but the correct framing is:

- Post-warm-reset, 15 processes are spawned
  (`spawn_module_createprocess_path` hits=15 total across both
  boots) but only 7 modules signal ready. 8 spawned processes are
  in some state (still initialising, stuck in DllMain, blocked on
  an event) and none of them signals ready.
- The launcher thread (`FUN_800806BC`,
  `launcher_reads_init_Launch_keys_spawns_filesys`) has processed
  the 4 modules it is responsible for (`0x00`, `0x14`, `0x1E`,
  `0x3B`) and presumably exited. Per WinCE 3.0 conventions,
  **filesys.exe's `InitializeLaunch`** drives subsequent
  `HKLM\init\LaunchNN` spawns (shell, welcome.exe, etc.), not the
  launcher in NK.
- Visible-progress expectation (`CLAUDE.md` §"Observed real-HW
  framebuffer sequence"): after OAL draws `Starting` splash, the
  display blanks (step 3), then user-mode graphics re-draws
  `Starting` (step 4). The emulator stays at step 2. That
  transition must be driven by gwes.exe + display driver, or by
  whichever process issues the PMU display-off sequence.

**Pass 32 should look at:**

1. **Which of the 15 spawned processes hasn't signalled ready.**
   Probes to add: entry probes at each Launch target's WinMain
   (welcome.exe, SafeShell.exe, Boot.exe/its child, etc.). Compare
   probe hit counts against 15.
2. **filesys.exe's `InitializeLaunch` loop.** Once NK + filesys
   are both imported into the Ghidra project, find the code that
   reads `HKLM\init\LaunchNN` sequentially and waits on each
   module's ready event. That loop, not the launcher in NK, is
   the driver of post-0x3B boot progress.
3. **gwes.exe's display-blank / user-mode graphics transition.**
   The emulator's screenshot still matches `Starting.bmp`
   byte-for-byte, meaning gwes never re-draws or issues the
   display-off that should happen between steps 2 and 3. Pass 22
   memory has relevant SIU/GWES context. Probe for MMIO writes to
   PMU display-off registers (VR4131 UM §12 PMU) during the
   post-reset window.

## Ghidra changes this pass

Committed in the coredll-side Ghidra project (persistent):

- `FUN_01F8DF9C` renamed `RefreshKernelAlarm_trap_FFFFFB52` with
  decompiler comment.
- `FUN_01F8D910` renamed `SetKernelAlarm_trap_FFFFFB8E` with
  decompiler comment.
- `FUN_01FA0C04` renamed `SetLocalTime_or_SetSystemTime_wrapper_A`
  with comment.
- `FUN_01FA0D10` renamed `SetLocalTime_or_SetSystemTime_wrapper_B`.

**Not yet done** (needed Ghidra project containing NK.exe):

- Rename NK `0x80086884` → `RefreshKernelAlarm_kernel_side`
- Rename NK `0x806698CC` data label → `g_hKernelAlarmEvent`
- Update decompiler comment on NK `0x80082300` to note that the
  WFM is the alarm scheduler, not a boot-ready wait.

## Status of Pass 31 primary fix

Unaffected by this correction. The KjCMU warm-reset trigger is
functionally correct:

- `[KjCMU] warm reset triggered` fires exactly once at
  `pc=0x8007a174`.
- ROM re-runs; NK re-enters; Boot.exe takes its already-init
  branch and signals `0x3B` ready.
- Launcher advances through its full Launch queue
  (`0x00` → `0x14` → `0x1E` → `0x3B`) post-reset.

The commit (`dcab434b`) and the gxemul-side companion
(`09f835b`) stand.
