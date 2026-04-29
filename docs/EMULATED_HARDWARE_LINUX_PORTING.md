# BE-300 Emulated Hardware Reference for Linux Porting

This document describes the BE-300 hardware interface exposed by the
emulator, with enough detail for a Linux developer to build board support and
drivers for the emulated machine. It is written from the emulator-visible ABI:
what a guest kernel can read, write, map, and receive as interrupts.

The emulator is being developed to cold-boot the real Casio BE-300 Windows CE
3.0 NAND image, so some behavior is complete hardware modeling, some behavior
is a surveyed readback latch, and some behavior is a compatibility shim added
to let the unmodified WinCE drivers pass a known hardware checkpoint. Treat the
status labels below as part of the specification.

## Status Labels

| Label | Meaning for a Linux driver |
| --- | --- |
| Hardware-confirmed | Backed by the VR4131/VRC4173 manuals, real BE-300 dumps, or both. A Linux driver can rely on it as the intended hardware model. |
| Emulator-modeled | Implemented as an active device in the emulator and expected to be stable for guests, but not always a complete silicon model. |
| Survey latch | Register bytes are seeded from real hardware and guest writes usually latch. Useful for platform probing, not enough for a full driver unless behavior is also documented. |
| Compatibility shim | A behavior added to match an observed boot path. Use only if unavoidable; expect it to be replaced by a better hardware model later. |
| Stub/unknown | Reads/writes are accepted or simplified to avoid bus faults. Do not build a driver contract around it without adding a better model. |

Primary source files for this document:

- `src/be300.h`
- `src/main.c`
- `src/machine_be300.c`
- `src/be300_devices.c`
- `src/hw/*.c` and `src/hw/*.h`
- `gxemul/src/machines/machine_hpcmips.c`
- `gxemul/src/devices/dev_vr41xx.c`
- `docs/CURRENT_STATUS.md`
- `docs/HARDWARE_GROUND_TRUTH.md`
- `docs/HARDWARE_SURVEY_SYNTHESIS.md`

## Binary-Only Emulator Contract

The current emulator binary accepts three boot styles:

| Mode | CLI | Hardware coverage |
| --- | --- | --- |
| WinCE NAND cold boot | `--nand <image>` | Full BE-300 model: VR4131, VRC4173 latch, NAND, framebuffer, touch, buttons, CF windows, PPSH defaults. |
| CF restore boot | `--restore --cf <image>` | Full BE-300 model plus CF image visible to the ROM/NANDWRITER recovery path. |
| Positional ROM image | `<rom.bin>` | Baseline GXemul hpcmips setup only. The BE-300 custom VRC4173/NAND/input devices are not registered in this path. |

For a Linux port that needs all emulated BE-300 hardware, use the `--nand` or
`--restore` path, or update the emulator to register the same custom devices in
ROM-image mode. The old `--kernel` ELF loader path was removed from this
project. A binary-only Linux bring-up therefore needs one of these strategies:

- Package a Linux loader/kernel into a BE-300-compatible NAND image and enter
  through the real reset ROM and SPL-style flow.
- Use `--restore --cf` to boot a small recovery payload that can load Linux
  from CF, if the payload obeys the existing BE-300 ROM/restore hardware
  protocol.
- Use positional ROM mode only for very early CPU experiments; it does not
  expose the full custom hardware described here unless the emulator is changed.

The normal full-hardware invocation is:

```sh
./be300 --nand ce/restore_images/All_nand_300.bin
```

Optional useful flags while developing Linux support:

| Flag | Use |
| --- | --- |
| `--cf <image>` | Attach a 512-byte-sector CF image. Useful for block-driver work. |
| `--ne2000` | Attach the PCMCIA NE2000 Ethernet card model. |
| `--ppsh` | Enable the PPSH debug-shell transport at `0x0C000120/0x0C000520`. |
| `--rtc-host-time` | Initialize the VR4131 RTC from host local time. Native cold boots leave this off by default; the web frontend enables it by default. |
| `--pcconnect-time-sync` | Enable the experimental PC Connect serial/dock model. Not needed for normal kernel bring-up. |
| `--sdram <MB>` | Set SDRAM size, valid range 1-64 MB. Default is 16 MB. |
| `--speed <N>` | Throttle to approximately N million guest instructions/sec. Default is 166. `0` means unthrottled. |
| `--log-mmio` | Print every MMIO access. Very noisy but useful while writing drivers. |
| `--mmio-coverage` | Print first-hit MMIO coverage and a shutdown table. |
| `--detect-stall` | Report tight guest spin loops. Useful when a driver waits for a bit that the emulator does not yet model. |

## CPU and Core Platform

| Property | Value |
| --- | --- |
| CPU | NEC VR4131, VR41xx MIPS little-endian |
| PRId | `0x00000C80` |
| Endianness | Little-endian |
| Default board clock model | 166 MHz CPU class, `machine->emulated_hz = 131072000` |
| Main interrupt pin | VRIP aggregates to MIPS CPU interrupt 2 (`Cause.IP2`) |
| Addressing | 32-bit MIPS; normal kseg0/kseg1 aliases apply |
| Reset vector | Virtual `0xBFC00000`, physical `0x1FC00000` |
| Reset ROM | 16 KiB masked ROM at physical `0x1FC00000` on NAND/restore boots |

The emulator uses GXemul's MIPS CPU engine for CP0, TLB, exceptions, Count,
Compare, and instruction execution. Linux should use normal MIPS low-level
exception and TLB setup. WinCE uses mixed 4 KiB and 16 KiB TLB mappings on real
hardware; Linux can use its normal VR41xx/MMU configuration as long as it does
not assume a fixed single page size in hardware.

CP0 Count/Compare is emulator-driven by instruction progress. The VR41xx timer
and RTC helpers in the emulator use this emulated progress rather than host
wall-clock sleeps for core timer delivery. For Linux, the safest initial timer
bring-up is:

1. Use CP0 Count/Compare for the scheduler clock if your MIPS port already has
   that path.
2. Add VR4131 RTC/RTCL1 support after interrupts are stable.
3. Avoid depending on exact host wall-clock cadence from a tight loop; use
   interrupt-driven progress and timeouts.

## Physical Memory Map

All addresses below are physical. KSEG1 addresses are formed by OR'ing with
`0xA0000000`; for example, physical `0x0A200000` is uncached virtual
`0xAA200000`.

