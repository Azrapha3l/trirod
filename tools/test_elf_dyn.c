// Tests for runtime/elf_dyn.h, the dynamic-section formats of the v1.1.0 update's nnSdk 20 modules: the symbol count
// from DT_GNU_HASH tables laid out as a linker writes them (symbols from symoffset on grouped by bucket, each chain's
// last hash with its low bit set), and DT_RELR packed relative relocations encoded the way lld does.
// Build and run: clang -O2 -Wall -Iruntime -o build/test_elf_dyn tools/test_elf_dyn.c && build/test_elf_dyn
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "../runtime/elf_dyn.h"

static int fails;

static uint32_t gnu_hash(const char *s) {
    uint32_t h = 5381;
    for (; *s; s++) h = h * 33 + (uint8_t)*s;
    return h;
}

// A table for n symbols after symoffset undefined ones, nbuckets buckets; returns its size in *len
static uint8_t *make_table(uint32_t symoffset, uint32_t n, uint32_t nbuckets, size_t *len) {
    uint32_t bloom = 2, *hash = calloc(n ? n : 1, 4), *order = calloc(n ? n : 1, 4);
    char name[32];
    for (uint32_t i = 0; i < n; i++) snprintf(name, sizeof name, "sym_%u", i * 7919u), hash[i] = gnu_hash(name);
    uint32_t k = 0;  // the linker sorts the defined symbols by bucket
    for (uint32_t b = 0; b < nbuckets; b++)
        for (uint32_t i = 0; i < n; i++)
            if (hash[i] % nbuckets == b) order[k++] = i;
    *len = 16 + bloom * 8 + (size_t)nbuckets * 4 + (size_t)n * 4;
    uint8_t *t = calloc(1, *len);
    uint32_t hdr[4] = {nbuckets, symoffset, bloom, 6};
    memcpy(t, hdr, 16);
    uint8_t *buckets = t + 16 + bloom * 8, *chain = buckets + (size_t)nbuckets * 4;
    for (uint32_t j = 0; j < n; j++) {
        uint32_t h = hash[order[j]], b = h % nbuckets, idx = symoffset + j;
        uint32_t first;
        memcpy(&first, buckets + b * 4, 4);
        if (!first) memcpy(buckets + b * 4, &idx, 4);
        bool last = j + 1 == n || hash[order[j + 1]] % nbuckets != b;
        h = last ? h | 1 : h & ~1u;
        memcpy(chain + (size_t)j * 4, &h, 4);
    }
    free(hash), free(order);
    return t;
}

static void check(uint32_t symoffset, uint32_t n, uint32_t nbuckets) {
    size_t len;
    uint8_t *t = make_table(symoffset, n, nbuckets, &len);
    uint64_t got = elf_gnu_hash_nsyms(t);
    bool ok = got == (uint64_t)symoffset + n;
    printf("%s symoffset %u, %u defined, %u buckets -> %llu\n", ok ? "ok  " : "FAIL", symoffset, n, nbuckets,
           (unsigned long long)got);
    fails += !ok;
    free(t);
}

// lld's DT_RELR encoding of sorted, 8-aligned offsets: an address entry, then bitmaps over the following 63-word windows
static uint64_t relr_encode(const uint64_t *offs, uint64_t n, uint64_t *out) {
    uint64_t k = 0, i = 0;
    while (i < n) {
        out[k++] = offs[i];
        uint64_t base = offs[i++] + 8;
        for (;;) {
            uint64_t bitmap = 0;
            while (i < n && offs[i] >= base && offs[i] - base < 63 * 8 && (offs[i] - base) % 8 == 0)
                bitmap |= 1ull << ((offs[i++] - base) / 8 + 1);
            if (!bitmap) break;
            out[k++] = bitmap | 1;
            base += 63 * 8;
        }
    }
    return k;
}

static void check_relr(const char *what, const uint64_t *offs, uint64_t n) {
    enum { SIZE = 0x10000 };
    static uint8_t img[SIZE], want[SIZE];
    static uint64_t relr[4096];
    for (int i = 0; i < SIZE; i++) img[i] = (uint8_t)(i * 37 + 11);
    memcpy(want, img, SIZE);
    const uint64_t base = 0x7000000000ull;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t v;
        memcpy(&v, want + offs[i], 8), v += base, memcpy(want + offs[i], &v, 8);
    }
    uint64_t k = relr_encode(offs, n, relr), bad = 1;
    uint64_t done = elf_relr_apply(img, SIZE, base, relr, k, &bad);
    bool ok = done == n && bad == 0 && !memcmp(img, want, SIZE);
    printf("%s relr %s (%llu offsets in %llu entries)\n", ok ? "ok  " : "FAIL", what, (unsigned long long)n,
           (unsigned long long)k);
    fails += !ok;
}

int main(void) {
    check(1, 1, 1);        // one symbol
    check(12, 500, 37);    // a typical module: imports first, many buckets
    check(700, 3, 64);     // most buckets empty
    check(5, 200, 1);      // every symbol in one chain
    check(9, 0, 4);        // nothing defined: the count is symoffset
    check(1, 23030, 4099); // the v1.1.0 sdk's size: 23 031 dynamic symbols in all (measured)
    static uint64_t offs[2048];
    uint64_t n = 0;
    offs[n++] = 0x100;
    check_relr("one address", offs, n);
    n = 0;
    for (uint64_t o = 0x200; o < 0x200 + 8 * 200; o += 8) offs[n++] = o;  // a dense run over several bitmaps
    check_relr("a dense run (address + 4 bitmaps)", offs, n);
    n = 0;
    for (uint64_t o = 0x1000; o < 0xF000; o += 8 * (1 + (o >> 3) % 70)) offs[n++] = o;  // gaps above 63 words too
    check_relr("sparse offsets with gaps over a bitmap window", offs, n);
    n = 0;
    offs[n++] = 0x40, offs[n++] = 0x48 + 62 * 8, offs[n++] = 0x48 + 63 * 8;  // last bit of a window, then the next
    check_relr("a window's last word and the next window", offs, n);
    uint64_t bogus[2] = {0x20000, 0x8 | 1}, bad = 0;  // an address past the image, then a bitmap after it
    static uint8_t small[0x100];
    elf_relr_apply(small, sizeof small, 1, bogus, 2, &bad);
    bool ok = bad == 2;
    printf("%s relr entries outside the image are counted, not written\n", ok ? "ok  " : "FAIL");
    fails += !ok;
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
