# Session Handoff

Date: 2026-04-08
Branch: `main`

## Ground rules confirmed this session

- Do not use the host `objdump` for MIPS work. Use Ghidra or the `mips-dev` Docker container.
- `docs/nk_decompressed.bin` is for the WinCE 3.0 NAND image, not the `.NET` image.
- The correct `.NET` NK dump from this session is `build-host/nk_decompressed.bin`.
  - md5: `4f26163dfa3d995809ff0c8491bc33a1`
- Latest logs and run artifacts are in `build-host/`.
- Prefer real hardware behavior over emulator-only hacks. Temporary probes are fine, but do not leave workaround logic as the final fix.

## Major breakthroughs from this session

### 1. WinCE debug serial now works

- NK.exe debug output was going to the VR4131 DSIU at PA `0x0F000820`.
- That UART was previously unregistered, so all WinCE debug output was being dropped.
- Registering it as `ns16550` exposed the boot messages and made later debugging practical.

Observed output after the fix:

```text
Windows CE Kernel for MIPS Built on Apr 11 2001 at 15:23:09
PPSH Ctrl Err= 0 instead of 2320
InitializeJit
Exception 01a Thread=80fff024 PC=8007a4a8 BVA=ffffaf0c
Halting system
```

Relevant commits:

- main: `fb9d0234`
- gxemul: `52901c1`

### 2. PPSH ID check passes

- Seeded PA `0x0C000520` with `0x2320`, which matches the Casio MCU identification expected by the PPSH path.
- This moved the boot past the initial ID failure.
- The remaining PPSH issue is now MCU interaction/timeout, not the ID value itself.

Relevant commit:

- main: `8238400b`

### 3. The old WinCE 3.0 `$at` clobber panic was isolated

- Root cause: a timer interrupt hit between `lui at, ...` and the dependent `sw` in the idle path.
- The exception entry path that executed at that point clobbered `$at`, so return from exception produced a bad store to `0xFFFFAF0C` and a fatal TLB fault.
- This pointed to exception-entry fidelity rather than guest idle logic.

### 4. Guest exception entry was restored to the real guest path

- The emulator was moved back toward the real WinCE-installed vector flow instead of synthetic rewrites.
- After that change, the old `Exception 01a ... BVA=ffffaf0c / Halting system` signature no longer reproduced in the tested WinCE 3.0 NAND boots.
- The 3.0 image still stalls later, but not at the old `$at` corruption panic.

Relevant commits:

- gxemul: `453e528`
- main: `d5ba7b56`

### 5. The `.NET` NAND image became a useful higher-level reference

- The `.NET` image reproduced a later failure in `filesys.exe`:

```text
Main:Kernel address is A0029004
Windows CE Kernel for MIPS Built on Dec 16 2001 at 18:18:46
Exception 002 Thread=80ff8810 ...
Process 'filesys.exe'
```

- Live dumps and tracing showed the original `filesys.exe` fault was a real null dereference downstream of a WinCE service/API thunk.
- Treating WinCE negative `jalr` stubs as fetch-time `ADEL` instead of delay-slot faults moved the `.NET` image past that earlier crash.

Relevant commits:

- gxemul: `6fc948c`
- main: `5206b081`
- gxemul: `cb16261`
- main: `54f1a19b`
- gxemul: `deca086`
- main: `64ce12ce`

### 6. `.NET` now reaches a later kernel/device wait state

- After the negative-stub fix, `.NET` progressed beyond the old `filesys.exe` crash.
- Later tracing moved the blocker into a VRC4173/audio-facing path, then beyond that into the scheduler/low-power path.
- Narrowing the VRC4173 write-one-to-clear handling for `0x1100..0x113F` removed one emulator-side distortion and changed runtime behavior in a meaningful way.

Current relevant uncommitted source files from that later investigation:

- `src/be300_devices.c`
- `src/machine_be300.c`
- `src/wince_boot.c`

Do not assume those changes are final. They contain ongoing probes from the interrupted investigation.

## Screenshot state

