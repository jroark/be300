#!/usr/bin/env python3
"""Run the WinCE 3.0 cold-boot regression without modifying its source NAND."""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from compare_screenshot import nonzero_pct, pixel_similarity, read_bmp_pixels


VALID_IMAGE_SIZES = {16_449_536, 16_777_216}
SPL_SIGNATURE_OFFSET = 0x4000
SPL_SIGNATURE = b"B000FF\n"
MILESTONES = (
    b"KLOADER",
    b"Kloader exit with 80060004",
    b"Windows CE Kernel for MIPS",
    b"CASIO Original Shell started.",
)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_image(path):
    size = path.stat().st_size
    if size not in VALID_IMAGE_SIZES:
        expected = ", ".join(f"{item:,}" for item in sorted(VALID_IMAGE_SIZES))
        raise ValueError(f"unsupported NAND size {size:,}; expected {expected} bytes")
    with open(path, "rb") as stream:
        stream.seek(SPL_SIGNATURE_OFFSET)
        signature = stream.read(len(SPL_SIGNATURE))
    if signature != SPL_SIGNATURE:
        raise ValueError("missing B000FF\\n SPL signature at offset 0x4000")


def ordered_milestones(output):
    cursor = 0
    positions = []
    for milestone in MILESTONES:
        position = output.find(milestone, cursor)
        if position < 0:
            return False, milestone.decode("ascii", "replace")
        positions.append(position)
        cursor = position + len(milestone)
    return True, positions


def validate_screenshot(path, baselines_dir):
    width, height, _bpp, pixels = read_bmp_pixels(path)
    if (width, height) != (240, 320):
        raise ValueError(f"unexpected screenshot dimensions {width}x{height}")
    if nonzero_pct(pixels) < 1.0:
        raise ValueError("final screenshot is blank")

    for name in ("Initializing.bmp", "Starting.bmp"):
        baseline_path = baselines_dir / name
        bw, bh, _bbpp, baseline = read_bmp_pixels(baseline_path)
        if (width, height) != (bw, bh):
            continue
        similarity = pixel_similarity(pixels, baseline) * 100.0
        if similarity >= 99.5:
            raise ValueError(f"final screenshot is still at {name} ({similarity:.2f}%)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=Path)
    parser.add_argument("--nand", required=True, type=Path)
    parser.add_argument("--baselines-dir", required=True, type=Path)
    parser.add_argument("--artifacts-dir", required=True, type=Path)
    parser.add_argument("--autostop-seconds", type=int, default=60)
    parser.add_argument("--timeout-seconds", type=int, default=120)
    parser.add_argument("--require-screenshot", action="store_true")
    args = parser.parse_args()

    emulator = args.emulator.resolve()
    source_nand = args.nand.resolve()
    baselines_dir = args.baselines_dir.resolve()
    artifacts_dir = args.artifacts_dir.resolve()
    artifacts_dir.mkdir(parents=True, exist_ok=True)
    stdout_path = artifacts_dir / "cold_stdout.log"
    stderr_path = artifacts_dir / "cold_stderr.log"
    final_screenshot = artifacts_dir / "final.bmp"
    if final_screenshot.exists():
        final_screenshot.unlink()

    try:
        if not emulator.is_file():
            raise ValueError(f"emulator not found: {emulator}")
        validate_image(source_nand)
        source_hash = sha256(source_nand)
    except (OSError, ValueError) as exc:
        print(f"cold_boot: {exc}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="be300-cold-boot-") as temp_name:
        workdir = Path(temp_name)
        test_nand = workdir / "All_nand_300.bin"
        shutil.copyfile(source_nand, test_nand)
        env = os.environ.copy()
        env["BE300_AUTOSTOP_SEC"] = str(args.autostop_seconds)
        env["BE300_AUDIO"] = "0"
        command = [str(emulator), "--nand", str(test_nand)]

        try:
            completed = subprocess.run(
                command,
                cwd=workdir,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=args.timeout_seconds,
                check=False,
            )
        except subprocess.TimeoutExpired as exc:
            stdout_path.write_bytes(exc.stdout or b"")
            stderr_path.write_bytes(exc.stderr or b"")
            print(f"cold_boot: host timeout after {args.timeout_seconds}s", file=sys.stderr)
            return 1

        stdout_path.write_bytes(completed.stdout)
        stderr_path.write_bytes(completed.stderr)
        screenshots = sorted(workdir.glob("screenshot_*.bmp"))
        if screenshots:
            shutil.copyfile(screenshots[-1], final_screenshot)

        errors = []
        if completed.returncode != 0:
            errors.append(f"emulator exited with status {completed.returncode}")
        if b"[BE300] Autostop reached" not in completed.stderr:
            errors.append("orderly autostop marker is missing")
        milestones_ok, detail = ordered_milestones(completed.stdout)
        if not milestones_ok:
            errors.append(f"missing or out-of-order serial milestone: {detail}")
        if sha256(source_nand) != source_hash:
            errors.append("source NAND image changed during the test")

        if final_screenshot.exists():
            try:
                validate_screenshot(final_screenshot, baselines_dir)
            except (OSError, ValueError) as exc:
                errors.append(str(exc))
        elif args.require_screenshot:
            errors.append("orderly shutdown did not save a screenshot")

        if errors:
            print("cold_boot: FAILED", file=sys.stderr)
            for error in errors:
                print(f"  - {error}", file=sys.stderr)
            print(f"  artifacts: {artifacts_dir}", file=sys.stderr)
            return 1

    print("cold_boot: PASS")
    print("  milestones: " + " -> ".join(item.decode("ascii") for item in MILESTONES))
    print(f"  source NAND sha256: {source_hash}")
    print(f"  artifacts: {artifacts_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
