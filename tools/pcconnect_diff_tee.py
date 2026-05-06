#!/usr/bin/env python3
r"""Diff a /tmp/pcc.tee bridge log against a real-hardware PortMon capture.

The bridge tee log emits one line per direction-event ::

    441657971988000 H>G 10: 10 22 00 0a 03 46 00 00 00 85
    441657941480000 G>H 1: 20

The PortMon TSV columns (from pcconnect.log / pcconnect_2.log) are::

    line_no  delta_seconds  proc  IRP  port  status  details

For our purposes:
  * ``IRP_MJ_WRITE`` = host writes to its serial port = H->G
  * ``IRP_MJ_READ``  = host reads from its serial port = G->H
  * ``Length N: HH HH ...`` is the byte payload

This script normalizes both into ``[(t_us, dir, bytes)]`` traces, re-bases
both at the first H->G non-empty event (matching the host wake-byte burst on
real hardware), and emits a side-by-side hex tape with a first-divergence
marker. It's read-only; safe to run while the emulator is live.
"""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Event:
    t_us: int        # microseconds, relative to first cable-up byte
    direction: str   # 'H>G' or 'G>H' (drops collapsed)
    payload: bytes


HEX_RE = re.compile(r"^[0-9a-fA-F]+$")


def _hex_tokens_to_bytes(tokens: list[str]) -> bytes:
    out = bytearray()
    for tok in tokens:
        if not HEX_RE.match(tok):
            raise ValueError(f"non-hex token in payload: {tok!r}")
        out.append(int(tok, 16))
    return bytes(out)


