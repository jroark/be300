#!/usr/bin/env python3

import argparse
import math
import os
import pathlib
import struct
import sys
from dataclasses import dataclass


BYTES_PER_SECTOR = 512
ROOT_ENTRY_COUNT = 512
NUM_FATS = 2
RESERVED_SECTORS = 1
MEDIA_DESCRIPTOR = 0xF8
DEFAULT_SIZE_MIB = 24
DEFAULT_LABEL = "BE300CF"
FAT16_MIN_CLUSTERS = 4085
FAT16_MAX_CLUSTERS = 65524
DEFAULT_VOLUME_ID = 0xBE300CF0
FIXED_FAT_TIME = 0
FIXED_FAT_DATE = ((1980 - 1980) << 9) | (1 << 5) | 1


@dataclass
class InputFile:
    host_path: pathlib.Path
    dest_name: str
    data: bytes
    short_name: bytes = b""
    start_cluster: int = 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build a FAT16 CF image for BE-300 restore boot."
    )
    parser.add_argument(
        "--output",
        required=True,
        help="Path to the output FAT16 CF image.",
    )
    parser.add_argument(
        "--size-mib",
        type=int,
        default=DEFAULT_SIZE_MIB,
        help="CF image size in MiB (default: 24).",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Overwrite the output image if it already exists.",
    )
    parser.add_argument(
        "--volume-label",
        default=DEFAULT_LABEL,
        help="FAT volume label (default: BE300CF).",
    )
    parser.add_argument(
        "--nandwriter",
        default="ce/restore_images/NANDWRITER.bin",
        help="Path to NANDWRITER.bin (default: ce/restore_images/NANDWRITER.bin).",
    )
    parser.add_argument(
        "--kloader",
        default="ce/restore_images/KLOADER.bin",
        help="Path to KLOADER.bin (default: ce/restore_images/KLOADER.bin).",
    )
    parser.add_argument(
        "--all-nand",
        default="ce/restore_images/All_nand_300.bin",
        help="Path to the NAND restore image (default: ce/restore_images/All_nand_300.bin).",
    )
    parser.add_argument(
        "--all-nand-name",
        default="All_nand.bin",
        help="Destination filename for the NAND restore image (default: All_nand.bin).",
    )
    parser.add_argument(
        "--file",
        action="append",
        default=[],
        metavar="DEST=SRC",
        help="Add an extra root-directory file.",
    )
    return parser.parse_args()


def read_input_file(host_path: pathlib.Path, dest_name: str) -> InputFile:
    data = host_path.read_bytes()
    return InputFile(host_path=host_path, dest_name=dest_name, data=data)


def parse_extra_file(spec: str) -> tuple[str, pathlib.Path]:
    if "=" not in spec:
        raise ValueError(f"Invalid --file entry {spec!r}; expected DEST=SRC")
    dest, src = spec.split("=", 1)
    dest = dest.strip()
    src = src.strip()
    if not dest or not src:
        raise ValueError(f"Invalid --file entry {spec!r}; expected DEST=SRC")
    return dest, pathlib.Path(src)


def fat_pad(text: str, length: int) -> bytes:
    cleaned = text.encode("ascii", "replace")[:length]
    return cleaned.ljust(length, b" ")


def dirent_pad(text: str, length: int, pad_byte: bytes) -> bytes:
    cleaned = text.encode("ascii", "replace")[:length]
    return cleaned.ljust(length, pad_byte)


def sectors_for_size(size_bytes: int) -> int:
    return (size_bytes + BYTES_PER_SECTOR - 1) // BYTES_PER_SECTOR


def root_dir_sectors() -> int:
    return (ROOT_ENTRY_COUNT * 32 + (BYTES_PER_SECTOR - 1)) // BYTES_PER_SECTOR


def choose_fat_layout(total_sectors: int) -> tuple[int, int, int]:
    root_sectors = root_dir_sectors()
    for sectors_per_cluster in (1, 2, 4, 8, 16, 32, 64, 128):
        fat_sectors = 1
        while True:
            data_sectors = total_sectors - RESERVED_SECTORS - (NUM_FATS * fat_sectors) - root_sectors
            if data_sectors <= 0:
                break
            clusters = data_sectors // sectors_per_cluster
            needed_fat_sectors = math.ceil(((clusters + 2) * 2) / BYTES_PER_SECTOR)
            if needed_fat_sectors == fat_sectors:
                break
            fat_sectors = needed_fat_sectors
        else:
            continue

        if data_sectors <= 0:
            continue
        clusters = data_sectors // sectors_per_cluster
        if FAT16_MIN_CLUSTERS <= clusters <= FAT16_MAX_CLUSTERS:
            return sectors_per_cluster, fat_sectors, clusters

    raise ValueError(
        f"Cannot format {total_sectors * BYTES_PER_SECTOR} bytes as FAT16 with the current layout"
    )


