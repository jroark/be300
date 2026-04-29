#!/usr/bin/env python3
"""Build a bootable BE-300 NAND image from a Casio backup image.

Casio backup files are not full NAND dumps. They contain a small backup-tool
header followed by the FAT16 filesystem partition payload. This tool detects
and validates that payload, then grafts it onto a known-good bootable NAND
template, preserving the template's boot metadata, SPL, and NK.exe partitions.
"""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path


BYTES_PER_SECTOR = 512
ASCII_BACKUP_SIGNATURE = b"CASIO WinCE PDA backup file"
JX740_CFBACKUP_SIGNATURE = "Jx740 CFBackup Ver1.00".encode("utf-16le")
MAX_AUTODETECT_HEADER_SIZE = 4096
FAT_SIGNATURE = b"\x55\xaa"
FILESYSTEM_PARTITION_INDEX = 3
DEFAULT_TEMPLATE = "ce/restore_images/All_nand_300.bin"


@dataclass(frozen=True)
class NandPartition:
    index: int
    start_sector: int
    sector_count: int

    @property
    def offset(self) -> int:
        return self.start_sector * BYTES_PER_SECTOR

    @property
    def size_bytes(self) -> int:
        return self.sector_count * BYTES_PER_SECTOR

    @property
    def end(self) -> int:
        return self.offset + self.size_bytes


@dataclass(frozen=True)
class BackupPayload:
    data: bytes
    header_size: int
    trailer_size: int
    format_name: str


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--backup",
        required=True,
        help="Casio backup image with an autodetected wrapper, or a raw FAT16 payload.",
    )
    ap.add_argument(
        "--output",
        required=True,
        help="Output bootable NAND image.",
    )
    ap.add_argument(
        "--template",
        default=DEFAULT_TEMPLATE,
        help=f"Bootable donor NAND image (default: {DEFAULT_TEMPLATE}).",
    )
    ap.add_argument(
        "--raw-fat",
        action="store_true",
        help="Require --backup to be exactly the raw FAT16 partition payload without a Casio backup header.",
    )
    ap.add_argument(
        "--force",
        action="store_true",
        help="Overwrite --output if it already exists.",
    )
    return ap.parse_args()


def read_u16(data: bytes | bytearray, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0]


