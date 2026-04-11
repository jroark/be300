#!/usr/bin/env python3
"""Build a flashable NAND image that boots a Linux kernel on a real Casio BE-300.

Packages a Linux ELF kernel into a NAND image using B000FF record format.
The ROM's native B000FF walker loads everything into RAM, then a tiny MIPS32
bootstrap stub sets up boot arguments and jumps to the kernel entry point.

The bootstrap stub never returns to ROM, so WinCE-specific ROM post-processing
(section copier, callback registrar) never executes.

Usage:
    python3 tools/build_nand_linux.py \\
        --kernel kernels/vmlinux-pgui-demo \\
        --cmdline "console=tty0 root=/dev/ram" \\
        --output linux_nand.bin
"""

import argparse
import struct
import sys
from pathlib import Path


# --- Constants ---

NAND_PAGE_SIZE = 512
NAND_PAGES_PER_BLOCK = 32
NAND_BLOCK_COUNT = 1004
NAND_IMAGE_SIZE = NAND_BLOCK_COUNT * NAND_PAGES_PER_BLOCK * NAND_PAGE_SIZE

# Partition table: 16-byte entries at page 0
# Format: 8 bytes 0xFF + start_sector(4 LE) + size_sectors(4 LE)
PART_ENTRY_SIZE = 16
PART0_START = 0       # boot metadata
PART0_SIZE = 32       # 32 sectors = 16KB
PART1_START = 32      # SPL/kernel (our B000FF image)

# B000FF format
B000FF_SIG = b"B000FF\n"

# MIPS bootstrap stub load address (in SPL region, safe from kernel overlap)
BOOTSTRAP_VA = 0x80F00000

# Temporary load offset: kernel segments are loaded to (original_va + TEMP_OFFSET)
# to avoid overwriting the ROM's stack at PA 0x3800 during B000FF loading.
# The bootstrap stub copies data from temp back to final addresses before
# jumping to the kernel entry point.
TEMP_OFFSET = 0x200000  # 2MB offset — keeps temp well above ROM data structures

# BE-300 hardware constants
FB_PA = 0x0A200000
FB_WIDTH = 240
FB_HEIGHT = 320
FB_XSIZE_MEM = 256
FB_BITS = 16

# hpc_bootinfo constants
HPC_BOOTINFO_MAGIC = 0x13536135
BIFB_D16_0000 = 5
BI_CNUSE_BUILTIN = 1

# Platform IDs (from GXemul machine_hpcmips.c)
PLATID_CPU = (
    (1 << 26)   # MIPS
    + (1 << 20) # VR
    + (1 << 14) # VR41XX
    + (6 << 8)  # VR4131
    + 0          # flags
)
PLATID_MACHINE = (
    (3 << 22)   # Casio
    + (1 << 16) # CassiopeiaE
    + (2 << 8)  # EXXX
    + 3          # E500 (used for BE-300 in GXemul)
)

# 16MB SDRAM
RAM_SIZE = 16 * 1024 * 1024
RAM_BASE = 0x80000000  # kseg0

# Boot argument locations (matches GXemul prom emulation)
ARGV_VA = RAM_BASE + RAM_SIZE - 512   # 0x80FFFE00
BOOTINFO_VA = RAM_BASE + RAM_SIZE - 256  # 0x80FFFF00


# --- ELF Parsing ---

ELF_MAGIC = b"\x7fELF"
ET_EXEC = 2
EM_MIPS = 8
PT_LOAD = 1

