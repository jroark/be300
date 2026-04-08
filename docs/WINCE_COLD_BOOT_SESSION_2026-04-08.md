# WinCE NAND Cold-Boot Session Report

Date: 2026-04-08

## Goal

The goal of this work is to boot the BE-300 WinCE NAND image from a true
cold reset, using the real ROM, the real SPL, and the real NK image path,
without depending on synthetic resume state, stale SDRAM contents, or any
other warm-boot shortcut.

The practical success condition is not just "boot farther". It is:

- start at the ROM reset vector,
- let the ROM load SPL,
- let SPL decompress NK,
- let NK follow its real cold-start path,
- reach the post-kernel user-visible WinCE boot flow,
- and do all of that without seeding `resume_ctx` or relying on random RAM.

That requirement matters because the real BE-300 can boot from a fully
unpowered state. Any emulator path that only works when RAM already contains
useful resume data is not a valid cold boot.

## Session Summary

This session continued the WinCE cold-boot work from the point where earlier
investigation had already removed the old "seed `resume_ctx` and jump into
warm restore" crutch and pushed the cold path much farther into NK.

The major outcomes of this session were:

- the cold path remains firmly in the real cold-start flow rather than the
  old warm-resume trap,
- the low-power/scheduler path is now the dominant blocker,
- the user observed live progression from the `Initializing...` screen to the
  `Starting...` screen during this session,
- the latest headless timed runs still end in the same low-power loop at
  `0x80079990`, so the visible progress is real but not yet stable enough to
  survive to a clean captured exit,
- and the current blocker is now narrow enough to describe in terms of
  specific companion-chip registers and exact branch conditions.

## Working Theory

### 1. Cold boot must be self-contained

The baseline theory for the project is unchanged:

- the ROM, SPL, and NK cold-start code are sufficient to boot a real machine
  from a fully unpowered state,
- therefore the emulator should be able to do the same,
- and any requirement for pre-populated `resume_ctx` or arbitrary SDRAM state
  indicates either a bad emulator assumption or a wrong device model.

This theory has been reinforced, not weakened, by the session results.

### 2. The old warm-resume path was a diagnostic dead end

Earlier in the project, the cold boot kept falling into code that looked like
warm restore. That led to temporary `resume_ctx` seeding for diagnosis.

The work in this session continued the shift away from that approach. The
current traces show the emulator now reaches the true kernel cold-start chain:

- `0x8007B57C`
- `0x800947C8`
- `0x8008B21C`
- `0x8007A3FC`
- `0x80079898`

That is the important strategic result. The emulator is no longer blocked by
the old fake-resume dependency. The remaining problem is later and narrower.

### 3. The current blocker is a low-power / wake-source decision

The current theory is that cold boot now reaches the real kernel scheduler
and then stalls because the OAL/kernel low-power helper keeps choosing the
`SUSPEND` path and never sees a valid wake source.

The relevant function is `FUN_80079898`, which leads into the branch at
`0x80079910` and then into `COP0_SUSPEND` at `0x80079990`.

The decompilation and runtime traces now support the following model:

- `FUN_80079898` manipulates companion-chip wake latches around the low-power
  transition,
- it explicitly clears some latches under a specific condition,
- then it checks four branch inputs,
- and if all four are clear it takes the `SUSPEND` path.

The four currently relevant branch inputs are:

- `0xAF000046`
- `0x0A0008A0`
- `0x0A000A00`
- `0x0A00130C`

At the current failure point, they are all zero.

## Why The Session Moved The Project Forward

Even though the latest timed run still stalls, this session materially
improved the debug position in four ways.

### 1. The failure is now later than the old pre-init traps

Earlier cold-boot failures were still consistent with "wrong path selection
very early in OAL init". That is no longer the case.

The current path reaches:

- the true cold-start kernel entry,
- kernel init with `pTOC`,
- scheduler entry,
- and the idle/power helper.