def read_u32(data: bytes | bytearray, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def parse_partition(template: bytes, index: int) -> NandPartition:
    entry_off = index * 16
    if entry_off + 16 > len(template):
        raise ValueError(f"partition entry {index} is outside the template")

    marker = template[entry_off:entry_off + 8]
    if marker != b"\xff" * 8:
        raise ValueError(
            f"partition entry {index} marker is not 8 bytes of 0xFF"
        )

    start_sector = read_u32(template, entry_off + 8)
    sector_count = read_u32(template, entry_off + 12)
    if start_sector in (0, 0xFFFFFFFF) and sector_count == 0xFFFFFFFF:
        raise ValueError(f"partition entry {index} is unused")
    if sector_count == 0 or sector_count == 0xFFFFFFFF:
        raise ValueError(f"partition entry {index} has invalid sector count")

    part = NandPartition(
        index=index,
        start_sector=start_sector,
        sector_count=sector_count,
    )
    if part.end > len(template):
        raise ValueError(
            f"partition {index} extends past template: "
            f"end=0x{part.end:X} size=0x{len(template):X}"
        )
    return part


def validate_fat16_payload(payload: bytes, expected_size: int) -> None:
    if len(payload) != expected_size:
        raise ValueError(
            f"payload size mismatch: 0x{len(payload):X} bytes, "
            f"expected partition size 0x{expected_size:X}"
        )
    if len(payload) < BYTES_PER_SECTOR:
        raise ValueError("payload is too small for a FAT boot sector")
    if payload[510:512] != FAT_SIGNATURE:
        raise ValueError("payload boot sector is missing the 0x55AA signature")

    bytes_per_sector = read_u16(payload, 0x0B)
    if bytes_per_sector != BYTES_PER_SECTOR:
        raise ValueError(
            f"payload BPB bytes/sector is {bytes_per_sector}, expected 512"
        )

    total_sectors = read_u16(payload, 0x13)
    if total_sectors == 0:
        total_sectors = read_u32(payload, 0x20)
    expected_sectors = expected_size // BYTES_PER_SECTOR
    if total_sectors != expected_sectors:
        raise ValueError(
            f"payload BPB total sectors is {total_sectors}, "
            f"expected {expected_sectors}"
        )

    fat_type = payload[0x36:0x3E].rstrip(b" ")
    if fat_type and fat_type != b"FAT16":
        raise ValueError(
            f"payload BPB filesystem label is {fat_type!r}, expected FAT16"
        )


def describe_backup_format(header: bytes, trailer_size: int) -> str:
    if not header and not trailer_size:
        return "raw-fat16"
    if header.startswith(ASCII_BACKUP_SIGNATURE):
        return "casio-ascii-backup"
    if header.startswith(JX740_CFBACKUP_SIGNATURE):
        return "jx740-cfbackup"
    return f"unknown-casio-header-0x{len(header):X}"


def add_candidate_offset(
    offsets: list[int],
    seen: set[int],
    offset: int,
    max_offset: int,
) -> None:
    if 0 <= offset <= max_offset and offset not in seen:
        offsets.append(offset)
        seen.add(offset)


def candidate_payload_offsets(backup: bytes, expected_size: int) -> list[int]:
    if len(backup) < expected_size:
        return []

    max_offset = min(len(backup) - expected_size, MAX_AUTODETECT_HEADER_SIZE)
    offsets: list[int] = []
    seen: set[int] = set()

    add_candidate_offset(offsets, seen, len(backup) - expected_size, max_offset)
    add_candidate_offset(offsets, seen, 0, max_offset)
    add_candidate_offset(offsets, seen, 0x200, max_offset)
    add_candidate_offset(offsets, seen, 0x240, max_offset)

    for offset in range(max_offset + 1):
        signature_off = offset + BYTES_PER_SECTOR - len(FAT_SIGNATURE)
        if backup[signature_off:signature_off + len(FAT_SIGNATURE)] == FAT_SIGNATURE:
            add_candidate_offset(offsets, seen, offset, max_offset)

    return offsets


def extract_payload(
    backup: bytes,
    raw_fat: bool,
    expected_size: int,
) -> BackupPayload:
    if raw_fat:
        validate_fat16_payload(backup, expected_size=expected_size)
        return BackupPayload(
            data=backup,
            header_size=0,
            trailer_size=0,
            format_name="raw-fat16",
        )

    errors: list[str] = []
    for offset in candidate_payload_offsets(backup, expected_size):
        payload = backup[offset:offset + expected_size]
        try:
            validate_fat16_payload(payload, expected_size=expected_size)
        except ValueError as exc:
            errors.append(f"0x{offset:X}: {exc}")
            continue

        trailer_size = len(backup) - offset - expected_size
        return BackupPayload(
            data=payload,
            header_size=offset,
            trailer_size=trailer_size,
            format_name=describe_backup_format(backup[:offset], trailer_size),
        )

    if len(backup) < expected_size:
        raise ValueError(
            f"backup is too small: 0x{len(backup):X} bytes, "
            f"expected at least payload size 0x{expected_size:X}"
        )
    if errors:
        joined = "; ".join(errors[:4])
        if len(errors) > 4:
            joined += f"; ... {len(errors) - 4} more"
        raise ValueError(f"could not validate a FAT16 backup payload ({joined})")
    raise ValueError(
        "could not find a FAT16 backup payload in the first "
        f"0x{MAX_AUTODETECT_HEADER_SIZE:X} bytes"
    )


def main() -> int:
    args = parse_args()
    backup_path = Path(args.backup)
    template_path = Path(args.template)
    output_path = Path(args.output)

    if output_path.exists() and not args.force:
        raise SystemExit(
            f"error: output already exists: {output_path} "
            "(use --force to overwrite)"
        )

    try:
        template = template_path.read_bytes()
        backup = backup_path.read_bytes()
        fs_part = parse_partition(template, FILESYSTEM_PARTITION_INDEX)
        payload = extract_payload(
            backup,
            raw_fat=args.raw_fat,
            expected_size=fs_part.size_bytes,
        )
    except OSError as exc:
        raise SystemExit(f"error: {exc}") from exc
    except ValueError as exc:
        raise SystemExit(f"error: {exc}") from exc

    out = bytearray(template)
    out[fs_part.offset:fs_part.end] = payload.data

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(out)

    summary = [
        "[build_bootable_nand_from_backup]",
        f"template={template_path}",
        f"backup={backup_path}",
        f"output={output_path}",
        f"backup_format={payload.format_name}",
        f"payload_offset=0x{payload.header_size:X}",
        f"partition={fs_part.index}",
        f"nand_offset=0x{fs_part.offset:X}",
        f"size=0x{fs_part.size_bytes:X}",
    ]
    if payload.trailer_size:
        summary.append(f"trailer=0x{payload.trailer_size:X}")
    print(" ".join(summary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
