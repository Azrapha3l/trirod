#!/usr/bin/env python3
"""Merge one or more Trirod gameplay warm caches into shader_catalog/warm.

Only the guest Maxwell code referenced by valid pipeline recipes is copied. The
export uses schema 2 with wildcard GPU IDs: each install rebuilds compatible
recipes using its own Vulkan driver, and keeps the driver's pipeline cache local.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import struct
import sys
import tempfile
from pathlib import Path


MAGIC = b"EOWWARM\0"
SCHEMA_LOCAL = 1
SCHEMA_PORTABLE = 2
RECORD_BYTES = 808
PAYLOAD_BYTES = 800
FNV_OFFSET = 0xCBF29CE484222325
FNV_CHECK_OFFSET = 0x84222325CBF29CE4
FNV_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1
CODE_NAME = re.compile(r"^([0-9a-f]{16})-([0-9a-f]+)\.bin$")


class ExportError(Exception):
    pass


def fnv64(seed: int, data: bytes) -> int:
    h = seed
    for value in data:
        h = ((h ^ value) * FNV_PRIME) & MASK64
    return h


def code_id(code: bytes) -> int:
    return fnv64(fnv64(FNV_OFFSET, code), struct.pack("<I", len(code))) or 1


def read_catalog(root: Path, portable_only: bool = False) -> tuple[dict[bytes, bytes], set[bytes], list[str]]:
    recipe_path = root / "recipes.bin"
    try:
        raw = recipe_path.read_bytes()
    except OSError as exc:
        raise ExportError(f"cannot read {recipe_path}: {exc}") from exc
    if len(raw) < 32 or raw[:8] != MAGIC:
        raise ExportError(f"{recipe_path}: invalid EOWWARM header")
    schema, rec_bytes, vendor, device, count = struct.unpack_from("<IIIII", raw, 8)
    if rec_bytes != RECORD_BYTES or schema not in (SCHEMA_LOCAL, SCHEMA_PORTABLE):
        raise ExportError(f"{recipe_path}: unsupported schema/record size ({schema}, {rec_bytes})")
    # Runtime caches use schema 2 as well, but retain their source GPU IDs so the
    # game only reuses those recipes on that device. write_catalog() clears both
    # IDs when exporting the records into the shared portable catalog.
    if portable_only and (schema != SCHEMA_PORTABLE or vendor or device):
        raise ExportError(f"{recipe_path}: expected a portable schema-2 catalog")
    available = (len(raw) - 32) // RECORD_BYTES
    if count > available or 32 + count * RECORD_BYTES != len(raw):
        raise ExportError(f"{recipe_path}: record count does not match file length")

    code_dir = root / "code"
    codes: dict[bytes, bytes] = {}
    recipes: set[bytes] = set()
    messages: list[str] = []
    for i in range(count):
        record = raw[32 + i * RECORD_BYTES:32 + (i + 1) * RECORD_BYTES]
        payload = record[:PAYLOAD_BYTES]
        (stored_sum,) = struct.unpack_from("<Q", record, PAYLOAD_BYTES)
        if fnv64(FNV_CHECK_OFFSET, payload) != stored_sum:
            raise ExportError(f"{recipe_path}: recipe {i} has a bad checksum")
        ids = struct.unpack_from("<QQII", payload, 0)
        gids, extents = ids[:2], ids[2:]
        if not all(gids) or not all(extents) or any(n > 0x10000 for n in extents):
            raise ExportError(f"{recipe_path}: recipe {i} has invalid shader identities")
        for gid, extent in zip(gids, extents):
            ident = (gid, extent)
            if ident in codes:
                continue
            code_path = code_dir / f"{gid:016x}-{extent:x}.bin"
            try:
                code = code_path.read_bytes()
            except OSError as exc:
                raise ExportError(f"recipe {i} references missing code {code_path}: {exc}") from exc
            if len(code) != extent or code_id(code) != gid:
                raise ExportError(f"recipe {i} references corrupt code {code_path}")
            codes[ident] = code
        recipes.add(record)
    messages.append(f"{count} recipes from {recipe_path} (source GPU {vendor:#06x}:{device:#06x})")
    return codes, recipes, messages


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.replace(temp_name, path)
    finally:
        try:
            os.unlink(temp_name)
        except FileNotFoundError:
            pass


def write_catalog(out: Path, codes: dict[tuple[int, int], bytes], recipes: set[bytes]) -> None:
    code_dir = out / "code"
    code_dir.mkdir(parents=True, exist_ok=True)
    for (gid, extent), data in sorted(codes.items()):
        atomic_write(code_dir / f"{gid:016x}-{extent:x}.bin", data)
    records = sorted(recipes)
    header = bytearray(32)
    header[:8] = MAGIC
    struct.pack_into("<IIIII", header, 8, SCHEMA_PORTABLE, RECORD_BYTES, 0, 0, len(records))
    atomic_write(out / "recipes.bin", bytes(header) + b"".join(records))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("cache", nargs="+", type=Path, help="one or more cache/warm folders from gameplay")
    ap.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "shader_catalog" / "warm",
                    help="repository catalog output (default: shader_catalog/warm)")
    args = ap.parse_args(argv)
    try:
        codes: dict[tuple[int, int], bytes] = {}
        recipes: set[bytes] = set()
        messages: list[str] = []
        if (args.output / "recipes.bin").is_file():
            old_codes, old_recipes, info = read_catalog(args.output, portable_only=True)
            codes.update(old_codes)
            recipes.update(old_recipes)
            messages.extend(f"existing: {line}" for line in info)
        for cache in args.cache:
            root = cache / "warm" if (cache / "warm" / "recipes.bin").is_file() else cache
            new_codes, new_recipes, info = read_catalog(root)
            for ident, data in new_codes.items():
                previous = codes.get(ident)
                if previous is not None and previous != data:
                    raise ExportError(f"code identity collision for {ident[0]:016x}-{ident[1]:x}")
                codes[ident] = data
            recipes.update(new_recipes)
            messages.extend(info)
        if not recipes:
            raise ExportError("no valid pipeline recipes found")
        write_catalog(args.output, codes, recipes)
    except ExportError as exc:
        print(f"[shader-warm-export] error: {exc}", file=sys.stderr)
        return 1
    for message in messages:
        print(f"[shader-warm-export] {message}", file=sys.stderr)
    print(f"[shader-warm-export] wrote {len(recipes)} unique recipes and {len(codes)} guest shader records to {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
