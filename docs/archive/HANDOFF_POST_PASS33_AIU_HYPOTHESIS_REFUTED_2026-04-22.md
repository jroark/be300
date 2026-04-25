# Handoff — Pass 33 · AIU wave-driver hypothesis refuted by probe

> **Superseded by `docs/HANDOFF_POST_PASS34_HIBERNATE_AND_AB_WINDOW_2026-04-22.md` for "Next pivot" (§5 here).** The AIU refutation in §1–§4 below is still authoritative. Pass 34 replaces §5 with the HIBERNATE fix + 0xAB000000 stub + a new ranked ddi.dll probe list against the current blocker.

**Date:** 2026-04-22
**Predecessor:** `docs/HANDOFF_POST_PASS32_LAUNCHER_BLOCKS_ON_BOOTEXE_READY_2026-04-22.md`
**Plan executed:** `/Users/jroark/.claude/plans/buzzing-tumbling-ember.md`
**State of tree:** no code changes. A one-block `fprintf` probe was added
to `src/be300_devices.c` `DEVICE_ACCESS(be300_vrc4173)` to capture
first-hit accesses in the AIU range (`0x0E0..0x0FC`) and the BlgReg range
(`0x980..0x99F`), the 180 s cold boot was run, and the probe was reverted
(`git diff --stat` is empty for `src/be300_devices.c`).

## 1. Finding in one sentence

The hypothesis floated at the close of Pass 32 — that a WinCE wave-driver
DLL probing the VRC4173 AIU register block (`PA 0x0A0000E0..0x0A0000FC`)
during init would explain why the post-Boot.exe-ready desktop never
paints — is **refuted by empirical probe**: across a 180 s cold boot
the AIU range sees **zero accesses** from any code path (ROM, SPL, NK,
or user-mode). The only audio-adjacent access is four one-time writes
to Casio BlgReg at `PA 0x0A000980..0x0A00098C` from SPL, which match
the real-hardware quiescent values and are not gated on any readback.

## 2. Probe design (one-shot first-hit per offset, working-copy-only)

Inserted at the top of `DEVICE_ACCESS(be300_vrc4173)` (revert-before-commit):

```c
/* Pass 33 Phase 2 probe — first-hit per AIU/BlgReg offset. REVERT. */
static uint32_t aiu_seen = 0, blg_seen = 0;
if (off >= 0xE0 && off < 0x100) {
    uint32_t bit = 1u << ((off - 0xE0) >> 1);
    if (!(aiu_seen & bit)) {
        aiu_seen |= bit;
        uint64_t wval = (writeflag == MEM_WRITE)
            ? memory_readmax64(cpu, data, len) : 0ULL;
        fprintf(stderr, "[P33/AIU] pc=0x%08x off=0x%03x w=%d len=%u wval=0x%llx\n",
            (uint32_t)cpu->pc, off, writeflag, (unsigned)len,
            (unsigned long long)wval);
    }
} else if (off >= 0x980 && off < 0x9A0) {
    uint32_t bit = 1u << ((off - 0x980) >> 2);
    if (!(blg_seen & bit)) {
        blg_seen |= bit;
        uint64_t wval = (writeflag == MEM_WRITE)
            ? memory_readmax64(cpu, data, len) : 0ULL;
        fprintf(stderr, "[P33/BLG] pc=0x%08x off=0x%03x w=%d len=%u wval=0x%llx\n",
            (uint32_t)cpu->pc, off, writeflag, (unsigned)len,
            (unsigned long long)wval);
    }
}
```

Run: `gtimeout 180s ./be300 --nand ../ce/restore_images/All_nand_300.bin`.
Full stderr (14 lines total; probe reverted after this capture):

```
no such device ("pcic")
machine model: Casio Cassiopeia BE-300
machine clock speed: 131.07 MHz
[P33/BLG] pc=0x80f038f8 off=0x980 w=1 len=4 wval=0x0
[P33/BLG] pc=0x80f038fc off=0x984 w=0 len=4 wval=0x0
[P33/BLG] pc=0x80f038fc off=0x984 already suppressed (not re-logged)
[P33/BLG] pc=0x80f03920 off=0x988 w=1 len=4 wval=0xf70
[P33/BLG] pc=0x80f03928 off=0x98c w=1 len=4 wval=0x0
%  WARNING! Input to console handle "dsiu" wasn't enabled, …
[KjCMU] warm reset triggered at pc=8007a174 (PA 0x0A00A0C4<=7, PA 0x0A00A0C8<=10)
[UI] Screenshot saved: screenshot_20260422_141302.bmp
```

Screenshot SHA `e8a8c83cd66b9327f50fc1827eada71fb028b332` — byte-identical
to the Pass 32 stuck-`Starting.bmp` baseline.

## 3. Why this refutes the AIU hypothesis concretely