That means the emulator already survives much more of WinCE boot than before.

### 2. The ram-size and board-strap path is no longer the main blocker

Two earlier fixes were important and remain valid:

- setting VR41xx `GIUPIODL` (`0xAF000144`) to the real hardware strap value
  `0xAAE2`,
- and seeding the NAND/VRC strap window so `0x0A00A0E0` drives the real RAM
  sizing decision.

Those changes moved boot past earlier failures:

- the old `0x80076FBC -> 0x80079460` path no longer dominates,
- the bogus >16 MB clear/copy behavior disappeared,
- and `PA 0x2554` now becomes `0x00000002`, matching the expected cold path.

### 3. The companion-chip latch state is now understood more accurately

One important lesson from this session is that the older VA-based probe reads
for some VRC4173 registers were misleading.

The new instrumentation added:

- explicit startup logging of the raw VRC4173 seed state,
- direct raw-latch reads for key wake registers,
- and side-by-side logging of both the MMIO-view values and the raw latch.

That exposed an important distinction:

- the seeded raw latch contains the expected hardware-like values,
- but some VA-based reads still report zero,
- so the raw latch is the trustworthy signal for these registers in the
  current device model.

This matters because it prevents chasing the wrong conclusion about "missing"
seed state.

### 4. The low-power helper logic is now concrete

By combining runtime logs, Ghidra, and objdump in the cross-tool container,
the low-power helper is no longer a black box.

The session established that:

- `FUN_80079898` is the low-power decision/helper function,
- `0x80079990` is `COP0_SUSPEND`,
- and `0x800A78E4` is a helper that writes `1` to:
  - `0x0A00112C`
  - `0x0A001120`
  - `0x0A001B20`

The helper writes are visible in the logs at:

- `0x800A78EC`
- `0x800A78F8`
- `0x800A78FC`

That gives a direct caller/callee bridge between the scheduler path and the
companion-chip wake-latch block.

## Process Used In This Session

The work in this session followed the same process that has been working for
the WinCE cold-boot effort in general:

### 1. Work backward from the actual failure PC

The session kept using the current stop site as the anchor, rather than
trying broad speculative fixes.

The main backward-walk chain was:

- observe the current failure PC and surrounding MMIO,
- identify the controlling function in Ghidra,
- disassemble the relevant address range in the cross-tool container,
- add narrow instrumentation around the exact registers/branches involved,
- rerun,
- and then move one step upstream.

### 2. Prefer narrow instrumentation over broad emulation changes

The session deliberately used small, targeted logs instead of large generic
trace floods. The instrumentation added here focused on:

- boot-path checkpoints,
- raw VRC4173 latch state,
- exact MMIO accesses to low-power wake registers,
- and idle/low-power transitions.

This matters because the WinCE boot path is timing- and state-sensitive, and
large intrusive tracing has a higher chance of perturbing behavior.

### 3. Use hardware dumps where the code is clearly reading straps or latches

Where a register was behaving like a board strap or persistent latch, the
session continued to align the model to captured hardware state instead of
guessing. That includes:

- `GIUPIODL` / `0xAF000144`,
- the NAND/VRC strap block around `0x0A00A0E0`,
- and the sparse companion wake/interrupt latch values around
  `0x0A001120`, `0x0A00112C`, `0x0A001B10`, and `0x0A001B20`.

### 4. Keep Linux as a regression guardrail

After the low-power and latch instrumentation changes, Linux 2.4 was rerun.
It still reached userspace under timeout, with:

- `Initializing RT netlink socket`
- `Starting kswapd`

and no WinCE-only probe noise in its stderr log.

That regression check remains important because the project has already shown
that apparently isolated CPU/IRQ changes can destabilize unrelated paths.

## Session Timeline

This is the practical arc that brought the project to the current state.

### A. Remove dependence on fake warm-resume state

The session continued from earlier work that had already removed the seeded
`resume_ctx` cold-boot workaround and confirmed that true cold boot should
not depend on RAM leftovers.

