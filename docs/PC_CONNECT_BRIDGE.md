# PC Connect Bridge — Wire Dock Sync Bytes To A Host Chardev

`--pcconnect-bridge` connects the BE-300 dock cable to a host-side chardev
(TCP socket, Unix socket, or PTY) so real `PCConnect.exe` in a UTM Windows
VM can sync with the live WinCE 3.0 guest. What travels over the chardev
depends on `--pcconnect-dock`:

| Dock mode | Guest side | Host chardev carries | UTM/QEMU device |
|---|---|---|---|
| `rs232` (default) | VRC4173 SIU at `0xaa008680`, serial.dll | raw serial bytes | `isa-serial` (a COM port) |
| `usb-sync` | USB function controller, usb.dll (`docs/BE300_USB_FUNCTION.md`) | usbredir protocol | stock `usb-redir` |

In USB mode the BE-300 itself is the USB 1.1 full-speed device
(`USB\VID_07CF&PID_2002`, vendor class, bulk EP1 IN / EP2 OUT, interrupt EP3
IN), exactly as on real hardware. `src/usbredir_dev.c` presents the
controller model (`src/hw/usbf.c`) to QEMU's `usb-redir`; descriptors,
control-request answers and bulk data all come from the unmodified guest
`usb.dll`. No UTM/QEMU rebuild is needed. The earlier `usb-be300-dock` QEMU
patch (PID `2001`, raw bytes) did not match the hardware and is retired.

## CLI

```text
--pcconnect-bridge <S>   Open a host chardev and pipe the SIU UART through it.
                         S = tcp:HOST:PORT             (emulator dials out)
                           | tcp-listen:PORT[@ADDR]    (emulator listens)
                           | unix:/PATH                (emulator dials out)
                           | unix-listen:/PATH         (emulator listens)
                           | pty:auto                  (allocate a host PTY;
                                                        slave path printed)
--pcconnect-tee <P>      Mirror both directions to file P (annotated bytes in
                         rs232 mode, decoded usbredir packets in usb-sync).
                         Requires --pcconnect-bridge.
--pcconnect-baud <N>     Pace guest-to-host bytes to N baud (8N1).
                         Default is 115200; 0 disables pacing. rs232 only.
--pcconnect-dock <D>     Guest-visible dock socket identity.
                         D = rs232 (default) | usb-sync
```

Set `BE300_PCC_TRACE=1` to enable bridge state-change logging on stderr, and
`BE300_USBREDIR_TRACE=1` for usbredir connection logging.

## Dock mode

`--pcconnect-dock rs232` preserves the original bridge behavior: after the
CommMode dock edge, socket.dll sees raw socket value `0x0008` and loads the
RS-232 serial path.

`--pcconnect-dock usb-sync` reports raw socket value `0x000c`, which selects
the WinCE USB dock entry. `usb-vcom` is accepted as a compatibility alias, but
new commands should use `usb-sync`.

With socket `0x000c` the guest loads `usb.dll` (XIP module at VA
`0x01920000`), which drives the BE-300 USB function controller (companion
`0x1100`, `0x1300`-`0x161F`) and never touches the companion SIU. The
emulator offers the device to the usbredir peer once the dock edge has fired
and `usb.dll` has written its descriptors and enabled the controller; a guest
reset (KjCMU, e.g. after an OS update) disconnects it and re-init offers it
again, so Windows re-enumerates as it would on real hardware.

`usb.dll` is also what raises `NOTIFICATION_EVENT_RS232_DETECTED` (event 9),
the notification that auto-starts the device-side sync app. It does so only on
a DCD rising edge reported by its own modem-status source while the port is
closed (`usb.dll` code near VA `0x01921b50`). `serial.dll`, used by the RS-232
socket `0x0008`, has no such call, so in RS-232 mode the device-side PC
Connect app has to be started on the device. In USB mode the PC's
`SET_CONTROL_LINE_STATE` (DTR) reaches `usb.dll` through the controller and
the app starts by itself.

## Tee log format

```text
# pcconnect-bridge tee log; mono_ns dir count: hex bytes
<mono_ns> H>G <count>: <space-separated lowercase hex>      ← host → guest
<mono_ns> G>H <count>: <space-separated lowercase hex>      ← guest → host
<mono_ns> H>G:queued <count>: …                              ← host bytes retained
                                                              before UART ready
<mono_ns> H>G:drop <count>: …                                ← host bytes while
                                                              the emulated cable
                                                              is disconnected
<mono_ns> G>H:drop <count>: …                                ← guest bytes while
                                                              the emulated cable
                                                              or host endpoint is
                                                              unavailable
```

