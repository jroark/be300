# Hardware Accuracy Review (2026-11-10)

A cross-check of the emulator (`be300` + `gxemul`) against the two guest ports that have since
run on a real BE-300: Linux (`jroark/mipsel-cross-image`, "L") and Glaze
(`jroark/glaze-os`, "Z"). The emulator was shaped by what Windows CE needed; the guests then
found hardware behaviour CE never exercised. Each row cites the evidence. "E" paths are in this
repository, "G" paths in the `gxemul/` submodule.

The **Status** column is kept current: `open`, `fixed <commit subject>`, or `needs probe`
(no hardware evidence yet, see the probe list at the end).

Line numbers are those of the 2026-11-10 trees.

## 1. Emulator less accurate than known hardware

### CPU and caches

| ID | Gap | Evidence | Emulator site | Status |
|----|-----|----------|---------------|--------|
| C1 | Config resets to 0x10125930 and only bits 1:0 are writable. Real reads 0x10135923 (BP=1, DB=0 i.e. 16-byte D-line, K0=3). ROM and SPL write 0x00035B23; Linux and Glaze OR in bits 22 and 16 for the PRId 0x0C80 CACHE workaround | `docs/hardware/cpu_state.txt:19`, `docs/ROM_SPL_HANDOFF.md:83,220`, Z `kernel/start.S:61-79` | G `src/cpus/cpu_mips_coproc.c:162-196, 872` | fixed: gxemul "VR4131 Config: 16-byte D-cache lines; BP, bit 22 and K0 writable" (reset value before the ROM write still unknown) |
| C2 | No cache model: CACHE is a no-op and fetch always sees memory. Hides the ROM's one-line I-cache tag loop (`0x1FC00700`), un-zeroed SPL BSS, D-cache colour aliasing, and the PRId 0x0C80 CACHE erratum | L `docs/HARDWARE.md:104-117`, Z `kernel/shm.c:1-11`, Z `docs/dev/GLAZEBOOT.md:229-263` | G `src/cpus/cpu_mips_instr.c:1302-1308` | open |
| C3 | LL/SC/LLD/SCD execute (VR4131 UM: not supported). MOVN/MOVZ (MIPS IV) execute. 64-bit opcodes ignore Status.UX in user mode. MIPS II trap-immediates (TGEI..TNEI) are wrongly reserved | VR4131 UM instruction set; L `build_be300_kernel.sh:161-163` (opcode scan) | G `src/cpus/cpu_mips_instr.c:5007-5033, 4070, 4123, 4938-4949` | fixed except the UX check: gxemul "MIPS: LL/SC reserved on VR41xx, MOVN/MOVZ need MIPS IV, trap-immediates MIPS II" |
| C4 | SUSPEND is a plain WAIT: it does not set IE and Count/timers keep running. UM 12.1.4(3): every clock but the RTC's, ICU's and PMU's stops | VR4131 UM §12.1.4; L `board/casio-be300/pm.c:5-6,80-82` | G `src/cpus/cpu_mips_instr.c:4503-4519` | open |
| C5 | 1 KB pages abort the emulator | VR4131 UM (TLB) | G `src/cpus/cpu_mips_coproc.c:1718, 1845` | open (no guest uses them) |

### Interrupts, GPIO, power

