#!/usr/bin/env python3
"""Generate an experimental WinCE NAND image with a larger display mode.

The stock BE-300 WinCE 3.0 display driver hardcodes a 240x320 mode, a
512-byte primary-surface stride, and a 0x40000-byte framebuffer mapping. This
tool patches the verified ``ddi.dll`` constants in the NK XIP image to expose
480x640 with a 1024-byte stride and a 0xA0000-byte framebuffer mapping, then
repacks the NK partition into a new NAND image.
"""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path

from nk_lzss import (
    decode_lzss,
    decode_nk_partition,
    encode_lzss,
    patch_logical_stream_from_flat,
)


NK_BASE = 0x80060000
PTOC_VA = 0x80655C54
TABLE_VA = 0x80655CA4
TOC_ENTRY_SIZE = 32
E32_SIZE = 0x6C
O32_ENTRY_SIZE = 24


@dataclass(frozen=True)
class WordPatch:
    va: int
    old_word: int
    new_word: int
    label: str


def imm_patch(va: int, old_word: int, new_imm: int, label: str) -> WordPatch:
    return WordPatch(
        va=va,
        old_word=old_word,
        new_word=(old_word & 0xFFFF0000) | (new_imm & 0xFFFF),
        label=label,
    )


PATCHES_480X640 = (
    WordPatch(0x01A538B4, 0x000FCA40, 0x000FCA80, "glyph destination y stride shift"),
    imm_patch(0x01A5391C, 0x241901E0, 0x03C0, "blit row bytes"),
    imm_patch(0x01A539CC, 0x241100F0, 0x01E0, "blit width"),
    imm_patch(0x01A539D0, 0x24100140, 0x0280, "blit height"),
    imm_patch(0x01A53A38, 0x244201E0, 0x03C0, "blit row advance"),
    imm_patch(0x01A53A80, 0x28A10140, 0x0280, "blit row loop"),
    imm_patch(0x01A53A88, 0x24420200, 0x0400, "framebuffer stride"),
    WordPatch(0x01A53B14, 0x00077240, 0x00077280, "copy destination y stride shift"),
    WordPatch(0x01A53C4C, 0x00118A40, 0x00118A80, "copy scratch stride shift"),
    imm_patch(0x01A53BD4, 0x258E0200, 0x0400, "glyph stride"),
    imm_patch(0x01A53D0C, 0x26940200, 0x0400, "glyph row advance"),
    WordPatch(0x01A53DDC, 0x00077A40, 0x00077A80, "fb-to-fb destination y stride shift"),
    WordPatch(0x01A53E1C, 0x000B7240, 0x000B7280, "fb-to-fb source y stride shift"),
    WordPatch(0x01A53EE4, 0x000E7A40, 0x000E7A80, "solid-fill destination y stride shift"),
    imm_patch(0x01A54128, 0x24090200, 0x0400, "surface stride"),
    imm_patch(0x01A54690, 0x3C050004, 0x000A, "framebuffer map bytes"),
    WordPatch(0x01A54834, 0x00067240, 0x00067280, "cursor/pixel y stride shift"),
    imm_patch(0x01A55F78, 0x24120140, 0x0280, "clear height"),
    imm_patch(0x01A55F8C, 0x24060200, 0x0400, "clear row bytes"),
    imm_patch(0x01A55F98, 0x26100200, 0x0400, "clear row advance"),
    imm_patch(0x01A56520, 0x240800F0, 0x01E0, "surface width"),
    imm_patch(0x01A56524, 0x24090140, 0x0280, "surface height"),
    WordPatch(0x01A5C6F0, 0x8E290004, 0x240901E0, "DrvGetModes width"),
    WordPatch(0x01A5C6FC, 0x8E2A0008, 0x240A0280, "DrvGetModes height"),
)


def parse_int_auto(text: str) -> int:
    return int(text, 0)


