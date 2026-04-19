# Handoff — Post-PPSH WinCE Cold-Boot Stall

**Date:** 2026-04-18 (canonical draft, pruned)
**Branch:** `main`
**Submodule branch:** `gxemul/be300-minimal`

## Thesis

WinCE 3.0 cold boot stalls at **"Starting…"** because **the launcher never
receives a `SignalStarted(0x14)`-equivalent readiness notification for launch
ID `0x14` (`device.exe`)**, so `gwes.exe` (`0x1E`, depends `0x14`), `Boot.exe`
(`0x3B`, depends `0x14`, `0x1E`), and `coshell.exe` (`0x3C`, depends `0x14`,
`0x1E`, `0x3B`) never launch.

`device.exe` *does* spawn — threads 3, 4, 5 exist — but those threads are
internally deadlocked on memmgr CS `0x806695A0` (see §3). So the kernel API
call that would mark launch ID `0x14` ready is never reached. The launcher is
a downstream victim; fix device.exe's internal block and the launcher wakes
next pass.

The earlier framings "missing kernel iterator for the dispatch table at
`0x800B8E00`" and "launcher holds memmgr CS across WFM" are both obsolete —
see §8 for why and do not rechase them.

## Reproduce

```bash
cd build-host && cmake .. && make -j$(sysctl -n hw.ncpu)
BE300_LIFECYCLE_PROBE=1 gtimeout 30s ./be300 --nand ../ce/restore_images/All_nand_300.bin \
  > cold_stdout.log 2> cold_stderr.log
shasum screenshot_*.bmp
grep LIFECYCLE_CS cold_stderr.log        # stall-state CS snapshot
grep LAUNCHER_ENTRY cold_stderr.log      # HKLM\Init table contents
```

Expected: exit 124, screenshot SHA `e8a8c83cd66b9327f50fc1827eada71fb028b332`
(byte-identical to `./Starting.bmp`).

## 1. Threads at stall

Six threads, all blocked, enumerated via the `be300_run_batch` probe that
walks each thread's saved `s0` (= primary waiter address) and reads the
waiter's handle/type/owner fields:

| idx | Thread VA | EPC | Role | Waits on | Type |
|-----|-----------|-----|------|----------|------|
| 0 | `0x80FFF024` | `0x800819A4` | boot trampoline | boot event (handle `0x00FFFFC6`, obj `0x80FFC260`) | kernel-internal |
| **1** | `0x80FFC7CC` | `0x800819A4` | **launcher** (`FUN_800806BC` → `FUN_80080AA4`) | **launcher event (handle `0x00FFC9C6`, obj `0x80FFCA0C`)** | **HT_EVENT (4)** |
| 2 | `0x80FFCB54` | `0x800819A4` | NK-internal | obj `0x80FF7E2C` | kernel-internal |
| **3** | **`0x80FF7B90`** | `0x800819A4` | **device.exe** | thread 5 exit (HT_THREAD) — **holds memmgr CS** | HT_THREAD (1) |
| 4 | `0x80FE4000` | `0x80088230` | device.exe | memmgr CS sync `0x80FFF750` | CS-sync (0xFE) |
| 5 | `0x80FD592C` | `0x80088230` | device.exe | memmgr CS sync (same) | CS-sync (0xFE) |

## 2. Launch table at stall

The launcher parses `HKLM\Init\Launch##`/`Depend##` into an array at
`_DAT_8066AEEC` (count `_DAT_8066AEE8`, stride `0x250`), with ready flag at
`+0x04`. At stall, five entries, all `ready=0`:

| id | image | deps |
|----|-------|------|
| `0x0A` | `shell.exe` | — |
| `0x14` | `device.exe` | — |
| `0x1E` | `gwes.exe` | `0x14` |
| `0x3B` | `Boot.exe` | `0x14`, `0x1E` |
| `0x3C` | `coshell.exe` | `0x14`, `0x1E`, `0x3B` |

**`0x14` is the first broken link.** Once marked ready, gwes → Boot → coshell
unblock in sequence.

The kernel-side handler that marks a launch ID ready is
`FUN_80080D38` (`launcher_module_ready_notify_pulses_DAT_8066af00`):

```c
if (_DAT_8066AEEC != NULL) {
    if (param_1 == 0) { PulseEvent(_DAT_8066AF00); return; }       // generic
    for (entry in table) {
        if (entry[0] == param_1) { entry[1] = 1; PulseEvent(...); return; }
    }
}
```

