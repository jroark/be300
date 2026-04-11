#!/usr/bin/env python3
"""Clean up combined BEDiag HW register dump.

Strips diagnostic scaffolding, zero-fill regions, and corrupted merged
lines.  Deduplicates hex dumps by address+data.  Splits output into
focused files grouped by hardware block.

Usage:
    python3 tools/cleanup_hw_dump.py docs/hw_dump_combined.txt --outdir docs/ --stats
"""

import argparse
import re
import sys
from collections import defaultdict, OrderedDict

# ---------------------------------------------------------------------------
# Tag sets for classification
# ---------------------------------------------------------------------------

DROP_TAGS = frozenset([
    "BEDIAG_KICK", "BEDIAG_STAGE", "BEDIAG_DORMANT_INIT", "BEDIAG",
    "REG_VALUE", "REG_KEY", "REG_SUBKEY",
    "FS_ENTRY", "FS_ROOT",
    "CAPTURE_BEGIN", "UTF16_HINT",
    "STORAGE", "STORAGE_LOOP", "NAND", "NAND_LOOP",
])

NOISE_SECTION_HEADERS = frozenset([
    "BEDIAG DONE", "BEDIAG INIT", "CAPTURE PASSES",
    "DRIVER INVENTORY", "DRIVER STATE", "FILESYSTEM ROOTS",
    "NAND WORKLOAD", "STORAGE MANAGER", "STORAGE WORKLOAD",
    "SNAPSHOT INIT", "PRIVILEGE BOOTSTRAP", "RUN METADATA",
])

BARE_NOISE_PREFIXES = [
    "tick_ms=", "worker_started=", "build=", "context=0x",
    "init_tick_ms=", "exe_path=", "output_path=", "output_path_source=",
    "serial_status=", "file_status=", "probe_complete", "survey_complete",
    "osversion", "sysinfo", "status=", "path=\\", "Output Path:",
    "active_key=", "backlog_used=", "boot_tag", "serial_open_failed",
    "VirtualCopy",
]

# Destination file keys
VR4131 = "vr4131"
VRC4173 = "vrc4173"
MEMORY = "memory"
TLB = "tlb"
DIFFS = "diffs"

# Tag -> destination mapping
TAG_DEST = {
    "BCU_FIELD": VR4131, "BCU_RANGE": VR4131, "BCU_COMPARE": VR4131,
    "TLB_ENTRY": TLB, "TLB_MATCH": TLB, "TLB_TABLE": TLB,
    "DIFF_WORD": DIFFS, "DIFF_REGION": DIFFS, "DIFF_TOTAL": DIFFS,
    "DIFF_TRUNCATED": DIFFS,
    "FOCUS": DIFFS, "FOCUS3": DIFFS, "VFOCUS": DIFFS, "FOCUS_DUMP": DIFFS,
    "REGION": DIFFS, "VREGION": DIFFS,
    "PAGE_COMPARE": DIFFS, "PASS_COMPARE": DIFFS,
    "REGION_RAW": MEMORY, "VREGION_RAW": MEMORY,
    "RAMDUMP": MEMORY, "ROM_PAGE": MEMORY, "ROM_RANGE": MEMORY,
    "PRIV": MEMORY, "READ_FAULT": MEMORY,
}

# Hex dump regex: lines like "0xADDR: HHHHHHHH HHHHHHHH ..."
HEX_DUMP_RE = re.compile(r"^(0x[0-9A-Fa-f]+):\s+(.+)$")

# Bracketed tag regex
TAG_RE = re.compile(r"^\[([A-Z_0-9]+)\]\s*(.*)")

# Section header regex: --- TEXT ---
SECTION_RE = re.compile(r"^---\s+(.+?)\s+---$")

# Merged line detection: two bracket entries on one line
MERGED_RE = re.compile(r"^(.*?)\[([A-Z_0-9]+)\](.*)$")

# All-zero data in REGION_RAW
ZERO_DATA_RE = re.compile(r"data=0{64}")

# All-zero hex dump line (4 words of 8 zeros)
ZERO_HEX_RE = re.compile(r"^0x[0-9A-Fa-f]+:\s+0{8}\s+0{8}\s+0{8}\s+0{8}\s*$")


def parse_hex_addr(addr_str):
    """Parse hex address string, return (normalized, numeric, is_short)."""
    val = int(addr_str, 16)
    is_short = len(addr_str) <= 6  # 0x000 through 0x0FFF
    return addr_str, val, is_short


def route_hex_dump(addr_str, addr_val, is_short):
    """Map hex dump address to destination file."""
    if is_short:
        return VRC4173  # broad scan of 0x0A00xxxx uses short offsets
    if 0x0F000000 <= addr_val <= 0x0FFFFFFF:
        return VR4131
    if 0x0A000000 <= addr_val <= 0x0AFFFFFF:
        return VRC4173
    if addr_val <= 0x000FFFFF:
        return MEMORY
    if 0x1FC00000 <= addr_val <= 0x1FCFFFFF:
        return MEMORY
    return MEMORY  # fallback


