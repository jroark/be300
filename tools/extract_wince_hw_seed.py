#!/usr/bin/env python3
import re
import sys
import zlib
from pathlib import Path


REGION_RE = re.compile(
    r'^\[REGION\] phase=(?P<phase>\w+) name=(?P<name>\S+) '
    r'PA=0x(?P<pa>[0-9A-Fa-f]+) size=0x(?P<size>[0-9A-Fa-f]+) '
    r'status=(?P<status>\w+) crc32=0x(?P<crc>[0-9A-Fa-f]+)'
)

RAW_RE = re.compile(
    r'^\[REGION_RAW\] phase=(?P<phase>\w+) name=(?P<name>\S+) '
    r'PA=0x(?P<pa>[0-9A-Fa-f]+) off=0x(?P<off>[0-9A-Fa-f]+) '
    r'size=0x(?P<size>[0-9A-Fa-f]+) data=(?P<data>[0-9A-Fa-f]+)'
)

VREGION_RE = re.compile(
    r'^\[VREGION\] phase=(?P<phase>\w+) name=(?P<name>\S+) '
    r'VA=0x(?P<va>[0-9A-Fa-f]+) size=0x(?P<size>[0-9A-Fa-f]+) '
    r'status=(?P<status>\w+)'
    r'(?: valid_prefix=0x(?P<valid_prefix>[0-9A-Fa-f]+))? '
    r'crc32=0x(?P<crc>[0-9A-Fa-f]+)'
)

VRAW_RE = re.compile(
    r'^\[VREGION_RAW\] phase=(?P<phase>\w+) name=(?P<name>\S+) '
    r'VA=0x(?P<va>[0-9A-Fa-f]+) off=0x(?P<off>[0-9A-Fa-f]+) '
    r'size=0x(?P<size>[0-9A-Fa-f]+) data=(?P<data>[0-9A-Fa-f]+)'
)

LEGACY_SECTION_RE = re.compile(
    r'^--- (?P<title>.+?) \(PA: 0x(?P<pa>[0-9A-Fa-f]+)'
    r'(?:, Size: 0x(?P<size>[0-9A-Fa-f]+))?\) ---$'
)

LEGACY_WORD_RE = re.compile(
    r'^0x(?P<off>[0-9A-Fa-f]+):\s+(?P<words>[0-9A-Fa-f ]+)$'
)

LEGACY_OVERRIDE_NAMES = ("ctx_tlb", "resume_ctx")


def sanitize(name: str) -> str:
    out = []
    for ch in name:
        if ch.isalnum():
            out.append(ch.lower())
        else:
            out.append("_")
    return "".join(out)


def parse_boot_log(path: Path):
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    regions, vregions = parse_structured_boot_log(lines)

    if regions or vregions:
        regions = apply_legacy_region_overrides(regions)
        return regions, vregions

    legacy_regions, legacy_vregions = parse_legacy_dump(lines)
    if legacy_regions or legacy_vregions:
        return legacy_regions, legacy_vregions

    return regions, vregions


def parse_structured_boot_log(lines):
    regions = {}
    raw_chunks = {}
    vregions = {}
    vraw_chunks = {}

    for raw_line in lines:
        line = raw_line.strip()
        m = REGION_RE.match(line)
        if m and m.group("phase") == "init" and m.group("status") == "OK":
            name = m.group("name")
            regions[name] = {
                "name": name,
                "pa": int(m.group("pa"), 16),
                "size": int(m.group("size"), 16),
                "crc32": int(m.group("crc"), 16),
            }
            continue

        m = RAW_RE.match(line)
        if m and m.group("phase") == "init":
            name = m.group("name")
            raw_chunks.setdefault(name, []).append(
                {
                    "pa": int(m.group("pa"), 16),
                    "off": int(m.group("off"), 16),
                    "size": int(m.group("size"), 16),
                    "data": bytes.fromhex(m.group("data")),
                }
            )
            continue

        m = VREGION_RE.match(line)
        if m and m.group("phase") == "init":
            name = m.group("name")
            status = m.group("status")
            valid_prefix = int(m.group("valid_prefix") or "0", 16)
            if status == "OK":
                emit_size = int(m.group("size"), 16)
                complete = True
            elif status == "PARTIAL" and valid_prefix != 0:
                emit_size = valid_prefix
                complete = False
            else:
                continue
            vregions[name] = {
                "name": name,
                "va": int(m.group("va"), 16),
                "size": emit_size,
                "full_size": int(m.group("size"), 16),
                "crc32": int(m.group("crc"), 16),
                "complete": complete,
            }
            continue

        m = VRAW_RE.match(line)
        if m and m.group("phase") == "init":
            name = m.group("name")
            vraw_chunks.setdefault(name, []).append(
                {
                    "va": int(m.group("va"), 16),
                    "off": int(m.group("off"), 16),
                    "size": int(m.group("size"), 16),
                    "data": bytes.fromhex(m.group("data")),
                }
            )

    extracted = extract_regions(regions, raw_chunks, "pa", "REGION_RAW")
    v_extracted = extract_regions(vregions, vraw_chunks, "va", "VREGION_RAW")

    return select_pa_seed_regions(extracted), v_extracted


