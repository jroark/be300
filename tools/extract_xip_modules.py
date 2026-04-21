#!/usr/bin/env python3
"""Extract WinCE XIP modules from a flat packed NK.bin image.

The NK.bin at `docs/nk_decompressed.bin` is not just nk.exe — it is the packed
WinCE XIP image containing nk.exe plus 94 user-mode modules (gwes.exe,
coredll.dll, filesys.exe, ...). Each module has its own preferred VA base and
its own section list.

This tool walks the ROMHDR at pTOC and writes each module into its own .bin,
ready to load into Ghidra at the module's real vbase (printed alongside the
file name). Alongside each .bin it writes a .txt sidecar with the section
map (vbase, rva, vsize, psize, flags) needed to configure Ghidra memory
blocks with correct permissions; and an `index.txt` at the root listing all
modules with their vbases and sizes.

Section flags are the standard PE IMAGE_SCN_* bits. The sidecar decodes
them into a block-name hint (".text" / ".rdata" / ".data" / ".pdata") so
the Ghidra-side loader can preserve R/W/X permissions.

Usage:
    python3 tools/extract_xip_modules.py
    python3 tools/extract_xip_modules.py --nk docs/nk_decompressed.bin \\
                                         --out build-host/modules \\
                                         --nk-base 0x80060000

ROMHDR / TOC entry layout (32 bytes, verified against entry 0 == "nk.exe"):
    +0x00 dwAttrs            (4)
    +0x04 ?                  (4)
    +0x08 FILETIME           (8)
    +0x10 nFileSize          (4)
    +0x14 lpszName (VA)      (4)
    +0x18 ulE32Off (VA)      (4)
    +0x1C ulO32Off (VA)      (4)

E32 header (used fields):
    +0x00 objcnt             (u16)
    +0x08 vbase              (u32)
    +0x14 vsize              (u32)

O32 entry layout (24 bytes):
    +0x00 vsize              (4)
    +0x04 rva                (4)
    +0x08 psize              (4)
    +0x0C dataptr (VA)       (4)
    +0x10 realaddr           (4)
    +0x14 flags              (4)

PE section flags (IMAGE_SCN_*):
    0x00000020 CNT_CODE
    0x00000040 CNT_INITIALIZED_DATA
    0x00000080 CNT_UNINITIALIZED_DATA
    0x20000000 MEM_EXECUTE
    0x40000000 MEM_READ
    0x80000000 MEM_WRITE
"""

import argparse
import os
import struct
import sys
from pathlib import Path

PTOC_VA = 0x80655C54
TABLE_VA = 0x80655CA4  # first TOC entry; entry 0 name must be "nk.exe"
TOC_ENTRY_SIZE = 32
O32_ENTRY_SIZE = 24


def va_to_off(va, nk_base):
    return va - nk_base


def read_u32(data, va, nk_base):
    off = va_to_off(va, nk_base)
    return struct.unpack_from("<I", data, off)[0]


def read_u16(data, va, nk_base):
    off = va_to_off(va, nk_base)
    return struct.unpack_from("<H", data, off)[0]


def read_cstr(data, va, nk_base):
    off = va_to_off(va, nk_base)
    end = data.find(b"\x00", off)
    if end < 0:
        end = off
    return data[off:end].decode("latin1", "replace")


# IMAGE_SCN_* (PE/COFF section flags, relevant bits)
SCN_CNT_CODE                = 0x00000020
SCN_CNT_INITIALIZED_DATA    = 0x00000040
SCN_CNT_UNINITIALIZED_DATA  = 0x00000080
SCN_MEM_EXECUTE             = 0x20000000
SCN_MEM_READ                = 0x40000000
SCN_MEM_WRITE               = 0x80000000