def hex_sort_key(line):
    """Sort key for hex dump lines by address."""
    m = HEX_DUMP_RE.match(line)
    if m:
        return int(m.group(1), 16)
    return 0


def tag_sort_key(line):
    """Sort key for tagged lines."""
    return line


def classify_and_route(line):
    """Classify a line. Returns (dest, line) or None to drop."""
    stripped = line.rstrip("\n\r")
    if not stripped:
        return None

    # Check for corrupted merged lines: contains ][ pattern
    # e.g., "[BEDIAG_STAGE] ...[REGION_RAW] ..."
    # or " serial_open_failed=0 retried=0[REGION_RAW] ..."
    bracket_positions = [i for i, c in enumerate(stripped) if c == "["]
    if len(bracket_positions) >= 2:
        # Try to extract the second bracketed tag
        second_start = bracket_positions[1]
        remainder = stripped[second_start:]
        m = TAG_RE.match(remainder)
        if m:
            tag = m.group(1)
            if tag in DROP_TAGS:
                # Both parts are noise
                first_part = stripped[:second_start].rstrip()
                # Check if first part alone is useful
                result = classify_and_route(first_part)
                return result
            # Drop the first part (noise), keep second part
            return classify_and_route(remainder)

    # Section headers
    m = SECTION_RE.match(stripped)
    if m:
        header_text = m.group(1)
        # Strip pass numbers for matching: "PASS 1 BCU READBACK ..." -> check base
        base_text = re.sub(r"PASS \d+ ", "", header_text)
        base_text = re.sub(r"\(PA:.*\)", "", base_text).strip()
        if base_text in NOISE_SECTION_HEADERS or header_text in NOISE_SECTION_HEADERS:
            return None
        # Route to appropriate file based on content
        if "VR4131" in header_text or "BCU" in header_text:
            return (VR4131, stripped)
        if "VRC4173" in header_text or "PCI I/O" in header_text or "Broad Scan" in header_text:
            return (VRC4173, stripped)
        if "Detailed Dump" in header_text:
            return (VRC4173, stripped)
        if "ROM" in header_text or "RESET WINDOW" in header_text:
            return (MEMORY, stripped)
        if "Exception" in header_text or "RAM DUMP" in header_text:
            return (MEMORY, stripped)
        if "BASELINE" in header_text or "EARLY" in header_text or \
           "SETTLE" in header_text or "POST" in header_text:
            return (MEMORY, stripped)
        if "DIFF" in header_text or "PASS DIFF" in header_text:
            return (DIFFS, stripped)
        # Default: keep in memory
        return (MEMORY, stripped)

    # Bracketed tags
    m = TAG_RE.match(stripped)
    if m:
        tag = m.group(1)
        if tag in DROP_TAGS:
            return None
        dest = TAG_DEST.get(tag)
        if dest is None:
            return None  # unknown tag, drop
        # For REGION_RAW/VREGION_RAW, check for all-zero data
        if tag in ("REGION_RAW", "VREGION_RAW") and ZERO_DATA_RE.search(stripped):
            return None
        return (dest, stripped)

    # Hex dump lines
    m = HEX_DUMP_RE.match(stripped)
    if m:
        if ZERO_HEX_RE.match(stripped):
            return None
        addr_str = m.group(1)
        addr_str_norm, addr_val, is_short = parse_hex_addr(addr_str)
        dest = route_hex_dump(addr_str_norm, addr_val, is_short)
        return (dest, stripped)

    # "DETECTED non-zero" lines
    if stripped.startswith("DETECTED non-zero"):
        return (MEMORY, stripped)

    # Lines starting with " (registry values) - drop
    if stripped.startswith('"'):
        return None

    # Bare noise lines
    for prefix in BARE_NOISE_PREFIXES:
        if stripped.startswith(prefix):
            return None
    # Also check without leading space
    s = stripped.lstrip()
    for prefix in BARE_NOISE_PREFIXES:
        if s.startswith(prefix):
            return None

    # Unknown line - drop
    return None


def subsection_key(dest, line):
    """Generate a subsection name for grouping within a file."""
    m = SECTION_RE.match(line)
    if m:
        return ("_section_header", line)

    m = HEX_DUMP_RE.match(line)
    if m:
        return ("hex_dump", None)

    m = TAG_RE.match(line)
    if m:
        return ("tagged_" + m.group(1), None)

    return ("other", None)


