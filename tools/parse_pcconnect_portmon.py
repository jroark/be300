#!/usr/bin/env python3
"""Parse Sysinternals PortMon logs from Casio PC Connect captures.

PortMon exports seen so far come in two useful forms:

* masked text exports, where non-printable bytes are rendered as "."
* raw hex exports, where payload bytes are printed as "10 01 00 ..."

The raw hex export can still truncate long transfer display columns. This tool
keeps that visible-prefix distinction explicit so investigation output does not
accidentally treat a 21-byte display prefix as a complete 66-byte transfer.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


EVENT_RE = re.compile(
    r"^(?P<line>\d+)\t(?P<dt>[0-9.]+)\t(?P<proc>[^\t]+)\t"
    r"(?P<op>[^\t]+)\t(?P<port>[^\t]+)\t(?P<status>[^\t]+)\t(?P<detail>.*)$"
)
LEN_RE = re.compile(r"Length (?P<length>\d+): (?P<payload>.*)$")
HEX_BYTE_RE = re.compile(r"^[0-9A-Fa-f]{2}$")


@dataclass
class Event:
    line: int
    dt: float
    op: str
    port: str
    status: str
    length: int | None
    payload: str

    @property
    def direction(self) -> str:
        if self.op == "IRP_MJ_WRITE":
            return "host_to_device"
        if self.op == "IRP_MJ_READ":
            return "device_to_host"
        return "control"

    @property
    def visible_text(self) -> str:
        if self.raw_bytes is not None:
            return "".join(chr(b) for b in self.raw_bytes if 0x20 <= b < 0x7f)
        return "".join(ch for ch in self.payload if ch != ".")

    @property
    def raw_bytes(self) -> list[int] | None:
        tokens = self.payload.split()
        if not tokens:
            return [] if self.length == 0 else None
        if not all(HEX_BYTE_RE.match(tok) for tok in tokens):
            return None
        return [int(tok, 16) for tok in tokens]

    @property
    def is_truncated(self) -> bool:
        raw = self.raw_bytes
        return self.length is not None and raw is not None and len(raw) < self.length

    @property
    def byte_pattern(self) -> list[int | None]:
        raw = self.raw_bytes
        if raw is not None:
            return list(raw)
        return [None if ch == "." else ord(ch) for ch in self.payload]

    def to_json(self) -> dict[str, object]:
        raw = self.raw_bytes
        return {
            "line": self.line,
            "delta_seconds": self.dt,
            "direction": self.direction,
            "op": self.op,
            "port": self.port,
            "status": self.status,
            "length": self.length,
            "visible_byte_count": None if raw is None else len(raw),
            "truncated": self.is_truncated,
            "visible_text": self.visible_text,
            "bytes": None if raw is None else [f"{b:02x}" for b in raw],
            "byte_pattern": [
                None if b is None else f"{b:02x}" for b in self.byte_pattern
            ],
        }


def parse_events(path: Path) -> Iterable[Event]:
    with path.open("r", encoding="latin1", newline="") as f:
        for raw in f:
            line = raw.rstrip("\r\n")
            m = EVENT_RE.match(line)
            if not m:
                continue

            detail = m.group("detail")
            length = None
            payload = ""
            lm = LEN_RE.search(detail)
            if lm:
                length = int(lm.group("length"))
                payload = lm.group("payload").rstrip("\t")

            yield Event(
                line=int(m.group("line")),
                dt=float(m.group("dt")),
                op=m.group("op"),
                port=m.group("port"),
                status=m.group("status"),
                length=length,
                payload=payload,
            )


def is_interesting(event: Event) -> bool:
    if event.length is None or event.length == 0:
        return False
    if event.length > 1:
        return True
    return event.visible_text not in {"U", ""}


def format_pattern(event: Event) -> str:
    cells = []
    for byte in event.byte_pattern:
        cells.append("??" if byte is None else f"{byte:02X}")
    if event.is_truncated and event.length is not None:
        cells.append(f"...(+{event.length - len(event.byte_pattern)})")
    return " ".join(cells)


def frame_summary(event: Event) -> str | None:
    raw = event.raw_bytes
    if not raw:
        return None

    data = raw
    skipped_sync = 0
    while data and data[0] == 0x55:
        skipped_sync += 1
        data = data[1:]

    if len(data) < 6 or data[0] not in (0x10, 0x11):
        return None

    declared = (data[2] << 8) | data[3]
    complete = declared <= len(data) and not event.is_truncated
    kind = "host_cmd" if data[0] == 0x10 else "device_rsp"
    extra = ""
    if skipped_sync:
        extra = f" sync_prefix={skipped_sync}"

    return (
        f"{kind} seq=0x{data[1]:02X} len={declared} opcode=0x{data[5]:02X}"
        f" visible={len(data)} complete={complete}{extra}"
    )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("log", type=Path)
    ap.add_argument("--jsonl", action="store_true",
                    help="emit one JSON object per parsed event")
    ap.add_argument("--all", action="store_true",
                    help="include length-zero reads and repetitive sync bytes")
    args = ap.parse_args()

    events = list(parse_events(args.log))
    selected = [e for e in events if args.all or is_interesting(e)]

    if args.jsonl:
        for event in selected:
            print(json.dumps(event.to_json(), sort_keys=True))
        return 0

    reads = sum(1 for e in events if e.op == "IRP_MJ_READ")
    writes = sum(1 for e in events if e.op == "IRP_MJ_WRITE")
    controls = len(events) - reads - writes
    print(f"events={len(events)} reads={reads} writes={writes} controls={controls}")
    print()

    for event in selected:
        length = event.length if event.length is not None else 0
        frame = frame_summary(event)
        frame_text = f" {frame}" if frame else ""
        print(
            f"{event.line:5d} {event.direction:14s} len={length:3d} "
            f"text={event.visible_text!r} pattern={format_pattern(event)}"
            f"{frame_text}"
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())
