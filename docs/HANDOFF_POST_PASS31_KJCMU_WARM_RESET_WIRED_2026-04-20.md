# Handoff — Pass 31: KjCMU warm-reset trigger wired; Boot.exe advances; new stall at SignalBootReady PSL gate

**Date:** 2026-04-20
**Branch:** `main`
**Commits:**
- Main: `dcab434b` "Pass 31: wire VRC4173/KjCMU warm-reset trigger so Boot.exe reboot advances"
- GXemul submodule (`be300-minimal`): `09f835b` "mips: resync dyntrans next_ic after mips_cpu_cold_reset()"
**Supersedes:** `docs/HANDOFF_POST_PASS30_BOOT_EXE_TRIGGERS_UNEMULATED_VRC4173_RESET_2026-04-20.md`

## Thesis

Pass 30 root-caused Boot.exe's stall to NK's `FUN_8007A140` writing
`7 → PA 0x0A00A0C4` + `10 → PA 0x0A00A0C8` then falling through into
the only self-jal in 6 MB of NK at `0x8007A178`. Real silicon
warm-resets the CPU on those writes; our emulator silently latched
them. Pass 31 teaches `DEVICE_ACCESS(be300_nand)` to intercept the
magic-value pair and call `mips_cpu_cold_reset(cpu)`.

A companion gxemul-side fix to `mips_cpu_cold_reset` was required:
the function set `cpu->pc = 0xBFC00000` but did not re-resolve
dyntrans's `next_ic` pointer from the new PC. Without that resync,
the dyntrans engine continued walking its stale translated page,
producing 448,307 `non-existant paddr` log lines in 60 s. Fix
mirrors `mips_cpu_exception:2103-2108`.

Boot now advances past the self-jal. Real-HW sequence from
`CLAUDE.md` is approximated: 1st-boot `Initializing` → `Starting`
splash rendered, Boot.exe runs init, writes `Initialized.$$$`,
issues KjCMU warm reset → ROM re-runs → NK re-enters → Boot.exe
takes the already-initialised branch on its 2nd invocation and
signals module `0x3B` ready.

**New stall surface:** the boot thread's trampoline at NK `0x80082300`
creates an event at kernel global `0x806698cc` before spawning the
launcher and blocks on `WaitForMultipleObjects(1, &event, FALSE,
INFINITE)` at `0x80082618`. The only signaller of that event reachable
from normal-path boot is `SignalBootReady` at NK `0x80086884`, which
is a PSL-dispatch-slot entry — it can only be invoked from user-mode
via a kernel-trap call number. In the Pass 31 run that PSL is never
invoked, so the WFM parks forever.

This is the Pass 32 investigation target.

## 1. Evidence

### 1.1 KjCMU warm-reset fires correctly

```text
[KjCMU] warm reset triggered at pc=8007a174 (PA 0x0A00A0C4<=7, PA 0x0A00A0C8<=10)
```

Exactly one hit per run. PC `0x8007A174` matches the `sw 10, 0xC8(a0)`
instruction at NK `FUN_8007A140 + 0x34`.

### 1.2 Dyntrans resync works

Before the gxemul commit:

- 448,307 `[ memory READ: from non-existant paddr=0x000000001c07a178+... ]`
  lines in 60 s, linearly walking PA `0x1C07A178 → 0x1C22FE18` (≈1.79 MB).

After the gxemul commit:

- 10 total stderr lines in 60 s. CPU correctly resumes from
  `0xBFC00000`.

### 1.3 Post-reset progress (from `BE300_LIFECYCLE_PROBE=1` summary)

```text
[BE300_LIFECYCLE_SUMMARY] exec label=boot_trampoline_entry               hits=2
[BE300_LIFECYCLE_SUMMARY] exec label=boot_thread_main                    hits=2
[BE300_LIFECYCLE_SUMMARY] exec label=boot_trampoline_spawn_launcher       hits=2
[BE300_LIFECYCLE_SUMMARY] exec label=boot_trampoline_wfm_call             hits=4
[BE300_LIFECYCLE_SUMMARY] reset label=rom_reset_vector                    hits=2
[BE300_LIFECYCLE_SUMMARY] reset label=oal_display_dispatcher              hits=2
[BE300_LIFECYCLE_SUMMARY] reset label=splash_caller_a060a0                hits=2
[BE300_LIFECYCLE_SUMMARY] exec label=launcher_module_ready_notify         hits=7  (3 pre-reset + 4 post-reset)
[BE300_LIFECYCLE_SUMMARY] exec label=launcher_blocking_wait_call          hits=6  (3 pre-reset + 3 post-reset)
```