| ID | Gap | Evidence | Emulator site | Status |
|----|-----|----------|---------------|--------|
| I1 | Any write to the GIRQ0 source words `0x0A000010`/`0x0A000014` acknowledges the CF and button sources. On hardware a source is silenced by masking bit 3; writing pending bits is not an acknowledge. Status bits 4-7 and the bit-7 latch are not modelled | Z `kernel/hw.h:87-101`, Z `kernel/drivers/irq.c:44-46`, L `docs/HARDWARE.md:31-32` | `src/be300_devices.c:116-119, 940-948` | needs probe: Windows CE's InterruptDone writes 0x30, 0x04, 0x0C to a source word and Glaze saw that writing a word clears its bit-7 latch, which suggests bits 4-7 are write-1-to-clear status rather than "any write acknowledges". Probe 2 decides; not changed |
| I2 | GIUINTEN/INTTYP/INTALSEL/INTHTSEL are latches with no effect on interrupt assertion, so sources left enabled by CE cannot storm | VR4131 UM GIU; Z `kernel/drivers/irq.c:24-30`, L `board/casio-be300/hardware-regs.h:524-534` | G `src/devices/dev_vr41xx.c:252-270, 1057-1075` | partly fixed: gxemul "VR4131 GIU: a pin requests an interrupt only while GIUINTEN enables it"; INTTYP/ALSEL/HTSEL still not interpreted |
| I3 | GIUPIODL writes are dropped; reads return a constant 0xAAE2 plus battery bits. GPIO4/6 (USB, dock), GPIO5 (socket power) and GPIO8 (battery sense) have no effect | `docs/hardware/hw_dump_vr4131.txt:175-176` (0xAAE2 and 0xAAE6 seen) | G `src/devices/dev_vr41xx.c:1098-1120` | fixed: gxemul "VR41xx GIU: latch GIUPIODL outputs and read them back"; boards do not act on the outputs yet (K4, S1, F1) |
| I4 | RTCLONG1 and SYSINT1 bit 2 are cleared by the emulator when the CPU takes the interrupt; SYSINT1/SYSINT2 are writable though UM §11.2.1 makes them read-only | VR4131 UM §11.2.1 | G `src/devices/dev_vr41xx.c:226-245, 847-855, 884-892` | SYSINT1/2 read-only fixed (gxemul "VR41xx ICU: SYSINT1REG and SYSINT2REG are read-only"); RTCLONG1 auto-clear needs probe: the CE dump has RTCINTREG=0x0006 and SYSINT1=0x0004 at rest, yet NK never writes RTCINTREG in its tick handler |
| P1 | POWERSWINTR (SYSINT1 bit 1) is never raised while running | `docs/hardware/hardware.txt:46-55` | G `src/devices/dev_vr41xx.c:1203-1215` | needs RE: here a Power press already reaches Windows CE through the GIRQ0 button source (`src/devices/input.c`). Raising POWERSWINTR as well was tried and reverted because its effect on CE could not be checked headless |
| R1 | (alarm compare fixed: "rtc: the ELAPSEDTIME alarm is a match with ECMP") ETIME writes are visible at once (hardware: a read right after a write returns the old count; guests wait 200 µs). ECMP fires on `etime >= ecmp`, so ECMP=0 wakes HIBERNATE at once. The ETIME interrupt is delivered only while RTCL1 is stopped. RTCL2CNT and TCLKCNT alias ETIME | Z `kernel/drivers/rtc.c:40-71`; `docs/hardware/hw_survey_v8.txt:28-31` (ETIME far above ECMP with no alarm bit) | `src/hw/rtc.c:5-23, 184-189`, G `src/devices/dev_vr41xx.c:641-651` | open |
| U1 | Only the framebuffer and the dock SIU stall the bus when gated, and the SIU counts as clocked with any one of `1108/112C/1138/113C` on. Hardware: any gated companion unit hangs the bus (NAND gate `1134` reads zeros); the SIU is only proven with all four on | Z `docs/hardware/NAND_BOOT_AUDIT.md:312-339`, Z `kernel/hw.h:391-395` | `src/devices/gate_stall.c:14-17, 51-60` | needs probe (11) |
| X1 | The Kj reset (7 to `0xA0C4`, 10 to `0xA0C8`) reset only the CPU, and `0xA060-0xA07E` were seeded with CE's 0x000C at power-on. NK uses `0xA07A`/`0xA07C` bit 2 to tell a reboot from a HIBERNATE wake | NK `0x80077520-0x80077564`, `0x80077770-0x80077944`; Z `docs/hardware/NAND_BOOT_AUDIT.md:329` (words differ cold vs CE) | `src/devices/nand_window.c`, `src/hw/nand.c` (`nand_kj_reset`) | fixed: "Kj reset clears the companion's A060-A07E words; zero at power-on". What else the reset reaches is unknown |
| W1 | ScCmcu CONFIG (`0x0A007810`) was a read-back latch. Bit 16 is "rebooted by the watchdog" status, write 1 to clear; as a latch it made every boot after the first show "Restart: Due to an OS problem" | cedmbltin.dll `0x019F1FE8-0x019F2028` ("System was rebooted by WDT."), PowerOn.dll `0x01A1140C-0x01A11524` | `src/hw/cmcu.c` | fixed: "cmcu: CONFIG bit 16 is \"rebooted by the watchdog\" status, write 1 to clear" |