def parse_elf(data):
    """Parse ELF32 LE MIPS executable, return (entry_va, segments)."""
    if len(data) < 52:
        raise ValueError("file too small for ELF header")
    if data[:4] != ELF_MAGIC:
        raise ValueError("not an ELF file")

    ei_class = data[4]
    if ei_class != 1:
        raise ValueError(f"not ELF32 (class={ei_class})")
    ei_data = data[5]
    if ei_data != 1:
        raise ValueError(f"not little-endian (data={ei_data})")

    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    if e_type != ET_EXEC:
        raise ValueError(f"not executable (type={e_type})")
    if e_machine != EM_MIPS:
        raise ValueError(f"not MIPS (machine={e_machine})")

    (e_entry, e_phoff, _, _, _ehsz, e_phentsize, e_phnum) = struct.unpack_from(
        "<IIIIHHH", data, 24
    )

    segments = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        (p_type, p_offset, p_vaddr, p_paddr,
         p_filesz, p_memsz, p_flags, p_align) = struct.unpack_from(
            "<IIIIIIII", data, off
        )
        if p_type == PT_LOAD and p_filesz > 0:
            seg_data = data[p_offset:p_offset + p_filesz]
            # Use vaddr, masked to kseg0
            va = p_vaddr | 0x80000000
            va &= 0xFFFFFFFF
            segments.append({
                "va": va,
                "data": seg_data,
                "filesz": p_filesz,
                "memsz": p_memsz,
            })

    if not segments:
        raise ValueError("no PT_LOAD segments found")

    return e_entry, segments


# --- MIPS32 Bootstrap Stub ---

def build_bootstrap_stub(kernel_entry, argc, argv_va, bootinfo_va,
                         copy_src=None, copy_dst=None, copy_size=None):
    """Build a MIPS32 bootstrap stub that optionally relocates kernel data,
    sets $a0-$a2, and jumps to kernel entry.

    If copy_src/copy_dst/copy_size are provided, the stub first copies
    kernel data from the temporary B000FF load address to the kernel's
    actual link address, then sets up boot arguments and jumps.

    All values are encoded as immediate operands (LUI+ORI pairs).
    """
    def lui_ori(reg, val):
        """Generate LUI+ORI instruction pair for a 32-bit immediate."""
        val &= 0xFFFFFFFF
        hi = (val >> 16) & 0xFFFF
        lo = val & 0xFFFF
        # LUI reg, hi  -> 001111 00000 reg hi16
        lui = (0x3C000000 | (reg << 16) | hi)
        # ORI reg, reg, lo -> 001101 reg reg lo16
        ori = (0x34000000 | (reg << 21) | (reg << 16) | lo)
        return struct.pack("<II", lui, ori)

    code = bytearray()

    if copy_src is not None:
        # Phase 1: Copy kernel from temporary to final address
        # $s0 = src, $s1 = dst, $s2 = src_end
        code += lui_ori(16, copy_src)          # $s0 = copy_src
        code += lui_ori(17, copy_dst)          # $s1 = copy_dst
        code += lui_ori(18, copy_src + copy_size)  # $s2 = copy_src_end
        # copy_loop:
        #   lw $t0, 0($s0)
        #   sw $t0, 0($s1)
        #   addiu $s0, $s0, 4
        #   bne $s0, $s2, copy_loop
        #   addiu $s1, $s1, 4     (delay slot)
        code += struct.pack("<I", 0x8E080000)  # lw $t0, 0($s0)
        code += struct.pack("<I", 0xAE280000)  # sw $t0, 0($s1)
        code += struct.pack("<I", 0x26100004)  # addiu $s0, $s0, 4
        code += struct.pack("<I", 0x1612FFFC)  # bne $s0, $s2, -4 (copy_loop)
        code += struct.pack("<I", 0x26310004)  # addiu $s1, $s1, 4

    # Phase 2: Set up boot arguments and jump to kernel
    # $a0 (reg 4) = argc
    code += lui_ori(4, argc)
    # $a1 (reg 5) = argv pointer
    code += lui_ori(5, argv_va)
    # $a2 (reg 6) = bootinfo pointer
    code += lui_ori(6, bootinfo_va)
    # $t0 (reg 8) = kernel entry point
    code += lui_ori(8, kernel_entry)
    # JR $t0
    code += struct.pack("<I", 0x01000008)  # jr $t0
    # NOP (delay slot)
    code += struct.pack("<I", 0x00000000)
    return bytes(code)


# --- Boot Arguments ---

