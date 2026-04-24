# Handoff — Pass 34 · HIBERNATE implemented + 0xAB000000 companion window stubbed

**Date:** 2026-04-22
**Predecessors:**
- `docs/HANDOFF_POST_PASS33_AIU_HYPOTHESIS_REFUTED_2026-04-22.md` (Pass 33, audio hypothesis refuted)
- `docs/HANDOFF_POST_PASS32_LAUNCHER_BLOCKS_ON_BOOTEXE_READY_2026-04-22.md` (Pass 32, Boot.exe ready-signal on 2nd boot)

**State of tree (both working-copy, uncommitted):**
- `gxemul/src/cpus/cpu_mips_instr.c` +61 lines — `X(hibernate_vr41xx)` handler + COP0_HIBERNATE routing.
- `src/machine_be300.c` +18 lines — 64 KB ram-backed stub at PA `0x0B000000`.

## 1. Finding in one sentence

Two legitimate upstream/emulator gaps are now addressed: (a) VR4131 `COP0 HIBERNATE` was unimplemented in GXemul 0.7.0 (`/* TODO */ goto bad;` halts the CPU); (b) the VRC4173 companion chip's secondary decode window at PA `0x0B000000` (documented in `docs/hardware/hardware.txt:204` as `0xab000060 CMU CLKMSK` / `0xab00011c GIU_PODATL` "on companion") had no backing device. Both gaps surface only after the Pass 31 KjCMU warm-reset lets the second-boot OAL run — the default 180–600 s cold-boot never reaches either path, but the diag-NAND scenario (`cardex_diag/All_nand_300_card_ex.dll_diag.bin`) hits both within 45 s.

## 2. Empirical reproduction and validation

### 2.1 Repro command (user-authoritative)

```
cd build-host
gtimeout 45s ./be300 --nand cardex_diag/All_nand_300_card_ex.dll_diag.bin \
    > cardex_diag_nand2_fixed_stdout.log 2> cardex_diag_nand2_fixed_stderr.log
```

### 2.2 Before Pass 34 (preserved in `build-host/cardex_diag_nand2_stderr.log`)

```
[KjCMU] warm reset triggered at pc=8007a174 (PA 0x0A00A0C4<=7, PA 0x0A00A0C8<=10)
[ memory READ: from non-existant paddr=0x000000000b000104 len=4 pc=0xffffffff8007b200 ]
[ memory READ: from non-existant paddr=0x000000000b000150 len=4 pc=0xffffffff8007b248 ]
… 12 more non-existant 0x0B000xxx accesses …
cpu: UNIMPLEMENTED instruction at 0xffffffff80079598
ffffffff80079598: 42000023        hibernate
[UI] No valid frame — cannot save screenshot
```

CPU halts mid-frame; no screenshot.

### 2.3 After Pass 34 (`build-host/cardex_diag_nand2_fixed_stderr.log`)

```
[KjCMU] warm reset triggered at pc=8007a174 (PA 0x0A00A0C4<=7, PA 0x0A00A0C8<=10)
[UI] Screenshot saved: screenshot_20260422_154241.bmp
```

- Zero `UNIMPLEMENTED` hits (HIBERNATE routes to `mips_cpu_cold_reset`).
- Zero `non-existant paddr=0x0B` warnings (0xAB window absorbs writes).
- Screenshot SHA `633ddd0ae106399b1040d2c08858ea24dca6fb17` — pure white (240×320×24, 230 397 bytes `0xFF`, 3 bytes `0x00`), matching the user's "framebuffer is redrawn white" observation.
- Guest serial cycles through `KLOADER` + `[EARLYDIAG] stream-driver XIP replacement active` + `phase=init` + `phase=afterspin` **twice** in 45 s, confirming hibernate → cold-reset → NK-decompress → diag-driver → hibernate loop.

## 3. Change details (citations mandatory per CLAUDE.md)

### 3.1 HIBERNATE in GXemul

File: `gxemul/src/cpus/cpu_mips_instr.c`
- New `X(hibernate_vr41xx)` at ~line 2518: syncs PC to the hibernate instruction, calls `mips_cpu_cold_reset(cpu)` (preserves SDRAM + peripherals, reinitialises CP0, PC←0xBFC00000), sets `next_ic = &nothing_call` to terminate the current dyntrans batch.
- `case COP0_HIBERNATE` in the decode switch at ~line 4439: routes to `instr(hibernate_vr41xx)` when `cpu_type.rev == MIPS_R4100`; routes to `instr(reserved)` with a one-shot warning otherwise (mirrors the existing SUSPEND/STANDBY pattern).