def parse_tee(path: Path) -> list[Event]:
    """Parse a pcconnect-bridge tee log into Event objects.

    Drop ``H>G:drop`` events; those are host bytes consumed before the emulated
    cable edge. Treat ``H>G:queued`` as host-to-guest data because the bridge
    retains those bytes and releases them when the guest UART becomes ready.
    Microsecond timestamps are derived from the mono_ns column; the first
    usable event is re-based to t=0 by the caller.
    """
    out: list[Event] = []
    for line in path.read_text(errors="replace").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        # mono_ns dir count: hex bytes
        if len(parts) < 4 or ":" not in parts[2]:
            continue
        try:
            mono_ns = int(parts[0])
        except ValueError:
            continue
        direction = parts[1]
        if direction.endswith(":drop"):
            continue
        if direction == "H>G:queued":
            direction = "H>G"
        if direction not in ("H>G", "G>H"):
            continue
        # parts[2] is "<count>:" — discard, we'll measure
        try:
            payload = _hex_tokens_to_bytes(parts[3:])
        except ValueError:
            continue
        if not payload:
            continue
        out.append(Event(t_us=mono_ns // 1000, direction=direction, payload=payload))
    return out


def parse_portmon(path: Path) -> list[Event]:
    """Parse a PortMon TSV log into Event objects.

    The ``delta_seconds`` column is per-row, not absolute, so we accumulate.
    Only IRP_MJ_WRITE / IRP_MJ_READ rows with a Length payload contribute.
    """
    out: list[Event] = []
    cum_s = 0.0
    detail_re = re.compile(r"Length\s+\d+:\s+([0-9A-Fa-f\s]+?)\s*$")
    for line in path.read_text(errors="replace").splitlines():
        cols = line.split("\t")
        if len(cols) < 7:
            continue
        try:
            cum_s += float(cols[1])
        except ValueError:
            continue
        irp = cols[3]
        if irp not in ("IRP_MJ_WRITE", "IRP_MJ_READ"):
            continue
        m = detail_re.search(cols[6])
        if not m:
            continue
        try:
            payload = _hex_tokens_to_bytes(m.group(1).split())
        except ValueError:
            continue
        if not payload:
            continue
        direction = "H>G" if irp == "IRP_MJ_WRITE" else "G>H"
        out.append(Event(t_us=int(cum_s * 1_000_000), direction=direction, payload=payload))
    return out


def rebase_to_first(events: list[Event]) -> list[Event]:
    """Subtract the first event's timestamp from all events."""
    if not events:
        return events
    base = events[0].t_us
    return [Event(t_us=e.t_us - base, direction=e.direction, payload=e.payload) for e in events]


def trim_until_first_g2h(events: list[Event]) -> list[Event]:
    """Drop leading events up to (but not including) the first G->H event.

    Useful when the emulator capture has a long pre-launch tail of host bytes
    that arrived before AtPcCnct opened the UART. Real hardware starts
    interleaving G->H within milliseconds of the first host wake byte, so
    aligning both streams at the first G->H gives a meaningful diff.
    """
    for i, e in enumerate(events):
        if e.direction == "G>H":
            return events[i:]
    return events


def coalesce_to_byte_stream(events: list[Event]) -> list[tuple[int, str, int]]:
    """Expand multi-byte events into one tuple per byte, preserving order."""
    out: list[tuple[int, str, int]] = []
    for e in events:
        for i, b in enumerate(e.payload):
            # Tee logs already emit one byte per line for typical traffic; the
            # H>G burst from a TCP read() can be many bytes — preserve t.
            out.append((e.t_us, e.direction, b))
        _ = i  # silence pyflakes
    return out


def first_divergence(a: list[tuple[int, str, int]],
                     b: list[tuple[int, str, int]]) -> int:
    """Return the index where the (direction, byte) tuples first differ."""
    n = min(len(a), len(b))
    for i in range(n):
        if a[i][1] != b[i][1] or a[i][2] != b[i][2]:
            return i
    if len(a) != len(b):
        return n
    return -1


def hex_for(byte: int) -> str:
    return f"{byte:02x}"


def fmt_row(idx: int, label_a: str, a: tuple[int, str, int] | None,
            label_b: str, b: tuple[int, str, int] | None,
            marker: str = " ") -> str:
    def cell(label: str, e: tuple[int, str, int] | None) -> str:
        if e is None:
            return f"{label}: ----"
        t_us, d, by = e
        ms = t_us / 1000.0
        return f"{label}: {ms:9.2f}ms {d} {hex_for(by)}"

    return f"{marker} {idx:5d}  {cell(label_a, a):42s} | {cell(label_b, b)}"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Side-by-side diff of a pcconnect-bridge tee vs. a PortMon capture."
    )
    parser.add_argument(
        "--tee",
        default="/tmp/pcc.tee",
        help="Bridge tee log (default: /tmp/pcc.tee).",
    )
    parser.add_argument(
        "--portmon",
        default="pcconnect_2.log",
        help="Real-hardware PortMon TSV log (default: pcconnect_2.log).",
    )
    parser.add_argument(
        "--rows",
        type=int,
        default=120,
        help="Maximum rows to print (default: 120).",
    )
    parser.add_argument(
        "--around-divergence",
        type=int,
        default=8,
        help="Rows of context around the first divergence (default: 8).",
    )
    parser.add_argument(
        "--align-on-g2h",
        action="store_true",
        help="Trim leading H>G-only prefix from both streams; align at first "
             "G>H byte. Useful when emulator capture has a long pre-launch "
             "host-only tail.",
    )
    args = parser.parse_args()

    tee_path = Path(args.tee)
    portmon_path = Path(args.portmon)
    if not tee_path.exists():
        raise SystemExit(f"error: tee log not found: {tee_path}")
    if not portmon_path.exists():
        raise SystemExit(f"error: portmon log not found: {portmon_path}")

    tee_events = parse_tee(tee_path)
    pm_events = parse_portmon(portmon_path)
    if args.align_on_g2h:
        tee_events = trim_until_first_g2h(tee_events)
        pm_events = trim_until_first_g2h(pm_events)
    tee_events = rebase_to_first(tee_events)
    pm_events = rebase_to_first(pm_events)
    tee_stream = coalesce_to_byte_stream(tee_events)
    pm_stream = coalesce_to_byte_stream(pm_events)

    print(f"# tee:     {tee_path}  events={len(tee_events)}  bytes={len(tee_stream)}")
    print(f"# portmon: {portmon_path}  events={len(pm_events)}  bytes={len(pm_stream)}")

    div = first_divergence(tee_stream, pm_stream)
    if div < 0:
        print("# streams match within shared prefix length")
        upper = min(args.rows, len(tee_stream), len(pm_stream))
        for i in range(upper):
            print(fmt_row(i, "tee", tee_stream[i], "real", pm_stream[i]))
        return 0

    print(f"# first divergence at byte index {div}")
    a_at = tee_stream[div] if div < len(tee_stream) else None
    b_at = pm_stream[div] if div < len(pm_stream) else None
    print(f"#   tee[{div}]={a_at}  real[{div}]={b_at}")
    print()

    lo = max(0, div - args.around_divergence)
    hi = min(div + args.around_divergence + 1,
             max(len(tee_stream), len(pm_stream)))
    for i in range(lo, hi):
        a_i = tee_stream[i] if i < len(tee_stream) else None
        b_i = pm_stream[i] if i < len(pm_stream) else None
        marker = "*" if i == div else " "
        print(fmt_row(i, "tee", a_i, "real", b_i, marker))

    print()
    print("# extending tee for", min(args.rows, len(tee_stream) - div), "bytes from divergence:")
    for i in range(div, min(div + args.rows, len(tee_stream))):
        print(fmt_row(i, "tee", tee_stream[i], "real",
                      pm_stream[i] if i < len(pm_stream) else None))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
