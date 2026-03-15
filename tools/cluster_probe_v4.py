#!/usr/bin/env python3
from __future__ import annotations

import argparse
import glob
import json
import re
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


PHASES = ["early", "settle", "post"]
REGION_NAMES = [
    "resume_context_2200",
    "stack_frame_1700",
    "callback_table_51600",
    "callback_globals_80679400",
    "vrc4173_base",
    "vr4131_bcu_window",
    "vr4131_icu_pmu_window",
]
FOCUS_LABELS = [
    "nand_sideband_c38",
    "nand_c30",
    "nand_c34",
    "nand_c48",
    "nand_c4c",
    "icu_0080",
    "pmu_0100",
    "ctx_2220",
    "ctx_2228",
    "ctx_2274",
    "stack_1760",
    "stack_1764",
    "stack_1788",
    "stack_179C",
    "stack_17B4",
    "g94EC",
    "g94F0",
    "g9508",
    "g9510",
]
DIFF_PAIRS = ["early_to_settle", "settle_to_post", "early_to_post"]
BOOT_TAGS = {
    "Cold battery boot": "cold",
    "Warm retained boot": "warm",
    "Unknown": "unknown",
}


@dataclass
class FieldSummary:
    kind: str
    name: str
    stable_across_runs_and_phases: bool
    phase_varying_within_run: bool
    family_count: int
    correlation: str
    signatures: Dict[str, List[str]]

def default_paths() -> Tuple[Path, Path, Path]:
    repo_root = Path(__file__).resolve().parents[1]
    return (
        repo_root / "hardware_survey",
        repo_root / "hardware_survey" / "BE300Probe_v4_summary.json",
        repo_root / "hardware_survey" / "BE300Probe_v4_cluster_report.md",
    )


def make_parser() -> argparse.ArgumentParser:
    survey_dir, json_out, md_out = default_paths()
    parser = argparse.ArgumentParser(
        description="Parse and cluster BE300Probe_v4 hardware survey outputs."
    )
    parser.add_argument(
        "--glob",
        default=str(survey_dir / "BE300Probe_v4_*.txt"),
        help="Glob for v4 report files.",
    )
    parser.add_argument(
        "--json-out",
        default=str(json_out),
        help="Path to machine-readable JSON summary.",
    )
    parser.add_argument(
        "--md-out",
        default=str(md_out),
        help="Path to human-readable Markdown report.",
    )
    return parser


def parse_report(path: Path) -> Dict[str, object]:
    text = path.read_text(encoding="utf-8", errors="replace")
    run: Dict[str, object] = {
        "filename": path.name,
        "path": str(path),
        "boot_tag": None,
        "boot_tag_short": None,
        "output_path": None,
        "probe_complete_status": None,
        "old_fault_0f0200": "[READ_FAULT] PA=0x0F000200" in text,
        "nand": {"status": "ABSENT", "selected_file": None, "found_by": None},
        "storage": {"status": "ABSENT", "selected_file": None, "found_by": None},
        "regions": {name: {} for name in REGION_NAMES},
        "focus": {},
        "utf16_hint": {},
        "diff_totals": {},
    }

    def capture(pattern: str) -> Optional[re.Match[str]]:
        return re.search(pattern, text, re.MULTILINE)

    match = capture(r'^boot_tag="([^"]*)"')
    if match:
        run["boot_tag"] = match.group(1)
        run["boot_tag_short"] = BOOT_TAGS.get(match.group(1), "unknown")

    match = capture(r'^output_path="([^"]*)"')
    if match:
        run["output_path"] = match.group(1)

    match = capture(r"probe_complete status=([A-Z_]+)")
    if match:
        run["probe_complete_status"] = match.group(1)

    workload_pattern = re.compile(
        r'^\[(NAND|STORAGE)\] root="([^"]*)" selected_file="([^"]*)" found_by=([a-z_]+) loops=(\d+)$',
        re.MULTILINE,
    )
    for workload_match in workload_pattern.finditer(text):
        section = workload_match.group(1).lower()
        run[section] = {
            "status": "OK",
            "root": workload_match.group(2),
            "selected_file": workload_match.group(3),
            "found_by": workload_match.group(4),
            "loops": int(workload_match.group(5)),
        }

    region_pattern = re.compile(
        r"^\[REGION\] phase=(early|settle|post) name=([A-Za-z0-9_]+) "
        r"PA=0x[0-9A-F]+ size=0x[0-9A-F]+ status=([A-Z]+) crc32=0x([0-9A-F]+)$",
        re.MULTILINE,
    )
    for region_match in region_pattern.finditer(text):
        phase, name, status, crc = region_match.groups()
        if name in run["regions"]:
            run["regions"][name][phase] = {
                "status": status,
                "crc32": f"0x{crc}",
            }

    focus_pattern = re.compile(
        r"^\[FOCUS3\] label=([A-Za-z0-9_]+) PA=0x[0-9A-F]+ alias=.*? "
        r"early=0x([0-9A-F]+) settle=0x([0-9A-F]+) post=0x([0-9A-F]+)$",
        re.MULTILINE,
    )
    for focus_match in focus_pattern.finditer(text):
        label, early, settle, post = focus_match.groups()
        run["focus"][label] = {
            "early": f"0x{early}",
            "settle": f"0x{settle}",
            "post": f"0x{post}",
        }

    unreadable_focus_pattern = re.compile(
        r"^\[FOCUS3\] label=([A-Za-z0-9_]+) PA=0x[0-9A-F]+ alias=.*? "
        r"status=UNREADABLE early_ok=(\d+) settle_ok=(\d+) post_ok=(\d+)$",
        re.MULTILINE,
    )
    for focus_match in unreadable_focus_pattern.finditer(text):
        label, early_ok, settle_ok, post_ok = focus_match.groups()
        run["focus"][label] = {
            "status": "UNREADABLE",
            "early_ok": int(early_ok),
            "settle_ok": int(settle_ok),
            "post_ok": int(post_ok),
        }

    utf16_pattern = re.compile(
        r'^\[UTF16_HINT\] phase=(early|settle|post) PA=0x[0-9A-F]+ size=0x[0-9A-F]+ text="(.*)"(?: trunc=1)?$',
        re.MULTILINE,
    )
    for utf_match in utf16_pattern.finditer(text):
        phase, hint = utf_match.groups()
        run["utf16_hint"][phase] = hint

    diff_pattern = re.compile(
        r"^\[DIFF_TOTAL\] pair=(early_to_settle|settle_to_post|early_to_post) changed_words_total=(\d+)$",
        re.MULTILINE,
    )
    for diff_match in diff_pattern.finditer(text):
        pair, total = diff_match.groups()
        run["diff_totals"][pair] = int(total)

    return run


