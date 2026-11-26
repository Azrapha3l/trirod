"""Per-instruction replacements applied by gen.py (guest address -> function(gen, addr) returning C code).

Every patch checks the original instruction word at its address before replacing it (expect()), so a wrong
address - or a different game build - fails generation loudly instead of miscompiling silently. Addresses are
absolute guest addresses (module base + offset) and valid only for the pinned build of the game (docs/STATUS.md).

Example:
    SITE = 0x7000123456

    def _site(gen, a):
        expect(gen, a, 0xbd400000)  # ldr s0, [x0]
        return '{ ... C replacing the instruction ... }'

    PATCHES[SITE] = _site
"""
import struct

PATCHES = {}  # every build
BUILD_PATCHES = {}  # main's build id (first 16 hex digits) -> {address: patch}: sites that differ between versions


def patches_for(nso):
    """The patches for one module: PATCHES plus its build's own (BUILD_PATCHES)."""
    p = dict(PATCHES)
    p.update(BUILD_PATCHES.get(nso.build_id[:16], {}))
    return p


def word_at(gen, a):
    return struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]


def expect(gen, a, want, mask=0xFFFFFFFF):
    """Assert the original instruction at guest address a (under mask) before patching it."""
    w = word_at(gen, a)
    assert (w & mask) == want, f'patch site {a:#x}: found {w:08x}, expected {want:08x} (mask {mask:08x})'
    return w


# ---- optional cheats (runtime/cheats.c, settings cheat_*; the menu's Cheats tab)
#
# Echoes of Wisdom keeps the player's wallet, life and Swordfighter Form energy in one block of the player actor
# (v1.0.0 at +0x1b00..; v1.1.0 moved the whole block 0x340 lower):
#   wallet  int32  +0x1b98 / +0x1858  clamped 0..9999 by every writer (CheckRupee / AddRupee in the event queries read it)
#   max     int32  +0x1b08 / +0x17c8  quarter hearts, at most 0x50 (20 hearts)
#   life    int32  +0x1b0c / +0x17cc  quarter hearts, clamped 0..max (CheckLife compares it, CheckDamage with max)
#   energy  float  +0x1bb0 / +0x1870  clamped 0..a table entry picked by the storage-level bits 0x2000/0x4000/0x8000
#                                     of +0x1bb4 / +0x1874 (GetMorphEnergyStrageLv reads those bits)
# Each writer computes the new value from the old one and stores it once; the patches below replace that store. With
# the cheat's bit set a smaller value keeps the old one (and the register gets the stored value, so code after the
# store sees what memory holds); with it clear the store is the original. Stores that set a value outright (save load,
# new heart container, reset to 0) are not patched: a cheat never mixes one save's numbers into another. Sites with
# bit 0 only report (EOWRECOMP_CHEAT_TRACE): event-script writers left alone on purpose (scripted damage).
#
# Life is the exception: ordinary damage doesn't reach any of these stores (found by a scripted run, see
# _life_restore), so CHEAT_HEALTH instead refills life to max at the start of the player's per-frame status update.
#
# Might Crystals are never stored as a count that goes down: what Lueburry's upgrades check (HaveEnoughForLuberiWork)
# and the menus show is computed by one function (v1.0.0 f_70021d5070, v1.1.0 f_700222b1b0) as the crystals collected
# minus the costs of the upgrades done (bits of +0x1bb4: 10/20 each for sword, bow, bombs, 5/10/20 for the energy
# levels). The patch replaces that subtraction: with CHEAT_MIGHT 99 are available. Not exposed: nothing sets CHEAT_MIGHT
# (no menu toggle; not verified in game - the map's Might Crystal count is the crystals found, not this number).


