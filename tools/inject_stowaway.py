#!/usr/bin/env python3
"""Inject the Stowaway keyboard driver into a BE-300 NAND image.

The BE-300 restore image keeps the persistent user/system area as a FAT16
volume at NAND offset 0x3b4000.  This tool extracts the WinCE CAB payload,
copies the driver files under ``\\Nand Disk\\Program Files\\Patch``, and
updates the compact binary registry snapshot in ``\\Backup\\System.reg``.
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


DEFAULT_NAND = Path("nand.bin")
DEFAULT_CAB = Path("Stowaway.PPC300_4000.cab")
DEFAULT_OUTPUT = Path("nand_stowaway.bin")
DEFAULT_FS_OFFSET = 0x3B4000

REG_ROOT_HKLM = 2
REG_SZ = 1
REG_DWORD = 4


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
    ("Dll", REG_SZ, r"\Nand Disk\Program Files\Patch\Stowaway.dll"),
    ("Prefix", REG_SZ, "STO"),
    ("Index", REG_DWORD, 1),
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
    for rec in records:
        if rec.root == REG_ROOT_HKLM and rec.key == STOWAWAY_KEY:
            continue
        kept.append(rec)

    body = bytearray()
    for rec in kept:
        body.extend(rec.raw)
    body.extend(encode_key_record(REG_ROOT_HKLM, STOWAWAY_KEY))
    for name, reg_type, value in STOWAWAY_REG_VALUES:
        body.extend(encode_value_record(name, reg_type, value))

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
        ensure_patch_dir(output, args.fs_offset)
        copy_payload_files(output, args.fs_offset, cab_dir)
        update_registry_files(output, args.fs_offset, tmpdir)

    print(f"Wrote Stowaway-enabled NAND image: {output}")
    print(r"  payload: \Nand Disk\Program Files\Patch")
    print(r"  registry: HKLM\Drivers\BuiltIn\Stowaway")
    return 0


if __name__ == "__main__":
    sys.exit(main())