Post-reset launcher sequence (from stderr near the KjCMU marker):

```text
launcher_blocking_wait_call a2=0x00000014  =>  module_ready_notify a0=0x14
launcher_blocking_wait_call a2=0x0000001e  =>  module_ready_notify a0=0x1e
launcher_blocking_wait_call a2=0x0000003b  =>  module_ready_notify a0=0x3b (Boot.exe)
(no 4th launcher_blocking_wait_call — sequence ends)
```

### 1.4 Stdout — NK re-runs cleanly after reset

```text
CASIO Compress File System Device Driver Ver.0.19.00 : 80f221d4 00000001 00000000
CASIO Original Oomui Initialize (Ver1.00.02).
...
A007B398                                      <-- post-reset kernel re-entry
Windows CE Kernel for MIPS Built on Apr 11 2001 at 15:23:09
InitDebugEther
...
Exception 003 Thread=80ff7b90 Proc=80fe536a
AKY=00000009 PC=019a3d14 RA=019a3bf8 BVA=00311000
Process 'device.exe'
CASIO Compress File System Device Driver Ver.0.19.00 : 80f241f8 00000001 00000000
CASIO Original Oomui Initialize (Ver1.00.02).
(silence — 2nd-boot stall)
```

The `Exception 003` at PC `0x019A3D14` is an SH (`sw $t0, 0($t9)`) in
nanddisk.dll (vbase `0x019A0000`, probe label
`nanddisk_blank_bat_store`). The probe fired 42 times while the
exception printed only 2 times — the TLB refill handler services
40/42, the remaining 2 surface as user-unhandled exceptions. The
killed thread does not prevent subsequent Compress FS + Oomui
driver init messages on either boot — this exception is **not the
stall cause**, it is a pre-existing consistency issue.

### 1.5 Screenshot still byte-matches `Starting.bmp`

```text
md5(Starting.bmp)                    = 4ef8a9458952d2cc9b76e7a8266ff32e
md5(screenshot_20260420_195140.bmp)  = 4ef8a9458952d2cc9b76e7a8266ff32e
```

Expected: display-blank + second `Starting...` render are downstream
of the SignalBootReady gate (observed-HW step 3 → 4 per
`CLAUDE.md` §"Observed real-hardware framebuffer sequence").

## 2. What Pass 32 should look at

### 2.1 Primary target: SignalBootReady PSL never invoked

**Where:** NK `0x80086884` (`SignalBootReady_pulses_DAT_806698CC` per
Ghidra). Only caller paths:

- PSL dispatch slot at NK `0x80075488` (DATA xref only; must be
  invoked from user-mode via a kernel-trap call number).
- Secondary: `FUN_800A2F88 + 0xB8` (= `0x800A3040`), a rare
  VM-pressure branch — not a normal-boot signaller.

The probe `signal_boot_ready_pulser` at pc=0x80086884 **never fires** in
either the pre- or post-reset window.

**What Pass 32 needs to find:**
1. Which user-mode application is supposed to invoke the
   SignalBootReady PSL after basic boot? Candidates: gwes.exe,
   shell.exe (module 6 per `build-host/modules/index.txt`),
   explorer/the Casio custom shell.
2. What trap number/coredll stub exposes SignalBootReady to
   user-mode? Search coredll.dll (module 1) for PSL thunks that
   pulse kernel event handles.
3. Why doesn't that user-mode caller invoke the PSL in our run?
   Is the relevant process not being spawned, not reaching the
   call site, or stuck on a prerequisite?

