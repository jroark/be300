# Handoff — Pass 11/12 HALTimer: Implemented, Theory Refuted

**Date:** 2026-04-19
**Branch:** `main`
**Submodule branch:** `gxemul/be300-minimal`
**Supersedes:** `docs/HANDOFF_POST_CADENCE_FIX_2026-04-19.md` §10 (HW-reset hypothesis)
**Restores as active target:** `docs/INVESTIGATION_CDM_DLL_PASS9.md` / `memory/project_post_ppsh_stall.md` Pass 9

## Thesis

Two things happened today:

1. **VR4131 HALTimer watchdog, SOFTRST, and CPU cold-reset were implemented end-to-end.** The mechanics are correct and citable against UM §6.3.1 / §12.2. A software-issued SOFTRST (PMUCNT2REG bit 4) now triggers a full cold reset that sets CP0 / MIPS16 / LL-SC / dyntrans state per the UM and re-enters ROM at 0xBFC00000; SDRAM and PMU/RTC peripheral state are preserved; the ROM's cold/warm marker check at 0xBFC0042C correctly branches to the warm-path dispatcher at 0xBFC003C8 and jumps through the mailbox at PA 0x24FC.

2. **The Pass 10 premise that motivated the whole pass was wrong.** Pass 10 concluded "NK never pets HALTIMERRST across 30 s of cold boot → on real HW the watchdog fires and drives the warm-resume between the two Starting splashes." That reading was built on a probe-output endian misread. Re-examining the same probe data with little-endian bytes (as a MIPS VR4131 always is): ROM, SPL, and NK all write PMUCNTREG bit 2 = 1 multiple times in early boot. UM §12.2.2 is explicit that this is a one-shot "program is running normally" acknowledgement, not a periodic pet — once any of them writes bit 2 = 1, the HALTimer is disarmed for the rest of the session. The real-hardware PMUCNTREG readback of `0x1006` (bit 2 set) in `hw_dump_vr4131.txt:35` is the disarmed state.

**Net consequence:** HALTimer never actually fires during normal cold boot on either real hardware or our emulator. The "black screen between the two Starting splashes" is *not* a CPU reset; it's almost certainly a display-subsystem reset or LCD blanking (see CLAUDE.md observation step 3). The boot-path stall reverts to the Pass 9 cdm.dll LoaderCS deadlock — that's the real active target again, and it's the same stall we had before Pass 10.

## Reproduce

```bash
cd build-host && cmake .. && make -j$(sysctl -n hw.ncpu)
gtimeout 60s ./be300 --nand ../ce/restore_images/All_nand_300.bin \
  > cold_stdout.log 2> cold_stderr.log
shasum screenshot_*.bmp
grep -c 'HALTimer fired' cold_stderr.log
```

Expected:
- Screenshot sha: `e8a8c83cd66b9327f50fc1827eada71fb028b332` (pre-Pass-11 `Starting.bmp` baseline).
- HALTimer fires: **0** in 60 s.

## 1. State of the tree

### Committed

`main`:
- `2e28e087` — `be300: fix PMU_PMUINTREG2 mislabel and drop unused m->pmu` (pure rename + dead-code removal).
- `611c55ad` — `be300: PMU HALTimer watchdog + SOFTRST + cold-reset wiring`. Adds `pmu_arm_initial`, `pmu_tick`, `pmu_apply_pending_reset_state`, W1C on PMUINTREG, SOFTRST dispatch, bit masks. Bumps the `gxemul/` submodule.
- `8b93ca33` — `pmu: HALTIMERRST is one-shot "program OK" ack, not periodic pet`. The Pass 12 correction: `pmu_write(PMUCNTREG)` with bit 2 set now permanently disarms (zeroes both remaining and arm-cycles); `pmu_apply_pending_reset_state` no longer re-arms.

`gxemul/be300-minimal`:
- `b9f57dc` — `add mips_cpu_cold_reset() for VR4131 HW-reset emulation`. Resets CP0 per UM §6.3.1, MIPS16 (`mips16`, `m16_delay_target`, `m16_delay_jalx`), LL/SC (`rmw` family), delay slot, dyntrans translation cache; preserves COUNT/COMPARE; sets PC = 0xBFC00000.
- `9d01ac5` — `vr41xx: wire PMU HALTimer tick + SOFTRST cold-reset thunk`. Calls `pmu_tick` from `DEVICE_TICK(vr41xx)`, `pmu_apply_pending_reset_state` + thunk from the MMIO dispatcher on softrst_pending, arms HALTimer from `dev_vr41xx_init`.