def write_output(dest_key, lines, outdir, stats):
    """Write a destination file with organized content."""
    filenames = {
        VR4131: "hw_dump_vr4131.txt",
        VRC4173: "hw_dump_vrc4173.txt",
        MEMORY: "hw_dump_memory.txt",
        TLB: "hw_dump_tlb.txt",
        DIFFS: "hw_dump_diffs.txt",
    }
    titles = {
        VR4131: "VR4131 SoC Register Dumps (BCU, ICU, PMU/RTC, GIU, PCI)",
        VRC4173: "VRC4173 Companion Chip Register Dumps",
        MEMORY: "Memory Dumps (SDRAM, Exception Vectors, Boot Params, ROM)",
        TLB: "TLB State Snapshots",
        DIFFS: "Memory Diffs, Focus Probes, and Region Summaries",
    }

    filepath = f"{outdir}/{filenames[dest_key]}"

    # Separate lines by type
    section_headers = []
    hex_lines = []
    tagged_groups = defaultdict(list)
    other_lines = []

    for line in lines:
        m = SECTION_RE.match(line)
        if m:
            section_headers.append(line)
            continue
        m = HEX_DUMP_RE.match(line)
        if m:
            hex_lines.append(line)
            continue
        m = TAG_RE.match(line)
        if m:
            tagged_groups[m.group(1)].append(line)
            continue
        other_lines.append(line)

    # Dedup hex dumps by (address, data)
    seen_hex = set()
    deduped_hex = []
    for line in hex_lines:
        m = HEX_DUMP_RE.match(line)
        key = (m.group(1), m.group(2).strip())
        if key not in seen_hex:
            seen_hex.add(key)
            deduped_hex.append(line)
    hex_dedup_count = len(hex_lines) - len(deduped_hex)
    hex_lines = deduped_hex

    # Dedup tagged lines by exact content
    tag_dedup_count = 0
    for tag in tagged_groups:
        before = len(tagged_groups[tag])
        tagged_groups[tag] = list(OrderedDict.fromkeys(tagged_groups[tag]))
        tag_dedup_count += before - len(tagged_groups[tag])

    # Dedup other lines
    other_before = len(other_lines)
    other_lines = list(OrderedDict.fromkeys(other_lines))
    other_dedup = other_before - len(other_lines)

    total_dedup = hex_dedup_count + tag_dedup_count + other_dedup
    total_lines = len(hex_lines) + sum(len(v) for v in tagged_groups.values()) + \
                  len(other_lines) + len(section_headers)

    stats[dest_key] = {
        "lines": total_lines,
        "hex_dedup": hex_dedup_count,
        "tag_dedup": tag_dedup_count,
    }

    with open(filepath, "w") as f:
        f.write("#" * 78 + "\n")
        f.write(f"# {titles[dest_key]}\n")
        f.write(f"# Source: BEDiag dumps from real Casio BE-300 hardware\n")
        f.write("#" * 78 + "\n\n")

        # Write section headers first (as context)
        if section_headers:
            for h in sorted(set(section_headers)):
                f.write(h + "\n")
            f.write("\n")

        # Write hex dumps sorted by address
        if hex_lines:
            f.write("#" + "-" * 77 + "\n")
            f.write("# Register / Memory Hex Dumps\n")
            f.write("#" + "-" * 77 + "\n")

            # Group hex dumps by address range for sub-headers
            if dest_key == VR4131:
                groups = group_vr4131_hex(hex_lines)
            elif dest_key == VRC4173:
                groups = group_vrc4173_hex(hex_lines)
            else:
                groups = [("All", hex_lines)]

            for group_name, group_lines in groups:
                group_lines.sort(key=hex_sort_key)
                if len(groups) > 1:
                    f.write(f"\n# {group_name}\n")
                for line in group_lines:
                    f.write(line + "\n")
            f.write("\n")

        # Write tagged groups
        for tag in sorted(tagged_groups.keys()):
            tag_lines = sorted(tagged_groups[tag], key=tag_sort_key)
            f.write("#" + "-" * 77 + "\n")
            f.write(f"# [{tag}] ({len(tag_lines)} entries)\n")
            f.write("#" + "-" * 77 + "\n")
            for line in tag_lines:
                f.write(line + "\n")
            f.write("\n")

        # Write other lines
        if other_lines:
            f.write("#" + "-" * 77 + "\n")
            f.write("# Other\n")
            f.write("#" + "-" * 77 + "\n")
            for line in sorted(other_lines):
                f.write(line + "\n")
            f.write("\n")

    return total_lines


