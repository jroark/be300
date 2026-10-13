# BE-300 USB Function Controller (companion window 0x1100 / 0x1300-0x161F)

Status: reverse-engineered from `usb.dll` and real-hardware dumps. It is
modelled in `src/hw/usbf.c` (test: `tests/test_usbf.c`), but nothing connects
it to a host yet (2026-10-12). This is the hardware behind
`--pcconnect-dock usb-sync`.

## What it is

The BE-300 is a USB **device**, not a host:

- `DriverDesignGuidlinesforBE-300.pdf` §3.3 says "Although BE does have a USB
  interface, it is a device, not a host". It is USB 1.1 full speed.
- Table 3-1 of the same guide puts USB D+/D- on dock pins 17/18. SEL code
  `1-100` is "USB connection interface", which is socket value `0x000c`.
- The real-hardware dump `docs/hardware/hw_dump_vrc4173.txt:105-116` holds the
  device's descriptors at PA `0x0A00153C`-`0x0A00161C`, one byte per 32-bit
  word.

There is no dock bridge chip and no OHCI host involved. `usb.dll` (XIP, VA
`0x01920000`) is a WinCE serial PDD (`HW_VTBL` at `0x01921058`) on top of
this controller.

Descriptors from the dump (and written by `usb.dll` init, pc
`0x019235e8`-`0x01923748`):

| Descriptor | Bytes | Meaning |
| --- | --- | --- |
| Device | `12 01 00 01 FF FF FF 10 CF 07 02 20 …` | USB 1.0, vendor class, EP0 max packet 16, VID `07CF`, PID `2002` |
| Config | `09 02 27 00 01 01 00 40 01` | 39 bytes, 1 interface, self-powered |
| Interface | `09 04 00 00 03 FF FF FF 00` | 3 endpoints, vendor class |
| EP | `07 05 81 02 40 00 00` | EP1 IN bulk, 64 bytes |
| EP | `07 05 02 02 40 00 00` | EP2 OUT bulk, 64 bytes |
| EP | `07 05 83 03 08 00 01` | EP3 IN interrupt, 8 bytes, interval 1 |

Windows binds this as `USB\VID_07CF&PID_2002` with Casio's `wceusbsh`-based
driver.

## Interrupt route

1. USB events raise companion level-1 source `AA000004` bit 7.
2. That source cascades through GIU GIRQ0.
3. The OAL table at PA `0x51200 + 7*4` points to handler `0x800aaab0`. The
   handler masks `AA00002C` bit 3 (`&= ~8`) and returns SYSINTR `0x12`.
4. `usb.dll` HWInit stores `dwIntID = 0x12` (pc `0x01921598`).

The OAL tables are filled at runtime. They were read from emulator RAM after
boot:

| Latch bit | Handler | SYSINTR |
| --- | --- | --- |
| `AA000004` bit 0 | `800a9790` | `0x15` |
| `AA000004` bit 1 | `800a91bc` | `0x10` |
| `AA000004` bit 7 | `800aaab0` | `0x12` (USB) |
| `AA000004` bit 8 | `800a8dcc` | `0x14` / `0x20` |
| `AA000004` bit 9 | `800aa5d8` | `0x11` |
| `AA000004` bits 2, 4, 6, 12, 16 | sub-tables | — |

## Register bases used by usb.dll

`usb.dll` maps the companion window with `GetVMem` (cdm.dll) and keeps these
pointers:

| Global | Window | Use |
| --- | --- | --- |
| `G7248` | `+0x1100` | clock/power, `0x1114` |
| `G7254` | `+0x1300` | DMA / top-level |
| `G7244` | `+0x1440` | USB core |
| `G7240` | `+0x14C4` | FIFO ports |
| `G724C` | `+0x1500` | descriptor RAM |

It also `VirtualCopy`s PA `0x8000`-`0xBFFF` (`G7258`). That region holds two
128-slot × 64-byte DMA rings:

