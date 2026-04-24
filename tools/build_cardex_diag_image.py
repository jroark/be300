#!/usr/bin/env python3
"""Build a diagnostic flat NK image by patching one XIP stream-driver module.

The diagnostic payload is a fixed raw .text blob. To retarget it to different
stream drivers, this script rewrites the module's DllMain entry RVA and export
function table so the existing loader metadata points at the payload's stub
RVAs.
"""

from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path

from extract_xip_modules import (
    PTOC_VA,
    TABLE_VA,
    TOC_ENTRY_SIZE,
    read_cstr,
    read_u16,
    read_u32,
    reassemble_module,
)


DEFAULT_OUT_DIR = Path("build-host/cardex_diag")
DEFAULT_NK = Path("docs/nk_decompressed.bin")
DEFAULT_PATCH = DEFAULT_OUT_DIR / "cardex_diag_patch.bin"
DEFAULT_MODULE = "compdisk.dll"

PATCH_ENTRY_OFFSET = 0x05B8
PATCH_EXPORT_OFFSETS = {
    "close": 0x045C,
    "deinit": 0x10D0,
    "iocontrol": 0x0F44,
    "init": 0x1104,
    "open": 0x10C8,
    "powerdown": 0x0C38,
    "powerup": 0x0C4C,
    "read": 0x0C54,
    "seek": 0x0C54,
    "write": 0x0C54,
}
REQUIRED_EXPORTS = set(PATCH_EXPORT_OFFSETS)


def read_cstr_from_flat(data: bytes, rva: int) -> str:
    end = data.index(b"\x00", rva)
    return data[rva:end].decode("ascii", "replace")


def sanitize_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", name)


def get_text_section(module: dict) -> dict:
    for section in module["sections"]:
        if section["flags"] & 0x20:
            return section
    raise SystemExit(f"error: module {module['name']} has no code section")


def get_module_entry(nk: bytes, nk_base: int, idx: int) -> dict:
    entry_va = TABLE_VA + idx * TOC_ENTRY_SIZE
    name_va = read_u32(nk, entry_va + 0x14, nk_base)
    e32_va = read_u32(nk, entry_va + 0x18, nk_base)
    o32_va = read_u32(nk, entry_va + 0x1C, nk_base)
    name = read_cstr(nk, name_va, nk_base)
    objcnt = read_u16(nk, e32_va + 0x00, nk_base)
    entry_rva = read_u32(nk, e32_va + 0x04, nk_base)
    flat, vbase, vsize, sections = reassemble_module(nk, nk_base, e32_va, o32_va, objcnt)
    return {
        "idx": idx,
        "name": name,
        "e32_va": e32_va,
        "o32_va": o32_va,
        "objcnt": objcnt,
        "entry_rva": entry_rva,
        "flat": bytearray(flat),
        "vbase": vbase,
        "vsize": vsize,
        "sections": sections,
    }


def find_module_by_name(nk: bytes, nk_base: int, name: str) -> dict:
    nummods = read_u32(nk, PTOC_VA + 0x10, nk_base)
    want = name.lower()

    for idx in range(nummods):
        module = get_module_entry(nk, nk_base, idx)
        if module["name"].lower() == want:
            return module

    raise SystemExit(f"error: module not found in NK image: {name}")


def inject_module_into_nk(nk: bytearray, nk_base: int, module: dict) -> None:
    for section in module["sections"]:
        psize = section["psize"]
        if psize == 0:
            continue

        dptr = section["dataptr"]
        start = dptr - nk_base
        end = start + psize
        rva = section["rva"]

        if start < 0 or end > len(nk):
            raise ValueError(
                f"section write out of bounds: rva=0x{rva:X} start=0x{start:X} end=0x{end:X}"
            )

        nk[start:end] = module["flat"][rva:rva + psize]


def export_patch_role(name: str) -> str | None:
    lower = name.lower()
    for role in PATCH_EXPORT_OFFSETS:
        if lower.endswith("_" + role):
            return role
    return None


