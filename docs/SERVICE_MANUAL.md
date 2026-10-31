# Service-manual evidence and regression checklist

Source: Casio **BE-300 (JX-740A), September 2001 service manual**, supplied as
`007354722.pdf`. Page references below are the printed numbers (PDF page =
printed page + 2). The local PDF and device/service binaries are not distributed
with this repository.

## Board evidence

| Component / behavior | Source | Emulator implication |
|---|---|---|
| VR4131; nominal 166 MHz, diagnostic 165 MHz CPU / 82 MHz VTClock | pp. 2, 51, 63, 65 | Retain the established 165.888 MHz CPU timebase and existing Count ratio. |
| IC1: custom **RAKKO** gate array; IC300: **CAK2007** analog AD/DA | pp. 63–65 | Companion VRC4173 names in code are historical. Preserve register behavior established by dumps/driver traces; the schematic is not a replacement MMIO specification. |
| IC3 EEPROM, SDA/SCL/address-select/write-protect pins; factory UUID | pp. 50, 65 | WinCE's bit-banged bus registers are identified in `WINCE_DISPLAY_CONTROLS.md`; component capacity, factory contents and persistence behavior still need verification. Do not fabricate factory contents to satisfy diagnostics. |
| Independent AC, charging, battery detectors, card-power detection and jacket SW2 | pp. 2–3, 52–55, 64–65 | AC (GIUPIODL bit 12), charging (bit 2) and the battery A/D port are modelled from gwes.exe's driver (`docs/BATTERY.md`, `--battery*`). The VDET detectors, card-power detection and jacket SW2 still need measured signal/IRQ routing. |
| ADC calibration at 4.2 V / 3.7 V; contrast Evol calibration | pp. 53–56 | Battery calibration bytes are EEPROM 0x94/0x95 (gwes.exe 0xB0AF0; `--eeprom-cal`); real units' values and the contrast trim (0x88) are still unknown. |
| 240×320, 32,768-color STN panel; separate contrast/backlight controls | pp. 2, 18, 50, 53, 55–56, 63–65 | Keep panel presentation separate from the hardware-observed RGB565 framebuffer format. |
| Stereo headphone output and separate buzzer | pp. 2, 5, 52, 63–64 | Preserve guest L/R PCM in native and web output. Guest DMA must advance even when host sound is muted. |
| Two K4S641633D-GN96 parts, IC6/IC7, DQ0–15 / DQ16–31 on shared controls | pp. 65, 78 | Stock 16 MiB on a 32-bit bus. The bare-chip 52-ball CSP/BGA assembly does not establish a compatible physical RAM upgrade. |
| Normal reset, Power+Reset initialization, OK+Reset safe mode | pp. 25–27, 39–40 | Exercise electrical reset/button inputs and guest behavior; establish retention before widening the current KjCMU reset scope. |
| Diagnostic boot-ROM checksum addresses BE000000 / BE000002 | p. 51 | Investigate the normal ROM-select window with hardware reads and chip-select settings before adding an alias to the captured reset ROM. |

## Regression matrix

| Service procedure | Expected observation | Coverage / outstanding evidence |
|---|---|---|
| CPU / VTClock readout, p. 51 | Approximately 165 / 82 MHz | Existing timebase; retain RTC/PMU/timer regressions. |
| 0.5 / 1 / 2 kHz and channel tests, p. 52 | Correct pitch and channel routing | `audio_pcm`, `test_ui_audio`, `test_ui_web_audio`, `audio_dma`; `tests/web_audio.mjs` and the frontend integration test cover worker/playback packets. Factory LR waveform still requires local Diag.exe. |
| Display fills, white frame, grayscale, pp. 50, 53 | Correct bounds/colors and panel controls | Digital panel/backlight gating and serial contrast tracking implemented (`BE300_LCD.md`); optical curves remain unmeasured. |
| Touch points / tracing cross, p. 50 | Stable calibration and continuous tracing | Existing `piu` tests; service-application interaction remains a local check. |
| Keys and Power+arrow chords, pp. 18, 50, 53 | Correct key transitions and LCD controls | `ui_input` and browser integration tests cover source ownership, Power+Up, focus release and Stowaway aliases. Display-control writes are identified in `WINCE_DISPLAY_CONTROLS.md`; panel response needs measurements. |
| RAM / NAND / boot-ROM checks, p. 51 | Successful memory access without modified guest bytes | Existing NAND/cold-boot tests; normal-ROM alias remains unverified. |
| AC / charge / VDET / jacket, pp. 52–55 | Independent input transitions | `test_battery` covers AC/charge lines, the A/D port and the calibration EEPROM; VDET and jacket need captures. |
| Reset / initialization / safe mode, pp. 25–27, 39–40 | Correct guest path and persistent NAND | Existing CPU/software reset implementation; physical reset routing/retention needs verification. |
| CF insertion while off, pp. 46, 49 | Wake and card auto-run | Existing socket model; service-program wake/auto-run remains a local check. |
| Identity and calibration readback, pp. 50, 53–56 | Stable per-unit EEPROM values | Needs EEPROM transactions and a local content capture. |

## Running checks

Configure a host build with `BUILD_TESTING=ON`, then build and run CTest. The
stereo resampler/ring test has no SDL or guest-image dependency. When SDL
headers are present, two recording-sink tests compile the production SDL
adapter and browser-ring adapter, checking exact PCM and queue/reset behavior.
The audio DMA test uses a controlled clock to compare audible and muted buffer
completion, status and interrupts. Run the browser tests with:

```sh
node --experimental-vm-modules --test tests/web_audio.mjs tests/web_launch_config.mjs
```

For full-system validation configure
`-DBE300_TEST_NAND=/absolute/path/All_nand_300.bin`. The local `cold_boot` CTest
uses a temporary copy, checks ordered WinCE 3.0 boot milestones and, with SDL,
screenshots, and verifies that the source image hash stays unchanged.

Casio's service `AutoRun.exe` / `Diag.exe` are separate local prerequisites:
`CE/R4100/AutoRun.exe` and `JX740diag/CASIOconfidential/Diag.exe` on CF. Follow
pp. 45–56 on disposable NAND/EEPROM copies; diagnostic exit/initialization can
remove data. Exclude the factory-only UUID-write operation from automation.
The manual itself does not provide the executables, transaction captures,
register addresses, reset timing or ADC conversion law needed for the later
models. Those items remain evidence-gathering tasks.

Driver analysis can proceed without the factory diagnostic binaries. See
`WINCE_DISPLAY_CONTROLS.md` for the stock WinCE 3.0 contrast, backlight and
EEPROM routines already identified from the local NAND.
