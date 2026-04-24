#!/usr/bin/env python3
"""Repack a NAND image's compressed NK partition from a replacement flat NK."""

from __future__ import annotations

import argparse
from pathlib import Path

from nk_lzss import decode_lzss, decode_nk_partition, encode_lzss, patch_logical_stream_from_flat


def parse_int_auto(text: str) -> int:
    return int(text, 0)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--nand-image",
        default="ce/restore_images/All_nand_300.bin",
        help="Source NAND image (default: ce/restore_images/All_nand_300.bin)",
    )
    ap.add_argument(
        "--flat-nk",
        required=True,
        help="Replacement flat NK image (for example docs/nk_decompressed.bin)",
    )
    ap.add_argument(
        "--out-image",
        required=True,
        help="Output NAND image path",
    )
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
    flat_path = Path(args.flat_nk)
    out_path = Path(args.out_image)
    meta_path = Path(args.meta_out) if args.meta_out else out_path.with_suffix(out_path.suffix + ".meta.txt")

    nand = nand_path.read_bytes()
    replacement_flat = flat_path.read_bytes()
    image = decode_nk_partition(nand, partition_index=args.partition_index)
    replacement_logical = patch_logical_stream_from_flat(image, replacement_flat)
    replacement_raw = encode_lzss(replacement_logical)

    if len(replacement_raw) > image.partition.size_bytes:
        raise SystemExit(
            f"error: repacked NK raw image does not fit partition: "
            f"0x{len(replacement_raw):X} > 0x{image.partition.size_bytes:X}"
        )

    roundtrip_logical, raw_consumed = decode_lzss(replacement_raw, output_limit=len(replacement_logical))
    if roundtrip_logical != replacement_logical:
        raise SystemExit("error: LZSS roundtrip verification failed")

    out = bytearray(nand)
    part_start = image.partition.offset
    out[part_start:part_start + len(replacement_raw)] = replacement_raw
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(out)

    with meta_path.open("w", encoding="ascii") as f:
        f.write(f"source_nand={nand_path}\n")
        f.write(f"flat_nk={flat_path}\n")
        f.write(f"out_image={out_path}\n")
        f.write(f"partition_index={image.partition.index}\n")
        f.write(f"partition_offset=0x{image.partition.offset:X}\n")
        f.write(f"partition_size=0x{image.partition.size_bytes:X}\n")
        f.write(f"original_raw_consumed=0x{image.raw_consumed:X}\n")
        f.write(f"replacement_raw_size=0x{len(replacement_raw):X}\n")
        f.write(f"replacement_raw_consumed=0x{raw_consumed:X}\n")
        f.write(f"logical_stream_size=0x{len(replacement_logical):X}\n")
        f.write(f"flat_size=0x{image.flat_size:X}\n")
        f.write(f"entry_va=0x{image.entry_va:08X}\n")

    print(
        "[repack_nk_partition]"
        f" out={out_path}"
        f" raw_size=0x{len(replacement_raw):X}"
        f" partition=0x{image.partition.size_bytes:X}"
        f" raw_consumed=0x{raw_consumed:X}"
        f" entry=0x{image.entry_va:08X}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