def patch_stream_driver_metadata(
    nk: bytearray,
    nk_base: int,
    module: dict,
    *,
    rewrite_all_exports: bool = False,
    patch_entry_offset: int = PATCH_ENTRY_OFFSET,
    patch_export_offsets: dict[str, int] = PATCH_EXPORT_OFFSETS,
) -> dict:
    exp_rva = read_u32(nk, module["e32_va"] + 0x20, nk_base)
    exp_size = read_u32(nk, module["e32_va"] + 0x24, nk_base)
    if exp_rva == 0 or exp_size == 0:
        raise SystemExit(f"error: module {module['name']} has no export table")

    flat = module["flat"]
    text_rva = get_text_section(module)["rva"]
    patch_entry_rva = text_rva + patch_entry_offset
    patch_export_rvas = {
        role: text_rva + offset
        for role, offset in patch_export_offsets.items()
    }

    ordinal_base = struct.unpack_from("<I", flat, exp_rva + 16)[0]
    nfunc = struct.unpack_from("<I", flat, exp_rva + 20)[0]
    nname = struct.unpack_from("<I", flat, exp_rva + 24)[0]
    addr_funcs = struct.unpack_from("<I", flat, exp_rva + 28)[0]
    addr_names = struct.unpack_from("<I", flat, exp_rva + 32)[0]
    addr_ords = struct.unpack_from("<I", flat, exp_rva + 36)[0]

    seen_roles: set[str] = set()
    patched_exports: list[str] = []

    if rewrite_all_exports:
        for ordidx in range(nfunc):
            struct.pack_into("<I", flat, addr_funcs + ordidx * 4, patch_entry_rva)

    for i in range(nname):
        name_rva = struct.unpack_from("<I", flat, addr_names + i * 4)[0]
        ordidx = struct.unpack_from("<H", flat, addr_ords + i * 2)[0]
        export_name = read_cstr_from_flat(flat, name_rva)
        role = export_patch_role(export_name)
        if role is None:
            if rewrite_all_exports:
                patched_exports.append(f"{export_name}=0x{patch_entry_rva:X}")
            continue

        struct.pack_into("<I", flat, addr_funcs + ordidx * 4, patch_export_rvas[role])
        seen_roles.add(role)
        patched_exports.append(f"{export_name}=0x{patch_export_rvas[role]:X}")

    missing = sorted(set(patch_export_offsets) - seen_roles)
    if missing:
        raise SystemExit(
            f"error: module {module['name']} is missing required stream exports: {', '.join(missing)}"
        )

    struct.pack_into("<I", nk, module["e32_va"] + 0x04 - nk_base, patch_entry_rva)
    module["entry_rva"] = patch_entry_rva

    return {
        "ordinal_base": ordinal_base,
        "nfunc": nfunc,
        "patched_exports": patched_exports,
        "export_rva": exp_rva,
        "export_size": exp_size,
        "patched_entry_rva": patch_entry_rva,
        "patch_text_rva": text_rva,
    }


def compute_safe_patch_limit(nk: bytes, nk_base: int, module: dict) -> int:
    text = get_text_section(module)
    text_rva = text["rva"]
    limit = text["psize"]

    # Keep the payload from trampling export/import metadata if either table
    # happens to live inside .text. Other directories such as the tiny MIPS
    # pdata/debug records at .text+0 are not needed for this diagnostic blob.
    for i in (0, 1):
        dir_rva = read_u32(nk, module["e32_va"] + 0x20 + i * 8, nk_base)
        dir_size = read_u32(nk, module["e32_va"] + 0x24 + i * 8, nk_base)
        if dir_size == 0:
            continue
        if text_rva <= dir_rva < text_rva + limit:
            limit = min(limit, dir_rva - text_rva)

    return limit


def patch_stream_driver_module(
    nk: bytearray,
    nk_base: int,
    module_name: str,
    patch: bytes,
    *,
    rewrite_all_exports: bool = False,
    patch_entry_offset: int = PATCH_ENTRY_OFFSET,
    patch_export_offsets: dict[str, int] = PATCH_EXPORT_OFFSETS,
) -> dict:
    module = find_module_by_name(nk, nk_base, module_name)
    safe_limit = compute_safe_patch_limit(nk, nk_base, module)
    if len(patch) > safe_limit:
        raise SystemExit(
            f"error: patch binary too large for {module['name']}: "
            f"0x{len(patch):X} > safe limit 0x{safe_limit:X}"
        )

    original_entry_rva = module["entry_rva"]
    text_section = get_text_section(module)
    text_rva = text_section["rva"]
    text_end = text_rva + len(patch)
    module["flat"][text_rva:text_end] = patch

    export_meta = patch_stream_driver_metadata(
        nk,
        nk_base,
        module,
        rewrite_all_exports=rewrite_all_exports,
        patch_entry_offset=patch_entry_offset,
        patch_export_offsets=patch_export_offsets,
    )
    inject_module_into_nk(nk, nk_base, module)

    return {
        "module": module,
        "original_entry_rva": original_entry_rva,
        "safe_limit": safe_limit,
        "text_section": text_section,
        "text_rva": text_rva,
        "export_meta": export_meta,
    }


