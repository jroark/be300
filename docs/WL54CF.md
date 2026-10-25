# AmbiCom WL54-CF

`--wl54cf` attaches a Marvell CF8385 station card in the primary PCMCIA socket.
The supported guest is **Glaze OS with its native MIPS WL54-CF driver**. The
supplied `wl54gcfppc41/wince41.cab` contains an ARM/Thumb CE 4.1 miniport;
it cannot run in the BE-300's MIPS WinCE 3.0. Windows CE support for this card
is not claimed.

The station scans/authenticates/associates with the open virtual **BE300** AP,
channel **3**, BSSID **02:42:45:33:30:30**. Its Ethernet edge uses the same GXemul
DHCP/DNS/IP gateway as XI-825 and NE2000. `--net-mac` applies to it. The card is
also selectable in desktop VM setup/settings; older VM files default
`enable_wl54cf` to false. All three network cards are mutually exclusive and
require NAND boot. A simultaneous `--cf` uses the existing secondary socket,
whose MMIO is not decoded.

## Glaze build and launch

In the matching Glaze worktree:

```sh
python3 tools/extract_wl54cf_firmware.py /path/to/wl54gcfppc41/wince41.cab
./build.sh WL54CF=1
cp out/glaze.bin out/wireless-test.bin
/path/to/be300 --nand out/wireless-test.bin --wl54cf --rtc-host-time
```

The extractor verifies the exact vendor DLL and both firmware image SHA-256
hashes before writing ignored local assets. It requires `cabextract` for CAB
input and also accepts the extracted DLL. Nothing from the vendor package is
redistributed. Ordinary Glaze builds do not require firmware; an attached card
with missing firmware fails clearly, stays interrupt-masked and is not probed
as an NE2000 or ATA device.

## Protocol evidence

The implementation uses the real Marvell CF host interface, independently
implemented from these primary sources:

- Linux v6.6 [if_cs.c](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/marvell/libertas/if_cs.c):
  CF8385 MANFID `02df:8103`, B1 product revision `0x12`, register addresses,
  mask polarity, FIFO transfer lengths, host-status/interrupt handshakes,
  helper upload, firmware block requests, scratch `0x5a`, and CF responses
  reporting eight extra bytes.
- Linux v6.6 [host.h](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/marvell/libertas/host.h),
  [cfg.c](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/marvell/libertas/cfg.c),
  [tx.c](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/marvell/libertas/tx.c) and
  [rx.c](https://github.com/torvalds/linux/blob/v6.6/drivers/net/wireless/marvell/libertas/rx.c):
  command header/sequence/result, hardware-spec response, scan beacon layout,
  open authentication, association TLVs, MAC-control bits, 24-byte TxPD,
  20-byte RxPD and received 802.3/LLC/SNAP conversion.
- Supplied `AMWL54G.dll` (CAB member `0AMWL54G.001`), SHA-256
  `e38d1aa0700da233bbca0d191d475b4faba3edd9b3f6887420cb97c5725155fc`:
  helper at file offset `0x14ac0`, length 2136; main firmware at offset
  `0x15318`, length 97128. The main image matches the published
  [Libertas CF8385 firmware](https://github.com/wkennington/linux-firmware/blob/master/libertas/cf8385.bin)
  byte for byte. Its records have little-endian command/address/length/checksum
  headers, data command 1, and a final command 4 with zero payload.
- CAB install header `AMBICO~1.000`: `Marvell-802.11_CF-10D8`,
  `Marvell-88W8300_802.11g_PC_Card-507F`, adapter `amwl54g1`, firmware 3.2.8.

The socket/window/GIRQ0 routing reuses existing board behavior and its hardware
citations. No GXemul changes are necessary.

## Model boundaries

This models the **station firmware service**, not execution of its ARM CPU or
the analog radio. Reset starts at the ROM upload handshake; helper data and
its terminating zero-length transfer precede the record-driven main upload.
Scratch readiness is published only after the terminal firmware record. The
model checks framing and bounds, not ARM instruction validity or firmware
checksums. Firmware upload tests can therefore use structural fixture records.

Supported station commands are GET_HW_SPEC, SCAN, AUTHENTICATE, ASSOCIATE,
DEAUTHENTICATE, RADIO_CONTROL and MAC_CONTROL. Unsupported commands and unsupported
MAC modes return an error. A bad association reports failure and leaves the
station disconnected. TX/RX forwarding requires association and the appropriate
MAC-control enable. FIFO ownership, pending causes and interrupt masks are
separate; an RX interrupt stays pending while queued packets remain. Queues
and transfers are bounded, and reset drops all pending command/packet state.

**TODO (2026-10-25): capture a physical WL54-CF CIS and verify its BE-300 bridge
configuration.** The current minimal CIS uses known identity strings/MANFID,
a standard 64-byte relocatable I/O configuration and COR at attribute offset
`0x200`; its complete bytes are an emulated configuration, not an EEPROM dump.
No bytes are padded to force a WinCE card-ID CRC match. The model chooses the
Libertas-supported B1 revision; it does not claim to reproduce early B0 cards.
Glaze's COR access uses the existing byte-addressed `0x0b400000` emulator alias,
which is not yet verified on real hardware. It deliberately avoids `0x0a00d200`,
which is the board NAND direct-I/O port.

WEP/WPA, ad-hoc mode, physical-network discovery/bridging, browser/Android card
selection, hot removal/reassociation and real-card certification are deferred.

## Validation

```sh
ctest --test-dir build-host -R 'wl54cf|xi825|network_cli|vm_config' --output-on-failure
```

The asset-free tests cover CIS/configuration/reset, firmware framing and failure,
scan filtering, authentication/association, unknown commands, malformed TLVs,
FIFO bounds/odd lengths, MAC control, SNAP conversion, queue backpressure,
interrupt ownership/masks/acknowledgment and backend TX. They also check CLI
socket conflicts and desktop JSON compatibility. The WL54-CF tests pass with
AddressSanitizer and UndefinedBehaviorSanitizer.

The matching Glaze worktree provides `net-driver-test` and `wl54cf-driver-test`
native targets, and `tests/run_wl54cf.py` for a disposable-NAND local HTTP smoke
test. Glaze kernel tests with WL54-CF obtained an actual DHCP lease, pinged the
gateway, resolved DNS, fetched HTTP through both socket APIs and finished with
zero failures. NE2000/no-card kernel tests and the emulator's unchanged default
WinCE 3.0 cold-boot test also passed. These results validate the two software
implementations together; they do not establish physical-card compatibility.

Validated 2026-10-25 from the two `codex/wl54cf-*` worktrees: all 14
asset-free emulator CTests, the WinCE cold-boot test and both Glaze native
driver tests passed. Glaze `ktest` passed with WL54-CF, NE2000, no card and
CF storage. The local Browser fixture rendered **WL54-CF HTTP PASS** and
retained the source NAND hash. Internet HTTP and DNS also succeeded.
