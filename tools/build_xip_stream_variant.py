#!/usr/bin/env python3
"""Patch multiple XIP stream-driver modules inside a flat NK image."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from build_cardex_diag_image import patch_stream_driver_module, sanitize_name


DEFAULT_NK = Path("docs/nk_decompressed.bin")
DEFAULT_PATCH = Path("build-host/xip_reduction/stream_stub.bin")
DEFAULT_OUT = Path("build-host/xip_reduction")
NK_BASE = 0x80060000


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--nk-image",
        default=str(DEFAULT_NK),
        help=f"Source flat NK image (default: {DEFAULT_NK})",
    )
    ap.add_argument(
        "--patch-bin",
        default=str(DEFAULT_PATCH),
        help=f"Replacement .text blob (default: {DEFAULT_PATCH})",
    )
    ap.add_argument(
        "--module",
        action="append",
        dest="modules",
        required=True,
        help="Target stream-driver module name; repeatable",
    )
    ap.add_argument(
        "--out-nk",
        required=True,
        help="Output patched flat NK image path",
    )
    ap.add_argument(
        "--meta-out",
        help="Optional JSON metadata output path",
    )
    ap.add_argument(
        "--modules-out-dir",
        help="Optional directory for per-module flat images",
    )
    ap.add_argument(
        "--rewrite-all-exports",
        action="store_true",
        help="Rewrite every export RVA to the stub entry unless it matches a known stream role",
    )
    args = ap.parse_args()

    nk_path = Path(args.nk_image)
    patch_path = Path(args.patch_bin)
    out_nk_path = Path(args.out_nk)
    meta_path = Path(args.meta_out) if args.meta_out else out_nk_path.with_suffix(".json")
    modules_out_dir = Path(args.modules_out_dir) if args.modules_out_dir else None

    if not nk_path.is_file():
        raise SystemExit(f"error: flat NK image not found: {nk_path}")
    if not patch_path.is_file():
        raise SystemExit(f"error: patch binary not found: {patch_path}")

    patch = patch_path.read_bytes()
    if not patch:
        raise SystemExit("error: patch binary is empty")

    nk = bytearray(nk_path.read_bytes())
    results = []

    if modules_out_dir is not None:
        modules_out_dir.mkdir(parents=True, exist_ok=True)

    for module_name in args.modules:
        patch_result = patch_stream_driver_module(
            nk,
            NK_BASE,
            module_name,
            patch,
            rewrite_all_exports=args.rewrite_all_exports,
        )
        module = patch_result["module"]
        export_meta = patch_result["export_meta"]
        module_tag = f"{module['idx']:02d}_{sanitize_name(module['name'])}"

        if modules_out_dir is not None:
            (modules_out_dir / f"{module_tag}.bin").write_bytes(module["flat"])

        results.append(
            {
                "module_idx": module["idx"],
                "module_name": module["name"],
                "module_vbase": module["vbase"],
                "module_vsize": module["vsize"],
                "text_rva": patch_result["text_rva"],
                "text_psize": patch_result["text_section"]["psize"],
                "safe_patch_limit": patch_result["safe_limit"],
                "original_entry_rva": patch_result["original_entry_rva"],
                "patched_entry_rva": export_meta["patched_entry_rva"],
                "export_rva": export_meta["export_rva"],
                "export_size": export_meta["export_size"],
                "patched_exports": export_meta["patched_exports"],
            }
        )

    out_nk_path.parent.mkdir(parents=True, exist_ok=True)
    out_nk_path.write_bytes(nk)

    meta = {
        "source_nk": str(nk_path),
        "patch_bin": str(patch_path),
        "out_nk": str(out_nk_path),
        "rewrite_all_exports": args.rewrite_all_exports,
        "modules": results,
    }
    meta_path.parent.mkdir(parents=True, exist_ok=True)
    meta_path.write_text(json.dumps(meta, indent=2, sort_keys=True) + "\n", encoding="ascii")

    print(f"[build_xip_stream_variant] patched {len(results)} modules")
    print(f"[build_xip_stream_variant] wrote {out_nk_path}")
    print(f"[build_xip_stream_variant] wrote {meta_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
