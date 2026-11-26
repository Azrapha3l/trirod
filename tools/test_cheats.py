"""The cheat patches (tools/patches.py, runtime/cheats.c) without the game: each kind of patch is generated for a
synthetic instruction word, compiled against runtime/cpu.h with a stand-in for the runtime's g_cheats (defined here,
as the app does: EOW_RUNTIME), and run with each cheat off and on. Checks: off, the store writes what the game
computed; on, a smaller value keeps the old one (and the register is set to it); a larger value always goes through;
a trace-only site never keeps; the life restore refills to max only with its cheat; the Might Crystal subtraction gives
99 with its cheat and the real difference without.

Usage: tools/test_cheats.py [cc]   (default: clang)"""
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import patches  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
BASE = 0x7000000000


class FakeNso:
    def __init__(self, image):
        self.image = image


class FakeGen:
    """What patches.word_at / expect read: the instruction at one address."""
    def __init__(self, addr, word):
        self.base = BASE
        img = bytearray(addr - BASE + 4)
        struct.pack_into('<I', img, addr - BASE, word)
        self.nso = FakeNso(bytes(img))


def snippet(patch, word, addr=0x7000001000):
    return patch(FakeGen(addr, word), addr)


def str_w(rt, rn, off):  # str wT, [xN, #off]
    return 0xB9000000 | ((off // 4) << 10) | (rn << 5) | rt


def str_s(rt, rn, off):  # str sT, [xN, #off]
    return 0xBD000000 | ((off // 4) << 10) | (rn << 5) | rt


def main():
    cc = sys.argv[1] if len(sys.argv) > 1 else 'clang'
    cases = []  # (name, C body run with c and mem set up, returns 0 on success)
    wallet = snippet(patches._store_keep(1, 0x1b98), str_w(9, 8, 0x1b98))
    life_trace = snippet(patches._store_keep(0, 0x1b0c), str_w(9, 8, 0x1b0c))
    energy = snippet(patches._store_keep(4, 0x1bb0, True), str_s(0, 20, 0x1bb0))
    might = snippet(patches._might_available, 0x4B090108)
    restore = snippet(patches._life_restore(0x1b08, 0x1b0c), 0xAA0003F3)
    # each patch is checked for the expected original word: a wrong one must fail generation
    try:
        snippet(patches._store_keep(1, 0x1b98), str_w(9, 8, 0x1b9c))
        print('FAIL: a store at another offset was accepted')
        return 1
    except AssertionError:
        pass

    c = r'''
#include <stdio.h>
#include <string.h>
#include "cpu.h"
volatile uint32_t g_cheats;
static int traced;
void rt_cheat_trace(uint64_t site, uint64_t obj, double old, double computed, double stored) {
    (void)site, (void)obj, (void)old, (void)computed, (void)stored;
    traced++;
}
static uint8_t mem[0x2000] __attribute__((aligned(16)));
static int fails;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); fails++; } } while (0)

static void wallet(Cpu *c, uint32_t cheats, int32_t old, int32_t computed) {
    g_cheats = cheats;
    memcpy(mem + 0x1b98, &old, 4);
    c->x[8] = (uint64_t)(uintptr_t)mem;
    c->x[9] = (uint32_t)computed;
    WALLET
}
static void life_trace(Cpu *c, uint32_t cheats, int32_t old, int32_t computed) {
    g_cheats = cheats;
    memcpy(mem + 0x1b0c, &old, 4);
    c->x[8] = (uint64_t)(uintptr_t)mem;
    c->x[9] = (uint32_t)computed;
    LIFE_TRACE
}
static void energy(Cpu *c, uint32_t cheats, float old, float computed) {
    g_cheats = cheats;
    memcpy(mem + 0x1bb0, &old, 4);
    c->x[20] = (uint64_t)(uintptr_t)mem;
    uint32_t b;
    memcpy(&b, &computed, 4);
    c->v[0] = b;
    ENERGY
}
static void might(Cpu *c, uint32_t cheats, uint32_t collected, uint32_t spent) {
    g_cheats = cheats;
    c->x[8] = collected, c->x[9] = spent;
    MIGHT
}
static void restore(Cpu *c, uint32_t cheats, int32_t max, int32_t life) {
    g_cheats = cheats;
    memcpy(mem + 0x1b08, &max, 4);
    memcpy(mem + 0x1b0c, &life, 4);
    c->x[0] = (uint64_t)(uintptr_t)mem;
    c->x[19] = 0;
    RESTORE
}
static int32_t i32at(int off) { int32_t v; memcpy(&v, mem + off, 4); return v; }
static float f32at(int off) { float v; memcpy(&v, mem + off, 4); return v; }

int main(void) {
    static Cpu cpu;
    Cpu *c = &cpu;
    wallet(c, 0, 500, 420);
    CHECK(i32at(0x1b98) == 420 && c->x[9] == 420, "wallet, cheat off: spending lowers it");
    wallet(c, CHEAT_HEALTH | CHEAT_ENERGY, 500, 420);
    CHECK(i32at(0x1b98) == 420, "wallet: another cheat's bit doesn't keep it");
    wallet(c, CHEAT_RUPEES, 500, 420);
    CHECK(i32at(0x1b98) == 500 && c->x[9] == 500, "wallet, cheat on: spending keeps it (memory and register)");
    wallet(c, CHEAT_RUPEES, 500, 600);
    CHECK(i32at(0x1b98) == 600 && c->x[9] == 600, "wallet, cheat on: a find still adds");
    wallet(c, CHEAT_RUPEES, 0, 9999);
    CHECK(i32at(0x1b98) == 9999, "wallet, cheat on: up to the limit");
    life_trace(c, CHEAT_HEALTH, 12, 8);
    CHECK(i32at(0x1b0c) == 8, "trace-only life site: never kept, even with the cheat");
    traced = 0;
    life_trace(c, CHEAT_HEALTH | CHEAT_TRACE, 12, 8);
    CHECK(i32at(0x1b0c) == 8 && traced == 1, "trace-only life site: reported with EOWRECOMP_CHEAT_TRACE");
    energy(c, 0, 30.0f, 29.5f);
    CHECK(f32at(0x1bb0) == 29.5f, "energy, cheat off: drains");
    energy(c, CHEAT_ENERGY, 30.0f, 29.5f);
    CHECK(f32at(0x1bb0) == 30.0f && (uint32_t)c->v[0] == 0x41f00000u, "energy, cheat on: no drain (memory, s0)");
    energy(c, CHEAT_ENERGY, 10.0f, 15.0f);
    CHECK(f32at(0x1bb0) == 15.0f, "energy, cheat on: a pickup still fills");
    energy(c, CHEAT_ENERGY, 0.0f, 0.0f);
    CHECK(f32at(0x1bb0) == 0.0f, "energy, cheat on: equal values unchanged");
    might(c, 0, 40, 35);
    CHECK(c->x[8] == 5, "might, cheat off: collected - spent");
    might(c, CHEAT_MIGHT, 40, 35);
    CHECK(c->x[8] == 99, "might, cheat on: 99 available");
    might(c, CHEAT_RUPEES, 40, 35);
    CHECK(c->x[8] == 5, "might: another cheat's bit doesn't change it");
    restore(c, 0, 20, 8);
    CHECK(i32at(0x1b0c) == 8 && c->x[19] == (uint64_t)(uintptr_t)mem, "restore, cheat off: life as it was (x19 = x0)");
    restore(c, CHEAT_HEALTH, 20, 8);
    CHECK(i32at(0x1b0c) == 20 && c->x[19] == (uint64_t)(uintptr_t)mem, "restore, cheat on: life back to max");
    restore(c, CHEAT_HEALTH, 20, 20);
    CHECK(i32at(0x1b0c) == 20, "restore, cheat on: full stays full");
    if (!fails) puts("cheat patches: PASS");
    return fails != 0;
}
'''
    c = (c.replace('WALLET', wallet).replace('LIFE_TRACE', life_trace).replace('ENERGY\n', energy + '\n')
          .replace('MIGHT\n', might + '\n')
          .replace('RESTORE\n', restore + '\n'))
    with tempfile.TemporaryDirectory() as d:
        src, exe = os.path.join(d, 'test_cheats.c'), os.path.join(d, 'test_cheats.exe')
        open(src, 'w').write(c)
        r = subprocess.run([cc, '-O1', '-Wall', '-DEOW_RUNTIME', '-I', os.path.join(ROOT, 'runtime'), src, '-o', exe])
        if r.returncode:
            print('FAIL: the generated patches do not compile')
            return 1
        return subprocess.run([exe]).returncode


if __name__ == '__main__':
    sys.exit(main())
