# BE-300 piezo PWM

The buzzer uses a second channel in the shared BLG bank. The backlight's
enable at `0x0A000980` is not a tone trigger. Register evidence comes from
the user's stock WinCE 3.0 `bltinbuz.dll` at preferred base `018F0000` and
the captured startup MMIO writes. The original NAND SHA-256 is
`cfafca7f1bdcdd82f9a9980b4a684a9830562c8b8f19bc1287131c83a674d6ec`.
Vendor binaries, dictionaries and NAND images stay local.

| Companion offset | Use | WinCE evidence |
| --- | --- | --- |
| `0980`, `0988`, `098C` | Backlight control, period and duty | SPL/OAL/ddi; `BE300_LCD.md` |
| `0984` | Shared BLG config; buzzer ORs `0202` | `018F131C`, `018F1638..1640` |
| `09A0` | Buzzer control, 0 off / 1 on | `018F1304`, `018F1624`, `018F1678`; stop uses a halfword at `018F2190` |
| `09A4` | Buzzer period | `018F1658` |
| `09A8` | Buzzer duty | `018F166C` |
| `1128` | Shared BLG clock gate, bit 0 | SDK CMM resource range and playback gate RMW at `018F1604..1618` |

The saved unmodified startup trace confirms `018F1304/131C/1324/132C` writing
`0/0202/0/0` to `09A0/0984/09A4/09A8`. These initialization writes are silent.
The DLL's copied `.data` bytes are ROM-compressed and must not be mistaken
for initialized resource descriptors or pointers.

At `018F119C`, the clock constant is `00158880` = **1,411,200 Hz** and the
period calculation is `clock / requested_Hz - 1`, masked to 16 bits.
At `018F11F8`, duty is `period * (100 - pulse_percent) / 100 - 1`.
Normal playback passes 50 percent. Playback enables the shared clock,
stops the channel, ORs config `0202`, writes period/duty and sets control 1.
The counters' intervals include +1; the renderer keeps the rational clock
ratio rather than truncating the resulting pitch to integer Hz.

## Model

`src/hw/lcd.c` owns both observed BLG channels and their shared clock/config
so the devices cannot acquire conflicting copies of a shared register.
Byte, halfword and word accesses retain the existing little-endian storage.
`lcd_buzzer_signal()` exposes the recovered counter settings only when
the buzzer enable, shared clock and observed configuration bits are set.
Constant-output duty settings are silent. An off panel or backlight alone
does not suppress the separate buzzer.

After BLG or shared clock writes, the companion dispatcher updates the
host signal. LCD/backlight writes cannot invent a tone. The legacy NAND
recovery buffer starts at `0B00`, outside the buzzer channel. The separate
`0C00..0C4F` LCD/NAND exclusion remains unchanged.

Native SDL uses one software PCM ring and an audio callback that mixes
the continuous piezo signal with the guest's resampled stereo wave stream.
Each host frame gets the same buzzer sample on L/R, with saturation after
adding it to each guest channel. Native state/ring access is protected by
the SDL audio-device lock; audio callbacks never access guest memory.
PWM stops and changes take effect at the next callback without clearing
headphone samples or suppressing the buzzer while PCM is queued.

The web worker keeps its wave ring and renders the current PWM into drained
stereo frames. Tone-only frame generation is limited by elapsed host time,
with bounded catch-up after a worker pause. Repeated drains at one time
cannot generate an unlimited future tone. AudioContext remains owned by
the page; the worker does not open an SDL audio device. Mute resets queued
host data and timing but does not rewrite guest registers or stop guest DMA.

The old `ui_buzzer_pulse()` frontend helper remains for embedding callers;
no guest MMIO invokes it. The former backlight heuristic was removed in
`d4e6bab4` and is not restored here.

## Limits and tests

The observed driver programs 16-bit counts and config `0202`. Other BLG
channels/configuration modes, physical output polarity, acoustic response,
analog filtering and actual piezo loudness remain unmeasured. The host's
amplitude/DC-centering is an audio presentation choice. Host callback and
browser scheduling determine presentation latency. Physical-device checks
of Glaze's matching driver remain outstanding.

`test_lcd` checks startup silence, channel isolation, clock/config gating,
halfword stop and tone output with the panel dark. `test_ui_audio` and
`test_ui_web_audio` check independent stereo mixing, silence after stop,
PCM queue bounds and bounded web generation. `test_nand_boot` checks that
neither BLG channel enters the recovery dispatcher. Existing PCM/DMA and
unmodified WinCE cold-boot tests remain required.