Given the launcher processes modules through `0x3B` (Boot.exe)
and then idles without issuing a 4th wait, the expected sequence
is that Boot.exe's 2nd-invocation post-return path (or a sibling
process kicked off by Boot.exe) should invoke SignalBootReady.

### 2.2 Secondary target: Exception 003 at nanddisk.dll + 0x3D14

Not a stall, but a consistency issue worth capturing. BVA
`0x00311000` is in device.exe's user-heap region; 40 out of 42
reaches of that PC execute successfully via TLB refill, 2 do not.
Root cause probably resolves naturally once the main stall is
fixed (the killed thread's work gets picked up elsewhere), but if
it persists past Pass 32, investigate what backs VA `0x00311000`
in device.exe's address space (probably a nanddisk.dll scratch
buffer whose mapping isn't established before the access).

## 3. Cited hardware references (Pass 31 fix)

- `docs/hardware/hardware.txt:192` — "KjCMU Base on companion"
  identifies PA `0x0A00A000..` as a Casio custom block, not a
  standard NEC register map (NEC VRC4173 UM does not describe
  KjCMU).
- `docs/hardware/hw_dump_vrc4173.txt:524` — real-hw quiescent
  values `0x0A00A0C0..C8 = 0x7100, 0x00000003, 0x0000FFFF, 0x0`,
  distinct from NK's `7`/`10` write magic, consistent with
  trigger-then-quiescent register semantics.
- Pass 30 §1.1 — only self-jal in 6 MB of NK sits immediately
  after the two writes at `0x8007A178`.
- TODO 2026-04-20 in the code comment: confirm via a located
  KjCMU register map (Casio internal doc or undisclosed NEC
  annex) when available.

## 4. Files modified

- `src/be300_devices.c` — added `cpu_mips.h` include; added the
  KjCMU interception block at the top of `DEVICE_ACCESS(be300_nand)`
  with full citation chain comment (52 lines added). Functional
  change only; no probe code, no diagnostics.
- `gxemul/src/cpus/cpu_mips.c` — added `mips32_pc_to_pointers(cpu)`
  /`mips_pc_to_pointers(cpu)` call at the end of
  `mips_cpu_cold_reset` (15 lines added). Tenth delta commit on
  top of pristine GXemul 0.7.0 (the nine enumerated in
  `CLAUDE.md` §"GXemul Submodule Hygiene" do not include this
  one yet — the next edit to that paragraph should bump the
  count).

## 5. Explicit non-goals / scope boundary

- **Not investigating the SignalBootReady gate.** Pass 32 work.
- **Not investigating Exception 003 at nanddisk.dll + 0x3D14.**
  Probably secondary; characterised for reference (§2.2).
- **Not bumping `CLAUDE.md` delta-commit count from "nine" to
  "ten".** Leaving to the next routine `CLAUDE.md` revision so
  it's not mixed with Pass 31 mechanics.

## 6. Instrumentation hygiene

Working-copy probes stay uncommitted per `CLAUDE.md`:

- `src/be300_probe.c` / `src/be300_probe.h` — untracked
- `gxemul/src/cpus/cpu_dyntrans.c` `#I` macro hook,
  `gxemul/src/cpus/cpu_mips_instr_loadstore.c` load/store hooks
  — modified working-copy only, not committed. (These predate
  this pass.)

Only the functional fix is committed.

## 7. Files for the next investigator

1. This file.
2. `docs/HANDOFF_POST_PASS30_BOOT_EXE_TRIGGERS_UNEMULATED_VRC4173_RESET_2026-04-20.md`
   — predecessor, still useful for background on the original
   self-jal symptom.
3. `src/be300_devices.c` — the KjCMU intercept lives at the top of
   `DEVICE_ACCESS(be300_nand)`.
4. `gxemul/src/cpus/cpu_mips.c` — the dyntrans resync fix.
5. `build-host/modules/index.txt` — module base table (useful for
   mapping PCs like `0x019A3D14` back to their DLL).
6. Ghidra project with NK — renamed functions now include
   `boot_thread_trampoline_spawns_launcher_and_WFMs_0x806698CC` at
   `0x80082300` and `SignalBootReady_pulses_DAT_806698CC` at
   `0x80086884`.
