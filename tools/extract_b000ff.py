#!/usr/bin/env python3
"""Extract a Casio B000FF image into a flat binary for disassembly.

This reproduces loader.c parsing semantics, including the 4-byte sentinel
entry record handling and strict entry validation.
"""

import argparse
import pathlib
import struct
import sys
from dataclasses import dataclass


B000FF_SIG = b"B000FF\n"
B000FF_SIG_LEN = len(B000FF_SIG)


@dataclass
class Record:
    idx: int
    va: int
    pa: int
    length: int
    cksum: int
    data_off: int
    data_end: int


def parse_u32(data: bytes, off: int) -> int:
    if off < 0 or off + 4 > len(data):
        raise ValueError(f"u32 read out of bounds at 0x{off:X}")
    return struct.unpack_from("<I", data, off)[0]


def is_kseg_va(addr: int) -> bool:
    top = addr & 0xE0000000
    return top == 0x80000000 or top == 0xA0000000


def locate_b000ff(data: bytes, image_off: int, scan_window: int) -> int:
    if image_off < 0:
        raise ValueError("offset must be >= 0")
    if image_off + B000FF_SIG_LEN + 8 + 12 > len(data):
        raise ValueError("image too small for B000FF header")

    if data[image_off:image_off + B000FF_SIG_LEN] == B000FF_SIG:
        return image_off

    if scan_window <= 0:
        raise ValueError(
            f"no B000FF signature at offset 0x{image_off:X}"
        )

    scan_end = min(len(data), image_off + scan_window + B000FF_SIG_LEN)
    found = data.find(B000FF_SIG, image_off, scan_end)
    if found < 0:
        raise ValueError(
            f"no B000FF signature in offset window 0x{image_off:X}-0x{scan_end - 1:X}"
        )
    return found


def choose_header_offset(data: bytes, sig_off: int) -> int:
    """Pick the image_start/image_length header offset after B000FF signature.

    SPL uses an immediate header, but NK images can include one or more pad
    bytes between signature and header. We select the first plausible header
    where image_start is kseg and records_end stays within the NAND image.
    """
    sig_end = sig_off + B000FF_SIG_LEN
    best = None

    for pad in range(0, 8):
        hdr = sig_end + pad
        if hdr + 8 > len(data):
            break
        image_start = parse_u32(data, hdr)
        image_length = parse_u32(data, hdr + 4)
        records_start = hdr + 8
        records_end = records_start + image_length
        if not is_kseg_va(image_start):
            continue
        if image_length < 12:
            continue
        if records_end > len(data):
            continue
        best = hdr
        break

    if best is None:
        raise ValueError(
            f"unable to find plausible B000FF header after signature at 0x{sig_off:X}"
        )
    return best


def parse_b000ff(data: bytes, image_off: int):
    hdr_off = choose_header_offset(data, image_off)
    off = hdr_off
    image_start = parse_u32(data, off)
    off += 4
    image_length = parse_u32(data, off)
    off += 4

    records_start = off
    records_end = records_start + image_length
    if records_end > len(data):
        raise ValueError(
            f"B000FF records overflow image: end=0x{records_end:X} size=0x{len(data):X}"
        )

    records = []
    entry_va = None
    entry_cksum = None
    sentinel_skipped = False
    rec_idx = 0

    while off + 12 <= records_end:
        rec_hdr_off = off
        addr = parse_u32(data, off)
        length = parse_u32(data, off + 4)
        cksum = parse_u32(data, off + 8)
        off += 12

        if length == 0:
            entry_va = addr
            entry_cksum = cksum
            break

        if off + length > records_end:
            # Same sentinel rule as loader_load_nand() in src/loader.c
            if addr == 0 and rec_hdr_off + 4 + 12 <= records_end:
                next_off = rec_hdr_off + 4
                next_addr = parse_u32(data, next_off)
                next_len = parse_u32(data, next_off + 4)
                next_cksum = parse_u32(data, next_off + 8)
                if next_len == 0 and is_kseg_va(next_addr) and next_cksum == 0xFFFFFFFF:
                    sentinel_skipped = True
                    entry_va = next_addr
                    entry_cksum = next_cksum
                    break
            raise ValueError(
                f"record {rec_idx} data overflows records window"
            )

        data_off = off
        data_end = off + length
        records.append(
            Record(
                idx=rec_idx,
                va=addr,
                pa=addr & 0x1FFFFFFF,
                length=length,
                cksum=cksum,
                data_off=data_off,
                data_end=data_end,
            )
        )
        off = data_end
        rec_idx += 1

    if entry_va is None:
        entry_va = image_start
        entry_cksum = 0

    if records:
        base_va = min(r.va for r in records) & ~0xFFF
        max_end = max((r.va + r.length) for r in records)
    else:
        base_va = entry_va & ~0xFFF
        max_end = entry_va

    flat_size = max(0, max_end - base_va)
    flat = bytearray(flat_size)

    for r in records:
        start = r.va - base_va
        end = start + r.length
        if start < 0 or end > flat_size:
            raise ValueError(
                f"record {r.idx} VA range outside computed flat image"
            )
        flat[start:end] = data[r.data_off:r.data_end]

    return {
        "mode": "records",
        "image_off": image_off,
        "header_off": hdr_off,
        "header_pad": hdr_off - (image_off + B000FF_SIG_LEN),
        "image_start": image_start,
        "image_length": image_length,
        "records_start": records_start,
        "records_end": records_end,
        "records": records,
        "entry_va": entry_va,
        "entry_cksum": entry_cksum,
        "sentinel_skipped": sentinel_skipped,
        "base_va": base_va,
        "flat": bytes(flat),
        "flat_size": flat_size,
    }


