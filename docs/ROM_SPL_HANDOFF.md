# BE-300 ROM -> SPL Load And Handoff

This note reconstructs the cold-boot path from the masked ROM into the SPL and
back out to the next-stage entry handoff.

It does **not** treat the emulator source or emulator runtime as ground truth.
Evidence is ranked in this order:

1. Raw ROM dump: `docs/be300_boot_rom.bin`, `docs/BE300BootROM_v1.txt`
2. Raw NAND bytes: `ce/restore_images/All_nand_300.bin`
3. Static decoding of those bytes, including `tools/extract_b000ff.py`
4. Existing repo notes and captures, only when they match the raw artifacts
5. Emulator code/logs, only as hints, never as proof

Status labels used below:

- `Confirmed`: directly supported by ROM/NAND bytes
- `Inferred`: best explanation from the confirmed control flow
- `Open`: still not pinned down from static evidence alone

## 1. Confirmed ROM Mailbox Contract

The MIPS32 ROM code around `0x9FC00354..0x9FC00494` establishes a small SDRAM
handoff mailbox at physical `0x2400/0x24FC`.

Confirmed from `docs/be300_boot_rom.bin`:

```mips
9fc00354: lui   a1,0xa000
9fc00358: addiu a1,a1,0x2400
9fc0035c: sw    zero,0(a1)
9fc00360: lui   a1,0xa000
9fc00364: addiu a1,a1,0x24fc
9fc00368: sw    zero,0(a1)
```

The ROM later exposes two helper routines:

```mips
9fc00430: lw    t0,0x2400($t1)
9fc00444: beq   t0,0x03020100,...
9fc0044c: addi  a1,a1,1        # 0x03020101
9fc00458: sw    zero,0x2400($t1)
```

```mips
9fc00464: sw    a0,0x24fc($a1)
9fc00478: lui   a0,0x0302
9fc0047c: ori   a0,a0,0x0101
9fc00484: sw    a0,0x2400($a1)
```

Confirmed conclusions:

- `0x24FC` is the next-stage jump target mailbox.
- `0x2400` is a state/version marker mailbox.
- The ROM accepts `0x03020100` and `0x03020101` as meaningful values in
  `0x2400`; anything else gets cleared.
- A helper at `0x9FC00464` writes the jump target to `0x24FC` and the marker
  `0x03020101` to `0x2400`.

Corroborating capture:

- `docs/hw_dump_combined.txt` shows post-boot SDRAM at `0x2400` containing
  `0x03020100`, which matches the ROM-side compare logic.

## 2. Top-Level ROM Cold-Boot Flow

### 2.1 Reset and early setup

Confirmed from the reset vector and the MIPS32 ROM body:

```mips
9fc00000: nop
9fc00004: lui   k0,0x9fc0
9fc00008: ori   k0,k0,0x02f0
9fc0000c: jr    k0
```

At `0x9FC002F0`, the ROM:

- writes CP0 `$18` with `-8`
- writes CP0 Config with `0x00035B23`
- calls `0x9FC006F0`
- calls `0x9FC0064C`
- calls `0x9FC005A0`
- calls `0x9FC006F0` again
- calls `0x9FC003EC`

`0x9FC006F0` is confirmed to clear/tag memory lines:

```mips
9fc006f0: mtc0  zero,$28
9fc006f4: lui   a0,0x8000
...
```

### 2.2 Cold/warm gating

Confirmed:

- `0x9FC003EC` checks VR4131/VRC4173 state and returns a flag in `v0`.
- If that path does not resolve boot state, the ROM calls `0x9FC0042C`, which
  validates the marker at `0xA0002400`.
- On the cold path, the ROM clears both mailboxes, calls `0x9FC00734`, sets
  `sp = 0x80003800`, and calls `0x9FC00498` with `a0 = 0`.

`0x9FC00734` is confirmed to touch VR4131 state and perform a RAM test/write
pattern pass rooted at `0xA00037F0`. It is ROM-side hardware/init logic, not
the SPL entry.

### 2.3 Where the ROM stops being explicit

After that setup, the top-level MIPS32 ROM flow is:

```mips
9fc00390: jalr  0x9fc00c85
9fc003a0: jalr  0x9fc00c21
9fc003b0: jal   0x9fc004e8
9fc003b8: jal   0x9fc00488
9fc003c0: beqz  v0,0x9fc003a0
9fc003c8: lw    t0,0x24fc
9fc003d4: jr    t0
```

Confirmed:

