# BE-300 Device Coverage Audit

One-time static enumeration of every physical address range claimed by the
emulator, cross-referenced with the device that handles it and the degree to
which the device is *actually modeled* versus *synthesized* versus *latched*.
Produced for Pass 35 reassessment; see `docs/HANDOFF_POST_PASS34_...md` for
the runtime context.

Classification:

- **full** — every register in range has modeled read/write semantics.
- **stub** — handler returns a synthesized constant, or silently absorbs
  writes without state changes. No hardware model.
- **latch** — catch-all store-on-write / return-on-read. Pretends to be RAM.
- **ram-mirror** — `dev_ram` mirror or backing store, not a device.
- **unmapped** — no registration; falls through `gxemul` default.

All citations are paths into this repo unless noted. `VR4131 UM` = NEC
Vr4131-um_200203.pdf; `VRC4173 UM` = U14579EJ2V0UM00.pdf.

## PA map

| PA start      | PA end        | Device label           | Registration site (file:line)     | Class   | Citation / Note |
|---------------|---------------|------------------------|-----------------------------------|---------|-----------------|
| `0x00000000`  | `0x04000000`  | SDRAM (64 MB)          | `be300.h:32–33`, `machine_hpcmips.c:305` (via fb init) | ram-mirror | Plain `dev_ram`. Actual SDRAM size is `cfg.sdram_size` (default 16 MB); low addresses match real hardware. |
| `0x09100000`  | `0x09102000`  | ROM-dispatcher stack   | `machine_hpcmips.c:322–323`       | ram-mirror | 8 KB for BE-300 ROM MIPS16 dispatcher. VA `0xA9100000` in kseg1. No datasheet reference; inferred from ROM disassembly. |
| `0x0A000000`  | `0x0A000300`  | VRC4173 latch (0a)     | `be300_devices.c:1098`            | latch   | Catch-all. **Covers PIU/AIU/KIU control regs per `hw_dump_vrc4173.txt:1–50`.** Runtime: ddi.dll reads/writes offset `0x00200..0x00234` 445M+ times (Pass 35 coverage dump) — suspected display control register block. Needs VRC4173 UM §video/LCD section verified. |
| `0x0A000300`  | `0x0A000360`  | PIU touchpanel         | `be300_devices.c:1458`            | full    | `hw_dump_vrc4173.txt:60–90`. Sequencer states 0..5, PENCHGINTR, bit-0 auto-clear on PIUCNTREG (Pass 25). |
| `0x0A000360`  | `0x0A008680`  | VRC4173 latch (0b)     | `be300_devices.c:1099`            | latch/mixed | Covers GIU (`0x1100..0x113F`) W1C semantics modeled; CF-companion (`0x1000..0x1FFF`) full; NAND-restore scoped; DMA-done synth at `0x1CD0`. Bulk remaining offsets are pure latch. |
| `0x0A007800`  | `0x0A007840`  | ScCmcu MCU stub        | `be300_devices.c:558–562`         | **stub**| **No hardware citation.** Returns 0 on all reads to simulate "instant command completion" for the Casio companion MCU (command byte 0x35 at reg `0x7834`, 0x5C at `0x7800`). Original comment: "On real hardware, a companion microcontroller receives the command... we return 0 on all reads." TODO: locate Casio "Kj" companion MCU datasheet or probe real hardware. |
| `0x0A008680`  | `0x0A0086A0`  | NS16550 SIU (VRC4173)  | `machine_hpcmips.c:87–93`         | full    | GXemul upstream `dev_ns16550`. `addr_mult=4`, 8-byte window. Covers base UART regs only. |
| `0x0A0086C0`  | `0x0A00A000`  | VRC4173 latch (1)      | `be300_devices.c:1100`            | latch   | Between SIU and NAND. Accesses during boot: zero observed in Pass 35 coverage (offset 0x86C0..0xA000 empty). |
| `0x0A00A000`  | `0x0A00A040`  | NAND controller (lo)   | `be300_devices.c:888`             | full    | VRC4173 NAND + KjCMU warm-reset trigger at `0xA0C4`/`0xA0C8`. Citations chain: `hardware.txt:192`, `hw_dump_vrc4173.txt:524`, NK `FUN_8007A140` (Pass 31). |
| `0x0A00A040`  | `0x0A00A050`  | Buttons input          | `be300_devices.c:1465`            | full    | Offsets `0x02`/`0x03` = `btn_set1`/`btn_set2`. Derived from `hardware.txt` and VRC4173 UM. |
| `0x0A00A050`  | `0x0A00D800`  | NAND controller (hi)   | `be300_devices.c:894`             | full    | Same device model; split around buttons carve-out. Stream-mode transfers, OOB ECC, restore-engine commands all modeled. |
| `0x0A00D800`  | `0x0A00E000`  | (gap, unmapped)        | —                                 | unmapped | Falls through `memory.c:553` default. Not observed in Pass 35 coverage. |
| `0x0A00E000`  | `0x0A020000`  | VRC4173 latch (2)      | `be300_devices.c:1101`            | latch   | Covers interrupt-status-register space. W1C semantics enforced at `0x060..0x078`, `0x1100..0x1140`, `0x1B00..0x1B30` per `hw_dump_vrc4173.txt:120–140`. Remaining offsets plain latch. |
| `0x0A200000`  | `0x0A226800`  | Framebuffer (LCD ctrl) | `machine_hpcmips.c:303`           | ram-mirror | `dev_fb_init`, 240×324 @ 16bpp (BIFB_D16_0000). Kernel-side OAL writes go here. Runtime: 600K+ writes in Pass 34 from `0x80F037CC`/`0x80079130`. |
| `0x0C000120`  | `0x0C000620`  | PPSH / wince_aux       | `be300_devices.c:1115` / `dev_ram_init:1130` | stub/conditional | With `--ppsh`: full protocol handler. Without `--ppsh`: a `dev_ram` page is created at the containing 4 KB frame, offset `0x400` seeded to `0x2320`. This is itself a synthesis shortcut. |
| `0x0F000000`  | `0x0F000800`  | VR4131 internal I/O    | `dev_vr41xx.c:1` (init)           | mostly-full | ICU, timer, RTC, clock, PMU, RTCL1, GIU; all modeled per VR4131 UM §3. Default-case handler (`dev_vr41xx.c:1027`) previously silent via `debug()`; Pass 35 adds coverage tag `vr41xx-default` class=`default`. Runtime: ~11 distinct `default` offsets hit, all w/ hits=2 (gwes driver probe writes). |
| `0x0F000800`  | `0x0F000808`  | NS16550 SIU (VR4131)   | (via `dev_vr41xx_init`)           | full    | Legacy upstream registration. |
| `0x0F000808`  | `0x0F000820`  | SIU extension          | `be300_devices.c:853`             | full    | `hw/siu.c`. Covers MCR/LSR/SCR beyond NS16550's 8-byte window. Pass 22. |
| `0x0F000820`  | `0x0F000828`  | NS16550 DSIU           | (via `dev_vr41xx_init`)           | full    | Legacy upstream registration. |
| `0x0F000828`  | `0x0F000880`  | DSIU extension         | `be300_devices.c:863`             | **stub**| **No hardware citation.** Reads return 0, writes silently accepted. Comment: "If a driver polls a DSIU status bit that doesn't naturally clear, a later pass can expand beyond a stub." Pass 22. |
| `0x1E000000`  | `0x20000000`  | CF window / ROM        | `be300_devices.c:1142`            | full    | With `--cf`: CF recovery window. Otherwise: ROM space loaded from `docs/hardware/be300_boot_rom.bin`. |
| `0x1FC00000`  | `0x1FC04000`  | Boot ROM               | `machine_be300.c` ROM load        | full    | 16 KB real-HW dump embedded at build time. CRC32 `0xFA3B5582`. MIPS32 + MIPS16 code; GXemul runs both natively. |
| `0x20000000`  | `0x40000000`  | SDRAM mirror           | `machine_hpcmips.c:317`           | ram-mirror | VR4131 29-bit PA bus — kseg1 `0xAA200000` leaks bit 29 to PA `0x2A200000`. Mirrors low 512 MB. |
| `0x80000000`  | `0xA0000000`  | SDRAM kseg0 mirror     | `machine_hpcmips.c:311`           | ram-mirror | NetBSD/hpcmips compat + BE-300 kernel mapping. |