def build_raw_fallback(data: bytes, image_off: int) -> dict:
    """Best-effort fallback for images whose payload is not BIN-record formatted."""
    hdr_off = choose_header_offset(data, image_off)
    image_start = parse_u32(data, hdr_off)
    image_length = parse_u32(data, hdr_off + 4)
    records_start = hdr_off + 8
    records_end = records_start + image_length
    if records_end > len(data):
        raise ValueError(
            f"fallback payload overflows image: end=0x{records_end:X} size=0x{len(data):X}"
        )

    base_va = image_start & ~0xFFF
    payload = data[records_start:records_end]
    pre_pad = image_start - base_va
    flat_size = pre_pad + len(payload)
    flat = bytearray(flat_size)
    flat[pre_pad:pre_pad + len(payload)] = payload

    return {
        "mode": "raw_fallback",
        "image_off": image_off,
        "header_off": hdr_off,
        "header_pad": hdr_off - (image_off + B000FF_SIG_LEN),
        "image_start": image_start,
        "image_length": image_length,
        "records_start": records_start,
        "records_end": records_end,
        "records": [],
        "entry_va": image_start,
        "entry_cksum": 0,
        "sentinel_skipped": False,
        "base_va": base_va,
        "flat": bytes(flat),
        "flat_size": flat_size,
    }


def write_outputs(out_dir: pathlib.Path, image_path: pathlib.Path,
                  image_off: int, parsed: dict, parse_error: str = "") -> None:
    out_dir.mkdir(parents=True, exist_ok=True)

    records_tsv = out_dir / "nk_records.tsv"
    with records_tsv.open("w", encoding="ascii") as f:
        f.write("idx\tva\tpa\tlen\tcksum\tdata_off\tdata_end\n")
        for r in parsed["records"]:
            f.write(
                f"{r.idx}\t0x{r.va:08X}\t0x{r.pa:08X}\t0x{r.length:X}\t"
                f"0x{r.cksum:08X}\t0x{r.data_off:X}\t0x{r.data_end:X}\n"
            )

    (out_dir / "nk_flat.bin").write_bytes(parsed["flat"])

    meta = out_dir / "nk_meta.txt"
    with meta.open("w", encoding="ascii") as f:
        f.write(f"mode={parsed.get('mode', 'records')}\n")
        f.write(f"image_path={image_path}\n")
        f.write(f"requested_image_offset=0x{image_off:X}\n")
        f.write(f"image_offset=0x{parsed['image_off']:X}\n")
        f.write(f"header_offset=0x{parsed['header_off']:X}\n")
        f.write(f"header_pad={parsed['header_pad']}\n")
        f.write(f"image_start=0x{parsed['image_start']:08X}\n")
        f.write(f"image_length=0x{parsed['image_length']:X}\n")
        f.write(f"records_start=0x{parsed['records_start']:X}\n")
        f.write(f"records_end=0x{parsed['records_end']:X}\n")
        f.write(f"record_count={len(parsed['records'])}\n")
        f.write(f"entry_va=0x{parsed['entry_va']:08X}\n")
        f.write(f"entry_cksum=0x{parsed['entry_cksum']:08X}\n")
        f.write(
            f"sentinel_skipped={'1' if parsed['sentinel_skipped'] else '0'}\n"
        )
        f.write(f"base_va=0x{parsed['base_va']:08X}\n")
        f.write(f"flat_size=0x{parsed['flat_size']:X}\n")
        if parse_error:
            f.write(f"parse_error={parse_error}\n")


def parse_int_auto(text: str) -> int:
    return int(text, 0)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--image", required=True,
                    help="Path to NAND image (e.g. ce/restore_images/All_nand_300.bin)")
    ap.add_argument("--offset", type=parse_int_auto, default=0x14000,
                    help="B000FF image offset inside NAND (default: 0x14000)")
    ap.add_argument("--out-dir", required=True,
                    help="Output directory for nk_records.tsv/nk_flat.bin/nk_meta.txt")
    ap.add_argument("--scan-window", type=parse_int_auto, default=0x20,
                    help="If signature is not exactly at --offset, search forward this many bytes (default: 0x20)")
    ap.add_argument("--no-fallback", action="store_true",
                    help="Disable raw payload fallback if BIN-record parsing fails")
    args = ap.parse_args()

    image_path = pathlib.Path(args.image)
    out_dir = pathlib.Path(args.out_dir)

    if not image_path.is_file():
        print(f"error: image not found: {image_path}", file=sys.stderr)
        return 2

    data = image_path.read_bytes()
    parse_error = ""
    try:
        actual_off = locate_b000ff(data, args.offset, args.scan_window)
        parsed = parse_b000ff(data, actual_off)
    except ValueError as exc:
        if args.no_fallback:
            print(f"error: {exc}", file=sys.stderr)
            return 1
        parse_error = str(exc).replace("\n", " ").replace("\t", " ")
        try:
            actual_off = locate_b000ff(data, args.offset, args.scan_window)
            parsed = build_raw_fallback(data, actual_off)
        except ValueError as fexc:
            print(f"error: {fexc}", file=sys.stderr)
            return 1

    write_outputs(out_dir, image_path, args.offset, parsed, parse_error=parse_error)

    print(
        "[extract_b000ff]"
        f" req_off=0x{args.offset:X}"
        f" off=0x{parsed['image_off']:X}"
        f" mode={parsed['mode']}"
        f" records={len(parsed['records'])}"
        f" entry=0x{parsed['entry_va']:08X}"
        f" base=0x{parsed['base_va']:08X}"
        f" flat_size=0x{parsed['flat_size']:X}"
        f" sentinel={1 if parsed['sentinel_skipped'] else 0}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