### NAND

| ID | Gap | Evidence | Emulator site | Status |
|----|-----|----------|---------------|--------|
| N1 | The XFER stream ignores the chip's area pointer (READ1 01h / READOOB 50h). Linux relies on that (discards 256/512 stream bytes, pads 512 bytes before a spare program); Glaze assumes a real chip | Small-page NAND pointer commands; L `board/casio-be300/nand_controller.h:151-165`, Z `kernel/drivers/nand.c:174` | `src/hw/nand.c:420-449, 836-845` | needs probe: Linux physical-NAND builds ran JFFS2 on a real BE-300 with the discard/pad scheme, which suggests the engine may stream from column 0 whatever the pointer; probe 4 decides |
| N2 | NAND clock gate `0x1134` and DMA ownership `0x1CC0` are not enforced. Hardware: gated reads return zeros; while `1CC0`=1 page data goes to DMA, and turning DMA off raises GIRQ0 source 12 | Z `kernel/hw.h:391-402`, Z `docs/dev/GLAZEBOOT.md:270-274` | `src/hw/nand.c:757`, `src/be300_devices.c:531-551, 1224` | open |
| N3 | The hardware-ECC result registers (`A4A0-A4AC`, `A4C0`) return an invented syndrome. On hardware, loaded as nanddisk.dll loads them, they "gave syndromes that matched nothing" | Z `docs/dev/GLAZEBOOT.md:283-285` | `src/hw/nand.c:124-163, 866-875` | needs probe |
| N4 | Data-only images get a synthetic spare area (tags on pages 0-4 only, no RS parity) and spare-area programs are dropped with status 0xC0 | Z `docs/dev/GLAZEBOOT.md:274-283` (real: every page tagged, parity in bytes 8-15) | `src/hw/nand.c:53-68, 179-185` | open |
| N5 | Program/erase of a block marked bad returns 0xC1 and changes nothing; a real chip does not refuse | NAND datasheet | `src/hw/nand.c:95-103, 175-176, 215` | needs probe |

### CompactFlash and PCMCIA

