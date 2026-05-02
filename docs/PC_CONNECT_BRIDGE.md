# PC Connect Bridge — Wire The Emulator's Dock UART To A Host Chardev

`--pcconnect-bridge` pipes the emulated VRC4173 SIU UART (the BE-300 dock
serial port at PA `0xaa008680`) to a host-side chardev — TCP socket, Unix
socket, or PTY. Pair it with a UTM Windows VM whose virtual serial port
sits on the other end of that chardev, and real `PCConnect.exe` running
in the VM can talk to the live WinCE 3.0 guest in the emulator.

## Why a serial bridge, not USB redirection

The BE-300 dock cradle has a USB-to-UART bridge IC inside it. WinCE on
the device drives a plain 115200 8N1 UART; Windows on the PC enumerates
the cradle as a USB→Serial COM port. `pcconnect.log` and
`pcconnect_2.log` in the repo root are real PortMon captures proving
this — pure serial framing (`0x10` host markers, `0x11` device markers,
opcode `0x49 = WilSetSystemTime`).

Neither the VR4131 SoC nor the VRC4173 companion chip has a USB function
(gadget) controller, so emulating the BE-300 as a USB device is
fictional. The right model is "the emulator's UART = the cradle's UART;
let UTM put a `usb-serial` device in front of it on the PC side."

This bridge is mutually exclusive with `--pcconnect-time-sync` (the small
synthesizer that fakes a `WilSetSystemTime` frame for the WinCE first-boot
date dialog). Pick one peer at a time on the SIU UART.

## CLI

```text
--pcconnect-bridge <S>   Open a host chardev and pipe the SIU UART through it.
                         S = tcp:HOST:PORT             (emulator dials out)
                           | tcp-listen:PORT[@ADDR]    (emulator listens)
                           | unix:/PATH                (emulator dials out)
                           | unix-listen:/PATH         (emulator listens)
                           | pty:auto                  (allocate a host PTY;
                                                        slave path printed)
--pcconnect-tee <P>      Mirror both directions of the byte stream to file P
                         (annotated text). Requires --pcconnect-bridge.
```

Set `BE300_PCC_TRACE=1` to enable bridge state-change logging on stderr.

## Tee log format

```text
# pcconnect-bridge tee log; mono_ns dir count: hex bytes
<mono_ns> H>G <count>: <space-separated lowercase hex>      ← host → guest
<mono_ns> G>H <count>: <space-separated lowercase hex>      ← guest → host
<mono_ns> H>G:drop <count>: …                                ← bytes received
                                                              before cable
                                                              detect; dropped
                                                              to mimic an
                                                              unpowered dock
```

Times are `clock_gettime(CLOCK_MONOTONIC)` nanoseconds. The file is
line-buffered, so it can be `tail -f`'d while a session is in progress.
Compare against `pcconnect.log` / `pcconnect_2.log` (real PortMon
captures) to align frame structure.

## Cable gating and dock detect

