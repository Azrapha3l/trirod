"""Tests without game files for tools/nso.py's DT_GNU_HASH symbol count and DT_RELR decoding (the v1.1.0 update's
nnSdk 20 modules have no DT_HASH and keep all relative relocations in DT_RELR; runtime/elf_dyn.h does the same, tested
by tools/test_elf_dyn.c) and tools/shader_catalog.py's game version from main's build ID (synthetic NSO headers).

Usage: tools/venv/Scripts/python.exe -I tools/test_nso.py
"""
import os
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nso import gnu_hash_nsyms, relr_offsets  # noqa: E402
from shader_catalog import game_version  # noqa: E402


def gnu_hash(name):
    h = 5381
    for c in name.encode():
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def make_table(symoffset, n, nbuckets, pad=24):
    """A DT_GNU_HASH table laid out as a linker writes it, after `pad` unrelated bytes."""
    hashes = [gnu_hash(f'sym_{i * 7919}') for i in range(n)]
    order = sorted(range(n), key=lambda i: hashes[i] % nbuckets)
    bloom = 2
    buckets, chain = [0] * nbuckets, []
    for j, i in enumerate(order):
        b = hashes[i] % nbuckets
        if not buckets[b]:
            buckets[b] = symoffset + j
        last = j + 1 == n or hashes[order[j + 1]] % nbuckets != b
        chain.append(hashes[i] | 1 if last else hashes[i] & ~1 & 0xFFFFFFFF)
    t = struct.pack('<4I', nbuckets, symoffset, bloom, 6) + bytes(bloom * 8)
    t += struct.pack(f'<{nbuckets}I', *buckets) + struct.pack(f'<{n}I', *chain)
    return bytes(pad) + t


def relr_encode(offs):
    """lld's DT_RELR encoding of sorted, 8-aligned offsets."""
    out, i = [], 0
    while i < len(offs):
        out.append(offs[i])
        base = offs[i] + 8
        i += 1
        while True:
            bitmap = 0
            while i < len(offs) and 0 <= offs[i] - base < 63 * 8 and (offs[i] - base) % 8 == 0:
                bitmap |= 1 << ((offs[i] - base) // 8 + 1)
                i += 1
            if not bitmap:
                break
            out.append(bitmap | 1)
            base += 63 * 8
    return out


def main():
    fails = 0
    cases = {'one address': [0x100], 'a dense run': list(range(0x200, 0x200 + 8 * 200, 8)),
             'window edge': [0x40, 0x48 + 62 * 8, 0x48 + 63 * 8]}
    o, sparse = 0x1000, []
    while o < 0xF000:
        sparse.append(o)
        o += 8 * (1 + (o >> 3) % 70)
    cases['sparse with gaps'] = sparse
    for what, offs in cases.items():
        enc = relr_encode(offs)
        ok = relr_offsets(enc) == offs
        fails += not ok
        print(f'{"ok  " if ok else "FAIL"} relr {what} ({len(offs)} offsets in {len(enc)} entries)')
    for symoffset, n, nbuckets in ((1, 1, 1), (12, 500, 37), (700, 3, 64), (5, 200, 1), (9, 0, 4), (1, 23030, 4099)):
        got = gnu_hash_nsyms(make_table(symoffset, n, nbuckets), 24)
        ok = got == symoffset + n
        fails += not ok
        print(f'{"ok  " if ok else "FAIL"} symoffset {symoffset}, {n} defined, {nbuckets} buckets -> {got}')
    with tempfile.TemporaryDirectory() as d:
        for build_id, want in (('a581406bc9858651', '1.0.0'), ('9f221e1afa65ba54', '1.1.0'),
                               ('0123456789abcdef', 'unknown')):
            p = Path(d) / 'main'
            p.write_bytes(b'NSO0' + bytes(0x3C) + bytes.fromhex(build_id) + bytes(0x18 + 0x40))
            got = game_version(p)
            ok = got == (want, build_id)
            fails += not ok
            print(f'{"ok  " if ok else "FAIL"} catalog game version of main {build_id}: {got[0]}')
        ok = game_version(None) == ('unknown', '')
        fails += not ok
        print(f'{"ok  " if ok else "FAIL"} catalog game version without a main: unknown')
    print(f'{fails} FAILED' if fails else 'all passed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