| ID | Gap | Evidence | Emulator site | Status |
|----|-----|----------|---------------|--------|
| K1 | ATA, NE2000 and 3c589 decode I/O whatever the COR holds. Hardware: a COR of zero leaves the card unconfigured | Z `docs/hardware/PHYSICAL_NAND.md:24-27`, L `board/casio-be300/hardware.c:329-333` | `src/hw/cf.c:513, 1258-1294` | fixed for storage cards: "cf: the card's COR decides its I/O decode; the ROM sees the card as it is". NE2000 and 3c589 still ignore their COR |
| K2 | SET FEATURES 01h has no effect: a 16-bit data access returns two bytes. Hardware after CE: one byte per access, high byte zero | Z `kernel/drivers/cf.c:496-503`, L `board/casio-be300/pata_be300.c:4-9` | `src/hw/cf.c:1025-1053` | fixed: "cf: honour SET FEATURES 01h/81h (8-bit data transfers)" |
| K3 | `0x0A00A020` bit 3 is ignored. Hardware: with it set, `D200/D202` reach the card's COR/CCSR; with it clear they are the NAND direct-I/O port, and a COR write "sent the NAND a stray byte and hung its next read" | Z `kernel/hw.h:181-190, 283-286` | `src/devices/nand_window.c:96-101, 155-177, 200-211` | fixed: "cf: the card's COR decides its I/O decode; the ROM sees the card as it is" (bit 3 set out of reset, from the ROM's COR write at `0x9FC01212`) |
| K4 | Socket power (GPIO5, `A034`), the `A004` reset pulse, `A010-A018` timing and gate `111C` are not modelled; the card answers from attach time | Z `kernel/drivers/cf.c:660-688`, L `board/casio-be300/hardware.c:355-372` | `src/hw/cf.c`, G `dev_vr41xx.c` | partly: the `A004` RESET pulse now unconfigures a storage card. Socket power is GPIO5 high (ROM `0x9FC0108C-0x9FC0109C`); not modelled, nor `A034`, `A010-A018`, gate `111C` |
| K5 | Card-detect values: `A008` reads 0 until `A000` bit 0 is written; hardware reads 0x3FF empty after power-on. `0x1000` reads 4 with a card; hardware reads 0 with a card at power-on and 8 with none | Z `kernel/hw.h:196-204`, Z `docs/hardware/NAND_BOOT_AUDIT.md:316-320` | `src/hw/nand.c:723-728`, `src/hw/cf.c:12-13, 76-78` | needs probe |
| K6 | Without `--jacket`, the socket-1 attribute window `0x0B400000-0x0B6FFFFF` shows the socket-0 card | Z `kernel/hw.h:206-258` | `src/devices/pcmcia_attr.c:57-62, 85-129` | open |
| K7 | NE2000 remote DMA runs while the DP8390 is stopped (no STA) | DP8390 datasheet; Z `kernel/drivers/ne2000.c:96-100` | `src/hw/cf_ne2000.c:336-342` | open |
| K8 | WL54-CF reports revision 0x12 (B1), a non-zero permanent MAC and firmware 3.2.8, and answers at once. The real card is B0 (0x10, reading 0 just after configuration), MAC zero (from CIS FUNCE), firmware 2.5, ASSOCIATE answers 0x50/0x8012 | Z `kernel/drivers/wl54cf.c:178-205, 499-536` | `src/hw/wl54cf.c:158-173, 432-433` | open |
| K9 | XI-825 CIS is synthetic (8- and 16-bit I/O entry); Glaze logged the real one (CONFIG 0x3E0, 16-bit only) | Z `docs/dev/GLAZEBOOT.md:257-260` | `src/hw/cf.c:470-485` | open |

### Serial, audio, touch, display, USB

