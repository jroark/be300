# Battery, AC adapter, charger and the calibration EEPROM

What Windows CE's battery driver reads is modelled, so a guest can be
tested on battery, while charging and as the battery runs down. The driver
is compiled into `gwes.exe` (`BatteryPDDGetStatus`, 0xB02CC–0xB09DC; VA =
file offset + 0x10000 in be300-modules `03_gwes.exe.bin`). glaze-os
`docs/POWER.md` describes the guest side.

| Signal | Register | Model |
|---|---|---|
| Adapter | GIUPIODL `0x0F000144` bit 12 (1: on battery) | `battery_gpio()`, through gxemul's `dev_vr41xx_set_giu_pins_hook` |
| Charger | GIUPIODL bit 2 (with the adapter: 1 charging) | same |
| Voltage | PIU A/D port result, `0x0A000390` | `piu_state_t.adport`, served by the latch region `vrc4173-piu-adport` |
| Calibration | serial EEPROM, word address 0x94/0x95 | `src/hw/eeprom.c`, on SCL `0xA19C`, SDA out `0xA1A0`, SDA in `0xA1A4` (in the NAND window) |

- **A/D conversion** (`src/hw/piu.c`): a PIUCNT write with PIUSEQEN and
  PIUMODE 01 (bit 3) converts `aux_adc` at once. It sets PIUINT bit 6
  (PADADPINTR) and latches the 12-bit result.
- **Battery level** (`src/hw/battery.c`): the level maps piecewise
  linearly onto CE's own scale. That is 0 % at 3.4 V (`min - 3*span/5`, an
  edge gwes computes but doesn't use, the manual's cut-off), then 10, 25,
  50, 75 and 100 % at its band edges (3.65, 3.7, 3.8, 4.0 and 4.2 V).
  Glaze reads the configured level back. Windows CE shows it rounded up to
  its next band, so `--battery 40` reads 50 % there.
- **Real dumps** read `0xC6D`–`0xE22` at `0x0A000390`
  (`docs/hardware/hw_dump_diffs.txt`).
- **The EEPROM** is a two-wire slave at device 0xA0. It starts erased
  (0xFF), because real units' factory bytes are unknown. CE and Glaze then
  take their default calibration.

## Options

| Option | Effect |
|---|---|
| `--battery PCT` | level, 0–100 (default 100) |
| `--battery-state ac\|charging\|battery` | default `ac`: adapter in, charged (GIUPIODL 0xAAE2, as before) |
| `--battery-drain N` | N % per emulated minute: down on battery, up while charging, ending in `ac` at 100 |
| `--eeprom-cal B0,B1` | EEPROM bytes 0x94 (max) and 0x95 (min) |

The config stores the level as the percentage *used*. A zeroed
`machine_config_t` (web, Android, launcher) is therefore a full battery
with the adapter in.

## STANDBY

gxemul's VR41xx STANDBY used to reuse WAIT and left Status.IE alone. The
VR4131 sets IE as STANDBY executes, and Linux's `vr41xx_cpu_wait` relies on
that by issuing it with interrupts disabled. A kernel that does the same
would never wake. `instr(standby_vr41xx)` now sets IE before idling.
SUSPEND is unchanged, because NetBSD/hpcmips uses it as a halt.

## Not modelled

- The PMU's battery interrupt (PMUINT BATTINTR, SYSINT1 bit 0).
- The voltage detectors (VDET0B, VDET2, VDETCF).
- The EEPROM's write cycle.

`src/hw/pmu.h` numbers PMUINT's bits differently from VR4131 UM 12.2.1
(bit 1 BATTINTR, bit 3 RSTSW, bit 4 RTCRST, bit 5 TIMOUTRST, bit 8
BATTINH, bit 10 DCDST), which a guest reading BATTINTR would notice. That
is left for a separate change.

`tests/test_battery.c` drives the models as CE's drivers do.