def field_signature_from_region(run: Dict[str, object], name: str) -> Tuple[Optional[str], Optional[str], Optional[str]]:
    phases = run["regions"].get(name, {})
    return tuple(phases.get(phase, {}).get("crc32") for phase in PHASES)  # type: ignore[return-value]


def field_signature_from_focus(run: Dict[str, object], label: str) -> Tuple[Optional[str], Optional[str], Optional[str]]:
    values = run["focus"].get(label, {})
    return tuple(values.get(phase) for phase in PHASES)  # type: ignore[return-value]


def signature_key(sig: Tuple[Optional[str], Optional[str], Optional[str]]) -> str:
    return "/".join(value if value is not None else "MISSING" for value in sig)


def correlate_signatures(runs: List[Dict[str, object]], field_name: str, kind: str) -> str:
    tag_sets: Dict[str, set[str]] = defaultdict(set)
    for run in runs:
        short = run.get("boot_tag_short")
        if short not in {"cold", "warm"}:
            continue
        if kind == "region_crc":
            sig = field_signature_from_region(run, field_name)
        else:
            sig = field_signature_from_focus(run, field_name)
        tag_sets[str(short)].add(signature_key(sig))

    if not tag_sets:
        return "insufficient"
    if "cold" not in tag_sets or "warm" not in tag_sets:
        return "single_tag_only"
    if len(tag_sets["cold"] | tag_sets["warm"]) <= 1:
        return "single_family"
    if tag_sets["cold"].isdisjoint(tag_sets["warm"]):
        return "tag_disjoint"
    return "no_clear_correlation"


def summarize_field(
    runs: List[Dict[str, object]], kind: str, name: str
) -> FieldSummary:
    signatures: Dict[str, List[str]] = {}
    phase_varying = False
    for run in runs:
        if kind == "region_crc":
            sig = field_signature_from_region(run, name)
        else:
            sig = field_signature_from_focus(run, name)
        key = signature_key(sig)
        signatures.setdefault(key, []).append(str(run["filename"]))
        present_values = [value for value in sig if value is not None]
        if len(set(present_values)) > 1:
            phase_varying = True

    stable = len(signatures) == 1
    if stable:
        only_sig = next(iter(signatures))
        stable = len(set(only_sig.split("/"))) == 1 and "MISSING" not in only_sig

    correlation = correlate_signatures(runs, name, kind)
    if len(signatures) == len(runs) and len(runs) > 1:
        correlation = "per_run_unique"

    return FieldSummary(
        kind=kind,
        name=name,
        stable_across_runs_and_phases=stable,
        phase_varying_within_run=phase_varying,
        family_count=len(signatures),
        correlation=correlation,
        signatures=signatures,
    )


