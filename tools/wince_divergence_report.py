#!/usr/bin/env python3
"""Summarize WinCE NAND divergence markers from emulator stdout/stderr logs."""

from __future__ import annotations

import argparse
import os
import re
from typing import Dict, Iterable, List, Optional, Tuple


LineHit = Tuple[int, str]


def read_lines(path: str) -> List[str]:
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return [line.rstrip("\n") for line in f]


def first_contains(lines: Iterable[str], needle: str) -> Optional[LineHit]:
    for idx, line in enumerate(lines, start=1):
        if needle in line:
            return idx, line
    return None


def first_match(lines: Iterable[str], pattern: re.Pattern[str]) -> Optional[LineHit]:
    for idx, line in enumerate(lines, start=1):
        if pattern.search(line):
            return idx, line
    return None


def collect_lines(lines: Iterable[str], prefix: str) -> List[LineHit]:
    out: List[LineHit] = []
    for idx, line in enumerate(lines, start=1):
        if line.startswith(prefix):
            out.append((idx, line))
    return out


def last_by_region(lines: Iterable[LineHit], regex: re.Pattern[str]) -> Dict[str, LineHit]:
    out: Dict[str, LineHit] = {}
    for lineno, line in lines:
        m = regex.search(line)
        if not m:
            continue
        reason = m.group("reason")
        region = m.group("region")
        out[f"{reason}:{region}"] = (lineno, line)
    return out