Relevant prior cold-boot milestones on `main` included:

- `581caaac` - instrument cold boot without `resume_ctx` seeding
- `1e210bd1` - trace the pre-init timeout path

### B. Fix board straps that were steering cold boot into the wrong path

Two strap-related fixes were key:

- `307866d2` - advance cold boot past the `GIUPIODL` timeout by using the
  real hardware strap value,
- `8611f24e` - seed the NAND/VRC strap block so NK sees the correct RAM-size
  decision and no longer clears or copies beyond the modeled SDRAM window.

After those fixes, the dominant failure moved to the later low-power path.

### C. Prove that the real cold path reaches kernel init and scheduler code

The next milestone was:

- `ce0ccaaf` - trace the cold path into kernel init and the suspend helper

That established that the current cold path reaches:

- `0x8007B57C`
- `0x800947C8`
- `0x8008B21C`
- `0x8007A3FC`
- `0x80079898`

This is the strongest evidence so far that the emulator is now executing the
real kernel cold-start path rather than a fake resume.

### D. Instrument the raw companion-chip latch state

This session’s new documentation-grade instrumentation was committed as:

- `9f95aef1` on the parent repo
- `98fd749` on the `gxemul` submodule

That added:

- startup logging of the VRC4173 seed state,
- raw-latch reads in the boot-path probes,
- MMIO watches for the low-power wake block,
- and more explicit CPU idle-path instrumentation.

## Current Status

### High-level status

The cold-boot path is significantly farther forward than before, but not yet
stable to a successful captured desktop boot.

The best short description of the current state is:

- the emulator is on the real cold path,
- it gets far enough that the user observed the `Starting...` screen live,
- but the current headless timed runs still loop in the scheduler/low-power
  path and exit with timeout before a screenshot can be saved.

### Exact current failure signature

The latest headless NAND run (`build-host/cold_stderr_idlefix3.log`) still
times out with the CPU parked at:

- `PC = 0x80079990`
- `EPC = 0x80079994`

This is the `SUSPEND` instruction in the low-power helper path.

### Current low-power state at the stall

At `0x80079990`, the important state is:

- raw latch `0x0A001120 = 0`
- raw latch `0x0A00112C = 1`
- raw latch `0x0A001B10 = 0x48`
- raw latch `0x0A001B20 = 0`

and the branch inputs checked by `FUN_80079898` are still zero:

- `0xAF000046 = 0`
- `0x0A0008A0 = 0`
- `0x0A000A00 = 0`
- `0x0A00130C = 0`

That combination explains why the code keeps selecting `SUSPEND`.

### Important new finding from Ghidra

The session produced a precise new interpretation of `FUN_80079898`:

- it is not merely waiting for an interrupt,
- it explicitly mutates companion wake/interrupt latches before deciding
  which low-power path to take,
- and it clears `0x1120` and `0x1B20` when:
  - `0x1134 == 0`
  - and `0x1B10 & 0x40 != 0`

That is important because it means the zero state of `0x1120` and `0x1B20`
at the failure point is not random. The guest itself is helping create that
state.

### Important new finding from the runtime logs

The session also established that the kernel later calls a helper at
`0x800A78E4` that repeatedly writes `1` back to:

- `0x112C`
- `0x1120`
- `0x1B20`

So the current picture is:

- one path clears `0x1120` and `0x1B20`,
- another path attempts to reassert them,
- and despite those writes the low-power decision still resolves to
  `SUSPEND`.

That is the most useful current clue.

## Evidence Worth Preserving

The following facts are the ones most worth carrying into the next session.

### Proven-good current facts

- Cold boot no longer depends on seeded `resume_ctx`.
- The path reaches kernel init and scheduler code.
- The user observed `Starting...` live during this session.
- Raw VRC4173 latch seeding is present at boot.
- Linux regression still works.

### Latest reliable file-level evidence