def patch_named_exports_metadata(
    nk: bytearray,
    nk_base: int,
    module: dict,
    *,
    action_offsets: dict[str, int],
    entry_action: str,
    exact_actions: dict[str, str] | None = None,
    suffix_actions: dict[str, str] | None = None,
    rewrite_all_action: str | None = None,
    required_exact: set[str] | None = None,
    required_suffix: set[str] | None = None,
) -> dict:
    exp_rva = read_u32(nk, module["e32_va"] + 0x20, nk_base)
    exp_size = read_u32(nk, module["e32_va"] + 0x24, nk_base)
    if exp_rva == 0 or exp_size == 0:
        raise SystemExit(f"error: module {module['name']} has no export table")

    flat = module["flat"]
    text_rva = get_text_section(module)["rva"]
    patch_action_rvas = {
        action: text_rva + offset
        for action, offset in action_offsets.items()
    }
    patch_entry_rva = patch_action_rvas[entry_action]

    nfunc = struct.unpack_from("<I", flat, exp_rva + 20)[0]
    nname = struct.unpack_from("<I", flat, exp_rva + 24)[0]
    addr_funcs = struct.unpack_from("<I", flat, exp_rva + 28)[0]
    addr_names = struct.unpack_from("<I", flat, exp_rva + 32)[0]
    addr_ords = struct.unpack_from("<I", flat, exp_rva + 36)[0]

    exact_actions_lower = {
        name.lower(): action
        for name, action in (exact_actions or {}).items()
    }
    suffix_actions_lower = [
        (suffix.lower(), action)
        for suffix, action in (suffix_actions or {}).items()
    ]
    required_exact_lower = {name.lower() for name in (required_exact or set())}
    required_suffix_lower = {suffix.lower() for suffix in (required_suffix or set())}

    if rewrite_all_action is not None:
        default_rva = patch_action_rvas[rewrite_all_action]
        for ordidx in range(nfunc):
            struct.pack_into("<I", flat, addr_funcs + ordidx * 4, default_rva)

    seen_exact: set[str] = set()
    seen_suffix: set[str] = set()
    patched_exports: list[str] = []

    for i in range(nname):
        name_rva = struct.unpack_from("<I", flat, addr_names + i * 4)[0]
        ordidx = struct.unpack_from("<H", flat, addr_ords + i * 2)[0]
        export_name = read_cstr_from_flat(flat, name_rva)
        export_name_lower = export_name.lower()
        action = None

        if export_name_lower in exact_actions_lower:
            action = exact_actions_lower[export_name_lower]
            seen_exact.add(export_name_lower)
        else:
            for suffix, suffix_action in suffix_actions_lower:
                if export_name_lower.endswith(suffix):
                    action = suffix_action
                    seen_suffix.add(suffix)
                    break

        if action is None:
            continue

        struct.pack_into("<I", flat, addr_funcs + ordidx * 4, patch_action_rvas[action])
        patched_exports.append(f"{export_name}=0x{patch_action_rvas[action]:X}")

    missing_exact = sorted(required_exact_lower - seen_exact)
    if missing_exact:
        raise SystemExit(
            f"error: module {module['name']} is missing required named exports: "
            f"{', '.join(missing_exact)}"
        )

    missing_suffix = sorted(required_suffix_lower - seen_suffix)
    if missing_suffix:
        raise SystemExit(
            f"error: module {module['name']} is missing required export suffixes: "
            f"{', '.join(missing_suffix)}"
        )

    struct.pack_into("<I", nk, module["e32_va"] + 0x04 - nk_base, patch_entry_rva)
    module["entry_rva"] = patch_entry_rva

    return {
        "patched_exports": patched_exports,
        "export_rva": exp_rva,
        "export_size": exp_size,
        "patched_entry_rva": patch_entry_rva,
        "patch_text_rva": text_rva,
    }