def select_pa_seed_regions(extracted):
    by_name = {region["name"]: region for region in extracted}
    selected = []

    for name in ("low_sdram_0000", "ctx_tlb", "resume_ctx"):
        region = by_name.get(name)
        if region is not None:
            selected.append(region)

    page = by_name.get("low_sdram_1000")
    if page is None:
        page = by_name.get("ctx_high_page")
    if page is not None:
        selected.append(slice_region(page, "low_sdram_1880", 0x0880, 0x0080))
        selected.append(slice_region(page, "low_sdram_1ac0", 0x0AC0, 0x0100))

    return selected


def apply_legacy_region_overrides(regions):
    legacy_path = Path(__file__).resolve().parents[1] / "docs" / "introspection.txt"
    if not legacy_path.exists():
        return regions

    legacy_lines = legacy_path.read_text(encoding="utf-8", errors="replace").splitlines()
    legacy_regions, _ = parse_legacy_dump(legacy_lines)
    if not legacy_regions:
        return regions

    legacy_by_name = {region["name"]: region for region in legacy_regions}
    merged = []
    seen = set()

    for region in regions:
        name = region["name"]
        override = legacy_by_name.get(name)
        if name in LEGACY_OVERRIDE_NAMES and override is not None:
            merged.append(override)
        else:
            merged.append(region)
        seen.add(name)

    for name in LEGACY_OVERRIDE_NAMES:
        if name in seen:
            continue
        override = legacy_by_name.get(name)
        if override is not None:
            merged.append(override)

    return merged


def parse_legacy_dump(lines):
    sections = []
    current = None

    for raw_line in lines:
        line = raw_line.strip()
        m = LEGACY_SECTION_RE.match(line)
        if m:
            if current is not None:
                sections.append(finalize_legacy_section(current))
            current = {
                "title": m.group("title"),
                "pa": int(m.group("pa"), 16),
                "size": int(m.group("size") or "0", 16),
                "chunks": [],
            }
            continue

        if current is None:
            continue

        m = LEGACY_WORD_RE.match(line)
        if not m:
            continue

        words = bytes()
        for word in m.group("words").split():
            words += int(word, 16).to_bytes(4, byteorder="little")

        current["chunks"].append(
            {
                "off": int(m.group("off"), 16),
                "data": words,
            }
        )

    if current is not None:
        sections.append(finalize_legacy_section(current))

    vectors = next(
        (
            section for section in sections
            if section["pa"] == 0x00000000
            and "Exception Vectors" in section["title"]
        ),
        None,
    )
    kdata = next(
        (
            section for section in sections
            if section["pa"] == 0x00002000
            and "Context/KData" in section["title"]
        ),
        None,
    )

    regions = []
    if vectors and len(vectors["data"]) >= 0x400:
        regions.append(
            make_region("low_sdram_0000", 0x00000000, vectors["data"][:0x400])
        )

    if kdata and len(kdata["data"]) >= 0x300:
        regions.append(
            make_region("ctx_tlb", 0x00002000, kdata["data"][:0x200])
        )
        regions.append(
            make_region("resume_ctx", 0x00002200, kdata["data"][0x200:0x300])
        )

    return regions, []


def finalize_legacy_section(section):
    size = section["size"]
    if size == 0:
        size = max(
            (chunk["off"] + len(chunk["data"]) for chunk in section["chunks"]),
            default=0,
        )

    buf = bytearray(size)
    for chunk in section["chunks"]:
        end = chunk["off"] + len(chunk["data"])
        if end > size:
            raise SystemExit(
                f"{section['title']}: legacy chunk overruns declared size"
            )
        buf[chunk["off"]:end] = chunk["data"]

    return {
        "title": section["title"],
        "pa": section["pa"],
        "data": bytes(buf),
    }


def make_region(name: str, pa: int, data: bytes):
    return {
        "name": name,
        "ident": sanitize(name),
        "pa": pa,
        "size": len(data),
        "crc32": zlib.crc32(data) & 0xFFFFFFFF,
        "data": data,
    }


def slice_region(region, name: str, off: int, size: int):
    end = off + size
    if end > len(region["data"]):
        raise SystemExit(
            f"{region['name']}: slice {name} overruns source region "
            f"(off=0x{off:X} size=0x{size:X})"
        )

    return make_region(name, region["pa"] + off, region["data"][off:end])