| Range | Size | Status | Description |
| --- | ---: | --- | --- |
| `0x00000000` | default 16 MiB, configurable up to 64 MiB | Emulator-modeled | Main SDRAM. Default comes from `--sdram` default in `src/main.c`. |
| `0x09100000-0x09101FFF` | 8 KiB | Compatibility shim | Extra RAM used by the real ROM MIPS16 boot dispatcher stack. |
| `0x0A000000-0x0A01FFFF` | 128 KiB sparse | Mixed | VRC4173 primary companion window, split among active devices and latches. |
| `0x0A200000` | framebuffer allocation | Emulator-modeled | LCD framebuffer: 240x320 visible, 256-pixel stride, 16 bpp. |
| `0x0B000000-0x0B00FFFF` | 64 KiB | Survey latch/compatibility shim | Secondary companion decode window used by PCMCIA/OAL code. |
| `0x0B400000-0x0B6FFFFF` | 3 MiB | Compatibility shim | PCMCIA attribute-memory/CIS window. |
| `0x0C000120-0x0C00061F` | 0x500 | Emulator-modeled/stub | PPSH/parallel-port auxiliary mailbox when `--ppsh` is enabled; idle RAM/stub otherwise. |
| `0x0F000000-0x0F0007FF` | 2 KiB | Emulator-modeled | VR4131 internal peripherals: BCU, CMU, ICU, PMU, RTC, GIU/GPIO. |
| `0x0F000800-0x0F00081F` | 32 bytes | Emulator-modeled | VR4131 SIU, ns16550-compatible base plus extension registers. |
| `0x0F000820-0x0F00087F` | 0x60 | Mixed | VR4131 DSIU, ns16550 base plus stubbed extension registers. |
| `0x140003E0` | legacy | Stub/unknown | GXemul legacy `pcic` registration on VR41xx GIU. Not BE-300 PCMCIA support. |
| `0x15000000-0x15FFFFFF` | 16 MiB | GXemul mirror | Mirror of `0x14000000` ISA-like space. Usually irrelevant. |
| `0x1E000000-0x1E000FFF` | 4 KiB | Emulator-modeled | CF boot window registered as `be300_cf_window`. |
| `0x1FC00000-0x1FC03FFF` | 16 KiB | Hardware-confirmed | BE-300 masked boot ROM on NAND/restore boots. |
| `0x20000000-0x3FFFFFFF` | 512 MiB | Compatibility mirror | Mirror of low physical space. Handles BE-300 address-line behavior seen by display mappings. |
| `0x80000000-0x9FFFFFFF` | 512 MiB | GXemul mirror | Mirror of low physical space for hpcmips framebuffer expectations. |

Important VRC4173 primary-window splits:

| Range | Owner |
| --- | --- |
| `0x0A000000-0x0A0002FF` | VRC4173 latch segment `vrc4173_0a` |
| `0x0A000300-0x0A00035F` | PIU touch panel device |
| `0x0A000360-0x0A00867F` | VRC4173 latch segment `vrc4173_0b` |
| `0x0A008680-0x0A00869F` | VRC4173 companion SIU, ns16550-compatible with `addr_mult=4` |
| `0x0A0086C0-0x0A009FFF` | VRC4173 latch segment `vrc4173_1` |
| `0x0A00A000-0x0A00A03F` | NAND low register segment |
| `0x0A00A040-0x0A00A04F` | Button input device |
| `0x0A00A050-0x0A00D7FF` | NAND high register segment |
| `0x0A00E000-0x0A01FFFF` | VRC4173 latch segment `vrc4173_2` |

## Interrupt Topology

The VR4131 interrupt controller is modeled in `dev_vr41xx.c`. It registers
VRIP interrupt lines 0-25 and GIU sub-lines 0-31. VRIP asserts MIPS CPU
interrupt 2 (`Cause.IP2`). The emulator treats IP2 as edge-triggered for
delivery cleanup so RTCL1 and shared level sources do not repeatedly re-enter
WinCE's ISR after a single delivered edge.

VRIP source numbers used by the emulator:

| VRIP line | Name | Typical guest source |
| ---: | --- | --- |
| 0 | BAT | Battery-related source, mostly not modeled. |
| 1 | POWER | PMU/power. |
| 2 | RTCL1 | RTC long timer 1. |
| 3 | ETIMER | Elapsed timer/Compare-style VR41xx timer. |
| 5 | PIU | Native PIU source, but BE-300 touch is routed through GIU0. |
| 6 | AIU | Audio interrupt source, mostly latch/stub. |
| 7 | KIU | Keyboard interface source, mostly not used by BE-300 input. |
| 8 | GIU | GPIO/general interrupt unit cascade. |
| 9 | SIU | Serial interface unit. |
| 11 | SOFT | Software interrupt. |
| 16 | RTCL2 | RTC long timer 2. |
| 20 | FIR | FIR. |
| 21 | DSIU | Debug SIU. |
| 22 | PCI/LCD | Legacy VR41xx constant, not BE-300 PCI. |
| 23-25 | SCU/CSI/BCU | Mostly unimplemented. |

VR4131 ICU registers live at physical `0x0F000080`:

| Offset | Name | Behavior |
| ---: | --- | --- |
| `0x00` | `SYSINT1REG` | Pending system interrupt 1 bits. Writes clear written bits in the GXemul model. |
| `0x08` | `GIUINTLREG` | Low GIU pending summary. Writes clear written bits. |
| `0x0C` | `MSYSINT1REG` | System interrupt 1 mask. Bit value 1 enables delivery. |
| `0x14` | `MGIUINTLREG` | Low GIU mask. Bit value 1 enables delivery. |
| `0x20` | `SYSINT2REG` | Pending system interrupt 2 bits. |
| `0x26` | `MSYSINT2REG` | System interrupt 2 mask. |

Important `SYSINT1REG` bit definitions:

| Bit | Name | Meaning |
| ---: | --- | --- |
| 0 | `POWER` | PMU/power. |
| 2 | `RTC` | RTCLONG1. |
| 3 | `ETIME` | Elapsed-time compare. |
| 5 | `PIU` | Native PIU. |
| 6 | `AIU` | Audio. |
| 7 | `KIU` | Keyboard interface. |
| 8 | `GIU` | Cascaded VRC4173/GIU source. |
| 9 | `SIU` | Serial. |
| 10 | `WDOG` | Watchdog. |
| 11 | `SOFT` | Software interrupt. |

BE-300-specific cascades:

| Device | Route |
| --- | --- |
| Touch panel | MIPS IP2 -> VRIP line 8 GIU -> GIU line 0 -> VRC4173 offset `0x0004` bit 9 -> PIUINTREG at `0x0A000304`. |
| Buttons | MIPS IP2 -> VRIP line 8 GIU -> GIU line 0 -> VRC4173 offset `0x0004` bit 1. |
| CF/PCMCIA | MIPS IP2 -> VRIP line 8 GIU -> GIU line 0 -> VRC4173 offset `0x0004` bit 0. |
| PC Connect CommMode | MIPS IP2 -> VRIP line 8 GIU -> GIU line 0 -> VRC4173 offset `0x0004` bit 4, only with `--pcconnect-time-sync`. |
| VR4131 SIU | MIPS IP2 -> VRIP line 9. |
| VR4131 DSIU | MIPS IP2 -> VRIP line 21. |
| RTCL1 | MIPS IP2 -> VRIP line 2, status also appears in RTCINTREG. |

VRC4173 interrupt-like registers use write-one-to-clear semantics in the
emulator for these ranges:

| Offset range from `0x0A000000` | Notes |
| --- | --- |
| `0x0060` | VRC4173 `SYSINT1REG` aggregate. Read-only for writes in the emulator; writes to bytes `0x060/0x061` do not clear it. |
| `0x0062-0x006A` | Level-2 status registers. W1C. |
| `0x1100-0x113F` | GIU/wake/status area. W1C except selected suspend latches. |
| `0x1B00-0x1B2F` | Interrupt status/mask/ack area. W1C. |

