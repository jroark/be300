# BE-300 LCD model and recovered startup evidence

The companion LCD bank is at PA `0x0A000C00..0x0A000C4F`, with backlight
words at `0x0A000980..0x0A00098F`. A separate piezo PWM channel lives at
`0x0A0009A0..0x0A0009AB`; both share config `0984` and clock `1128`.
The LCD is separate from the drawing engine
at offsets `0x0200..0x0234`. The framebuffer is RGB565 at PA `0x0A200000`
(KSEG1 `0xAA200000`), with 240 visible pixels, 320 rows, and a 256-pixel
(512-byte) software pitch. Row padding is not presented to the host.

## Sources and confidence

The Casio BE-300 JX-740A service manual (September 2001, printed pp. 2,
18, 50, 53, 55–56, 63–65) describes the 240×320 STN panel and separate
brightness/contrast controls. Its panel color specification does not change
the guest framebuffer's independently identified RGB565 format.

The following addresses refer to disassembly of the unmodified WinCE 3.0
`300.bin` NAND, SHA256
`cfafca7f1bdcdd82f9a9980b4a684a9830562c8b8f19bc1287131c83a674d6ec`.
The SPL and NK/OAL writes were also captured during an unmodified ROM cold
boot. NK writer PCs use uncached aliases `A007....` in the trace.
`ddi.dll` addresses are relative to its preferred image base `0x01A50000`.
Guest binaries, extracted modules and NAND images remain local-only.

| Code address | Evidence |
|---|---|
| SPL `80F034A0`, NK `80078F38` | Panel parameter writes, two clock/gate writes, serial contrast and staged enable. NK skips setup if C48 is already `030F`. |
| SPL `80F03638`, `80F03734` | RGB565 bitmap/rectangle operations at AA200000; row shift 9, pixel shift 1; 240×320 bounds. |
| NK `80079268` | Fills 480 visible bytes per row, advances 512, ends at 0x28000. |
| SPL `80F038DC`, ddi `01A55900` | Backlight setup/power; CMM 1128 bit 0, control 0980, mode 0984, period 0988, duty 098C. |
| SPL `80F0393C` | Shutdown before NK handoff: C48 `020F → 0201 → 0`, C30=1; backlight off. |
| ddi `01A545CC` | Maps source AA200000, length 0x40000; mapping stored at object+6C. |
| ddi `01A54128/01A54134`, `01A54834/01A54838` | Literal software pitch 512 and explicit row/pixel shifts. |
| ddi `01A56468` | Capability construction: width 240, height 320, RGB565 masks. |
| ddi `01A55994`, `01A55EB0` | Panel off/on implementations, same panel parameter table. Object+3C maps C00. These routines were not the startup writers in the captured boot. |
| ddi `01A56A5C` | Contrast serialization: data bit 2, rising clock bit 1, latch bit 0 at C4C; eight bits MSB first. |
| ddi `01A56BB0/01A56BD0`, tables `01A52050/01A52070` | Backlight duty table includes 3339, 2553, 1964, 981, 0; zero is maximum brightness, not off. |

Backlight setup programs 1128=1, 0980=0, the low nibble of 0984=2,
0988=0F70, 098C=0, then 0980=1. The recovered SPL/OAL panel sequence is:

1. Write 1 to companion gates 1100 and 110C.
2. Program C00=20, C04=02, C08=50, C0C=F1, C10=02, C14=0,
   C1C=0, C24=14 (hexadecimal).
3. C48=0, then 0001; delay; C48=0101; C2C=1; serial contrast C1.
4. C48=0103; delay; C48=010F; delay; C48=030F.

The OAL delay helper `80077210` receives 1000 and 20000 for enable
settling and 5 between contrast edges. SPL uses its RTC-based helper
`80F027BC` with different argument scaling. Logging wall times include host
and container overhead and are not electrical timing measurements.
The ddi wrapper uses object+54 mapped at 1110 where SPL/OAL use 1100;
its entire power wrapper must not be assumed identical to SPL/OAL.

## Implemented behavior

