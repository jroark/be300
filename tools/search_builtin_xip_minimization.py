#!/usr/bin/env python3
"""Build reduced XIP-driver NAND images and search for a blocker reintroduction."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

from build_cardex_diag_image import patch_module_named_exports
from nk_lzss import decode_nk_partition, encode_lzss, patch_logical_stream_from_flat


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_NK = REPO_ROOT / "docs" / "nk_decompressed.bin"
DEFAULT_NAND = REPO_ROOT / "ce" / "restore_images" / "All_nand_300.bin"
DEFAULT_BE300 = REPO_ROOT / "build-host" / "be300"
DEFAULT_OUT = REPO_ROOT / "build-host" / "xip_minimization"
DEFAULT_BASELINE_BMP = REPO_ROOT / "Starting.bmp"
DEFAULT_TIMEOUT = 60
NK_BASE = 0x80060000
HELPER_ACTION_OFFSETS = {
    "ret1": 0x00,
    "ret0": 0x08,
    "nop": 0x10,
}

CANDIDATE_PRIORITY = [
    "touch.dll",
    "keybddr.dll",
    "PowerOn.dll",
    "eeprom.dll",
    "digcam.dll",
    "serial.dll",
    "socket.dll",
    "usb.dll",
    "wavedev.dll",
    "buzzer.dll",
    "bltinbuz.dll",
    "modmonitordll.dll",
    "card_ex.dll",
    "cardstate.dll",
    "compdisk.dll",
    "nanddisk.dll",
    "ddi.dll",
]

SOFT_KEEP = {
    "card_ex.dll",
    "cardstate.dll",
    "compdisk.dll",
}

HARD_KEEP = {
    "nanddisk.dll",
}

STREAM_MODULES = {
    "poweron.dll",
    "digcam.dll",
    "serial.dll",
    "usb.dll",
    "wavedev.dll",
    "bltinbuz.dll",
    "modmonitordll.dll",
    "card_ex.dll",
    "compdisk.dll",
    "nanddisk.dll",
}

STREAM_SUFFIX_ACTIONS = {
    "_close": "ret1",
    "_deinit": "ret1",
    "_iocontrol": "ret0",
    "_init": "ret0",
    "_open": "ret0",
    "_powerdown": "nop",
    "_powerup": "nop",
    "_read": "ret0",
    "_seek": "ret0",
    "_write": "ret0",
}

SUMMARY_EXEC_RE = re.compile(
    r"^\[BE300_LIFECYCLE_SUMMARY\] exec label=(?P<label>\S+) hits=(?P<hits>\d+) "
    r"pc=0x(?P<pc>[0-9a-fA-F]+)$"
)
SUMMARY_MEM_RE = re.compile(
    r"^\[BE300_LIFECYCLE_SUMMARY\] mem label=(?P<label>\S+) reads=(?P<reads>\d+) "
    r"writes=(?P<writes>\d+) range=0x(?P<start>[0-9a-fA-F]+)\.\.0x(?P<end>[0-9a-fA-F]+)$"
)
SCREENSHOT_RE = re.compile(r"^\[UI\] Screenshot saved: (?P<path>.+)$")
CREATEPROCESS_RE = re.compile(
    r'^\[BE300_LIFECYCLE_CREATEPROCESS\] hit=(?P<hit>\d+) a0=0x[0-9a-fA-F]+ image="(?P<image>.*)"$'
)


@dataclass(frozen=True)
class VariantResult:
    name: str
    modules_removed: tuple[str, ...]
    classification: str
    stdout_log: Path
    stderr_log: Path
    nand_image: Path
    flat_nk: Path
    manifest: Path
    screenshot_copy: Path | None
    screenshot_sha1: str | None
    boot_ready_hits: int
    createprocess_hits: int
    ddi_blit_hits: int
    gdi_surface_writes: int
    ddi_user_writes: int
    coshell_spawned: bool
    exit_code: int


def sha1_file(path: Path) -> str:
    return hashlib.sha1(path.read_bytes()).hexdigest()


def sanitize_variant_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", name)


def build_stream_stub(out_dir: Path) -> Path:
    obj_path = out_dir / "stream_stub.o"
    bin_path = out_dir / "stream_stub.bin"
    container_obj = obj_path.relative_to(REPO_ROOT).as_posix()
    container_bin = bin_path.relative_to(REPO_ROOT).as_posix()
    command = (
        "cd /work && "
        f"mipsel-linux-gnu-as -32 -EL -o {container_obj} tools/compact_helper_stub.S && "
        f"mipsel-linux-gnu-objcopy -j .text -O binary {container_obj} {container_bin}"
    )

    out_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["docker", "compose", "run", "--rm", "mips-dev", "/bin/bash", "-lc", command],
        cwd=REPO_ROOT,
        check=True,
    )
    if not bin_path.is_file():
        raise SystemExit(f"error: expected stub binary missing: {bin_path}")
    return bin_path


def module_patch_profile(module_name: str) -> dict[str, object]:
    lower = module_name.lower()

    if lower in STREAM_MODULES:
        return {
            "action_offsets": HELPER_ACTION_OFFSETS,
            "entry_action": "ret1",
            "rewrite_all_action": "ret0",
            "suffix_actions": STREAM_SUFFIX_ACTIONS,
            "required_suffix": set(STREAM_SUFFIX_ACTIONS),
        }

    if lower == "touch.dll":
        exact = {
            "TouchPanelCalibrateAPoint": "ret0",
            "TouchPanelDisable": "ret0",
            "TouchPanelEnable": "ret0",
            "TouchPanelGetDeviceCaps": "ret0",
            "TouchPanelPowerHandler": "nop",
            "TouchPanelReadCalibrationAbort": "ret0",
            "TouchPanelReadCalibrationPoint": "ret0",
            "TouchPanelSetCalibration": "ret0",
            "TouchPanelSetMode": "ret0",
        }
        return {
            "action_offsets": HELPER_ACTION_OFFSETS,
            "entry_action": "ret1",
            "rewrite_all_action": "ret0",
            "exact_actions": exact,
            "required_exact": set(exact),
        }

    if lower == "keybddr.dll":
        exact = {
            "KeyAPI_WinCapsDelete": "ret0",
            "KeyAPI_WinCapsEntry": "ret0",
            "KeybdDriverGetInfo": "ret0",
            "KeybdDriverInitStates": "ret0",
            "KeybdDriverInitialize": "ret0",
            "KeybdDriverPowerHandler": "nop",
            "KeybdDriverSetMode": "ret0",
            "KeybdDriverVKeyToUnicode": "ret0",
        }
        return {
            "action_offsets": HELPER_ACTION_OFFSETS,
            "entry_action": "ret1",
            "rewrite_all_action": "ret0",
            "exact_actions": exact,
            "required_exact": set(exact),
        }

    if lower == "socket.dll":
        exact = {
            "DllMain": "ret1",
            "ModemSockCloseDevice": "ret0",
            "ModemSockDevContDerestriction": "ret0",
            "ModemSockDevContRestriction": "ret0",
            "ModemSockEventClose": "ret0",
            "ModemSockEventOpen": "ret0",
            "ModemSockForceSerialClose": "ret0",
            "ModemSockForceSerialOpen": "ret0",
            "ModemSockGetIntID": "ret0",
            "ModemSockGetSocketStatus": "ret0",
            "ModemSockOpenDevice": "ret0",
            "ModemSockReset": "ret0",
        }
        return {
            "action_offsets": HELPER_ACTION_OFFSETS,
            "entry_action": "ret1",
            "rewrite_all_action": "ret0",
            "exact_actions": exact,
            "required_exact": set(exact),
        }

    if lower == "eeprom.dll":
        exact = {
            "E2pRead": "ret0",
            "E2pWrite": "ret0",
        }
        return {
            "action_offsets": HELPER_ACTION_OFFSETS,
            "entry_action": "ret1",
            "rewrite_all_action": "ret0",
            "exact_actions": exact,
            "required_exact": set(exact),
        }

    if lower == "buzzer.dll":
        exact = {
            "BuzzerByKey": "ret0",
            "BuzzerByTp": "ret0",
            "PlayBuzz": "ret0",
            "StopBuzz": "ret0",
        }
        return {
            "action_offsets": HELPER_ACTION_OFFSETS,
            "entry_action": "ret1",
            "rewrite_all_action": "ret0",
            "exact_actions": exact,
            "required_exact": set(exact),
        }

    if lower == "cardstate.dll":
        exact = {
            "CSAPIDeregisterJacketDetect": "ret0",
            "CSAPIGetState": "ret0",
            "CSAPIGetTuple": "ret0",
            "CSAPIRegisterJacketDetect": "ret0",
        }
        return {
            "action_offsets": HELPER_ACTION_OFFSETS,
            "entry_action": "ret1",
            "rewrite_all_action": "ret0",
            "exact_actions": exact,
            "required_exact": set(exact),
        }

    if lower == "ddi.dll":
        exact = {
            "DrvEnableDriver": "ret0",
            "HALInit": "nop",
        }
        return {
            "action_offsets": HELPER_ACTION_OFFSETS,
            "entry_action": "ret1",
            "rewrite_all_action": "ret0",
            "exact_actions": exact,
            "required_exact": set(exact),
        }

    raise SystemExit(f"error: no patch profile defined for module {module_name}")


def patch_module_for_minimization(
    nk: bytearray,
    patch: bytes,
    module_name: str,
):
    profile = module_patch_profile(module_name)
    return patch_module_named_exports(
        nk,
        NK_BASE,
        module_name,
        patch,
        action_offsets=profile["action_offsets"],
        entry_action=profile["entry_action"],
        exact_actions=profile.get("exact_actions"),
        suffix_actions=profile.get("suffix_actions"),
        rewrite_all_action=profile.get("rewrite_all_action"),
        required_exact=profile.get("required_exact"),
        required_suffix=profile.get("required_suffix"),
    )


def probe_patchable_modules(
    nk_bytes: bytes,
    patch: bytes,
    candidates: list[str],
) -> tuple[list[str], dict[str, str], dict[str, int]]:
    patchable: list[str] = []
    skipped: dict[str, str] = {}
    safe_limits: dict[str, int] = {}

    for module_name in candidates:
        trial_nk = bytearray(nk_bytes)
        try:
            result = patch_module_for_minimization(trial_nk, patch, module_name)
        except SystemExit as exc:
            skipped[module_name] = str(exc)
            continue

        patchable.append(module_name)
        safe_limits[module_name] = int(result["safe_limit"])

    return patchable, skipped, safe_limits


def build_flat_variant(
    nk_bytes: bytes,
    patch: bytes,
    modules_removed: list[str],
) -> tuple[bytes, list[dict[str, object]]]:
    nk = bytearray(nk_bytes)
    details: list[dict[str, object]] = []

    for module_name in modules_removed:
        result = patch_module_for_minimization(nk, patch, module_name)
        details.append(
            {
                "module_name": result["module"]["name"],
                "module_idx": result["module"]["idx"],
                "module_vbase": result["module"]["vbase"],
                "module_vsize": result["module"]["vsize"],
                "safe_patch_limit": result["safe_limit"],
                "patched_entry_rva": result["export_meta"]["patched_entry_rva"],
                "patched_exports": result["export_meta"]["patched_exports"],
            }
        )

    return bytes(nk), details


def repack_nand_variant(
    source_nand: bytes,
    source_image,
    replacement_flat: bytes,
    out_path: Path,
) -> dict[str, int]:
    replacement_logical = patch_logical_stream_from_flat(source_image, replacement_flat)
    replacement_raw = encode_lzss(replacement_logical)

    if len(replacement_raw) > source_image.partition.size_bytes:
        raise SystemExit(
            f"error: repacked NK raw image does not fit partition: "
            f"0x{len(replacement_raw):X} > 0x{source_image.partition.size_bytes:X}"
        )

    out = bytearray(source_nand)
    part_start = source_image.partition.offset
    out[part_start:part_start + len(replacement_raw)] = replacement_raw
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(out)

    return {
        "replacement_raw_size": len(replacement_raw),
        "logical_stream_size": len(replacement_logical),
    }


def parse_run(stderr_path: Path, baseline_sha1: str, runtime_cwd: Path) -> dict[str, object]:
    exec_hits: dict[str, int] = {}
    mem_writes: dict[str, int] = {}
    screenshot_rel: str | None = None
    coshell_spawned = False

    for raw_line in stderr_path.read_text(errors="replace").splitlines():
        line = raw_line.strip()
        m = SUMMARY_EXEC_RE.match(line)
        if m:
            exec_hits[m.group("label")] = int(m.group("hits"))
            continue

        m = SUMMARY_MEM_RE.match(line)
        if m:
            mem_writes[m.group("label")] = int(m.group("writes"))
            continue

        m = SCREENSHOT_RE.match(line)
        if m:
            screenshot_rel = m.group("path")
            continue

        m = CREATEPROCESS_RE.match(line)
        if m and m.group("image").lower() == "coshell.exe":
            coshell_spawned = True

    screenshot_path = None
    screenshot_sha1 = None
    screenshot_matches_starting = False
    if screenshot_rel:
        candidate = Path(screenshot_rel)
        if not candidate.is_absolute():
            candidate = runtime_cwd / candidate
        if candidate.is_file():
            screenshot_path = candidate
            screenshot_sha1 = sha1_file(candidate)
            screenshot_matches_starting = screenshot_sha1 == baseline_sha1

    boot_ready_hits = exec_hits.get("launcher_module_ready_notify", 0)
    createprocess_hits = exec_hits.get("spawn_module_createprocess_path", 0)
    ddi_blit_hits = exec_hits.get("ddi_blit_dispatcher_entry", 0)
    gdi_surface_writes = mem_writes.get("gdi_surface_0x140000", 0)
    ddi_user_writes = mem_writes.get("ddi_mapped_user_va", 0)

    boot_ready_reached = boot_ready_hits >= 7 or coshell_spawned

    if not boot_ready_reached:
        classification = "EARLY_FAIL"
    elif (
        coshell_spawned
        and ddi_blit_hits > 0
        and gdi_surface_writes > 0
        and ddi_user_writes == 0
        and screenshot_matches_starting
    ):
        classification = "SAME_BLOCKER"
    elif coshell_spawned and (ddi_user_writes > 0 or (screenshot_sha1 and not screenshot_matches_starting)):
        classification = "PAST_STARTING"
    else:
        classification = "NEW_BLOCKER"

    return {
        "classification": classification,
        "exec_hits": exec_hits,
        "mem_writes": mem_writes,
        "screenshot_path": screenshot_path,
        "screenshot_sha1": screenshot_sha1,
        "boot_ready_hits": boot_ready_hits,
        "createprocess_hits": createprocess_hits,
        "ddi_blit_hits": ddi_blit_hits,
        "gdi_surface_writes": gdi_surface_writes,
        "ddi_user_writes": ddi_user_writes,
        "coshell_spawned": coshell_spawned,
    }


def run_variant(
    *,
    name: str,
    modules_removed: list[str],
    flat_nk: bytes,
    patch_details: list[dict[str, object]],
    source_nand: bytes,
    source_image,
    be300_bin: Path,
    baseline_sha1: str,
    work_root: Path,
    timeout_s: int,
) -> VariantResult:
    variant_dir = work_root / sanitize_variant_name(name)
    variant_dir.mkdir(parents=True, exist_ok=True)
    flat_nk_path = variant_dir / "nk_flat.bin"
    nand_path = variant_dir / "All_nand_variant.bin"
    stdout_log = variant_dir / "stdout.log"
    stderr_log = variant_dir / "stderr.log"
    manifest_path = variant_dir / "manifest.json"

    flat_nk_path.write_bytes(flat_nk)
    repack_meta = repack_nand_variant(source_nand, source_image, flat_nk, nand_path)

    env = os.environ.copy()
    env["BE300_LIFECYCLE_PROBE"] = "1"
    env["BE300_AUTOSTOP_SEC"] = str(timeout_s)

    with stdout_log.open("wb") as stdout_file, stderr_log.open("wb") as stderr_file:
        try:
            proc = subprocess.run(
                [str(be300_bin), "--nand", str(nand_path)],
                cwd=be300_bin.parent,
                env=env,
                stdout=stdout_file,
                stderr=stderr_file,
                check=False,
                timeout=max(timeout_s * 3, timeout_s + 60),
            )
        except subprocess.TimeoutExpired:
            proc = subprocess.CompletedProcess(
                args=[str(be300_bin), "--nand", str(nand_path)],
                returncode=124,
            )

    runtime_cwd = be300_bin.parent
    parsed = parse_run(stderr_log, baseline_sha1, runtime_cwd)
    screenshot_copy = None
    if parsed["screenshot_path"] is not None:
        screenshot_copy = variant_dir / "final_screenshot.bmp"
        shutil.copy2(parsed["screenshot_path"], screenshot_copy)

    manifest = {
        "name": name,
        "modules_removed": modules_removed,
        "patch_details": patch_details,
        "classification": parsed["classification"],
        "exit_code": proc.returncode,
        "stdout_log": str(stdout_log),
        "stderr_log": str(stderr_log),
        "nand_image": str(nand_path),
        "flat_nk": str(flat_nk_path),
        "screenshot_copy": str(screenshot_copy) if screenshot_copy else None,
        "screenshot_sha1": parsed["screenshot_sha1"],
        "boot_ready_hits": parsed["boot_ready_hits"],
        "createprocess_hits": parsed["createprocess_hits"],
        "ddi_blit_hits": parsed["ddi_blit_hits"],
        "gdi_surface_writes": parsed["gdi_surface_writes"],
        "ddi_user_writes": parsed["ddi_user_writes"],
        "coshell_spawned": parsed["coshell_spawned"],
        "repack": repack_meta,
        "exec_hits": parsed["exec_hits"],
        "mem_writes": parsed["mem_writes"],
    }
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="ascii")

    return VariantResult(
        name=name,
        modules_removed=tuple(modules_removed),
        classification=str(parsed["classification"]),
        stdout_log=stdout_log,
        stderr_log=stderr_log,
        nand_image=nand_path,
        flat_nk=flat_nk_path,
        manifest=manifest_path,
        screenshot_copy=screenshot_copy,
        screenshot_sha1=parsed["screenshot_sha1"],
        boot_ready_hits=int(parsed["boot_ready_hits"]),
        createprocess_hits=int(parsed["createprocess_hits"]),
        ddi_blit_hits=int(parsed["ddi_blit_hits"]),
        gdi_surface_writes=int(parsed["gdi_surface_writes"]),
        ddi_user_writes=int(parsed["ddi_user_writes"]),
        coshell_spawned=bool(parsed["coshell_spawned"]),
        exit_code=proc.returncode,
    )


def write_summary(
    out_dir: Path,
    *,
    patchable: list[str],
    skipped: dict[str, str],
    safe_limits: dict[str, int],
    runs: list[VariantResult],
    minimal_success: VariantResult | None,
    blocker_result: VariantResult | None,
) -> None:
    summary = {
        "patchable_modules": patchable,
        "skipped_modules": skipped,
        "safe_limits": safe_limits,
        "runs": [
            {
                "name": run.name,
                "modules_removed": list(run.modules_removed),
                "classification": run.classification,
                "nand_image": str(run.nand_image),
                "flat_nk": str(run.flat_nk),
                "stdout_log": str(run.stdout_log),
                "stderr_log": str(run.stderr_log),
                "manifest": str(run.manifest),
                "screenshot_copy": str(run.screenshot_copy) if run.screenshot_copy else None,
                "screenshot_sha1": run.screenshot_sha1,
                "boot_ready_hits": run.boot_ready_hits,
                "createprocess_hits": run.createprocess_hits,
                "ddi_blit_hits": run.ddi_blit_hits,
                "gdi_surface_writes": run.gdi_surface_writes,
                "ddi_user_writes": run.ddi_user_writes,
                "coshell_spawned": run.coshell_spawned,
                "exit_code": run.exit_code,
            }
            for run in runs
        ],
        "minimal_success": str(minimal_success.manifest) if minimal_success else None,
        "blocker_result": str(blocker_result.manifest) if blocker_result else None,
    }
    (out_dir / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="ascii")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--nk-image", default=str(DEFAULT_NK))
    ap.add_argument("--nand-image", default=str(DEFAULT_NAND))
    ap.add_argument("--be300-bin", default=str(DEFAULT_BE300))
    ap.add_argument("--out-dir", default=str(DEFAULT_OUT))
    ap.add_argument("--baseline-bmp", default=str(DEFAULT_BASELINE_BMP))
    ap.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT)
    ap.add_argument(
        "--candidate",
        action="append",
        dest="candidates",
        help="Candidate module to stub; repeatable",
    )
    ap.add_argument(
        "--soft-keep",
        action="append",
        dest="soft_keep",
        help="Modules to keep in the conservative starting set; repeatable",
    )
    ap.add_argument(
        "--hard-keep",
        action="append",
        dest="hard_keep",
        help="Modules to never remove; repeatable",
    )
    ap.add_argument(
        "--singleton-only",
        action="store_true",
        help="Skip the coarse multi-module stages and test stock-minus-one variants only",
    )
    args = ap.parse_args()

    nk_path = Path(args.nk_image).resolve()
    nand_path = Path(args.nand_image).resolve()
    be300_path = Path(args.be300_bin).resolve()
    out_dir = Path(args.out_dir).resolve()
    baseline_bmp = Path(args.baseline_bmp).resolve()

    for path in (nk_path, nand_path, be300_path, baseline_bmp):
        if not path.is_file():
            raise SystemExit(f"error: required input not found: {path}")

    out_dir.mkdir(parents=True, exist_ok=True)
    patch_path = build_stream_stub(out_dir)
    patch = patch_path.read_bytes()
    nk_bytes = nk_path.read_bytes()
    source_nand = nand_path.read_bytes()
    source_image = decode_nk_partition(source_nand, partition_index=2)
    baseline_sha1 = sha1_file(baseline_bmp)

    candidates = args.candidates or list(CANDIDATE_PRIORITY)
    soft_keep = set(args.soft_keep or SOFT_KEEP)
    hard_keep = set(args.hard_keep or HARD_KEEP)

    patchable, skipped, safe_limits = probe_patchable_modules(nk_bytes, patch, candidates)
    runs: list[VariantResult] = []

    search_stages = [
        ("stage1_conservative", [m for m in patchable if m not in soft_keep and m not in hard_keep]),
        ("stage2_aggressive", [m for m in patchable if m not in hard_keep]),
        ("stage3_full", list(patchable)),
    ]

    minimal_success: VariantResult | None = None
    if not args.singleton_only:
        for stage_name, modules_removed in search_stages:
            flat_nk, patch_details = build_flat_variant(nk_bytes, patch, modules_removed)
            result = run_variant(
                name=stage_name,
                modules_removed=modules_removed,
                flat_nk=flat_nk,
                patch_details=patch_details,
                source_nand=source_nand,
                source_image=source_image,
                be300_bin=be300_path,
                baseline_sha1=baseline_sha1,
                work_root=out_dir,
                timeout_s=args.timeout,
            )
            runs.append(result)
            if result.classification == "PAST_STARTING":
                minimal_success = result
                break

    if minimal_success is None:
        for module_name in patchable:
            flat_nk, patch_details = build_flat_variant(nk_bytes, patch, [module_name])
            result = run_variant(
                name=f"singleton_{module_name}",
                modules_removed=[module_name],
                flat_nk=flat_nk,
                patch_details=patch_details,
                source_nand=source_nand,
                source_image=source_image,
                be300_bin=be300_path,
                baseline_sha1=baseline_sha1,
                work_root=out_dir,
                timeout_s=args.timeout,
            )
            runs.append(result)
            if result.classification == "PAST_STARTING":
                minimal_success = result
                break

    blocker_result: VariantResult | None = None
    if minimal_success is not None:
        current_removed = list(minimal_success.modules_removed)
        addback_order = [m for m in candidates if m in current_removed]

        for module_name in addback_order:
            next_removed = [m for m in current_removed if m != module_name]
            flat_nk, patch_details = build_flat_variant(nk_bytes, patch, next_removed)
            result = run_variant(
                name=f"addback_{module_name}",
                modules_removed=next_removed,
                flat_nk=flat_nk,
                patch_details=patch_details,
                source_nand=source_nand,
                source_image=source_image,
                be300_bin=be300_path,
                baseline_sha1=baseline_sha1,
                work_root=out_dir,
                timeout_s=args.timeout,
            )
            runs.append(result)

            if result.classification == "PAST_STARTING":
                current_removed = next_removed
                minimal_success = result
                continue

            blocker_result = result
            break

    write_summary(
        out_dir,
        patchable=patchable,
        skipped=skipped,
        safe_limits=safe_limits,
        runs=runs,
        minimal_success=minimal_success,
        blocker_result=blocker_result,
    )

    print(f"[xip_minimization] patchable={len(patchable)} skipped={len(skipped)}")
    if minimal_success is None:
        print("[xip_minimization] no PAST_STARTING image found")
    else:
        print(
            "[xip_minimization] minimal_success"
            f" name={minimal_success.name}"
            f" removed={len(minimal_success.modules_removed)}"
            f" image={minimal_success.nand_image}"
        )
    if blocker_result is None:
        print("[xip_minimization] no blocker reintroduced during add-back")
    else:
        print(
            "[xip_minimization] blocker_result"
            f" name={blocker_result.name}"
            f" classification={blocker_result.classification}"
            f" image={blocker_result.nand_image}"
        )
    print(f"[xip_minimization] summary={out_dir / 'summary.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