Linux driver implication: use normal mask/status/ack sequencing. Do not assume
writing zero clears a status bit in these VRC4173 ranges.

## VR4131 Internal Peripherals

The VR4131 internal block is mapped at physical `0x0F000000`. Register accesses
are little-endian. Most VR4131 registers are 16-bit and 2-byte aligned.

### BCU: Bus Control Unit

Status: emulator-modeled latch with hardware-confirmed fixed readbacks.

Base: `0x0F000000`

| Offset | Name | Linux use |
| ---: | --- | --- |
| `0x00` | `BCUCNTREG1` | Latches writes. ROM/boot firmware programs this. |
| `0x02` | `BCUCNTREG2` | Latches writes. |
| `0x04` | `ROMSIZEREG` | Latches writes. |
| `0x06` | `ROMSPEEDREG` | Latches writes. |
| `0x08` | `IO0SIZEREG` | Latches writes. |
| `0x0A` | `IO0SPEEDREG` | Latches writes. |
| `0x0C` | `IO1SPEEDREG` | Latches writes. |
| `0x10` | `REVIDREG` | Seeded readback `0x00005002` in the GXemul VR4131 device. |
| `0x14` | `CLKSPEEDREG` | Seeded readback low halfword `0x020C`. |

Linux should not need an active BCU driver beyond early board setup and
read-only identification. If Linux reprograms bus timing, the emulator will
latch the writes but does not model timing effects.

### CMU: Clock Mask Unit

Status: emulator-modeled latch.

Base: `0x0F000060`

| Offset | Name | Behavior |
| ---: | --- | --- |
| `0x00` | `CMUCLKMSK` | 16-bit clock mask. Writes latch, reads return last value with sticky bits from the implementation. |

Linux can implement this as a simple clock gate register if desired. The
emulator does not currently gate peripheral behavior based on this mask.

### PMU: Power Management Unit

Status: emulator-modeled for reset cause, HALTimer, and software reset.

Base: `0x0F0000C0`

| Offset | Name | Behavior |
| ---: | --- | --- |
| `0x00` | `PMUINTREG` | Reset-cause/status bits, write-one-to-clear. |
| `0x02` | `PMUCNTREG` | Control. Bit 2 pets/rearms the HALTimer. |
| `0x04` | `PMUTCLKDIVREG` | Latches writes. |
| `0x06` | `PMUCNT2REG` | Control 2. Bit 4 triggers immediate cold reset. |
| `0x08` | `PMUWAITREG` | Latches writes. |
| `0x0C` | `PMUDIVREG` | Latches writes. |

`PMUINTREG` bits:

| Bit | Name | Meaning |
| ---: | --- | --- |
| 0 | `RTCRST` | Power-on reset. |
| 1 | `TIMOUTRST` | HALTimer timeout reset. |
| 2 | `RSTSW` | Reset switch or software reset. |
| 3 | `BATTINH` | Battery low/inhibit. |

At power-on the emulator arms a roughly 4-second HALTimer window. A guest that
writes bit 2 (`HALTIMERRST`) in `PMUCNTREG` pets the timer. If the timer
expires, the emulator stages `TIMOUTRST` and cold-resets the CPU with SDRAM
preserved. If the guest writes bit 4 (`SOFTRST`) in `PMUCNT2REG`, the emulator
stages `RSTSW` and immediately cold-resets the CPU.

Linux implication: an early PMU driver should acknowledge reset-cause bits and
pet or disable the HALTimer using the same sequence as real VR4131 firmware.
If Linux hangs early with repeated resets, check PMU HALTimer handling first.

### RTC and Timers

Status: emulator-modeled for elapsed-time, compare, RTCINT, and RTCL1 delivery;
calendar fields are simplified.

Base: `0x0F000100`

| Offset | Name | Behavior |
| ---: | --- | --- |
| `0x00` | `ETIMELREG` | Low 16 bits of 48-bit elapsed-time counter. |
| `0x02` | `ETIMEMREG` | Middle 16 bits of elapsed-time counter. |
| `0x04` | `ETIMEHREG` | High 16 bits of elapsed-time counter. |
| `0x08` | `ECMPLREG` | Elapsed-time compare low. |
| `0x0A` | `ECMPMREG` | Compare middle. |
| `0x0C` | `ECMPHREG` | Compare high. |
| `0x10` | `RTCL1LREG` | RTCL1 divisor/calendar low. Writes also configure RTCL1 cadence. |
| `0x12` | `RTCL1HREG` | RTCL1 high. |
| `0x14` | `RTCL1CNTLREG` | RTCL1 counter low. |
| `0x16` | `RTCL1CNTHREG` | RTCL1 counter high. |
| `0x18` | `RTCL2LREG` | RTCL2 low. |
| `0x1A` | `RTCL2HREG` | RTCL2 high. |
| `0x1C` | `RTCL2CNTLREG` | RTCL2 counter low. |
| `0x1E` | `RTCL2CNTHREG` | RTCL2 counter high. |
| `0x20` | `TCLKLREG` | TClock low. |
| `0x22` | `TCLKHREG` | TClock high. |
| `0x24` | `TCLKCNTLREG` | TClock counter low. |
| `0x26` | `TCLKCNTHREG` | TClock counter high. |
| `0x3E` | `RTCINTREG` | RTC interrupt status. Writes acknowledge selected bits. |

`RTCINTREG` bits:

| Bit | Name |
| ---: | --- |
| 0 | elapsed-time compare |
| 1 | RTCLONG1 |
| 2 | RTCLONG2 |
| 3 | TCLOCK |

Native BE-300 cold-boot behavior intentionally starts with an
uninitialized/default RTC presentation so WinCE can ask the user to set the
date/time. `--rtc-host-time` is an emulator convenience path that seeds the
elapsed counter from host local time using WinCE's 1850 epoch; the web frontend
enables that path by default. A Linux RTC driver should still treat the silicon
interface as an elapsed-time/long-timer device, with calendar policy coming
from bootloader or emulator configuration rather than from a separate CMOS
calendar block.

### GIU/GPIO

Status: mixed emulator-modeled latch and board-strap readback.

Base: `0x0F000140`

| Offset | Name | Behavior |
| ---: | --- | --- |
| `0x00` | `GIUIOSELL` | Latches writes. |
| `0x02` | `GIUIOSELH` | Latches writes. |
| `0x04` | `GIUPIODL` | Reads board strap/input value `0xAAE2` on BE-300. Writes are ignored for input bits. |
| `0x06` | `GIUPIODH` | Latches writes. |
| `0x08` | `GIUINTSTATL` | W1C interrupt status low. Also clears GIU summary bits. |
| `0x0A` | `GIUINTSTATH` | W1C interrupt status high. |
| `0x0C` | `GIUINTENL` | Latches writes. |
| `0x0E` | `GIUINTENH` | Latches writes. |
| `0x10` | `GIUINTTYPL` | Latches writes. |
| `0x12` | `GIUINTTYPH` | Latches writes. |
| `0x14` | `GIUINTALSELL` | Latches writes. |
| `0x16` | `GIUINTALSELH` | Latches writes. |
| `0x18` | `GIUINTHTSELL` | Latches writes. |
| `0x1A` | `GIUINTHTSELH` | Latches writes. |