def read_exit_code(path: Optional[str]) -> Optional[int]:
    if not path:
        return None
    if not os.path.exists(path):
        return None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read().strip()
    if not text:
        return None
    try:
        return int(text.splitlines()[-1].strip())
    except ValueError:
        return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stdout", required=True, help="path to emulator stdout log")
    parser.add_argument("--stderr", required=True, help="path to emulator stderr log")
    parser.add_argument("--exit-code-file", help="optional file containing timeout/emu exit code")
    parser.add_argument("--mmio-tail", type=int, default=12, help="number of MMIO tail lines to show")
    args = parser.parse_args()

    out_lines = read_lines(args.stdout)
    err_lines = read_lines(args.stderr)
    exit_code = read_exit_code(args.exit_code_file)

    progress_markers = [
        "Kernel loader core",
        "Start downloading...",
        "Image start address:",
        "Image length",
    ]

    stall_hit = first_match(err_lines, re.compile(r"^\[WINCE_STALL\] PC=0x[0-9A-Fa-f]+"))
    null_hit = first_match(err_lines, re.compile(r"^\[WINCE_INTR\] NULL call detected"))
    bailout_hit = first_match(err_lines, re.compile(r"^\[WINCE_NULL_BAILOUT\]"))

    events: List[Tuple[int, str, str]] = []
    if stall_hit:
        events.append((stall_hit[0], "STALL", stall_hit[1]))
    if null_hit:
        events.append((null_hit[0], "NULL_CALL", null_hit[1]))
    if bailout_hit:
        events.append((bailout_hit[0], "NULL_BAILOUT", bailout_hit[1]))
    events.sort(key=lambda x: x[0])

    region_writes = collect_lines(err_lines, "[WINCE_REGION_WRITE]")
    region_nz2z = collect_lines(err_lines, "[WINCE_REGION_NZ2Z]")
    region_summary = collect_lines(err_lines, "[WINCE_REGION_SUMMARY]")
    region_nz2z_summary = collect_lines(err_lines, "[WINCE_REGION_NZ2Z_SUMMARY]")
    div_events = collect_lines(err_lines, "[WINCE_DIV_EVENT]")
    div_summary = collect_lines(err_lines, "[WINCE_DIV_SUMMARY]")
    div_lines = collect_lines(err_lines, "[WINCE_DIV]")
    div_call_returns = collect_lines(err_lines, "[WINCE_DIV_CALL_RETURN]")
    div_call_summary = collect_lines(err_lines, "[WINCE_DIV_CALL_SUMMARY]")
    mmio_lines = collect_lines(err_lines, "[WINCE_STALL_MMIO]")
    null_mmio_lines = collect_lines(err_lines, "[WINCE_NULL_MMIO]")

    key_tokens = [
        "ctx_table_0xA0051680",
        "ctx_bootparam_0xA001D000",
        "ctx_bootparam_0xA002D000",
        "ctx_bootctx_0xA0006000",
    ]

    key_hits: Dict[str, Optional[LineHit]] = {}
    for token in key_tokens:
        key_hits[token] = first_contains(err_lines, token)

    region_summary_last = last_by_region(
        region_summary,
        re.compile(r"reason=(?P<reason>[^ ]+) region=(?P<region>[^ ]+)"),
    )
    region_nz2z_summary_last = last_by_region(
        region_nz2z_summary,
        re.compile(r"reason=(?P<reason>[^ ]+) region=(?P<region>[^ ]+)"),
    )

    print("--- RUN ---")
    print(f"stdout: {args.stdout}")
    print(f"stderr: {args.stderr}")
    if exit_code is None:
        print("exit_code: unknown")
    else:
        kind = "timeout (expected for bounded runs)" if exit_code == 124 else "normal/error"
        print(f"exit_code: {exit_code} ({kind})")

    print("--- PROGRESS ---")
    for marker in progress_markers:
        hit = first_contains(out_lines, marker)
        if hit:
            print(f"{marker}: yes (stdout:{hit[0]})")
        else:
            print(f"{marker}: no")

    print("--- FIRST DIVERGENCE ---")
    if events:
        first = events[0]
        print(f"first_event: {first[1]} (stderr:{first[0]})")
        print(first[2])
    else:
        print("first_event: not found")

    if stall_hit:
        print(f"first_stall: stderr:{stall_hit[0]} {stall_hit[1]}")
    else:
        print("first_stall: not found")

    if null_hit:
        print(f"first_null_call: stderr:{null_hit[0]} {null_hit[1]}")
    else:
        print("first_null_call: not found")

    if bailout_hit:
        print(f"first_null_bailout: stderr:{bailout_hit[0]} {bailout_hit[1]}")
    else:
        print("first_null_bailout: not found")

    print("--- DIVERGENCE CORRIDOR ---")
    if div_events:
        print("first_div_events:")
        for lineno, line in div_events[:8]:
            print(f"stderr:{lineno} {line}")
    else:
        print("first_div_events: none")

    if div_summary:
        lineno, line = div_summary[-1]
        print(f"latest_div_summary: stderr:{lineno} {line}")
    else:
        print("latest_div_summary: none")

    if div_lines:
        print("latest_div_tail:")
        for lineno, line in div_lines[-8:]:
            print(f"stderr:{lineno} {line}")
    else:
        print("latest_div_tail: none")

    if div_call_returns:
        print("latest_div_call_return:")
        lineno, line = div_call_returns[-1]
        print(f"stderr:{lineno} {line}")
    else:
        print("latest_div_call_return: none")

    if div_call_summary:
        print("latest_div_call_summary:")
        lineno, line = div_call_summary[-1]
        print(f"stderr:{lineno} {line}")
    else:
        print("latest_div_call_summary: none")

    print("--- KEY SNAPSHOT ---")
    for token in key_tokens:
        hit = key_hits[token]
        if hit:
            print(f"{token}: stderr:{hit[0]} {hit[1]}")
        else:
            print(f"{token}: not found")

    print("--- REGION TIMELINE ---")
    if region_writes:
        print("first_region_writes:")
        for lineno, line in region_writes[:12]:
            print(f"stderr:{lineno} {line}")
    else:
        print("first_region_writes: none")

    if region_nz2z:
        print("first_region_nz2z:")
        for lineno, line in region_nz2z[:12]:
            print(f"stderr:{lineno} {line}")
    else:
        print("first_region_nz2z: none")

    print("--- REGION SUMMARY ---")
    if region_summary_last:
        for key in sorted(region_summary_last.keys()):
            lineno, line = region_summary_last[key]
            print(f"stderr:{lineno} {line}")
    else:
        print("region_summary: none")

    if region_nz2z_summary_last:
        for key in sorted(region_nz2z_summary_last.keys()):
            lineno, line = region_nz2z_summary_last[key]
            print(f"stderr:{lineno} {line}")
    else:
        print("region_nz2z_summary: none")

    print("--- MMIO TAIL ---")
    if null_mmio_lines:
        tail_count = max(args.mmio_tail, 0)
        for lineno, line in null_mmio_lines[-tail_count:]:
            print(f"stderr:{lineno} {line}")
    elif mmio_lines:
        tail_count = max(args.mmio_tail, 0)
        for lineno, line in mmio_lines[-tail_count:]:
            print(f"stderr:{lineno} {line}")
    else:
        print("mmio_tail: none")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
