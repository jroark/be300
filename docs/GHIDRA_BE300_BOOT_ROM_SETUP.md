# Ghidra Setup For `be300_boot_rom.bin`

This note turns the current ROM reverse-engineering workflow into a concrete
Ghidra setup procedure for the BE-300 boot ROM.

The target is the raw 16 KB masked ROM image at:

- [`docs/be300_boot_rom.bin`](/Users/jroark/src/be300-framebuffer/docs/be300_boot_rom.bin)

This ROM is mixed-mode:

- MIPS32 reset/setup code
- MIPS16 function library
- MIPS32 helper routines callable from the MIPS16 region

Do not use [`docs/be300_boot_rom.dis`](/Users/jroark/src/be300-framebuffer/docs/be300_boot_rom.dis)
as the import source. That file is a transformed ROM view, not a directly
correct disassembly. See the appendix in
[`docs/ROM_SPL_HANDOFF.md`](/Users/jroark/src/be300-framebuffer/docs/ROM_SPL_HANDOFF.md)
for how to interpret it.

## 1. Import Settings

Create a new Ghidra project and import the ROM as a raw binary.

Use these settings:

- Format: `Raw Binary`
- File: [`docs/be300_boot_rom.bin`](/Users/jroark/src/be300-framebuffer/docs/be300_boot_rom.bin)
- Language: `MIPS:LE:32:default`
- Compiler spec: default MIPS compiler spec
- Base address: `0x9FC00000`

Recommended program name:

- `be300_boot_rom_9fc00000`

Defaults to keep:

- little-endian
- 32-bit MIPS language for the initial import
- no rebasing after import
- no attempt to treat the whole image as MIPS16

## 2. First Validation Pass

Run auto-analysis once, but treat it as a first pass only. The MIPS32 region
should decode cleanly; the MIPS16 region will likely need manual help.

Before doing any deeper work, verify these MIPS32 anchor points:

### Reset vector

At `0x9FC00000`, expect:

```mips
9fc00000: nop
9fc00004: lui   k0,0x9fc0
9fc00008: ori   k0,k0,0x02f0
9fc0000c: jr    k0
```

### Main boot entry

At `0x9FC002F0`, expect the start of the main boot flow.

### Mailbox clear / handoff loop

At `0x9FC00354..0x9FC003D4`, expect:

- clear `0x2400`
- clear `0x24FC`
- call `0x9FC00C85`
- call `0x9FC00C21`
- poll `0x24FC`
- `jr t0`

### Mailbox write helper

At `0x9FC00464`, expect the helper that writes:

- next-stage target to `0x24FC`
- `0x03020101` to `0x2400`

If these anchors do not decode correctly, stop and fix the import settings
before proceeding.

## 3. ROM Layout To Mark Up

Create bookmarks or labels for the major regions:

- `0x9FC00000..0x9FC000FF`: reset and exception stubs
- `0x9FC00100..0x9FC00C1F`: MIPS32 initialization and helpers
- `0x9FC00C20..0x9FC0219B`: MIPS16 function library
- `0x9FC0219C..0x9FC0224F`: ROM metadata and function table
- `0x9FC02250..0x9FC03FFF`: mostly padding / unused space

Create these labels immediately:

- `rom_reset_vector` at `0x9FC00000`
- `rom_boot_main` at `0x9FC002F0`
- `rom_mailbox_write_helper` at `0x9FC00464`
- `rom_m16_entry_0c21` at `0x9FC00C21`
- `rom_m16_entry_0c85` at `0x9FC00C85`
- `rom_m16_table` at `0x9FC021C0`

Add a comment at `0x9FC00390..0x9FC003D4` summarizing the confirmed top-level
ROM handoff loop from
[`docs/ROM_SPL_HANDOFF.md`](/Users/jroark/src/be300-framebuffer/docs/ROM_SPL_HANDOFF.md).

## 4. Function Table Workflow

The function table at `0x9FC021C0` is one of the strongest anchors for the
MIPS16 region. Decode it manually as little-endian 32-bit words.

Minimum known entries:

```text
0x9FC021C0: 0x9FC00C21
0x9FC021C4: 0x9FC00C85
0x9FC021D0: 0x9FC00C29
0x9FC021D4: 0x9FC00C85
0x9FC021D8: 0x9FC00CFD
```

For each nonzero pointer:

1. Define the table entry as a 32-bit little-endian value.
2. Create a label for the target.
3. Convert the target into code.
4. Create a function there if Ghidra does not do it automatically.

Do not try to disassemble the entire `0x9FC00C20..0x9FC0219B` block as one
flat region first. Start from known entrypoints and work outward.

## 5. MIPS16 Region Handling

The MIPS16 library is the hard part. The practical workflow is:

1. Go to `0x9FC00C20`.
2. If Ghidra already created nonsense code in `0x9FC00C20..0x9FC0219B`, clear
   the incorrect interpretation in the specific subrange you are about to fix.
3. Start from known function-table entrypoints such as:
   - `0x9FC00C21`
   - `0x9FC00C29`
   - `0x9FC00C85`
   - `0x9FC00CFD`
4. If Ghidra exposes a mode/context setting for MIPS16 on the function or
   instruction stream, set it before disassembling the body.
5. Disassemble outward from that entrypoint only.
6. Stop when control flow clearly:
   - returns
   - lands in known MIPS32 helpers
   - reaches invalid padding
   - collides with another known function

Treat the MIPS16 region as a manually guided recovery problem, not a one-pass
analysis problem.

## 6. Known MIPS32 Helper Targets

These helper targets are known MIPS32 ROM sites and are useful cross-checks
when you recover MIPS16 call edges:

- `0x9FC00464`
- `0x9FC00834`
- `0x9FC00888`
- `0x9FC00980`
- `0x9FC009BC`
- `0x9FC00BC0`
- `0x9FC00C04`

If a recovered MIPS16 function reaches one of these and the target already
decodes cleanly as MIPS32, that is good evidence the mixed-mode interpretation
is on the right track.

## 7. Evidence Discipline

Use these evidence grades in comments or bookmarks:

- `Confirmed`: directly supported by the raw ROM or NAND bytes
- `Inferred`: best explanation from the confirmed control flow
- `Open`: still not pinned down

Seed annotations from
[`docs/ROM_SPL_HANDOFF.md`](/Users/jroark/src/be300-framebuffer/docs/ROM_SPL_HANDOFF.md)
only where the underlying bytes already support the claim.

In particular, keep these points explicit:

- the top-level ROM MIPS32 flow is confirmed
- the mailbox contract at `0x2400` / `0x24FC` is confirmed
- the exact MIPS16 basic block that copies the SPL and jumps to `0x80F00004`
  is still open
- the exact MIPS16 return path back into the ROM poll-and-`jr` loop is still
  open

Do not upgrade emulator-derived hypotheses into facts inside the Ghidra
database unless they are independently revalidated from the ROM bytes.

## 8. Acceptance Checks

A useful Ghidra session should end with:

- clean MIPS32 decoding for `0x9FC00000..0x9FC00C1F`
- labeled top-level ROM handoff logic at `0x9FC00390..0x9FC003D4`
- decoded function-table entries at `0x9FC021C0`
- at least the entry functions at `0x9FC00C21` and `0x9FC00C85` identified
  and separated from surrounding data/noise
- some trustworthy mixed-mode cross-references from MIPS16 into the known MIPS32
  helper set

If you do not get those five things, keep treating the session as setup/debug
work rather than as a trustworthy disassembly base.
