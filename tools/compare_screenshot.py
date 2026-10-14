#!/usr/bin/env python3
"""Compare an emulator BMP screenshot with the boot-stage BMP baselines.

The emulator saves RGB565 while the hardware reference images are RGB24, so
both formats are normalized to RGB888 before calculating pixel similarity.
"""

import argparse
import os
import struct
import sys


def _scale_masked(value, mask):
    if not mask:
        return 0
    shift = (mask & -mask).bit_length() - 1
    maximum = mask >> shift
    return ((value & mask) >> shift) * 255 // maximum


def read_bmp_pixels(path):
    """Return ``(width, height, source_bpp, rgb888_bytes)`` for a BMP."""
    with open(path, "rb") as stream:
        blob = stream.read()

    if len(blob) < 54 or blob[:2] != b"BM":
        raise ValueError(f"{path}: not a supported BMP file")
    pixel_offset = struct.unpack_from("<I", blob, 10)[0]
    dib_size = struct.unpack_from("<I", blob, 14)[0]
    if dib_size < 40 or len(blob) < 14 + dib_size:
        raise ValueError(f"{path}: unsupported BMP DIB header")

    width, signed_height = struct.unpack_from("<ii", blob, 18)
    planes, bpp = struct.unpack_from("<HH", blob, 26)
    compression = struct.unpack_from("<I", blob, 30)[0]
    if planes != 1 or width <= 0 or signed_height == 0:
        raise ValueError(f"{path}: invalid BMP geometry")
    if bpp not in (16, 24, 32):
        raise ValueError(f"{path}: unsupported {bpp}-bpp BMP")
    if compression not in (0, 3):
        raise ValueError(f"{path}: unsupported BMP compression {compression}")

    height = abs(signed_height)
    top_down = signed_height < 0
    row_size = ((width * bpp + 31) // 32) * 4
    if pixel_offset + row_size * height > len(blob):
        raise ValueError(f"{path}: truncated BMP pixel data")

    if bpp == 16:
        if compression == 3:
            mask_offset = 14 + 40
            if mask_offset + 12 > pixel_offset:
                raise ValueError(f"{path}: missing RGB bit masks")
            red_mask, green_mask, blue_mask = struct.unpack_from(
                "<III", blob, mask_offset
            )
        else:
            red_mask, green_mask, blue_mask = 0x7C00, 0x03E0, 0x001F

    rgb = bytearray(width * height * 3)
    for y in range(height):
        source_y = y if top_down else height - 1 - y
        row = pixel_offset + source_y * row_size
        for x in range(width):
            if bpp == 16:
                value = struct.unpack_from("<H", blob, row + x * 2)[0]
                r = _scale_masked(value, red_mask)
                g = _scale_masked(value, green_mask)
                b = _scale_masked(value, blue_mask)
            elif bpp == 24:
                b, g, r = blob[row + x * 3 : row + x * 3 + 3]
            else:
                b, g, r, _ = blob[row + x * 4 : row + x * 4 + 4]
            dest = (y * width + x) * 3
            rgb[dest : dest + 3] = bytes((r, g, b))
    return width, height, bpp, bytes(rgb)


def pixel_similarity(data_a, data_b):
    """Return RGB channel similarity from 0.0 to 1.0."""
    if len(data_a) != len(data_b) or not data_a:
        return 1.0 if data_a == data_b else 0.0
    error = sum(abs(a - b) for a, b in zip(data_a, data_b))
    return 1.0 - error / (255.0 * len(data_a))


def nonzero_pct(data):
    if not data:
        return 0.0
    pixels = len(data) // 3
    nonblack = sum(
        1 for i in range(0, len(data), 3) if data[i] or data[i + 1] or data[i + 2]
    )
    return 100.0 * nonblack / pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("screenshot", help="Path to screenshot BMP")
    parser.add_argument(
        "--baselines-dir",
        default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."),
        help="Directory containing Starting.bmp and Initializing.bmp",
    )
    parser.add_argument(
        "--threshold",
        type=float,
        default=99.5,
        help="Similarity threshold for a match (default: 99.5%%)",
    )
    args = parser.parse_args()

    baselines = {}
    for name in ("Starting.bmp", "Initializing.bmp"):
        path = os.path.join(args.baselines_dir, name)
        if os.path.exists(path):
            try:
                baselines[name] = read_bmp_pixels(path)
            except Exception as exc:
                print(f"WARNING: could not read baseline {path}: {exc}", file=sys.stderr)
    if not baselines:
        print("ERROR: no baseline images found", file=sys.stderr)
        return 2

    try:
        sw, sh, sbpp, sdata = read_bmp_pixels(args.screenshot)
    except Exception as exc:
        print(f"ERROR: could not read screenshot: {exc}", file=sys.stderr)
        return 2

    print(f"Screenshot: {args.screenshot}")
    print(f"  Size: {sw}x{sh} {sbpp}bpp, normalized RGB bytes={len(sdata)}")
    print(f"  Non-black pixels: {nonzero_pct(sdata):.1f}%\n")

    best_name = None
    best_similarity = 0.0
    for name, (bw, bh, bbpp, bdata) in sorted(baselines.items()):
        if (sw, sh) != (bw, bh):
            print(f"  vs {name}: DIMENSION MISMATCH ({bw}x{bh} {bbpp}bpp)")
            continue
        similarity = pixel_similarity(sdata, bdata) * 100.0
        status = "MATCH" if similarity >= args.threshold else "no match"
        print(f"  vs {name}: {similarity:.2f}% similar [{status}]")
        if similarity > best_similarity:
            best_similarity = similarity
            best_name = name

    print()
    if best_name and best_similarity >= args.threshold:
        print(f"RESULT: matches {best_name} ({best_similarity:.2f}%)")
        return 0
    print(f"RESULT: no baseline match (best={best_similarity:.2f}%)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