- The ROM does **not** contain a direct MIPS32 `jal/jr` to `0x80F00004`.
- The only explicit next-stage jump in the top-level MIPS32 path is `jr t0`
  using the value from `0x24FC`.
- The ROM function metadata area at `0x9FC021C0` contains both
  `0x9FC00C21` and `0x9FC00C85` in its address table:

```text
0x9FC021C0: 0x9FC00C21
0x9FC021C4: 0x9FC00C85
0x9FC021D0: 0x9FC00C29
0x9FC021D4: 0x9FC00C85
0x9FC021D8: 0x9FC00CFD
```

Inferred:

- The actual ROM -> SPL transfer happens inside the MIPS16 region entered via
  `0x9FC00C85` and/or `0x9FC00C21`.
- That same MIPS16 region is also responsible for eventually causing a valid
  jump target to appear in `0x24FC`.
- Because `0x9FC00C21` and `0x9FC00C85` also appear in the ROM's own function
  table, they are more likely library-style MIPS16 entry points than one-off
  branch targets.

Open:

- The exact MIPS16 basic block that copies the SPL records and branches to
  `0x80F00004` is not isolated in this pass.

## 3. Confirmed SPL Container Format at NAND `0x4000`

The NAND image at `ce/restore_images/All_nand_300.bin` contains a record-based
SPL image starting at byte offset `0x4000`.

Confirmed raw bytes:

```text
0x00004000: 42 30 30 30 46 46 0A    "B000FF\n"
0x00004007: 00 00 F0 80             image_start  = 0x80F00000
0x0000400B: 84 E0 00 00             image_length = 0x0000E084
```

Using the raw bytes plus `tools/extract_b000ff.py`, the confirmed record list
is:

| Record | VA | Length | Notes |
| --- | --- | ---: | --- |
| 0 | `0x80F01000` | `0xB3BC` | Main SPL code/data block |
| 1 | `0x80F0D000` | `0x0A3C` | Secondary SPL code/data block |
| 2 | `0x80F00040` | `0x0008` | `ECEC` header fragment |
| 3 | `0x80F0E000` | `0x0054` | Small metadata block |
| 4 | `0x80F0E054` | `0x0020` | Small metadata block |
| 5 | `0x80F0E074` | `0x0010` | Small metadata block |
| 6 | `0x80F00000` | `0x0014` | Reset stub / entry veneer |
| 7 | `0x80F0C3BC` | `0x0120` | Late metadata/pointer block |

The entry record is not padded to 4-byte alignment. The tail looks like:

```text
0x00010027: 00 00 00 00             sentinel word
0x0001002B: 04 00 F0 80             entry_va    = 0x80F00004
0x0001002F: 00 00 00 00             entry_len   = 0
0x00010033: FF FF FF FF             entry_cksum = 0xFFFFFFFF
```

Confirmed conclusions:

- The SPL is not a flat binary in NAND. It is a byte-packed record stream.
- The ROM-side loader must tolerate a 4-byte zero sentinel followed by a
  regular entry record.
- The explicit SPL entry is `0x80F00004`.
- The SPL-side generic walker at `0x80F0A160` consumes that same tail as a
  12-byte terminal control block `[0, entry_va, 0]` and ignores the trailing
  `0xFFFFFFFF` checksum word.

## 4. Confirmed SPL Entry Stub

Record 6 installs the first instructions at `0x80F00000`:

```mips
80f00000: nop
80f00004: lui   k0,0x80f0
80f00008: ori   k0,k0,0x2404
80f0000c: jr    k0
80f00010: nop
```

The first substantive SPL code is therefore at `0x80F02404`:

```mips
80f02404: lui   t0,0x0003
80f02408: ori   t0,t0,0x5b23
80f0240c: mtc0  t0,$16
80f02410: lui   t0,0x80f0
80f02414: addiu t0,t0,0x2428
80f02418: lui   k0,0x2000
80f0241c: or    t0,t0,k0
80f02420: jr    t0
```

Confirmed conclusion:

- The SPL immediately switches itself to the uncached alias and continues at
  `0xA0F02428`.

## 5. Confirmed SPL Loader Core and ROM Mailbox Write

The success/failure split is explicit in SPL MIPS32 code around
`0x80F02280..0x80F023EC`.

Relevant confirmed instructions:

```mips
80f02364: jal   0x80f03b64
80f02368: nop
80f0236c: move  a1,v0
80f02370: bnez  v0,0x80f023b4
```

If `0x80F03B64` returns zero, the SPL prints failure strings and never programs
the ROM jump mailbox.