Times are `clock_gettime(CLOCK_MONOTONIC)` nanoseconds. The file is
line-buffered, so it can be `tail -f`'d while a session is in progress.
Compare against `pcconnect.log` / `pcconnect_2.log` (real PortMon
captures) to align frame structure.

## Cable gating and dock detect

The bridge rides the VRC4173 dock-detect plumbing: when the GIU "cradle
inserted" edge fires (after a Boot.exe reset; see
`src/be300_devices.c:be300_pcconnect_raise_dock_edge`),
`pcconnect_set_cable_connected(true)` is called, the bridge logs
`[PC_CONNECT_BRIDGE] cable connected`, and guest-to-host bytes can flow.
Host-to-guest bytes received before the dock edge are consumed and tee'd as
`H>G:drop`; real disconnected serial pins do not buffer the PC's polling
stream. After the dock edge, bytes received before the guest UART is configured
are retained only as a small UART-sized tail, tee'd as `H>G:queued`. This gives
the guest serial path a real polling edge without replaying an unbounded backlog
that a real finite UART FIFO would have lost or serial.dll would have flushed
during COM open.

After insertion, the bridge keeps the physical cable state connected across
BE-300 CPU resets.  The guest-visible CommMode socket edge may be replayed
after reset so socket.dll sees a fresh insertion, but the host-side serial
polling stream is treated as a still-connected dock until emulator shutdown or
an explicit future physical-disconnect model.

The companion `AA001B50` status bit used during socket setup is exposed as a
one-shot insert edge, matching the CF model's treatment of the same byte. It is
not held as a storage-card present level after socket.dll has consumed the dock
transition.

Guest-to-host bytes are paced at the configured baud through a 256 KB queue.
The emulated UART keeps reporting THRE as always ready: `serial.dll` sends one
byte per TX interrupt on the companion SIU, and those interrupts reach it
through the edge-triggered, write-1-to-clear CommMode latch. Gating THRE on the
bridge backlog (tried 2026-10-12) lost TX-ready edges whenever the OAL
acknowledged the latch. Replies then stalled for ~100 ms per byte, and the PC
rejected the slow frames. The queue holds several maximum-size (64 KB)
PCConnect frames. If it ever fills, new bytes are dropped and tee'd as
`G>H:overflow`; queued bytes are never overwritten.

Once the selected dock driver opens the companion UART, the bridge releases
host-to-guest bytes through a finite guest-visible UART FIFO. Normal startup
and poll traffic is
released as fast as the guest UART FIFO can accept it, preserving the handshake
behavior seen during successful syncs. Large TCP batches from UTM are retained
in the bridge backlog and then released at the configured serial cadence, so
restore-time RAPI file-write payloads do not arrive at effectively infinite
baud. If that backlog fills, the emulator stops reading the host fd so TCP
applies backpressure instead of dropping or replaying sync payload bytes.

A guest receive-FIFO reset (FCR1, VR4131 UM §20.2.7) clears only the 16-byte
guest-visible FIFO; host bytes still in the bridge backlog are on the wire and
arrive afterwards. Reprogramming the divisor (DLAB set, then cleared) pauses
delivery without discarding anything. The 64-byte pre-open cap applies only
until the guest first configures the UART after a dock edge or guest reset.

The physical cable state is separate from the guest-visible serial data path.
On a BE-300 CPU reset, the bridge clears transient UART FIFOs and marks the
guest data path not-yet-inserted while leaving the physical cable connected.
Host polling bytes received before the replayed CommMode socket edge are
consumed as `H>G:drop`; bytes between the edge and UART configuration are capped
to the latest UART-sized tail, matching a real serial device's finite/reset
FIFO rather than preserving an unbounded reset-time backlog.

A peer-side disconnect closes the local fd but does **not** drop the
cable.  Transient serial FIFOs are cleared so bytes sent while the PC-side
COM handle is closed are not replayed to the next open. PTY mode re-opens its
master so a fresh `screen` session can attach. TCP/Unix listen modes
re-`accept()` in the next tick. TCP/Unix connect modes re-dial every 500 ms
until the peer is back.

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
    --pcconnect-dock rs232 \
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

Both setups use stock UTM and UTM → VM → Edit → QEMU → Arguments (or the
equivalent device settings). Verified with a Windows 2000 VM (`-machine pc`,
UTM System → USB 2.0); the generated command line is in UTM's log.

### USB (`--pcconnect-dock usb-sync`)