- TX (EP1 IN) at PA `0x8000`
- RX (EP2 OUT) at PA `0xA000` (`G7250` = `+0x2000`)

## Register map

Each register below is one 32-bit word with the value in the low bits.
Behaviour is cited by the `usb.dll` PC that relies on it.

### 0x1300 block — DMA and top level

| Offset | Access | Meaning | Evidence |
| --- | --- | --- | --- |
| `0x1300` | W | Unit enable: 1 = on, 0 = off | init `0x019233e0`, OAL `0x800a5cf8` |
| `0x1308` | W | DMA/channel enable | `0x019233bc`, `0x019237d0` |
| `0x130C` | W / R | DMA start (write 1); reads non-zero while busy. Write `0x10` = abort (PowerOff). | `0x01922bdc`, `0x01922fbc`, `0x01922338` |
| `0x1310` | RW | DMA direction, bit 0: 1 = memory→EP1 (TX), 0 = EP2→memory (RX) | `0x01922b88`, `0x01922f70` |
| `0x1318` | W | DMA high address / mode (written 0) | `0x01922b90` |
| `0x131C` | W | DMA length in bytes (≤ 64) | `0x01922bb0`, `0x01922f7c` |
| `0x1320` | W | DMA address. Written as kseg1 `0xA000xxxx`; the dump reads back the PA (`0x8040`, `0x9F80`). | `0x01922bd4` |
| `0x1324` | R | Top-level status. Bit 0: RX DMA done. Bit 1: TX DMA done. Bit 2: EP2 OUT packet ready (level). Bit 3: EP1 DMA request (level, while `0x1488` bit 7 is set and EP1 has room). Bit 4: USB core interrupt pending (level; OR of the three core status/mask pairs). | GetIntrType `0x01921d00`; the dump has bit 3 clear with it enabled (`0x1324=0x01`, `0x132C=0x1E`) |
| `0x1328` | W | Status clear (write 1 to clear): bit 0 = RX done, bit 1 = TX done | `0x019232ec`, `0x01922a7c` |
| `0x132C` | RW | Top-level interrupt enable for the `0x1324` bits. Init: `0x1A` (board variant 2) or `0x1F`. | `0x019266d4` |
| `0x1338` | W | Reset pulse (write 1, then 0) | `0x01923484` |

`AA000004` bit 7 should be asserted while `(0x1324 & 0x132C) != 0`. This is
inferred from GetIntrType reading only that pair before it dispatches.

### 0x1440 block — USB core

| Offset | Access | Meaning | Evidence |
| --- | --- | --- | --- |
| `0x1444` | RW | Core enable. Init writes 1 and polls until it reads non-zero, then writes 0 at the end of init. | `0x019235a8`-`0x019235cc`, `0x019237f4` |
| `0x1448` | W | Written 2 on disable | `0x01923434` |
| `0x1450` | R | Device state. Bit 7: configured (read on a config-change event). | `0x01922548` |
| `0x1454` | R | Bit 3: EP2 OUT data pending (PIO). Bit 4: EP3 packet still pending. | `0x01923034`, `0x01923314` |
| `0x1460` | RW | EP0 control. Bit 3: STALL. | `0x0192275c` |
| `0x1464` | R | INTSTAT0. Bit 2: EP1 IN FIFO empty / TX ready. Bit 7: line event. | GetIntrType |
| `0x1468` | R | INTSTAT1. Bit 0: class/vendor SETUP received. Bit 1: modem event. Bit 4: SETUP overwritten. Bit 5: EP0 status stage ready. | `0x019225c0` |
| `0x146C` | R | INTSTAT2. Bit 1: event, clear only (bus reset or suspend?). Bit 2: configuration changed. Bit 3: EP0 status stage complete. | `0x019224e4`, `0x0192253c`, `0x01922844` |
| `0x1470`/`0x1474`/`0x1478` | RW | INTMASK0/1/2 (1 = masked). Init: `0x7D` / `0x38` / `0x09`, which matches the dump. | `0x019266f8` |
| `0x147C`/`0x1480`/`0x1484` | W | INTCLR0/1/2. Write `~bit` (0 = clear). The driver polls the status bit until it reads clear. | `0x0192250c`, `0x01922640` |
| `0x1488` | W | EP1 DMA request control. For board `0x7100`, `0x01926680` gives `0x85` (request on, written while TX ring entries remain) and `0` (off, written when the ring is drained). | `0x01922a60`, `0x01922b54`, `0x01922c78` |
| `0x148C` | W | FIFO-clear commands (self-clearing). Init writes `0x3F` (all). Bit 0 is written on an EP0 stall, bit 1 at the status stage, and bit 2 clears the EP1 FIFO before an empty commit that sends a ZLP. | `0x019237a8`, `0x01922764`, `0x019227e4`, `0x01922ad8` |
| `0x1490` | W | EP commit (self-clearing). Bit 0: EP0 status ACK. Bit 1: EP1 short packet ready (a full 64-byte DMA fill is sent without it); an empty commit is a ZLP. Bit 2: EP3 packet ready. Bit 4: written before EP1 DMA starts (meaning unknown). | `0x019227fc`, `0x01922a04`, `0x01923354`, `0x01922c68` |
| `0x1494` | W | Written 1, then `~1`, at init | `0x01923590` |

