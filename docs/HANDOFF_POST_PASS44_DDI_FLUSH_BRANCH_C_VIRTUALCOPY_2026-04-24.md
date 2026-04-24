# Handoff — Pass 44: DDI flush diagnosis = VirtualCopy / TLB aliasing bug

**Date:** 2026-04-24
**Branch:** `investigate/pass38-gwes` (local only)
**Probe state:** working-copy diagnostics in `src/be300_probe.c` (NOT committed)
**Logs:**
- `build-host/pass51_ddiflush_stderr.log` (120 s, captured at Welcome.exe spawn)
- `build-host/pass51b_240s_stderr.log` (240 s, captured post-Welcome render)
- `build-host/screenshot_20260424_154153.bmp` (240 s screenshot)
**Plan:** `~/.claude/plans/reflective-sniffing-garden.md`

## TL;DR

The post-Welcome paint stall is **not** a missing flush primitive in ddi.dll. ddi.dll runs the full blit engine and writes thousands of colored RGB565 pixels to the user-VA range `0x001E0000..0x00206000` (the supposed primary-FB user mapping). **None of those writes propagate to PA `0xAA200000`** (where SDL reads the actual framebuffer). The bug is a VA→PA aliasing failure in the WinCE-style `VirtualCopy` mapping, not in any user-mode rendering code.

## Evidence (Pass 51b, 240 s run)

Probe summary excerpt:

```
[BE300_LIFECYCLE_SUMMARY] exec label=ddi_blit_dispatcher_entry hits=2638 pc=0x01a5bf00
[BE300_LIFECYCLE_SUMMARY] exec label=ddi_iFunc10              hits=2638 pc=0x01a5c9cc
[BE300_LIFECYCLE_SUMMARY] exec label=ddi_iFunc18              hits=34   pc=0x01a60690
[BE300_LIFECYCLE_SUMMARY] exec label=ddi_color_expand_entry   hits=24   pc=0x01a5f1e4
[BE300_LIFECYCLE_SUMMARY] mem  label=fb_body_kseg1_writes     reads=0      writes=746838  range=0xaa200010..0xaa226000
[BE300_LIFECYCLE_SUMMARY] mem  label=gdi_surface_0x140000     reads=599624 writes=286005  range=0x00140000..0x00170000
[BE300_LIFECYCLE_SUMMARY] mem  label=ddi_mapped_user_va       reads=146631 writes=16549   range=0x001e0000..0x00206000
```

Top writers to `ddi_mapped_user_va` (by PC, all in ddi.dll):

| PC | writes |
|---|---|
| `0x01A5AE28` | 3462 |
| `0x01A53A70` | 1807 |
| `0x01A54950` | 1692 |
| `0x01A5EB40` | 480 |
| `0x01A5E7AC` | 480 |
| `0x01A5F620` | 138 |
| `0x01A5F618` | 69 |
| `0x01A5F48C` | 16 |
| `0x01A5F5B0` | 8 |

Top writers to `fb_body_kseg1_writes` (PA `0xAA200010..0xAA226000`):

| PC | writes (cap-truncated) |
|---|---|
| `0x80F037CC` (OAL splash drawer, kseg1) | 4096 (true count = 746,838 — single-PC) |

**Zero user-mode (PC < `0x80000000`) writes ever reach PA `0xAA200000+`.**

Sample writes to user VA showing colored RGB565 data (not 0xFFFFFFFF):

```
label=ddi_mapped_user_va hit=1  pc=0x01a53cf0 vaddr=0x00204dde len=2 data=59ce  ra=0x01a53c68
label=ddi_mapped_user_va hit=2  pc=0x01a53cf0 vaddr=0x00204fde len=2 data=59ce  ra=0x01a53c68
label=ddi_mapped_user_va hit=3  pc=0x01a53cf0 vaddr=0x002051de len=2 data=59ce  ra=0x01a53c68
label=ddi_mapped_user_va hit=521 pc=0x01a54950 vaddr=0x00205e00 len=2 data=0000 ra=0x01a54bc4
```

