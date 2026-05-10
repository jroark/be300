#!/usr/bin/env python3
"""Analyze the BE-300 All_nand_Net registration gate.

The CE .NET restore image boots far enough to show a registration-blocking
modal. This tool makes the evidence reproducible from the NAND image:

* find the FAT16 volume embedded in the NAND restore image
* check whether the expected Cassiopeia.dll registration artifact is present
* decode the compressed NK partition and locate the PowerOn.dll XIP module
* report the strings that tie PowerOn.dll to Cassiopeia.dll and the modal

If a recovered Cassiopeia.dll is available, the tool can also copy it into a
new NAND image with mtools. It intentionally does not synthesize a fake DLL.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import shutil
import struct
import subprocess
import sys
from pathlib import Path

from nk_lzss import decode_nk_partition


SECTOR_SIZE = 512
TOC_ENTRY_SIZE = 32
O32_ENTRY_SIZE = 24

REGISTRATION_DLL_PATH = r"\Nand Disk\Program Files\Cassiopeia.dll"
REGISTRATION_DLL_NAME = "Cassiopeia.dll"
REGISTRATION_CPK_NAME = "Cassiopeia.dll.cpk"
PROGRAM_FILES_NAME = "Program Files"


def u16(data: bytes, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0]


def u32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def va_to_off(va: int, base_va: int) -> int:
    return va - base_va


def va_in_image(va: int, base_va: int, data: bytes, size: int = 1) -> bool:
    off = va_to_off(va, base_va)
    return 0 <= off <= len(data) - size


def read_cstr(data: bytes, va: int, base_va: int, max_len: int = 128) -> str | None:
    if not va_in_image(va, base_va, data):
        return None
    off = va_to_off(va, base_va)
    end = data.find(b"\x00", off, min(len(data), off + max_len))
    if end < 0:
        return None
    raw = data[off:end]
    try:
        return raw.decode("ascii")
    except UnicodeDecodeError:
        return None


@dataclass(frozen=True)
class PartitionEntry:
    index: int
    start_sector: int
    sector_count: int

    @property
    def offset(self) -> int:
        return self.start_sector * SECTOR_SIZE

    @property
    def size_bytes(self) -> int:
        return self.sector_count * SECTOR_SIZE


@dataclass(frozen=True)
class FatEntry:
    short_name: str
    long_name: str | None
    attr: int
    first_cluster: int
    size: int

    @property
    def display_name(self) -> str:
        return self.long_name or self.short_name

    @property
    def is_dir(self) -> bool:
        return bool(self.attr & 0x10)

    @property
    def is_volume_label(self) -> bool:
        return bool(self.attr & 0x08)

    def matches(self, name: str) -> bool:
        target = name.casefold()
        if self.short_name.casefold() == target:
            return True
        return self.long_name is not None and self.long_name.casefold() == target


@dataclass(frozen=True)
class Fat16Volume:
    image: bytes
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
    def end_offset(self) -> int:
        return self.offset + self.total_sectors * self.bytes_per_sector

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

        return cls(
            image=image,
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

    def _fat16_entry(self, cluster: int) -> int:
        off = self.fat_offset + cluster * 2
        if off + 2 > len(self.image):
            return 0xFFFF
        return u16(self.image, off)

    def cluster_chain(self, first_cluster: int) -> list[int]:
        if first_cluster < 2:
            return []
        chain: list[int] = []
        seen: set[int] = set()
        cluster = first_cluster
        max_steps = self.total_clusters + 2
        while 2 <= cluster < 0xFFF8 and cluster not in seen and len(chain) < max_steps:
            seen.add(cluster)
            chain.append(cluster)
            nxt = self._fat16_entry(cluster)
            if nxt == 0 or nxt >= 0xFFF8:
                break
            cluster = nxt
        return chain

    def read_cluster(self, cluster: int) -> bytes:
        if cluster < 2:
            return b""
        off = self.data_offset + (cluster - 2) * self.cluster_size
        return self.image[off:off + self.cluster_size]

    def read_directory_bytes(self, entry: FatEntry | None = None) -> bytes:
        if entry is None:
            return self.image[self.root_dir_offset:self.root_dir_offset + self.root_dir_size]
        out = bytearray()
        for cluster in self.cluster_chain(entry.first_cluster):
            out += self.read_cluster(cluster)
        return bytes(out)

    def read_directory(self, entry: FatEntry | None = None) -> list[FatEntry]:
        return parse_fat_directory(self.read_directory_bytes(entry))

    def find_path(self, components: list[str]) -> FatEntry | None:
        current: FatEntry | None = None
        for i, component in enumerate(components):
            entries = self.read_directory(current)
            match = next(
                (
                    ent
                    for ent in entries
                    if not ent.is_volume_label and ent.matches(component)
                ),
                None,
            )
            if match is None:
                return None
            if i != len(components) - 1 and not match.is_dir:
                return None
            current = match
        return current


def decode_short_name(raw: bytes) -> str:
    base = raw[:8].decode("ascii", "replace").rstrip()
    ext = raw[8:11].decode("ascii", "replace").rstrip()
    return f"{base}.{ext}" if ext else base


def decode_lfn(entries: list[bytes]) -> str | None:
    if not entries:
        return None
    chars: list[str] = []
    for entry in reversed(entries):
        raw = entry[1:11] + entry[14:26] + entry[28:32]
        for i in range(0, len(raw), 2):
            code = u16(raw, i)
            if code in (0x0000, 0xFFFF):
                continue
            chars.append(chr(code))
    name = "".join(chars).rstrip("\uffff\x00")
    return name or None


def parse_fat_directory(data: bytes) -> list[FatEntry]:
    entries: list[FatEntry] = []
    lfn_entries: list[bytes] = []
    for off in range(0, len(data) - 31, 32):
        ent = data[off:off + 32]
        first = ent[0]
        if first == 0x00:
            break
        if first == 0xE5:
            lfn_entries = []
            continue
        attr = ent[0x0B]
        if attr == 0x0F:
            lfn_entries.append(ent)
            continue

        short_name = decode_short_name(ent[:11])
        long_name = decode_lfn(lfn_entries)
        lfn_entries = []
        first_cluster = (u16(ent, 0x14) << 16) | u16(ent, 0x1A)
        size = u32(ent, 0x1C)
        entries.append(
            FatEntry(
                short_name=short_name,
                long_name=long_name,
                attr=attr,
                first_cluster=first_cluster,
                size=size,
            )
        )
    return entries


def parse_nand_partitions(image: bytes, max_entries: int = 16) -> list[PartitionEntry]:
    partitions: list[PartitionEntry] = []
    for index in range(max_entries):
        off = index * 16
        if off + 16 > len(image):
            break
        marker = image[off:off + 8]
        start_sector = u32(image, off + 8)
        sector_count = u32(image, off + 12)
        if marker != b"\xff" * 8:
            continue
        if start_sector in (0, 0xFFFFFFFF) and sector_count in (0, 0xFFFFFFFF):
            continue
        if sector_count == 0 or sector_count == 0xFFFFFFFF:
            continue
        if start_sector * SECTOR_SIZE + sector_count * SECTOR_SIZE > len(image):
            continue
        partitions.append(PartitionEntry(index, start_sector, sector_count))
    return partitions


def find_all(data: bytes, needle: bytes) -> list[int]:
    out: list[int] = []
    off = data.find(needle)
    while off >= 0:
        out.append(off)
        off = data.find(needle, off + 1)
    return out


def find_fat16_volumes(image: bytes, partitions: list[PartitionEntry]) -> list[Fat16Volume]:
    candidates = {part.offset for part in partitions}
    candidates.update(off - 0x36 for off in find_all(image, b"FAT16   ") if off >= 0x36)

    volumes: list[Fat16Volume] = []
    seen_offsets: set[int] = set()
    for offset in sorted(candidates):
        if offset in seen_offsets:
            continue
        try:
            volume = Fat16Volume.from_boot_sector(image, offset)
        except ValueError:
            continue
        volumes.append(volume)
        seen_offsets.add(offset)
    return volumes


@dataclass(frozen=True)
class ModuleSection:
    index: int
    rva: int
    vsize: int
    psize: int
    data_ptr: int
    flags: int
    copied: int


@dataclass(frozen=True)
class ModuleEntry:
    index: int
    name: str
    entry_va: int
    e32_va: int
    o32_va: int
    objcnt: int
    vbase: int
    vsize: int
    sections: tuple[ModuleSection, ...]


@dataclass(frozen=True)
class TocCandidate:
    table_off: int
    module_count: int
    modules: tuple[ModuleEntry, ...]

    @property
    def table_va(self) -> int:
        if not self.modules:
            return 0
        first = self.modules[0]
        return first.entry_va


def section_entries_valid(data: bytes, base_va: int, o32_va: int, objcnt: int) -> bool:
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


def parse_module_entry(data: bytes, base_va: int, table_off: int, index: int) -> ModuleEntry | None:
    entry_off = table_off + index * TOC_ENTRY_SIZE
    if entry_off < 0 or entry_off + TOC_ENTRY_SIZE > len(data):
        return None

    entry_va = base_va + entry_off
    name_va = u32(data, entry_off + 0x14)
    e32_va = u32(data, entry_off + 0x18)
    o32_va = u32(data, entry_off + 0x1C)
    name = read_cstr(data, name_va, base_va)
    if not name or len(name) > 64:
        return None
    if any(ord(ch) < 0x20 or ord(ch) > 0x7E for ch in name):
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
    if not section_entries_valid(data, base_va, o32_va, objcnt):
        return None

    sections: list[ModuleSection] = []
    for i in range(objcnt):
        o32_off = va_to_off(o32_va + i * O32_ENTRY_SIZE, base_va)
        sec_vsize = u32(data, o32_off + 0x00)
        rva = u32(data, o32_off + 0x04)
        psize = u32(data, o32_off + 0x08)
        data_ptr = u32(data, o32_off + 0x0C)
        flags = u32(data, o32_off + 0x14)
        copied = 0
        if psize and va_in_image(data_ptr, base_va, data, psize) and rva + psize <= vsize:
            copied = psize
        sections.append(ModuleSection(i, rva, sec_vsize, psize, data_ptr, flags, copied))

    return ModuleEntry(
        index=index,
        name=name,
        entry_va=entry_va,
        e32_va=e32_va,
        o32_va=o32_va,
        objcnt=objcnt,
        vbase=vbase,
        vsize=vsize,
        sections=tuple(sections),
    )


def count_modules_at(data: bytes, base_va: int, table_off: int, limit: int = 256) -> TocCandidate | None:
    modules: list[ModuleEntry] = []
    for index in range(limit):
        module = parse_module_entry(data, base_va, table_off, index)
        if module is None:
            break
        modules.append(module)
    if not modules or modules[0].name != "nk.exe":
        return None
    return TocCandidate(table_off, len(modules), tuple(modules))


def find_xip_toc(data: bytes, base_va: int) -> TocCandidate:
    candidates: list[TocCandidate] = []
    for off in range(0, len(data) - TOC_ENTRY_SIZE, 4):
        name_va = u32(data, off + 0x14)
        if read_cstr(data, name_va, base_va, max_len=16) != "nk.exe":
            continue
        candidate = count_modules_at(data, base_va, off)
        if candidate is not None and candidate.module_count > 8:
            candidates.append(candidate)
    if not candidates:
        raise ValueError("could not locate XIP module table")
    return max(candidates, key=lambda c: c.module_count)


def reassemble_module(data: bytes, base_va: int, module: ModuleEntry) -> bytes:
    image = bytearray(module.vsize)
    for section in module.sections:
        if not section.copied:
            continue
        src_off = va_to_off(section.data_ptr, base_va)
        image[section.rva:section.rva + section.copied] = data[src_off:src_off + section.copied]
    return bytes(image)


@dataclass(frozen=True)
class FoundString:
    offset: int
    text: str


def utf16le_strings(data: bytes, min_chars: int = 4) -> list[FoundString]:
    out: list[FoundString] = []
    start: int | None = None
    chars: list[str] = []

    def flush(end_off: int) -> None:
        nonlocal start, chars
        if start is not None and len(chars) >= min_chars:
            out.append(FoundString(start, "".join(chars)))
        start = None
        chars = []

    for off in range(0, len(data) - 1, 2):
        code = u16(data, off)
        if 0x20 <= code <= 0x7E:
            if start is None:
                start = off
            chars.append(chr(code))
        else:
            flush(off)
    flush(len(data))
    return out


def interesting_utf16_strings(module_image: bytes) -> list[FoundString]:
    needles = (
        "Cassiopeia.dll",
        "Certificate",
        "Verificate",
        "CASSIOPEIA can no longer",
        "registration procedure",
        "Connections",
    )
    strings = utf16le_strings(module_image, min_chars=4)
    interesting = [
        found
        for found in strings
        if any(needle.casefold() in found.text.casefold() for needle in needles)
    ]
    interesting.sort(key=lambda item: item.offset)
    return interesting


def first_or_none(volume: Fat16Volume, components: list[str]) -> FatEntry | None:
    try:
        return volume.find_path(components)
    except (ValueError, struct.error):
        return None


def install_registration_dll(
    image_path: Path,
    out_image_path: Path,
    dll_path: Path,
    volume: Fat16Volume,
) -> None:
    if not dll_path.is_file():
        raise ValueError(f"registration DLL not found: {dll_path}")
    if image_path.resolve() == out_image_path.resolve():
        raise ValueError("--out-image must not overwrite the source NAND image")

    mcopy = shutil.which("mcopy")
    if mcopy is None:
        raise ValueError("mcopy was not found; install mtools or copy the DLL manually")

    dll_head = dll_path.read_bytes()[:2]
    if dll_head != b"MZ":
        print(f"warning: {dll_path} does not start with an MZ header", file=sys.stderr)

    out_image_path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(image_path, out_image_path)

    dest = f"::/{PROGRAM_FILES_NAME}/{REGISTRATION_DLL_NAME}"
    cmd = [
        mcopy,
        "-o",
        "-i",
        f"{out_image_path}@@0x{volume.offset:X}",
        str(dll_path),
        dest,
    ]
    try:
        subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except subprocess.CalledProcessError as exc:
        stderr = exc.stderr.decode("utf-8", "replace").strip()
        stdout = exc.stdout.decode("utf-8", "replace").strip()
        details = "\n".join(part for part in (stdout, stderr) if part)
        raise ValueError(f"mcopy failed: {details}") from exc

    updated = out_image_path.read_bytes()
    updated_volume = Fat16Volume.from_boot_sector(updated, volume.offset)
    if first_or_none(updated_volume, [PROGRAM_FILES_NAME, REGISTRATION_DLL_NAME]) is None:
        raise ValueError("mcopy completed, but Cassiopeia.dll was not found afterward")


def report(
    image_path: Path,
    partitions: list[PartitionEntry],
    volume: Fat16Volume,
    nk_base: int,
    nk_entry: int,
    toc: TocCandidate,
    poweron: ModuleEntry | None,
    strings: list[FoundString],
    cassiopeia_dll: FatEntry | None,
    cassiopeia_cpk: FatEntry | None,
) -> None:
    print(f"image: {image_path}")
    print()
    print("NAND partitions:")
    for part in partitions:
        print(
            f"  [{part.index}] sectors=0x{part.start_sector:X}+0x{part.sector_count:X} "
            f"offset=0x{part.offset:X} size=0x{part.size_bytes:X}"
        )
    print()
    print(
        "FAT16 volume: "
        f"offset=0x{volume.offset:X} end=0x{volume.end_offset:X} "
        f"label={volume.volume_label!r} sectors={volume.total_sectors}"
    )
    print(
        f"{PROGRAM_FILES_NAME}/{REGISTRATION_DLL_NAME}: "
        f"{'present' if cassiopeia_dll else 'missing'}"
    )
    print(
        f"{PROGRAM_FILES_NAME}/{REGISTRATION_CPK_NAME}: "
        f"{'present' if cassiopeia_cpk else 'missing'}"
    )
    print()
    print(
        "NK: "
        f"base=0x{nk_base:08X} entry=0x{nk_entry:08X} "
        f"module_table=0x{toc.table_va:08X} modules={toc.module_count}"
    )
    if poweron is None:
        print("PowerOn.dll: missing from XIP module table")
    else:
        print(
            "PowerOn.dll: "
            f"index={poweron.index} vbase=0x{poweron.vbase:08X} "
            f"vsize=0x{poweron.vsize:X} objcnt={poweron.objcnt}"
        )
        print("PowerOn.dll registration strings:")
        for found in strings:
            print(f"  rva=0x{found.offset:X}: {found.text}")
    print()
    print("Inference:")
    if poweron is not None and any("Cassiopeia.dll" in s.text for s in strings):
        print(
            f"  PowerOn.dll expects {REGISTRATION_DLL_PATH} and references "
            "Certificate/Verificate strings near that path."
        )
    if cassiopeia_dll is None:
        print(
            "  The FAT volume does not contain that DLL, so the stock image "
            "has no completed-registration artifact."
        )
    print(
        "  A recovered Cassiopeia.dll should be copied into a new NAND image; "
        "do not patch NK or the emulator to bypass the gate."
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="All_nand_Net.bin NAND image")
    parser.add_argument(
        "--install-cassiopeia-dll",
        type=Path,
        metavar="DLL",
        help="copy a recovered Cassiopeia.dll into a new NAND image",
    )
    parser.add_argument(
        "--out-image",
        type=Path,
        help="output NAND image for --install-cassiopeia-dll",
    )
    parser.add_argument(
        "--partition-index",
        type=int,
        default=2,
        help="NK partition-table index (default: 2)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    image_path: Path = args.image
    if not image_path.is_file():
        sys.exit(f"error: image not found: {image_path}")

    image = image_path.read_bytes()
    partitions = parse_nand_partitions(image)
    volumes = find_fat16_volumes(image, partitions)
    if not volumes:
        sys.exit("error: no FAT16 volume found")

    volume = max(
        volumes,
        key=lambda candidate: (
            first_or_none(candidate, [PROGRAM_FILES_NAME]) is not None,
            candidate.total_sectors,
        ),
    )

    cassiopeia_dll = first_or_none(volume, [PROGRAM_FILES_NAME, REGISTRATION_DLL_NAME])
    cassiopeia_cpk = first_or_none(volume, [PROGRAM_FILES_NAME, REGISTRATION_CPK_NAME])

    nk = decode_nk_partition(image, partition_index=args.partition_index)
    toc = find_xip_toc(nk.flat_image, nk.base_va)
    poweron = next(
        (module for module in toc.modules if module.name.casefold() == "poweron.dll"),
        None,
    )
    strings: list[FoundString] = []
    if poweron is not None:
        poweron_image = reassemble_module(nk.flat_image, nk.base_va, poweron)
        strings = interesting_utf16_strings(poweron_image)

    report(
        image_path=image_path,
        partitions=partitions,
        volume=volume,
        nk_base=nk.base_va,
        nk_entry=nk.entry_va,
        toc=toc,
        poweron=poweron,
        strings=strings,
        cassiopeia_dll=cassiopeia_dll,
        cassiopeia_cpk=cassiopeia_cpk,
    )

    if args.install_cassiopeia_dll is not None:
        if args.out_image is None:
            sys.exit("error: --install-cassiopeia-dll requires --out-image")
        try:
            install_registration_dll(
                image_path=image_path,
                out_image_path=args.out_image,
                dll_path=args.install_cassiopeia_dll,
                volume=volume,
            )
        except ValueError as exc:
            sys.exit(f"error: {exc}")
        print()
        print(
            f"installed {args.install_cassiopeia_dll} as "
            f"{PROGRAM_FILES_NAME}/{REGISTRATION_DLL_NAME} in {args.out_image}"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