### 0x14C0 block — FIFO ports

| Offset | Access | Meaning | Evidence |
| --- | --- | --- | --- |
| `0x14C0` | W | Written `0x22` at init (dump shows `0x3E`) | `0x019237ec` |
| `0x14CC` | R | SETUP FIFO; each read pops one byte. Two dump reads gave `0x21`, `0x22`, the start of SET_CONTROL_LINE_STATE. | `0x01922650`, `hw_dump_vrc4173.txt:102-103` |
| `0x14D4` | R | EP2 OUT data FIFO; each read pops one byte | `0x019230a4` |
| `0x14D8` | R | EP2 OUT current packet length (non-destructive) | `0x01922f50`, `0x01923078` |
| `0x14E0` | W | EP3 data port: the DTR/RTS handlers (`0x01922070`-`0x01922150`) write the serial state, then 0, then commit with `0x1490` bit 2. The sender (`0x01923308`) skips the update while `0x1454` bit 4 (EP3 busy) is set, so at most one state per host poll (bInterval 1 ms) leaves the device; the TX service (`0x01922d5c`) sends `state \| 1` to announce pending EP1 data | `0x01923334` |

### 0x1500 block — descriptor RAM and hardware request state

| Offset | Access | Meaning | Evidence |
| --- | --- | --- | --- |
| `0x1500`, `0x1510`, `0x1518`, `0x1520`, `0x1528` | W | Init writes 1, 0, 0, 0, 0 | `0x01923754`-`0x01923784` |
| `0x1530` | R | USB address assigned by the host via SET_ADDRESS (dump: 8) | `0x01922568` |
| `0x1534` | R | Configuration value set by the host via SET_CONFIGURATION (dump: 1) | `0x01922574` |
| `0x153C`-`0x161C` | W | Device, config, interface and endpoint descriptors, one byte per word | `0x019235e8`-`0x01923748` |

The controller answers the **standard** requests itself from this RAM:
GET_DESCRIPTOR, SET_ADDRESS and SET_CONFIGURATION.
- **Evidence it does:** `usb.dll` never parses those requests, and only reads
  the resulting address/config on an INTSTAT2 bit 2 event.
- **What reaches software:** only class and vendor requests reach the SETUP
  FIFO (INTSTAT1 bit 0).

## Control requests handled by software

Handler: `0x019225c0`.

1. Read 8 bytes from `0x14CC`, writing `~0x10` to `0x1480` before each byte.
   Restart if INTSTAT1 bit 4 is set again afterwards.