def classify_section(flags):
    """Return (block_name, perm_string) like (".text", "r-x") based on PE
    section flags. block_name is what the Ghidra-side loader should call
    the memory block; perm_string is the RWX summary for human reading."""
    r = "r" if (flags & SCN_MEM_READ) else "-"
    w = "w" if (flags & SCN_MEM_WRITE) else "-"
    x = "x" if (flags & SCN_MEM_EXECUTE) else "-"
    perm = f"{r}{w}{x}"
    if flags & SCN_CNT_CODE:
        name = ".text"
    elif flags & SCN_CNT_UNINITIALIZED_DATA:
        name = ".bss"
    elif flags & SCN_CNT_INITIALIZED_DATA:
        # Heuristic: writable initialized data is .data, read-only is .rdata.
        name = ".data" if (flags & SCN_MEM_WRITE) else ".rdata"
    else:
        name = ".sec"
    return name, perm


def reassemble_module(data, nk_base, e32_va, o32_va, objcnt):
    """Reassemble a module image: allocate vsize bytes, copy each section's
    psize bytes from dataptr into the image at rva.

    Returns (image_bytes, vbase, vsize, section_summary).
    """
    vbase = read_u32(data, e32_va + 0x08, nk_base)
    vsize = read_u32(data, e32_va + 0x14, nk_base)

    img = bytearray(vsize)
    sections = []
    for j in range(objcnt):
        o = o32_va + j * O32_ENTRY_SIZE
        sec_vsize = read_u32(data, o + 0x00, nk_base)
        rva = read_u32(data, o + 0x04, nk_base)
        psize = read_u32(data, o + 0x08, nk_base)
        dptr = read_u32(data, o + 0x0C, nk_base)
        flags = read_u32(data, o + 0x14, nk_base)
        copied = 0
        if dptr >= nk_base and psize > 0:
            fstart = dptr - nk_base
            fend = fstart + psize
            if fend <= len(data) and rva + psize <= vsize:
                img[rva:rva + psize] = data[fstart:fend]
                copied = psize
        block_name, perm = classify_section(flags)
        sections.append({
            "idx": j,
            "rva": rva,
            "vsize": sec_vsize,
            "psize": psize,
            "dataptr": dptr,
            "flags": flags,
            "copied": copied,
            "block_name": block_name,
            "perm": perm,
        })
    return bytes(img), vbase, vsize, sections


def write_sidecar(sidecar_path, name, vbase, vsize, objcnt, sections):
    """Write a .txt sidecar describing the section map for Ghidra loading."""
    lines = []
    lines.append(f"module: {name}")
    lines.append(f"vbase:  0x{vbase:08X}")
    lines.append(f"vsize:  0x{vsize:06X}")
    lines.append(f"objcnt: {objcnt}")
    lines.append("")
    lines.append(
        f"{'idx':>3} {'name':<6} {'perm':<4} "
        f"{'vstart':>10} {'vsize':>8} {'psize':>8} "
        f"{'file_off':>10} {'flags':>10} {'src':>8}"
    )
    lines.append("-" * 80)
    for s in sections:
        vstart = vbase + s["rva"]
        # file_off is the offset within THIS module's .bin (= rva, since
        # the .bin is the full virtual image aligned at vbase).
        file_off = s["rva"]
        src = "copied" if s["copied"] else ("zero" if s["psize"] == 0 else "missing")
        lines.append(
            f"{s['idx']:>3} {s['block_name']:<6} {s['perm']:<4} "
            f"0x{vstart:08X} 0x{s['vsize']:06X} 0x{s['psize']:06X} "
            f"0x{file_off:08X} 0x{s['flags']:08X} {src:>8}"
        )
    sidecar_path.write_text("\n".join(lines) + "\n")


