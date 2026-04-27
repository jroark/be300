# Casio PC Connect Time Sync Notes

Goal: make host date/time sync into the emulated BE-300 through the same
guest-visible PC Connect path used by real hardware, not by preloading RTC or
patching guest memory.

## Current Evidence

The local PortMon captures are real Win2K `PCconnect.exe` sessions against a
BE-300 on `VCP0`.

Observed serial setup:

- baud: `115200`
- framing: `8N1`
- write timeout: `WM:2 WC:500`
- host wake starts as single-byte writes of
  `41 54 00 00 0d 00 00 00 55 55`, repeated twice
- the BE-300 emits a `0x20` ready byte before the host sends the first
  framed PC Connect command
- repeated `0x55` sync bytes appear around the wake phase; after the `0x20`
  ready byte the captured host mirrors two more `0x55` bytes, then sends the
  first framed PC Connect command

Important visible command strings from the first masked text capture:

- `Platform`
- `Release`
- `Software\CASIO\PCLINK`
- `VERSION`
- `nowsynchro.exe`
- `NowSynchro.exe`
- `NowSynchro`

The host-side restore installer in `ce/restore_images/Setup.exe` imports Win32
`GetSystemTime` and `WilSetSystemTime` from `PCLADP.dll` along with other
`Wil*` remote file, registry, NAND, and process APIs. That strongly suggests PC
Connect exposes a RAPI-like host DLL API, and time sync is a normal PC Connect
command rather than an RTC side channel.

The second local capture, `pcconnect.log`, shows raw byte prefixes. PortMon
still truncates long transfer display columns, but the short frames establish
the frame shape. `pcconnect_2.log` preserves the full long frames, including the
time-sync payload:

```text
10 08 00 1A 03 49 02 29 00 A9 07 EA 00 04 00 00 00 1A 00 00 00 2B 00 34 00 BB
```

Frame layout inferred from complete short frames:

- byte 0: direction marker, `0x10` host command or `0x11` device response
- byte 1: sequence id
- bytes 2-3: big-endian total frame length
- byte 4: command class, observed `0x03`
- byte 5: opcode
- bytes 6-7: big-endian byte-sum over the command payload
- bytes 8-9: big-endian byte-sum over bytes 0-7 after bytes 6-7 are set
- bytes 10-end: opcode-specific big-endian payload

The `0x49` frame is the `WilSetSystemTime` command. Its payload is a
big-endian Win32 `SYSTEMTIME`: year, month, day-of-week, day, hour, minute,
second, milliseconds. The capture above decodes as
`2026-04-26 00:43:52.187`, Sunday, matching a Win32 `GetSystemTime` UTC value.

Use:

```bash
python3 tools/parse_pcconnect_portmon.py pcconnect.log
```

to inspect preserved direction, declared length, visible byte prefixes, and
truncation state without treating truncated PortMon rows as complete frames.

## Emulator Integration

`--pcconnect-time-sync` enables an experimental peer on the VR4131 SIU by
default. Set `BE300_PCC_UART=vrc4173siu` for the companion SIU at
`0xaa008680`, or `BE300_PCC_UART=auto` to claim both candidate UARTs during
short diagnostics.

The guest-visible cable insertion path is the VRC4173 Vic/CommMode page:

- `docs/hardware/hardware.txt` identifies `0xaa008000` as Vic/CommMode and
  `0xaa008680` as the companion SIU.
- The GIRQ0-4 dispatcher reads `AA008004` as pending bits in byte 0 and mask
  bits in byte 1, with sub-bit 0 and sub-bit 4 dispatches.
- Runtime socket.dll probes show the low five bits of `AA008010` select a
  socket table entry. Observed values include `0x0007` for an empty no-driver
  entry, `0x0008` for `serial.dll`, `0x000c` for `usb.dll`, and `0x000d` for
  VCom.

For serial PC Connect, the emulator masks the `AA008010` socket selector to
`0x0007` until an emulated cable edge, then presents `0x0008`. This prevents
the stale hardware-dump seed value `0x000c` from launching `StartingUSB.exe`
before any emulated cable is inserted. The edge is normally delayed until after
the Boot.exe reset; `BE300_PCC_CONNECT_DELAY_MS` controls that delay. For
manual guest-side PC Connect testing, the edge can also be raised when the
claimed UART is in 8N1 mode and the guest asserts DTR/RTS.

The default post-reset cable edge delay is 1000 ms. Larger values are useful
for diagnostics, but make no-CF PC Connect boots appear stalled while the
guest waits for the dock insertion path.

The peer currently:

- claims only the selected ns16550 instance
- suppresses binary PC Connect bytes from the normal console path
- waits for a cable edge and a guest-ready UART before queueing the captured
  `AT\0\0\r\0\0\0UU` wake sequence twice
- mirrors early `0x55` sync bytes until the guest emits the `0x20` ready byte
- clears stale sync echo bytes, mirrors two post-ready `0x55` sync bytes, then
  sends a captured `0x2D` probe frame
- delays each post-ready host response by about 4 ms to match the captured
  serial cadence and avoid presenting receive data while the guest is still
  inside its transmit path
- raises the CommMode sub-bit 4 edge whenever post-ready host RX data becomes
  available. The BE-300 serial port lives on the VRC4173 companion
  (`hardware.txt`: Vic/CommMode at `0xaa008000`, companion SIU at
  `0xaa008680`), and NK dispatches `AA008004` through GIRQ0-4, so the guest
  does not reliably drain the `0x2D` and `0x49` frames from UART status alone.
- waits for the guest `0x2D` response before sending a generated `0x49`
  `WilSetSystemTime` frame using current host UTC
- optionally traces raw guest/host bytes with `BE300_PCC_TRACE=1`

This is intentionally scoped to time sync. It does not emulate file, registry,
NAND, or process-management PC Connect APIs.
