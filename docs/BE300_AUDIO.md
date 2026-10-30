# BE-300 Wave Audio Output

The BE-300 has two sound paths:

- **Buzzer**: separate BLG PWM channel at `0x0A0009A0/9A4/9A8`, recovered
  from `bltinbuz.dll`. It shares config/clock with the backlight but uses
  independent counters and enable. The former `0x0A000980` backlight-to-tone
  heuristic remains removed. See [BE300_BUZZER.md](BE300_BUZZER.md).
- **Wave output** ("Buzzer and headphones" in Volume & Sounds): `wavedev.dll`
  drives a Casio DMA block in the Kj companion window. This document covers
  that path. It is not the VRC4173 UM Ch. 10 AIU (Pass 33 saw zero accesses
  at VRC4173 `+0x0E0..+0x0FC`).

Emulation: `src/hw/aiu.c`, wired into the companion latch in
`src/be300_devices.c`. The DMA and interrupt model always runs. `--audio`
only routes the PCM to the host SDL device.

## wavedev.dll

XIP module `wavedev.dll`, vbase `0x01910000`, built Aug 28 2001. It imports
`VRB_GetResDma` / `VRB_RelResDma` from `cdm.dll`.

`WAV_Init` path at `0x019124E4` uses VirtualAlloc (coredll ord 0x20C) and
VirtualCopy (ord 0x230) to map these windows:

| Global | Source | Size | Use |
|---|---|---|---|
| `0x019150E0` | `0xAF000000` | 0x1000 | VR4131 regs |
| `0x019150BC` | `0xAA000000` | 0x2000 | Kj companion audio regs |
| `0x019150B4` | `0xAA00A000` | 0x1000 | KjCMU |
| `0x01915098` | `0xAA008000` | 0x1000 | Vic/CommMode |
| `0x019150C0` | `0xA004A000` | 0x800  | DMA buffer window |
| `0x019150C8` | `0xA004A000` | 0x6000 | buffer 0 (`+0`) / buffer 1 (`+0x3000`) |

The interrupt is SYSINTR `0x14`, stored at `0x019150E8` by `0x019124EC`.
The IST loop is at `0x01912E74`. It calls WaitForSingleObject, then
InterruptDone (ord 0x274), then the status decode at `0x0191274C`.

## Data format

Both buffers always hold **signed 16-bit little-endian PCM**. They are mono
or interleaved stereo. The fill routine at `0x019114EC` dispatches on the
format index (`0x019150DC`, jump table `0x01911054`):

| Index | Source format | Written to buffer |
|---|---|---|
| 0 | 8-bit mono | S16 mono (`(u8-0x80)*vol >> 8`) |
| 1 | 16-bit mono | S16 mono (`s16*vol >> 16`) |
| 2 | 8-bit stereo | S16 stereo |
| 3 | 16-bit stereo | S16 stereo |
| 4, 5 | 16-bit mono | S16 mono (`s16*vol >> 12`) |

Buffer size is set at `0x019119FC`: `0x600 * class` bytes. `class` is 1 below
15001 Hz, 2 below 26001 Hz, and 4 otherwise. It doubles again for stereo.

## Registers (offsets from PA `0x0A000000`)

| Off | Meaning | Evidence |
|---|---|---|
| `0x3C0` | Codec serial command. Start writes `0x5000`, then `(0x3F4&0x80)\|0xD007`, `0xD007\|x2<<7`, `0xD001\|x2<<7`. Stop writes `0xD007`. | `0x01911280`, `0x01911CF0` |
| `0x3C4` | Codec ready. wavedev spins until it reads non-zero. Real-HW value `1`. | hw_dump_vrc4173.txt:994 |
| `0x3F4` | Codec status. wavedev folds bit 7 into a command. | `0x019112B8` |
| `0x880` | Bits 6:4 = rate code. Bit 3 = output enable. Bit 2 = idle. OAL reset value `4`. | `0x01911244`, `0x01911C9C`, OAL `0x800A8FDC` |
| `0x884` | Bit 0 = enable, bit 1 = codec on, bit 2 = sampled before arming. OAL reset value `1`. | `0x019112F8`, `0x01911398` |
| `0x888` | Bit 1 set = mono (cleared for formats 2/3). Bit 2 cleared at rate setup. | `0x01911C0C`, `0x01911268` |
| `0x890` | Bit 1 set on start. | `0x01911C74` |
| `0x898` | Bit 0 = DMA run. | `0x01911374`, OAL `0x800A8FDC` |
| `0x89C` | In the OAL handler, bit 7 selects SYSINTR `0x20` instead of `0x14`. Bit 2 is cleared there. | OAL `0x800A8E58` |
| `0x8A0` | Bit 0 = output armed. Start writes 1; stop writes 0. NK polls it from `0x80079924`. | `0x01911378`, `0x01911D04` |
| `0x8A8` | **Status. Bit 0 = buffer 0 drained, bit 1 = buffer 1 drained.** | `0x0191275C` |
| `0x8B0`/`0x8B4` | Buffer 0 start / end (inclusive). wavedev writes kseg1 VAs (`0xA004A000`/`0xA004A5FF`); the bus uses the PA. | `0x019114A0` |
| `0x8B8`/`0x8BC` | Buffer 1 start / end (inclusive), `0xA004D000`... | `0x019114A0` |
| `0x8C4` | Written 1 at start. | `0x019113D0` |
| `0x8C8`/`0x8CC` | Ack. The IST writes `0x11` to both after reading `0x8A8`. Start/stop write `0x10` then `0x11`. | `0x019127BC`, `0x01911B78` |
| `0x1104`, `0x1118` | Power/wake enables. | `0x019113CC`, `0x01912484` |