| ID | Gap | Evidence | Emulator site | Status |
|----|-----|----------|---------------|--------|
| S1 | The dock UART works with its transceiver unpowered. Hardware needs dock control `0x0A008000` = 0x1F08-style power-up and GPIO6 high (the UART is silent until CE opens `COM1:`) | Z `docs/dev/GLAZEBOOT.md:225-228`, Z `kernel/drivers/stowaway_hw.c:1-20, 86-92` | `src/be300_devices.c:1191-1203`, `src/devices/serial_dock.c` | open |
| S2 | Stowaway answers `FA FD` at once on an RTS edge, MSR always shows DSR, no DCD pulse. Real Targus: MSR 0x98 (no DSR), DCD rises 0.4-0.6 s after DTR and drops 200 ms later, an RTS edge before 500 ms goes unanswered, the last release byte of a burst arrives twice | Z `tests/STOWAWAY.md:51-59` | `src/stowaway.c:232-246`, G `src/devices/dev_ns16550.c:333-345` | open |
| S3 | Dock ID `0x0A008010` reads 0x0C (USB cradle) with no dock option; real empty dock reads 0x0F | Z `tests/STOWAWAY.md` | `src/be300_devices.c:1200`, `src/devices/serial_dock.c:41-45, 765-772` | fixed: "Empty dock reads ID 0x0F, not the USB cradle's 0x0C", after the Kj reset fix (X1) |
| A1 | Wave DMA strictly alternates buffer pair 0 (`8B0/8B4`) and pair 1 (`8B8/8BC`). Hardware: current/next, the next pair is copied into current; the dump shows both pairs equal; status repeats | `docs/hardware/hw_dump_vrc4173.txt:1071-1077`, Z `kernel/drivers/audio.c:22-26, 262-266` | `src/hw/aiu.c:105-117, 270-280` | needs probe: wavedev.dll treats status bits 0/1 as buffer 0/1 with separate refill events (docs/BE300_AUDIO.md), which fits the current ping-pong model; Glaze saw repeated status 1. Probe 8 decides |
| A2 | Wave DMA ignores codec power and clock (`0x1104`, `0x1118`, `0x880` bit 3, `0x884`) and times completion on the host clock | Z `kernel/drivers/audio.c:325-327` | `src/hw/aiu.c:36-42` | open |
| T1 | Idle PIU page reads return the end-point constants with bit 15 (valid) set. Real idle pages hold stale 12-bit values without it | `docs/hardware/hw_dump_vrc4173.txt:986-994` | `src/hw/piu.c:351-357` | fixed: "piu: idle page buffers keep stale data without the valid bit" |
| T2 | Touch samples are exact and linear, with no settling. Real panels need the guests' stroke filters (drop first, hold back, average) | Z `kernel/drivers/input.c:36-50`, L `board/casio-be300/touch_filter.h:1-15` | `src/hw/piu.c:66-84` | open |
| T3 | The A/D port scan finishes at once on the PIUCNT write; `0x314/0x318` channel setup and the GPIO8 settle are ignored | gwes 0xB041C sequence (L `board/casio-be300/battery.c`) | `src/hw/piu.c:278-284` | open |
| D1 | `0x0A000C38` is a free-running counter on hardware; the emulator stores writes | `docs/HARDWARE_SURVEY_SYNTHESIS.md:116-126` | `src/hw/lcd.c` | open (no guest reads it) |
| F1 | USB function: gate `0x1114`, GPIO4 low / GPIO6 high and `1B2C`=1 are not required; INTSTAT0 bit 2 (EP1 empty) is reported with no cable | Z `kernel/drivers/usb.c:52, 251-254`, Z `docs/hardware/NAND_BOOT_AUDIT.md:312-315` | `src/hw/usbf.c:57-58, 377-379` | open |

**Note S3 and how NK reads a reset (2026-11-11).** With the dock ID seeded 0x0F, CE used to
sleep after its first-boot reboot. NK's reset entry (`0x80077300`-`0x80077a00`) explains why:

- `PMUINTREG` bit 4 (`RTCRST`) set: power-on. Clear it and cold-start.
- Otherwise the companion words `0xA07A`/`0xA07C` decide. `0xA07A` bit 2 clear means the
  companion lost its state: NK sets bit 2 in both, checks the kernel image checksum against
  PA `0x2588` and the pattern at PA `0x2408`-`0x24FB`, and restarts the OS.
- `0xA07A` bit 2 still set means the CPU woke from HIBERNATE. If no boot key is held
  (PA `0x251C`, built from `0xA042`/`0xA044`), NK wants a wake reason: `PMUINTREG` bit 0, 9
  or 13; `AA000004` bit 0 or 1, or bit 16 without bit 3; a dock ID of 8 or 0x0C; the jacket
  (`0x1B50 & 0x1B58` bit 3); `0x104C` bits 4-5. With none it sleeps again (`0x800794C8`).
- A suspend signature at PA `0x2700`-`0x27FF` then selects resume over cold start.

The emulator kept the companion words across the Kj reset, so the reboot was read as a wake,
and only the cradle ID let it continue. See X1.