Linux implication: use this as a simple GPIO/IRQ controller for VR4131-side
GPIO only. BE-300 user buttons and touch are not exposed as plain GIU pins;
they are VRC4173 devices that summarize through GIU line 0.

### VR4131 SIU and DSIU

Status: SIU is emulator-modeled, DSIU is partially modeled/stubbed.

VR4131 SIU base: `0x0F000800`

| Offset | Register | Notes |
| ---: | --- | --- |
| `0x00` | RBR/THR/DLL | ns16550-compatible base register. |
| `0x02` | IER/DLM | ns16550-compatible. |
| `0x04` | IIR/FCR | ns16550-compatible. |
| `0x06` | LCR | ns16550-compatible. |
| `0x08` | MCR | Extension window handled by `siu_read/siu_write`. |
| `0x0A` | LSR | THR empty/TEMT and data-ready bits. |
| `0x0C` | MSR | Modem status. |
| `0x0E` | SCR | Scratch. |

VR4131 DSIU base: `0x0F000820`

- The first ns16550-sized region is registered by GXemul.
- Extension range `0x0F000828-0x0F00087F` accepts writes and returns zero.
- Existing WinCE debug output uses DSIU base registers for polling LSR and
  writing THR. The extension range is not a complete hardware model.

The VRC4173 companion SIU is separate and lives at `0x0A008680`.

## Framebuffer and Display Engines

Status: framebuffer is emulator-modeled; several VRC4173 display acceleration
blocks are compatibility models for observed WinCE DDI operations.

### Linear Framebuffer

Base: `0x0A200000`

| Property | Value |
| --- | --- |
| Visible width | 240 pixels |
| Visible height | 320 pixels |
| Allocated stride | 256 pixels |
| Allocated height | 324 rows |
| Bits per pixel | 16 |
| Format | RGB565-like 16-bit little-endian framebuffer |
| KSEG1 alias used by WinCE | `0xAA200000` |

The extra stride and rows are intentional. The SPL and WinCE display paths
write aligned rows and can touch row 320. A Linux framebuffer driver should
advertise 240x320 visible resolution while using a 512-byte line length
(`256 * 2`) for memory pitch.

Minimum Linux driver behavior:

- Map physical `0x0A200000` uncached or write-combined.
- Use 16 bits per pixel.
- Set `xres = 240`, `yres = 320`, `line_length = 512`.
- Do not assume the framebuffer allocation ends exactly at `240 * 320 * 2`.

### VRC4173/Casio Display Fill Block

Base: `0x0A000200`

Status: compatibility shim with partial active modeling.

Observed registers:

| Offset | Meaning in emulator |
| ---: | --- |
| `0x200` | Mode. Mode 0 is solid fill. Mode 2 is transparent 1-bpp glyph expansion. |
| `0x204` | RGB565 color. |
| `0x208` | Width. |
| `0x20C` | Height. |
| `0x210` | Destination byte offset low. |
| `0x214` | Destination byte offset high. |
| `0x220` | Source byte offset low for mode 2. |
| `0x224` | Source byte offset high for mode 2. |
| `0x228` | Source bit offset for mode 2. |
| `0x234` | Start/busy bit. Writing bit 0 starts; reads return bit 0 clear after immediate completion. |

Linux can ignore this block and use the linear framebuffer directly. A Linux
accelerated framebuffer driver could use mode 0 for fills and mode 2 for glyphs,
but the command set is not complete.

### VRC4173/Casio Blit Block

Base: `0x0A000A00`

Status: compatibility shim with one modeled command.

Observed registers:

| Offset | Meaning in emulator |
| ---: | --- |
| `0xA00` | Trigger/status. Bit 0 starts and then self-clears. |
| `0xA04` | Command. Observed command `0x81` means SDRAM-to-framebuffer copy. |
| `0xA08` | Word count. |
| `0xA10` | Source physical or kseg address. |
| `0xA14` | Destination framebuffer byte offset or framebuffer address. |

Linux should initially ignore this block. Use the framebuffer directly.

## VRC4173 Primary Companion Window

Status: mixed survey latch, active device dispatch, and compatibility shims.

Primary base: `0x0A000000`

The emulator seeds a 128 KiB VRC4173 latch array from hardware dumps and then
overlays active devices for specific ranges. Unknown reads usually return the
last written byte or the seeded value. Unknown writes usually latch.

Important platform identification and control offsets:

| Offset | Status | Meaning |
| ---: | --- | --- |
| `0x0000` | Survey latch | Core/PIU-adjacent value seeded to `0x00000002`. |
| `0x0004` | Emulator-modeled summary | GIRQ0 source register. Bit 0 CF, bit 1 buttons, bit 4 CommMode, bit 9 touch. |
| `0x0060` | Hardware-confirmed behavior | VRC4173 `SYSINT1REG` aggregate; read-only in emulator writes. |
| `0x08A0` | Compatibility shim | Interrupt aggregate bit 0 reads as clear. |
| `0x0980-0x09E7` | Survey latch/emulator-modeled host effect | Buzzer/BLG registers. Writes can trigger host buzzer pulses. |
| `0x0A00` | Compatibility shim | Display blit trigger/status. |
| `0x1128` | Survey latch/emulator-modeled host effect | Buzzer/CMM word. |
| `0x1440-0x149F` | Emulator-modeled/stub | VRC4173 USB OpenHCI operational registers. Root hub remains disconnected unless PC Connect mode uses related state. |
| `0x1B00-0x1B2F` | Mixed | Interrupt/status/mask area, W1C handling. |
| `0x7800-0x783F` | Stub/unknown | ScCmcu companion MCU command area; reads return zero to mean instant command completion. |
| `0x8004` | Emulator-modeled with PC Connect option | CommMode pending/mask word. |
| `0x8010` | Emulator-modeled with PC Connect option | CommMode socket identity. |
| `0x8680` | Emulator-modeled | VRC4173 companion SIU base. |
| `0xA0C0` | Hardware-confirmed strap | Board ID, returns `0x00007100`. Writes are ignored for this word. |

Board ID `0x7100` is the main BE-300 companion-chip identity. A Linux board
probe can use physical `0x0A00A0C0` to identify the emulated machine.

### VRC4173 USB Operational Window

Base: `0x0A001440`

Status: emulator-modeled/stub.

This is modeled as an OpenHCI-like operational register window:

| Offset | Meaning |
| ---: | --- |
| `0x00` | HcRevision, returns `0x0010`. |
| `0x0C` | HcInterruptStatus. |
| `0x10` | HcInterruptEnable. |
| `0x14` | HcInterruptDisable/readback of enable state. |
| `0x50` | Root hub descriptor/control-like register. |
| `0x54` | Port 0 status. |
| `0x58` | Port 1 status. |