- Latest WinCE NAND stderr:
  [cold_stderr_idlefix3.log](/Users/jroark/src/be300-framebuffer/build-host/cold_stderr_idlefix3.log)
- Latest WinCE NAND stdout:
  [cold_stdout_idlefix3.log](/Users/jroark/src/be300-framebuffer/build-host/cold_stdout_idlefix3.log)
- Latest Linux regression stdout:
  [2.4_stdout_idlefix3.log](/Users/jroark/src/be300-framebuffer/build-host/2.4_stdout_idlefix3.log)
- Latest Linux regression stderr:
  [2.4_stderr_idlefix3.log](/Users/jroark/src/be300-framebuffer/build-host/2.4_stderr_idlefix3.log)

### Important caveat about screenshots

The timed test runs in this session all exited with `124` from `gtimeout`.
That means there was no clean process shutdown and therefore no saved
screenshot from those runs.

So the user’s observation of `Starting...` is important and should not be
discarded just because the saved screenshot mechanism did not capture it.

## What The Current Theory Says Should Happen Next

The next debugging step should not be another broad workaround. The current
state is narrow enough that the next work should trace the real branch inputs
backward.

The highest-value targets are:

### 1. Trace producers of the four low-power branch inputs

Specifically:

- `0xAF000046`
- `0x0A0008A0`
- `0x0A000A00`
- `0x0A00130C`

At least one of these likely differs on real hardware during the successful
cold path and should cause `FUN_80079898` to avoid the current `SUSPEND`
decision or to resume immediately afterward.

### 2. Trace the `0x1134` / `0x1B10` gating pair

The newly discovered clear condition depends on:

- `0x0A001134`
- `0x0A001B10`

Those now deserve targeted producer tracing because they directly explain why
`0x1120` and `0x1B20` become zero before the suspend instruction.

### 3. Follow the caller chain around `0x800A78E4`

The wake-helper writes are real and frequent. The next session should trace:

- who calls `0x800A78E4`,
- under what conditions,
- and whether the corresponding wake-source status bits are supposed to be
  asserted elsewhere in the same path.

### 4. Reconcile the VA-read path with the raw latch path

The raw latch now appears to be the correct ground truth for these companion
registers. The next session should determine whether the VA-based reads are:

- supposed to reflect those raw latch values directly,
- supposed to pass through some intermediate modeled logic,
- or currently bypassing the relevant latch state entirely.

This is not yet clearly a bug, but it is now clearly important.

### 5. Capture a clean post-`Starting...` exit if possible

Because the user has already observed `Starting...` live, a useful practical
goal for the next session is to get a run that:

- visibly reaches the same point,
- is stopped cleanly,
- and saves a screenshot or equivalent state snapshot.

That would make the current "transient progress versus final timeout" split
easier to reason about.

## Practical Next-Step Plan

If work resumes from this document, the next pass should do the following in
order:

1. Add one-shot tracing for reads and writes of:
   - `0x0A0008A0`
   - `0x0A000A00`
   - `0x0A00130C`
   - `0x0A001134`
   - `0x0A001B10`
2. Use Ghidra plus the cross-tool container to identify the caller chain for:
   - `FUN_80079898`
   - `0x800A78E4`
3. Compare those branch-input values to the available hardware dumps and any
   reachable diagnostic state on the real device.
4. Keep the Linux regression after each change.
5. Avoid reintroducing any `resume_ctx` seed or fake cold-start shortcut.

## Bottom Line

This session did not finish the WinCE cold boot, but it did move the project
from a broad "something in cold boot is still wrong" state to a much narrower
and more actionable statement:

the emulator is now on the true cold-start path, reaches the kernel scheduler
and low-power helper, and appears to be stuck because the low-power decision
sees no valid wake source in the companion-chip / VR41xx state currently being
modeled.

That is a better failure than the project had before this session, and it is
specific enough to drive the next round of reverse engineering.