- All four BlgReg hits are from SPL VA `0x80F0_3*` (Kloader running at
  VA `0x80F00000+`). SPL writes `0x980=0`, reads `0x984`, writes
  `0x988=0xF70`, writes `0x98C=0` — exactly the `docs/hardware/hw_dump_vrc4173.txt:1084`
  quiescent values. These are one-shot SPL init writes, not a driver
  polling loop. No `pc==0x80F0_3928` re-hits.
- The AIU register window (`0xE0..0xFC` per VRC4173 UM Chapter 10) is
  not touched once across the full 180 s window — not by ROM, not by
  SPL, not by NK/OAL, not by any user-mode driver. The predicted
  "wave driver DLL probes AIU during init and stalls on mismatched
  status bits" pattern is not present in this image.
- The stall at `Starting.bmp` therefore is **not** caused by missing
  AIU emulation. The audio-tone-on-tap observation from real HW either
  (a) comes from a code path that only runs after the calibration UI
  appears (so after the stall we're currently sitting at), or (b) is
  driven through a pure GPIO / buzzer path that doesn't touch AIU.

## 4. Corollary: the probe IS wired, just env-var-gated

*Correction (Pass 34, same day):* `src/be300_probe.c` IS already in the
build. `CMakeLists.txt:106` lists it under `BE300_SOURCES`;
`src/machine_be300.c:591` calls `be300_probe_attach(gxm)` and `:644`
calls `be300_probe_detach`; `gxemul/src/cpus/cpu_dyntrans.c:125` calls
`be300_probe_note_exec(cpu, exec_pc)` per-basic-block. The probe is
inert unless `BE300_LIFECYCLE_PROBE=1` is set in the environment
(`be300_probe.c:1725` gate). Default cold-boot stderr is clean because
the gate drops the hooks to no-ops.

To reproduce Pass 32's evidence on the current tree:

```
cd build-host
BE300_LIFECYCLE_PROBE=1 gtimeout 180s ./be300 \
    --nand ../ce/restore_images/All_nand_300.bin \
    > p34_stdout.log 2> p34_stderr.log
```

Pass 34 (2026-04-22) ran this command and **verified the Pass 32
narrative against the current clean tree**:
`launcher_module_ready_notify hits=7` with `a0=0x3B` on hit 7 (Boot.exe
signals ready on 2nd boot); `[BE300_LIFECYCLE_CREATEPROCESS] hit=15
image="coshell.exe"` (coshell.exe spawns post-Boot.exe-ready);
`ddi_blit_dispatcher_entry hits=27` / `ddi_iFunc10 hits=27` /
`ddi_iFunc18 hits=17` (ddi.dll's blit pipeline fires repeatedly);
`gdi_surface_0x140000 writes=6992` (ddi fills a GDI surface);
`fb_body_kseg1_writes writes=600934` (OAL fills the kseg1 FB with the
`Starting` splash). Crucially: **zero user-mode writes to user VA
`0x001E0000`** (the mapped primary FB surface for the process that owns
ddi.dll), matching `project_pass32_framebuffer_blit_missing.md`. The
Pass 34 stderr is at `build-host/p34_stderr.log` (54725 lines, 54564
LIFECYCLE hits); screenshot SHA still `e8a8c83cd66b9327f50fc1827eada71fb028b332`.

## 5. Next pivot (grounded in Pass 34 probe re-capture)

Pass 34 re-capture (§4) confirms Boot.exe signals ready on the 2nd
boot and coshell.exe spawns. ddi.dll's `DrvEnableDriver` runs twice,
`ddi_iFunc10` (the blit entry point) fires 27 times, `ddi_iFunc18`
(raster/copy) fires 17 times. The stall is downstream: **ddi.dll
produces 6992 writes into a GDI off-screen surface at VA 0x00140000
but never flushes any of that content to the primary FB at user VA
0x001E0000.** Only OAL touches the kseg1 FB (600 934 writes drawing
the `Starting` splash).

Concrete next probes (ranked, each falsifiable in one 180 s run):

1. **Probe DrvCopyBits / surface bind.** ddi.dll's 27 `ddi_iFunc10`
   hits are likely `DrvCopyBits` or `DrvBitBlt` calls with a source
   surface and destination surface. In the Pass 32 memory entry
   `project_pass32_framebuffer_blit_missing.md` the observation is
   that the destination is consistently the GDI off-screen, never
   the primary. Add a one-line `fprintf` inside `ddi_iFunc10` that
   logs the destination surface address (read from the `a0`/`a1`
   argument struct). If the destination is always VA 0x00140000,
   the bug is in the surface-swap / present path, not in the blit
   itself.
2. **Find who maps VA 0x001E0000.** The probe's
   `BE300_GDI_DUMP region=primary_uva chunks_ok=0/38` indicates that
   at the time the probe dumps, VA 0x001E0000 has no page mapping in
   the active ASID. But ddi's `DrvEnablePDEV` should map the primary
   surface somewhere the driver can reach. Probe
   `VirtualCopy`/`VirtualAlloc` calls from ddi's init path (PC hits
   on `ddi_iFunc0_DrvEnablePDEV_guess @ 0x01A5D2F0`, entry hits=2)
   to see what VA it actually binds the primary to. The "user VA
   0x001E0000" in the memory entry might be a stale address from an
   earlier analysis — the real mapping may be somewhere else, and
   ddi's blits may actually be hitting it without our probe noticing.
3. **Check for a pending `Present` / flush event.** WinCE's graphics
   stack batches blits into an off-screen surface then flushes on a
   vsync/present signal. The flush may be event-driven. The 17
   `ddi_iFunc18` hits are candidates — but also look for a WFSO on
   a "present" event from gwes. Since `event_modify_set_reset_pulse
   hits=439` in the run, that event-dispatch is active; find the
   specific event handle ddi waits on before flushing.

Explicitly DO NOT:
- Revisit audio/AIU. Refuted in §3.
- Blame gwes_worker `WFSO(0x6834)`. That worker both entered and
  returned in Pass 34 (`gwes_worker_wfso_6834_ret hits=2`), and the
  user's real-HW photo analysis shows the identical block on real
  hardware.
- Add RAM seeds, `resume_ctx` shortcuts, or guest patches.

## 6. What Pass 33 did NOT do (and why)

- **No functional commit.** The plan's Phase 3 (seed AIU reset values
  `0x0800 / 0x0200 / 0x0200 / 0x0800` at offsets `0xE0 / 0xE2 / 0xE6 / 0xF0`)
  was contingent on Phase 2 showing AIU accesses. Zero accesses means
  the seed would be a pure speculative change under CLAUDE.md §Emulation
  Philosophy — it would "fix" a register the guest never reads. Leaving
  the AIU range at its (already-correct) all-zero default.