def build_argv_block(kernel_name, cmdline, argv_va):
    """Build the argv array + strings block (512 bytes max)."""
    block = bytearray(512)

    # String locations within the block
    name_offset = 16
    cmdline_offset = 64

    name_va = argv_va + name_offset
    cmdline_va = argv_va + cmdline_offset

    name_bytes = kernel_name.encode("ascii") + b"\x00"
    cmdline_bytes = cmdline.encode("ascii") + b"\x00"

    if name_offset + len(name_bytes) > cmdline_offset:
        raise ValueError("kernel name too long")
    if cmdline_offset + len(cmdline_bytes) > 512:
        raise ValueError("command line too long (max ~448 chars)")

    # argc determines how many argv pointers
    has_cmdline = len(cmdline) > 0

    # argv[0] = pointer to kernel name
    struct.pack_into("<I", block, 0, name_va)
    if has_cmdline:
        # argv[1] = pointer to cmdline
        struct.pack_into("<I", block, 4, cmdline_va)
        # argv[2] = NULL
        struct.pack_into("<I", block, 8, 0)
    else:
        # argv[1] = NULL
        struct.pack_into("<I", block, 4, 0)

    # Strings
    block[name_offset:name_offset + len(name_bytes)] = name_bytes
    block[cmdline_offset:cmdline_offset + len(cmdline_bytes)] = cmdline_bytes

    # Trim trailing zeros for the B000FF record (no need to send 512 bytes
    # of mostly zeros — the ROM copies to RAM that was just cleared by ROM init)
    # Actually, keep the full 512 to match GXemul layout exactly.
    return bytes(block)