**Citation:** VR4131 UM rev 2.00 §6.1.3 (Software shutdown):
> "When the software executes the HIBERNATE instruction, the VR4131 sets the DRAM to self refresh mode and sets the MPOWER pin as inactive, then enters reset status. Recovery from reset status occurs when the POWER pin is asserted… A reset by software shutdown initializes the entire internal state except for the RTC timer and the PMU."

Semantically identical to the Pass 31 KjCMU VRC4173 GPIO-triggered cold reset at `src/be300_devices.c:119` — both converge on `mips_cpu_cold_reset()`; only the trigger differs.

### 3.2 0xAB000000 companion-window stub

File: `src/machine_be300.c` after the `be300_register_vrc4173_latch` call:
```
dev_ram_init(gxm, 0x0B000000ULL, 0x10000ULL,
    DEV_RAM_RAM, 0, "be300_companion_ab_window");
```

**Citation:** `docs/hardware/hardware.txt:204`:
```
0xab000060	CMU CLKMSK on companion ?
0xab00011c	GIU_PODATL on companion ?
```

This is a RAM-backed first pass — writes are latched, reads return last-written value (0 if never written). Real-HW register semantics for specific offsets in the user's observed access set (`0x104, 0x108, 0x10C, 0x110, 0x138, 0x13C, 0x150, 0x204, 0x208, 0x520, 0x524`) are not yet characterised; the stub silences the noise and lets any driver that does readback-of-own-writes proceed, but drivers that poll for hardware-supplied status bits will still spin. Upgrade from RAM to proper semantics is a follow-up for when card_ex.dll reverse-engineering pins a specific register's expected behaviour.

## 4. What this means for the broader boot stall

The default boot (`--nand ce/restore_images/All_nand_300.bin`) **does not reach either new code path** in 180 s or 600 s — zero accesses to PA `0x0B000xxx`, zero hits on PC `0x8007B1xx`, zero `COP0 HIBERNATE` decodes. The default-image stall at `Starting.bmp` (SHA `e8a8c83cd66b9327f50fc1827eada71fb028b332`) is therefore upstream of the HIBERNATE path and cannot be ascribed to it. The Pass 32 observation — coshell.exe spawns, ddi.dll fires `blit_dispatcher` 27×, `iFunc10` 27×, `iFunc18` 17×, fills the GDI off-screen surface at user VA `0x00140000` with 6 992 bytes, and **zero writes reach the primary FB user mapping at VA `0x001E0000`** — remains the active blocker.

