// Dynamic-section formats of newer SDK modules. Echoes of Wisdom v1.0.0's modules (nnSdk 16) have DT_HASH beside
// DT_GNU_HASH and plain R_AARCH64_RELATIVE entries in DT_RELA. The v1.1.0 update's (nnSdk 20, measured with
// tools/nso.py) have only DT_GNU_HASH in sdk / subsdk0 / subsdk1, and every module, main included, keeps its relative
// relocations packed in DT_RELR (none left in DT_RELA). A loader without these sees no symbols and leaves every
// pointer in the data unrelocated. Used by runtime/loader.c; tested by tools/test_elf_dyn.c (tools/nso.py has both).
#pragma once
#include <stdint.h>
#include <string.h>

enum { ELF_DT_RELRSZ = 0x23, ELF_DT_RELR = 0x24, ELF_DT_RELRENT = 0x25, ELF_DT_GNU_HASH = 0x6ffffef5 };

// The dynamic symbol count from a DT_GNU_HASH table. Layout (ELF64): u32 nbuckets, symoffset, bloom_size, bloom_shift;
// u64 bloom[bloom_size]; u32 buckets[nbuckets]; u32 chain[]. Every symbol from symoffset on sits in exactly one
// bucket's chain, which ends at the entry whose low bit is set, so the count is one past the end of the chain that
// starts last (symoffset when every bucket is empty).
static inline uint64_t elf_gnu_hash_nsyms(const uint8_t *t) {
    uint32_t nbuckets, symoffset, bloom_size, last = 0;
    memcpy(&nbuckets, t, 4), memcpy(&symoffset, t + 4, 4), memcpy(&bloom_size, t + 8, 4);
    const uint8_t *buckets = t + 16 + (uint64_t)bloom_size * 8, *chain = buckets + (uint64_t)nbuckets * 4;
    for (uint32_t i = 0; i < nbuckets; i++) {
        uint32_t b;
        memcpy(&b, buckets + (uint64_t)i * 4, 4);
        if (b > last) last = b;
    }
    if (last < symoffset) return symoffset;
    for (;; last++) {
        uint32_t h;
        memcpy(&h, chain + (uint64_t)(last - symoffset) * 4, 4);
        if (h & 1) return (uint64_t)last + 1;
    }
}

// Applies a DT_RELR table (n u64 entries) to the module image at `image`, loaded at guest address `base`: each listed
// word gets base added (the implicit addend is the word itself). An even entry is the offset of one word and starts a
// run there; an odd entry is a bitmap whose bits 1..63 mark the 63 words after the run's position, which then moves on
// by 63 words (the ELF gABI "SHT_RELR" encoding, as LLVM's lld writes it). Offsets past image_size are skipped and
// counted in *bad. Returns the number of words relocated.
static inline uint64_t elf_relr_apply(uint8_t *image, uint64_t image_size, uint64_t base, const uint64_t *relr, uint64_t n,
                                      uint64_t *bad) {
    uint64_t where = 0, done = 0;
    if (bad) *bad = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t e = relr[i];
        if (!(e & 1)) {  // an address
            if (e > image_size - 8 || image_size < 8) { if (bad) ++*bad; where = e + 8; continue; }
            uint64_t v;
            memcpy(&v, image + e, 8), v += base, memcpy(image + e, &v, 8);
            done++, where = e + 8;
            continue;
        }
        for (int b = 1; b < 64; b++)  // a bitmap over the next 63 words
            if (e >> b & 1) {
                uint64_t off = where + (uint64_t)(b - 1) * 8, v;
                if (image_size < 8 || off > image_size - 8) { if (bad) ++*bad; continue; }
                memcpy(&v, image + off, 8), v += base, memcpy(image + off, &v, 8);
                done++;
            }
        where += 63 * 8;
    }
    return done;
}