It is installed at PSL method-table slot `0x80075570` (kernel API set; the
coredll-side `SignalStarted` export has RVA=0, patched at load to a syscall
trap — user-side PC watch impossible, use the kernel-side watch at
`0x80080D38`). Probe-confirmed: **`FUN_80080D38` fires exactly once with
`a0 = 0` early in boot** (from coredll at `ra=0x01F8F3E0`, before
`_DAT_8066AEE8` is populated). That early generic pulse cannot mark any
launch ID. **It never fires again with `a0 = 0x14`** because device.exe's
threads are all blocked before they can reach the call.

## 3. Root cause: lock inversion inside device.exe

Stall-state CS snapshot (one-shot dump at `boot_trampoline_wfm_call` hit=1
— i.e., at the moment thread 0, the last thread to park, enters its WFM):
of the 16 kernel CS structs from `FUN_80087F2C`, **exactly one is held**:

```
addr=0x806695A0 count=1 owner=0xA0FFC9EA sync=0x80FFF750 field_c=1 ← HELD
(all 15 others have count=1 owner=0 field_c=0, i.e. initialized but free;
 0x80669660 / 0x80669640 / 0x80669600 have count=0, never taken)
```

Owner `0xA0FFC9EA` is the kseg1 alias of handle `0x00FFC9EA`. Dumping the
handle-table entry at VA `0x80FFC9E8` yields:

| off | value | meaning |
|-----|-------|---------|
| `+0x14` | `0x80074C38` | classdesc = `THRD` (process-lifecycle §5.8) |
| `+0x18` | **`0x80FF7B90`** | **object VA = thread 3** (row 3 of §1) |

**Memmgr CS `0x806695A0` is held by thread 3 (device.exe).** Thread 3 is
parked in `WaitForMultipleObjects_yield` waiting for thread 5's exit (type 1,
HT_THREAD). Thread 5 is parked in `EnterCS_wait_calls_YieldToScheduler`
waiting for the CS thread 3 holds. Classic lock inversion, entirely within
device.exe.

Pulse-log corroboration: the earliest `EventModify_kernel_SetResetPulse` call
in the run is `(handle=0x00FFC9EA, op=3)` from `ra=0x8008F888` inside the
process-creation completion function `FUN_8008F790`. The same handle that
later appears as CS owner fired a completion pulse, so thread 3 is a process
created during the early spawn chain whose main body ran far enough to emit
one pulse and then parked on the thread-5-exit wait. It is not in the
priority queue (`_DAT_80669840 = 0`) and is not currently executing
(`_DAT_80669844 = 0`).

## 4. Why synthetic signals won't fix this

Tried and failed (see attempt log in §9); don't retry without new evidence.

- Pulsing the launcher event from an idle-context hook — no running thread to
  be the signaler, so the scheduler's wake callback (`LAB_800845D4`) never
  populates the WFM cleanup chain, and any resumed waiter faults in the
  cleanup loop at PC `0x80081A5C`.
- Force-waking thread 1 via raw MAKE_READY — same fault for the same reason.
- Force-waking thread 3 — it resumes briefly, cascade-wakes thread 5, thread
  5 faults trying to execute at `0x01A01BD0` (cdm.dll code) with RA in
  coredll `0x01F951AC`; confirms scheduler context save/restore is correct
  but also confirms there is no way to manufacture the right CS-release + 
  thread-exit sequence from outside a running kernel thread.

The fix must come from real CPU execution that un-inverts the lock inside
device.exe.

## 5. Next actions — narrowed to device.exe lock inversion

### A. Find the thread-3 code path that takes memmgr CS and waits on thread 5

Thread 3's EPC is `0x800819A4` (inside `WaitForMultipleObjects_yield`). Its
waiter holds thread 5's handle with type 1 (HT_THREAD → wait-for-exit). The
question: *what device.exe or coredll code sequence brought thread 3 to
take memmgr CS and then WFSO on thread 5's handle?* The thread-3 RA chain at
stall is the direct evidence. Operational:

