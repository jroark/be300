#!/usr/bin/env python3
"""Helpers for the BE-300 compressed NK partition.

The WinCE 3.0 NK partition in ``All_nand_300.bin`` is a standard Casio
``B000FF`` logical record stream, compressed directly with the SPL's LZSS
variant. The SPL stops consuming the logical stream at a terminal control
block ``[addr=0, len=entry_va, cksum=0]``; trailing decompressor output beyond
that point is not part of the logical image and should be ignored.
"""

from __future__ import annotations

from collections import defaultdict, deque
from dataclasses import dataclass
import struct
from typing import Deque, DefaultDict


B000FF_SIG = b"B000FF\n"
ENTRY_BYTES = 12
LZSS_RING_SIZE = 0x1000
LZSS_RING_MASK = LZSS_RING_SIZE - 1
LZSS_START_R = 0x0FEE
LZSS_MAX_MATCH = 18
LZSS_MIN_MATCH = 3


@dataclass(frozen=True)
class NandPartition:
    index: int
    start_sector: int
    sector_count: int

    @property
    def offset(self) -> int:
        return self.start_sector * 512

    @property
    def size_bytes(self) -> int:
        return self.sector_count * 512


@dataclass(frozen=True)
class LogicalRecord:
    index: int
    va: int
    length: int
    checksum: int
    data_off: int


@dataclass(frozen=True)
class LogicalNkImage:
    partition: NandPartition
    image_start: int
    declared_flat_size: int
    entry_va: int
    base_va: int
    flat_size: int
    terminal_off: int
    stream_end: int
    raw_consumed: int
    records: tuple[LogicalRecord, ...]
    logical_stream: bytes
    flat_image: bytes


def _u32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def parse_nand_partition(data: bytes, index: int = 2) -> NandPartition:
    entry_off = index * 16
    if entry_off + 16 > len(data):
        raise ValueError(f"partition entry {index} out of range")

    start_sector = _u32(data, entry_off + 8)
    sector_count = _u32(data, entry_off + 12)
    if start_sector == 0xFFFFFFFF or sector_count == 0xFFFFFFFF:
        raise ValueError(f"partition entry {index} is unused")
    if sector_count == 0:
        raise ValueError(f"partition entry {index} has zero size")

    return NandPartition(
        index=index,
        start_sector=start_sector,
        sector_count=sector_count,
    )


def decode_lzss(src: bytes, output_limit: int | None = None) -> tuple[bytes, int]:
    """Decode the SPL's LZSS stream.

    Returns ``(decoded_bytes, raw_consumed)``. If ``output_limit`` is set, the
    decoder stops as soon as that many output bytes have been produced.
    """

    text = bytearray(LZSS_RING_SIZE)
    ring_index = LZSS_START_R
    flags = 0
    state = 0
    low = 0
    src_off = 0
    out = bytearray()

    while src_off < len(src):
        if output_limit is not None and len(out) >= output_limit:
            break

        if state == 0:
            flags >>= 1
            if (flags & 0x100) == 0:
                flags = src[src_off] | 0xFF00
                src_off += 1
            state = 1
            continue

        if state == 1:
            if flags & 1:
                byte = src[src_off]
                src_off += 1
                out.append(byte)
                text[ring_index] = byte
                ring_index = (ring_index + 1) & LZSS_RING_MASK
                state = 0
                continue

            low = src[src_off]
            src_off += 1
            state = 2
            continue

        encoded = src[src_off]
        src_off += 1
        match_pos = low | ((encoded & 0xF0) << 4)
        match_len = (encoded & 0x0F) + LZSS_MIN_MATCH

        for _ in range(match_len):
            byte = text[match_pos & LZSS_RING_MASK]
            out.append(byte)
            text[ring_index] = byte
            ring_index = (ring_index + 1) & LZSS_RING_MASK
            match_pos += 1
            if output_limit is not None and len(out) >= output_limit:
                break

        state = 0

    return bytes(out), src_off


def _ring_index_for_output_pos(pos: int) -> int:
    return (LZSS_START_R + pos) & LZSS_RING_MASK


def encode_lzss(data: bytes, max_candidates: int = 256) -> bytes:
    """Encode bytes using the SPL's LZSS flavor.

    ``max_candidates`` bounds the backwards search per token. The higher
    default keeps patched NK images inside the stock compressed-stream
    envelope while staying fast enough for build use.
    """

    positions: DefaultDict[bytes, Deque[int]] = defaultdict(deque)
    out = bytearray()
    src_off = 0
    total = len(data)

    def prune_key(key: bytes, min_pos: int) -> Deque[int]:
        dq = positions[key]
        while dq and dq[0] < min_pos:
            dq.popleft()
        return dq

    while src_off < total:
        flags_off = len(out)
        out.append(0)
        flags = 0
        bit = 1

        for _ in range(8):
            if src_off >= total:
                break

            best_len = 0
            best_pos = 0

            if src_off + (LZSS_MIN_MATCH - 1) < total:
                key = data[src_off:src_off + LZSS_MIN_MATCH]
                candidates = prune_key(key, src_off - LZSS_RING_SIZE)
                checked = 0

                for pos in reversed(candidates):
                    checked += 1
                    if checked > max_candidates:
                        break

                    max_len = min(LZSS_MAX_MATCH, total - src_off)
                    match_len = LZSS_MIN_MATCH
                    while (
                        match_len < max_len
                        and data[pos + match_len] == data[src_off + match_len]
                    ):
                        match_len += 1

                    if match_len > best_len:
                        best_len = match_len
                        best_pos = pos
                        if match_len == LZSS_MAX_MATCH:
                            break

            if best_len >= LZSS_MIN_MATCH:
                match_pos = _ring_index_for_output_pos(best_pos)
                out.append(match_pos & 0xFF)
                out.append(((match_pos >> 4) & 0xF0) | (best_len - LZSS_MIN_MATCH))
                step = best_len
            else:
                flags |= bit
                out.append(data[src_off])
                step = 1

            for rel in range(step):
                if src_off + rel + (LZSS_MIN_MATCH - 1) < total:
                    positions[data[src_off + rel:src_off + rel + LZSS_MIN_MATCH]].append(
                        src_off + rel
                    )

            src_off += step
            bit <<= 1

        out[flags_off] = flags

    return bytes(out)