## Runtime-Observed Forgiveness Hotspots (Pass 35, 30 s cold boot, `--mmio-coverage`)

Sorted by hit count; **bold** rows are where the emulator is likely hiding a real hardware behavior behind a too-forgiving default:

| Rank | Dev                         | Off      | Op | Hits        | Class     | First PC      | Note |
|------|-----------------------------|----------|----|-------------|-----------|---------------|------|
| 1    | ~~`vrc4173-latch`~~ → `vrc4173-ddi-ctrl-busy` | `0x00234`| R  | ~~524,965,081~~ | ~~latch~~ → **stub** | `0x01a53818`  | **Resolved Pass 35**: ddi.dll at PC `0x01A5382C` spun on `andi $25, $24, 1; bnel $25, $0, -2` waiting for bit 0 of `0x234` to clear. Fix: `be300_devices.c` read path returns `latched & ~1u` at this offset — instant-completion semantics (same pattern as ScCmcu / PIU bit-0). TODO 2026-04-23: no VRC4173 UM citation for this block; confirm real-HW semantics via BEDiag or a captured boot trace. |
| 2    | ~~`vrc4173-latch`~~ → `vrc4173-intr-aggr-stub` | `0x00A00`| R  | ~~956,773,151~~ | ~~latch~~ → **stub** | `0x80079938`  | **Resolved Pass 36**: NK OAL idle helper at VA `0x80079920..0x80079958` read bit 0 of PA `0x0A0008A0`/`0x0A000A00`/`0x0A00130C` to decide whether to issue `c0 0x22` SUSPEND (wake-on-interrupt) vs the "pending" handle path. After ddi.dll wrote non-zero to `0x00A00` (PC `0x01A53D04`) and pcmcia.dll wrote to `0x008A0` (PC `0x01911D04`), our catch-all latch kept bit 0 stuck at 1, forcing the idle helper to burn 32 M function-calls per wall second on the non-sleep path and starving ddi.dll's user-mode event wait at PC `0x01A53C84`. Fix: reads at `0x8A0` and `0xA00` return `latched & ~1u`. Real-HW (hw_dump_vrc4173.txt) shows both as `0x00000000` quiesced. TODO 2026-04-23: upgrade to a full interrupt-aggregate-status model driven by the lower-level enable/ACK regs at `0x1120`/`0x1B20`/`0x112C`. |
| 3    | ~~`vrc4173-latch`~~ → `vrc4173-intr-aggr-stub` | `0x008A0`| R  | ~~4,541~~      | ~~latch~~ → **stub** | `0x80079924`  | Resolved Pass 36 in the same edit as `0x00A00`. Real HW: `0x00000000`. |
| 2    | `vrc4173-nand`              | `0x0B000`| R  | 1,023,528   | known     | `0x9fc01a98`  | NAND read flow, expected. |
| 3–13 | `vrc4173-nand` write set    | various  | W  | 25K–76K     | known     | 0x9fc01e** range | NAND controller config, expected. |
| 14   | `vrc4173-cf-companion`      | `0x01B50`| R  | 29,462      | known     | `0x0198ce64`  | pcmcia.dll, expected. |
| —    | `vrc4173-dma-done-synth`    | `0x01CD0`| R  | 9,820       | **stub**  | `0x019a55c0`  | Per-page NAND DMA completion synthesis; audited OK. |
| —    | `vrc4173-scmcu-stub`        | `0x07834`| R  | ~2          | **stub**  | `0x9fc00d46`  | ROM-era MCU probe; confirmed behavior vs real HW pending. |
| —    | `vr4131-dsiu-stub`          | `0x00038`| RW | 2/2         | **stub**  | `0x0194988c`  | gwes driver probe. Expected but *unverified* against real HW. |
| —    | `vr41xx-default`            | various  | W  | 2 each      | default   | `0x800a5e*`   | 11 offsets in the `0x000A8..0x0015E` range; gwes SIO probe. Likely SIOCNT/SIOMOD — VR4131 UM §9 (Serial Port) offsets. **Candidate default → full upgrade** for Pass 36. |

