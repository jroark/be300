# Power-on state and gated units

As of 2026-10-30 the emulator starts the Casio companion as a power-on
leaves it, not as Windows CE leaves it. The evidence comes from a real BE-300
booted with its battery pulled into Glaze's CF recovery payload. Glaze's
`HWDUMP=1` kernels snapshot registers at entry and after setup. Their USB
`peek`/`poke` let each finding be toggled live on the running device (see
glaze-os `docs/NAND_BOOT_AUDIT.md`, "Power-on findings"). Power+Reset does
not reset the companion, so earlier "cold" captures still carried CE's
state.

## What is modelled

| Behaviour | Hardware evidence | Where |
| --- | --- | --- |
| Clock gates `0x1100-0x113C` start off | Entry snapshot read only `111C` on, which the ROM sets | `be300_devices.c` `cmu_regs[]` |
| Framebuffer access with `1100` or `110C` off stalls the bus for good | Recovery stub hung right after its first beep, at its green-marker write | `src/devices/gate_stall.c` |
| Companion SIU access with `1108`, `112C`, `1138` and `113C` all off stalls | Kernel hung at its first console write; setting the four gates cleared it | `src/devices/gate_stall.c` |
| `0x0008` bit 0 gates every companion line into GIU line 0; 0 at power-on | Setting it live made SYSINT1's GIU bit appear | `be300_girq0_line_live` |
| A source's line needs bits 2 **and** 3 of its word | Touch died with `0x08` and worked with `0x0C`; CE re-arms with `0x30, 0x04, 0x0C` | `be300_girq0_line_live` |
| Source words, `0x0000` and `0x0008` read 0 at power-on (`0x1C` reads `0x30`) | Entry snapshot | `core_regs[]` |
| PIU samples only if `0x3C8` = 3 when `PIUCNT` is written; 0 at power-on | Setting it on a running PIU did nothing until a restart | `hw/piu.h` `adc_on` |

A stall prints `[BUS] stall: ...` once. The CPU then stops while the window,
screenshot and shutdown keep working. Up to one dyntrans block (~120
instructions) may still run after the stalled access.

Checked: stock CE boots to its shell and opens Contacts on a scripted tap.
Glaze's NAND image and recovery payload boot and unlock by touch. The read-back
card images of the two Glaze payloads that hung on hardware stall at the same
access in the emulator: the stub's framebuffer write, and the kernel's first
SIU status read.

`--touch-script FILE` feeds timed pen events (`<ms> down|tap <x> <y>`,
`<ms> up`, emulated ms, LCD pixels) into the same entry point the window's mouse
uses, for headless tests of a guest's real touch path.

## Not modelled yet

- **Which gate clocks which unit.** Only the two stalls above are proven. The
  SIU works once all four candidates are on, but which one it needs is unknown.
  Other units (USB, audio, PIU, NAND, socket) are not gated in the model.
- **USB on a power-on boot** enumerated only after GPIO4 (low) and GPIO6
  (high) became outputs and `0x1B2C` was set, all CE's steady state. Which of
  these it needs is unknown. The USB model does not depend on them.
- **`0x0A001000` card status.** On hardware it reads 4 only after Windows CE has
  set a card up, 0 with a card after a power-on, and 8 with none. The model
  still returns its old present/absent values. Use `A008` bits 1-2 (the
  card-detect inputs) for presence.
- **VR4131 CMU** (`0F000060`) reads 0 after a power-on vs `902` under CE, and
  nothing has needed it yet.