def parse_logical_nk_stream(logical: bytes, partition: NandPartition | None = None) -> LogicalNkImage:
    if len(logical) < len(B000FF_SIG) + 8 + ENTRY_BYTES:
        raise ValueError("logical stream is too small")
    if not logical.startswith(B000FF_SIG):
        raise ValueError("logical stream does not start with B000FF signature")

    image_start = _u32(logical, len(B000FF_SIG))
    declared_flat_size = _u32(logical, len(B000FF_SIG) + 4)

    off = len(B000FF_SIG) + 8
    records: list[LogicalRecord] = []
    entry_va = 0
    terminal_off = -1

    while True:
        if off + ENTRY_BYTES > len(logical):
            raise ValueError(f"record header exceeds logical stream at 0x{off:X}")

        addr = _u32(logical, off)
        length = _u32(logical, off + 4)
        checksum = _u32(logical, off + 8)
        hdr_off = off
        off += ENTRY_BYTES

        if addr == 0:
            entry_va = length
            terminal_off = hdr_off
            break

        data_end = off + length
        if data_end > len(logical):
            raise ValueError(
                f"record {len(records)} overflows logical stream: "
                f"data_end=0x{data_end:X} size=0x{len(logical):X}"
            )

        records.append(
            LogicalRecord(
                index=len(records),
                va=addr,
                length=length,
                checksum=checksum,
                data_off=off,
            )
        )
        off = data_end

    if not records:
        raise ValueError("logical NK stream has no records")

    base_va = min(record.va for record in records) & ~0xFFF
    flat_size = max(record.va + record.length for record in records) - base_va
    flat = bytearray(flat_size)

    for record in records:
        start = record.va - base_va
        flat[start:start + record.length] = logical[record.data_off:record.data_off + record.length]

    if partition is None:
        partition = NandPartition(index=-1, start_sector=0, sector_count=0)

    return LogicalNkImage(
        partition=partition,
        image_start=image_start,
        declared_flat_size=declared_flat_size,
        entry_va=entry_va,
        base_va=base_va,
        flat_size=flat_size,
        terminal_off=terminal_off,
        stream_end=terminal_off + ENTRY_BYTES,
        raw_consumed=0,
        records=tuple(records),
        logical_stream=logical[:terminal_off + ENTRY_BYTES],
        flat_image=bytes(flat),
    )


def decode_nk_partition(nand: bytes, partition_index: int = 2) -> LogicalNkImage:
    partition = parse_nand_partition(nand, partition_index)
    raw_partition = nand[partition.offset:partition.offset + partition.size_bytes]

    logical_full, _ = decode_lzss(raw_partition)
    parsed = parse_logical_nk_stream(logical_full, partition=partition)
    _, raw_consumed = decode_lzss(raw_partition, output_limit=parsed.stream_end)

    return LogicalNkImage(
        partition=partition,
        image_start=parsed.image_start,
        declared_flat_size=parsed.declared_flat_size,
        entry_va=parsed.entry_va,
        base_va=parsed.base_va,
        flat_size=parsed.flat_size,
        terminal_off=parsed.terminal_off,
        stream_end=parsed.stream_end,
        raw_consumed=raw_consumed,
        records=parsed.records,
        logical_stream=parsed.logical_stream,
        flat_image=parsed.flat_image,
    )


def patch_logical_stream_from_flat(image: LogicalNkImage, replacement_flat: bytes) -> bytes:
    if len(replacement_flat) != image.flat_size:
        raise ValueError(
            f"replacement flat NK size mismatch: "
            f"0x{len(replacement_flat):X} != 0x{image.flat_size:X}"
        )

    logical = bytearray(image.logical_stream)
    struct.pack_into("<I", logical, len(B000FF_SIG), image.base_va)
    struct.pack_into("<I", logical, len(B000FF_SIG) + 4, image.flat_size)

    for record in image.records:
        start = record.va - image.base_va
        chunk = replacement_flat[start:start + record.length]
        logical[record.data_off:record.data_off + record.length] = (
            chunk
        )
        checksum_off = record.data_off - ENTRY_BYTES + 8
        struct.pack_into("<I", logical, checksum_off, sum(chunk) & 0xFFFFFFFF)

    struct.pack_into("<I", logical, image.terminal_off + 0, 0)
    struct.pack_into("<I", logical, image.terminal_off + 4, image.entry_va)
    struct.pack_into("<I", logical, image.terminal_off + 8, 0)
    return bytes(logical)
