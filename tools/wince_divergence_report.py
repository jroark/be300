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
    div_call_steps = collect_lines(err_lines, "[WINCE_DIV_CALL_STEP]")
    div_call_repeats = collect_lines(err_lines, "[WINCE_DIV_CALL_REPEAT]")
    div_call_returns = collect_lines(err_lines, "[WINCE_DIV_CALL_RETURN]")
    div_call_summary = collect_lines(err_lines, "[WINCE_DIV_CALL_SUMMARY]")
    div_stack_arm = collect_lines(err_lines, "[WINCE_DIV_STACK_ARM]")
    div_stack_summary = collect_lines(err_lines, "[WINCE_DIV_STACK_SUMMARY]")
    div_stack_slot_summary = collect_lines(err_lines, "[WINCE_DIV_STACK_SLOT_SUMMARY]")
    div_stack_slot_timeline = collect_lines(err_lines, "[WINCE_DIV_STACK_SLOT_TIMELINE]")
    div_stack_window_summary = collect_lines(err_lines, "[WINCE_DIV_STACK_WINDOW_SUMMARY]")
    div_stack_phase_summary = collect_lines(err_lines, "[WINCE_DIV_STACK_PHASE_SUMMARY]")
    mmio_lines = collect_lines(err_lines, "[WINCE_STALL_MMIO]")
    null_mmio_lines = collect_lines(err_lines, "[WINCE_NULL_MMIO]")
    replay_redirect_hit = first_contains(err_lines, "[BE300] Resume replay:")
    replay_prepared_hits = collect_lines(err_lines, "[WINCE_REPLAY] prepared")
    replay_pc_hits = collect_lines(err_lines, "[WINCE_REPLAY_PC]")
    replay_write_hits = collect_lines(err_lines, "[WINCE_REPLAY_WRITE]")
    replay_ra_hits = collect_lines(err_lines, "[WINCE_REPLAY_RA]")
    replay_cmp_hits = collect_lines(err_lines, "[WINCE_REPLAY_CMP]")

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

    replay_cmp_last: Dict[str, LineHit] = {}
    for lineno, line in replay_cmp_hits:
        m = re.search(r"region=([^ ]+)", line)
        if not m:
            continue
        replay_cmp_last[m.group(1)] = (lineno, line)

    replay_first_mismatch: Optional[LineHit] = None
    for hit in replay_write_hits:
        if "kind=first-mismatch" in hit[1]:
            replay_first_mismatch = hit
            break

    replay_first_pc = replay_pc_hits[0] if replay_pc_hits else None
    replay_first_bev: Optional[LineHit] = None
    replay_stub_return: Optional[LineHit] = None
    replay_first_corridor: Optional[LineHit] = None
    moved_past_stub = False
    stub_return_ra_nonzero: Optional[bool] = None
    for lineno, line in replay_pc_hits:
        if "label=resume_stub_return" in line and replay_stub_return is None:
            replay_stub_return = (lineno, line)
            ra_match = re.search(r" ra=0x([0-9A-Fa-f]+)", line)
            if ra_match:
                stub_return_ra_nonzero = int(ra_match.group(1), 16) != 0
        if "label=bev_" in line and replay_first_bev is None:
            replay_first_bev = (lineno, line)
        if "label=corridor_" in line and replay_first_corridor is None:
            replay_first_corridor = (lineno, line)
            moved_past_stub = True

    last_loop_hit: Optional[LineHit] = None
    loop_pc_re = re.compile(r"^\[BE300\] Loop batch \d+, PC=(0x[0-9A-Fa-f]+)")
    for lineno, line in enumerate(err_lines, start=1):
        if loop_pc_re.search(line):
            last_loop_hit = (lineno, line)

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

    print("--- REPLAY ---")
    if replay_redirect_hit:
        print(f"resume_replay_redirect: stderr:{replay_redirect_hit[0]} {replay_redirect_hit[1]}")
    else:
        print("resume_replay_redirect: not found")

    if replay_prepared_hits:
        lineno, line = replay_prepared_hits[-1]
        print(f"resume_replay_prepared: stderr:{lineno} {line}")
    else:
        print("resume_replay_prepared: not found")

    if replay_first_pc:
        print(f"first_replay_pc: stderr:{replay_first_pc[0]} {replay_first_pc[1]}")
    else:
        print("first_replay_pc: not found")

    if replay_first_bev:
        print(f"first_replay_bev: stderr:{replay_first_bev[0]} {replay_first_bev[1]}")
    else:
        print("first_replay_bev: not found")

    if replay_stub_return:
        print(f"resume_stub_return: stderr:{replay_stub_return[0]} {replay_stub_return[1]}")
    else:
        print("resume_stub_return: not reached")

    if stub_return_ra_nonzero is None:
        print("resume_stub_return_nonzero_ra: unknown")
    else:
        print(f"resume_stub_return_nonzero_ra: {'yes' if stub_return_ra_nonzero else 'no'}")

    if replay_first_corridor:
        print(f"first_replay_corridor: stderr:{replay_first_corridor[0]} {replay_first_corridor[1]}")
    else:
        print("first_replay_corridor: not found")

    print(f"moved_past_117a8: {'yes' if moved_past_stub else 'no'}")

    if replay_first_mismatch:
        print(f"first_replay_mismatch: stderr:{replay_first_mismatch[0]} {replay_first_mismatch[1]}")
    else:
        print("first_replay_mismatch: none")

    if "resume_context_22a0" in replay_cmp_last:
        lineno, line = replay_cmp_last["resume_context_22a0"]
        print(f"latest_replay_cmp_resume_context_22a0: stderr:{lineno} {line}")
    else:
        print("latest_replay_cmp_resume_context_22a0: none")

    if "stack_frame_1770" in replay_cmp_last:
        lineno, line = replay_cmp_last["stack_frame_1770"]
        print(f"latest_replay_cmp_stack_frame_1770: stderr:{lineno} {line}")
    else:
        print("latest_replay_cmp_stack_frame_1770: none")

    if replay_ra_hits:
        lineno, line = replay_ra_hits[-1]
        print(f"latest_replay_ra: stderr:{lineno} {line}")
    else:
        print("latest_replay_ra: none")

    if last_loop_hit:
        print(f"last_loop_pc: stderr:{last_loop_hit[0]} {last_loop_hit[1]}")
    else:
        print("last_loop_pc: none")

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

    if div_call_steps:
        print("latest_div_call_steps:")
        for lineno, line in div_call_steps[-10:]:
            print(f"stderr:{lineno} {line}")
    else:
        print("latest_div_call_steps: none")

    if div_call_repeats:
        print("latest_div_call_repeats:")
        for lineno, line in div_call_repeats[-4:]:
            print(f"stderr:{lineno} {line}")
    else:
        print("latest_div_call_repeats: none")

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

    if div_stack_arm:
        print("latest_div_stack_arm:")
        lineno, line = div_stack_arm[-1]
        print(f"stderr:{lineno} {line}")
    else:
        print("latest_div_stack_arm: none")

    if div_stack_summary:
        print("latest_div_stack_summary:")
        lineno, line = div_stack_summary[-1]
        print(f"stderr:{lineno} {line}")
    else:
        print("latest_div_stack_summary: none")

    if div_stack_slot_summary:
        print("latest_div_stack_slots:")
        for lineno, line in div_stack_slot_summary[-4:]:
            print(f"stderr:{lineno} {line}")
    else:
        print("latest_div_stack_slots: none")

    if div_stack_slot_timeline:
        print("latest_div_stack_timeline:")
        for lineno, line in div_stack_slot_timeline[-4:]:
            print(f"stderr:{lineno} {line}")
    else:
        print("latest_div_stack_timeline: none")

    if div_stack_window_summary:
        print("latest_div_stack_windows:")
        for lineno, line in div_stack_window_summary[-4:]:
            print(f"stderr:{lineno} {line}")
    else:
        print("latest_div_stack_windows: none")

    if div_stack_phase_summary:
        print("latest_div_stack_phases:")
        for lineno, line in div_stack_phase_summary[-4:]:
            print(f"stderr:{lineno} {line}")
    else:
        print("latest_div_stack_phases: none")

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
