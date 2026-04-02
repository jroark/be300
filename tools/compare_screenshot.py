#!/usr/bin/env python3
"""Compare an emulator screenshot against baseline reference images.

Usage:
    python3 tools/compare_screenshot.py <screenshot.bmp> [--baselines-dir DIR]

Compares the given BMP screenshot against baseline images (Starting.bmp,
Initializing.bmp) in the project root (or specified directory).  Reports
pixel-level similarity and whether the screenshot matches any baseline.

Exit codes:
    0 — matches a baseline
    1 — no match (or error)
"""

import argparse
import os
import struct
import sys


def read_bmp_pixels(path):
    """Read a BMP file and return (width, height, pixel_data_bytes)."""
    with open(path, "rb") as f:
        sig = f.read(2)
        if sig != b"BM":
            raise ValueError(f"{path}: not a BMP file")
        f.seek(10)
        offset = struct.unpack("<I", f.read(4))[0]
        f.seek(18)
        width = struct.unpack("<i", f.read(4))[0]
        height = struct.unpack("<i", f.read(4))[0]
        f.seek(28)
        bpp = struct.unpack("<H", f.read(2))[0]
        f.seek(offset)
        data = f.read()
    return width, height, bpp, data


def pixel_similarity(data_a, data_b):
    """Return fraction of identical bytes between two pixel buffers."""
    if len(data_a) != len(data_b):
        return 0.0
    if len(data_a) == 0:
        return 1.0
    matching = sum(1 for a, b in zip(data_a, data_b) if a == b)
    return matching / len(data_a)


def nonzero_pct(data):
    """Return percentage of non-zero bytes."""
    if len(data) == 0:
        return 0.0
    return 100.0 * sum(1 for b in data if b != 0) / len(data)


def main():
    parser = argparse.ArgumentParser(
        description="Compare emulator screenshot against baselines"
    )
    parser.add_argument("screenshot", help="Path to screenshot BMP")
    parser.add_argument(
        "--baselines-dir",
        default=None,
        help="Directory containing baseline BMPs (default: project root)",
    )
    parser.add_argument(
        "--threshold",
        type=float,
        default=99.5,
        help="Similarity threshold for match (default: 99.5%%)",
    )
    args = parser.parse_args()

    if args.baselines_dir is None:
        # Default: project root (two levels up from tools/)
        args.baselines_dir = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), ".."
        )

    baselines = {}
    for name in ["Starting.bmp", "Initializing.bmp"]:
        path = os.path.join(args.baselines_dir, name)
        if os.path.exists(path):
            try:
                w, h, bpp, data = read_bmp_pixels(path)
                baselines[name] = (w, h, bpp, data)
            except Exception as e:
                print(f"WARNING: could not read baseline {path}: {e}", file=sys.stderr)

    if not baselines:
        print("ERROR: no baseline images found", file=sys.stderr)
        return 1

    try:
        sw, sh, sbpp, sdata = read_bmp_pixels(args.screenshot)
    except Exception as e:
        print(f"ERROR: could not read screenshot: {e}", file=sys.stderr)
        return 1

    snz = nonzero_pct(sdata)
    print(f"Screenshot: {args.screenshot}")
    print(f"  Size: {sw}x{sh} {sbpp}bpp, {len(sdata)} bytes")
    print(f"  Non-zero: {snz:.1f}%")
    print()

    best_match = None
    best_sim = 0.0

    for name, (bw, bh, bbpp, bdata) in sorted(baselines.items()):
        bnz = nonzero_pct(bdata)
        if sw != bw or sh != bh or sbpp != bbpp:
            print(f"  vs {name}: DIMENSION MISMATCH ({bw}x{bh} {bbpp}bpp)")
            continue
        sim = pixel_similarity(sdata, bdata) * 100.0
        exact = sdata == bdata
        status = "EXACT MATCH" if exact else (
            "MATCH" if sim >= args.threshold else "no match"
        )
        print(f"  vs {name}: {sim:.1f}% similar, non-zero={bnz:.1f}% [{status}]")
        if sim > best_sim:
            best_sim = sim
            best_match = name if sim >= args.threshold else None

    print()
    if best_match:
        print(f"RESULT: matches {best_match} ({best_sim:.1f}%)")
        return 0
    else:
        print(f"RESULT: no baseline match (best={best_sim:.1f}%)")
        return 1


if __name__ == "__main__":
    sys.exit(main())