2. Accept only `bmRequestType=0x21`, `bRequest=0x22` (CDC
   SET_CONTROL_LINE_STATE) with `wIndex=0` and `wLength=0`.
3. Any other request prints "Illeagal Reqeust" and is stalled:
   `0x1460 |= 8`, `0x148C = 1`.
4. Status stage:
   - wait for INTSTAT1 bit 5
   - write `0x148C = 2` and `0x1490 |= 1`
   - clear INTSTAT0/1 bit 5
   - wait for INTSTAT2 bit 3 and clear it
5. Apply `wValue`:
   - bit 0 (DTR) → modem status DSR | DCD (`0xA0`)
   - bit 1 (RTS) → CTS (`0x10`)

`HWModemIntrHandler` (`0x01921a98`) raises
`CeEventHasOccurred(NOTIFICATION_EVENT_RS232_DETECTED)` on a DCD rising edge
while the port is closed. That starts `AtPcCnct.exe`, as the design guide's
§3.2.3 footnote says: "For a USB connection this will happen when DTR … is
set in the USB class driver."

## Bulk data paths

### TX (EP1 IN)

1. `0x01922aa8` programs one ≤64-byte DMA:
   - `0x1310 = 1`
   - `0x131C = len`
   - `0x1320 = PA 0x8000 + slot*64`
   - `0x130C = 1`
2. On TX-done (`0x1324` bit 1), `0x0192298c` commits a **short** packet
   (`0x1490 |= 2`, `0x019229ec`-`0x01922a04`).
3. A full 64-byte packet is **not** committed: the controller sends a full
   FIFO by itself. The handler instead sets a ZLP-pending flag and unmasks
   INTSTAT0 bit 2 (`0x019229c0`-`0x019229e8`); on EP1-empty the ZLP is sent
   with `0x148C |= 4` and an empty commit (`0x01922ad8`). Modelling a full
   packet as commit-only loses it: the next DMA overwrites it (observed
   2026-10-12 against a real Win2K PC Connect session, a 72-byte reply
   arrived as its 8-byte tail).
4. `0x1488 = 0x85` keeps `0x1324` bit 3 (EP1 DMA request) raised while
   ring entries remain; `0x01922a70` re-enables that source after every
   TX-done. INTSTAT0 bit 2 (EP1 empty) restarts the flow after the ring
   drains.

### RX (EP2 OUT)

- **DMA mode** (`0x01922f0c`, on `0x1324` bit 2):
  - read the length from `0x14D8`
  - `0x1310 = 0`, `0x131C = len`, `0x1320 = PA 0xA000 + slot*64`, `0x130C = 1`
  - mask bit 2
  - RX-done (`0x1324` bit 0) is acknowledged via `0x1328 = 1` and bit 2 is
    re-enabled (`0x019232ec`)
- **PIO mode** (`0x01922ff4`, when DMA is busy or not used):
  - while `0x1454` bit 3 is set or `0x14D8 != 0`, read the length from
    `0x14D8`, then that many bytes from `0x14D4`

### Open / close

- `HWOpen` (`0x019223d0`) sets `0x148C |= 4` and `0x1490 |= 2`, and resets
  both rings.
- `PowerOff` (`0x01922314`) writes `0x130C = 0x10`.

## Open questions for the emulator model

- Which INTSTAT0 bit 7 / INTSTAT2 bit 1 events correspond to bus reset and
  suspend. Watch them on a real enumeration.
- `0x1490` bit 4 meaning. It is modelled as a no-op.
- The OAL pulses the block at boot (`0x800a5cf0`: clock `0x1114`, unit
  `0x1300`, reset `0x1338`, then off). In a usb-sync boot `usb.dll` then
  enables it (`0x1300=1`, `0x132C=0x1A`, `0x1308=1`) and leaves it on.
- Whether `AA00002C` bit 3 gates `AA000004` bit 7 (the ISR masks it before
  returning, so a level source must respect it to avoid an IRQ storm).
