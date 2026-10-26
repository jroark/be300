# WinCE 3.0 display-control evidence

The service manual distinguishes panel contrast from backlight brightness
(printed pp. 18, 50, 53, 55–56). Analysis of the unmodified `300.bin` image
identifies their guest programming paths. These findings establish software
transactions, not a measured analog panel response. No guest bytes or MMIO
semantics were changed for this investigation.

## Reproducing the analysis

Keep all images and extracted binaries outside Git. With `NAND` pointing to
the local WinCE 3.0 image and `OUT` to an external working directory:

```sh
python3 tools/extract_nk_lzss.py --image "$NAND" --out-dir "$OUT"
python3 tools/extract_xip_modules.py --nk "$OUT/nk_flat.bin" --out "$OUT/modules"
mipsel-linux-gnu-objdump -D -b binary -m mips:4000 -EL \
  --adjust-vma=0x01a50000 "$OUT/modules/62_ddi.dll.bin" > "$OUT/ddi.dis"
```

The extracted modules' preferred bases are `0x01A50000` (`ddi.dll`),
`0x01A30000` (`keybddr.dll`) and `0x01A20000` (`eeprom.dll`). Code addresses
below use those bases; binary offsets equal address minus base. Module SHA256:

| Extracted module | SHA256 |
|---|---|
| `62_ddi.dll.bin` | `1b29276810cfd6322fc8592e5498517708ea96e9827382fc9458e4aae45458f7` |
| `64_keybddr.dll.bin` | `cca166cbb3f1ac044d6b5471c3989699c661724b41f8fe8c4d11bb2ae88df17e` |
| `65_eeprom.dll.bin` | `dc5d253ada7daa4d1f67bc18a66e5e2a207c40d57618ebe298ca7b3588cbdb98` |

## Contrast and backlight

`ddi.dll` at `0x01A581B4` maps the companion `0xAA000000` window and
the `0xA0002000` window into user space. The pointers are stored at
`0x01A63768` and `0x01A6376C`, respectively. This lets register accesses
through those pointers be resolved without guessing from their names.

| Routine / data | Observed behavior |
|---|---|
| `ddi.dll:0x01A56A5C` | Contrast setter accepts indices 0–9, defaults an out-of-range index to 5, and selects a word from the runtime table at `0x01A63740`. |
| `ddi.dll:0x01A56A98–0x01A56B68` | Sends eight bits, most significant first, through companion offset `0x0C4C`: bit 2 carries data, bit 1 is pulsed for each bit, and bit 0 is pulsed at the end. Physical register: `0x0A000C4C`. Upper bits are preserved. |
| `ddi.dll:0x01A56BB0` / `0x01A56BD0` | Battery / external-power backlight setters select words from `0x01A52070` / `0x01A52050` and write companion offset `0x098C` (`0x0A00098C`). Both extracted tables contain `[3339, 3339, 2553, 1964, 981, 0]`. These are register values, not measured brightness percentages. |
| `ddi.dll:0x01A56BF0` | Reads the 16-bit word at mapped `0xA0002000 + 0x504`, i.e. physical `0x2504`. Subsequent selection logic examines bits 0 and 1. Their complete electrical meanings need hardware confirmation. |
| `ddi.dll:0x01A572F0` | Reads `Drivers\CASIO\BackLight` registry settings, including `Contrast`, `TimeoutBattery`, `TimeoutExPower`, `BrightBattery` and `BrightExPower`. |
| `ddi.dll:0x01A58068–0x01A580F0` | Resolves `E2pRead` in `eeprom.dll`, requests selector `0x44`, offset 0, length 2, and uses a successful first byte in the range `0xAB–0xD6` to adjust the contrast table relative to its center entry. |

The keyboard and display drivers communicate through named events:
`OEMBacklightContrastIncEvent`, `OEMBacklightContrastDecEvent`, and separate
`OEMBacklightBright{Inc,Dec}{EXPower,Battery}Event` events. For example,
`keybddr.dll:0x01A3622C` and `0x01A36290` signal contrast changes, and
`0x01A362F4–0x01A3647C` contains the four brightness event helpers.
This confirms that host input should deliver physical button combinations
to the guest rather than adjusting the screen directly.

The recovered SPL/OAL initialization and ddi backlight power routines also
identify `0x0980/0x0984/0x0988` as backlight control/configuration/period.
The old BLG-to-buzzer heuristic has been removed: these writes no longer
produce invented audio. The service manual's separate buzzer needs a verified
programming model. Digital panel/backlight power and serial contrast tracking
are implemented; see `BE300_LCD.md`. This does not establish every function
of the larger BLG block or an optical brightness/contrast curve.

## EEPROM bus

`eeprom.dll` exports `E2pRead` at `0x01A210FC` and `E2pWrite` at
`0x01A21058`. The read transport at `0x01A212F4` initializes companion words
`0xAA001124`, `0xAA00A140` and `0xAA00A194`, then performs a random read:
START, device write byte `0xA0`, address byte, repeated START, device read
byte `0xA1`, data bytes and STOP. The address byte is the low eight bits of
twice the transport address argument.

The low-level helpers expose the actual bus connections:

| Driver helper | Bus operation |
|---|---|
| `0x01A21AA4` | Writes the requested clock level to `0xAA00A19C`. |
| `0x01A21AB0` | Writes the requested data level to `0xAA00A1A0`. |
| `0x01A21ABC` | Reads the data input from `0xAA00A1A4`. |
| `0x01A21510` / `0x01A216FC` | Generate START / STOP transitions. |
| `0x01A21654` / `0x01A21784` | Send / receive a byte, including the ninth acknowledge cycle. |

This supports an I²C-style EEPROM model without needing to discover the
register addresses on a physical unit. It does not establish the exact IC3
part, total capacity, page-write behavior or per-unit factory contents.

## Next validation

1. Capture contrast index, the eight-bit serial payload and backlight word
   while exercising Power+arrow combinations in stock WinCE 3.0. Include
   minimum, middle and maximum settings and AC insertion/removal.
2. On hardware, compare the same digital transactions and panel response;
   confirm the bit meanings at physical `0x2504`. Keep the existing RGB565
   guest framebuffer unchanged.
3. Identify IC3 and obtain a read-only EEPROM capture before adding persistent
   storage. Establish how calibration and UUID are laid out; exclude UUID
   writes and factory initialization from automated captures.

The register paths above can now guide focused tracing. A faithful display
response and EEPROM contents still require those measurements; neither
should be synthesized from an idle register dump.