### Rate encoding

`0x019111DC` maps the requested rate to `0x880` bits 6:4. A codec
double-rate bit (bit 7 of the `0xDxxx` command) doubles some rates:

| Requested Hz | `0x880 & 0x70` | x2 | HW rate |
|---|---|---|---|
| < 10001 | `0x40` | 0 | 8000 |
| 10001-15000 | `0x10` | 0 | 11025 |
| 15001-19000 | `0x30` | 0 | 16000 |
| 19001-26000 | `0x00` | 0 | 22050 |
| 26001-40000 | `0x30` | 1 | 32000 |
| > 40000 | `0x00` | 1 | 44100 |

The HW rates are inferred from the thresholds. Code `0x20` is never used.

## Start / stop sequence

Start (`0x01911B8C`):

1. Program the format (`0x888`) and the buffer pointers (`0x8B0..0x8BC`).
2. Set `0x890 |= 2`, ack `0x8C8`/`0x8CC`, and set `0x884 |= 1`, `0x880 |= 8`.
3. Write `0x8C4 = 1`.
4. Run the codec rate sequence (`0x3C0`).
5. Write `0x898 = 1`, then `0x8A0 = 1`. DMA begins with buffer 0.

Stop (`0x01911CF0`): write `0x8A0 = 0`, `0x880 &= ~8`, `0x884 |= 1`,
`0x898 &= ~1`, `0x890 &= ~2`, then the codec off command `0xD007`.

## Interrupt path

1. A buffer drains. `0x8A8` bit 0 or bit 1 sets.
2. Companion level-1 source `AA000004` bit 8 asserts, gated by the source
   enable `AA000030` bit 3. Real-HW idle value of `AA000030` is `0x0C`
   (hw_dump_vrc4173.txt:27). The source cascades through GIU GIRQ0.
3. OAL handler `0x800A8DCC` clears `AA000030` bit 3. It returns SYSINTR
   `0x14`, or `0x20` if `0x89C` bit 7 is set.
4. wavedev's IST reads `0x8A8` and acks both `0x8C8` and `0x8CC`. It
   refills the drained buffer: bit 0 → event `0x10`, bit 1 → event `0x20`.
   It stops output when the wave queue is empty.

## Emulator model

- DMA plays buffer 0 then buffer 1, alternating. Buffer bounds are latched
  when each buffer is entered.
- The cursor advances with host monotonic time at the decoded rate (the
  main-loop throttle uses the same clock).
- When a buffer drains, the model sets its `0x8A8` bit and updates the GIRQ0
  line. Writing bit 0 to `0x8C8` clears status bit 0; writing bit 0 to
  `0x8CC` clears status bit 1.
- Run state is `0x898` bit 0 AND `0x8A0` bit 0.

Verified 2026-10-12: Settings → Volume & Sounds → Volume tab → headphone
volume arrow. wavedev starts 11025 Hz mono with 1536-byte buffers. Buffer 0
drains and SYSINTR `0x14` reaches the IST. wavedev acks and stops (the click
fits in one buffer). The buffer contains non-zero PCM.

Open questions:

- Exact enable/ack split of the `0x8C4`/`0x8C8`/`0x8CC` bits.
- Whether reads of `0x8B0`/`0x8B8` return the live DMA address. The real-HW
  dump shows both at `0x4D000`.
- The capture path (`0x19113F4`, SYSINTR `0x20`).

## Host stereo output

The Casio service manual (printed pp. 2, 5, 52) specifies stereo headphones
and an audio/channel service check. Both native SDL and Web Audio preserve
interleaved S16 L/R PCM at 44100 Hz. Mono guest PCM and host-generated tone
pulses are presented equally on both host channels. `src/audio_pcm.c` performs streaming
linear interpolation with integer phase, keeping each channel separate and
preserving results across arbitrary DMA chunk boundaries. A rate/channel
change or a new guest DMA stream resets interpolation history.

The web drain API uses **frames**, not samples or bytes: the destination
holds `max_frames * 2` S16 samples and the return value is a frame count.
Packets carry `frames`, `channels: 2`, `sampleRate: 44100` and an owned
interleaved `Int16Array`. Ring overflow discards whole oldest frames. Queue
limits are measured in stereo bytes so their latency bounds stay unchanged.
Sound toggles reset the web ring and interpolation history; host muting never
changes guest DMA cursor/status/interrupt advancement.

See `docs/SERVICE_MANUAL.md` for the evidence map and regression checklist.