The "Restart" dialog comes from PA `0x254C`, which PowerOn.dll reads once: bit 0 "shut down,
high current consumption by hardware", bit 1 the same "by the card", bits 2-3 "Restart: Due
to an OS problem". NK sets 1 and 2; modmonitor.exe (`0x12CD0`) sets 4; cedmbltin.dll sets 8
from the watchdog status. See W1.

**How the boot ROM treats a card (2026-11-11).** Decoded from the ROM with a MIPS16 decoder
(the committed `docs/hardware/be300_boot_rom.dis` is byte-swapped and useless):

- `0x9FC003EC`: returns button bit 15 (`A044`) only if `PMUINTREG` has `RSTSW`; it also
  writes `PMUINTREG & 8` back. That is the Power+Reset test.
- `0x9FC00334`-`0x9FC0034C`: if that test fails and the mailbox at PA `0x2400` holds
  `0x03020100`, jump to PA `0x24FC` at once (a kernel still in RAM). Otherwise clear both
  words and run the full loader.
- The full loader always tries the card first: `0x9FC01088` makes GPIO5 an output and drives
  it high, waits, and returns `(A008 & 2) == 0`; then RESET, the COR write and verify
  (`0x9FC011FC`-`0x9FC0122E`; a mismatch calls `0x9FC003DC`), IDENTIFY, and sector reads.

So Power+Reset forces a full boot; it does not pick the card. A bootable card is booted on
any full boot, including a battery-pull power-on.

## 2. Uncited emulator assumptions

These have no hardware citation. Each needs a probe before it is changed.

- **`0x0A00A000` block.** Modelled as NAND "NFCNT/NFSTAT" (`src/hw/nand.c`), but both guests,
  the boot ROM and pcmcia.dll use `A000/A004/A008` as the card socket's control, RESET and
  status. The 0x43 "echo test" at D200 was the ROM's COR write and is gone (K1). The
  `0xC000-0xC0FF` "ROM transfer engine" (`nand.h:42-53`) overlaps the card I/O window and is
  still uncited.
- **PC-matched behaviour** in `src/hw/nand.c:1015-1030, 1264-1282, 1347-1358` and the forced
  busy-clear WORKAROUNDs at `src/be300_devices.c:711-788` (`0x234`, `0x8A0`, `0xA00`), all with
  TODO dates in 2026-04.
- **Touch geometry.** A/D ranges 0x81E1-0x8E30 / 0x8300-0x8D1B and the 360-row panel
  (`src/hw/piu.c:66-84`) were chosen here and copied into both guests.
- **Power-on table** (`src/be300_devices.c:1150-1225`) seeds the audio block, `0x1CC0-0x1CCC`,
  `0x8004`, `0x8040` and `A0DC` with CE steady-state values.
- **`0x0A00A040`** is hard-wired to 0x9EFF; whether a cold boot must write it is unknown
  (Linux writes it, Glaze does not).
- **CMCU watchdog.** 53/3A alone reloads it here; how expiry resets the CPU is a TODO;
  CONFIG bits other than 16 are a plain latch
  (`src/be300_devices.c:839-842`).
- **CPU.** One instruction per PClock cycle; each `mfc0 Count` adds 1 to Count; reset Status
  and PageMask; `INITIAL_STACK_POINTER`.
- **NAND.** ID `EC 73`, the burst-520 arm/clear conditions (`src/hw/nand.h:163-176`), instant
  busy, ACK accepted on writes, 4-byte address queue.
- **Display engine.** Instant completion, `0x238` unmodelled, stride register unknown; the
  brightness/contrast curve is illustrative (`src/hw/lcd.c:104-108`); LCD parameters, write
  order and settle delays are not required; gate `0x1110` unmodelled.
- **UART.** Baud modelled as 115200/divisor; the real baud clock is about 1.152 MHz.
- **Measured skew.** Real Count runs about 0.36% fast against the RTC (Z `kernel/timer.c:37-39`,
  `docs/hardware/hw_survey_v8.txt:4`); the emulator is exactly nominal.

## 3. Guest inconsistencies