The root hub normally has no connected USB device. A generic OHCI driver may
probe this, but useful USB device functionality is not implemented. Bring up
serial/debug/storage first.

### Buzzer

Status: survey latch with host-side effect.

Relevant offsets:

| Offset | Meaning |
| ---: | --- |
| `0x0980-0x09E7` | BLG/buzzer-like register bank. Seeded words include `0x00000001`, `0x00000202`, `0x00000F70`, `0x00000000`. |
| `0x1128-0x112B` | CMM/buzzer-related word, seeded to `0x00000001`. |

Writes that transition control bits can pulse the host UI buzzer. Linux can
ignore this until audio/input feedback support is desired.

## Serial Devices

The emulator exposes three serial-like devices:

| Device | Base | Status | Interrupt |
| --- | ---: | --- | --- |
| VR4131 SIU | `0x0F000800` | ns16550-compatible plus extension handling | VRIP 9 |
| VR4131 DSIU | `0x0F000820` | ns16550 base, extension stub | VRIP 21 |
| VRC4173 SIU | `0x0A008680` | ns16550-compatible, `addr_mult=4` | VRIP 9 in current setup |

Recommended Linux console path:

1. Start with the VR4131 SIU at `0x0F000800` if you control the firmware entry.
2. Use the VRC4173 SIU at `0x0A008680` for BE-300/PC Connect-compatible serial
   work. Its ns16550 registers are spaced by 4 bytes because GXemul registers
   it with `addr_mult=4`.
3. Treat DSIU extension registers as incomplete. The base ns16550 registers are
   usable for simple output, but the wider debug serial hardware is not fully
   modeled.

For ns16550 drivers:

- Register 0 is THR/RBR/DLL.
- Register 1 is IER/DLM.
- Register 2 is IIR/FCR.
- Register 3 is LCR.
- Register 5 is LSR.
- LSR bit 5 (`THRE`) and bit 6 (`TEMT`) indicate transmitter readiness.

## Touch Panel: VRC4173 PIU

Status: emulator-modeled for the WinCE touch driver path.

Base: `0x0A000300`, size `0x60`

Input coordinate range accepted from the host:

| Axis | Range |
| --- | --- |
| X | `0..239` |
| Y | `0..359`; `320..359` represents the BE-300 icon strip below the LCD. |

Register layout:

| Offset | Name/role | Behavior |
| ---: | --- | --- |
| `0x00` | `PIUCNTREG` | Control plus dynamic state. Bit 0 is command/start and self-clears. Bits `12:10` expose PADSTATE. Bit 13 is pen-down state. |
| `0x04` | `PIUINTREG` | Low byte pending status, high byte interrupt mask. Pending bits are W1C. |
| `0x08` | `PIUSIVLREG` | Scan interval. Reset/default `0x05DC`. Low 11 bits are interval units. |
| `0x0C` | `PIUSTBLREG` | Stabilization interval. Reset/default `0x00C8`. Low 6 bits are used. |
| `0x20` | Page 0 Y+ ADC | Coordinate buffer page 0. |
| `0x24` | Page 0 Y- ADC | Coordinate buffer page 0. |
| `0x28` | Page 0 X- ADC | Coordinate buffer page 0. |
| `0x2C` | Page 0 X+ ADC | Coordinate buffer page 0. |
| `0x50` | Page 1 Y+ ADC | Coordinate buffer page 1. |
| `0x54` | Page 1 Y- ADC | Coordinate buffer page 1. |
| `0x58` | Page 1 X- ADC | Coordinate buffer page 1. |
| `0x5C` | Page 1 X+ ADC | Coordinate buffer page 1. |

`PIUINTREG` pending bits:

| Bit | Name | Meaning |
| ---: | --- | --- |
| 0 | PENCHG | Pen state changed. |
| 2 | DATALOST | Both coordinate pages were unavailable, data lost. |
| 3 | PAGE0 | Page 0 coordinate data valid. |
| 4 | PAGE1 | Page 1 coordinate data valid. |

The high byte of `PIUINTREG` is the interrupt mask. The active source test is:

```text
active = pending_low_byte & mask_high_byte
```

Touch IRQ summary appears at VRC4173 offset `0x0004` bit 9 and is routed
through GIU line 0 to VRIP 8.

Emulated scan behavior:

- `PIUCNTREG` bit 0 self-clears after command writes.
- When PIUSEQEN/PADATSTART/PIUMODE configure WaitPenTouch, a host pen-down
  transition moves into PenDataScan.
- The emulator fills two page buffers alternately.
- Coordinate conversion timing uses the PIU peripheral-time model, not CP0
  Count. Stabilization is in 30 microsecond units plus conversion overhead.
- If both page buffers remain valid when a new sample is due, DATALOST is set.
- Reading coordinate registers returns the page sample if valid, otherwise
  default idle ADC values.

Linux input driver guidance:

- Implement an interrupt-driven IIO/input driver around `PIUINTREG`.
- Enable the high-byte masks for PENCHG, PAGE0, PAGE1, and DATALOST.
- On PAGE0/PAGE1, read four ADC words from the matching page, then W1C the page
  bit in `PIUINTREG`.
- On PENCHG, read bit 13 from `PIUCNTREG`, then W1C bit 0.
- Treat X/Y calibration as board-specific. The raw ADC endpoints currently map
  approximately:
  - Y+: `0x81E1..0x8E30`
  - Y-: `0x8E30..0x81E1`
  - X-: `0x8D1B..0x8300`
  - X+: `0x8300..0x8D1B`

## Buttons

Status: emulator-modeled.

Base: `0x0A00A040`, size `0x10`

Readback bytes:

| Offset | Value |
| ---: | --- |
| `0x00` | `0xFF` idle low byte from hardware dump. |
| `0x01` | `0x9E` idle high byte from hardware dump. |
| `0x02` | Live `btn_set1`. |
| `0x03` | Live `btn_set2`. |

`btn_set1` bits:

| Bit mask | Button |
| ---: | --- |
| `0x04` | OK |
| `0x08` | Escape |
| `0x10` | Up |
| `0x20` | Down |
| `0x40` | Right |
| `0x80` | Left |

`btn_set2` bits:

| Bit mask | Button |
| ---: | --- |
| `0x10` | Rocket/modifier |
| `0x80` | Power |

Button state changes assert the keyboard source at VRC4173 offset `0x0004`
bit 1, routed through GIU line 0. The OAL/guest acknowledges this source by
writing the GIRQ0 acknowledge path at VRC4173 offset `0x0014`.

Linux input driver guidance:

- Polling works for early bring-up.
- Interrupt support should watch GIU0/VRC4173 bit 1, then read bytes 2 and 3.
- Do not treat the low halfword `0x9EFF` as button state; it is the idle
  quiescent readback.

## NAND Flash and Controller

Status: emulator-modeled for BE-300 ROM, SPL, WinCE NAND, and NANDWRITER paths.

Primary VRC4173 NAND register coverage:

| Physical range | Offset range | Role |
| --- | --- | --- |
| `0x0A00A000-0x0A00A01F` | `0xA000-0xA01F` | Legacy control/timing registers. |
| `0x0A00A060-0x0A00A15F` | `0xA060-0xA15F` | Strap/readback block including board ID at `0xA0C0`. |
| `0x0A00A400-0x0A00A4FF` | `0xA400-0xA4FF` | SPL transfer engine. |
| `0x0A00B000-0x0A00B20F` | `0xB000-0xB20F` | SPL stream window. |
| `0x0A00B100-0x0A00B10F` | `0xB100-0xB10F` | Enable registers. |
| `0x0A00C000-0x0A00C0FF` | `0xC000-0xC0FF` | ROM transfer engine. |
| `0x0A00C170-0x0A00C177` | `0xC170-0xC177` | ROM DMA command/status/FIFO block; also used as CF taskfile when CF windows are enabled. |
| `0x0A00C376` | `0xC376` | ROM DMA control/ack; also CF alternate status when CF windows are enabled. |
| `0x0A00D200` | `0xD200` | Direct I/O data port. |
| `0x0A00D202` | `0xD202` | Direct I/O control, CLE/ALE mode. |
| `0x0A00D7F8-0x0A00D7FF` | `0xD7F8-0xD7FF` | Legacy indexed data/status/device-ID ports. |

NAND geometry:

| Property | Value |
| --- | --- |
| Page data size | 512 bytes |
| OOB/spare size | 16 bytes |
| Raw page size | 528 bytes |
| Pages per block | 32 |
| Blocks | 1024 |
| Data image size | `1024 * 32 * 512 = 16,777,216` bytes for full emulated chip buffer |
| Restore image active data | BE-300 restore images are 1004 blocks of data-only pages, 16,449,536 bytes |
| NAND ID | Maker `0xEC`, device `0x73`, read as `0x73EC` at legacy device-ID port |

The active WinCE restore image `ce/restore_images/All_nand_300.bin` is raw
sector data in logical block order. The emulator synthesizes OOB state from the
loaded data and uses identity logical block mapping for the restore image.

Supported NAND commands:

| Command | Value | Behavior |
| --- | ---: | --- |
| READ0 | `0x00` | Read data area starting at column 0. |
| READ1 | `0x01` | Read data area starting at column 256. |
| READOOB | `0x50` | Read spare/OOB area. |
| STATUS | `0x70` | Read status. Emulator returns ready status `0xC0` in command paths. |
| SEQIN | `0x80` | Program setup. |
| READID | `0x90` | Returns `0xEC, 0x73`. |
| PAGEPROG | `0x10` | Program confirm. |
| ERASE1 | `0x60` | Erase setup. |
| ERASE2 | `0xD0` | Erase confirm. |
| RESET | `0xFF` | Reset to idle/ready. |

### Legacy Direct I/O Path

Registers:

| Offset | Name | Behavior |
| ---: | --- | --- |
| `0xAC00` | `NAND_REG_CMD` | Command byte. |
| `0xAC04` | `NAND_REG_ADDR` | Address byte. |
| `0xAC48` | `NAND_REG_READY` | Bit 0 ready. |
| `0xD200` | Direct data | Data/command/address depending on `0xD202`. |
| `0xD202` | Direct control | `0x80` means CLE command, `0x01` means ALE address, `0x00` means data. |

This path is closest to a raw NAND controller model. A Linux MTD driver could
use it for a simple command/address/data implementation.

### SPL Transfer Engine

Registers:

| Offset | Name | Behavior |
| ---: | --- | --- |
| `0xA410` | CTRL | Latched. |
| `0xA414` | CMD/phase | Phase `0x03` command, phase `0x06` address. |
| `0xA420` | ADDR/data byte | Feeds command/address byte stream and can return queued data bytes. |
| `0xA430` | ACK | Commits current phase. |
| `0xA440` | STATUS | Returns bit 0 ready. |
| `0xA460` | KICK | Starts busy transition. |
| `0xA464` | MODE | Controls stream/buffer/program modes. |
| `0xA468` | MISC/ECC input | Receives six 10-bit ECC words in mode 1. |
| `0xA4A0-0xA4AC` | BUFFER | 16-byte mode/ECC result buffer. |
| `0xA4C0` | STATUS2 | Result/status latch. |
| `0xB000` | STREAM_DATA | Sequential stream data window. |

Known `MODE` values:

| Mode | Meaning |
| ---: | --- |
| `0x00` | Reset/idle. |
| `0x01` | Prepare for ECC/buffer result read. |
| `0x04` | Fill 16-byte result buffer from current stream position. |
| `0x05` | 520-byte stream burst through `0xB000`. |
| `0x06` | Final 8-byte OOB/ECC write window through `0xB000`. |
| `0x07` | 520-byte program burst, 512 data plus first 8 OOB bytes. |

ECC behavior: the emulator assumes bit-perfect NAND data. Hardware ECC
syndrome outputs are zero after six stored ECC words are supplied. A Linux
driver can initially ignore hardware correction and use software ECC or treat
the medium as clean, but a production-quality MTD driver should still model
the OOB layout expected by the BE-300 image.

### ROM Transfer Engine

Registers:

| Offset | Name | Behavior |
| ---: | --- | --- |
| `0xC010` | CTRL | Clearing bit 0 resets the ROM transfer engine. |
| `0xC014` | CMD | Command/phase. |
| `0xC020` | ADDR | Address byte stream. |
| `0xC030` | ACK | Latched. |
| `0xC040` | STATUS | Bit 0 ready. |
| `0xC060` | KICK | Clears ready. |
| `0xC064` | MODE | Mode `0x05` starts stream; `0x04` sets ready; `0x00/0x01` reset ECC state. |
| `0xC068` | ECC_IN | Six 10-bit stored ECC words. |
| `0xC0A0-0xC0AC` | ECC_OUT | Zero syndrome outputs. |
| `0xC0C0` | STATUS2 | ECC/result status. |

This path mainly exists for the real boot ROM. Linux does not need to use it.

### NANDWRITER Restore Engine

Status: emulator-modeled for recovery tooling.

The restore engine has buffers and sideband registers outside the normal
`0xA000` NAND window:

| Offset range from VRC4173 base | Role |
| --- | --- |
| `0x0980-0x098F` | Sideband control. |
| `0x0B00-0x0CFF` | Page buffer window. |
| `0x0C00-0x0CC0` | Restore C-window registers. |
| `0x1100`, `0x1104`, `0x1108`, `0x110C`, `0x1128`, `0x1B20`, `0x1CC0-0x1CCC` | Additional restore-related latches. |

Seeded restore C-window values include:

| Offset | Reset/readback seed |
| ---: | ---: |
| `0x0C00` | `0x00000020` |
| `0x0C04` | `0x00000002` |
| `0x0C08` | `0x00000050` |
| `0x0C0C` | `0x000000F1` |
| `0x0C24` | `0x00000014` |
| `0x0C2C` | `0x00000001` |
| `0x0C30` | `0x0000E000` |
| `0x0C34` | `0x000001FF` |
| `0x0C3C` | `0x00001F1F` |
| `0x0C40` | `0x00000500` |
| `0x0C48` | `0x0000030F` |
| `0x0C4C` | `0x00000000` |

