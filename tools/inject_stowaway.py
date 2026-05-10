#!/usr/bin/env python3
"""Inject the Stowaway keyboard driver into a BE-300 NAND image.

This tool extracts the WinCE CAB payload, copies the driver files under
``\\Nand Disk\\Program Files\\Patch``, stages the boot-time stream driver into
the NK XIP module table, and updates or creates the compact binary registry
snapshot in ``\\Backup\\System.reg``.
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
LEGACY_FS_OFFSET = 0x3B4000
SECTOR_SIZE = 512

REG_ROOT_HKLM = 2
REG_SZ = 1
REG_DWORD = 4
COMPACT_REGISTRY_MAGIC = b"\xb2\x74\x83\x1d"

TOC_ENTRY_SIZE = 32
O32_ENTRY_SIZE = 24
E32_SIZE = 0x6C
MAX_XIP_MODULES = 256
O32_SINGLE_IMAGE_FLAGS = 0xE0000060
XIP_DRIVER_SLOT_CANDIDATES = ("redir.dll", "ddhel.dll", "waveapi.dll")
XIP_DRIVER_CAB_NAME = "STOWAWAY.014"
XIP_DRIVER_MODULE_NAME = "Stowaway.dll"
NET_NK_BASE = 0x80029000
NET_STOWAWAY_PREFERRED_BASE = 0x00100000
NET_STOWAWAY_API_READY_RVA = 0x3B34
NET_STOWAWAY_API_READY_ORIG = (0x0C04135F, 0x24040011)
NET_STOWAWAY_API_READY_PATCH = (0x24020001, 0x00000000)
NET_BOOT_HOOK_STORAGE_VA = 0x800E2C10
NET_BOOT_HOOK_STORAGE_END_VA = 0x800E30E4
NET_BOOT_HOOK_RUNTIME_VA = 0x03FCEC10


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

# The Net image does not reliably consume the persisted compact registry early
# enough for this driver.  A tiny boot.exe hook writes the minimum runtime key
# and activates the XIP-resident driver after the system APIs are up.
NET_STOWAWAY_RUNTIME_REG_VALUES: list[tuple[str, int, str | int]] = [
    ("Dll", REG_SZ, r"\Windows\Stowaway.dll"),
    ("Prefix", REG_SZ, "STO"),
    ("Index", REG_DWORD, 1),
    ("Order", REG_DWORD, 0),
    ("Keep", REG_DWORD, 1),
    ("Port", REG_DWORD, 1),
    ("Enabled", REG_DWORD, 1),
    ("HotDock", REG_DWORD, 0),
    ("KeyClick", REG_DWORD, 0),
    ("KeyLoud", REG_DWORD, 0),
    ("RepeatRate", REG_DWORD, 0x96),
    ("InitialDelay", REG_DWORD, 0x226),
    ("IdleDelay", REG_DWORD, 0x7D0),
    ("LampDelay", REG_DWORD, 0x3E8),
    ("RunawayCount", REG_DWORD, 0),
    ("RunawayPing", REG_DWORD, 0),
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


@dataclass(frozen=True)
class NandPartition:
    index: int
    start_sector: int
    sector_count: int

    @property
    def offset(self) -> int:
        return self.start_sector * SECTOR_SIZE

    @property
    def size_bytes(self) -> int:
        return self.sector_count * SECTOR_SIZE

    @property
    def end_offset(self) -> int:
        return self.offset + self.size_bytes


@dataclass(frozen=True)
class Fat16Volume:
    offset: int
    bytes_per_sector: int
    sectors_per_cluster: int
    reserved_sectors: int
    fat_count: int
    root_entry_count: int
    total_sectors: int
    sectors_per_fat: int
    volume_label: str

    @property
    def end_offset(self) -> int:
        return self.offset + self.total_sectors * self.bytes_per_sector

    @property
    def fat_offset(self) -> int:
        return self.offset + self.reserved_sectors * self.bytes_per_sector

    @property
    def root_dir_offset(self) -> int:
        return self.fat_offset + self.fat_count * self.sectors_per_fat * self.bytes_per_sector

    @property
    def root_dir_size(self) -> int:
        return self.root_entry_count * 32

    @property
    def root_dir_sectors(self) -> int:
        return (self.root_dir_size + self.bytes_per_sector - 1) // self.bytes_per_sector

    @property
    def data_offset(self) -> int:
        return self.root_dir_offset + self.root_dir_sectors * self.bytes_per_sector

    @property
    def cluster_size(self) -> int:
        return self.bytes_per_sector * self.sectors_per_cluster

    @property
    def total_clusters(self) -> int:
        data_bytes = self.end_offset - self.data_offset
        return max(0, data_bytes // self.cluster_size)

    @classmethod
    def from_boot_sector(cls, image: bytes, offset: int) -> "Fat16Volume":
        if offset < 0 or offset + SECTOR_SIZE > len(image):
            raise ValueError("boot sector is out of range")
        boot = image[offset:offset + SECTOR_SIZE]
        if boot[510:512] != b"\x55\xaa":
            raise ValueError("missing FAT boot signature")
        if boot[0x36:0x3E] != b"FAT16   ":
            raise ValueError("not a FAT16 boot sector")

        bytes_per_sector = u16(boot, 0x0B)
        sectors_per_cluster = boot[0x0D]
        reserved_sectors = u16(boot, 0x0E)
        fat_count = boot[0x10]
        root_entry_count = u16(boot, 0x11)
        total_sectors = u16(boot, 0x13) or u32(boot, 0x20)
        sectors_per_fat = u16(boot, 0x16)
        volume_label = boot[0x2B:0x36].decode("ascii", "replace").strip()

        if bytes_per_sector not in (512, 1024, 2048, 4096):
            raise ValueError(f"unsupported sector size {bytes_per_sector}")
        if sectors_per_cluster == 0 or sectors_per_cluster & (sectors_per_cluster - 1):
            raise ValueError(f"invalid sectors-per-cluster {sectors_per_cluster}")
        if fat_count not in (1, 2):
            raise ValueError(f"invalid FAT count {fat_count}")
        if root_entry_count == 0 or root_entry_count % 16:
            raise ValueError(f"invalid root entry count {root_entry_count}")
        if reserved_sectors == 0 or sectors_per_fat == 0 or total_sectors == 0:
            raise ValueError("invalid FAT geometry")
        if offset + total_sectors * bytes_per_sector > len(image):
            raise ValueError("FAT volume exceeds image size")

        volume = cls(
            offset=offset,
            bytes_per_sector=bytes_per_sector,
            sectors_per_cluster=sectors_per_cluster,
            reserved_sectors=reserved_sectors,
            fat_count=fat_count,
            root_entry_count=root_entry_count,
            total_sectors=total_sectors,
            sectors_per_fat=sectors_per_fat,
            volume_label=volume_label,
        )
        if not (4085 <= volume.total_clusters <= 65524):
            raise ValueError("FAT geometry is not FAT16-sized")
        return volume


@dataclass(frozen=True)
class XipToc:
    base_va: int
    table_off: int
    module_count: int

    @property
    def table_va(self) -> int:
        return self.base_va + self.table_off


@dataclass(frozen=True)
class XipSection:
    rva: int
    virtual_size: int
    physical_size: int
    data_va: int
    flags: int
    o32_off: int


@dataclass(frozen=True)
class XipModuleInfo:
    name: str
    entry_off: int
    e32_off: int
    o32_off: int
    vbase: int
    directories: list[tuple[int, int]]
    sections: list[XipSection]


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
        default=None,
        help=(
            "FAT16 filesystem offset inside NAND. "
            "Defaults to auto-detection; CE 3.0 images usually use 0x3b4000."
        ),
    )
    parser.add_argument(
        "--xip-slot",
        default=None,
        help=(
            "XIP module slot to replace. Defaults to the first compatible slot "
            f"from {', '.join(XIP_DRIVER_SLOT_CANDIDATES)}."
        ),
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


def parse_nand_partitions(data: bytes, max_entries: int = 16) -> list[NandPartition]:
    partitions: list[NandPartition] = []
    for index in range(max_entries):
        off = index * 16
        if off + 16 > len(data):
            break
        marker = data[off:off + 8]
        start_sector = u32(data, off + 8)
        sector_count = u32(data, off + 12)
        if marker != b"\xff" * 8:
            continue
        if start_sector in (0, 0xFFFFFFFF) and sector_count in (0, 0xFFFFFFFF):
            continue
        if sector_count == 0 or sector_count == 0xFFFFFFFF:
            continue
        part = NandPartition(index, start_sector, sector_count)
        if part.end_offset > len(data):
            continue
        partitions.append(part)
    return partitions


def find_all(data: bytes, needle: bytes) -> list[int]:
    found: list[int] = []
    off = data.find(needle)
    while off >= 0:
        found.append(off)
        off = data.find(needle, off + 1)
    return found


def containing_partition(volume: Fat16Volume, partitions: list[NandPartition]) -> NandPartition | None:
    matches = [
        part
        for part in partitions
        if part.offset <= volume.offset and volume.end_offset <= part.end_offset
    ]
    if not matches:
        return None
    return max(matches, key=lambda part: part.index)


def find_fat16_volumes(data: bytes, partitions: list[NandPartition]) -> list[Fat16Volume]:
    candidates = {part.offset for part in partitions}
    candidates.update(off - 0x36 for off in find_all(data, b"FAT16   ") if off >= 0x36)

    volumes: list[Fat16Volume] = []
    seen: set[int] = set()
    for offset in sorted(candidates):
        if offset in seen:
            continue
        try:
            volume = Fat16Volume.from_boot_sector(data, offset)
        except ValueError:
            continue
        volumes.append(volume)
        seen.add(offset)
    return volumes


def choose_fat16_volume(
    volumes: list[Fat16Volume],
    partitions: list[NandPartition],
) -> Fat16Volume:
    if not volumes:
        raise SystemExit("error: no FAT16 volume found in NAND image")

    def score(volume: Fat16Volume) -> tuple[int, int, int, int]:
        part = containing_partition(volume, partitions)
        in_partition = 1 if part is not None else 0
        part_index = part.index if part is not None else -1
        legacy = 1 if volume.offset == LEGACY_FS_OFFSET else 0
        return (in_partition, part_index, legacy, volume.total_sectors)

    return max(volumes, key=score)


def validate_nand(image: Path, fs_offset: int | None) -> tuple[Fat16Volume, list[NandPartition]]:
    data = image.read_bytes()
    if data[0x4000:0x4007] != b"B000FF\n":
        raise SystemExit("error: NAND image does not have B000FF SPL signature at 0x4000")
    partitions = parse_nand_partitions(data)
    if fs_offset is not None:
        try:
            return Fat16Volume.from_boot_sector(data, fs_offset), partitions
        except ValueError as exc:
            raise SystemExit(f"error: no valid FAT16 boot sector at 0x{fs_offset:x}: {exc}")

    volumes = find_fat16_volumes(data, partitions)
    return choose_fat16_volume(volumes, partitions), partitions


def u16(data: bytes | bytearray, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0]


def u32(data: bytes | bytearray, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


ZERO = 0
V0 = 2
A0 = 4
A1 = 5
A2 = 6
A3 = 7
T0 = 8
T9 = 25
S0 = 16
S1 = 17
SP = 29
RA = 31


def mips_r_type(rs: int, rt: int, rd: int, shamt: int, funct: int) -> int:
    return (rs << 21) | (rt << 16) | (rd << 11) | (shamt << 6) | funct


def mips_i_type(op: int, rs: int, rt: int, imm: int) -> int:
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def addiu(rt: int, rs: int, imm: int) -> int:
    return mips_i_type(9, rs, rt, imm)


def ori(rt: int, rs: int, imm: int) -> int:
    return mips_i_type(13, rs, rt, imm)


def lui(rt: int, imm: int) -> int:
    return mips_i_type(15, 0, rt, imm)


def sw(rt: int, off: int, base: int) -> int:
    return mips_i_type(43, base, rt, off)


def lw(rt: int, off: int, base: int) -> int:
    return mips_i_type(35, base, rt, off)


def addu(rd: int, rs: int, rt: int) -> int:
    return mips_r_type(rs, rt, rd, 0, 33)


def jalr(rs: int) -> int:
    return mips_r_type(rs, 0, RA, 0, 9)


def jr(rs: int) -> int:
    return mips_r_type(rs, 0, 0, 0, 8)


def nop() -> int:
    return 0


def beq(rs: int, rt: int, off: int) -> int:
    return mips_i_type(4, rs, rt, off)


def load_abs(rt: int, addr: int) -> list[int]:
    return [lui(rt, (addr >> 16) & 0xFFFF), ori(rt, rt, addr & 0xFFFF)]


def call_abs(addr: int) -> list[int]:
    return load_abs(T9, addr) + [jalr(T9), nop()]


def pack_words(words: list[int]) -> bytes:
    return b"".join(struct.pack("<I", word) for word in words)


class MipsAsm:
    def __init__(self) -> None:
        self.words: list[int] = []
        self.labels: dict[str, int] = {}
        self.fixups: list[tuple[int, str]] = []

    def emit(self, *words: int) -> None:
        self.words.extend(words)

    def label(self, name: str) -> None:
        self.labels[name] = len(self.words)

    def beq_label(self, rs: int, rt: int, label: str) -> None:
        self.fixups.append((len(self.words), label))
        self.words.append(beq(rs, rt, 0))

    def b_label(self, label: str) -> None:
        self.beq_label(ZERO, ZERO, label)

    def finish(self) -> bytes:
        for index, label in self.fixups:
            if label not in self.labels:
                raise ValueError(f"undefined MIPS label: {label}")
            offset = self.labels[label] - (index + 1)
            if not -32768 <= offset <= 32767:
                raise ValueError(f"MIPS branch to {label} is out of range")
            self.words[index] = (self.words[index] & 0xFFFF0000) | (offset & 0xFFFF)
        return pack_words(self.words)


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


def va_to_off(va: int, base_va: int) -> int:
    return va - base_va


def va_in_image(va: int, base_va: int, data: bytes | bytearray, size: int = 1) -> bool:
    off = va_to_off(va, base_va)
    return 0 <= off <= len(data) - size


def read_xip_cstr(
    data: bytes | bytearray,
    va: int,
    base_va: int,
    max_len: int = 128,
) -> str | None:
    if not va_in_image(va, base_va, data):
        return None
    off = va_to_off(va, base_va)
    end = data.find(b"\0", off, min(len(data), off + max_len))
    if end < 0:
        return None
    raw = data[off:end]
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError:
        return None
    if any(ord(ch) < 0x20 or ord(ch) > 0x7E for ch in text):
        return None
    return text


def xip_section_entries_valid(
    data: bytes | bytearray,
    base_va: int,
    o32_va: int,
    objcnt: int,
) -> bool:
    for i in range(objcnt):
        o32_off = va_to_off(o32_va + i * O32_ENTRY_SIZE, base_va)
        if o32_off < 0 or o32_off + O32_ENTRY_SIZE > len(data):
            return False
        vsize = u32(data, o32_off + 0x00)
        rva = u32(data, o32_off + 0x04)
        psize = u32(data, o32_off + 0x08)
        data_ptr = u32(data, o32_off + 0x0C)
        flags = u32(data, o32_off + 0x14)
        if rva > 0x04000000 or vsize > 0x04000000 or psize > 0x04000000:
            return False
        if psize and data_ptr and not (
            va_in_image(data_ptr, base_va, data, min(psize, 1))
            or data_ptr >= 0x02000000
        ):
            return False
        if flags == 0:
            return False
    return True


def parse_xip_module_name(
    data: bytes | bytearray,
    base_va: int,
    entry_off: int,
) -> str | None:
    if entry_off < 0 or entry_off + TOC_ENTRY_SIZE > len(data):
        return None

    name_va = u32(data, entry_off + 0x14)
    e32_va = u32(data, entry_off + 0x18)
    o32_va = u32(data, entry_off + 0x1C)
    name = read_xip_cstr(data, name_va, base_va)
    if not name or len(name) > 64:
        return None
    if not va_in_image(e32_va, base_va, data, 0x18):
        return None
    if not va_in_image(o32_va, base_va, data, O32_ENTRY_SIZE):
        return None

    e32_off = va_to_off(e32_va, base_va)
    objcnt = u16(data, e32_off + 0x00)
    vbase = u32(data, e32_off + 0x08)
    vsize = u32(data, e32_off + 0x14)
    if objcnt == 0 or objcnt > 32 or vbase == 0 or vsize == 0 or vsize > 0x04000000:
        return None
    if not xip_section_entries_valid(data, base_va, o32_va, objcnt):
        return None
    return name


def count_xip_modules_at(
    data: bytes | bytearray,
    base_va: int,
    table_off: int,
    limit: int = MAX_XIP_MODULES,
) -> XipToc | None:
    modules = 0
    for index in range(limit):
        entry_off = table_off + index * TOC_ENTRY_SIZE
        name = parse_xip_module_name(data, base_va, entry_off)
        if name is None:
            break
        if index == 0 and name != "nk.exe":
            return None
        modules += 1
    if modules == 0:
        return None
    return XipToc(base_va=base_va, table_off=table_off, module_count=modules)


def find_xip_toc(data: bytes | bytearray, base_va: int) -> XipToc:
    candidates: list[XipToc] = []
    for off in range(0, len(data) - TOC_ENTRY_SIZE, 4):
        name_va = u32(data, off + 0x14)
        if read_xip_cstr(data, name_va, base_va, max_len=16) != "nk.exe":
            continue
        toc = count_xip_modules_at(data, base_va, off)
        if toc is not None and toc.module_count > 8:
            candidates.append(toc)
    if not candidates:
        raise ValueError("could not locate XIP module table")
    return max(candidates, key=lambda toc: toc.module_count)


def find_xip_module(
    nk: bytes | bytearray,
    base_va: int,
    module_name: str,
) -> tuple[XipToc, int, int]:
    toc = find_xip_toc(nk, base_va)
    want = module_name.lower()
    for index in range(toc.module_count):
        entry_off = toc.table_off + index * TOC_ENTRY_SIZE
        name_va = u32(nk, entry_off + 0x14)
        name = read_xip_cstr(nk, name_va, base_va)
        if name is not None and name.lower() == want:
            return toc, index, entry_off
    raise ValueError(f"XIP module slot not found: {module_name}")


def get_xip_module_info(
    nk: bytes | bytearray,
    base_va: int,
    module_name: str,
) -> XipModuleInfo:
    _toc, _index, entry_off = find_xip_module(nk, base_va, module_name)
    e32_off = va_to_off(u32(nk, entry_off + 0x18), base_va)
    o32_off = va_to_off(u32(nk, entry_off + 0x1C), base_va)
    if e32_off < 0 or e32_off + E32_SIZE > len(nk):
        raise ValueError(f"{module_name}: E32 metadata out of range")
    if o32_off < 0 or o32_off + O32_ENTRY_SIZE > len(nk):
        raise ValueError(f"{module_name}: O32 metadata out of range")

    objcnt = u16(nk, e32_off + 0x00)
    vbase = u32(nk, e32_off + 0x08)
    directories = [
        (u32(nk, e32_off + 0x20 + i * 8), u32(nk, e32_off + 0x24 + i * 8))
        for i in range(9)
    ]
    sections: list[XipSection] = []
    for index in range(objcnt):
        section_off = o32_off + index * O32_ENTRY_SIZE
        if section_off < 0 or section_off + O32_ENTRY_SIZE > len(nk):
            raise ValueError(f"{module_name}: O32 section {index} out of range")
        sections.append(
            XipSection(
                rva=u32(nk, section_off + 0x04),
                virtual_size=u32(nk, section_off + 0x00),
                physical_size=u32(nk, section_off + 0x08),
                data_va=u32(nk, section_off + 0x0C),
                flags=u32(nk, section_off + 0x14),
                o32_off=section_off,
            )
        )
    return XipModuleInfo(
        name=module_name,
        entry_off=entry_off,
        e32_off=e32_off,
        o32_off=o32_off,
        vbase=vbase,
        directories=directories,
        sections=sections,
    )


def xip_rva_to_storage_off(
    module: XipModuleInfo,
    base_va: int,
    rva: int,
) -> int:
    for section in module.sections:
        section_size = max(section.virtual_size, section.physical_size)
        if section.rva <= rva < section.rva + section_size:
            off = va_to_off(section.data_va + rva - section.rva, base_va)
            if off < 0:
                raise ValueError(f"{module.name}: RVA 0x{rva:x} maps before NK image")
            return off
    raise ValueError(f"{module.name}: RVA 0x{rva:x} is outside stored sections")


def xip_exports(
    nk: bytes | bytearray,
    base_va: int,
    module_name: str,
) -> dict[str, int]:
    module = get_xip_module_info(nk, base_va, module_name)
    export_rva, export_size = module.directories[0]
    if export_rva == 0 or export_size == 0:
        raise ValueError(f"{module_name}: no export directory")
    export_off = xip_rva_to_storage_off(module, base_va, export_rva)
    if export_off + 40 > len(nk):
        raise ValueError(f"{module_name}: export directory out of range")

    (
        _characteristics,
        _timestamp,
        _major,
        _minor,
        _name_rva,
        ordinal_base,
        function_count,
        name_count,
        functions_rva,
        names_rva,
        ordinals_rva,
    ) = struct.unpack_from("<IIHHIIIIIII", nk, export_off)
    exports: dict[str, int] = {}
    for index in range(name_count):
        name_rva = u32(nk, xip_rva_to_storage_off(module, base_va, names_rva + index * 4))
        name_off = xip_rva_to_storage_off(module, base_va, name_rva)
        name_end = nk.find(b"\0", name_off)
        if name_end < 0:
            raise ValueError(f"{module_name}: unterminated export name")
        name = nk[name_off:name_end].decode("ascii")
        ordinal = u16(nk, xip_rva_to_storage_off(module, base_va, ordinals_rva + index * 2))
        if ordinal >= function_count:
            raise ValueError(f"{module_name}: export ordinal out of range")
        function_rva = u32(
            nk,
            xip_rva_to_storage_off(module, base_va, functions_rva + ordinal * 4),
        )
        exports[name] = module.vbase + function_rva

    _ = ordinal_base
    return exports


def module_entry_storage(
    nk: bytes | bytearray,
    base_va: int,
    module_name: str,
) -> tuple[int, int]:
    module = get_xip_module_info(nk, base_va, module_name)
    entry_rva = u32(nk, module.e32_off + 0x04)
    entry_va = module.vbase + entry_rva
    return entry_va, xip_rva_to_storage_off(module, base_va, entry_rva)


def install_xip_pe_module(
    nk: bytearray,
    base_va: int,
    slot_name: str,
    module_name: str,
    pe_path: Path,
) -> tuple[XipToc, int]:
    pe = parse_pe_image(pe_path)
    flat = pe_to_flat_image(pe)
    toc, slot_index, entry_off = find_xip_module(nk, base_va, slot_name)
    e32_va = u32(nk, entry_off + 0x18)
    o32_va = u32(nk, entry_off + 0x1C)
    e32_off = va_to_off(e32_va, base_va)
    o32_off = va_to_off(o32_va, base_va)

    if e32_off < 0 or e32_off + E32_SIZE > len(nk):
        raise ValueError(f"{slot_name}: E32 metadata out of range")
    if o32_off < 0 or o32_off + O32_ENTRY_SIZE > len(nk):
        raise ValueError(f"{slot_name}: O32 metadata out of range")

    slot_vbase = u32(nk, e32_off + 0x08)
    storage_va = u32(nk, o32_off + 0x0C)
    storage_size = u32(nk, o32_off + 0x08)
    realaddr_base = (u32(nk, o32_off + 0x10) - u32(nk, o32_off + 0x04)) & 0xFFFFFFFF
    storage_off = va_to_off(storage_va, base_va)
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
    struct.pack_into("<I", nk, o32_off + 0x10, realaddr_base)
    struct.pack_into("<I", nk, o32_off + 0x14, O32_SINGLE_IMAGE_FLAGS)
    return toc, slot_index


def build_net_hook_data(
    data_base_va: int,
    values: list[tuple[str, int, str | int]],
) -> tuple[dict[str, int], bytes]:
    data = bytearray()
    addrs: dict[str, int] = {}

    def align(alignment: int = 4) -> None:
        while len(data) % alignment:
            data.append(0)

    def put(name: str, blob: bytes, alignment: int = 2) -> int:
        align(alignment)
        addrs[name] = data_base_va + len(data)
        data.extend(blob)
        return addrs[name]

    put("key", encode_utf16z(STOWAWAY_KEY))
    align(4)
    entries_off = len(data)
    data.extend(b"\0" * (len(values) * 16))
    addrs["entries"] = data_base_va + entries_off

    for index, (name, reg_type, value) in enumerate(values):
        name_addr = put(f"name_{index}", encode_utf16z(name))
        if reg_type == REG_SZ:
            if not isinstance(value, str):
                raise TypeError(f"{name}: REG_SZ value must be str")
            value_blob = encode_utf16z(value)
            value_addr = put(f"value_{index}", value_blob)
        elif reg_type == REG_DWORD:
            if not isinstance(value, int):
                raise TypeError(f"{name}: REG_DWORD value must be int")
            value_blob = struct.pack("<I", value)
            value_addr = put(f"value_{index}", value_blob, 4)
        else:
            raise ValueError(f"unsupported Net hook registry type: {reg_type}")
        struct.pack_into(
            "<IIII",
            data,
            entries_off + index * 16,
            name_addr,
            reg_type,
            value_addr,
            len(value_blob),
        )

    return addrs, bytes(data)


def build_net_boot_hook_code(
    runtime_va: int,
    boot_entry_va: int,
    original_entry_bytes: bytes,
    exports: dict[str, int],
    data_addrs: dict[str, int],
    value_count: int,
) -> bytes:
    if len(original_entry_bytes) != 16:
        raise ValueError("boot.exe entry patch requires exactly 16 original bytes")

    a = MipsAsm()
    frame_size = 0x90
    hkey_sp_off = 0x60
    a.emit(
        addiu(SP, SP, -frame_size),
        sw(RA, 0x8C, SP),
        sw(A0, 0x80, SP),
        sw(A1, 0x84, SP),
        sw(A2, 0x88, SP),
        sw(A3, 0x7C, SP),
        sw(S0, 0x78, SP),
        sw(S1, 0x74, SP),
    )
    a.emit(
        *load_abs(A0, 0x80000002),
        *load_abs(A1, data_addrs["key"]),
        addu(A2, ZERO, ZERO),
        addu(A3, ZERO, ZERO),
    )
    a.emit(
        sw(ZERO, 0x10, SP),
        sw(ZERO, 0x14, SP),
        sw(ZERO, 0x18, SP),
        addiu(T0, SP, hkey_sp_off),
        sw(T0, 0x1C, SP),
        sw(ZERO, 0x20, SP),
        *call_abs(exports["RegCreateKeyExW"]),
    )
    a.emit(*load_abs(S0, data_addrs["entries"]), addiu(S1, ZERO, value_count))

    a.label("value_loop")
    a.beq_label(S1, ZERO, "values_done")
    a.emit(
        nop(),
        lw(A0, hkey_sp_off, SP),
        lw(A1, 0, S0),
        addu(A2, ZERO, ZERO),
        lw(A3, 4, S0),
        lw(T0, 8, S0),
        sw(T0, 0x10, SP),
        lw(T0, 12, S0),
        sw(T0, 0x14, SP),
        *call_abs(exports["RegSetValueExW"]),
        addiu(S0, S0, 16),
        addiu(S1, S1, -1),
    )
    a.b_label("value_loop")
    a.emit(nop())

    a.label("values_done")
    a.emit(
        lw(A0, hkey_sp_off, SP),
        *call_abs(exports["RegFlushKey"]),
        lw(A0, hkey_sp_off, SP),
        *call_abs(exports["RegCloseKey"]),
    )
    a.emit(
        *load_abs(A0, data_addrs["key"]),
        addu(A1, ZERO, ZERO),
        addu(A2, ZERO, ZERO),
        addu(A3, ZERO, ZERO),
        *call_abs(exports["ActivateDeviceEx"]),
    )
    a.emit(
        lw(S1, 0x74, SP),
        lw(S0, 0x78, SP),
        lw(A3, 0x7C, SP),
        lw(A0, 0x80, SP),
        lw(A1, 0x84, SP),
        lw(A2, 0x88, SP),
        lw(RA, 0x8C, SP),
        addiu(SP, SP, frame_size),
    )
    for off in range(0, 16, 4):
        a.emit(struct.unpack_from("<I", original_entry_bytes, off)[0])
    a.emit(*load_abs(T9, boot_entry_va + 16), jr(T9), nop())
    _ = runtime_va
    return a.finish()


def build_net_boot_hook_blob(
    runtime_va: int,
    boot_entry_va: int,
    original_entry_bytes: bytes,
    exports: dict[str, int],
    values: list[tuple[str, int, str | int]],
) -> bytes:
    dummy_addrs = {"key": runtime_va, "entries": runtime_va}
    code = build_net_boot_hook_code(
        runtime_va,
        boot_entry_va,
        original_entry_bytes,
        exports,
        dummy_addrs,
        len(values),
    )
    data_base = align_up(runtime_va + len(code), 4)
    data_addrs, data = build_net_hook_data(data_base, values)
    code = build_net_boot_hook_code(
        runtime_va,
        boot_entry_va,
        original_entry_bytes,
        exports,
        data_addrs,
        len(values),
    )
    data_base = align_up(runtime_va + len(code), 4)
    data_addrs, data = build_net_hook_data(data_base, values)
    code = build_net_boot_hook_code(
        runtime_va,
        boot_entry_va,
        original_entry_bytes,
        exports,
        data_addrs,
        len(values),
    )
    return code + (b"\0" * (align_up(len(code), 4) - len(code))) + data


def patch_net_stowaway_driver(nk: bytearray, base_va: int) -> list[str]:
    module = get_xip_module_info(nk, base_va, XIP_DRIVER_MODULE_NAME)
    if not module.sections:
        raise ValueError(f"{XIP_DRIVER_MODULE_NAME}: no sections")

    struct.pack_into("<I", nk, module.e32_off + 0x08, NET_STOWAWAY_PREFERRED_BASE)
    struct.pack_into("<I", nk, module.o32_off + 0x10, NET_STOWAWAY_PREFERRED_BASE)

    api_ready_off = xip_rva_to_storage_off(module, base_va, NET_STOWAWAY_API_READY_RVA)
    current = (u32(nk, api_ready_off), u32(nk, api_ready_off + 4))
    if current == NET_STOWAWAY_API_READY_ORIG:
        struct.pack_into("<II", nk, api_ready_off, *NET_STOWAWAY_API_READY_PATCH)
        api_note = "patched Stowaway.dll IsAPIReady gate"
    elif current == NET_STOWAWAY_API_READY_PATCH:
        api_note = "Stowaway.dll IsAPIReady gate already patched"
    else:
        raise ValueError(
            "Stowaway.dll Net API-ready patch point mismatch: "
            f"0x{current[0]:08x} 0x{current[1]:08x}"
        )

    return [
        f"set Stowaway.dll preferred base to 0x{NET_STOWAWAY_PREFERRED_BASE:08x}",
        api_note,
    ]


def install_net_boot_activation_hook(nk: bytearray, base_va: int) -> str:
    exports = xip_exports(nk, base_va, "coredll.dll")
    required_exports = (
        "RegCreateKeyExW",
        "RegSetValueExW",
        "RegFlushKey",
        "RegCloseKey",
        "ActivateDeviceEx",
    )
    missing = [name for name in required_exports if name not in exports]
    if missing:
        raise ValueError("coredll.dll missing exports: " + ", ".join(missing))

    boot_entry_va, boot_entry_off = module_entry_storage(nk, base_va, "boot.exe")
    original_entry = bytes(nk[boot_entry_off:boot_entry_off + 16])
    hook_jump = pack_words(
        [
            lui(T9, (NET_BOOT_HOOK_RUNTIME_VA >> 16) & 0xFFFF),
            ori(T9, T9, NET_BOOT_HOOK_RUNTIME_VA & 0xFFFF),
            jr(T9),
            nop(),
        ]
    )
    if original_entry == hook_jump:
        raise ValueError("boot.exe already contains the Net Stowaway activation hook")

    blob = build_net_boot_hook_blob(
        NET_BOOT_HOOK_RUNTIME_VA,
        boot_entry_va,
        original_entry,
        exports,
        NET_STOWAWAY_RUNTIME_REG_VALUES,
    )
    budget = NET_BOOT_HOOK_STORAGE_END_VA - NET_BOOT_HOOK_STORAGE_VA
    if len(blob) > budget:
        raise ValueError(f"Net boot activation hook is too large: 0x{len(blob):x} > 0x{budget:x}")
    storage_off = va_to_off(NET_BOOT_HOOK_STORAGE_VA, base_va)
    if storage_off < 0 or storage_off + budget > len(nk):
        raise ValueError("Net boot activation hook storage is outside the NK image")

    nk[storage_off:storage_off + budget] = b"\0" * budget
    nk[storage_off:storage_off + len(blob)] = blob
    nk[boot_entry_off:boot_entry_off + 16] = hook_jump
    return (
        f"boot.exe activation hook size=0x{len(blob):x} "
        f"storage=0x{NET_BOOT_HOOK_STORAGE_VA:08x}"
    )


def update_xip_driver(image: Path, cab_dir: Path, requested_slot: str | None = None) -> None:
    driver_path = cab_dir / XIP_DRIVER_CAB_NAME
    if not driver_path.is_file():
        raise SystemExit(f"error: missing CAB payload for XIP driver: {XIP_DRIVER_CAB_NAME}")

    nand = image.read_bytes()
    parsed = decode_nk_partition(nand, partition_index=2)
    nk = bytearray(parsed.flat_image)
    slot_names = (requested_slot,) if requested_slot else XIP_DRIVER_SLOT_CANDIDATES
    last_error: Exception | None = None
    actual_slot: str | None = None
    toc: XipToc | None = None
    slot_index = -1
    for slot_name in slot_names:
        try:
            toc, slot_index = install_xip_pe_module(
                nk,
                parsed.base_va,
                slot_name,
                XIP_DRIVER_MODULE_NAME,
                driver_path,
            )
            actual_slot = slot_name
            break
        except ValueError as exc:
            last_error = exc
            if requested_slot:
                break
    if actual_slot is None or toc is None:
        tried = ", ".join(slot_names)
        raise SystemExit(f"error: no compatible XIP module slot found ({tried}): {last_error}")

    net_notes: list[str] = []
    if parsed.base_va == NET_NK_BASE:
        try:
            net_notes.extend(patch_net_stowaway_driver(nk, parsed.base_va))
            net_notes.append(install_net_boot_activation_hook(nk, parsed.base_va))
        except ValueError as exc:
            raise SystemExit(f"error: failed to apply Net Stowaway XIP hook: {exc}")

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
        f"  xip: {XIP_DRIVER_MODULE_NAME} from {actual_slot} "
        f"(nk_base=0x{parsed.base_va:08x}, table=0x{toc.table_va:08x}, "
        f"modules={toc.module_count}, slot={slot_index}, "
        f"raw=0x{len(replacement_raw):x}, consumed=0x{raw_consumed:x})"
    )
    for note in net_notes:
        print(f"  net hook: {note}")


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


def ensure_backup_dir(image: Path, fs_offset: int) -> None:
    mmd = require_tool("mmd")
    try:
        run([mmd, "-i", mtools_image_arg(image, fs_offset), BACKUP_DIR], env=mtools_env())
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


def try_copy_from_image(image: Path, fs_offset: int, guest_path: str, host_path: Path) -> bool:
    mcopy = require_tool("mcopy")
    result = subprocess.run(
        [mcopy, "-o", "-i", mtools_image_arg(image, fs_offset), guest_path, str(host_path)],
        env=mtools_env(),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode == 0


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


def build_stowaway_registry() -> bytes:
    body = bytearray()
    body.extend(encode_key_record(REG_ROOT_HKLM, STOWAWAY_KEY))
    for name, reg_type, value in STOWAWAY_REG_VALUES:
        body.extend(encode_value_record(name, reg_type, value))

    out = bytearray(COMPACT_REGISTRY_MAGIC)
    out.extend(b"\0\0\0\0")
    out.extend(body)
    struct.pack_into("<I", out, 4, len(out))
    return bytes(out)


def update_registry_files(image: Path, fs_offset: int, work_dir: Path) -> str:
    system_reg = work_dir / "System.reg"
    ensure_backup_dir(image, fs_offset)
    copied = try_copy_from_image(image, fs_offset, f"{BACKUP_DIR}/System.reg", system_reg)
    if copied:
        updated = update_stowaway_registry(system_reg.read_bytes())
        mode = "updated existing"
    else:
        updated = build_stowaway_registry()
        mode = "created"
    system_reg.write_bytes(updated)
    copy_to_image(image, fs_offset, system_reg, f"{BACKUP_DIR}/System.reg")
    copy_to_image(image, fs_offset, system_reg, f"{BACKUP_DIR}/System.$$$")
    return mode


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

    fat, partitions = validate_nand(output, args.fs_offset)
    fs_offset = fat.offset
    fs_part = containing_partition(fat, partitions)
    part_note = f", partition={fs_part.index}" if fs_part is not None else ""
    print(
        f"  fat16: offset=0x{fat.offset:x}, end=0x{fat.end_offset:x}, "
        f"label={fat.volume_label!r}, sectors={fat.total_sectors}{part_note}"
    )

    with tempfile.TemporaryDirectory(prefix="be300-stowaway-") as tmp:
        tmpdir = Path(tmp)
        cab_dir = tmpdir / "cab"
        cab_dir.mkdir()
        extract_cab(cab, cab_dir)
        update_xip_driver(output, cab_dir, args.xip_slot)
        ensure_patch_dir(output, fs_offset)
        copy_payload_files(output, fs_offset, cab_dir)
        registry_mode = update_registry_files(output, fs_offset, tmpdir)

    print(f"Wrote Stowaway-enabled NAND image: {output}")
    print(r"  payload: \Nand Disk\Program Files\Patch")
    print(f"  xip driver: {XIP_DRIVER_MODULE_NAME}")
    print(rf"  registry: {registry_mode} HKLM\Drivers\BuiltIn\Stowaway")
    return 0


if __name__ == "__main__":
    sys.exit(main())