def _store_keep(bit, off, float_=False):
    """Replace `str wT/sT, [xN, #off]`: with g_cheats & bit, never store a smaller value than memory holds."""
    def patch(gen, a):
        w = word_at(gen, a)
        op = 0xBD000000 if float_ else 0xB9000000
        expect(gen, a, op | ((off // 4) << 10), 0xFFFFFC00)
        rt, rn = w & 31, (w >> 5) & 31
        assert rt != 31 and rn != 31, f'patch site {a:#x}: wzr / sp form'
        reg = f'c->v[{rt}]' if float_ else f'c->x[{rt}]'
        if float_:
            cmp = 'float fo, fv; memcpy(&fo, &o, 4); memcpy(&fv, &v, 4); '
            less, show = 'fv < fo', '(double)fo, (double)fv, (double)(s == o ? fo : fv)'
        else:
            cmp, less, show = '', '(int32_t)v < (int32_t)o', '(int32_t)o, (int32_t)v, (int32_t)s'
        keep = f'(g_cheats & {bit}u) && {less}' if bit else '0'
        return (f'{{ uint64_t ea = c->x[{rn}] + {off:#x}ull; uint32_t o = LD32(ea), v = (uint32_t){reg}; '
                f'if (__builtin_expect(g_cheats != 0, 0)) {{ {cmp}uint32_t s = {keep} ? o : v; '
                f'if (g_cheats & CHEAT_TRACE) rt_cheat_trace({a:#x}ull, c->x[{rn}], {show}); '
                f'if (s != v) {{ v = s; {reg} = s; }} }} ST32(ea, v); }}')
    return patch


def _life_restore(max_off, life_off):
    """The player's per-frame status update (x0: the status block's owner), at `mov x19, x0`: with CHEAT_HEALTH, life
    back to max. Ordinary damage doesn't go through any store above (a scripted run lost a heart to a sand piranha with
    none of them firing), so Infinite Health restores before the next hit instead of blocking the damage."""
    def patch(gen, a):
        expect(gen, a, 0xAA0003F3)  # mov x19, x0
        return (f'c->x[19] = c->x[0]; '
                f'if (__builtin_expect(g_cheats & CHEAT_HEALTH, 0)) {{ uint64_t ea = c->x[0] + {life_off:#x}ull; '
                f'uint32_t mx = LD32(c->x[0] + {max_off:#x}ull), o = LD32(ea); '
                f'if ((int32_t)o < (int32_t)mx) {{ if (g_cheats & CHEAT_TRACE) '
                f'rt_cheat_trace({a:#x}ull, c->x[0], (int32_t)o, (int32_t)mx, (int32_t)mx); ST32(ea, mx); }} }}')
    return patch


def _might_available(gen, a):
    expect(gen, a, 0x4B090108)  # sub w8, w8, w9: crystals collected - costs of the upgrades done
    return ('{ uint32_t real = (uint32_t)c->x[8] - (uint32_t)c->x[9]; c->x[8] = (g_cheats & CHEAT_MIGHT) ? 99u : real; '
            f'if (g_cheats & CHEAT_TRACE) rt_cheat_trace({a:#x}ull, 0, (int32_t)real, (int32_t)c->x[8], (int32_t)c->x[8]); }}')


def _cheat_sites(wallet, life, energy, rupee_sites, life_sites, energy_sites, might_site, restore_site):
    p = {a: _store_keep(1, wallet) for a in rupee_sites}  # CHEAT_RUPEES
    p[restore_site] = _life_restore(life - 4, life)  # (max is the int before life)
    p.update({a: _store_keep(bit, life) for a, bit in life_sites})  # CHEAT_HEALTH (2) or trace only (0)
    p.update({a: _store_keep(bit, energy, True) for a, bit in energy_sites})  # CHEAT_ENERGY (4) or trace only
    p[might_site] = _might_available
    return p


# v1.0.0 (main a581406bc9858651). Wallet writers: the event action AddRupee (f_70021665d0) and the game's other
# add-a-delta paths (shops, pickups); the save-load init f_70020d5aa0 is left out.
BUILD_PATCHES['a581406bc9858651'] = _cheat_sites(
    0x1b98, 0x1b0c, 0x1bb0,
    [0x7000e8ff3c, 0x7000e90818, 0x700159f6f4, 0x70015a9f80, 0x70015bb744, 0x70017331ec, 0x700214a8c8,
     0x7002166668, 0x7002503f58],
    [(0x70017334b8, 2),   # f_70017333f0: heart recovery (plays SE_SYS_HEART_RECOVERY)
     (0x70020b5a88, 2),   # f_70020b5070
     (0x70020f0484, 2),   # f_70020ed610: a fairy from a bottle (+12, the bottle's count goes down)
     (0x70020f71dc, 2),   # f_70020f5e80: the player's status update
     (0x70021676d4, 0),   # f_7002167630: event action with an "amount" (scripted): trace only
     (0x70021d4ccc, 2),   # f_70021d4c70: item / smoothie effects by kind (kind 1 = life)
     (0x70024c2b78, 2)],  # f_70024c2a60
    [(0x7001733788, 4), (0x7001734130, 4),   # energy recovery (accessory buffs double / halve it)
     (0x70020f6a78, 4), (0x70020f6b64, 4),   # f_70020f5e80: the player's status update (drain: energy - rate * dt)
     (0x7002166138, 0),                      # f_7002166060: event action (scripted): trace only
     (0x70021d4e38, 4), (0x700254e9f4, 4)],
    0x70021d51dc,
    0x70020f5eb8)  # f_70020f5e80: the player's status update

# v1.1.0 (main 9f221e1afa65ba54): the same writers, in the same functions and order (block 0x340 lower)
BUILD_PATCHES['9f221e1afa65ba54'] = _cheat_sites(
    0x1858, 0x17cc, 0x1870,
    [0x7000ea3008, 0x7000ea38e4, 0x70015cf42c, 0x70015da62c, 0x70015ef240, 0x70017745f8, 0x70021a0614,
     0x70021bc434, 0x7002563224],
    [(0x70017748c4, 2), (0x700210af74, 2), (0x70021456e4, 2), (0x700214bbc8, 2), (0x70021bd4b4, 0),
     (0x700222ae00, 2), (0x7002520e7c, 2)],
    [(0x7001774b98, 4), (0x7001775550, 4), (0x700214c2d8, 4), (0x700214c3c4, 4), (0x70021bbf0c, 0),
     (0x700222af6c, 4), (0x70025ae0d4, 4)],
    0x700222b31c,
    0x700214b2d8)