def build_bootinfo():
    """Build the hpc_bootinfo struct (36 bytes, matches GXemul)."""
    info = bytearray(36)

    fb_addr_kseg0 = 0x80000000 + FB_PA
    fb_line_bytes = FB_XSIZE_MEM * (FB_BITS // 8)

    struct.pack_into("<h", info, 0, 36)                  # length
    struct.pack_into("<h", info, 2, 0)                    # reserved
    struct.pack_into("<i", info, 4, HPC_BOOTINFO_MAGIC)   # magic
    struct.pack_into("<I", info, 8, fb_addr_kseg0)        # fb_addr
    struct.pack_into("<h", info, 12, fb_line_bytes)       # fb_line_bytes
    struct.pack_into("<h", info, 14, FB_WIDTH)            # fb_width
    struct.pack_into("<h", info, 16, FB_HEIGHT)           # fb_height
    struct.pack_into("<h", info, 18, BIFB_D16_0000)       # fb_type
    struct.pack_into("<h", info, 20, BI_CNUSE_BUILTIN)    # bi_cnuse
    # 1 byte padding from alignment
    struct.pack_into("<I", info, 24, PLATID_CPU)          # platid_cpu
    struct.pack_into("<I", info, 28, PLATID_MACHINE)      # platid_machine
    struct.pack_into("<i", info, 32, 0)                   # timezone

    return bytes(info)


# --- B000FF Image Builder ---

def build_b000ff_record(addr, data):
    """Build a single B000FF record: addr(4) + len(4) + cksum(4) + data."""
    length = len(data)
    # Checksum: simple 32-bit sum (ROM walker ignores it, but compute anyway)
    cksum = 0
    for i in range(0, length - 3, 4):
        cksum += struct.unpack_from("<I", data, i)[0]
    cksum &= 0xFFFFFFFF

    header = struct.pack("<III", addr, length, cksum)
    return header + data


def build_b000ff_terminal(entry_va):
    """Build the terminal record: addr=0, len=entry_point, cksum=0."""
    return struct.pack("<III", 0, entry_va, 0)


def build_b000ff_image(image_start, records, entry_va):
    """Assemble a complete B000FF image.

    records: list of (addr, data) tuples
    entry_va: entry point for terminal record
    """
    # Build all record data
    records_data = bytearray()
    for addr, data in records:
        records_data += build_b000ff_record(addr, data)
    records_data += build_b000ff_terminal(entry_va)

    image_length = len(records_data)

    # B000FF header: signature + image_start + image_length
    header = B000FF_SIG + struct.pack("<II", image_start, image_length)

    return bytes(header) + bytes(records_data)


# --- NAND Image Builder ---

def build_partition_table(entries):
    """Build partition table page (512 bytes).

    entries: list of (start_sector, size_sectors) tuples
    """
    page = bytearray(b"\xff" * NAND_PAGE_SIZE)

    for i, (start, size) in enumerate(entries):
        off = i * PART_ENTRY_SIZE
        # 8 bytes 0xFF (already filled)
        struct.pack_into("<II", page, off + 8, start, size)

    return bytes(page)


def build_nand_image(partition_table, b000ff_data):
    """Assemble the complete 16MB NAND image."""
    image = bytearray(b"\xff" * NAND_IMAGE_SIZE)

    # Page 0: partition table
    image[0:NAND_PAGE_SIZE] = partition_table

    # Pages PART1_START onwards: B000FF data
    b000ff_offset = PART1_START * NAND_PAGE_SIZE
    if b000ff_offset + len(b000ff_data) > NAND_IMAGE_SIZE:
        raise ValueError(
            f"B000FF image too large for NAND: {len(b000ff_data)} bytes, "
            f"max {NAND_IMAGE_SIZE - b000ff_offset} bytes"
        )
    image[b000ff_offset:b000ff_offset + len(b000ff_data)] = b000ff_data

    return bytes(image)


# --- Main ---

def main():
    ap = argparse.ArgumentParser(
        description="Build a Linux-booting NAND image for Casio BE-300"
    )
    ap.add_argument(
        "--kernel", required=True,
        help="Path to Linux ELF kernel (e.g. kernels/vmlinux-pgui-demo)"
    )
    ap.add_argument(
        "--cmdline", default="console=tty0 root=/dev/ram",
        help="Kernel command line (default: 'console=tty0 root=/dev/ram')"
    )
    ap.add_argument(
        "--output", required=True,
        help="Output NAND image path (e.g. linux_nand.bin)"
    )
    ap.add_argument(
        "--kernel-name", default="vmlinux",
        help="Kernel name for argv[0] (default: vmlinux)"
    )
    ap.add_argument(
        "--entry-override", type=lambda x: int(x, 0), default=None,
        help="Override kernel entry point (hex address)"
    )
    ap.add_argument(
        "--no-bootinfo", action="store_true",
        help="Skip bootinfo/argv setup (for 2.6 kernels that ignore boot args)"
    )
    args = ap.parse_args()

    # Read and parse kernel ELF
    kernel_path = Path(args.kernel)
    if not kernel_path.is_file():
        print(f"error: kernel not found: {kernel_path}", file=sys.stderr)
        return 1

    print(f"Reading kernel: {kernel_path}")
    kernel_data = kernel_path.read_bytes()
    entry_va, segments = parse_elf(kernel_data)

    if args.entry_override is not None:
        entry_va = args.entry_override

    print(f"  Entry point: 0x{entry_va:08X}")
    print(f"  PT_LOAD segments: {len(segments)}")
    total_filesz = 0
    for i, seg in enumerate(segments):
        print(f"    [{i}] VA=0x{seg['va']:08X} filesz=0x{seg['filesz']:X} memsz=0x{seg['memsz']:X}")
        total_filesz += seg["filesz"]
    print(f"  Total file data: {total_filesz:,} bytes ({total_filesz / 1024 / 1024:.1f} MB)")

    # Build B000FF records
    records = []

    # Compute the kernel memory range (final addresses)
    kernel_lo = min(s["va"] for s in segments)
    kernel_hi = max(s["va"] + s["filesz"] for s in segments)
    # Align copy size up to 4 bytes
    copy_size = ((kernel_hi - kernel_lo) + 3) & ~3

    # Temporary addresses: shift kernel segments up by TEMP_OFFSET to
    # avoid overwriting the ROM's stack (PA 0x3800) during B000FF loading
    temp_lo = kernel_lo + TEMP_OFFSET
    temp_hi = temp_lo + copy_size

    # Verify temp area fits in SDRAM and doesn't overlap bootstrap/bootinfo
    temp_pa_end = (temp_hi & 0x1FFFFFFF)
    if temp_pa_end > RAM_SIZE:
        raise ValueError(
            f"Kernel too large for SDRAM with temp offset: "
            f"temp end PA 0x{temp_pa_end:08X} > RAM size 0x{RAM_SIZE:08X}"
        )

    print(f"\nKernel range: 0x{kernel_lo:08X}-0x{kernel_hi:08X} ({copy_size:,} bytes)")
    print(f"Temp range:   0x{temp_lo:08X}-0x{temp_hi:08X} (offset +0x{TEMP_OFFSET:X})")

    # Record 0: bootstrap stub (with memcpy from temp to final)
    if args.no_bootinfo:
        argc = 0
        stub = build_bootstrap_stub(
            entry_va, 0, 0, 0,
            copy_src=temp_lo, copy_dst=kernel_lo, copy_size=copy_size,
        )
    else:
        argc = 2 if args.cmdline else 1
        stub = build_bootstrap_stub(
            entry_va, argc, ARGV_VA, BOOTINFO_VA,
            copy_src=temp_lo, copy_dst=kernel_lo, copy_size=copy_size,
        )

    records.append((BOOTSTRAP_VA, stub))
    print(f"Bootstrap stub: {len(stub)} bytes at VA 0x{BOOTSTRAP_VA:08X}")

    # Records 1-N: kernel PT_LOAD segments at TEMPORARY addresses
    for seg in segments:
        temp_va = seg["va"] + TEMP_OFFSET
        records.append((temp_va, seg["data"]))

    # Records N+1, N+2: argv and bootinfo
    if not args.no_bootinfo:
        argv_block = build_argv_block(args.kernel_name, args.cmdline, ARGV_VA)
        records.append((ARGV_VA, argv_block))
        print(f"Argv block: {len(argv_block)} bytes at VA 0x{ARGV_VA:08X}")

        bootinfo = build_bootinfo()
        records.append((BOOTINFO_VA, bootinfo))
        print(f"Bootinfo: {len(bootinfo)} bytes at VA 0x{BOOTINFO_VA:08X}")

        print(f"  cmdline: \"{args.cmdline}\"")
        print(f"  argc={argc} argv=0x{ARGV_VA:08X} bootinfo=0x{BOOTINFO_VA:08X}")

    # Assemble B000FF image
    # Entry point for B000FF terminal = bootstrap stub address
    b000ff = build_b000ff_image(BOOTSTRAP_VA, records, BOOTSTRAP_VA)
    print(f"\nB000FF image: {len(b000ff):,} bytes ({len(b000ff) / 1024 / 1024:.1f} MB)")

    # Calculate partition 1 size in sectors
    part1_sectors = (len(b000ff) + NAND_PAGE_SIZE - 1) // NAND_PAGE_SIZE
    print(f"Partition 1: sectors {PART1_START}-{PART1_START + part1_sectors - 1} ({part1_sectors} sectors)")

    # Build partition table
    partition_table = build_partition_table([
        (PART0_START, PART0_SIZE),      # Entry 0: boot metadata
        (PART1_START, part1_sectors),    # Entry 1: SPL+kernel (our B000FF)
    ])

    # Assemble NAND image
    nand_image = build_nand_image(partition_table, b000ff)
    print(f"NAND image: {len(nand_image):,} bytes ({len(nand_image) / 1024 / 1024:.1f} MB)")

    # Write output
    output_path = Path(args.output)
    output_path.write_bytes(nand_image)
    print(f"\nWritten: {output_path}")
    print(f"\nTo test in emulator:")
    print(f"  gtimeout 30s ./be300 --nand {output_path} > linux_nand_stdout.log 2> linux_nand_stderr.log")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