1. VM → Edit → Input → USB Support: **USB 2.0**. UTM then creates
   `ich9-usb-ehci1` (`id=usb-controller-0`) with UHCI companions; the
   full-speed BE-300 is routed to a UHCI companion, which Win2K drives with
   its in-box driver.
2. Add QEMU arguments:
   ```text
   -chardev socket,id=be300usb,host=127.0.0.1,port=5556,server=on,wait=off
   -device usb-redir,chardev=be300usb,id=be300usbdev,bus=usb-controller-0.0
   ```
3. Start the VM, then the emulator (it redials every 500 ms, so the order
   does not matter):
   ```bash
   ./build-host/be300 --nand nand.bin \
       --pcconnect-dock usb-sync \
       --pcconnect-bridge tcp:127.0.0.1:5556 \
       --pcconnect-tee /tmp/usb.tee
   ```
4. After the BE-300 boots, stderr shows
   `[USB_REDIR] device 07cf:2002 offered to host`. Windows enumerates
   `USB\VID_07CF&PID_2002`; install the Casio USB sync driver and
   `Setup.exe` from `pccon.7z` the first time. PC Connect then starts on
   both sides without touching the device.

Only one client can hold QEMU's chardev at a time: a second emulator
pointed at the same port connects only after the first exits.

### RS-232 (`--pcconnect-dock rs232`)

Give the VM a COM port on a TCP chardev — a UTM Serial device in TCP Server
mode, or:

```text
-chardev socket,id=term0,host=127.0.0.1,port=5555,server=on,wait=off
-device isa-serial,chardev=term0
```

```bash
./build-host/be300 --nand nand.bin \
    --pcconnect-bridge tcp:127.0.0.1:5555 \
    --pcconnect-tee /tmp/pcc.tee
```

Point PCConnect at that COM port and start PC Connect on the device (RS-232
does not auto-launch it, see above).

### Protocol check

With the BE-300 configured to connect automatically, docking should show:

- host AT/`0x55` wake polling
- a guest `0x55` wake train
- the first host framed query, typically
  `10 01 00 0a 03 2d 00 00 00 4b`
- the matching guest ACK, typically
  `11 01 00 12 03 2d 00 00 00 54 ...`

The same frames appear in both modes; in the usbredir tee they are the data
of `type=101` (bulk) packets on endpoints `02` (PC → BE-300) and `81`
(BE-300 → PC). For protocol RE, `tail -f` the tee and exercise PCConnect
features.
Cross-reference against the original `pcconnect.log` captures.

`tools/pcconnect_diff_tee.py` aligns a tee log against a real-hardware
PortMon log (`pcconnect.log` / `pcconnect_2.log`) byte-by-byte and prints
a side-by-side hex tape with the first divergence flagged. Use
`--align-on-g2h` when the emulator capture has a long pre-launch
host-only tail (e.g. before AtPcCnct opened the UART), so both streams
re-base at the first guest-emitted byte.

## Known timing gap

The bridge can complete real PCConnect syncs through a Win2K VM, but the
initial `0x55` wake train is still slower than the real-hardware PortMon
capture. Host-to-guest pacing is intentionally adaptive: small handshake
frames remain FIFO-driven, while large transfer bursts are paced. Treat
byte-for-byte timing comparisons as diagnostic evidence, not as a current
accuracy guarantee.

## Implementation references

- `src/pcconnect_bridge.c` — RS-232 bridge state, drain logic, tee
- `src/host_chardev.c` — shared tcp/unix/pty openers, accept and redial
- `src/usbredir_dev.c` — usbredir device side for `usb-sync`
  (`tests/test_usbredir_dev.c`)
- `src/hw/usbf.c` — USB function controller model (`tests/test_usbf.c`)
- `src/pcconnect_bridge.h` — public API
- `src/pcconnect.c` — backend dispatch shims at the four `pcconnect_uart_*`
  entry points called from `gxemul/src/devices/dev_ns16550.c`
- `src/pcconnect_ring.h` — shared 4096-byte ring buffer
- `tests/test_pcconnect_bridge.c` — bulk TX/RX transport tests (`ctest`)
- `src/be300_devices.c` — dock socket selection and CommMode IRQ plumbing
- `src/machine_be300.c` — `pcconnect_bridge_configure` at machine
  create, `pcconnect_bridge_tick` driven from `be300_pcconnect_poll`,
  `pcconnect_bridge_shutdown` at machine destroy
- `src/main.c` — CLI parsing, mutual-exclusion checks
- `tools/pcconnect_diff_tee.py` — read-only diff between a bridge tee
  and a PortMon TSV log; emits side-by-side hex with first divergence
  highlighted