def u32(data: bytes | bytearray, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def read_xip_cstr(nk: bytes | bytearray, va: int) -> str:
    off = va - NK_BASE
    if off < 0 or off >= len(nk):
        raise ValueError(f"XIP string VA out of range: 0x{va:08X}")
    end = nk.find(b"\0", off)
    if end < 0:
        raise ValueError(f"unterminated XIP string at VA 0x{va:08X}")
    return nk[off:end].decode("latin1", "replace")


def find_xip_module(nk: bytes | bytearray, module_name: str) -> int:
    if read_xip_cstr(nk, u32(nk, TABLE_VA - NK_BASE + 0x14)) != "nk.exe":
        raise ValueError("XIP TOC sanity check failed")

    count = u32(nk, PTOC_VA - NK_BASE + 0x10)
    want = module_name.lower()
    for index in range(count):
        entry_off = TABLE_VA - NK_BASE + index * TOC_ENTRY_SIZE
        name_va = u32(nk, entry_off + 0x14)
        if read_xip_cstr(nk, name_va).lower() == want:
            return entry_off
    raise ValueError(f"XIP module slot not found: {module_name}")


def find_module_text(nk: bytes | bytearray, module_name: str) -> tuple[int, int, int]:
    entry_off = find_xip_module(nk, module_name)
    e32_va = u32(nk, entry_off + 0x18)
    o32_va = u32(nk, entry_off + 0x1C)
    e32_off = e32_va - NK_BASE
    o32_off = o32_va - NK_BASE

    if e32_off < 0 or e32_off + E32_SIZE > len(nk):
        raise ValueError(f"{module_name}: E32 metadata out of range")
    if o32_off < 0 or o32_off + O32_ENTRY_SIZE > len(nk):
        raise ValueError(f"{module_name}: O32 metadata out of range")

    vbase = u32(nk, e32_off + 0x08)
    rva = u32(nk, o32_off + 0x04)
    size = u32(nk, o32_off + 0x08)
    data_va = u32(nk, o32_off + 0x0C)
    data_off = data_va - NK_BASE
    if data_off < 0 or data_off + size > len(nk):
        raise ValueError(f"{module_name}: text storage out of range")

    return vbase + rva, data_off, size


def patch_ddi_480x640(nk: bytearray) -> list[str]:
    base_va, data_off, size = find_module_text(nk, "ddi.dll")
    notes: list[str] = []

    for patch in PATCHES_480X640:
        rel = patch.va - base_va
        if rel < 0 or rel + 4 > size:
            raise ValueError(f"{patch.label}: VA 0x{patch.va:08X} is outside ddi.dll")

        off = data_off + rel
        current = u32(nk, off)
        if current == patch.new_word:
            notes.append(f"already patched 0x{patch.va:08X} {patch.label}")
            continue
        if current != patch.old_word:
            raise ValueError(
                f"{patch.label}: expected 0x{patch.old_word:08X} at "
                f"0x{patch.va:08X}, found 0x{current:08X}"
            )

        struct.pack_into("<I", nk, off, patch.new_word)
        notes.append(
            f"patched 0x{patch.va:08X} {patch.label}: "
            f"0x{patch.old_word:08X} -> 0x{patch.new_word:08X}"
        )

    return notes


def build_patched_nand(nand: bytes, partition_index: int) -> tuple[bytes, list[str], dict[str, int]]:
    image = decode_nk_partition(nand, partition_index=partition_index)
    patched_flat = bytearray(image.flat_image)
    notes = patch_ddi_480x640(patched_flat)
    replacement_logical = patch_logical_stream_from_flat(image, bytes(patched_flat))
    replacement_raw = encode_lzss(replacement_logical)

    if len(replacement_raw) > image.partition.size_bytes:
        raise ValueError(
            f"repacked NK raw image does not fit partition: "
            f"0x{len(replacement_raw):X} > 0x{image.partition.size_bytes:X}"
        )
    if len(replacement_raw) > image.raw_consumed:
        raise ValueError(
            f"repacked NK raw image would overwrite bytes after the stock "
            f"compressed stream: 0x{len(replacement_raw):X} > "
            f"0x{image.raw_consumed:X}"
        )

    roundtrip_logical, raw_consumed = decode_lzss(
        replacement_raw, output_limit=len(replacement_logical)
    )
    if roundtrip_logical != replacement_logical:
        raise ValueError("LZSS roundtrip verification failed")

    out = bytearray(nand)
    part_start = image.partition.offset
    out[part_start:part_start + len(replacement_raw)] = replacement_raw

    meta = {
        "partition_index": image.partition.index,
        "partition_offset": image.partition.offset,
        "partition_size": image.partition.size_bytes,
        "original_raw_consumed": image.raw_consumed,
        "replacement_raw_size": len(replacement_raw),
        "replacement_raw_consumed": raw_consumed,
        "logical_stream_size": len(replacement_logical),
        "flat_size": image.flat_size,
        "entry_va": image.entry_va,
    }
    return bytes(out), notes, meta


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--nand-image",
        default="ce/restore_images/All_nand_300.bin",
        help="Source NAND image (default: ce/restore_images/All_nand_300.bin)",
    )
    ap.add_argument("--out-image", required=True, help="Output patched NAND image")
    ap.add_argument(
        "--partition-index",
        type=parse_int_auto,
        default=2,
        help="Partition-table index for NK (default: 2)",
    )
    ap.add_argument(
        "--meta-out",
        help="Optional metadata output path",
    )
    args = ap.parse_args()

    nand_path = Path(args.nand_image)
    out_path = Path(args.out_image)
    if nand_path.resolve() == out_path.resolve():
        raise SystemExit("error: --out-image must be different from --nand-image")
    meta_path = Path(args.meta_out) if args.meta_out else out_path.with_suffix(
        out_path.suffix + ".meta.txt"
    )

    nand = nand_path.read_bytes()
    patched, notes, meta = build_patched_nand(nand, args.partition_index)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(patched)
    with meta_path.open("w", encoding="ascii") as f:
        f.write(f"source_nand={nand_path}\n")
        f.write(f"out_image={out_path}\n")
        f.write("mode=480x640 stride=1024\n")
        for key, value in meta.items():
            if key.endswith("_va") or key.endswith("_offset") or key.endswith("_size") or key.endswith("_consumed"):
                f.write(f"{key}=0x{value:X}\n")
            else:
                f.write(f"{key}={value}\n")
        for note in notes:
            f.write(f"{note}\n")

    print(
        "[patch_wince_fb_size]"
        f" out={out_path}"
        f" mode=480x640"
        f" raw_size=0x{meta['replacement_raw_size']:X}"
        f" original_raw=0x{meta['original_raw_consumed']:X}"
        f" partition=0x{meta['partition_size']:X}"
    )
    for note in notes:
        print(f"[patch_wince_fb_size] {note}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
