"""Network card option validation must fail before touching NAND or opening SDL."""

import subprocess
import sys


def reject(args, message):
    result = subprocess.run(
        [sys.argv[1], *args], capture_output=True, text=True, timeout=5
    )
    assert result.returncode == 1, (args, result.returncode, result.stderr)
    assert message in result.stderr, (args, result.stderr)


reject(["--nand", "missing.bin", "--xi825", "--ne2000"], "pick one")
reject(["--restore", "--cf", "missing.img", "--xi825"], "primary PCMCIA socket")
reject(["missing.rom", "--xi825"], "only with --nand")
reject(
    ["--nand", "missing.bin", "--xi825", "--cf", "a.img", "--cf", "b.img"],
    "only one --cf",
)
reject(["--nand", "missing.bin", "--net-mac", "02:00:00:00:00:01"], "requires")
reject(["--nand", "missing.bin", "--xi825", "--net-mac", "01:00:00:00:00:01"], "MAC")
# Keep the existing adapter's socket validation covered as well.
reject(["--restore", "--cf", "missing.img", "--ne2000"], "primary PCMCIA socket")
for other in ("--ne2000", "--xi825"):
    reject(["--nand", "missing.bin", "--wl54cf", other], "pick one")
reject(["--restore", "--cf", "missing.img", "--wl54cf"], "primary PCMCIA socket")
reject(["missing.rom", "--wl54cf"], "only with --nand")
reject(["--nand", "missing.bin", "--wl54cf", "--cf", "a.img", "--cf", "b.img"], "only one --cf")
reject(["--nand", "missing.bin", "--wl54cf", "--net-mac", "01:00:00:00:00:01"], "MAC")
print("Network CLI validation passed")