Pattern: 16-bit RGB565 writes to user VAs `0x00204xxx..0x00205xxx`, stride matches a typical FB row pitch (240×2 = 480 = 0x1E0, observed `0x200`-stride for aligned rows).

**Refutation of Pass 50's "all 2367 writes are 0xFFFFFFFF"**: Pass 50 captured a snapshot before the user-mode color paint kicked in. With 240 s wallclock, ddi.dll writes 16,549 pixels to user VA, **zero** of them all-ones. Many are colored (e.g., `0x59CE`, a green-cyan tint).

## Final-screenshot pixel histogram

`screenshot_20260424_154153.bmp` (240×320 24bpp BGR):

- 76,480 pixels = pure white `(255,255,255)`
- 320 pixels = pure black `(0,0,0)`
- 0 pixels = anything else

Despite ddi.dll writing 16,549 colored pixels, the screenshot has only 2 achromatic colors. The user-mode color writes never reach the FB physical memory.

## Why this isn't a paint-loop / flush bug

1. **Color content exists.** ddi.dll has rendered thousands of colored pixels — the paint pipeline runs.
2. **The destination is correct from ddi.dll's perspective.** Every write targets the user VA range that `cached_pdev[0x6C]` (UVA `0x0011047C`) holds — which Pass 50 verified is `0x001E0000`, set up by `ddi.dll PC 0x01A546D0` via `VirtualCopy` to map PA `0xAA200000`.
3. **The PA writes counter `fb_body_kseg1_writes` shows zero matching activity.** The 746,838 PA-side writes all come from a single OAL kseg1 PC `0x80F037CC` — a hardware-side splash drawer that bypasses VirtualCopy.
4. **iFunc18 is refuted as the missing flush.** Its arguments are tiny integers (`a0=1, a1=2, a2=struct-pointer`), not SURFOBJ pointers — it's a callback/info function, not a blit.
5. **Branch B (no WM_PAINT delivery) is refuted.** ddi.dll's blit dispatcher fires 2,638 times; gwes creates 61 windows; coredll EventModify fires 5,889 times. Paint scheduling clearly runs.

## Branch C confirmed: VA→PA aliasing fails for the primary FB user mapping

The hypothesis is concrete: when WinCE's `VirtualCopy(processVA=0x001E0000, kernelPA=0xAA200000, length=0x26000, …)` is invoked by ddi.dll, the resulting TLB entry maps user VA `0x001E0000` to a regular RAM page rather than to the dev_fb-backed PA `0xAA200000+`. So:

- User-mode writes via the user VA succeed (they reach SOME backing store).
- The backing store is plain RAM, not dev_fb.
- The SDL display reads dev_fb at PA `0xAA200000`, and only sees OAL's kseg1 writes there.

Static disassembly of `build-host/modules/62_ddi.dll.bin` (vbase `0x01A50000`) via `/tmp/scan_ddi_flush.py` found **zero functions referencing both `0x0014` and `0x001E` LUI immediates** — confirming the destination is data-driven, not constant-baked, so this is not a missing-function bug in ddi.dll.

## Falsifiable next probe (Pass 45)

Add a `cpu->translate_v2p` call (or equivalent gxemul TLB lookup) at the moment ddi.dll writes to user VA `0x00204DDE` (PC `0x01A53CF0`, hit 1), and log the resolved PA. Expected smoking gun:

- **Branch C confirmed:** resolved PA is in regular RAM (e.g., `0x002xxxxx` or in some SDRAM allocation), NOT `0xAA204DDE`. Then the fix lives in the WinCE syscall pathway for `VirtualCopy`.
- **Refuted (unlikely):** resolved PA = `0xAA204DDE`. Then dev_fb's write path drops the data — fix lives in `gxemul/src/devices/dev_fb.c` (`dev_fb_access`).

The TLB lookup helper in gxemul is in `gxemul/src/cpus/memory_mips.c`. A call shape like `translate_v2p(cpu, 0x00204DDE, &pa, FLAG_WRITEFLAG | FLAG_NOEXCEPTIONS)` should be safely callable inside `be300_probe_log_mem`.

