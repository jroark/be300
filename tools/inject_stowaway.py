#!/usr/bin/env python3
"""Inject the Stowaway keyboard driver into a BE-300 NAND image.

The BE-300 restore image keeps the persistent user/system area as a FAT16
volume at NAND offset 0x3b4000.  This tool extracts the WinCE CAB payload,
copies the driver files under ``\\Nand Disk\\Program Files\\Patch``, stages the
boot-time stream driver into the NK XIP module table, and updates the compact
binary registry snapshot in ``\\Backup\\System.reg``.
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

try:
    from nk_lzss import decode_lzss, decode_nk_partition, encode_lzss, patch_logical_stream_from_flat
except ModuleNotFoundError:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from nk_lzss import decode_lzss, decode_nk_partition, encode_lzss, patch_logical_stream_from_flat


DEFAULT_NAND = Path("nand.bin")
DEFAULT_CAB = Path("Stowaway.PPC300_4000.cab")
DEFAULT_OUTPUT = Path("nand_stowaway.bin")
DEFAULT_FS_OFFSET = 0x3B4000

REG_ROOT_HKLM = 2
REG_SZ = 1
REG_DWORD = 4

NK_BASE = 0x80060000
PTOC_VA = 0x80655C54
TABLE_VA = 0x80655CA4
TOC_ENTRY_SIZE = 32
O32_ENTRY_SIZE = 24
E32_SIZE = 0x6C
O32_SINGLE_IMAGE_FLAGS = 0xE0000060
XIP_DRIVER_SLOT = "redir.dll"
XIP_DRIVER_CAB_NAME = "STOWAWAY.014"
XIP_DRIVER_MODULE_NAME = "Stowaway.dll"


PAYLOAD_FILES = {
    "STOWAW~1.001": "Stowaway.ESP.dll",
    "STOWAWAY.002": "Stowaway.exe",
    "STOWAW~2.003": "Stowaway.FRA.dll",
    "STOWAW~3.004": "Stowaway.FRC.dll",
    "STOWAW~4.005": "Stowaway.GER.dll",
    "STOWAW~5.006": "Stowaway.JPN.dll",
    "STOWAW~6.007": "Stowaway.USA.dll",
    "STOWAW~7.008": "stowawaysetup.dll",
    "STOWAWAY.009": "Stowaway.htp",
    "0KEYSOFT.010": "KeySoft.wav",
    "SOFTWA~1.011": "Software License Agreement.txt",
    "STOWAW~1.012": "Stowaway Default.reg",
    "0KEYLOUD.013": "KeyLoud.wav",
    "STOWAWAY.014": "Stowaway.dll",
}

STOWAWAY_KEY = r"Drivers\BuiltIn\Stowaway"
PATCH_DIR = "::/Program Files/Patch"
BACKUP_DIR = "::/Backup"

STOWAWAY_REG_VALUES: list[tuple[str, int, str | int]] = [
    ("Dll", REG_SZ, XIP_DRIVER_MODULE_NAME),
    ("Prefix", REG_SZ, "STO"),
    ("Index", REG_DWORD, 1),
    # Match the vendor CAB's boot-time BuiltIn load order.  The full CAB
    # payload remains on NAND Patch storage, but device.exe loads the stream
    # driver itself from the XIP slot because the FAT volume is too late for
    # BuiltIn driver enumeration.
    ("Order", REG_DWORD, 3),
    ("Keep", REG_DWORD, 1),
    ("Port", REG_DWORD, 1),
    ("Enabled", REG_DWORD, 1),
    ("HotDock", REG_DWORD, 0),
    ("KeyClick", REG_DWORD, 0),
    ("RepeatRate", REG_DWORD, 0x96),
    ("InitialDelay", REG_DWORD, 0x226),
    ("IdleDelay", REG_DWORD, 0x7D0),
    ("LampDelay", REG_DWORD, 0x3E8),
    ("HotKeyA1", REG_SZ, "pmail.exe"),
    ("HotKeyA2", REG_SZ, "addrbook.exe"),
    ("HotKeyA3", REG_SZ, "calendar.exe"),
    ("HotKeyA4", REG_SZ, "tasks.exe"),
    ("HotKeyA5", REG_SZ, "notes.exe"),
    ("HotKeyA6", REG_SZ, "pword.exe"),
    ("HotKeyA7", REG_SZ, "pxl.exe"),
    ("HotKeyA8", REG_SZ, "moneyce.exe"),
    ("HotKeyF1", REG_SZ, "Stowaway.exe"),
    ("HotKeyF2", REG_SZ, "Stowaway.exe"),
    ("HotKeyF3", REG_SZ, "Stowaway.exe"),
    ("HotKeyF4", REG_SZ, "Stowaway.exe"),
    ("HotKeyF5", REG_SZ, "Stowaway.exe"),
    ("HotKeyF6", REG_SZ, "Stowaway.exe"),
    ("HotKeyF7", REG_SZ, "Stowaway.exe"),
    ("HotKeyF8", REG_SZ, "Stowaway.exe"),
    ("HotKeyF9", REG_SZ, "Stowaway.exe"),
    ("HotKeyF0", REG_SZ, "Stowaway.exe"),
    ("HomeScreenApp", REG_SZ, r"\Windows\Menu.exe"),
    ("Window32768", REG_SZ, "DesktopExplorerWindow"),
]


@dataclass
class RegRecord:
    raw: bytes
    record_type: int
    root: int | None = None
    key: str | None = None
    value_name: str | None = None


@dataclass
class PeSection:
    name: str
    virtual_size: int
    virtual_address: int
    raw_size: int
    raw_ptr: int
    characteristics: int


@dataclass
class PeImage:
    data: bytes
    file_size: int
    characteristics: int
    entry_rva: int
    image_base: int
    major_subsystem: int
    minor_subsystem: int
    stack_reserve: int
    size_of_image: int
    directories: list[tuple[int, int]]
    sections: list[PeSection]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Inject the Targus/Think Outside Stowaway driver into a BE-300 NAND image."
    )
    parser.add_argument("--nand", default=str(DEFAULT_NAND), help="Source NAND image.")
    parser.add_argument("--cab", default=str(DEFAULT_CAB), help="Stowaway WinCE CAB.")
    parser.add_argument(
        "--output",
        default=str(DEFAULT_OUTPUT),
        help="Output NAND image. Ignored when --in-place is used.",
    )
    parser.add_argument(
        "--in-place",
        action="store_true",
        help="Modify --nand directly instead of writing an output copy.",
    )
    parser.add_argument(
        "--fs-offset",
        type=lambda value: int(value, 0),
        default=DEFAULT_FS_OFFSET,
        help="FAT16 filesystem offset inside NAND (default: 0x3b4000).",
    )
    parser.add_argument("--force", action="store_true", help="Overwrite output image.")
    return parser.parse_args()


def run(cmd: list[str], *, env: dict[str, str] | None = None) -> None:
    subprocess.run(cmd, check=True, env=env)


def require_tool(name: str) -> str:
    found = shutil.which(name)
    if not found:
        raise SystemExit(f"error: required tool not found in PATH: {name}")
    return found


def mtools_env() -> dict[str, str]:
    env = dict(os.environ)
    env["MTOOLS_SKIP_CHECK"] = "1"
    return env


def mtools_image_arg(image: Path, fs_offset: int) -> str:
    return f"{image}@@0x{fs_offset:x}"


def validate_nand(image: Path, fs_offset: int) -> None:
    data = image.read_bytes()
    if len(data) < fs_offset + 512:
        raise SystemExit(f"error: image too small for FAT offset 0x{fs_offset:x}: {image}")
    if data[0x4000:0x4007] != b"B000FF\n":
        raise SystemExit("error: NAND image does not have B000FF SPL signature at 0x4000")
    boot = data[fs_offset:fs_offset + 512]
    if boot[0:3] != b"\xeb\xfe\x90" or boot[54:62] != b"FAT16   ":
        raise SystemExit(f"error: no expected BE-300 FAT16 boot sector at 0x{fs_offset:x}")


def u16(data: bytes | bytearray, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0]


def u32(data: bytes | bytearray, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def parse_pe_image(path: Path) -> PeImage:
    data = path.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise ValueError(f"{path}: not an MZ executable")
    pe_off = u32(data, 0x3C)
    if pe_off + 0x18 > len(data) or data[pe_off:pe_off + 4] != b"PE\0\0":
        raise ValueError(f"{path}: missing PE signature")

    coff = pe_off + 4
    machine = u16(data, coff)
    if machine != 0x0166:
        raise ValueError(f"{path}: expected MIPS little-endian PE, got machine 0x{machine:04x}")

    section_count = u16(data, coff + 2)
    timestamp = u32(data, coff + 4)
    optional_size = u16(data, coff + 16)
    characteristics = u16(data, coff + 18)

    opt = coff + 20
    if opt + optional_size > len(data):
        raise ValueError(f"{path}: optional header extends beyond file")
    if u16(data, opt) != 0x010B:
        raise ValueError(f"{path}: expected PE32 optional header")

    entry_rva = u32(data, opt + 0x10)
    image_base = u32(data, opt + 0x1C)
    major_subsystem = u16(data, opt + 0x30)
    minor_subsystem = u16(data, opt + 0x32)
    size_of_image = u32(data, opt + 0x38)
    stack_reserve = u32(data, opt + 0x48)
    directory_count = u32(data, opt + 0x5C)

    directories: list[tuple[int, int]] = []
    dirs_off = opt + 0x60
    for i in range(9):
        if i < directory_count and dirs_off + i * 8 + 8 <= opt + optional_size:
            directories.append((u32(data, dirs_off + i * 8), u32(data, dirs_off + i * 8 + 4)))
        else:
            directories.append((0, 0))

    sections: list[PeSection] = []
    sec_off = opt + optional_size
    for i in range(section_count):
        off = sec_off + i * 40
        if off + 40 > len(data):
            raise ValueError(f"{path}: section table extends beyond file")
        raw_name = data[off:off + 8].split(b"\0", 1)[0]
        name = raw_name.decode("latin1", "replace")
        virtual_size = u32(data, off + 8)
        virtual_address = u32(data, off + 12)
        raw_size = u32(data, off + 16)
        raw_ptr = u32(data, off + 20)
        characteristics_sec = u32(data, off + 36)
        if raw_size and raw_ptr + raw_size > len(data):
            raise ValueError(f"{path}: section {name} raw data extends beyond file")
        sections.append(
            PeSection(
                name=name,
                virtual_size=virtual_size,
                virtual_address=virtual_address,
                raw_size=raw_size,
                raw_ptr=raw_ptr,
                characteristics=characteristics_sec,
            )
        )

    image = PeImage(
        data=data,
        file_size=len(data),
        characteristics=characteristics,
        entry_rva=entry_rva,
        image_base=image_base,
        major_subsystem=major_subsystem,
        minor_subsystem=minor_subsystem,
        stack_reserve=stack_reserve,
        size_of_image=size_of_image,
        directories=directories,
        sections=sections,
    )
    # Store the timestamp in the final E32 word only when the target image
    # already uses it there; current BE-300 XIP modules keep CPU metadata at
    # E32+0x68, so the timestamp is intentionally not copied.
    _ = timestamp
    return image


def pe_to_flat_image(pe: PeImage) -> bytes:
    flat = bytearray(pe.size_of_image)
    for section in pe.sections:
        if section.raw_size == 0:
            continue
        end = section.virtual_address + section.raw_size
        if end > len(flat):
            raise ValueError(f"PE section {section.name} exceeds SizeOfImage")
        flat[section.virtual_address:end] = pe.data[
            section.raw_ptr:section.raw_ptr + section.raw_size
        ]
    return bytes(flat)


def read_xip_cstr(data: bytes | bytearray, va: int) -> str:
    off = va - NK_BASE
    if off < 0 or off >= len(data):
        raise ValueError(f"XIP string VA out of range: 0x{va:08x}")
    end = data.find(b"\0", off)
    if end < 0:
        raise ValueError(f"unterminated XIP string at VA 0x{va:08x}")
    return data[off:end].decode("latin1", "replace")


def find_xip_module(nk: bytes | bytearray, module_name: str) -> tuple[int, int]:
    if read_xip_cstr(nk, u32(nk, TABLE_VA - NK_BASE + 0x14)) != "nk.exe":
        raise ValueError("XIP TOC sanity check failed")

    count = u32(nk, PTOC_VA - NK_BASE + 0x10)
    want = module_name.lower()
    for index in range(count):
        entry_off = TABLE_VA - NK_BASE + index * TOC_ENTRY_SIZE
        name_va = u32(nk, entry_off + 0x14)
        if read_xip_cstr(nk, name_va).lower() == want:
            return index, entry_off
    raise ValueError(f"XIP module slot not found: {module_name}")


def install_xip_pe_module(nk: bytearray, slot_name: str, module_name: str, pe_path: Path) -> None:
    pe = parse_pe_image(pe_path)
    flat = pe_to_flat_image(pe)
    _, entry_off = find_xip_module(nk, slot_name)
    e32_va = u32(nk, entry_off + 0x18)
    o32_va = u32(nk, entry_off + 0x1C)
    e32_off = e32_va - NK_BASE
    o32_off = o32_va - NK_BASE

    if e32_off < 0 or e32_off + E32_SIZE > len(nk):
        raise ValueError(f"{slot_name}: E32 metadata out of range")
    if o32_off < 0 or o32_off + O32_ENTRY_SIZE > len(nk):
        raise ValueError(f"{slot_name}: O32 metadata out of range")

    slot_vbase = u32(nk, e32_off + 0x08)
    storage_va = u32(nk, o32_off + 0x0C)
    storage_size = u32(nk, o32_off + 0x08)
    storage_off = storage_va - NK_BASE
    if storage_off < 0 or storage_off + storage_size > len(nk):
        raise ValueError(f"{slot_name}: section storage out of range")
    name_bytes = module_name.encode("ascii") + b"\0"
    name_off = align_up(pe.size_of_image, 4)
    required = name_off + len(name_bytes)
    if required > storage_size:
        raise ValueError(
            f"{slot_name}: replacement does not fit XIP storage "
            f"0x{required:x} > 0x{storage_size:x}"
        )

    nk[storage_off:storage_off + storage_size] = b"\0" * storage_size
    nk[storage_off:storage_off + pe.size_of_image] = flat
    nk[storage_off + name_off:storage_off + name_off + len(name_bytes)] = name_bytes

    name_va = storage_va + name_off
    struct.pack_into("<I", nk, entry_off + 0x10, pe.file_size)
    struct.pack_into("<I", nk, entry_off + 0x14, name_va)

    old_tail = bytes(nk[e32_off + 0x68:e32_off + E32_SIZE])
    e32 = bytearray(E32_SIZE)
    struct.pack_into("<HBB", e32, 0x00, 1, pe.characteristics & 0xFF, pe.characteristics >> 8)
    struct.pack_into("<I", e32, 0x04, pe.entry_rva)
    struct.pack_into("<I", e32, 0x08, slot_vbase)
    struct.pack_into("<HH", e32, 0x0C, pe.major_subsystem, pe.minor_subsystem)
    struct.pack_into("<I", e32, 0x10, pe.stack_reserve)
    struct.pack_into("<I", e32, 0x14, pe.size_of_image)
    struct.pack_into("<II", e32, 0x18, 0, 0)
    for i, (rva, size) in enumerate(pe.directories):
        struct.pack_into("<II", e32, 0x20 + i * 8, rva, size)
    e32[0x68:E32_SIZE] = old_tail
    nk[e32_off:e32_off + E32_SIZE] = e32

    struct.pack_into("<I", nk, o32_off + 0x00, pe.size_of_image)
    struct.pack_into("<I", nk, o32_off + 0x04, 0)
    struct.pack_into("<I", nk, o32_off + 0x08, pe.size_of_image)
    struct.pack_into("<I", nk, o32_off + 0x0C, storage_va)
    struct.pack_into("<I", nk, o32_off + 0x10, slot_vbase + 0x02000000)
    struct.pack_into("<I", nk, o32_off + 0x14, O32_SINGLE_IMAGE_FLAGS)


def update_xip_driver(image: Path, cab_dir: Path) -> None:
    driver_path = cab_dir / XIP_DRIVER_CAB_NAME
    if not driver_path.is_file():
        raise SystemExit(f"error: missing CAB payload for XIP driver: {XIP_DRIVER_CAB_NAME}")

    nand = image.read_bytes()
    parsed = decode_nk_partition(nand, partition_index=2)
    nk = bytearray(parsed.flat_image)
    install_xip_pe_module(nk, XIP_DRIVER_SLOT, XIP_DRIVER_MODULE_NAME, driver_path)

    replacement_logical = patch_logical_stream_from_flat(parsed, bytes(nk))
    replacement_raw = encode_lzss(replacement_logical)
    if len(replacement_raw) > parsed.partition.size_bytes:
        raise SystemExit(
            f"error: repacked NK does not fit partition: "
            f"0x{len(replacement_raw):x} > 0x{parsed.partition.size_bytes:x}"
        )

    roundtrip_logical, raw_consumed = decode_lzss(
        replacement_raw,
        output_limit=len(replacement_logical),
    )
    if roundtrip_logical != replacement_logical:
        raise SystemExit("error: NK LZSS roundtrip verification failed")

    out = bytearray(nand)
    part_start = parsed.partition.offset
    out[part_start:part_start + len(replacement_raw)] = replacement_raw
    out[part_start + len(replacement_raw):part_start + parsed.partition.size_bytes] = (
        b"\0" * (parsed.partition.size_bytes - len(replacement_raw))
    )
    image.write_bytes(out)
    print(
        f"  xip: {XIP_DRIVER_MODULE_NAME} from {XIP_DRIVER_SLOT} "
        f"(raw=0x{len(replacement_raw):x}, consumed=0x{raw_consumed:x})"
    )


def extract_cab(cab: Path, out_dir: Path) -> None:
    cabextract = require_tool("cabextract")
    run([cabextract, "-d", str(out_dir), str(cab)])
    missing = [name for name in PAYLOAD_FILES if not (out_dir / name).is_file()]
    if missing:
        raise SystemExit("error: CAB extraction missed payload files: " + ", ".join(missing))


def ensure_patch_dir(image: Path, fs_offset: int) -> None:
    mmd = require_tool("mmd")
    env = mtools_env()
    img = mtools_image_arg(image, fs_offset)
    try:
        run([mmd, "-i", img, PATCH_DIR], env=env)
    except subprocess.CalledProcessError:
        # Existing directory is fine; mtools returns non-zero for that case.
        pass


def copy_payload_files(image: Path, fs_offset: int, cab_dir: Path) -> None:
    mcopy = require_tool("mcopy")
    env = mtools_env()
    img = mtools_image_arg(image, fs_offset)
    for source_name, dest_name in sorted(PAYLOAD_FILES.items(), key=lambda item: item[1].lower()):
        source = cab_dir / source_name
        dest = f"{PATCH_DIR}/{dest_name}"
        run([mcopy, "-o", "-i", img, str(source), dest], env=env)


def copy_from_image(image: Path, fs_offset: int, guest_path: str, host_path: Path) -> None:
    mcopy = require_tool("mcopy")
    run(
        [mcopy, "-o", "-i", mtools_image_arg(image, fs_offset), guest_path, str(host_path)],
        env=mtools_env(),
    )


def copy_to_image(image: Path, fs_offset: int, host_path: Path, guest_path: str) -> None:
    mcopy = require_tool("mcopy")
    run(
        [mcopy, "-o", "-i", mtools_image_arg(image, fs_offset), str(host_path), guest_path],
        env=mtools_env(),
    )


def decode_utf16z(data: bytes) -> str:
    return data.decode("utf-16le", "replace").rstrip("\x00")


def encode_utf16z(text: str) -> bytes:
    return (text + "\x00").encode("utf-16le")


def parse_reg_records(data: bytes) -> list[RegRecord]:
    if len(data) < 8:
        raise ValueError("registry stream is too short")
    if struct.unpack_from("<I", data, 4)[0] != len(data):
        raise ValueError("registry size header does not match file length")

    records: list[RegRecord] = []
    pos = 8
    current_root: int | None = None
    current_key: str | None = None
    while pos < len(data):
        if pos + 4 > len(data):
            raise ValueError(f"truncated registry record header at 0x{pos:x}")
        payload_len, record_type = struct.unpack_from("<HH", data, pos)
        end = pos + 4 + payload_len
        if payload_len == 0 or end > len(data):
            raise ValueError(f"bad registry record length at 0x{pos:x}")
        raw = data[pos:end]

        if record_type == 1:
            root = struct.unpack_from("<H", raw, 4)[0]
            char_count = struct.unpack_from("<I", raw, 6)[0]
            key = decode_utf16z(raw[10:10 + char_count * 2])
            current_root = root
            current_key = key
            records.append(RegRecord(raw=raw, record_type=record_type, root=root, key=key))
        elif record_type == 2:
            name_chars = struct.unpack_from("<H", raw, 6)[0]
            name = decode_utf16z(raw[10:10 + name_chars * 2])
            records.append(
                RegRecord(
                    raw=raw,
                    record_type=record_type,
                    root=current_root,
                    key=current_key,
                    value_name=name,
                )
            )
        else:
            raise ValueError(f"unknown registry record type {record_type} at 0x{pos:x}")
        pos = end
    return records


def encode_key_record(root: int, key: str) -> bytes:
    key_bytes = encode_utf16z(key)
    char_count = len(key_bytes) // 2
    payload_len = 2 + 4 + len(key_bytes)
    return struct.pack("<HHHI", payload_len, 1, root, char_count) + key_bytes


def encode_value_record(name: str, reg_type: int, value: str | int) -> bytes:
    name_bytes = encode_utf16z(name)
    if reg_type == REG_SZ:
        if not isinstance(value, str):
            raise TypeError(f"{name}: REG_SZ value must be str")
        data = encode_utf16z(value)
    elif reg_type == REG_DWORD:
        if not isinstance(value, int):
            raise TypeError(f"{name}: REG_DWORD value must be int")
        data = struct.pack("<I", value)
    else:
        raise ValueError(f"unsupported registry type: {reg_type}")

    payload_len = 2 + 2 + 2 + len(name_bytes) + len(data)
    return (
        struct.pack("<HHHHH", payload_len, 2, reg_type, len(name_bytes) // 2, len(data))
        + name_bytes
        + data
    )


def update_stowaway_registry(data: bytes) -> bytes:
    records = parse_reg_records(data)
    kept: list[RegRecord] = []
    insert_after = -1
    for rec in records:
        if rec.root == REG_ROOT_HKLM and rec.key == STOWAWAY_KEY:
            continue
        if rec.root == REG_ROOT_HKLM and rec.key is not None:
            if rec.key == r"Drivers\BuiltIn" or rec.key.startswith("Drivers\\BuiltIn\\"):
                insert_after = len(kept)
        kept.append(rec)

    stowaway_records = [encode_key_record(REG_ROOT_HKLM, STOWAWAY_KEY)]
    for name, reg_type, value in STOWAWAY_REG_VALUES:
        stowaway_records.append(encode_value_record(name, reg_type, value))

    if insert_after < 0:
        insert_after = len(kept)

    body = bytearray()
    for index, rec in enumerate(kept):
        body.extend(rec.raw)
        if index == insert_after:
            for encoded in stowaway_records:
                body.extend(encoded)
    if insert_after == len(kept):
        for encoded in stowaway_records:
            body.extend(encoded)

    out = bytearray(data[:8])
    out.extend(body)
    struct.pack_into("<I", out, 4, len(out))
    return bytes(out)


def update_registry_files(image: Path, fs_offset: int, work_dir: Path) -> None:
    system_reg = work_dir / "System.reg"
    copy_from_image(image, fs_offset, f"{BACKUP_DIR}/System.reg", system_reg)
    updated = update_stowaway_registry(system_reg.read_bytes())
    system_reg.write_bytes(updated)
    copy_to_image(image, fs_offset, system_reg, f"{BACKUP_DIR}/System.reg")
    copy_to_image(image, fs_offset, system_reg, f"{BACKUP_DIR}/System.$$$")


def main() -> int:
    args = parse_args()
    source = Path(args.nand)
    cab = Path(args.cab)
    output = source if args.in_place else Path(args.output)

    if not source.is_file():
        print(f"error: source NAND image not found: {source}", file=sys.stderr)
        return 1
    if not cab.is_file():
        print(f"error: CAB not found: {cab}", file=sys.stderr)
        return 1
    if not args.in_place:
        if output.exists() and not args.force:
            print(f"error: refusing to overwrite existing output: {output}", file=sys.stderr)
            return 1
        shutil.copyfile(source, output)

    validate_nand(output, args.fs_offset)

    with tempfile.TemporaryDirectory(prefix="be300-stowaway-") as tmp:
        tmpdir = Path(tmp)
        cab_dir = tmpdir / "cab"
        cab_dir.mkdir()
        extract_cab(cab, cab_dir)
        update_xip_driver(output, cab_dir)
        ensure_patch_dir(output, args.fs_offset)
        copy_payload_files(output, args.fs_offset, cab_dir)
        update_registry_files(output, args.fs_offset, tmpdir)

    print(f"Wrote Stowaway-enabled NAND image: {output}")
    print(r"  payload: \Nand Disk\Program Files\Patch")
    print(f"  xip driver: {XIP_DRIVER_MODULE_NAME}")
    print(r"  registry: HKLM\Drivers\BuiltIn\Stowaway")
    return 0


if __name__ == "__main__":
    sys.exit(main())