def is_valid_short_char(ch: str) -> bool:
    return ch.isalnum() or ch in "$%'-_@~`!(){}^#&"


def sanitize_short_component(text: str) -> str:
    upper = text.upper()
    return "".join(ch if is_valid_short_char(ch) else "_" for ch in upper)


def split_name(name: str) -> tuple[str, str]:
    if "/" in name or "\\" in name:
        raise ValueError(f"Destination name must be a root entry, got {name!r}")
    if name in {".", ".."}:
        raise ValueError(f"Invalid destination name: {name!r}")
    stem, ext = os.path.splitext(name)
    if not stem:
        raise ValueError(f"Destination name must have a basename: {name!r}")
    ext = ext[1:] if ext.startswith(".") else ext
    return stem, ext


def short_name_checksum(short_name: bytes) -> int:
    checksum = 0
    for byte in short_name:
        checksum = (((checksum & 1) << 7) | (checksum >> 1)) + byte
        checksum &= 0xFF
    return checksum


def make_short_name(name: str, used: set[bytes]) -> bytes:
    stem, ext = split_name(name)
    stem_short = sanitize_short_component(stem)
    ext_short = sanitize_short_component(ext)

    candidate = dirent_pad(stem_short[:8], 8, b"\x00") + dirent_pad(ext_short[:3], 3, b"\x00")
    exact_short = (
        stem_short == stem.upper()
        and ext_short == ext.upper()
        and 1 <= len(stem_short) <= 8
        and len(ext_short) <= 3
        and "." not in stem
    )
    if exact_short and candidate not in used:
        used.add(candidate)
        return candidate

    base = stem_short[:6] or "FILE"
    ext_part = ext_short[:3]
    for index in range(1, 100000):
        tail = f"~{index}"
        short_base = (base[: max(0, 8 - len(tail))] + tail)[:8]
        candidate = dirent_pad(short_base, 8, b"\x00") + dirent_pad(ext_part, 3, b"\x00")
        if candidate not in used:
            used.add(candidate)
            return candidate

    raise ValueError(f"Could not derive a unique short name for {name!r}")