## Summary

- Modeled "full" ranges total ~14 devices; classifications are consistent between static inspection and runtime `--mmio-coverage` output.
- Five documented "forgiveness" shortcuts persist: VRC4173 catch-all latch (2 segments), ScCmcu MCU stub, DSIU extension stub, DMA-done synthesis, PPSH `dev_ram` fallback.
- The current cold-boot stall (Pass 34) maps cleanly onto the #1 forgiveness hotspot: a single latched VRC4173 offset (`0x00234`) touched at ~17.5M reads/sec by a 2-instruction tight loop in ddi.dll. That is a high-confidence next-probe target that would not have been visible without `--mmio-coverage`.

## Follow-ups flagged by this audit

1. Decode what real VRC4173 hardware lives at offset `0x00200..0x00234` — suspected display / LCD sub-block per VRC4173 UM. If so, the latch is actively misleading ddi.dll.
2. Upgrade `vr41xx-default` handler hits in the `0x000A8..0x0015E` range to `full` — likely SIO/SIO2 regs per VR4131 UM §9 (`SIO2STA`/`SIO2MOD` at `0x00AA`/`0x00AC`).
3. Add real hardware citation to ScCmcu stub and DSIU stub, or mark them with an explicit TODO-with-date in the code.
4. Deferred (W5 of `plans/streamed-painting-dragonfly.md`): on-shutdown "blame ring" of the last-K unique MMIO accesses — would make the #1 latch hotspot obvious even without eyeballing the full table.