### Uncommitted / investigation-only

- `src/be300_probe.c` (+ probe watches, + kseg1 address fix from `0xBF0000Cx` → `0xAF0000Cx`). These are local-only per `CLAUDE.md` Instrumentation Hygiene. Do **not** push. The fix for the kseg1 watch addresses is correct, but it's the probe, not functional code.
- `gxemul/src/cpus/cpu_dyntrans.c` and `gxemul/src/cpus/cpu_mips_instr_loadstore.c` — pre-existing probe-attachment macros (session-level investigation infrastructure, not part of this pass).
- `CLAUDE.md` has the Pass-10→11 "Active investigation pointer" section. **Delete this** in the next passes now that Pass 11+12 have landed and the hypothesis is refuted. See "Next-investigator action items" below.

## 2. Why Pass 10 was wrong

Pass 10 (`docs/HANDOFF_POST_CADENCE_FIX_2026-04-19.md` §10) read the probe output:

```
[BE300_LIFECYCLE_W] label=pmu_cntreg_kseg1 hit=1 pc=0xbfc00744 vaddr=0xaf0000c2 len=2 data=0400 …
```

and concluded `data=0400` meant "written value 0x0400 — bit 10 set, bit 2 clear → no HALTIMERRST pet."

`data=…` in the `LIFECYCLE_W` format is the byte sequence captured from the store buffer, in address order. For a 16-bit little-endian store (MIPS VR4131 is always little-endian), the low byte is at the lower address. The string `0400` means byte[0] = 0x04, byte[1] = 0x00 — which reassembles to 16-bit value `0x0004`, i.e. **bit 2 set, everything else clear**. That is exactly `HALTIMERRST = 1`, the watchdog pet.

Re-read the same probe output with the correct endian interpretation:

| Probe data | Correct value | Pet? | Source |
|-----------|---------------|------|--------|
| `0400` | `0x0004` | yes | ROM @ `0xBFC00744` |
| `0600` | `0x0006` | yes | SPL @ `0xA0F02524` |
| `0000` | `0x0000` | no | NK @ `0xA007A0B8` |
| `0600` | `0x0006` | yes | NK @ `0xA007A0E4` |
| `0210` | `0x1002` | no | NK @ `0x800A5EB4` |
| `0610` | `0x1006` | yes | NK @ `0x800A5FBC` |
| `0610` | `0x1006` | yes | NK @ `0x800A5FDC` |

Five pet writes in the first seconds of boot — ROM, SPL, and NK all acknowledge "program is running normally." Under UM §12.2.2's one-shot semantics, any one of them disarms HALTimer for the rest of the session.

Real-hardware PMUCNTREG readback in `docs/hardware/hw_dump_vr4131.txt:35` = `0x1006` with bit 2 set. That's the disarmed state.

**The HALTimer simply does not fire during normal cold boot on real hardware.** Pass 10's chain of reasoning was built on the opposite assumption and does not survive.

## 3. What Pass 11 still buys us

Even with the HALTimer not firing, Pass 11's four commits are worth keeping:

1. **A clean CP0 cold-reset routine** (`mips_cpu_cold_reset`) that covers all VR4131 UM §6.3.1 state plus this tree's MIPS16 / LL-SC / dyntrans-cache concerns. Any future reset path (HIBERNATE, user-issued warm-restart, etc.) can call it without reinventing the reset.
2. **PMUCNT2REG SOFTRST** implemented correctly. NK's `IOCTL_HAL_REBOOT`-equivalent path (whichever CASIO driver handles it) will route through here once it's exercised.
3. **ROM warm-path detection** verified end-to-end. When a reset does happen — triggered by SOFTRST, or by the eventual-calibration-path that writes `0x030200FF`/`0x03020100` to PA 0x2400 — the ROM correctly reads the marker, skips cold init, and jumps to the mailbox target. This matters for all future paths that want a warm restart.
4. **A correct PMUCNTREG W1C + HALTIMERRST model** that matches UM §12.2.2 one-shot semantics. Real hardware's disarmed state is now reproduced.

Do **not** revert Pass 11. The mechanics are right; only the trigger assumption was wrong.

## 4. State of the stall (restored from before Pass 10)

