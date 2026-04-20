# Legitimate GXemul Bug Fixes Not Currently Applied

## Purpose

During the 2026-04-17 minimization of the gxemul submodule to `0.7.0 + MIPS16 + BE-300 platform + RTC/ETIMER + AdEL EPC`, several commits from the old `be300` branch were evaluated and dropped because boot did not regress without them. A subset of those commits do, however, fix **real upstream GXemul 0.7.0 bugs** against the MIPS R4000-series spec and the VR4131 users manual; they just happen to be on code paths not exercised by the current BE-300 cold-boot path.

This file is a reference for future investigations. If a regression appears whose symptom matches one of the warning signs below, the corresponding commit is worth re-evaluating.

All commit hashes refer to the `be300` branch history in the `gxemul/` submodule (`github.com:jroark/GXemul.git`).

---

## What we re-applied and why

- **2026-04-19** — `2fa3853` ("mips: CP0 Compare timer rate hz/2 + cap pending IP7 at 1") on `be300-minimal` re-applies the *bug-fix essence* of section 1 below, specifically the `compare_interrupts_pending` cap-at-1 change and adds a related VR4131 datasheet correction (Count advances at CPU/2, so wall-clock timer rate is `emulated_hz / (2 * compare_diff)` not `emulated_hz / compare_diff`). Motivation: WinCE NK boot exposed a 12.6 kHz IP7 storm and `pending=194` backlog during cold-boot Pass 7 that contributed to the broader RTCL1 cadence problem (see commit `2fa3853` and `memory/project_post_ppsh_stall.md`). The remaining sub-fixes from section 1 (`COP0_COMPARE` schedule offset, dyntrans end-of-batch crossing check) were *not* applied — they remain unneeded on the BE-300 path because the guest still does not use MTC0 Compare-driven ticks. If a future investigation needs them, treat section 1 below as the reference.

---

## 1. `42b9ff5` — MIPS Compare/IP7 timer wake coherency

**What it fixes:**
- `compare_interrupts_pending` is an unbounded backlog counter in 0.7.0; drifts out of sync with `CAUSE.IP7`. Should be a latched 0/1 flag (set in timer callback, cleared on `MTC0 Compare`).
- `COP0_COMPARE` write handler schedules the host timer using `(new_compare - old_compare)` instead of `(new_compare - current_count)`. The diff is mostly meaningless and the `< 0` branch falls through via dead code.
- End-of-batch `Count` crossing check in `cpu_dyntrans.c` is disabled when `emulated_hz > 0` (marked "Not yet TODO"); the `diff1 > 0 && diff2 <= 0` check never fires.

**Why not load-bearing for us:** BE-300 drives timer interrupts via VR4131 RTCL1 → VRIP.ETIMER, not via MIPS `Count`/`Compare`. Our guest (WinCE) does not use the MIPS timer.

**Warning signs that would justify re-applying:**
- A guest OS port (NetBSD, Linux for VR4131, etc.) that uses MIPS `Compare`-driven ticks stalls indefinitely.
- Tests that call `MTC0 Compare` and expect IP7 to fire after exactly `(compare - count)` cycles.

**Citation:** VR4131 UM §7.1 (Count/Compare), MIPS R4000 ISA.

---

## 2. `c6af9cb` (partial) — VR4131 STANDBY wake IP7 assertion

**What it fixes:** In 0.7.0, `mips_timer_tick` only records that an interrupt is pending, and `INTERRUPT_ASSERT` on IP7 happens inside the `Count` *read* path (`coproc_register_read`). While the CPU is halted in `WAIT`/`STANDBY` no reads occur, so IP7 is never asserted and the CPU halts forever.

The commit:
- Asserts `irq_compare` directly from `mips_timer_tick`.
- In `X(idle)` for MIPS_R4100, checks `Count >= Compare` and asserts IP7 immediately (the real VR4131 clock continues during STANDBY).

(The `emulated_hz = 131072000` hunk is already in the minimal squash `3c3d5cb`, so only the two behavioral hunks above are dropped.)

**Why not load-bearing for us:** Same as #1 — we use RTCL1, not Compare.

**Warning signs:**
- Any `WAIT` / `STANDBY` halt that never wakes when the MIPS `Compare` timer is the only tick source.

**Citation:** VR4131 UM §4.2 (STANDBY), MIPS R4000 ISA.

---

## 3. `2203000` — MIPS WAIT unhalt on masked interrupts (WAIT-unhalt hunk only)

**What it fixes:** Per MIPS ISA §3.4, `WAIT` exits on *any* interrupt signal, regardless of `Status.IE`, `EXL`, `ERL`, or `Status.IM` masking. Upstream 0.7.0 only unhalts inside `mips_cpu_exception()`, which requires `enabled && mask`. If a guest enters `WAIT` with IE cleared or the pending IP masked out, it halts forever even though a pending interrupt exists.

The commit adds a check in `DYNTRANS_RUN_INSTR_DEF`: if the CPU is halted and any `Cause.IP` bit is set but delivery is blocked, still unhalt and advance past `WAIT`.

(The RTCL1-stop-API hunk from this commit is separate and stays dropped — pure diagnostic coupling.)