def group_vr4131_hex(lines):
    """Group VR4131 hex dumps by sub-block."""
    groups = OrderedDict([
        ("BCU (0x0F000000-0x0F00003F)", []),
        ("ICU (0x0F000080-0x0F0000BF)", []),
        ("PMU/RTC (0x0F0000C0-0x0F00013F)", []),
        ("GIU (0x0F000140-0x0F00019F)", []),
        ("PCI (0x0F000C00+)", []),
        ("Other", []),
    ])
    for line in lines:
        m = HEX_DUMP_RE.match(line)
        addr = int(m.group(1), 16)
        if 0x0F000000 <= addr <= 0x0F00003F:
            groups["BCU (0x0F000000-0x0F00003F)"].append(line)
        elif 0x0F000080 <= addr <= 0x0F0000BF:
            groups["ICU (0x0F000080-0x0F0000BF)"].append(line)
        elif 0x0F0000C0 <= addr <= 0x0F00013F:
            groups["PMU/RTC (0x0F0000C0-0x0F00013F)"].append(line)
        elif 0x0F000140 <= addr <= 0x0F00019F:
            groups["GIU (0x0F000140-0x0F00019F)"].append(line)
        elif addr >= 0x0F000C00:
            groups["PCI (0x0F000C00+)"].append(line)
        else:
            groups["Other"].append(line)
    return [(k, v) for k, v in groups.items() if v]


def group_vrc4173_hex(lines):
    """Group VRC4173 hex dumps by sub-block."""
    groups = OrderedDict([
        ("Core (0x0A000000-0x0A0000FF)", []),
        ("Detailed 0x0A001000", []),
        ("Video (0x0A002000-0x0A002FFF)", []),
        ("SIU/UART (0x0A008000-0x0A008FFF)", []),
        ("GPIO (0x0A00A000-0x0A00AFFF)", []),
        ("PCI/NAND I/O (0x0A00C000-0x0A00CFFF)", []),
        ("Detailed 0x0A00D000", []),
        ("Broad Scan (short offsets)", []),
        ("Other 0x0A00xxxx", []),
    ])
    for line in lines:
        m = HEX_DUMP_RE.match(line)
        addr_str = m.group(1)
        addr = int(addr_str, 16)
        is_short = len(addr_str) <= 6
        if is_short:
            groups["Broad Scan (short offsets)"].append(line)
        elif 0x0A000000 <= addr <= 0x0A0000FF:
            groups["Core (0x0A000000-0x0A0000FF)"].append(line)
        elif 0x0A001000 <= addr <= 0x0A001FFF:
            groups["Detailed 0x0A001000"].append(line)
        elif 0x0A002000 <= addr <= 0x0A002FFF:
            groups["Video (0x0A002000-0x0A002FFF)"].append(line)
        elif 0x0A008000 <= addr <= 0x0A008FFF:
            groups["SIU/UART (0x0A008000-0x0A008FFF)"].append(line)
        elif 0x0A00A000 <= addr <= 0x0A00AFFF:
            groups["GPIO (0x0A00A000-0x0A00AFFF)"].append(line)
        elif 0x0A00C000 <= addr <= 0x0A00CFFF:
            groups["PCI/NAND I/O (0x0A00C000-0x0A00CFFF)"].append(line)
        elif 0x0A00D000 <= addr <= 0x0A00DFFF:
            groups["Detailed 0x0A00D000"].append(line)
        else:
            groups["Other 0x0A00xxxx"].append(line)
    return [(k, v) for k, v in groups.items() if v]


def main():
    parser = argparse.ArgumentParser(
        description="Clean up combined BEDiag HW register dump")
    parser.add_argument("input", help="Path to hw_dump_combined.txt")
    parser.add_argument("--outdir", default=".",
                        help="Output directory for split files (default: .)")
    parser.add_argument("--stats", action="store_true",
                        help="Print classification statistics")
    args = parser.parse_args()

    with open(args.input) as f:
        raw_lines = f.readlines()

    # Classify and route all lines
    dest_lines = defaultdict(list)
    drop_count = 0
    total = len(raw_lines)

    for line in raw_lines:
        result = classify_and_route(line)
        if result is None:
            drop_count += 1
        else:
            dest, cleaned = result
            dest_lines[dest].append(cleaned)

    # Write output files
    stats = {}
    total_output = 0
    for dest_key in [VR4131, VRC4173, MEMORY, TLB, DIFFS]:
        if dest_lines[dest_key]:
            n = write_output(dest_key, dest_lines[dest_key], args.outdir, stats)
            total_output += n

    if args.stats:
        print(f"Input: {total} lines")
        print(f"Dropped: {drop_count} lines (noise/scaffolding/all-zeros)")
        print(f"Routed: {total - drop_count} lines")
        print()
        for dest_key in [VR4131, VRC4173, MEMORY, TLB, DIFFS]:
            if dest_key in stats:
                s = stats[dest_key]
                print(f"  {dest_key}: {s['lines']} lines "
                      f"(hex dedup: {s['hex_dedup']}, tag dedup: {s['tag_dedup']})")
        print(f"\nTotal output: {total_output} lines")
        print(f"Reduction: {total} -> {total_output} "
              f"({100 * (total - total_output) / total:.1f}%)")


if __name__ == "__main__":
    main()