def patch_module_named_exports(
    nk: bytearray,
    nk_base: int,
    module_name: str,
    patch: bytes,
    *,
    action_offsets: dict[str, int],
    entry_action: str,
    exact_actions: dict[str, str] | None = None,
    suffix_actions: dict[str, str] | None = None,
    rewrite_all_action: str | None = None,
    required_exact: set[str] | None = None,
    required_suffix: set[str] | None = None,
) -> dict:
    module = find_module_by_name(nk, nk_base, module_name)
    safe_limit = compute_safe_patch_limit(nk, nk_base, module)
    if len(patch) > safe_limit:
        raise SystemExit(
            f"error: patch binary too large for {module['name']}: "
            f"0x{len(patch):X} > safe limit 0x{safe_limit:X}"
        )

    original_entry_rva = module["entry_rva"]
    text_section = get_text_section(module)
    text_rva = text_section["rva"]
    text_end = text_rva + len(patch)
    module["flat"][text_rva:text_end] = patch

    export_meta = patch_named_exports_metadata(
        nk,
        nk_base,
        module,
        action_offsets=action_offsets,
        entry_action=entry_action,
        exact_actions=exact_actions,
        suffix_actions=suffix_actions,
        rewrite_all_action=rewrite_all_action,
        required_exact=required_exact,
        required_suffix=required_suffix,
    )
    inject_module_into_nk(nk, nk_base, module)

    return {
        "module": module,
        "original_entry_rva": original_entry_rva,
        "safe_limit": safe_limit,
        "text_section": text_section,
        "text_rva": text_rva,
        "export_meta": export_meta,
    }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Patch one XIP stream driver inside a flat decompressed NK image."
    )
    parser.add_argument(
        "--nk-image",
        default=str(DEFAULT_NK),
        help=f"Source flat NK image (default: {DEFAULT_NK})",
    )
    parser.add_argument(
        "--patch-bin",
        default=str(DEFAULT_PATCH),
        help=f"Assembled replacement .text blob (default: {DEFAULT_PATCH})",
    )
    parser.add_argument(
        "--module",
        default=DEFAULT_MODULE,
        help=f"Target XIP stream-driver module (default: {DEFAULT_MODULE})",
    )
    parser.add_argument(
        "--out-dir",
        default=str(DEFAULT_OUT_DIR),
        help=f"Output directory (default: {DEFAULT_OUT_DIR})",
    )
    args = parser.parse_args()

    nk_path = Path(args.nk_image)
    patch_path = Path(args.patch_bin)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    if not nk_path.is_file():
        raise SystemExit(f"error: flat NK image not found: {nk_path}")
    if not patch_path.is_file():
        raise SystemExit(f"error: patch binary not found: {patch_path}")

    patch = patch_path.read_bytes()
    if not patch:
        raise SystemExit("error: patch binary is empty")

    nk = bytearray(nk_path.read_bytes())
    nk_base = 0x80060000

    patch_result = patch_stream_driver_module(nk, nk_base, args.module, patch)
    module = patch_result["module"]
    original_entry_rva = patch_result["original_entry_rva"]
    safe_limit = patch_result["safe_limit"]
    text_section = patch_result["text_section"]
    text_rva = patch_result["text_rva"]
    export_meta = patch_result["export_meta"]

    module_tag = f"{module['idx']:02d}_{sanitize_name(module['name'])}"
    patched_module_path = out_dir / f"{module_tag}_diag.bin"
    patched_nk_path = out_dir / f"nk_{sanitize_name(module['name'])}_diag.bin"
    meta_path = out_dir / f"{module_tag}_diag_meta.txt"

    patched_module_path.write_bytes(module["flat"])
    patched_nk_path.write_bytes(nk)

    meta_lines = [
        f"source_nk={nk_path}",
        f"patch_bin={patch_path}",
        f"module_idx={module['idx']}",
        f"module_name={module['name']}",
        f"module_vbase=0x{module['vbase']:08X}",
        f"module_vsize=0x{module['vsize']:X}",
        f"original_entry_rva=0x{original_entry_rva:X}",
        f"patched_entry_rva=0x{export_meta['patched_entry_rva']:X}",
        f"text_rva=0x{text_rva:X}",
        f"text_psize=0x{text_section['psize']:X}",
        f"safe_patch_limit=0x{safe_limit:X}",
        f"patched_text_size=0x{len(patch):X}",
        f"export_rva=0x{export_meta['export_rva']:X}",
        f"export_size=0x{export_meta['export_size']:X}",
        f"patched_module={patched_module_path}",
        f"patched_nk={patched_nk_path}",
    ]
    meta_lines.extend(export_meta["patched_exports"])
    meta_path.write_text("\n".join(meta_lines) + "\n", encoding="ascii")

    print(f"[streamdiag] patched module {module['name']} (idx {module['idx']})")
    print(f"[streamdiag] wrote {patched_module_path}")
    print(f"[streamdiag] wrote {patched_nk_path}")
    print(f"[streamdiag] wrote {meta_path}")


if __name__ == "__main__":
    main()
