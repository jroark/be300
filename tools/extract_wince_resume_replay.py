#!/usr/bin/env python3
"""Generate a narrow WinCE warm-resume replay snapshot from hardware surveys."""

from __future__ import annotations

import argparse
import math
import re
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


REGION_RE = re.compile(
    r"^\[REGION\] phase=(?P<phase>\w+) name=(?P<name>\S+) "
    r"PA=0x(?P<pa>[0-9A-Fa-f]+) size=0x(?P<size>[0-9A-Fa-f]+) "
    r"status=(?P<status>\w+)"
)
WORD_RE = re.compile(r"^0x(?P<addr>[0-9A-Fa-f]+):\s+(?P<words>[0-9A-Fa-f ]+)$")

PHASE_RANK = {
    "baseline": 0,
    "early": 1,
    "settle": 2,
    "post": 3,
}


@dataclass(frozen=True)
class WindowSpec:
    name: str
    region_names: tuple[str, ...]
    base: int
    size: int


WINDOW_SPECS = (
    WindowSpec("resume_context_22a0", ("resume_context_2200", "kdata_page"),
        0x000022A0, 0x20),
    WindowSpec("stack_frame_1770", ("stack_frame_1700",), 0x00001770, 0x90),
    WindowSpec("bootctx_stub_63d0", ("bootctx_alias_A0006000",),
        0x000063D0, 0x20),
    WindowSpec("dispatch_ptr_table_660170", ("objptr_table_80660000",),
        0x00660170, 0x30),
    WindowSpec("callback_table_a0051680",
        ("callback_table_A0051680", "callback_table_51600"),
        0x00051680, 0x200),
)


def sanitize(name: str) -> str:
    out: List[str] = []
    for ch in name:
        out.append(ch.lower() if ch.isalnum() else "_")
    return "".join(out)


def format_u32_words(values: Iterable[int]) -> str:
    parts = [f"UINT32_C(0x{value:08X})" for value in values]
    lines: List[str] = []
    for start in range(0, len(parts), 4):
        chunk = ", ".join(parts[start:start + 4])
        lines.append(f"    {chunk},")
    return "\n".join(lines)


def format_u8_values(values: Iterable[int]) -> str:
    parts = [f"0x{value:02X}" for value in values]
    lines: List[str] = []
    for start in range(0, len(parts), 8):
        chunk = ", ".join(parts[start:start + 8])
        lines.append(f"    {chunk},")
    return "\n".join(lines)


def parse_region_words(lines: List[str], start_index: int) -> Dict[int, int]:
    out: Dict[int, int] = {}
    for raw in lines[start_index + 1:]:
        line = raw.strip()
        if line.startswith("[REGION]") or line.startswith("--- "):
            break
        match = WORD_RE.match(line)
        if not match:
            continue
        addr = int(match.group("addr"), 16)
        words = [int(word, 16) for word in match.group("words").split()]
        for index, value in enumerate(words):
            out[addr + index * 4] = value
    return out


def select_best_region(lines: List[str], names: tuple[str, ...]) -> Dict[int, int]:
    best_phase_rank = -1
    best_words: Dict[int, int] = {}

    for index, raw in enumerate(lines):
        match = REGION_RE.match(raw.strip())
        if not match or match.group("status") != "OK":
            continue
        if match.group("name") not in names:
            continue
        phase_rank = PHASE_RANK.get(match.group("phase"), -1)
        if phase_rank < best_phase_rank:
            continue
        best_phase_rank = phase_rank
        best_words = parse_region_words(lines, index)

    return best_words


def collect_window_votes(paths: List[Path], spec: WindowSpec) -> Tuple[Dict[int, Counter[int]], int]:
    counters = {addr: Counter() for addr in range(spec.base, spec.base + spec.size, 4)}
    samples_with_data = 0

    for path in paths:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        region_words = select_best_region(lines, spec.region_names)
        saw_data = False
        for addr in counters:
            if addr in region_words:
                counters[addr][region_words[addr]] += 1
                saw_data = True
        if saw_data:
            samples_with_data += 1

    return counters, samples_with_data


def choose_resume_target(votes: Counter[int]) -> int:
    for value, count in votes.most_common():
        if count < 2:
            break
        if value == 0:
            continue
        if (value & 3) != 0:
            continue
        if value >= 0x00200000:
            continue
        return value
    return 0


