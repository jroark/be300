#!/usr/bin/env python3
"""Summarize bounded WinCE TLB/refill diagnostics from stderr logs."""

from __future__ import annotations

import argparse
import collections
import re
from pathlib import Path


HEX = r"0x[0-9A-Fa-f]+"
POST_RE = re.compile(
    rf"\[WINCE_TLB_POST\].*fault=({HEX}).*vector_pc=({HEX}).*"
    rf"epc=({HEX}).*badvaddr=({HEX}).*entryhi=({HEX}).*"
    rf"context=({HEX}).*sec\[(\d+)\]=({HEX}).*l2=({HEX}).*"
    rf"lo0=({HEX}).*lo1=({HEX}).*repeat=(\d+)"
)
SECTION_RE = re.compile(
    rf"\[WINCE_SECTION_WRITE\] idx=(\d+) old=({HEX}) new=({HEX})"
    rf" PC=({HEX}) RA=({HEX})"
)
L2_STATE_RE = re.compile(
    rf"\[WINCE_L2_STATE\] (\S+) table=({HEX}).*focus_va=({HEX}).*"
    rf"focus=({HEX}).*dll600=({HEX}).*dll694=({HEX}).*dll7f8=({HEX})"
)
L2_WRITE_RE = re.compile(
    rf"\[WINCE_L2_WRITE\] table=(\S+) table_va=({HEX})"
    rf" off=({HEX}) new=({HEX}) PC=({HEX}) RA=({HEX})"
)


def parse_hex(value: str) -> int:
    return int(value, 16)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", type=Path)
    args = ap.parse_args()

    posts = []
    section_writes = []
    l2_states = []
    l2_writes = []

    with args.log.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if m := POST_RE.search(line):
                posts.append(
                    {
                        "fault": parse_hex(m.group(1)),
                        "vector_pc": parse_hex(m.group(2)),
                        "epc": parse_hex(m.group(3)),
                        "badvaddr": parse_hex(m.group(4)),
                        "entryhi": parse_hex(m.group(5)),
                        "context": parse_hex(m.group(6)),
                        "section": int(m.group(7)),
                        "section_val": parse_hex(m.group(8)),
                        "l2": parse_hex(m.group(9)),
                        "lo0": parse_hex(m.group(10)),
                        "lo1": parse_hex(m.group(11)),
                        "repeat": int(m.group(12)),
                    }
                )
            elif m := SECTION_RE.search(line):
                section_writes.append(
                    {
                        "idx": int(m.group(1)),
                        "old": parse_hex(m.group(2)),
                        "new": parse_hex(m.group(3)),
                        "pc": parse_hex(m.group(4)),
                        "ra": parse_hex(m.group(5)),
                    }
                )
            elif m := L2_STATE_RE.search(line):
                l2_states.append(
                    {
                        "tag": m.group(1),
                        "table": parse_hex(m.group(2)),
                        "focus_va": parse_hex(m.group(3)),
                        "focus": parse_hex(m.group(4)),
                        "dll600": parse_hex(m.group(5)),
                        "dll694": parse_hex(m.group(6)),
                        "dll7f8": parse_hex(m.group(7)),
                    }
                )
            elif m := L2_WRITE_RE.search(line):
                l2_writes.append(
                    {
                        "table_name": m.group(1),
                        "table": parse_hex(m.group(2)),
                        "off": parse_hex(m.group(3)),
                        "new": parse_hex(m.group(4)),
                        "pc": parse_hex(m.group(5)),
                        "ra": parse_hex(m.group(6)),
                    }
                )

    print(f"log={args.log}")
    print(f"tlb_post={len(posts)} section_writes={len(section_writes)} "
          f"l2_states={len(l2_states)} l2_writes={len(l2_writes)}")

    if section_writes:
        print("section writes:")
        for item in section_writes[:12]:
            print(
                f"  idx={item['idx']} old=0x{item['old']:08X} "
                f"new=0x{item['new']:08X} pc=0x{item['pc']:08X} "
                f"ra=0x{item['ra']:08X}"
            )

    if l2_states:
        print("l2 table states:")
        seen = set()
        for item in l2_states:
            key = (item["tag"], item["table"], item["focus_va"])
            if key in seen:
                continue
            seen.add(key)
            print(
                f"  {item['tag']} table=0x{item['table']:08X} "
                f"focus_va=0x{item['focus_va']:08X} "
                f"focus=0x{item['focus']:08X} "
                f"dll600=0x{item['dll600']:08X} "
                f"dll694=0x{item['dll694']:08X} "
                f"dll7f8=0x{item['dll7f8']:08X}"
            )
            if len(seen) >= 16:
                break

    if l2_writes:
        print("l2 writes:")
        for item in l2_writes[:16]:
            print(
                f"  {item['table_name']} table=0x{item['table']:08X} "
                f"off=0x{item['off']:03X} new=0x{item['new']:08X} "
                f"pc=0x{item['pc']:08X} ra=0x{item['ra']:08X}"
            )

    if posts:
        by_fault = collections.Counter(item["fault"] for item in posts)
        by_section = collections.Counter(item["section"] for item in posts)
        missing_l2 = sum(1 for item in posts if item["l2"] == 0)
        missing_pte = sum(
            1 for item in posts
            if item["l2"] & 0x80000000 and item["lo0"] == 0
            and item["lo1"] == 0
        )
        invalid_even = sum(
            1 for item in posts
            if item["l2"] & 0x80000000 and item["lo0"] == 0
            and item["lo1"] != 0
        )
        print("tlb post summary:")
        print(
            f"  unique_faults={len(by_fault)} sections={dict(by_section)} "
            f"missing_l2={missing_l2} missing_pte={missing_pte} "
            f"invalid_even_valid_odd={invalid_even}"
        )
        print("  first faults:")
        for item in posts[:12]:
            print(
                f"    fault=0x{item['fault']:08X} epc=0x{item['epc']:08X} "
                f"ctx=0x{item['context']:08X} sec{item['section']}="
                f"0x{item['section_val']:08X} l2=0x{item['l2']:08X} "
                f"lo0=0x{item['lo0']:08X} lo1=0x{item['lo1']:08X} "
                f"repeat={item['repeat']}"
            )
        print("  most common faults:")
        for fault, count in by_fault.most_common(12):
            print(f"    0x{fault:08X}: {count}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
