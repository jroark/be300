#!/usr/bin/env python3
"""Relocate a WinCE XIP-module sidecar to a new vbase for Ghidra loading.

Reads a .txt sidecar emitted by tools/extract_xip_modules.py and rewrites
it so `vbase:` and every section row's `vstart` are shifted by --offset.
Use when loading multiple process-private EXEs (all sharing vbase
0x00010000) into one Ghidra program: relocate filesys to slot 1
(offset 0x02000000), gwes to slot 2 (0x04000000), etc.

The referenced .bin file is NOT modified; only the sidecar. Pair the
relocated sidecar with an identically-basenamed copy of the .bin so the
Ghidra loader script (tools/ghidra_load_xip_module.py) picks them up as
a pair.

Usage:
    python3 tools/relocate_sidecar.py \
        --in  build-host/modules/02_filesys.exe.txt \
        --out build-host/modules/02_filesys_slot1.txt \
        --offset 0x02000000
"""

import argparse
import pathlib
import re


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--in", dest="inp", required=True, type=pathlib.Path)
    p.add_argument("--out", dest="out", required=True, type=pathlib.Path)
    p.add_argument("--offset", required=True, type=lambda s: int(s, 0))
    args = p.parse_args()

    lines = args.inp.read_text().splitlines()
    out = []
    section_row = re.compile(r"^(\s*\d+\s+\S+\s+\S+\s+)0x([0-9A-Fa-f]+)(\s+.*)$")
    for line in lines:
        if line.startswith("vbase:"):
            # "vbase:  0x00010000"
            parts = line.split()
            vbase = int(parts[1], 16) + args.offset
            out.append(f"vbase:  0x{vbase:08X}")
            continue
        m = section_row.match(line)
        if m:
            vstart = int(m.group(2), 16) + args.offset
            out.append(f"{m.group(1)}0x{vstart:08X}{m.group(3)}")
            continue
        out.append(line)
    args.out.write_text("\n".join(out) + "\n")
    print(f"Wrote {args.out} (offset +0x{args.offset:08X})")


if __name__ == "__main__":
    main()