1. Disassemble `build-host/modules/04_device.exe.bin` at its vbase
   `0x00010000`. Find the thread 3 entry (likely the module's primary thread
   entry — inspect the module's E32/O32 header for entry RVA).
2. Trace forward until it either (a) calls a coredll helper that takes
   memmgr CS, or (b) calls `WaitForSingleObject` on a thread handle. The
   pair of calls, in that order, is the bug.
3. Alternative / simultaneous: add a probe on `EnterCriticalSection_equivalent`
   (`0x800998C0`) filtered to `a0 == 0x806695A0`, logging RA each time.
   Capture the RA of the Enter that sticks (i.e., the last Enter before
   thread 3 blocks in WFSO). That RA names the device.exe / coredll caller.

### B. Audit priority inheritance in `EnterCS_wait`

If real hardware avoids this deadlock via priority inheritance (CS-holder
gets boosted to the waiter's priority when a higher-priority thread blocks
on the CS), gxemul may be missing it.

1. Re-read `EnterCS_wait_calls_YieldToScheduler` (`0x800880A8`) — does it
   update the holder's priority byte (thread_struct + 0x41) when enqueuing
   a waiter? If yes, the emulator's scheduler must honor the change.
2. Compare thread 3's priority byte against thread 4 and thread 5's at
   stall. If thread 3's is higher than the original spawn-time priority,
   the boost fired — check the emulator side. If not, real WinCE
   priority-inheritance is a genuine missing feature.

### C. Compare to real hardware boot ordering

The shipped WinCE image presumably boots on real BE-300 hardware, so this
lock-inversion must not happen there. Two plausible reasons the emulator
differs:

- Scheduling ordering: on real hardware, thread 5 runs to completion before
  thread 3 starts its WFSO. Emulator's tick cadence or priority queue
  ordering changes this.
- Environment / registry difference: device.exe takes a different branch
  based on something that reads as 0 / NULL on the emulator.

A useful discriminator: compare the `EnterCriticalSection` RA from (A) to
a real-hardware trace if one becomes available, or to the Platform Builder
reference tree.

## 6. Committable probe surface in `src/be300_probe.c`

Keep these — they're the minimal observer that established §1–§3 and will
verify any future fix.

- `g_exec_watches[]` — launcher family (`0x80080AA4`, `0x80080CB4`,
  `0x80080D38`, `0x8008130C`, `0x8008690C`), loader family (`0x8008FF00`,
  `0x800927CC`, `0x800929D0`, `0x800929E4`, `0x800BFA5Cu`, `0x01F84A5C`),
  boot-trampoline family (`0x80082248`, `0x80082300`, `0x80082560`,
  `0x800825C4`, `0x80082600`, `0x80082618`, `0x80082690`, `0x800826E0`,
  `0x80086884`), plus callback-registration watches (`0x80096094`,
  `0x80096620`).
- `g_mem_watches[]` — `boot_callback_gate` (0x80662AE0),
  `boot_ready_flag` (0x806694E0), `kernel_funcptr_table` (0x8066006C..88),
  `launcher_event_handle` (0x8066AF00), `boot_wait_event_handle`
  (0x806698CC), `boot_post_pulse_event_handle` (0x806697E8),
  `cs_memmgr_obj` (0x806695A0..B0), `cs_alloc_obj` (0x806697A0..B0),
  `coredll_oal_slot_65{44,48,4c,50,54}`, `coredll_desc_plus84`
  (0x80FFFF2C).
- `be300_probe_dump_all_cs_state(cpu, reason)` — one-shot dump of the 16 CS
  structs listed in `FUN_80087F2C`. Triggered from `note_exec` when
  `boot_trampoline_wfm_call` (PC `0x80082618`) fires hit=1 (= last thread to
  park = stall state). Produces `[BE300_LIFECYCLE_CS]` lines.
- `be300_probe_dump_candidate_thread_struct(cpu, base)` — 256-byte hex dump
  at any VA. Called with `0x80FFC9C8` and `0x80FFC9E8` from the CS-dump
  trigger to resolve the CS owner. Produces
  `[BE300_LIFECYCLE_THREAD_CANDIDATE]` lines.

For action A, add a filtered watch on `EnterCriticalSection_equivalent`
(`0x800998C0`) that only logs when `cpu->gpr[a0] == 0x806695A0`. That one
addition tells you who takes memmgr CS and from what RA.

## 7. Ghidra renames and comments

Persisted in the shared Ghidra project:

| Address | New name |
|---------|----------|
| `0x800816E0` | `WaitForMultipleObjects_yield` |
| `0x80081AD0` | `CreateEvent_kernel` |
| `0x80080AA4` | `launcher_main_loop_waits_DAT_8066af00` |
| `0x80080D38` | `launcher_module_ready_notify_pulses_DAT_8066af00` |
| `0x8008130C` | `EventModify_kernel_SetResetPulse` |
| `0x80086884` | `SignalBootReady_pulses_DAT_806698CC` |
| `0x80082300` | `boot_thread_trampoline_spawns_launcher_and_WFMs_0x806698CC` |
| `0x800862BC` | `SetThreadPriority_kernel` |
| `0x80082690` | `boot_trampoline_post_WFM_pulses_DAT_806697E8` |

Decompiler comments at `0x80080D38` (SignalStarted / PSL installation slot /
observed `a0=0` only), `0x80082300` (boot trampoline sequence), `0x80081A5C`
(WFM cleanup fault locus), `0x800816E0` (WFM-yield overview), `0x80080AA4`
(launcher overview).

## 8. What's NOT the bug (do not rechase)

- **Not "launcher holds memmgr CS across WFM".** CS snapshot proves owner is
  thread 3 (§3). Launcher's `FUN_80080AA4` never calls
  `EnterCriticalSection_equivalent` on `0x806695A0`.
- **Not an emulator CS-release-on-WFM bug.** `WaitForMultipleObjects_yield`
  and `YieldToScheduler_reschedule_loop` do not auto-release held CSes —
  that is real WinCE behavior. Other threads that need a held CS park via
  `EnterCS_wait_calls_YieldToScheduler`.
- **Not a missing kernel iterator over `0x800B8E00..0x800B8F80`.** That
  dispatch-triple table has zero code xrefs from any module (NK, coredll,
  filesys, gwes, device); it's orphaned legacy data. `FUN_80080D38` is
  reached via the PSL method table slot `0x80075570`, not that table.
- **Not a SYSINT / timer / IRQ gap.** Full 32-bit SYSINT sweep + Compare
  observation negative; no timer wakeup was ever requested (wait is
  `INFINITE`).
- **Not a user-mode SetEvent miss.** `FUN_80080D38` is the kernel-side
  handler; the coredll stub has export RVA 0 (kernel-patched).
- **Not a kernel scheduler context-save bug.** Force-wake resumes threads
  cleanly; faults in the cleanup loop are the expected consequence of
  bypassing the real pulse path.
- **Not a `LoadModule` / `GetProcAddress` / `_DAT_80662AE0` OEM hook gap.**
  `_DAT_80662AE0` is zero at boot on this image, and that path populates
  debug-print callbacks at `_DAT_80660070+` — unrelated to SignalStarted.

## 9. Appendix — event / waiter layout, supporting force-wake data

**Event object layout** (`CreateEvent_kernel` at `0x80081AD0`,
`FUN_800840B0` case 4):

| off | field |
|-----|-------|
| `+0x00` | packed header |
| `+0x04` | waiter list head (linked via waiter +0x04 / +0x10) |
| `+0x84` | alternate waiter head |
| `+0x88` | manual-reset flag byte |
| `+0x89` | signaled-state byte |
| `+0x8A` | auto-reset flag byte |
| `+0x8B` | priority high-water (init `0xFF`) |
| `+0x90` | fast-path extension (zero → slow path) |

**Waiter struct layout** (`WaitForMultipleObjects_yield` at
`0x80081848-0x80081894`): `+0x04` = next-in-object-queue, `+0x10` =
next-in-local-chain (cleanup), `+0x14` = handle (low 2 bits masked), `+0x18`
= type byte, `+0x19` = priority byte, `+0x1C` = owning thread, `+0x20` =
user-index. Thread's saved `s0` at WFM yield = primary waiter address — this
is how the §1 table was enumerated.

**Force-wake experiments (2026-04-18 afternoon, all failed — documented so
they are not re-run):**

| target | result |
|--------|--------|
| thread 1 via raw MAKE_READY | AdEL at PC `0x80081A5C` BVA `0x55800012` (expected — cleanup chain uninitialized) |
| thread 3 via raw MAKE_READY | TLBL at PC `0x80081A5C`; cascade-woke thread 5 which faulted fetching `0x01A01BD0` |
| thread 4 / 5 | re-block at EnterCS (CS still held) |
| pulse via `EventModify(event_obj, 3)` | event-address-vs-handle mismatch |
| pulse via `EventModify(handle, 3)` | hijacked from idle context, dereferences NULL `_DAT_ffffdac0` thread |
| memory-edit `event[+0x89]=1` + `_DAT_80669844=thread1` | same AdEL as raw MAKE_READY |

Summary: the scheduler's signal path only populates the WFM cleanup chain
when a running signaler thread yields through
`YieldToScheduler_reschedule_loop(&LAB_800845D4,…)`. In a full-deadlock
snapshot, no such thread exists, so no probe-driven signal injection can
work. The fix must be natural CPU execution — see §5.
