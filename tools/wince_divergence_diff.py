#!/usr/bin/env python3
"""Quick diff helper for two WinCE divergence reports."""

from __future__ import annotations

import argparse
from typing import Dict, List


def read_lines(path: str) -> List[str]:
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return [line.rstrip("\n") for line in f]


def extract_key_fields(lines: List[str]) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for line in lines:
        if line.startswith("first_event:"):
            out["first_event"] = line
        elif line.startswith("first_stall:"):
            out["first_stall"] = line
        elif line.startswith("first_null_call:"):
            out["first_null_call"] = line
        elif line.startswith("first_null_bailout:"):
            out["first_null_bailout"] = line
        elif line.startswith("first_div_events:"):
            out["first_div_events"] = line
        elif line.startswith("latest_div_summary:"):
            out["latest_div_summary"] = line
        elif line.startswith("latest_div_call_return:"):
            out["latest_div_call_return"] = line
        elif line.startswith("latest_div_call_summary:"):
            out["latest_div_call_summary"] = line
        elif line.startswith("Kernel loader core:"):
            out["progress_kernel_loader_core"] = line
        elif line.startswith("Start downloading...:"):
            out["progress_start_downloading"] = line
        elif "[WINCE_REGION_SUMMARY]" in line:
            key = line.split("region=", 1)[1].split()[0]
            out[f"region_summary:{key}"] = line
        elif "[WINCE_REGION_NZ2Z_SUMMARY]" in line:
            key = line.split("region=", 1)[1].split()[0]
            out[f"region_nz2z_summary:{key}"] = line
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report_a")
    parser.add_argument("report_b")
    args = parser.parse_args()

    a = extract_key_fields(read_lines(args.report_a))
    b = extract_key_fields(read_lines(args.report_b))

    keys = sorted(set(a.keys()) | set(b.keys()))
    print("--- WINCE DIVERGENCE DIFF ---")
    print(f"A: {args.report_a}")
    print(f"B: {args.report_b}")

    for key in keys:
        av = a.get(key, "<missing>")
        bv = b.get(key, "<missing>")
        status = "same" if av == bv else "diff"
        print(f"[{status}] {key}")
        if status == "diff":
            print(f"  A: {av}")
            print(f"  B: {bv}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