def write_index(index_path, modules):
    """Write the top-level index.txt summarizing every extracted module."""
    lines = []
    lines.append(
        "# WinCE XIP modules extracted from docs/nk_decompressed.bin\n"
        "# Load each *.bin into Ghidra as \"Raw Binary\" at the listed base.\n"
        "# Language: MIPS:LE:32:default. Options → Base Address: 0x<base>.\n"
        "# See tools/extract_xip_modules.py docstring and *.txt sidecars for\n"
        "# per-module section maps.\n"
    )
    lines.append(
        f"{'idx':>3}  {'name':<24} {'vbase':>10} {'vsize':>8} "
        f"{'objcnt':>6} {'status':<8}"
    )
    lines.append("-" * 80)
    for m in modules:
        lines.append(
            f"[{m['idx']:>2}] {m['name']:<24} 0x{m['vbase']:08X} "
            f"0x{m['vsize']:06X} {m['objcnt']:>6} {m['status']:<8}"
        )
    index_path.write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(
        description="Extract WinCE XIP modules from a packed NK.bin",
    )
    parser.add_argument("--nk", default="docs/nk_decompressed.bin",
                        help="Path to the flat packed NK image "
                             "(default: docs/nk_decompressed.bin)")
    parser.add_argument("--out", default="build-host/modules",
                        help="Output directory for per-module .bin files "
                             "(default: build-host/modules)")
    parser.add_argument("--nk-base", type=lambda x: int(x, 0),
                        default=0x80060000,
                        help="Base VA of the NK image (default: 0x80060000)")
    args = parser.parse_args()

    nk_path = Path(args.nk)
    if not nk_path.is_file():
        sys.exit(f"error: NK image not found: {nk_path}")

    data = nk_path.read_bytes()
    nk_base = args.nk_base
    print(f"Loaded {len(data)} bytes from {nk_path}, base VA=0x{nk_base:08X}")

    # Sanity: entry 0 name must decode as "nk.exe". Guards against wrong pTOC
    # or a corrupted image.
    name0_va = read_u32(data, TABLE_VA + 0x14, nk_base)
    name0 = read_cstr(data, name0_va, nk_base)
    if name0 != "nk.exe":
        sys.exit(
            f"error: TOC sanity check failed — expected entry[0] name "
            f"'nk.exe', got '{name0}' (name VA 0x{name0_va:08X}). "
            f"The pTOC or table address may be wrong for this image."
        )

    nummods = read_u32(data, PTOC_VA + 0x10, nk_base)
    print(f"pTOC=0x{PTOC_VA:08X} nummods={nummods} table_va=0x{TABLE_VA:08X}")
    print()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    compressed_only = []
    written = 0
    all_modules = []

    print(f"{'idx':>3}  {'name':<24} {'vbase':>10} {'vsize':>8} objcnt")
    print("-" * 60)

    for i in range(nummods):
        entry = TABLE_VA + i * TOC_ENTRY_SIZE
        name_va = read_u32(data, entry + 0x14, nk_base)
        name = read_cstr(data, name_va, nk_base)
        e32_va = read_u32(data, entry + 0x18, nk_base)
        o32_va = read_u32(data, entry + 0x1C, nk_base)
        objcnt = read_u16(data, e32_va + 0x00, nk_base)

        img, vbase, vsize, sections = reassemble_module(
            data, nk_base, e32_va, o32_va, objcnt,
        )

        print(f"[{i:>2}] {name:<24} 0x{vbase:08X} 0x{vsize:06X} {objcnt}")

        # A module whose every section has dataptr outside the NK image is
        # compressed / pulled from elsewhere — we can't extract usable bytes
        # from the flat NK image for it.
        is_compressed = bool(sections) and all(s["copied"] == 0 for s in sections)
        if is_compressed:
            compressed_only.append(name)

        safe = name.replace("/", "_").replace("\\", "_") or f"module_{i}"
        out_path = out_dir / f"{i:02d}_{safe}.bin"
        out_path.write_bytes(img)

        sidecar_path = out_dir / f"{i:02d}_{safe}.txt"
        write_sidecar(sidecar_path, name, vbase, vsize, objcnt, sections)

        all_modules.append({
            "idx": i,
            "name": name,
            "vbase": vbase,
            "vsize": vsize,
            "objcnt": objcnt,
            "status": "compressed" if is_compressed else "ok",
        })
        written += 1

    # Top-level index
    write_index(out_dir / "index.txt", all_modules)

    print()
    print(f"Wrote {written} module(s) to {out_dir}/ (each .bin + .txt sidecar)")
    print(f"Wrote {out_dir}/index.txt")
    if compressed_only:
        print()
        print(f"WARNING: {len(compressed_only)} module(s) had no in-NK "
              f"section bytes (likely compressed or externally stored):")
        for name in compressed_only:
            print(f"  {name}")


if __name__ == "__main__":
    main()