def extract_regions(regions, raw_chunks, addr_key: str, label: str):
    extracted = []
    for name, meta in sorted(regions.items(), key=lambda item: item[1][addr_key]):
        chunks = sorted(raw_chunks.get(name, []), key=lambda item: item["off"])
        if not chunks:
            continue

        buf = bytearray(meta["size"])
        seen = bytearray(meta["size"])
        for chunk in chunks:
            if chunk[addr_key] != meta[addr_key]:
                raise SystemExit(f"{name}: inconsistent base {addr_key.upper()} in {label}")
            if len(chunk["data"]) != chunk["size"]:
                raise SystemExit(f"{name}: size/data mismatch in {label}")
            end = chunk["off"] + chunk["size"]
            if end > meta["size"]:
                raise SystemExit(f"{name}: {label} chunk overruns region")
            buf[chunk["off"]:end] = chunk["data"]
            seen[chunk["off"]:end] = b"\x01" * chunk["size"]

        if any(v == 0 for v in seen):
            raise SystemExit(f"{name}: missing {label} coverage for one or more bytes")

        crc32 = zlib.crc32(bytes(buf)) & 0xFFFFFFFF
        if meta.get("complete", True) and crc32 != meta["crc32"]:
            raise SystemExit(
                f"{name}: CRC mismatch boot=0x{meta['crc32']:08X} generated=0x{crc32:08X}"
            )

        extracted.append(
            {
                "name": name,
                "ident": sanitize(name),
                addr_key: meta[addr_key],
                "size": meta["size"],
                "crc32": crc32,
                "data": bytes(buf),
            }
        )

    return extracted


def emit_header(path: Path, source_path: Path, regions, vregions):
    lines = []
    lines.append("/* Auto-generated by tools/extract_wince_hw_seed.py. */")
    lines.append(f"/* Source: {source_path.as_posix()} */")
    lines.append("#pragma once")
    lines.append("")

    for region in regions + vregions:
        lines.append(
            f"static const uint8_t wince_hw_seed_{region['ident']}_data[] = {{"
        )
        data = region["data"]
        for idx in range(0, len(data), 12):
            chunk = data[idx:idx + 12]
            payload = ", ".join(f"0x{b:02X}" for b in chunk)
            lines.append(f"    {payload},")
        lines.append("};")
        lines.append("")

    if regions:
        lines.append("static const wince_pa_seed_region_t wince_hw_seed_regions[] = {")
        for region in regions:
            lines.append(
                '    { "%s", UINT32_C(0x%08X), UINT32_C(0x%04X), UINT32_C(0x%08X), '
                "wince_hw_seed_%s_data },"
                % (
                    region["name"],
                    region["pa"],
                    region["size"],
                    region["crc32"],
                    region["ident"],
                )
            )
        lines.append("};")
    else:
        lines.append("static const wince_pa_seed_region_t wince_hw_seed_regions[1] = {")
        lines.append("    { NULL, UINT32_C(0), UINT32_C(0), UINT32_C(0), NULL },")
        lines.append("};")
    lines.append("")
    lines.append(
        f"static const uint32_t wince_hw_seed_region_count = UINT32_C({len(regions)});"
    )
    lines.append("")

    if vregions:
        lines.append("static const wince_va_seed_region_t wince_hw_vseed_regions[] = {")
        for region in vregions:
            lines.append(
                '    { "%s", UINT32_C(0x%08X), UINT32_C(0x%04X), UINT32_C(0x%08X), '
                "wince_hw_seed_%s_data },"
                % (
                    region["name"],
                    region["va"],
                    region["size"],
                    region["crc32"],
                    region["ident"],
                )
            )
        lines.append("};")
    else:
        lines.append("static const wince_va_seed_region_t wince_hw_vseed_regions[1] = {")
        lines.append("    { NULL, UINT32_C(0), UINT32_C(0), UINT32_C(0), NULL },")
        lines.append("};")
    lines.append("")
    lines.append(
        f"static const uint32_t wince_hw_vseed_region_count = UINT32_C({len(vregions)});"
    )
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main(argv):
    if len(argv) != 3:
        raise SystemExit(
            "usage: extract_wince_hw_seed.py <BEDiag_boot.txt> <out_header>"
        )

    source = Path(argv[1])
    out_header = Path(argv[2])
    regions, vregions = parse_boot_log(source)
    emit_header(out_header, source, regions, vregions)
    print(
        f"[extract_wince_hw_seed] wrote {len(regions)} pa regions and {len(vregions)} va regions to {out_header.as_posix()}"
    )


if __name__ == "__main__":
    main(sys.argv)
