#!/usr/bin/env python3
"""Decode the BE-300 compressed NK partition into logical and flat outputs."""

from __future__ import annotations

import argparse
from pathlib import Path

from nk_lzss import decode_nk_partition


def parse_int_auto(text: str) -> int:
    return int(text, 0)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--image",
        default="ce/restore_images/All_nand_300.bin",
        help="Path to the NAND image (default: ce/restore_images/All_nand_300.bin)",
    )
    ap.add_argument(
        "--partition-index",
        type=parse_int_auto,
        default=2,
        help="Partition-table index for NK (default: 2)",
    )
    ap.add_argument(
        "--out-dir",
        required=True,
        help="Output directory for nk_logical.bin / nk_flat.bin / nk_meta.txt",
    )
    args = ap.parse_args()

    image_path = Path(args.image)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    nand = image_path.read_bytes()
    image = decode_nk_partition(nand, partition_index=args.partition_index)

    (out_dir / "nk_logical.bin").write_bytes(image.logical_stream)
    (out_dir / "nk_flat.bin").write_bytes(image.flat_image)

    with (out_dir / "nk_records.tsv").open("w", encoding="ascii") as f:
        f.write("idx\tva\tlen\tcksum\tdata_off\n")
        for record in image.records:
            f.write(
                f"{record.index}\t0x{record.va:08X}\t0x{record.length:X}\t"
                f"0x{record.checksum:08X}\t0x{record.data_off:X}\n"
            )

    with (out_dir / "nk_meta.txt").open("w", encoding="ascii") as f:
        f.write(f"image_path={image_path}\n")
        f.write(f"partition_index={image.partition.index}\n")
        f.write(f"partition_offset=0x{image.partition.offset:X}\n")
        f.write(f"partition_size=0x{image.partition.size_bytes:X}\n")
        f.write(f"raw_consumed=0x{image.raw_consumed:X}\n")
        f.write(f"image_start=0x{image.image_start:08X}\n")
        f.write(f"declared_flat_size=0x{image.declared_flat_size:X}\n")
        f.write(f"base_va=0x{image.base_va:08X}\n")
        f.write(f"flat_size=0x{image.flat_size:X}\n")
        f.write(f"record_count={len(image.records)}\n")
        f.write(f"entry_va=0x{image.entry_va:08X}\n")
        f.write(f"terminal_off=0x{image.terminal_off:X}\n")
        f.write(f"stream_end=0x{image.stream_end:X}\n")

    print(
        "[extract_nk_lzss]"
        f" image={image_path}"
        f" partition={image.partition.index}"
        f" raw_consumed=0x{image.raw_consumed:X}"
        f" records={len(image.records)}"
        f" entry=0x{image.entry_va:08X}"
        f" flat_size=0x{image.flat_size:X}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
