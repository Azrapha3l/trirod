"""Extract portable Maxwell shader records from EoW's BNSH shader archives.

The catalog contains the original NVN control sections and code areas, not any
GPU/driver-specific Vulkan objects. Runtime-discovered ShaderKey and pipeline
variants remain a separate layer.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import mmap
import os
import struct
import sys
from pathlib import Path


MAGIC = b"BNSH\0\0\0\0"
STAGES = ("vs", "hs", "ds", "gs", "ps", "cs")
CTL_MAGIC = 0x98761234
GFX_MAGIC = 0x12345678
CS_MAGIC = 0x12345679


class CatalogError(Exception):
    pass


# main's build ID (NSO header 0x40, first 8 bytes) -> game version (docs/STATUS.md; runtime/tools/trbuild.c)
GAME_VERSIONS = {"a581406bc9858651": "1.0.0", "9f221e1afa65ba54": "1.1.0"}


def game_version(main: Path | None) -> tuple[str, str]:
    """(version, main build id hex) of the ExeFS main the catalog was made with; "unknown" without one."""
    if main is None:
        return "unknown", ""
    with main.open("rb") as f:
        head = f.read(0x60)
    if len(head) < 0x60 or head[:4] != b"NSO0":
        return "unknown", ""
    build_id = head[0x40:0x60].rstrip(b"\0").hex()
    return GAME_VERSIONS.get(build_id[:16], "unknown"), build_id


def u32(data: bytes, off: int) -> int:
    if off < 0 or off + 4 > len(data):
        raise CatalogError(f"u32 at {off:#x} is outside the archive")
    return struct.unpack_from("<I", data, off)[0]


def u64(data: bytes, off: int) -> int:
    if off < 0 or off + 8 > len(data):
        raise CatalogError(f"u64 at {off:#x} is outside the archive")
    return struct.unpack_from("<Q", data, off)[0]


def span(data: bytes, off: int, size: int, what: str) -> bytes:
    if not size or off < 0 or size < 0 or off + size > len(data):
        raise CatalogError(f"{what} range {off:#x}+{size:#x} is outside the archive")
    return data[off:off + size]


def parse_bnsh(data: bytes, base: int, label: str) -> tuple[list[dict], list[dict]]:
    if base + 0x84 > len(data) or data[base:base + 8] != MAGIC:
        raise CatalogError(f"{label}: invalid BNSH header")
    declared_size = u32(data, base + 0x1c)
    if declared_size < 0x84 or base + declared_size > len(data):
        raise CatalogError(f"{label}: invalid BNSH size {declared_size:#x}")
    end = base + declared_size
    if data[base + 0x60:base + 0x64] != b"grsc":
        raise CatalogError(f"{label}: BNSH has no grsc variation table")
    count, table_rel = struct.unpack_from("<IQ", data, base + 0x7c)
    if count > 65536 or table_rel < 0x84 or base + table_rel + count * 0x40 > end:
        raise CatalogError(f"{label}: invalid variation table ({count}, {table_rel:#x})")

    stages: list[dict] = []
    programs: list[dict] = []
    for vi in range(count):
        voff = base + table_rel + vi * 0x40
        prog_rel = u64(data, voff + 0x10)
        if not prog_rel:
            continue
        prog = base + prog_rel
        if prog < base or prog + 0x38 > end:
            raise CatalogError(f"{label}: variation {vi} program pointer is out of bounds")
        program_stages: dict[str, str] = {}
        for si, stage in enumerate(STAGES):
            stage_rel = u64(data, prog + 8 + si * 8)
            if not stage_rel:
                continue
            desc = base + stage_rel
            if desc < base or desc + 0x20 > end:
                raise CatalogError(f"{label}: variation {vi} {stage} descriptor is out of bounds")
            ctl_rel, code_rel, code_len, ctl_len = struct.unpack_from("<QQII", data, desc + 8)
            ctl = span(data, base + ctl_rel, ctl_len, f"{label} variation {vi} {stage} control")
            code = span(data, base + code_rel, code_len, f"{label} variation {vi} {stage} code")
            if len(ctl) < 0x24 or struct.unpack_from("<I", ctl)[0] != CTL_MAGIC:
                raise CatalogError(f"{label}: variation {vi} {stage} has invalid control data")
            code_magic = struct.unpack_from("<I", code)[0] if len(code) >= 4 else 0
            if len(code) < 0x34 or code_magic not in ((CS_MAGIC, GFX_MAGIC) if stage == "cs" else (GFX_MAGIC,)):
                raise CatalogError(f"{label}: variation {vi} {stage} has invalid Maxwell code")
            if u32(ctl, 0x704) != code_len:
                raise CatalogError(f"{label}: variation {vi} {stage} code/control lengths disagree")
            identity = hashlib.sha256(stage.encode() + b"\0" + ctl + code).hexdigest()
            stages.append({
                "key": identity,
                "stage": stage,
                "control_bytes": len(ctl),
                "code_bytes": len(code),
                "control_sha256": hashlib.sha256(ctl).hexdigest(),
                "code_sha256": hashlib.sha256(code).hexdigest(),
                "control": ctl,
                "code": code,
            })
            program_stages[stage] = identity
        if program_stages:
            programs.append({"source": label, "variation": vi, "stages": program_stages})
    return stages, programs


def scan(romfs: Path, main: Path | None) -> tuple[dict[str, dict], list[dict], dict]:
    if romfs.is_dir():
        files = sorted((p for p in romfs.rglob("*") if p.is_file()), key=lambda p: p.as_posix().lower())
    elif romfs.is_file():
        files = [romfs]
    else:
        raise CatalogError(f"RomFS source does not exist: {romfs}")
    if main is not None and main.is_file():
        files.append(main)
    unique: dict[str, dict] = {}
    programs: list[dict] = []
    found_bnsh = 0
    errors: list[str] = []
    for path in files:
        label = "exefs/main" if main is not None and path == main else str(path.relative_to(romfs)) if romfs.is_dir() else path.name
        try:
            if path.stat().st_size > 128 * 1024 * 1024:
                with path.open("rb") as f:
                    data = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
                    records, progs, nfound, bad = scan_data(data, label)
                for rec in records:
                    unique.setdefault(rec["key"], rec)
                programs.extend(progs)
                found_bnsh += nfound
                errors.extend(bad)
                continue
            data = path.read_bytes()
        except OSError as e:
            errors.append(f"{path}: {e}")
            continue
        records, progs, nfound, bad = scan_data(data, label)
        found_bnsh += nfound
        errors.extend(bad)
        for rec in records:
            unique.setdefault(rec["key"], rec)
        programs.extend(progs)
    return unique, programs, {"files_scanned": len(files), "bnsh_signatures": found_bnsh, "parse_errors": errors}


def scan_data(data, label: str):
    records: list[dict] = []
    programs: list[dict] = []
    errors: list[str] = []
    found = 0
    pos = 0
    while True:
        pos = data.find(MAGIC, pos)
        if pos < 0:
            break
        found += 1
        try:
            recs, progs = parse_bnsh(data, pos, f"{label}@{pos:#x}")
            records.extend(recs)
            programs.extend(progs)
        except CatalogError as e:
            errors.append(str(e))
        pos += len(MAGIC)
    return records, programs, found, errors


def write_catalog(outdir: Path, stages: dict[str, dict], programs: list[dict], stats: dict,
                  version: tuple[str, str] = ("unknown", "")) -> None:
    outdir.mkdir(parents=True, exist_ok=True)
    stage_ids = {key: i for i, key in enumerate(sorted(stages))}
    stage_rec_size, program_rec_size = 56, 28
    payload_offset = 32 + len(stage_ids) * stage_rec_size + len(programs) * program_rec_size
    index = bytearray()
    payload = bytearray()
    manifest_stages = []
    for key, rec in sorted(stages.items()):
        offset = payload_offset + len(payload)
        ctl, code = rec["control"], rec["code"]
        index.extend(struct.pack("<IIII32sQ", STAGES.index(rec["stage"]), len(ctl), len(code), 0,
                                  bytes.fromhex(key), offset))
        payload.extend(ctl)
        payload.extend(code)
        item = {k: v for k, v in rec.items() if k not in ("control", "code")}
        item["offset"] = offset
        item["record_bytes"] = len(ctl) + len(code)
        manifest_stages.append(item)
    prog_index = bytearray()
    for prog in programs:
        indices = [stage_ids.get(prog["stages"].get(name), 0xFFFFFFFF) for name in STAGES]
        prog_index.extend(struct.pack("<7I", prog["variation"], *indices))
    header = struct.pack("<8sIIIIII", b"EOWSCAT1", 1, len(stage_ids), len(programs), stage_rec_size,
                         program_rec_size, payload_offset)
    temp_catalog = outdir / "catalog.bin.tmp"
    with temp_catalog.open("wb") as f:
        f.write(header)
        f.write(index)
        f.write(prog_index)
        f.write(payload)
    os.replace(temp_catalog, outdir / "catalog.bin")
    (outdir / "stages.bin").unlink(missing_ok=True)  # the catalog is the sole binary payload
    manifest = {
        "format": "EOW_SHADER_CATALOG",
        "schema": 1,
        "game": "The Legend of Zelda: Echoes of Wisdom",
        "game_version": version[0],  # (was always "1.0.0"; the update's shaders are catalogued the same way)
        "main_build_id": version[1],
        "stages": manifest_stages,
        "programs": programs,
        "scan": stats,
    }
    tmp = outdir / "manifest.json.tmp"
    tmp.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    os.replace(tmp, outdir / "manifest.json")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("romfs", type=Path)
    ap.add_argument("outdir", type=Path)
    ap.add_argument("--main", type=Path, help="optional ExeFS/main (scans direct BNSH records only)")
    args = ap.parse_args(argv)
    if not args.romfs.exists():
        ap.error(f"RomFS source does not exist: {args.romfs}")
    if args.main is not None and not args.main.is_file():
        ap.error(f"ExeFS/main does not exist: {args.main}")
    stages, programs, stats = scan(args.romfs, args.main)
    write_catalog(args.outdir, stages, programs, stats, game_version(args.main))
    print(f"shader catalog: {len(stages)} unique stages, {len(programs)} program variations, "
          f"{stats['bnsh_signatures']} BNSH signatures in {stats['files_scanned']} files; "
          f"{len(stats['parse_errors'])} malformed/unrecognized BNSH blocks", file=sys.stderr)
    for err in stats["parse_errors"][:20]:
        print(f"[shader-catalog] {err}", file=sys.stderr)
    return 0 if stages else 2


if __name__ == "__main__":
    raise SystemExit(main())