`src/hw/lcd.c` owns the recovered LCD/backlight words and three shared gate
words. MMIO reads/writes preserve little-endian bytes and unknown bits.
CMU dispatch routes LCD-owned bytes to this state, retaining its existing
0x40-byte-window aliases and other devices' words. LCD dispatch precedes the
legacy NAND recovery region; LCD writes cannot run its page/command logic.
The old post-boot LCD and BLG seeds are removed. LCD storage starts at zero
and presentation starts off; unknown power-on constants are not claimed.

Presentation is active when C48 contains all observed enable bits `030F`,
1100/110C bit 0 are set, and backlight control 0980 / CMM 1128 bit 0 are
set. This is a digital power approximation based on the enable/disable
paths, not a decoding of each rail or panel signal. The model does not
require a specific sequence, parameter table or artificial initialization
flag. A guest may change unknown fields or preserve an already active panel.

Inactive presentation returns black through native SDL, screenshots and the
shared web/Android RGBA frame-copy API. Guest VRAM remains writable and
unchanged, and reappears when enabled. Black is a host blanking convention;
the unpowered reflective STN panel's appearance is not simulated. Backlight
duty zero stays visible. Contrast clock/latch edges record the command.

So that guest brightness and contrast controls are visible, presentation
applies an approximate curve to the copied pixels, never to VRAM. Backlight
gain is 30% + 70% × (1 − duty/period) from 0988/098C, so ddi.dll's duty
table reads as roughly 41%, 53%, 63%, 82% and 100%. Contrast scales each
channel about mid-grey by 1 + 0.08 × (C1 − command) / 3, about 8% per
ddi.dll level (command 208 − 3 × level). Duty zero with no latched command
or command C1 presents pixels unchanged. These constants are illustrative,
not a measured optical response.

The former BLG buzzer heuristic interpreted backlight enable/configuration
as tone triggers and converted 0988 into a guessed frequency. It has been
removed, including its enabled-at-reset snapshot. Wave PCM and host tone
rendering APIs are unchanged. The actual buzzer channel has since been
recovered from `bltinbuz.dll`; [BE300_BUZZER.md](BE300_BUZZER.md) describes
its independent PWM model and evidence. Backlight activity still cannot
trigger it.

## Limits and validation

No recovered code establishes a programmable framebuffer base or pitch.
Hardware dump C34=`01FF` resembles 512−1 but is not proof of pitch semantics;
it remains plain storage. C00..C24 timing fields, refresh rate, porches,
sync polarity, clock divisors, status/scanline counters, electrical settle
times and the panel's real brightness/contrast transfer curves remain
unknown. Apart from the approximate presentation curve above, this model
does not guess them or enforce delay durations. Glaze programs the recovered
sequence and delays independently; the emulator does not perform it for guests.

`test_lcd` covers reset-off state, individual gates/enable bits, byte/halfword/
word/64-bit readback, serial edges/latching, both presentation formats,
row padding, duty zero, the approximate brightness/contrast curve and
VRAM retention across blanking. `test_nand_boot`
checks that the recovery dispatcher excludes the LCD/backlight bank.
The local `cold_boot` regression requires an unmodified WinCE shell boot and
a nonblank screenshot, using a temporary NAND copy. Glaze is additionally
checked with its independent LCD initialization and an older image without
setup as a negative control. Source images must remain unchanged.

The remaining legacy NANDWRITER C-window mapping needs revalidation against
local recovery binaries; this change does not claim CF recovery correctness.
Cold ROM/SPL NAND transfers use their separate controller path. Physical
power-cycle and suspend/resume comparisons are still needed to refine the
LCD model beyond the recovered software-visible digital behavior.

Local validation on 2026-10-26: native build and all 22 CTests passed,
including the 60-second WinCE cold boot reaching the Casio shell. Both
normal NAND and RAM-trampoline Glaze builds reached SpringBoard with visible
screenshots; an older Glaze image reached SpringBoard but stayed entirely
black without panel setup. The new image's 42 ordered panel/contrast stores
match the WinCE SPL capture. Source NAND SHA256 values stayed unchanged.
The portable LCD test also passed ASan/UBSan with strict compiler warnings;
20 browser tests and the Emscripten 3.1.64 build passed. A Node-hosted WASM
worker smoke test checked reset blanking and the exported RGBA copy API
with new and old Glaze images; this does not constitute a physical-device
or Android application test.