If it succeeds:

```mips
80f023b4: lui   at,0x2000
80f023b8: or    a1,a1,at
80f023bc: lui   t5,0xa000
80f023c4: sw    a1,0x24fc(t5)
80f023c8: lui   t6,0x0302
80f023cc: ori   t6,t6,0x0101
80f023d0: sw    t6,0x2400(t7)
```

Confirmed conclusions:

- The SPL itself writes the ROM handoff mailboxes.
- The value written to `0x24FC` is the successful return value from
  `0x80F03B64`, ORed with `0x20000000`.
- In practice, that means the ROM is handed an **uncached** next-stage virtual
  address.

`0x80F03B64` is the core loader-side function:

- it logs the banner string at `0x80F014C8`, which is
  `"Kernel loader core - Ver 0.52"`
- it clears `0x2400`
- it calls a function pointer loaded from `0x80F0159C`, which resolves to
  `0x80F03CE0`
- it passes `sp+0x20` to `0x80F0A058` together with a callback pointer loaded
  from `0x80F01590`
- it finally returns the word stored at `sp+0x20`

Confirmed conclusion:

- `0x80F03B64` returns the next-stage entry address through an out-parameter
  slot and then mirrors that value in `v0`.

### 5.1 Confirmed callback table shape

The callback table at `0x80F01590` is explicit:

```text
0x80F01590: 0x80F03C20
0x80F01594: 0x80F03C28
0x80F01598: 0x80F03CD8
0x80F0159C: 0x80F03CE0
```

Confirmed:

- `0x80F03CE0` is the open/init hook used before the walker starts.
- `0x80F03C28` is the byte-stream read/copy hook used by both the header probe
  and the record walker.
- `0x80F03C20` and `0x80F03CD8` are stubs that return success/no-op.

`0x80F03C28` is confirmed to:

- copy bytes from an internal buffer to the caller destination
- update source pointers at `0x80E01084/0x80E6108C`
- call `0x80F03EB0` to refill the buffer when needed

This means the higher-level walker is operating on a logical byte stream,
not directly on raw NAND MMIO.

### 5.2 Confirmed header probe at `0x80F03D58`

`0x80F03D58` stages 7 bytes into `0x80E61090`, compares them against the
literal `"B000FF\n"` at `0x80F01E90`, and sets a mode flag at `0x80E61088`.

Confirmed behavior:

- if the 7-byte compare succeeds, it prints:
  - `"This data may be compressed"`
  - `"Header:"`
  - a 10-byte hex dump of the following header bytes
- if the compare fails, it prints `"WARN: BIN data is not compressed"`
- in either case it then calls `0x80F03F00(512)` to establish the next read
  window

Confirmed conclusion:

- The SPL has an explicit "B000FF-aware front end" before the generic loader
  loop begins.
- Raw and compressed payloads are handled behind a shared byte-stream reader.

### 5.3 Confirmed record walker at `0x80F0A058` / `0x80F0A160`

`0x80F0A058` is stricter than `0x80F03D58`: it requires the next stream to
start with an exact `"B000FF\n"` sync marker.

Confirmed behavior:

- it reads 7 bytes through callback slot 1 (`0x80F03C28`)
- if the compare fails, it prints `"Sync Error"` and returns `-1`
- if the compare succeeds, it prints `"Start downloading..."`
- it then reads the next 8 bytes and stores them at `0x80E71A80` and
  `0x80E71A78`
- it finally calls `0x80F0A160(out_ptr, ..., callbacks)`

`0x80F0A160` is the important generic loader loop. Confirmed from the code:

- it repeatedly reads 12-byte headers through callback slot 1
- it interprets the first two words of each 12-byte chunk as `(addr, len)`
- it does **not** use the third word in the loop body
- if `addr != 0` and `len != 0`, it copies `len` bytes to `(addr | 0x20000000)`
  through callback slot 1
- if `addr == 0`, it prints `"Done"`, stores `len` to `*out_ptr`, and returns
  success

Confirmed conclusion:

- The SPL-side handoff value comes directly from the terminal 12-byte control
  block consumed by `0x80F0A160`.
- The value later written to `0x24FC` is therefore `(terminal_len | 0x20000000)`,
  not an opaque result computed elsewhere.

Inferred:

- For the SPL image, the terminal control block is formed by the observed
  4-byte zero sentinel followed by the first 8 bytes of the entry record.
- For the NK image, the same walker is probably fed by a decompressed or
  dewrapped logical stream, because the on-NAND bytes do not match the raw
  record grammar at `records_start`.

