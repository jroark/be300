# NAND Controller Emulation: WinCE Data Transfer Support

## Context

The BE-300 emulator now boots WinCE to the point where the kernel attempts NAND flash I/O. The SPL (Second Program Loader) successfully reads NAND using the transfer engine stream path (registers 0xA410-0xA464, data via 0xB000). However, the WinCE kernel's NAND driver uses an additional access pattern involving registers 0xA4A0-0xA4AC and 0xA4C0 that are not currently implemented. These reads return 0, causing the kernel to stall or fail during filesystem access.

The NAND image (All_nand_300.bin, 16MB) contains: SPL at offset 0x4000, compressed NK.exe at 0x14000, and a FAT16 filesystem at ~0x3B5000. The kernel needs NAND access to mount the filesystem and load drivers/shell.

## Current NAND Architecture

### File Locations
- `src/hw/nand.h` — Register defines, state struct (nand_state_t), geometry constants
- `src/hw/nand.c` — NAND controller read/write logic, stream engine, OOB synthesis
- `src/be300_devices.c` — GXemul device registration (two segments: nand_lo at PA 0x0A00A000+0x40, nand_hi at PA 0x0A00A050+0x37B0)

### NAND Geometry
- Page: 512 bytes data + 16 bytes OOB = 528 bytes raw (`NAND_PAGE_RAW`)
- Block: 32 pages
- Total: 1024 blocks = 32768 pages = 16MB data

### Working Data Path (SPL Stream Engine)
```
1. Write CMD 0x03 or 0x06 to 0xA414     → opens address phase
2. Write 3 address bytes to 0xA420       → [col, row_lo, row_hi]
3. Write KICK=1 to 0xA460               → starts engine
4. Write MODE=0x05 to 0xA464            → activates 520-byte stream
5. Poll 0xA440 (XFER_STATUS)            → returns 0x01 when ready
6. Read sequentially from 0xB000         → nand_stream_read() returns data
```

This path is fully implemented in `nand.c` lines 141-201 (write) and 352-393 (read).

### Broken WinCE Path (Observed from Logs)

The WinCE kernel NAND driver does both stream reads AND buffer register reads:

```
[NAND] W32 offset=0xA464 val=0x00000004    ← mode=4 (8-byte OOB read?)
[NAND] R32 offset=0xB000 -> 0xFFFFFFFF     ← stream not active, returns FF
[NAND] W32 offset=0xA464 val=0x00000000    ← mode=0 (reset?)
[NAND] W32 offset=0xA464 val=0x00000001    ← mode=1 (?)
[NAND] W32 offset=0xA468 val=0x000003FF    ← XFER_MISC (repeated 6x)

[NAND] R32 offset=0xA4C0 -> 0x00000000     ← STATUS2 (not implemented)
[NAND] R32 offset=0xA4A0 -> 0x00000000     ← BUFFER[0] (not implemented)
[NAND] R32 offset=0xA4A4 -> 0x00000000     ← BUFFER[1] (not implemented)
[NAND] R32 offset=0xA4A8 -> 0x00000000     ← BUFFER[2] (not implemented)
[NAND] R32 offset=0xA4AC -> 0x00000000     ← BUFFER[3] (not implemented)
```

These registers fall through to the generic `xfer_regs[]` latch in `nand_read()` at line 364-374, which just returns whatever was last stored (initially 0).

## Implementation Plan

### Step 1: Add XFER_STATUS2 register at 0xA4C0

In `nand.h`, add:
```c
#define NAND_REG_XFER_STATUS2  0xA4C0u
```

In `nand_read()`, within the `NAND_XFER_BASE..NAND_XFER_END` block (line 364), add a case for 0xA4C0 that returns the ready status, same as 0xA440:

```c
} else if (offset == NAND_REG_XFER_STATUS2) {
    val = s->ready ? 0x00000001u : 0u;
```

### Step 2: Add buffer registers at 0xA4A0-0xA4AC

These appear to be a 16-byte data buffer that the controller fills after a mode-4 (8-byte OOB) transfer. The WinCE driver reads 4 consecutive 32-bit values.

In `nand.h`, add:
```c
#define NAND_REG_BUFFER_BASE   0xA4A0u
#define NAND_REG_BUFFER_END    0xA4B0u   /* 4 x 32-bit = 16 bytes */
```

Add a 16-byte buffer field to `nand_state_t`:
```c
uint8_t  xfer_buffer[16];       /* data buffer for mode-4 reads (0xA4A0-0xA4AC) */
bool     xfer_buffer_valid;     /* true after mode-4 populates the buffer */
```

In `nand_read()`, add a handler:
```c
if (offset >= NAND_REG_BUFFER_BASE && offset < NAND_REG_BUFFER_END) {
    uint32_t buf_off = offset - NAND_REG_BUFFER_BASE;
    val = 0;
    for (unsigned i = 0; i < size && (buf_off + i) < 16; i++)
        val |= (uint64_t)s->xfer_buffer[buf_off + i] << (i * 8);
    goto out;
}
```

### Step 3: Implement mode-4 buffer fill in XFER_MODE write handler