Linux should not use the NANDWRITER path as its primary MTD interface unless it
is deliberately implementing the Casio recovery protocol.

## CompactFlash and PCMCIA

Status: emulator-modeled enough for BE-300 restore boot and WinCE PCMCIA/ATA
storage attachment; the PCMCIA bridge is still partly approximated.

The emulator has three different CF/PCMCIA views:

| View | Physical address | Purpose |
| --- | ---: | --- |
| Boot CF taskfile window | `0x0A00C170-0x0A00C177`, alt `0x0A00C376` | ROM/NANDWRITER recovery path when CF is boot-visible. |
| Runtime CF taskfile window | Usually `0x0A00C170-0x0A00C177`, alt `0x0A00C376`, after PCMCIA windows are enabled | WinCE/atadisk-style ATA access. |
| CF boot window alias | `0x1E000000-0x1E000FFF` | Registered as `be300_cf_window`; decodes standard ATA taskfile offsets within the page. |
| PCMCIA attribute/CIS window | `0x0B400000-0x0B6FFFFF` | CIS scan and attribute memory. |
| VRC4173 CF companion page | `0x0A001000-0x0A001FFF` | Socket status, inserted/no-media state, card-window enable latches. |
| Secondary companion status | `0x0B000000-0x0B00FFFF` | Socket/status alias with no-card overlay. |

CF media:

- A CF image must be a non-empty file with size multiple of 512 bytes.
- Attached media sets `attached = true`, raises an insertion/state-change
  interrupt, and seeds a minimal CompactFlash CIS.
- No attached image still models the socket/adapter as present but with no
  media inserted.

### ATA Taskfile

The CF implementation exposes an 8-bit ATA taskfile and supports:

| Command | Value | Behavior |
| --- | ---: | --- |
| RECAL | `0x10` | Completes ready. |
| READ SECTORS | `0x20` | LBA sector reads. |
| WRITE SECTORS | `0x30` | LBA sector writes; marks image dirty. |
| READ VERIFY | `0x40` | Completes ready. |
| DIAGNOSTIC | `0x90` | Completes ready. |
| IDENTIFY | `0xEC` | Returns 512-byte identify data for `BE-300 RECOVERY CF`. |
| SET FEATURES | `0xEF` | Accepts feature `0x03` and `0x01`; other features return error. |

Status bits:

| Bit | Name |
| ---: | --- |
| 0 | ERR |
| 3 | DRQ |
| 4 | DSC |
| 6 | DRDY |
| 7 | BSY |

Taskfile decode inside a mapped page:

| Page offset | Role |
| ---: | --- |
| `0x170-0x177` | Secondary-style taskfile registers. |
| `0x180-0x187` | Alternate observed taskfile window. |
| `0x1F0-0x1F7` | Primary-style taskfile registers. |
| `0x206`, `0x376`, `0x3F6` | Alternate status/device control. |

Linux block driver guidance:

- Use the PCMCIA/CIS path to discover the card when running in full OS mode.
- Use LBA mode; CHS/non-LBA commands are rejected.
- After `IDENTIFY`, standard PIO sector I/O through the data register works.
- Implement timeouts. If a status bit never changes, run the emulator with
  `--log-mmio` or `--detect-stall` and check whether the access is landing in a
  real CF window or an unmapped/latch area.

### CIS and Attribute Memory

The emulator seeds a minimal CIS for a fixed-disk ATA card. Important tuples:

| Tuple | Purpose |
| --- | --- |
| `CISTPL_DEVICE` | Generic device descriptor. |
| `CISTPL_VERS_1` | Vendor/product strings. |
| `CISTPL_MANFID` | Manufacturer/product identifiers. |
| `CISTPL_FUNCID` | Function code `0x04` fixed disk. |
| `CISTPL_FUNCE` | Fixed-disk interface `0x01` IDE. |
| `CISTPL_CONFIG` | Configuration-register info. |
| `CISTPL_CFTABLE_ENTRY` | Single 8-bit I/O entry. |
| `CISTPL_NO_LINK`, `CISTPL_END` | Termination. |

The attribute-memory helper applies every-other-byte PC Card attribute stride.
No-card reads return `0xFF` (`CISTPL_END`) so scanners terminate instead of
walking a floating bus forever.

### CF Companion Status

VRC4173 companion CF page base: `0x0A001000`

Important offsets from that page:

| Offset | Behavior |
| ---: | --- |
| `0x0000`, `0x0008` | Card status. Attached returns `0x00000004`; no media returns `0x0000000C`. |
| `0x0040` | Card-state summary, `1` when attached, `0` when no media. |
| `0x0044` | No-card path returns `0` to preserve adapter/unit-present behavior. |
| `0x004C` | No-card path returns bit 6 set (`0x40`) so no CIS scan sees a synthetic card. |
| `0x0B10` | Inserted-card state. Before CIS FUNCID is read, exposes insertion edge bit; after, returns settled `0x48`. |
| `0x0B50` | One-shot insertion edge bit 3. |

Writing within `0x0B00-0x0B9F` when attached enables runtime card windows in
the emulator. This is a PCMCIA bridge approximation, not a fully decoded
VRC4173 CARDU model.

Linux PCMCIA guidance:

- Minimal path: hard-code a BE-300 CF socket platform device and map the ATA
  taskfile directly after confirming card-present bits.
- More complete path: implement a small PCMCIA socket driver around the
  VRC4173 companion page, CIS window, and GIRQ0 bit 0.
- Treat the bridge translation registers as incomplete. If Linux programs a
  different card I/O base, the emulator may not route it unless the bridge
  model is extended.

## PPSH / Auxiliary Debug Mailbox

Status: emulator-modeled when `--ppsh` is enabled; stubbed idle hardware
otherwise.

Base data register: `0x0C000120`

Base status/command register: `0x0C000520`

In the emulator these are represented as a 0x500-byte window starting at
`0x0C000120`:

| Window offset | Physical address | Role |
| ---: | ---: | --- |
| `0x000` | `0x0C000120` | Data register. |
| `0x400` | `0x0C000520` | Status/command register. |

When `--ppsh` is disabled:

- Offset `0x400` eventually reads status `0x2320`.
- Offset `0x000` reads `0xFF`/`0xFFFF`, meaning no host data.
- This causes WinCE's PPSH probe to fail cleanly and continue GUI boot.

When `--ppsh` is enabled:

- The status machine responds to command words such as `0x3330`, `0x1100`,
  `0x9100`, and `0x9900`.
- Guest output bytes submitted through the data path are emitted to stdout.
- Host input is queued and returned through the data register.

Linux can ignore PPSH for normal hardware support. It may be useful as a simple
early debug transport if you write a small mailbox driver.

## Secondary Companion Decode Window

Status: survey latch and compatibility shim.

Base: `0x0B000000`, size `0x10000`

The BE-300 software accesses a second companion-chip decode window through
kseg1 alias `0xAB000000`. The emulator backs this with a byte array and overlays
selected PCMCIA no-card status bytes.

Known use:

- OAL/card code writes offsets such as `0x104`, `0x108`, `0x10C`, `0x110`,
  `0x138`, `0x13C`, `0x204`, `0x208`, `0x520`, and `0x524`.