The bridge rides the same VRC4173 dock-detect plumbing as the
synthesizer: when the GIU "cradle inserted" edge fires (after a Boot.exe
reset; see `src/be300_devices.c:be300_pcconnect_raise_dock_edge`),
`pcconnect_set_cable_connected(true)` is called, the bridge logs
`[PC_CONNECT_BRIDGE] cable connected`, and bytes start flowing in both
directions. Bytes received from the host before that edge are dropped
(and tee'd as `H>G:drop`).

A peer-side disconnect closes the local fd but does **not** drop the
cable. PTY mode re-opens its master so a fresh `screen` session can
attach. TCP/Unix listen modes re-`accept()` in the next tick. TCP/Unix
connect modes re-dial every 500 ms until the peer is back.

## Host-side recipes

### PTY (debugging without UTM)

```bash
./build-host/be300 --nand ce/restore_images/All_nand_300.bin \
    --pcconnect-bridge pty:auto \
    --pcconnect-tee /tmp/pcc.tee
```

Watch stderr for:

```text
[PC_CONNECT_BRIDGE] pty slave=/dev/ttysNNN
[PC_CONNECT_BRIDGE]   attach with: screen /dev/ttysNNN 115200,cs8
```

Then in another terminal:

```bash
screen /dev/ttysNNN 115200,cs8
```

Detach with `Ctrl-A d`. Note: UTM does not expose PTY chardev directly
in its GUI. PTY mode is for `screen`-based loopback debugging only.

### TCP — emulator dials out (UTM listens)

This is the typical UTM path. Configure UTM's serial as a TCP server
*before* starting the emulator (or use `unix:/path`, which is bidirectional).

```bash
./build-host/be300 --nand ce/restore_images/All_nand_300.bin \
    --pcconnect-bridge tcp:127.0.0.1:5555 \
    --pcconnect-tee /tmp/pcc.tee
```

Loopback test with `nc`:

```bash
# In one terminal: nc listens on 5555 with a long-lived stdin
mkfifo /tmp/nc.in
( tail -f /tmp/nc.in ) | nc -l 127.0.0.1 5555

# In another: send bytes
echo "host writes" > /tmp/nc.in
```

Bytes appear in the tee log as `H>G`.

### TCP — emulator listens (UTM dials in)

```bash
./build-host/be300 --nand ce/restore_images/All_nand_300.bin \
    --pcconnect-bridge tcp-listen:5555 \
    --pcconnect-tee /tmp/pcc.tee
```

Default bind address is `127.0.0.1`. Add `@0.0.0.0` to listen on all
interfaces (e.g. `tcp-listen:5555@0.0.0.0`).

### Unix socket — emulator listens

```bash
./build-host/be300 --nand ce/restore_images/All_nand_300.bin \
    --pcconnect-bridge unix-listen:/tmp/pcc.sock \
    --pcconnect-tee /tmp/pcc.tee

# from anywhere:
echo "via unix" | nc -U -w 2 /tmp/pcc.sock
```

`unix:/path` (without `-listen`) makes the emulator the client; bring up
the listener first.

## UTM Windows VM setup

The integration is a stock UTM serial-device configuration; no UTM
patches required. Reference: `Configuration/UTMQemuConfiguration+Arguments.swift:347-385` (serial argument synthesis), `Configuration/UTMQemuConfigurationSerial.swift` (schema).

1. Boot a Windows 2000 / XP UTM VM.
2. UTM → VM → Edit → Devices → New → **Serial**.
   - **Mode:** TCP Server
   - **Address:** `127.0.0.1`
   - **Port:** `5555` (must match the emulator's `--pcconnect-bridge`)
   - **Hardware:** `usb-serial`  (Windows enumerates a USB→Serial COM port)
   - **Target:** Manual
3. Start the VM. In Windows, install the Casio USB→Serial INF if
   Device Manager flags an unknown USB device. Confirm a new `COM3` (or
   similar) appears.
4. Start the emulator with the matching bridge spec:
   ```bash
   ./build-host/be300 --nand ce/restore_images/All_nand_300.bin \
       --pcconnect-bridge tcp:127.0.0.1:5555 \
       --pcconnect-tee /tmp/pcc.tee
   ```
5. In the Windows VM, install Casio sync (`Setup.exe` from
   `ce/restore_images/`) if not already present, launch `PCConnect.exe`,
   and point it at the new COM port.

When `PCConnect.exe` clicks **Connect**, the tee should show:

- host AT/`0x55`/`0x20` wake handshake → guest `0x55` echoes → guest
  `0x20` ready
- a `0x10 …` framed PCConnect command from host
- a matching `0x11 …` ACK from guest

For protocol RE, `tail -f /tmp/pcc.tee` and exercise PCConnect features.
Cross-reference against `docs/PC_CONNECT_TIME_SYNC.md` (which decodes
the wake / `WilSetSystemTime` frames) and the original `pcconnect.log`
captures.

`tools/pcconnect_diff_tee.py` aligns a tee log against a real-hardware
PortMon log (`pcconnect.log` / `pcconnect_2.log`) byte-by-byte and prints
a side-by-side hex tape with the first divergence flagged. Use
`--align-on-g2h` when the emulator capture has a long pre-launch
host-only tail (e.g. before AtPcCnct opened the UART), so both streams
re-base at the first guest-emitted byte.

## Known protocol stall

The bridge transport is correct (host bytes reach the guest UART RX
queue verbatim, guest TX bytes reach the peer fd in order). The active
blocker is on the **device side**: AtPcCnct.exe never emits a multi-byte
framed response. Every G->H event in the tee is a single byte (`0x55`
sync or `0x20` ready). Real-hardware PortMon logs show AtPcCnct emitting
`55 11 .. 03 ?? ..` 34-byte framed responses within ~100µs of receiving
a `10 .. 03 ?? ..` host frame. In the emulator, those framed responses
never appear — neither for the first 0x2d query nor for any subsequent
opcode (0x26, 0x28, 0x46, ...).

Investigations so far have ruled out the cheaper hypotheses:

- NS16550 MSR holds DCD+DSR+CTS asserted for the vrc4173siu UART
  (`gxemul/src/devices/dev_ns16550.c:104`). MSR-change deltas are
  intentionally suppressed because the BE-300 routes modem transitions
  through the AA008004 latch instead of NS16550 IIR_MLSC.
- `LSR_OE` (overrun) is never asserted by the NS16550 driver.
- The bridge's per-byte rx-ready callback fires `BE300_COMMMODE_PENDING_MASK`
  + `BE300_COMMMODE_MODEM_EVENT_BITS` on every byte released to rx_ring
  (matches Stowaway, which works).

Remaining work is RE of XIP slot 59 (`AtPcCnct.exe`) and `COShellApi.dll`
to find the wake-to-framed-mode predicate that the emulator state isn't
satisfying. AtPcCnct.exe itself is a 5 KB shim with only an import on
`COShellApi.dll!CoshExecute`, so the actual frame parser lives in
COShellApi.dll or another module spawned by coshell. Auto-launching
AtPcCnct via `HKLM\init\Launch<NN>` does not solve the stall: the
kernel-level launch spawns the shim, but `CoshExecute` without the
correct shell-agent context apparently no-ops -- AtPcCnct never opens
COM1.

## Implementation references

- `src/pcconnect_bridge.c` — bridge state, openers, drain logic, tee
- `src/pcconnect_bridge.h` — public API
- `src/pcconnect.c` — backend dispatch shims at the four `pcconnect_uart_*`
  entry points called from `gxemul/src/devices/dev_ns16550.c`
- `src/pcconnect_ring.h` — shared 4096-byte ring buffer
- `src/be300_devices.c` — `be300_pcconnect_cable_enabled()` and the
  interrupt-connect block at the VRC4173 latch init treat
  `cfg.pcconnect_bridge` like `cfg.enable_pcconnect_time_sync`
- `src/machine_be300.c` — `pcconnect_bridge_configure` at machine
  create, `pcconnect_bridge_tick` driven from `be300_pcconnect_poll`,
  `pcconnect_bridge_shutdown` at machine destroy
- `src/main.c` — CLI parsing, mutual-exclusion checks
- `tools/pcconnect_diff_tee.py` — read-only diff between a bridge tee
  and a PortMon TSV log; emits side-by-side hex with first divergence
  highlighted