When MODE=4 is written to 0xA464, the controller should read 8 bytes of OOB/trailer data into the buffer registers. In `nand_write()`, in the XFER_MODE handler (line 171), add a case for mode 4:

```c
} else if (data_byte == 0x04u) {
    /* Mode 4: 8-byte OOB/trailer read into buffer registers.
     * Uses the already-active stream position (after a mode-5
     * read) to grab the next 8 bytes, typically OOB data. */
    memset(s->xfer_buffer, 0xFF, sizeof(s->xfer_buffer));
    if (s->stream_active) {
        for (int i = 0; i < 8; i++) {
            int b = nand_stream_byte(s, s->stream_cursor + i);
            if (b >= 0)
                s->xfer_buffer[i] = (uint8_t)b;
        }
        s->stream_cursor += 8;
    }
    s->xfer_buffer_valid = true;
}
```

Note: `nand_stream_byte()` is an existing static function (around line 60) that returns the byte at a given cursor position within the current stream page.

### Step 4: Handle mode-0 and mode-1 writes to XFER_MODE

From the logs, WinCE writes mode=0 and mode=1 to 0xA464 before the buffer reads. These likely mean:
- Mode 0: reset/idle — clear stream and buffer state
- Mode 1: start a new transfer setup phase

Add handling in the XFER_MODE write handler:
```c
if (data_byte == 0x00u) {
    /* Mode 0: reset stream state */
    s->stream_active = false;
    s->stream_cursor = 0;
    s->xfer_buffer_valid = false;
    memset(s->xfer_buffer, 0, sizeof(s->xfer_buffer));
} else if (data_byte == 0x01u) {
    /* Mode 1: prepare for buffer read (address already set) */
    s->xfer_buffer_valid = false;
}
```

### Step 5: Handle XFER_MISC register 0xA468

The WinCE driver writes 0x03FF to 0xA468 (NAND_REG_XFER_MISC) six times. This register is already latched (`xfer_regs[]`), but the repeated writes suggest it might trigger a buffer fill or configure transfer length.

For now, treat 0xA468 writes as a buffer-fill trigger when stream is active:

```c
} else if (offset == NAND_REG_XFER_MISC) {
    /* WinCE writes 0x03FF repeatedly after mode/address setup.
     * If stream is active, pre-fill the buffer registers with
     * the next 16 bytes from the stream. */
    if (s->stream_active && !s->xfer_buffer_valid) {
        memset(s->xfer_buffer, 0xFF, sizeof(s->xfer_buffer));
        for (int i = 0; i < 16; i++) {
            int b = nand_stream_byte(s, s->stream_cursor + i);
            if (b >= 0)
                s->xfer_buffer[i] = (uint8_t)b;
        }
        s->xfer_buffer_valid = true;
    }
}
```

### Step 6: Ensure mode-4 activates stream if not already active

The WinCE driver sometimes writes mode=4 to 0xA464 without a preceding mode=5. In this case, if `xfer_addr_count >= 3` (address was already set), activate the stream:

Add to the mode-4 handler:
```c
if (!s->stream_active && s->xfer_addr_count >= 3) {
    uint32_t row = (uint32_t)s->xfer_addr_bytes[1]
                 | ((uint32_t)s->xfer_addr_bytes[2] << 8);
    uint32_t col = (uint32_t)s->xfer_addr_bytes[0];
    s->stream_page = row;
    s->stream_col = col;
    s->stream_base = row * NAND_PAGE_DATA + col;
    s->stream_cursor = 0;
    s->stream_active = true;
}
```

## Files to Modify

1. **`src/hw/nand.h`** — Add register defines (XFER_STATUS2, BUFFER_BASE/END), add `xfer_buffer[16]` and `xfer_buffer_valid` to nand_state_t
2. **`src/hw/nand.c`** — Add read handlers for 0xA4A0-0xA4AC and 0xA4C0, add mode-0/1/4 handling in write, add XFER_MISC buffer-fill trigger

## Verification

1. Build: `cd build-host && cmake .. && make -j$(nproc)`
2. Run WinCE cold boot:
   ```
   gtimeout 60s ./be300 --nand ../ce/restore_images/All_nand_300.bin \
     --wince-cold-boot --log-mmio > cold_stdout.log 2> cold_stderr.log
   ```
3. Check NAND buffer reads now return data:
   ```
   grep "R32 offset=0xA4A" cold_stderr.log | head -20
   ```
   Should show non-zero values instead of 0x00000000
4. Check NAND stream reads continue to work:
   ```
   grep "NAND_STREAM_FIRST" cold_stderr.log | head -5
   ```
5. Check for further boot progress:
   ```
   tail -30 cold_stderr.log
   ```
   Should show new PCs / different behavior than the NAND stall
6. Check Linux kernel regression (must still boot to userspace):
   ```
   gtimeout 20s ./be300 --cmdline "console=tty0 root=/dev/ram" \
     --kernel ../kernels/vmlinux-pgui-demo > 2.4_stdout.log 2> 2.4_stderr.log
   ```
   NOTE: The emulator opens an SDL window — run from terminal, not from automated tools.
