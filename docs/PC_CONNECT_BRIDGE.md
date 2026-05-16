# PC Connect Bridge — Wire Dock Sync Bytes To A Host Chardev

`--pcconnect-bridge` pipes the emulated VRC4173 SIU UART (the BE-300 dock
serial port at PA `0xaa008680`) to a host-side chardev — TCP socket, Unix
socket, or PTY. Pair it with a UTM Windows VM whose PC-facing USB dock
device sits on the other end of that chardev, and real `PCConnect.exe`
running in the VM can talk to the live WinCE 3.0 guest in the emulator.

## Why this is still a bridge

The BE-300 uses the same dock connector for the RS-232 cable and the USB
dock. On the BE-300 side, the byte stream still reaches the VRC4173 SIU
and AtPcCnct. On the PC side, though, the USB dock is not an FTDI-style
USB serial adapter: Windows binds it through `wceusbsh.inf` /
`wceusbsh.sys` as a Windows CE USB sync device. The Casio driver accepts
`USB\VID_07CF&PID_2001`, `USB\VID_07CF&PID_2002`, and
`USB\VID_07CF&PID_2003`; the UTM model presents `07CF:2001`
(`CASIO USB Sync 2001`).

Neither the VR4131 SoC nor the VRC4173 companion chip has a USB function
(gadget) controller, so the emulator does not add a PDA-internal USB
controller. The UTM side models the external dock's PC-facing USB device
and backs its raw bulk endpoints with the same host chardev used by the
BE-300 UART bridge.

Do not use QEMU's stock `usb-serial` device for USB sync. It presents
FTDI IDs (`0403:6001`) and implements FTDI control requests plus 2-byte
FTDI status headers on bulk IN. The patched UTM/QEMU device for this
project is `usb-be300-dock`, which presents the Casio VID/PID and passes
raw bytes over bulk endpoints.

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
--pcconnect-baud <N>     Pace guest-to-host bytes to N baud (8N1).
                         Default is 115200; 0 disables pacing.
--pcconnect-dock <D>     Guest-visible dock socket identity.
                         D = rs232 (default) | usb-sync
```

Set `BE300_PCC_TRACE=1` to enable bridge state-change logging on stderr.

## Dock mode

`--pcconnect-dock rs232` preserves the original bridge behavior: after the
CommMode dock edge, socket.dll sees raw socket value `0x0008` and loads the
RS-232 serial path.

`--pcconnect-dock usb-sync` keeps the same BE-300-side byte transport but
reports raw socket value `0x000c`, which selects the WinCE USB dock entry.
Use this with NAND images configured to connect over USB, for example
`./nand.bin`. `usb-vcom` is accepted as a compatibility alias, but new
commands should use `usb-sync`.

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

Once the selected dock driver opens the companion UART, the bridge releases
host-to-guest bytes through a finite guest-visible UART FIFO. Normal startup
and poll traffic is
released as fast as the guest UART FIFO can accept it, preserving the handshake
behavior seen during successful syncs. Large TCP batches from UTM are retained
in the bridge backlog and then released at the configured serial cadence, so
restore-time RAPI file-write payloads do not arrive at effectively infinite
baud. If that backlog fills, the emulator stops reading the host fd so TCP
applies backpressure instead of dropping or replaying sync payload bytes.

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

The USB-sync path requires the UTM source changes in `../UTM`:

- `patches/qemu-10.0.2-utm.patch` adds QEMU device `usb-be300-dock`.
- `Configuration/QEMUConstantGenerated.swift` exposes that device in the
  i386/x86_64 serial hardware picker.

After rebuilding UTM/QEMU with those changes, configure the VM serial
device as the backend for the Casio USB dock model. Reference:
`Configuration/UTMQemuConfiguration+Arguments.swift:347-385` (serial
argument synthesis), `Configuration/UTMQemuConfigurationSerial.swift`
(schema).

If QEMU exits with `'usb-be300-dock' is not a valid device model name`,
UTM is still running a QEMU binary/framework that was built before the
patch. Verify the active dependency bundle before launching the VM:

```bash
DYLD_LIBRARY_PATH=../UTM/sysroot-macos-arm64/lib \
  ../UTM/sysroot-macos-arm64/bin/qemu-system-i386 -device help |
  rg usb-be300-dock

strings -a \
  ../UTM/sysroot-macos-arm64/Frameworks/qemu-i386-softmmu.framework/qemu-i386-softmmu |
  rg 'usb-be300-dock|CASIO USB Sync'
```

Both commands must show the BE-300 dock strings. Updating only
`QEMUConstantGenerated.swift` changes the UTM UI/configuration layer; it
does not register the device inside QEMU.

1. Boot a Windows 2000 / XP UTM VM.
2. UTM → VM → Edit → Devices → New → **Serial**.
   - **Mode:** TCP Server
   - **Address:** `127.0.0.1`
   - **Port:** `5555` (must match the emulator's `--pcconnect-bridge`)
   - **Hardware:** `CASIO BE-300 USB Sync Dock (usb-be300-dock)`
   - **Target:** Manual
3. Start the VM. In Windows 2000, install the Casio USB sync driver from
   `pccon.7z` if Device Manager asks for a driver. The device should show
   as `CASIO USB Sync 2001` with hardware ID `USB\VID_07CF&PID_2001`, not
   as HID and not as an FTDI COM port.
4. Start the emulator with the matching bridge spec:
   ```bash
   ./build-host/be300 --nand nand.bin \
       --pcconnect-bridge tcp:127.0.0.1:5555 \
       --pcconnect-dock usb-sync \
       --pcconnect-tee /tmp/pcc.tee
   ```
5. In the Windows VM, install Casio sync (`Setup.exe` from
   `pccon.7z`) if not already present and launch `PCConnect.exe`.

If the generated hardware picker has not been rebuilt yet, use UTM's
Additional QEMU Arguments instead of adding a Serial device:

```text
-chardev socket,id=be300pcc,host=127.0.0.1,port=5555,server=on,wait=off
-device usb-be300-dock,chardev=be300pcc
```

With those arguments, keep the emulator side as
`--pcconnect-bridge tcp:127.0.0.1:5555 --pcconnect-dock usb-sync`.

With the BE-300 configured to connect automatically, docking should show:

- host AT/`0x55` wake polling
- a guest `0x55` wake train
- the first host framed query, typically
  `10 01 00 0a 03 2d 00 00 00 4b`
- the matching guest ACK, typically
  `11 01 00 12 03 2d 00 00 00 54 ...`

For protocol RE, `tail -f /tmp/pcc.tee` and exercise PCConnect features.
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

- `src/pcconnect_bridge.c` — bridge state, openers, drain logic, tee
- `src/pcconnect_bridge.h` — public API
- `src/pcconnect.c` — backend dispatch shims at the four `pcconnect_uart_*`
  entry points called from `gxemul/src/devices/dev_ns16550.c`
- `src/pcconnect_ring.h` — shared 4096-byte ring buffer
- `src/be300_devices.c` — dock socket selection and CommMode IRQ plumbing
- `src/machine_be300.c` — `pcconnect_bridge_configure` at machine
  create, `pcconnect_bridge_tick` driven from `be300_pcconnect_poll`,
  `pcconnect_bridge_shutdown` at machine destroy
- `src/main.c` — CLI parsing, mutual-exclusion checks
- `tools/pcconnect_diff_tee.py` — read-only diff between a bridge tee
  and a PortMon TSV log; emits side-by-side hex with first divergence
  highlighted