def choose_majority_value(votes: Counter[int], required_support: int) -> tuple[int, int]:
    if not votes:
        return 0, 0
    value, support = votes.most_common(1)[0]
    return value, 1 if support >= required_support else 0


def render_header(paths: List[Path]) -> str:
    sample_count = len(paths)
    metadata: Dict[str, Dict[str, object]] = {}
    region_names: List[str] = []
    region_defs: List[str] = []
    region_required_supports: List[int] = []

    for spec in WINDOW_SPECS:
        votes, samples_with_data = collect_window_votes(paths, spec)
        required_support = max(2, math.ceil(samples_with_data * 0.40))
        words: List[int] = []
        valid: List[int] = []
        stable_words = 0

        for addr in range(spec.base, spec.base + spec.size, 4):
            value, is_valid = choose_majority_value(votes[addr], required_support)
            words.append(value)
            valid.append(is_valid)
            stable_words += is_valid

        if stable_words == 0:
            continue

        cname = sanitize(spec.name)
        region_names.append(spec.name)
        metadata[spec.name] = {
            "cname": cname,
            "base": spec.base,
            "size": spec.size,
            "word_count": len(words),
            "stable_words": stable_words,
            "required_support": required_support,
            "samples_with_data": samples_with_data,
        }
        region_required_supports.append(required_support)

        region_defs.append(
            f"static const uint32_t wince_resume_{cname}_words[] = {{\n"
            f"{format_u32_words(words)}\n"
            f"}};\n"
        )
        region_defs.append(
            f"static const uint8_t wince_resume_{cname}_valid[] = {{\n"
            f"{format_u8_values(valid)}\n"
            f"}};\n"
        )

    stack_votes, _stack_samples = collect_window_votes(paths, WINDOW_SPECS[1])
    synthetic_ra = 0
    if 0x000017BC in stack_votes and stack_votes[0x000017BC]:
        synthetic_ra = stack_votes[0x000017BC].most_common(1)[0][0]

    resume_votes, _resume_samples = collect_window_votes(paths, WINDOW_SPECS[0])
    resume_target_pc = choose_resume_target(resume_votes[0x000022B0])
    global_required_support = min(region_required_supports) if region_required_supports else 2

    region_table_lines: List[str] = []
    for name in region_names:
        meta = metadata[name]
        region_table_lines.append(
            '    { "%s", UINT32_C(0x%08X), UINT32_C(0x%04X), UINT32_C(%u),'
            " wince_resume_%s_words, wince_resume_%s_valid },"
            % (
                name,
                meta["base"],
                meta["size"],
                meta["word_count"],
                meta["cname"],
                meta["cname"],
            )
        )

    header = f"""#pragma once

/* Auto-generated by tools/extract_wince_resume_replay.py. */

static const uint32_t wince_resume_replay_sample_count = UINT32_C({sample_count});
static const uint32_t wince_resume_replay_required_support = UINT32_C({global_required_support});

{''.join(region_defs)}static const wince_resume_region_t wince_resume_replay_regions[] = {{
{chr(10).join(region_table_lines)}
}};

static const wince_resume_snapshot_t wince_resume_replay_snapshot = {{
    UINT32_C(0x{resume_target_pc:08X}),
    UINT32_C(0x00000000),
    UINT32_C(0x{synthetic_ra:08X}),
    wince_resume_replay_regions,
    UINT32_C({len(region_names)}),
    NULL,
    UINT32_C(0),
}};
"""
    return header


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", help="Generated header output path")
    parser.add_argument("inputs", nargs="+", help="hardware_survey probe dumps")
    args = parser.parse_args()

    input_paths = [Path(path) for path in args.inputs]
    missing = [str(path) for path in input_paths if not path.exists()]
    if missing:
        print("error: missing input files:", file=sys.stderr)
        for path in missing:
            print(f"  {path}", file=sys.stderr)
        return 2

    output_path = Path(args.output)
    output_path.write_text(render_header(input_paths), encoding="ascii")
    print(
        "[extract_wince_resume_replay]"
        f" inputs={len(input_paths)}"
        f" output={output_path}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