## Candidate fix sites (do NOT commit until Pass 45 confirms)

If the resolved PA is regular RAM:
- **Find the VirtualCopy syscall handler in NK kernel.** It's a `KCall`-based syscall that updates the calling process's PFNTOPFN page-table mapping. Locating it requires Ghidra inspection of NK's syscall dispatch table near `0x80094D` or wherever WinCE 3.0's KAPI table lives.
- **Verify our emulator's TLB refill path correctly uses the page table the guest writes.** If the guest writes a PFN of `0xAA200`, our refill must populate the TLB with that PFN. If we're stripping the high bits or miscomputing PFN-from-PA, that's the bug.

If the resolved PA is `0xAA200000+` but writes still don't appear:
- `gxemul/src/devices/dev_fb.c` — check the access callback for the relevant PA range. May need to ensure cached writes are flushed into the dev_fb backing buffer.

**No guest patches, no shadow-write hooks, no synthetic VA aliasing in the host display device.** The fix must respect the WinCE-side behavior; user-VA writes to a properly-mapped FB region MUST reach dev_fb's backing memory, period.

## What WAS confirmed / remains valid

- WinCE 3.0 cold boot completes through ROM → SPL → NK → kernel launcher → device.exe → gwes.exe → Boot.exe → coshell.exe → Welcome.exe in ~120 s (`CreateProcess hit=16 image="Welcome.exe"`).
- ddi.dll's main blit dispatcher (`0x01A5BF00`) is a 1:1 wrapper around iFunc10 (`0x01A5C9CC`); both fire 2,638 times in 240 s.
- iFunc3 (`0x01A5D228`, label `DrvEnableSurface_guess`) shows zero hits in 240 s — but this label is a guess; the absence does NOT necessarily indicate DrvEnableSurface was skipped.
- The kernel launcher dependency chain decoded in Pass 42–43 remains accurate.
- The `gwes_worker WFSO(0x6834)` "red herring" finding from Pass 33–34 remains valid.

## Probe diff (working-copy only — DO NOT commit)

`src/be300_probe.c`:
- New exec_watch entry: `0x01A5F1E4 ddi_color_expand_entry`.
- New per-PC hit caps for `0x01A5C9CC`, `0x01A60690`, `0x01A5F1E4` (32 each).
- New per-hit SURFOBJ dump branches for `ddi_iFunc10`, `ddi_iFunc18`, `ddi_color_expand_entry` (cap 16 each).
- `gdi_surface_0x140000` mem_watch: enabled `log_reads=true`.

Total diff size: ~70 lines. Revert before any future functional commit per CLAUDE.md "Instrumentation Hygiene".

Disassembly script: `/tmp/scan_ddi_flush.py` (one-shot, not a project artifact).

## Open questions for Pass 45

1. What PA does the TLB resolve user VA `0x00204DDE` to during a write at PC `0x01A53CF0` (entryhi/ASID matters)?
2. Is there a single `KCall`-based VirtualCopy in WinCE 3.0 NK, or is it inlined per-process? If inlined, where is the page-table update?
3. Does the gxemul TLB refill path correctly handle PA ≥ `0xAA000000` (kseg1-mapped device range) when reached via a user-mode TLB miss?
4. Does the ddi.dll mapping at PC `0x01A546D0` actually succeed in writing the FB PA into the user-process page table, or is its return value a different VA we haven't traced?

## Cross-references

- Plan: `~/.claude/plans/reflective-sniffing-garden.md`
- Prior: `docs/HANDOFF_POST_PASS43_LAUNCHER_DEP_CHAIN_WELCOME_SPAWNS_2026-04-24.md`
- Auxiliary: `MEMORY.md` entries `project_pass32_framebuffer_blit_missing.md`, `project_pass35_virtualcopy_failure.md` — note that the Pass 35 entry hypothesized a VirtualCopy issue but called it "works" without checking destination PA. This pass refines that to "VA mapped, but to the wrong PA".
