#!/usr/bin/env python3
"""Extract SPL raw memory image from BE-300 NAND B000FF stream."""
import sys

if len(sys.argv) < 3:
    print("usage: extract_spl.py <NAND image> <out.bin>", file=sys.stderr)
    sys.exit(1)

nand = open(sys.argv[1], "rb").read()
# B000FF signature at NAND offset 0x4000
assert nand[0x4000:0x4007] == b"B000FF\n", "signature mismatch at 0x4000"

base = 0x4007
image_start = int.from_bytes(nand[base:base + 4], "little")
total_len = int.from_bytes(nand[base + 4:base + 8], "little")
print(f"image start VA: 0x{image_start:08x}, total len: 0x{total_len:x}", file=sys.stderr)

# Parse records: (addr, len, cksum, data...)
pos = base + 8
# Allocate output buffer sized to total_len; fill from records at (addr - image_start)
out = bytearray(total_len)
record_count = 0
while pos + 12 < len(nand):
    addr = int.from_bytes(nand[pos:pos + 4], "little")
    length = int.from_bytes(nand[pos + 4:pos + 8], "little")
    cksum = int.from_bytes(nand[pos + 8:pos + 12], "little")
    pos += 12
    # Terminator: all-zero record
    if addr == 0 and length == 0:
        break
    if addr < image_start or addr + length > image_start + total_len:
        print(f"record out of range: addr=0x{addr:x} len=0x{length:x}", file=sys.stderr)
        break
    data = nand[pos:pos + length]
    pos += length
    off = addr - image_start
    out[off:off + length] = data
    record_count += 1

print(f"wrote {record_count} records covering {len(out)} bytes", file=sys.stderr)
open(sys.argv[2], "wb").write(out)