- Reads from selected socket-status bytes report no second card, even when
  `--cf` is attached, to avoid a duplicate synthetic socket.

Linux should not depend on this window until the companion bridge map is
properly modeled. If a Linux PCMCIA driver sees two sockets, treat the secondary
window as no-card/no-media.

## Reset, Power, and Reboot Behavior

The emulator has two important reset mechanisms:

| Mechanism | Trigger | Behavior |
| --- | --- | --- |
| VR4131 PMU software reset | Write bit 4 to `0x0F0000C6` (`PMUCNT2REG`) | Stages `RSTSW`, reapplies PMU reset state, cold-resets CPU. |
| BE-300 KjCMU warm-reset sequence | Write `7` to `0x0A00A0C4`, then `10` to `0x0A00A0C8` | Cold-resets CPU and preserves companion latch state. Used by WinCE Boot.exe path. |

The `0x0A00A0C4/0x0A00A0C8` sequence is a compatibility model for observed
BE-300 behavior. It is not a generic VR4131 reset path. Linux should use the PMU
software reset path first.

## Suggested Linux Bring-Up Order

This order minimizes dependency on incomplete companion-chip behavior.

1. Board entry and memory:
   - Enter in a way that registers the full BE-300 model (`--nand` or an
     emulator change that exposes full hardware in ROM mode).
   - Set MIPS little-endian VR4131 CPU support.
   - Configure SDRAM base `0x00000000`, default size 16 MiB unless passed by
     boot arguments.
   - Install exception vectors and TLB handlers before enabling interrupts.

2. Early serial:
   - Bring up VR4131 SIU at `0x0F000800` or VRC4173 SIU at `0x0A008680`.
   - Confirm TX by polling LSR THRE/TEMT and writing THR.

3. Interrupt controller:
   - Initialize VR4131 ICU masks at `0x0F000080`.
   - Route VRIP to MIPS IP2.
   - Enable GIU line 0 only after you have handlers for touch/buttons/CF, or
     mask sub-sources carefully.

4. Timer:
   - Start with CP0 Count/Compare for scheduler ticks.
   - Add VR4131 RTC/RTCL1 after basic interrupts are stable.
   - Pet PMU HALTimer early to avoid surprise reset.

5. Framebuffer:
   - Map `0x0A200000`.
   - Use 240x320 RGB565 with 512-byte line length.
   - Ignore acceleration blocks at first.

6. NAND:
   - Implement a simple raw NAND MTD driver using command/address/data
     registers or the SPL transfer engine.
   - Use 512-byte pages plus 16-byte OOB in the software model.
   - Read ID `0xEC/0x73` and support READ0/READ1/READOOB/STATUS/RESET.

7. Input:
   - Add buttons as a small input driver reading `0x0A00A042/0x0A00A043`.
   - Add PIU touch after GIU0 interrupt handling is reliable.

8. CF/PCMCIA:
   - Start with direct attached-CF ATA taskfile access.
   - Add CIS/socket handling later if you need hotplug-like behavior.

9. Optional devices:
   - PPSH mailbox for debug.
   - Buzzer host pulse support.
   - USB OHCI probe only after a real USB device model exists.

## Driver Inventory

Recommended Linux driver names are suggestions, not existing upstream bindings.

| Driver | Hardware | Minimum required behavior |
| --- | --- | --- |
| `be300-board` | Board file/platform setup | Declare memory, MMIO resources, IRQ routing, framebuffer resource, and platform devices. |
| `vr4131-icu` | `0x0F000080` | Mask/unmask/ack VRIP/GIU summary interrupts, route to MIPS IP2. |
| `vr4131-pmu` | `0x0F0000C0` | Read/clear reset causes, pet HALTimer, implement restart via SOFTRST. |
| `vr4131-rtc` | `0x0F000100` | Expose elapsed counter and RTCL1 alarms; valid wall-clock time depends on boot policy such as `--rtc-host-time`. |
| `vr4131-gpio` | `0x0F000140` | Expose GPIO direction/data/status if needed. |
| `vr4131-siu` | `0x0F000800` | Standard ns16550-style UART, 2-byte internal register spacing for extension path. |
| `vrc4173-uart` | `0x0A008680` | ns16550 UART with register shift 2 (`addr_mult=4`). |
| `be300-fb` | `0x0A200000` | RGB565 framebuffer, 240x320, stride 512 bytes. |
| `be300-nand` | `0x0A00A000` | Raw NAND command/address/data or transfer-engine MTD driver. |
| `be300-piu` | `0x0A000300` | Interrupt-driven touch input with two ADC pages. |
| `be300-buttons` | `0x0A00A040` | Input keys from bytes 2/3. |
| `be300-cf-socket` | `0x0A001000`, `0x0B400000`, taskfile windows | Socket/CIS/ATA glue for attached CF images. |
| `be300-ppsh` | `0x0C000120/0x0C000520` | Optional debug mailbox. |

## Known Gaps and Cautions

- Full BE-300 custom devices are not registered in positional ROM mode.
- The VRC4173 PCMCIA bridge is not fully modeled. Runtime CF works through
  observed windows and compatibility routing.
- VRC4173 display acceleration blocks are partial. Use the framebuffer first.
- VRC4173 USB exposes OHCI-like registers but no useful connected USB device.
- DSIU extension registers are stubbed.
- The ScCmcu range `0x0A007800-0x0A00783F` returns instant completion and is
  not a real MCU model.
- Unknown VRC4173 registers usually act as latches. A driver that depends on
  side effects from an undocumented register may need emulator work.
- The RTC starts as uninitialized/default-value for cold boot. Do not expect a
  host-synchronized date.
- The emulator prioritizes hardware-accurate cold boot of the real WinCE image.
  Do not rely on seeds, RAM prepopulation, or guest binary patches as Linux
  bring-up mechanisms.

## Quick Address Checklist

Use this list when validating that a Linux platform description covers the
emulated hardware ABI:

| Device | Physical address |
| --- | ---: |
| SDRAM | `0x00000000` |
| VRC4173 core/latch | `0x0A000000` |
| Touch PIU | `0x0A000300` |
| VRC4173 companion SIU | `0x0A008680` |
| VRC4173 CF companion page | `0x0A001000` |
| VRC4173 NAND | `0x0A00A000` |
| Buttons | `0x0A00A040` |
| LCD framebuffer | `0x0A200000` |
| Secondary companion window | `0x0B000000` |
| PCMCIA attribute/CIS window | `0x0B400000` |
| PPSH data/status | `0x0C000120`, `0x0C000520` |
| VR4131 BCU | `0x0F000000` |
| VR4131 CMU | `0x0F000060` |
| VR4131 ICU | `0x0F000080` |
| VR4131 PMU | `0x0F0000C0` |
| VR4131 RTC | `0x0F000100` |
| VR4131 GPIO/GIU | `0x0F000140` |
| VR4131 SIU | `0x0F000800` |
| VR4131 DSIU | `0x0F000820` |
| CF boot alias | `0x1E000000` |
| Reset ROM | `0x1FC00000` |
