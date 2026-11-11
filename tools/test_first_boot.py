#!/usr/bin/env python3
"""Drive a Windows CE 3.0 first boot to the shell with scripted pen input.

Boots a copy of the NAND image with tests/first_boot.touch (five calibration
holds, then OK on the date/time setup) and checks the serial milestones, that
CE did not report a watchdog reboot, and the final screen against
tests/first_boot_shell.bmp. The status bar (clock, card icon) is left out of
the comparison.

    tools/test_first_boot.py --emulator build-host/be300 \\
        --nand ce/restore_images/All_nand_300.bin [--cf card.img] [--seconds 90]
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from compare_screenshot import read_bmp_pixels

ROOT = Path(__file__).resolve().parent.parent
MILESTONES = (b"Windows CE Kernel for MIPS", b"CASIO Original Shell started.")
FORBIDDEN = (b"System was rebooted by WDT.",)
STATUS_BAR_ROWS = 24        # clock and icons at the bottom of the 240x320 LCD


def body_similarity(a, b, width=240, height=320):
    rows = height - STATUS_BAR_ROWS
    n = width * rows * 3
    same = sum(1 for i in range(0, n, 3) if a[i:i + 3] == b[i:i + 3])
    return same * 100.0 / (width * rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--emulator", required=True, type=Path)
    ap.add_argument("--nand", required=True, type=Path)
    ap.add_argument("--cf", type=Path)
    ap.add_argument("--seconds", type=int, default=90, help="host seconds to run")
    ap.add_argument("--baseline", type=Path, default=ROOT / "tests" / "first_boot_shell.bmp")
    ap.add_argument("--write-baseline", action="store_true")
    ap.add_argument("--keep", type=Path, help="copy the final screenshot here")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        nand = tmp / "nand.bin"
        shutil.copyfile(args.nand, nand)
        cmd = [str(args.emulator.resolve()), "--nand", str(nand), "--speed", "0",
               "--touch-script", str(ROOT / "tests" / "first_boot.touch")]
        if args.cf:
            cf = tmp / "cf.img"
            shutil.copyfile(args.cf, cf)
            cmd += ["--cf", str(cf)]
        env = dict(os.environ, BE300_AUTOSTOP_SEC=str(args.seconds),
                   SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
        run = subprocess.run(cmd, cwd=tmp, env=env, capture_output=True,
                             timeout=args.seconds + 120)
        problems = []
        cursor = 0
        for m in MILESTONES:
            cursor = run.stdout.find(m, cursor)
            if cursor < 0:
                problems.append("missing serial milestone: " + m.decode())
                break
        for f in FORBIDDEN:
            if f in run.stdout:
                problems.append("serial output has: " + f.decode())
        shots = sorted(tmp.glob("screenshot_*.bmp"))
        if not shots:
            problems.append("no screenshot (the emulator needs SDL2)")
        else:
            if args.keep:
                shutil.copyfile(shots[-1], args.keep)
            if args.write_baseline:
                shutil.copyfile(shots[-1], args.baseline)
            w, h, _bpp, pixels = read_bmp_pixels(shots[-1])
            bw, bh, _bbpp, base = read_bmp_pixels(args.baseline)
            if (w, h) != (bw, bh):
                problems.append("screenshot is %dx%d" % (w, h))
            else:
                sim = body_similarity(pixels, base, w, h)
                if sim < 99.9:
                    problems.append("final screen matches the shell baseline only %.2f%%" % sim)

    if problems:
        print("first_boot: FAILED")
        for p in problems:
            print("  - " + p)
        return 1
    print("first_boot: PASS (shell reached, no watchdog-reboot report)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