- **No BlgReg seed.** The SPL writes values that match real-HW dump
  unconditionally, so whatever the emulator returned at pre-write reads
  was not load-bearing for SPL's behavior, and there are no post-SPL
  reads in the 180 s window. Pre-seeding would be cosmetic.
- **No W1C widening to include INTREG (0xFC).** The W1C range
  `(off >= 0x060 && off < 0x078)` in `src/be300_devices.c:428` is
  correct for the ICU status regs. `INTREG` at `0xFC` is R/W per
  UM §10.2.11 with 6 W1C bits, but nothing ever writes to it, so the
  latent gap has no observable effect.
- **No SEQREG.AIURST auto-clear.** Same reason: `0xFA` is never
  written in the 180 s window.

## 7. Lessons for future passes

1. **Probe first, citations second.** Pass 33's plan built a detailed
   UM-citation table for AIU before running any probe. The table was
   correct (UM §10.2.1-10.2.11 reset values verified) but irrelevant:
   no guest code touches the range being cited. Flipping the phase
   order — probe for access patterns first, then go to UM for the
   subset that gets touched — would have produced the same refutation
   in one run instead of two phases of work.
2. **A reverted probe module is not ground truth.** When prior-pass
   memory entries describe emulator behavior in detail (launcher
   table contents, process spawn order, etc.), check that the probe
   that produced the detail is still wired into the clean build.
   Otherwise the narrative may be stale. In this tree
   `src/be300_probe.c` is 2151 lines of dormant code that zero build
   artifacts reference — easy to assume it's live when it isn't.
3. **Taskbar-during-wizards is a real observation.** The user's
   refutation of the gwes_worker `WFSO(0x6834)` narrative based on
   real-HW photos (coshell taskbar visible under both calibration and
   Home wizards) is load-bearing and correct. Whatever Pass 34 ends
   up probing, the success criterion is still "user-mode code writes
   to the primary framebuffer at VA `0x001E0000` / PA `0xAA200000`" —
   that is the point after which the screenshot stops matching
   `e8a8c83cd66b9327f50fc1827eada71fb028b332`.

## 8. Files and references

- Plan: `/Users/jroark/.claude/plans/buzzing-tumbling-ember.md`
- Predecessor handoff: `docs/HANDOFF_POST_PASS32_LAUNCHER_BLOCKS_ON_BOOTEXE_READY_2026-04-22.md`
- Source of the (reverted) probe: `src/be300_devices.c:371` `DEVICE_ACCESS(be300_vrc4173)`
- UM reference (unused this pass): `docs/hardware/U14579EJ2V0UM00.pdf` Chapter 10
- Dormant probe module: `src/be300_probe.c` (2151 lines, untracked, unbuilt)
- Baseline screenshot: `build-host/screenshot_20260422_141302.bmp`
  (SHA `e8a8c83cd66b9327f50fc1827eada71fb028b332`)