The post-PPSH stall described in `docs/HANDOFF_POST_PPSH_STALL_2026-04-18.md` §1–§4, §6–§9, §11 is the active target. Six threads are blocked on cold boot:

| idx | Thread VA | EPC | Role | Waits on |
|-----|-----------|-----|------|----------|
| 0 | `0x80FFF024` | `0x800819A4` | boot trampoline | boot event `0x00FFFFC6` |
| 1 | `0x80FFC7CC` | `0x800819A4` | launcher | launcher event `0x00FFC9C6` |
| 2 | `0x80FFCB54` | `0x800819A4` | NK-internal | obj `0x80FF7E2C` |
| **3** | **`0x80FF7B90`** | `0x800819A4` | **device.exe, HOLDS LoaderCS `0x806695A0`, WFSO(thread5 exit)** | thread 5 |
| 4 | `0x80FE4000` | `0x80088230` | device.exe | LoaderCS (same) |
| 5 | `0x80FD592C` | `0x80088230` | device.exe | LoaderCS (same) |

Thread 3 holds the LoaderCS (`0x806695A0`) via `LoadLibrary_kernel_acquire_loadercs_wrapper` at `0x80093040`. Inside the LoadLibrary chain it spawns driver work (cdm.dll's DllMain via PCMCIA.DLL) on Thread 5 and waits for it, while Thread 5 needs the same LoaderCS. Classic inversion.

**Pass 9** (`docs/INVESTIGATION_CDM_DLL_PASS9.md`) identified cdm.dll's DllMain as the specific driver that re-enters LoaderCS. That investigation was sidelined when the HALTimer theory looked more promising; it's the current frontier again.

## 5. Hypothesis space for Pass 13

Pass 4 (VRC4173/ScCmcu static-snapshot skew) is closed. Pass 5–8 (cadence) is closed. Pass 10 (HALTimer-fires) is refuted by this handoff. What remains:

### 5.1 cdm.dll DllMain specifics (HIGH-VALUE)

Read `docs/INVESTIGATION_CDM_DLL_PASS9.md` in full. The key sub-questions:

1. Which VRC4173 / PCMCIA register does cdm.dll's DllMain read or write that ends up re-entering LoaderCS? Is the re-entry through a nested LoadLibrary call, a kernel service that walks the module list, or a synchronization primitive that internally acquires LoaderCS?
2. On real hardware, does cdm.dll's DllMain actually complete, or does it fail early and skip the problematic path? If it fails, what hardware response tells it to fail?
3. Is there a VRC4173 bit or CF-controller state that would make cdm.dll's DllMain take a short path that avoids the deep LoadLibrary?

The `g_loader_watches` and `g_exec_watches` instrumentation in `src/be300_probe.c` is already scoped around `LoadLibrary_acquire_loadercs`, `EnterCriticalSection_kernel`, `WaitForMultipleObjects_kernel_syscall_shim`, etc. Reuse them.

### 5.2 Display-subsystem reset between Startings (MEDIUM)

CLAUDE.md observation step 3 describes a black screen between the two Starting renders. Pass 10 assumed this was a CPU reset; it isn't. It's plausibly:

- An LCD controller reset (VRC4173 LCD block reprogrammed by gwes or the user-mode display stack).
- A framebuffer bank swap (splash buffer at VA `0x80061188` cleared, then the graphics stack initializes its own).
- A PWM / backlight sequence driven by the PMU GPIO bits (bits 8–15 of PMUCNTREG are GPIO TRG/MSK).

None of these explain the stall on their own, but they might indicate what NK is trying to do when the deadlock traps it. Low-priority until 5.1 is exhausted.

### 5.3 Do NOT re-open

- Priority inheritance (Pass 3 settled this).
- Static VRC4173 / ScCmcu / SIU+9 skew (Pass 4 settled this).
- RTCL1 hold / IP7 rate / Count-at-wrong-clock (Pass 7–8 fixed all three).
- HALTimer / SOFTRST / cold-reset mechanics (Pass 11 landed, Pass 12 corrected the semantics).

## 6. Next-investigator action items

1. **Delete the Pass-10→11 pointer in `CLAUDE.md`** — the whole "Active investigation pointer" section. Replace with a short note pointing at `docs/INVESTIGATION_CDM_DLL_PASS9.md` as the active target. Commit as `docs: remove Pass-10 HALTimer pointer; restore Pass 9 as active target`.
2. **Update `memory/project_post_ppsh_stall.md`** with a concise Pass 12 epilogue: HALTimer theory refuted via endian re-read, Pass 11 mechanics retained, stall reverts to Pass 9. Do not leave the Pass 10 REVISED section as "current thinking" — it's wrong.
3. **Re-verify the probe kseg1 addresses before committing anything probe-adjacent.** `src/be300_probe.c:153-167` had kseg1 entries at `0xBF0000Cx` (PA `0x1F0000Cx`, ROM region) which were fixed this session to `0xAF0000Cx` (PA `0x0F0000Cx`, PMU region). That fix is uncommitted per hygiene rules — but when you add new probes, use the corrected addresses.
4. **Resume Pass 9 / cdm.dll investigation.** The deadlock is deterministic and the thread dumps are already characterized. The unsolved question is what the driver DllMain is doing and how real hardware makes it complete.

## 7. Ghidra renames (unchanged from prior handoff)

Still valid:

| Address | Name |
|---------|------|
| `0x800816E0` | `WaitForMultipleObjects_yield` |
| `0x80081AD0` | `CreateEvent_kernel` |
| `0x80080AA4` | `launcher_main_loop_waits_DAT_8066af00` |
| `0x80080D38` | `launcher_module_ready_notify_pulses_DAT_8066af00` |
| `0x8008130C` | `EventModify_kernel_SetResetPulse` |
| `0x80086884` | `SignalBootReady_pulses_DAT_806698CC` |
| `0x80082300` | `boot_thread_trampoline_spawns_launcher_and_WFMs_0x806698CC` |
| `0x800862BC` | `SetThreadPriority_kernel` |
| `0x80082690` | `boot_trampoline_post_WFM_pulses_DAT_806697E8` |
| `0x80093040` | `LoadLibrary_kernel_acquire_loadercs_wrapper` |
| `0x80090a24` | `LoadLibrary_increment_refcount_and_load` |
| `0x80091c90` | `LoadLibrary_parse_filename_and_dispatch` |
| `0x8008831c` | `WaitForMultipleObjects_kernel_syscall_shim` |
| `0x800998C0` | `EnterCriticalSection_kernel` |
| `0x80099924` | `LeaveCriticalSection_kernel` |

New from Pass 11/12 verification:

| Address | Name |
|---------|------|
| `0xBFC0042C` | `rom_warm_marker_check_returns_v0_1_if_0x03020100` |
| `0xBFC003C8` | `rom_warm_jump_prep_loads_and_jumps_to_PA_0x24FC` |
| `0xBFC003EC` | `rom_battery_check_tests_PMUINTREG_BATTINH` (name corrected — this is **not** the cold/warm gate despite what `docs/ROM_SPL_HANDOFF.md` §2.2 implies; it's a battery-low check) |
| `0x800175E0` | `nk_write_warm_marker_03020100_to_PA_0x2400` |
| `0x800175F0` | `nk_sw_at_PA_2400_followed_by_PMUINTREG_read` |

## 8. Files for the next investigator (in order)

1. This file.
2. `docs/INVESTIGATION_CDM_DLL_PASS9.md` — cdm.dll driver specifics.
3. `docs/HANDOFF_POST_PPSH_STALL_2026-04-18.md` §1–§4, §6–§9 — the stall characterization that predates all of Pass 10.
4. `memory/project_post_ppsh_stall.md` — evidence chain (read with the Pass 12 epilogue applied).
5. `build-host/modules/04_device.exe.bin` — device.exe binary for static analysis of the driver enumeration order and cdm.dll load path.
6. `docs/ROM_SPL_HANDOFF.md` §2.2 is partially inaccurate on the 0x9FC003EC role (see new Ghidra rename above); the rest of the document is still useful.

## 9. Open-ended risk

- Secondary boot paths (`--restore --cf`, `--ppsh`) were not regression-tested under the Pass 12 PMUCNTREG change. Before relying on them, run each once and confirm the screenshot shas match the known-good baselines from `docs/HANDOFF_POST_PPSH_STALL_2026-04-18.md`.
- If some future driver or OS update *does* need HALTimer to fire (e.g., an actual runaway-detection test), the current one-shot-disable semantics will need a compatibility mode. The code structure supports it — `haltimer_arm_cycles` is still a field — but the rule in `pmu_write` would need to change.
- The `rom_coldwarm_gating` probe at `0x9FC003EC` in `src/be300_probe.c` is misleadingly named given the new ghidra rename (`rom_battery_check`). Rename when you next touch that file.