- During this session, the `Starting...` screen briefly wrapped oddly in one live screenshot, but later returned to the expected layout.
- The later saved screenshot matched `Starting.bmp` exactly again.
- Several timeout-killed runs did not save a new screenshot at all.
- One recent run reported:

```text
[UI] No valid frame - cannot save screenshot
```

## Important artifacts and logs

### WinCE 3.0

- Base decompressed NK for analysis: `docs/nk_decompressed.bin`
- Key logs:
  - `build-host/cold_stderr.log`
  - `build-host/cold_stderr_120.log`

### WinCE .NET

- Correct decompressed NK for analysis: `build-host/nk_decompressed.bin`
- Key late-session logs:
  - `build-host/net_filesys25_stderr.log`
  - `build-host/net_filesys26_stderr.log`
- Useful dump pages:
  - `build-host/stuck_pc_page_00068000.bin`
  - `build-host/stuck_ra_page_00068000.bin`
  - `build-host/stuck_pc_page_00032000.bin`
  - `build-host/stuck_epc_page_00084000.bin`

## Current conclusion from the latest investigation

The current evidence points away from guest code and toward how GXemul wakes a halted VR4131 on compare/IP7.

What is now believed:

- The raw `WAIT` exit logic in dyntrans is conceptually correct.
- The failure is earlier: the compare wake source is not staying coherent between:
  - `compare_interrupts_pending`
  - `irq_compare`
  - `CAUSE.IP7`
  - the dyntrans `is_halted` path

Why this matters:

- In the bad `.NET` `WAIT` case, logs show `compare_interrupts_pending` already nonzero while `CAUSE.IP7` is still clear at the halt checkpoints.
- If `CAUSE.IP7` were set, dyntrans should leave `WAIT`.
- That means the backlog/accounting and the raw wake signal have drifted apart.

Strong evidence:

- `.NET` log: `build-host/net_filesys26_stderr.log`
  - repeated halt at `pc=0x80032880`
  - `cmp_pending=309`
  - `raw=0`
  - `cause=0x00000008`
- WinCE 3.0 log: `build-host/cold_stderr.log`
  - one `wake-exception` at `pc=0x80079990`
  - then immediate re-halt with:
    - `cmp_pending=33025`
    - `raw=0`
    - `cause=0x00000000`

Likely contributing GXemul issues:

- Compare is currently modeled as a queued backlog instead of a single latched event.
- `CAUSE.IP7` can be cleared by a guest `Compare` write while backlog remains nonzero.
- Compare rescheduling uses `new_compare - old_compare` logic, which is likely wrong for the guest's real wake deadline.
- GXemul's host timer base is only `65 Hz`, so compare callbacks can be batched/coalesced in ways that do not resemble a per-cycle CPU wake source.

## Recommended next steps

### 1. Fix compare/IP7 semantics in GXemul first

Do this before changing more guest-facing logic.

Priority:

1. Treat compare as a latched event, not an unbounded queued backlog.
2. Keep `CAUSE.IP7` coherent with that latched state until the guest acknowledges the event.
3. On `Compare` write, reschedule from `new_compare - current_count`, not `new_compare - old_compare`.
4. Re-test both WinCE 3.0 and `.NET`.

### 2. Use the existing logs as the acceptance baseline

Success condition for the next pass:

- a halted CPU at the WinCE idle `WAIT` path should either:
  - take the interrupt cleanly, or
  - leave `WAIT` because raw pending IP is visible
- but it should not sit with `cmp_pending > 0` and `raw=0`

### 3. Preserve the analysis split

- Use `docs/nk_decompressed.bin` for WinCE 3.0 analysis.
- Use `build-host/nk_decompressed.bin` for `.NET` analysis.
- Keep using Docker `mips-dev` or Ghidra for disassembly.

## Current workspace state

Tracked modified files:

- `src/be300_devices.c`
- `src/machine_be300.c`
- `src/wince_boot.c`

Untracked file that should not be committed accidentally:

- `FB_FIX_AND_CF_PLAN.md`

This handoff file was added to make the next session pick-up immediate.