The diag-NAND scenario (with card_ex.dll replaced by the user's stream-driver stub at `build-host/cardex_diag/All_nand_300_card_ex.dll_diag.bin`) is a **distinct** diagnostic path: it bypasses card_ex.dll's normal behaviour and drives NK OAL into a rebuild-from-scratch loop via the hibernate instruction. It's useful for probing what early-boot OAL code does when card_ex.dll is absent, but its stall state (pure-white framebuffer + infinite hibernate cycle) is not the same stall as the default boot.

## 5. Current blocker (active)

**The default cold-boot post-Pass-32 stall: ddi.dll blits are captured in an off-screen GDI surface but never flushed to the primary framebuffer user mapping.**

Pinning from Pass 32/34 probe evidence (preserved in `build-host/cold_stderr.log` at 42 180 lines and `build-host/p34_stderr.log` at 54 725 lines, both with `BE300_LIFECYCLE_PROBE=1`):

- Launcher table at user VA `0x0203b4d0`, 5 entries × 0x250 stride: `{0x0A shell, 0x14 device, 0x1E gwes, 0x3C coshell, 0x3B Boot}`.
- `launcher_module_ready_notify` hit=7 with `a0=0x3B` on the second boot proves Boot.exe signals ready post-warm-reset.
- `CREATEPROCESS hit=15 image="coshell.exe"` proves coshell spawns.
- `ddi_DrvEnableDriver_impl hits=2` / `ddi_blit_dispatcher_entry hits=27` / `ddi_iFunc10 hits=27` / `ddi_iFunc18 hits=17` prove ddi.dll's render pipeline executes many times.
- `gdi_surface_0x140000 writes=6992` — blits land in the off-screen surface.
- `fb_body_kseg1_writes writes=600934` — the only writes to PA `0xAA200000..0xAA226000` come from kernel-side OAL drawing the `Starting` splash (PC `0x80F037CC` / `0x80079130`).
- **Zero writes to user VA `0x001E0000`** (the primary FB user mapping per `project_pass32_framebuffer_blit_missing.md`).

Screenshot SHA on default boot (180 s, 600 s, and all Pass 33/34 runs): `e8a8c83cd66b9327f50fc1827eada71fb028b332`.

## 6. Next steps (ranked by falsifiability)

### 6.1 Probe ddi_iFunc10 source/dest surface pointers (top priority)

`ddi_iFunc10` at NK VA `0x01A5C9CC` fires 27× and is likely `DrvBitBlt` or `DrvCopyBits`. These WinCE DDI exports take source + destination surface pointers. Add one working-copy-only `fprintf` in `src/be300_probe.c` (pattern already established; enable via `BE300_LIFECYCLE_PROBE=1`) that logs `a0`/`a1` (source/dest) at `ddi_iFunc10` entry. Run 180 s and inspect:
- If destination is always the GDI off-screen surface (`0x00140000`): the bug is in the surface-swap / present path downstream of the blit, not in the blit itself.
- If some calls target VA `0x001E0000` but those specific writes don't land: TLB/ASID issue — our user VA mapping is wrong at those PCs.
- If no calls target VA `0x001E0000` ever: find who's supposed to initiate the "present" / "flip" by grepping gwes for `Present`, `FlushSurface`, or PDEV+offset accesses that match the primary surface's metadata.

Revert the `fprintf` before committing anything else.

### 6.2 Confirm DrvEnablePDEV binds primary correctly

`ddi_iFunc0_DrvEnablePDEV_guess` at `0x01A5D2F0` fires 2×. `DrvEnablePDEV` in WinCE DDI returns a `DHPDEV` that describes the primary surface, including its address. If our DDI returns a PDEV whose surface VA points at `0x00140000` (the GDI off-screen) instead of `0x001E0000` (the primary FB mapping), every subsequent blit lands in the off-screen and the primary never updates.

Probe target: `VirtualCopy`/`VirtualAlloc` calls from within `DrvEnablePDEV` — pin which VA the driver binds the primary to, and compare against what gwes expects.

### 6.3 card_ex.dll reverse-engineering (upgrades 0xAB000 stub)

Ghidra-decode `card_ex.dll` (extract from `ce/restore_images/All_nand_300.bin` or locate the XIP entry in the launcher/driver registry). Identify the specific register offsets in the `0x0B000000` window that the real driver reads back, and what values they expect. Upgrade the ram stub to a proper MMIO handler with those semantics. Likely-critical offsets based on the diag-NAND observed access pattern: `0x104, 0x108, 0x10C, 0x110, 0x138, 0x13C, 0x150, 0x204, 0x208, 0x520, 0x524`.

### 6.4 Out of scope / refuted

- **Audio / VRC4173 AIU**: refuted in Pass 33 (0 accesses to PA `0x0A0000E0..0x0A0000FC` in 180 s). Do not revisit without a probe-driven signal.
- **gwes_worker WFSO(0x000B6834)**: refuted by user's real-HW photo + Pass 34 `gwes_worker_wfso_6834_ret hits=2` (the wait does unblock in our emulator too). Not a stall site.
- **RAM seeds, `resume_ctx` shortcuts, guest patches**: forbidden by CLAUDE.md §Emulation Philosophy.

## 7. Supersedes / merges

- Supersedes the "Next pivot" section (§5) of `HANDOFF_POST_PASS33_AIU_HYPOTHESIS_REFUTED_2026-04-22.md`. The Pass 33 document is still valid as the historical record of the audio refutation and the probe code; its §5 recommendations are now folded into §6 above.
- Supersedes the "Remaining" section of `HANDOFF_POST_PASS32_LAUNCHER_BLOCKS_ON_BOOTEXE_READY_2026-04-22.md`. Pass 32's launcher-table evidence (§7, §10-§12) is still authoritative.

## 8. Pending commit decisions

Neither Pass 34 working-copy change is committed. If approved:

- **Commit A** (gxemul submodule, one commit): `cpu_mips_instr.c` +61 lines, message format per CLAUDE.md §Commit. Becomes the **10th delta commit** over upstream GXemul 0.7.0; the CLAUDE.md §"GXemul Submodule Hygiene" ledger at the top of that section must be updated from "Nine delta commits" to "Ten delta commits" with the new entry appended.
- **Commit B** (main repo, one commit): `src/machine_be300.c` +18 lines, citation in the commit message to `docs/hardware/hardware.txt:204`.