def summarize_runs(runs: List[Dict[str, object]]) -> Dict[str, object]:
    region_fields = [summarize_field(runs, "region_crc", name) for name in REGION_NAMES]
    focus_fields = [summarize_field(runs, "focus_word", label) for label in FOCUS_LABELS]
    field_summaries = region_fields + focus_fields

    stable_fields = [
        {"kind": field.kind, "name": field.name}
        for field in field_summaries
        if field.stable_across_runs_and_phases
    ]
    phase_varying_fields = [
        {"kind": field.kind, "name": field.name, "correlation": field.correlation}
        for field in field_summaries
        if field.phase_varying_within_run
    ]
    multi_family_fields = [
        {
            "kind": field.kind,
            "name": field.name,
            "family_count": field.family_count,
            "correlation": field.correlation,
            "signatures": field.signatures,
        }
        for field in field_summaries
        if field.family_count > 1
    ]

    return {
        "run_count": len(runs),
        "stable_fields": stable_fields,
        "phase_varying_fields": phase_varying_fields,
        "multi_family_fields": multi_family_fields,
    }


def markdown_table(rows: List[List[str]]) -> str:
    if not rows:
        return ""
    widths = [max(len(row[i]) for row in rows) for i in range(len(rows[0]))]
    lines = []
    for idx, row in enumerate(rows):
        lines.append("| " + " | ".join(row[i].ljust(widths[i]) for i in range(len(row))) + " |")
        if idx == 0:
            lines.append("| " + " | ".join("-" * widths[i] for i in range(len(row))) + " |")
    return "\n".join(lines)


def make_markdown_report(runs: List[Dict[str, object]], summary: Dict[str, object]) -> str:
    rows = [["File", "Tag", "Status", "NAND", "Storage", "0F0200", "UTF16"]]
    for run in runs:
        nand = run["nand"]["selected_file"] if run["nand"]["status"] == "OK" else run["nand"]["status"]
        storage = run["storage"]["selected_file"] if run["storage"]["status"] == "OK" else run["storage"]["status"]
        rows.append(
            [
                str(run["filename"]),
                str(run.get("boot_tag_short") or "unknown"),
                str(run.get("probe_complete_status") or "MISSING"),
                Path(nand).name if nand else "ABSENT",
                Path(storage).name if storage else "ABSENT",
                "YES" if run["old_fault_0f0200"] else "NO",
                str(run["utf16_hint"].get("early", "<missing>")),
            ]
        )

    stable_lines = [
        f"- `{item['kind']}:{item['name']}`"
        for item in summary["stable_fields"]
    ]
    phase_lines = [
        f"- `{item['kind']}:{item['name']}` correlation={item['correlation']}"
        for item in summary["phase_varying_fields"]
    ]
    family_lines = []
    for item in summary["multi_family_fields"]:
        family_lines.append(
            f"- `{item['kind']}:{item['name']}` families={item['family_count']} correlation={item['correlation']}"
        )
        for sig, files in item["signatures"].items():
            family_lines.append(f"  - `{sig}` -> {', '.join(files)}")

    report = [
        "# BE300Probe v4 Cluster Report",
        "",
        f"- runs_parsed: `{summary['run_count']}`",
        f"- all_runs_probe_complete_ok: `{all(run.get('probe_complete_status') == 'OK' for run in runs)}`",
        f"- any_old_0f0200_fault: `{any(bool(run['old_fault_0f0200']) for run in runs)}`",
        "",
        "## Run Table",
        "",
        markdown_table(rows),
        "",
        "## Stable Across All Current Runs And Phases",
        "",
        *(stable_lines or ["- none"]),
        "",
        "## Phase-Varying Within At Least One Run",
        "",
        *(phase_lines or ["- none"]),
        "",
        "## Multiple Families Across Runs",
        "",
        *(family_lines or ["- none"]),
        "",
    ]
    return "\n".join(report)


def main() -> int:
    parser = make_parser()
    args = parser.parse_args()

    files = sorted(Path(match) for match in glob.glob(args.glob))
    if not files:
        raise SystemExit(f"No files matched glob: {args.glob}")

    runs = [parse_report(path) for path in files]
    summary = summarize_runs(runs)

    json_payload = {
        "input_glob": args.glob,
        "runs": runs,
        "summary": summary,
    }
    json_out = Path(args.json_out)
    md_out = Path(args.md_out)
    json_out.write_text(json.dumps(json_payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    md_out.write_text(make_markdown_report(runs, summary) + "\n", encoding="utf-8")
    print(f"Wrote {json_out}")
    print(f"Wrote {md_out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