## 6. What the NK Image Tells Us

The next NAND image starts one byte later than the nominal offset, and all
four restore images share that trait.

```text
0x00014000: FF
0x00014001: 42 30 30 30 46 46 0A    "B000FF\n"
0x00014009: FF 00 06 80             image_start  = 0x800600FF
0x0001400D: C8 6A 5F 00             image_length = 0x005F6AC8
```

Confirmed:

- In `All_nand_300.bin`, `org_CE_30.bin`, `BE500.bin`, and `CE_Net.bin`, the
  NK signature is found at `0x14001` and the plausible header starts at
  `0x14009` (`header_pad = 1`).
- A direct record parse fails immediately in all tested images with
  `record 0 data overflows records window`.
- The first would-be record header at `records_start` is therefore not a valid
  simple `(addr, len, cksum)` header of the same kind used by the SPL.
- The local parser falls back to treating each NK payload as a wrapped raw blob.
- SPL strings mention both compressed and uncompressed BIN handling:
  - `"WARN: BIN data is not compressed"`
  - `"This data may be compressed"`
  - `"ERROR: Decompress data is overflow"`

Representative header values:

| Image | `image_start` | `image_length` | `image_start & ~0xFF` |
| --- | --- | ---: | --- |
| `All_nand_300.bin` | `0x800600FF` | `0x005F6AC8` | `0x80060000` |
| `org_CE_30.bin` | `0x800600FF` | `0x005F5AC4` | `0x80060000` |
| `BE500.bin` | `0x8004D0FF` | `0x006DF418` | `0x8004D000` |
| `CE_Net.bin` | `0x8002907F` | `0x005FD324` | `0x80029000` |

Confirmed conclusions:

- The SPL does more than copy a second simple `B000FF` record stream.
- There is a distinct NK staging/decompression path inside the SPL.
- The low byte of the NK `image_start` word is carrying some additional
  meaning or wrapper state; it is not just a plain aligned RAM base.

Inferred:

- The one-byte pad at `0x14008` and the odd low byte in `image_start` are part
  of a different outer wrapper around the main NK payload.
- That wrapper eventually yields the logical record stream consumed by
  `0x80F0A160`, because the generic loader still expects a terminal control
  block that returns the handoff value.

Open:

- The exact NK wrapper format and the precise returned success entry address for
  `All_nand_300.bin` are not proven from static evidence alone in this pass.

## 7. Final Reconstructed Handoff

### Confirmed sequence

1. ROM reset vector jumps to `0x9FC002F0`.
2. ROM performs CP0/cache/platform setup and cold/warm gating.
3. ROM clears `0x2400` and `0x24FC` on the cold path.
4. ROM enters the MIPS16 region at `0x9FC00C85` / `0x9FC00C21`.
5. The actual ROM -> SPL transfer occurs somewhere inside that MIPS16 region.
6. The SPL entry record points to `0x80F00004`, which immediately jumps to
   real code at `0x80F02404`, then to the uncached alias at `0xA0F02428`.
7. The SPL loader core at `0x80F03B64` drives the callback-based record walker
   and receives the terminal handoff value through `sp+0x20`.
8. The SPL writes `(handoff_value | 0x20000000)` to `0x24FC` and `0x03020101` to
   `0x2400`.
9. Control returns to ROM MIPS32 code, which polls `0x24FC` and finally
   executes `jr` to the value stored there.

### Inferred but not yet fully proven

- The MIPS16 calls at `0x9FC00C85` and `0x9FC00C21` are the hidden bridge that
  loads the SPL, later resumes ROM-side post-load processing, and eventually
  leads to the `jr 0x24FC` handoff.
- For CE images, the terminal handoff value produced by `0x80F0A160` is the
  true NK entry, so the final `0x24FC` value becomes that entry in uncached
  form.

## 8. Open Questions

- Which exact MIPS16 basic block copies the SPL records out of NAND and jumps
  to `0x80F00004`?
- Which exact MIPS16 basic block resumes ROM-side execution after the SPL has
  populated `0x24FC`, leading back to the poll-and-`jr` sequence at
  `0x9FC003A0..0x9FC003D4`?
- What exact terminal handoff value does `0x80F0A160` produce for the CE 3.0
  image, and where does that value land in the logical decoded NK stream?
- What exact outer wrapper/compression grammar turns the on-NAND NK bytes at
  `0x14001` into the logical stream consumed by `0x80F0A160`?