| Topic | Linux | Glaze | Hardware evidence |
|-------|-------|-------|-------------------|
| NAND spare access | READOOB then discard/pad 512 bytes (works only because of N1) | resets the pointer with READ0 | none; a real chip favours Glaze |
| Wave status still set after ack | `-EIO` (`audio.c:106-109`); no init 0xD007 | tolerated, masked, 100 Hz poll | Glaze, on hardware |
| Dock UART | `uartclk` 1843200; no dock power-up; Stowaway toggles RTS every 20 ms | 1.152 MHz clock; socket.dll power-up; waits for DCD | Glaze, real Targus capture |
| GIRQ0 acknowledge | `setup.c:80-97` relies on I1 | masks bit 3 | Glaze, on hardware |
| GIUINTHTSELL bit 0 | set (NK 800A5F60) | cleared | CE dump 0xBFFF favours Linux; both work |
| Hit_Writeback_Inv_D | split into two ops | kept, with the Config workaround | Glaze; Linux's split is untested on hardware |
| SIU gate check | any one gate (kernel), all four (SPL) | all four | all four |
| LCD gate `110C` in panel sleep | left on | off, as NK does | none |
| `nand_boot.c` | GZNAND1 quarantine pre-pass | no pre-pass | shared file has diverged |
| WL54-CF COR | `D000+0x200` with `A020` bit 3 (what the emulator now requires for `D200`) | `0x0B400100` in the driver | "COR 0x41 at attribute 0x200 reads back" favours Linux's path |
| SPL power-on | non-diagnostic SPL skips `1B2C`, `1B20/1B24`, GPIO4/6 | sets them | lead for "4.2 stays black on hardware" |
| PIU `0x310/0x318`, `0x1118` | written | not written; bisected as unnecessary | both work |

## 4. How the fixes were checked

- Host unit tests (`ctest`), with `tests/test_cf_ata.c` added and `test_piu.c`, `test_rtc.c`
  extended. `test_modem` fails before and after these changes: it predates the jacket change
  that made a modem count as `cf_is_network` and the current RC56LD CIS.
- `tools/test_cold_boot.py --require-screenshot` on a Windows CE 3.0 NAND image: same serial
  milestones and the Align Screen prompt.
- A `--touch-script` run through first boot: hold each of the five calibration targets
  (120,160; 24,62; 24,258; 216,258; 216,62 at 60, 64, 68, 72 and 76 emulated seconds, 1.5 s
  each), then tap OK on the date/time setup (227,12 at 95 s). Windows CE reaches the CASIO
  shell's Main menu, with the dock ID at 0x0F. Before the W1 fix a "Restart: Due to an OS
  problem" dialog sat in front of the date/time setup and needed its own tap (186,69).
- Linux and Glaze were not built or booted for these changes.

## 5. Hardware probes still needed

Each can be done with BEDiag or Glaze's `hwdump` over the debug link.

1. `0x0A00A008`, `0x0A001000`, `0x0A00A03C` with and without a card, at power-on and after
   socket power-up (K5).
2. Whether writing a GIRQ0 source word clears its bit-7 latch, and what bits 4-7 read (I1).
3. NAND ECC registers `A464-A4C0` after a known page (N3); NAND ID through `A420` (90h).
4. Spare read after 50h through the XFER engine: does the stream start at byte 512 (N1)?
5. Programming a factory-bad block (N5) — only on a block already retired.
6. `0x0A00A040` at power-on, and whether buttons read without writing it.
7. Whether CMCU 53/3A alone reloads the watchdog; what STATUS reads through a reset.
8. Wave DMA: do `8B0/8B8` read live addresses; which status bit follows which pair (A1).
9. PIU page contents while idle, and raw sample spread during a held touch (T1, T2).
10. Config after power-on, before the ROM writes it, and after Glaze's bit 22/16 write (C1).
11. Which single SIU gate (if any) is enough to avoid the stall (U1).
12. Whether SUSPEND/HIBERNATE wake on POWERSWINTR (P1).