def lfn_chunks(name: str) -> list[list[int]]:
    utf16 = name.encode("utf-16le")
    code_units = list(struct.unpack("<" + "H" * (len(utf16) // 2), utf16))
    code_units.append(0x0000)
    while len(code_units) % 13 != 0:
        code_units.append(0xFFFF)
    return [code_units[i : i + 13] for i in range(0, len(code_units), 13)]


def encode_lfn_entry(order: int, chunk: list[int], checksum: int) -> bytes:
    entry = bytearray(32)
    entry[0] = order
    entry[11] = 0x0F
    entry[12] = 0x00
    entry[13] = checksum
    entry[26:28] = b"\x00\x00"

    positions = (
        (1, 5),
        (14, 6),
        (28, 2),
    )
    chunk_offset = 0
    for start, count in positions:
        for i in range(count):
            value = chunk[chunk_offset + i]
            struct.pack_into("<H", entry, start + i * 2, value)
        chunk_offset += count
    return bytes(entry)


def encode_short_dirent(
    short_name: bytes,
    size_bytes: int,
    start_cluster: int,
    attr: int = 0x20,
) -> bytes:
    entry = bytearray(32)
    entry[0:11] = short_name
    entry[11] = attr
    struct.pack_into("<H", entry, 14, FIXED_FAT_TIME)
    struct.pack_into("<H", entry, 16, FIXED_FAT_DATE)
    struct.pack_into("<H", entry, 18, FIXED_FAT_DATE)
    struct.pack_into("<H", entry, 22, FIXED_FAT_TIME)
    struct.pack_into("<H", entry, 24, FIXED_FAT_DATE)
    struct.pack_into("<H", entry, 26, start_cluster & 0xFFFF)
    struct.pack_into("<I", entry, 28, size_bytes)
    return bytes(entry)


def decode_short_name(short_name: bytes) -> str:
    stem = short_name[:8].decode("ascii", "replace").rstrip(" \x00")
    ext = short_name[8:11].decode("ascii", "replace").rstrip(" \x00")
    return stem + (("." + ext) if ext else "")


def append_directory_entries(root_dir: bytearray, file_entry: InputFile) -> None:
    needs_lfn = file_entry.dest_name.upper() != decode_short_name(file_entry.short_name)
    if needs_lfn:
        checksum = short_name_checksum(file_entry.short_name)
        chunks = lfn_chunks(file_entry.dest_name)
        total = len(chunks)
        for index, chunk in enumerate(reversed(chunks), start=1):
            order = index
            if index == total:
                order |= 0x40
            root_dir.extend(encode_lfn_entry(order, chunk, checksum))
    root_dir.extend(
        encode_short_dirent(
            file_entry.short_name,
            len(file_entry.data),
            file_entry.start_cluster,
        )
    )


def cluster_count_for_bytes(size_bytes: int, cluster_size_bytes: int) -> int:
    if size_bytes == 0:
        return 0
    return (size_bytes + cluster_size_bytes - 1) // cluster_size_bytes


def build_boot_sector(
    total_sectors: int,
    sectors_per_cluster: int,
    fat_sectors: int,
    label: str,
) -> bytes:
    sector = bytearray(BYTES_PER_SECTOR)
    sector[0:3] = b"\xEB\x3C\x90"
    sector[3:11] = b"MSDOS5.0"
    struct.pack_into("<H", sector, 11, BYTES_PER_SECTOR)
    sector[13] = sectors_per_cluster
    struct.pack_into("<H", sector, 14, RESERVED_SECTORS)
    sector[16] = NUM_FATS
    struct.pack_into("<H", sector, 17, ROOT_ENTRY_COUNT)
    if total_sectors < 0x10000:
        struct.pack_into("<H", sector, 19, total_sectors)
        struct.pack_into("<I", sector, 32, 0)
    else:
        struct.pack_into("<H", sector, 19, 0)
        struct.pack_into("<I", sector, 32, total_sectors)
    sector[21] = MEDIA_DESCRIPTOR
    struct.pack_into("<H", sector, 22, fat_sectors)
    struct.pack_into("<H", sector, 24, 32)
    struct.pack_into("<H", sector, 26, 64)
    struct.pack_into("<I", sector, 28, 0)
    sector[36] = 0x80
    sector[37] = 0
    sector[38] = 0x29
    struct.pack_into("<I", sector, 39, DEFAULT_VOLUME_ID)
    sector[43:54] = fat_pad(label.upper(), 11)
    sector[54:62] = b"FAT16   "
    sector[510:512] = b"\x55\xAA"
    return bytes(sector)


def add_default_files(args: argparse.Namespace) -> list[InputFile]:
    files = [
        ("NANDWRITER.bin", pathlib.Path(args.nandwriter)),
        ("KLOADER.bin", pathlib.Path(args.kloader)),
        (args.all_nand_name, pathlib.Path(args.all_nand)),
    ]
    result = []
    for dest_name, host_path in files:
        result.append(read_input_file(host_path, dest_name))
    return result


def add_extra_files(files: list[InputFile], extra_specs: list[str]) -> None:
    for spec in extra_specs:
        dest_name, host_path = parse_extra_file(spec)
        files.append(read_input_file(host_path, dest_name))


def validate_files(files: list[InputFile]) -> None:
    seen = set()
    for entry in files:
        if not entry.host_path.is_file():
            raise FileNotFoundError(f"Missing input file: {entry.host_path}")
        if entry.dest_name.lower() in seen:
            raise ValueError(f"Duplicate destination filename: {entry.dest_name}")
        seen.add(entry.dest_name.lower())


def main() -> int:
    args = parse_args()
    output_path = pathlib.Path(args.output)

    if output_path.exists() and not args.force:
        print(f"Refusing to overwrite existing image: {output_path}", file=sys.stderr)
        return 1

    total_bytes = args.size_mib * 1024 * 1024
    if total_bytes < 24 * 1024 * 1024:
        print("CF image must be at least 24 MiB for the restore payload set", file=sys.stderr)
        return 1
    if total_bytes % BYTES_PER_SECTOR != 0:
        print("Image size must be a multiple of 512 bytes", file=sys.stderr)
        return 1

    try:
        files = add_default_files(args)
        add_extra_files(files, args.file)
        validate_files(files)
    except (FileNotFoundError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 1

    total_sectors = total_bytes // BYTES_PER_SECTOR
    try:
        sectors_per_cluster, fat_sectors, total_clusters = choose_fat_layout(total_sectors)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 1

    cluster_size_bytes = sectors_per_cluster * BYTES_PER_SECTOR
    root_sectors = root_dir_sectors()
    data_start_sector = RESERVED_SECTORS + (NUM_FATS * fat_sectors) + root_sectors

    next_cluster = 2
    for file_entry in files:
        cluster_count = cluster_count_for_bytes(len(file_entry.data), cluster_size_bytes)
        file_entry.start_cluster = next_cluster if cluster_count else 0
        next_cluster += cluster_count

    needed_clusters = next_cluster - 2
    if needed_clusters > total_clusters:
        print(
            f"Files need {needed_clusters} clusters but image only has {total_clusters}",
            file=sys.stderr,
        )
        return 1

    used_short_names: set[bytes] = set()
    for file_entry in files:
        file_entry.short_name = make_short_name(file_entry.dest_name, used_short_names)

    root_dir = bytearray()
    volume_label_short = fat_pad(args.volume_label.upper(), 11)
    root_dir.extend(encode_short_dirent(volume_label_short, 0, 0, attr=0x08))
    for file_entry in files:
        append_directory_entries(root_dir, file_entry)
    root_dir.extend(b"\x00" * 32)

    max_root_bytes = root_sectors * BYTES_PER_SECTOR
    if len(root_dir) > max_root_bytes:
        print("Root directory overflow; reduce file count or increase root entries", file=sys.stderr)
        return 1
    root_dir.extend(b"\x00" * (max_root_bytes - len(root_dir)))

    fat_entries = [0x0000] * (total_clusters + 2)
    fat_entries[0] = 0xFFF8
    fat_entries[1] = 0xFFFF
    for file_entry in files:
        cluster_count = cluster_count_for_bytes(len(file_entry.data), cluster_size_bytes)
        for cluster_index in range(cluster_count):
            cluster = file_entry.start_cluster + cluster_index
            if cluster_index == cluster_count - 1:
                fat_entries[cluster] = 0xFFFF
            else:
                fat_entries[cluster] = cluster + 1

    fat = bytearray(fat_sectors * BYTES_PER_SECTOR)
    for index, value in enumerate(fat_entries):
        struct.pack_into("<H", fat, index * 2, value)

    image = bytearray(total_bytes)
    image[0:BYTES_PER_SECTOR] = build_boot_sector(
        total_sectors=total_sectors,
        sectors_per_cluster=sectors_per_cluster,
        fat_sectors=fat_sectors,
        label=args.volume_label,
    )

    fat1_offset = RESERVED_SECTORS * BYTES_PER_SECTOR
    fat2_offset = fat1_offset + len(fat)
    image[fat1_offset : fat1_offset + len(fat)] = fat
    image[fat2_offset : fat2_offset + len(fat)] = fat

    root_offset = (RESERVED_SECTORS + NUM_FATS * fat_sectors) * BYTES_PER_SECTOR
    image[root_offset : root_offset + len(root_dir)] = root_dir

    for file_entry in files:
        cluster_count = cluster_count_for_bytes(len(file_entry.data), cluster_size_bytes)
        if cluster_count == 0:
            continue
        first_sector = data_start_sector + (file_entry.start_cluster - 2) * sectors_per_cluster
        file_offset = first_sector * BYTES_PER_SECTOR
        image[file_offset : file_offset + len(file_entry.data)] = file_entry.data

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(image)

    print(
        f"Wrote {output_path} ({total_bytes} bytes, FAT16, {sectors_per_cluster} sec/cluster, "
        f"{fat_sectors} FAT sectors)"
    )
    for file_entry in files:
        print(
            f"  {file_entry.dest_name} <- {file_entry.host_path} "
            f"(short={decode_short_name(file_entry.short_name)!r}, "
            f"cluster={file_entry.start_cluster}, size={len(file_entry.data)})"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