**Why not load-bearing for us:** Current guest has interrupts enabled when it issues `WAIT`, so the 0.7.0 path works.

**Warning signs:**
- Guest enters `WAIT` with `Status.IE = 0` or with `EXL`/`ERL` set and never wakes, even though a device is asserting an interrupt.

**Citation:** MIPS R4000 ISA §3.4 (WAIT instruction behavior).

---

## 4. `48efae8` — Count MFC0 drift fix

**What it fixes:** 0.7.0's `COP0_COUNT` read path increments `reg[COP0_COUNT]` by 1 per `MFC0` read. A tight calibration loop polling Count many times per dyntrans batch sees Count drift forward much faster than real instruction progress — potentially causing the guest to miscalibrate its CPU speed or enter idle prematurely. Per MIPS ISA, Count must reflect actual instruction progress (increment at PClock/2).

The commit synthesizes `base + n_translated_instrs` on each MFC0 without mutating `reg[COP0_COUNT]`; the end-of-batch fixup in `cpu_dyntrans.c` adds `n_instrs` to the base once per batch.

**Why not load-bearing for us:** The WinCE kernel's scheduler doesn't tightly calibrate via MIPS `Count`. BE-300 scheduler runs off RTCL1.

**Warning signs:**
- A guest OS reports wildly-wrong CPU speed (e.g., "this CPU is running at 4 GHz").
- Scheduler / delay loop behavior where emulated time runs multiple × faster than wall-clock.

**Caveat:** This commit has a dependency on `97b9535` (defer Status IRQ / Compare one-shot) which was an exploratory dead-end we dropped. Re-applying `48efae8` on its own requires a careful read of the diff to separate the Count-drift fix from the `compare_timer_armed` one-shot gating.

**Citation:** MIPS R4000 ISA §7 (Count register semantics).

---

## 5. `d6046bc` — MIPS cache-op invalidation

**What it fixes:** The MIPS `CACHE` instruction is a no-op stub in 0.7.0 (`/* TODO: Implement cache operations. */`). On real hardware it invalidates I-cache/D-cache lines; software that writes code into memory then issues `CACHE` expects the next instruction fetch to pick up the new bytes.

The commit implements invalidation of code translations at the affected vaddr/paddr via `cpu->invalidate_code_translation`.

**Why not load-bearing for us:** The BE-300 cold-boot SPL unpacks NK.exe into cached kseg0 addresses but the CPU re-fetches through the host-memory-pointer path, which always sees the latest bytes regardless of whether a `CACHE` op was issued — GXemul doesn't actually model per-line caching, so the stale-line scenario can't happen in practice.

**Warning signs:**
- Guest writes code to memory, issues `CACHE`, and executes the new code; instead of the new code, it runs stale code. (Would be visible as an exception at a wrong PC or a jump to garbage.)
- This is more likely to bite a guest that uses runtime code generation (JIT) than a pre-linked image.

**Citation:** MIPS R4000 ISA §4 (cache operations).

---

## Out of Scope — Commits That Are *Not* Legitimate Bug Fixes

For completeness, these dropped commits should **not** be reconsidered without a corresponding new justification. They are noted here so future investigations don't spend effort re-evaluating them:

| Commit | Why not |
|--------|---------|
| `e686a53` | Wrong EPC/BD semantics for misaligned-jump AdEL; superseded by `c4f8936`. |
| `deca086` | Intermediate stepping stone with same wrong EPC formula; superseded by `c4f8936`. The call-sites from this commit were brought forward alongside `c4f8936` (stripped of diagnostics). |
| `97b9535` | Exploratory "defer Status IRQ / Compare one-shot" — commit message acknowledges no boot progress. |
| `f559ec9` | Exploratory `wait/eret` hazard tightening — commit message calls it a dead-end. |
| `844a662` | References `host_io_console_stdin_enabled()` which does not exist on this branch. Orphaned. |
| `5b83f78` / `78885a4` | Force-override TLB[1] even page at boot. Not a real-HW behavior; masks a different emulator bug. |
| `2203000` (RTCL1-stop hunk) | Adds a `dev_vr41xx_stop_rtcl1()` API coupled to diagnostic infrastructure that was cleaned out of the submodule. |
| `bd3342d` | 0.7.0 already has the equivalent `ENTRYHI_VPN2_MASK | 0x1800` workaround in `memory_mips_v2p.c` for V2P_MMU4100. Net transformation is identity. |
| `0ab4232` / `521723c` | Functionality subsumed by the squashed RTC/ETIMER commit (`53c5910`). |

---

## How to Re-apply

If a warning sign matches:

1. Check out the commit: `git -C gxemul show <hash>`
2. Isolate the functional hunks (every commit in the table above has clearly separable diagnostic vs functional hunks — do not re-apply wince_boot_*, idle_diag_*, or the diagnostic fprintfs).
3. Cherry-pick on top of `be300-minimal` with `--no-commit`, hand-resolve conflicts, strip instrumentation, commit with a message that references this doc and the observed regression.
4. Add a line to the "What we re-applied and why" section at the top of this file so future readers see it.
5. Smoke-test the regression that motivated the re-apply to confirm the fix resolves it.
