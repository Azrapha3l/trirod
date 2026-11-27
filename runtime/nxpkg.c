// The player's game package read in place (nxpkg.h). Layout as in Atmosphere's fssystem (NcaFileSystemDriver,
// CompressedStorage, BucketTree) and Switchbrew's NCA / XCI / PFS0 / HFS0 pages, the same as tools/nca_extract.py:
//   package (NSP = PFS0 of NCAs; XCI = HFS0 root -> "secure" HFS0 of NCAs) -> program NCA (header: AES-XTS with
//   header_key) -> section (AES-CTR, key from the key area or the title key) -> hash layers -> [CompressedStorage]
//   -> PFS0 (ExeFS) / RomFS.
// Hashes are not verified (the files are the player's own dump; a bad one shows up as a game error).
#include "nxpkg.h"
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#if defined(__linux__)
#include <sys/auxv.h>
#ifndef HWCAP_AES
#define HWCAP_AES (1 << 3)
#endif
#endif
#endif

#ifdef __ANDROID__
#include "platform/android.h"
#endif

#define WHY(...) (snprintf(why, why_size, __VA_ARGS__), false)

// The package by path, or (Android) by the document URI the folder picker gave access to
static int open_package(const char *path) {
#ifdef __ANDROID__
    if (!strncmp(path, "content://", 10)) return android_open_document(path);
#endif
    return open(path, O_RDONLY);
}

// ------------------------------------------------------------------ AES-128 (software; ARMv8 AES for CTR)
static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9,
    0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f,
    0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07,
    0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3,
    0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58,
    0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3,
    0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f,
    0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac,
    0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a,
    0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70,
    0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42,
    0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};
static uint8_t INV_SBOX[256];

static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (; b; b >>= 1, a = xt(a)) if (b & 1) r ^= a;
    return r;
}

static void init_inv(void) {
    for (int i = 0; i < 256; i++) INV_SBOX[SBOX[i]] = (uint8_t)i;
}

static void aes_expand(const uint8_t key[16], uint8_t rk[176]) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, init_inv);
    memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
        if (i % 16 == 0) {
            uint8_t u = t[0];
            t[0] = SBOX[t[1]] ^ rcon, t[1] = SBOX[t[2]], t[2] = SBOX[t[3]], t[3] = SBOX[u];
            rcon = xt(rcon);
        }
        for (int k = 0; k < 4; k++) rk[i + k] = rk[i - 16 + k] ^ t[k];
    }
}
static void aes_encrypt_block(const uint8_t rk[176], uint8_t s[16]) {
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
    for (int r = 1; r <= 10; r++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[i] = SBOX[s[(i + 4 * (i % 4)) % 16]];  // SubBytes + ShiftRows (column-major)
        if (r < 10)
            for (int c = 0; c < 4; c++) {
                uint8_t *q = t + 4 * c, a0 = q[0], a1 = q[1], a2 = q[2], a3 = q[3];
                q[0] = xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3;
                q[1] = a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3;
                q[2] = a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3;
                q[3] = xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3);
            }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ rk[16 * r + i];
    }
}

static void aes_decrypt_block(const uint8_t rk[176], uint8_t s[16]) {
    for (int i = 0; i < 16; i++) s[i] ^= rk[160 + i];
    for (int r = 9; r >= 0; r--) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[(i + 4 * (i % 4)) % 16] = INV_SBOX[s[i]];  // InvShiftRows + InvSubBytes
        for (int i = 0; i < 16; i++) t[i] ^= rk[16 * r + i];
        if (r > 0)
            for (int c = 0; c < 4; c++) {
                uint8_t *q = t + 4 * c, a0 = q[0], a1 = q[1], a2 = q[2], a3 = q[3];
                q[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
                q[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                q[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
                q[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
            }
        memcpy(s, t, 16);
    }
}

void nx_aes128_ecb_decrypt(const uint8_t key[16], uint8_t *data, size_t len) {
    uint8_t rk[176];
    aes_expand(key, rk);
    for (size_t i = 0; i + 16 <= len; i += 16) aes_decrypt_block(rk, data + i);
}

static void ctr_inc(uint8_t c[16]) {
    for (int i = 15; i >= 0 && !++c[i]; i--) {}
}

#if defined(__aarch64__)
static bool have_arm_aes(void) {
#if defined(__APPLE__)
    return true;  // every Apple Silicon CPU
#elif defined(__linux__)
    return (getauxval(AT_HWCAP) & HWCAP_AES) != 0;
#else
    return false;
#endif
}

__attribute__((target("aes"))) static void ctr_arm(const uint8_t rk[176], uint8_t ctr[16], uint8_t *data, size_t len) {
    uint8x16_t k[11];
    for (int i = 0; i < 11; i++) k[i] = vld1q_u8(rk + 16 * i);
    while (len) {
        uint8x16_t b = vld1q_u8(ctr);
        for (int r = 0; r < 9; r++) b = vaesmcq_u8(vaeseq_u8(b, k[r]));
        b = veorq_u8(vaeseq_u8(b, k[9]), k[10]);
        size_t n = len < 16 ? len : 16;
        if (n == 16) vst1q_u8(data, veorq_u8(vld1q_u8(data), b));
        else {
            uint8_t ks[16];
            vst1q_u8(ks, b);
            for (size_t i = 0; i < n; i++) data[i] ^= ks[i];
        }
        data += n, len -= n;
        ctr_inc(ctr);
    }
}
#endif

void nx_aes128_ctr(const uint8_t key[16], const uint8_t iv[16], uint8_t *data, size_t len) {
    uint8_t rk[176], ctr[16];
    aes_expand(key, rk);
    memcpy(ctr, iv, 16);
#if defined(__aarch64__)
    static int arm = -1;
    if (arm < 0) arm = have_arm_aes();
    if (arm) { ctr_arm(rk, ctr, data, len); return; }
#endif
    while (len) {
        uint8_t ks[16];
        memcpy(ks, ctr, 16);
        aes_encrypt_block(rk, ks);
        size_t n = len < 16 ? len : 16;
        for (size_t i = 0; i < n; i++) data[i] ^= ks[i];
        data += n, len -= n;
        ctr_inc(ctr);
    }
}

// AES-XTS with Nintendo's tweak (the sector number big-endian), 0x200-byte sectors: the NCA header.
static void aes_xts_decrypt(const uint8_t key[32], uint8_t *data, size_t len, uint64_t sector) {
    uint8_t rk1[176], rk2[176];
    aes_expand(key, rk1);
    aes_expand(key + 16, rk2);
    for (size_t s = 0; s < len; s += 0x200, sector++) {
        uint8_t tw[16] = {0};
        for (int i = 0; i < 8; i++) tw[15 - i] = (uint8_t)(sector >> (8 * i));
        aes_encrypt_block(rk2, tw);
        for (size_t b = 0; b < 0x200; b += 16) {
            uint8_t *p = data + s + b;
            for (int i = 0; i < 16; i++) p[i] ^= tw[i];
            aes_decrypt_block(rk1, p);
            for (int i = 0; i < 16; i++) p[i] ^= tw[i];
            uint8_t carry = tw[15] >> 7;  // tweak * x in GF(2^128), little-endian bytes
            for (int i = 15; i > 0; i--) tw[i] = (uint8_t)(tw[i] << 1 | tw[i - 1] >> 7);
            tw[0] = (uint8_t)(tw[0] << 1) ^ (carry ? 0x87 : 0);
        }
    }
}

// ------------------------------------------------------------------ LZ4 block
int nx_lz4_decompress(const uint8_t *src, int src_len, uint8_t *dst, int dst_len) {
    const uint8_t *ip = src, *iend = src + src_len;
    uint8_t *op = dst, *oend = dst + dst_len;
    while (ip < iend) {
        unsigned tok = *ip++;
        size_t lit = tok >> 4;
        if (lit == 15) {
            unsigned b;
            do { if (ip >= iend) return -1; b = *ip++; lit += b; } while (b == 255);
        }
        if ((size_t)(iend - ip) < lit || (size_t)(oend - op) < lit) return -1;
        memcpy(op, ip, lit);
        op += lit, ip += lit;
        if (ip >= iend) break;  // the last sequence has literals only
        if (iend - ip < 2) return -1;
        size_t off = ip[0] | ip[1] << 8;
        ip += 2;
        size_t ml = tok & 15;
        if (ml == 15) {
            unsigned b;
            do { if (ip >= iend) return -1; b = *ip++; ml += b; } while (b == 255);
        }
        ml += 4;
        if (!off || off > (size_t)(op - dst) || (size_t)(oend - op) < ml) return -1;
        const uint8_t *m = op - off;
        if (off >= ml) memcpy(op, m, ml), op += ml;
        else while (ml--) *op++ = *m++;  // overlapping: byte by byte
    }
    return (int)(op - dst);
}

// ------------------------------------------------------------------ keys
typedef struct { char name[64]; uint8_t val[32]; int len; } Key;
typedef struct { Key *k; int n; } Keys;

static int unhex(const char *s, uint8_t *out, int cap) {
    int n = 0;
    while (s[0] && s[1] && n < cap) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1) break;
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static bool keys_load(const char *path, Keys *ks) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char name[64], val[256];
        if (sscanf(line, " %63s", name) != 1 || sscanf(eq + 1, " %255s", val) != 1) continue;
        ks->k = realloc(ks->k, sizeof *ks->k * (ks->n + 1));
        Key *k = &ks->k[ks->n++];
        snprintf(k->name, sizeof k->name, "%s", name);
        for (char *c = k->name; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
        k->len = unhex(val, k->val, 32);
    }
    fclose(f);
    return true;
}

static const uint8_t *key_get(const Keys *ks, const char *name, int len) {
    for (int i = 0; i < ks->n; i++)
        if (!strcmp(ks->k[i].name, name) && ks->k[i].len == len) return ks->k[i].val;
    return NULL;
}

// ------------------------------------------------------------------ package -> program NCA
typedef struct { char name[256]; uint64_t off, size; } Entry;
typedef struct { Entry *e; int n; } Entries;

static void entries_add(Entries *es, const char *name, uint64_t off, uint64_t size) {
    es->e = realloc(es->e, sizeof *es->e * (es->n + 1));
    Entry *e = &es->e[es->n++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->off = off, e->size = size;
}

static bool pread_all(int fd, void *dst, size_t n, uint64_t off) {
    uint8_t *p = dst;
    while (n) {
        ssize_t r = pread(fd, p, n, (off_t)off);
        if (r <= 0) return false;
        p += r, n -= (size_t)r, off += (uint64_t)r;
    }
    return true;
}

// PFS0 (entry 0x18) / HFS0 (entry 0x40) at `base`: its files, offsets absolute in the package
static bool read_fs0(int fd, uint64_t base, Entries *out) {
    uint8_t h[16];
    if (!pread_all(fd, h, 16, base)) return false;
    bool hfs = !memcmp(h, "HFS0", 4);
    if (memcmp(h, "PFS0", 4) && !hfs) return false;
    uint32_t n, strsz;
    memcpy(&n, h + 4, 4), memcpy(&strsz, h + 8, 4);
    if (n > 4096 || strsz > (1u << 20)) return false;
    size_t esz = hfs ? 0x40 : 0x18, meta = 16 + n * esz + strsz;
    uint8_t *m = malloc(meta);
    if (!pread_all(fd, m, meta, base)) { free(m); return false; }
    const char *strs = (const char *)m + 16 + n * esz;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t off, size;
        uint32_t no;
        memcpy(&off, m + 16 + i * esz, 8), memcpy(&size, m + 16 + i * esz + 8, 8), memcpy(&no, m + 16 + i * esz + 16, 4);
        if (no >= strsz) continue;
        char name[256];
        snprintf(name, sizeof name, "%.*s", (int)strnlen(strs + no, strsz - no), strs + no);
        entries_add(out, name, base + meta + off, size);
    }
    free(m);
    return true;
}

typedef struct {
    int fd;
    uint64_t base;      // the NCA's offset in the package
    uint8_t hdr[0xC00];  // decrypted header
    uint8_t ctr_key[16];
} Nca;

typedef struct {
    uint64_t off, size;           // in the NCA
    uint8_t fs_type, hash_type, enc_type;
    uint8_t ctr_hi[8];
    uint64_t data_off, data_size;  // the payload under the hash layers, relative to the section
    uint64_t comp_off, comp_size;  // compression table, relative to the payload
    uint8_t comp_hdr[16];
    uint64_t indirect_off, indirect_size;
    uint8_t indirect_hdr[16];
    uint64_t ctr_ex_off, ctr_ex_size;
    uint8_t ctr_ex_hdr[16];
    uint8_t meta_hash_type;              // fs header 0x5: patch meta hash type (1 = HierarchicalIntegrity)
    uint64_t meta_hash_off, meta_hash_size;  // fs header 0x1A0 / 0x1A8: the patch meta hash data (newer updates)
} Section;

static bool nca_header(int fd, uint64_t base, const Keys *ks, Nca *nca) {
    const uint8_t *hk = key_get(ks, "header_key", 32);
    if (!hk) return false;
    nca->fd = fd, nca->base = base;
    if (!pread_all(fd, nca->hdr, 0xC00, base)) return false;
    aes_xts_decrypt(hk, nca->hdr, 0xC00, 0);
    return !memcmp(nca->hdr + 0x200, "NCA3", 4);
}

static bool section_info(const Nca *nca, int i, Section *s) {
    uint32_t st, en;
    memcpy(&st, nca->hdr + 0x240 + i * 16, 4), memcpy(&en, nca->hdr + 0x244 + i * 16, 4);
    if (st == 0 && en == 0) return false;
    const uint8_t *fh = nca->hdr + 0x400 + i * 0x200;
    memset(s, 0, sizeof *s);
    s->off = (uint64_t)st * 0x200, s->size = (uint64_t)(en - st) * 0x200;
    s->fs_type = fh[2], s->hash_type = fh[3], s->enc_type = fh[4];
    for (int k = 0; k < 8; k++) s->ctr_hi[k] = fh[0x147 - k];
    if (s->hash_type == 2) {  // HierarchicalSha256: the last layer region is the data
        uint32_t layers;
        memcpy(&layers, fh + 0x8 + 0x24, 4);
        if (layers < 1 || layers > 5) return false;
        memcpy(&s->data_off, fh + 0x8 + 0x28 + (layers - 1) * 16, 8);
        memcpy(&s->data_size, fh + 0x8 + 0x30 + (layers - 1) * 16, 8);
    } else if (s->hash_type == 3) {  // HierarchicalIntegrity (IVFC): the last level is the data
        uint32_t levels;
        if (memcmp(fh + 0x8, "IVFC", 4)) return false;
        memcpy(&levels, fh + 0x8 + 0xC, 4);
        if (levels < 2 || levels > 7) return false;
        memcpy(&s->data_off, fh + 0x8 + 0x10 + (levels - 2) * 0x18, 8);
        memcpy(&s->data_size, fh + 0x8 + 0x18 + (levels - 2) * 0x18, 8);
    } else return false;
    memcpy(&s->comp_off, fh + 0x178, 8), memcpy(&s->comp_size, fh + 0x180, 8);
    memcpy(s->comp_hdr, fh + 0x188, 16);
    memcpy(&s->indirect_off, fh + 0x100, 8), memcpy(&s->indirect_size, fh + 0x108, 8);
    memcpy(s->indirect_hdr, fh + 0x110, 16);
    memcpy(&s->ctr_ex_off, fh + 0x120, 8), memcpy(&s->ctr_ex_size, fh + 0x128, 8);
    memcpy(s->ctr_ex_hdr, fh + 0x130, 16);
    s->meta_hash_type = fh[5];
    memcpy(&s->meta_hash_off, fh + 0x1A0, 8), memcpy(&s->meta_hash_size, fh + 0x1A8, 8);
    return true;
}

// AES-CTR bytes [off, off+n) of a section (off relative to the NCA section).
static bool section_read_ctr(int fd, uint64_t nca_base, const Section *s, const uint8_t key[16], uint8_t *dst,
                             uint64_t off, size_t n) {
    uint64_t a = off & ~15ull, pre = off - a;
    if (s->enc_type == 1) return pread_all(fd, dst, n, nca_base + s->off + off);
    uint8_t *tmp = pre ? malloc(n + pre) : dst;
    if (!pread_all(fd, tmp, n + pre, nca_base + s->off + a)) { if (pre) free(tmp); return false; }
    uint8_t iv[16];
    memcpy(iv, s->ctr_hi, 8);
    uint64_t blk = (s->off + a) >> 4;
    for (int k = 0; k < 8; k++) iv[15 - k] = (uint8_t)(blk >> (8 * k));
    nx_aes128_ctr(key, iv, tmp, n + pre);
    if (pre) memcpy(dst, tmp + pre, n), free(tmp);
    return true;
}

// Decrypted bytes [off, off+n) of an ordinary AES-CTR section.
static bool section_read(int fd, uint64_t nca_base, const Section *s, const uint8_t key[16], uint8_t *dst, uint64_t off,
                         size_t n) {
    if (s->enc_type != 1 && s->enc_type != 3) return false;
    return section_read_ctr(fd, nca_base, s, key, dst, off, n);
}

// ------------------------------------------------------------------ CompressedStorage
// BucketTree of CompressedStorage::Entry {s64 virt; s64 phys; u8 type; s8 level; u16; u32 phys_size}, node size
// 0x4000. Types: 0 none (copy), 1 zeros, 3 LZ4.
typedef struct { int64_t virt, phys; uint8_t type; uint32_t psize; } CEntry;
typedef struct { uint8_t *data; uint64_t entry; uint32_t len; uint64_t stamp; } CBlock;
// AesCtrCounterExtendedStorage::Entry {u8 offset[8]; u8 encryption_value (0 encrypted, 1 not); u8[3]; s32 generation}
typedef struct { uint64_t phys; uint32_t generation; uint8_t plain; } CtrEntry;
typedef struct { uint64_t virt, phys; uint32_t storage; } IndirectEntry;

typedef struct {
    int base_fd;
    uint64_t base_nca;
    Section base_sec;
    uint8_t base_key[16];
    CtrEntry *ctr;
    uint64_t nctr, patch_data_size;  // CTR-Ex covers [0, ctr_ex_off) of the section
    uint64_t indirect_data_size;     // IndirectStorage source 1 is [0, indirect_off) of that
    IndirectEntry *indirect;
    uint64_t nindirect, virtual_size;
} NxPatch;

#define NCACHE 16

struct NxRomfs {
    int fd;
    uint64_t nca_base;
    Section sec;
    uint8_t key[16];
    uint64_t size;  // the RomFS image's
    CEntry *ent;
    uint64_t nent;
    pthread_mutex_t lock;
    CBlock cache[NCACHE];
    uint64_t stamp;
    NxPatch *patch;
};

#define BKTR_NODE_SIZE 0x4000u

// Bucket-tree arrays use 0x4000-byte nodes. The root/index nodes come first, then
// entry-set nodes. Each entry-set node carries its index, count and logical end.
static bool bktr_nodes(uint8_t *table, size_t table_size, uint32_t count, uint32_t entry_size, uint8_t **sets_out,
                       uint32_t *set_count_out, uint64_t *end_out) {
    if (!count || !entry_size) return false;
    uint32_t per_set = (BKTR_NODE_SIZE - 0x10u) / entry_size;
    uint32_t set_count = (count + per_set - 1) / per_set;
    uint32_t per_index = (BKTR_NODE_SIZE - 0x10u) / 8u;
    uint32_t levels = set_count <= per_index ? 0 : (set_count - per_index + per_index - 2) / (per_index - 1);
    uint64_t node_bytes = (uint64_t)(1 + levels) * BKTR_NODE_SIZE;
    if (node_bytes + (uint64_t)set_count * BKTR_NODE_SIZE > table_size) return false;
    for (uint32_t i = 0; i < set_count; i++) {
        uint8_t *node = table + node_bytes + (uint64_t)i * BKTR_NODE_SIZE;
        int32_t idx, n;
        memcpy(&idx, node, 4);
        memcpy(&n, node + 4, 4);
        if (idx != (int32_t)i || n < 0 || (uint32_t)n > per_set) return false;
        if (i + 1 == set_count) memcpy(end_out, node + 8, 8);
    }
    *sets_out = table + node_bytes;
    *set_count_out = set_count;
    return true;
}

static bool ctr_ex_read(NxRomfs *r, uint8_t *dst, uint64_t size, uint64_t off);

// Read the section's decrypted payload. For an update NCA this is the BKTR
// indirection over base data and the update's AES-CTR-Ex patch data.
static bool romfs_storage_read(NxRomfs *r, uint8_t *dst, uint64_t size, uint64_t off) {
    if (r->patch) {
        NxPatch *p = r->patch;
        if (off >= p->virtual_size || size > p->virtual_size - off) return false;
        uint64_t lo = 0, hi = p->nindirect;
        while (hi - lo > 1) {
            uint64_t mid = (lo + hi) / 2;
            if (p->indirect[mid].virt <= off) lo = mid;
            else hi = mid;
        }
        uint64_t done = 0;
        while (done < size) {
            IndirectEntry *e = &p->indirect[lo];
            uint64_t end = lo + 1 < p->nindirect ? p->indirect[lo + 1].virt : p->virtual_size;
            uint64_t cur = off + done;
            if (cur < e->virt || cur >= end) return false;
            uint64_t n = end - cur < size - done ? end - cur : size - done;
            uint64_t phys = (uint64_t)e->phys + (cur - e->virt);
            bool ok;
            if (e->storage == 0)  // the base NCA's whole RomFS section, as Atmosphere's IndirectStorage index 0
                ok = phys <= p->base_sec.size && n <= p->base_sec.size - phys &&
                     section_read(p->base_fd, p->base_nca, &p->base_sec, p->base_key, dst + done, phys, (size_t)n);
            else if (e->storage == 1)  // the update's own data, below its relocation table
                ok = phys <= p->indirect_data_size && n <= p->indirect_data_size - phys &&
                     ctr_ex_read(r, dst + done, n, phys);
            else
                return false;
            if (!ok) return false;
            done += n;
            if (cur + n == end && lo + 1 < p->nindirect) lo++;
        }
        return true;
    }
    // (callers already add data_off: off is relative to the section, as in the patch case)
    return section_read(r->fd, r->nca_base, &r->sec, r->key, dst, off, (size_t)size);
}

static bool comp_load(NxRomfs *r, char *why, size_t why_size) {
    const uint8_t *h = r->sec.comp_hdr;
    if (memcmp(h, "BKTR", 4)) return WHY("bad compression table header");
    int32_t count;
    memcpy(&count, h + 8, 4);
    const uint64_t NODE = 0x4000, ENTRY = 0x18;
    uint8_t *table = malloc(r->sec.comp_size);
    if (!table || !romfs_storage_read(r, table, r->sec.comp_size, r->sec.data_off + r->sec.comp_off)) {
        free(table);
        return WHY("can't read the compression table (package cut short or damaged BKTR map)");
    }
    uint64_t per_set = (NODE - 0x10) / ENTRY, sets = (count + per_set - 1) / per_set, per_node = (NODE - 0x10) / 8;
    uint64_t l2 = sets <= per_node ? 0 : (sets - per_node + per_node - 2) / (per_node - 1);
    uint64_t node_storage = (1 + l2) * NODE;
    r->ent = calloc((size_t)count + 1, sizeof *r->ent);
    r->nent = 0;
    int64_t end = 0;
    for (uint64_t s = 0; s < sets; s++) {
        uint64_t b = node_storage + s * NODE;
        if (b + NODE > r->sec.comp_size) { free(table); return WHY("compression table cut short"); }
        int32_t idx, n;
        memcpy(&idx, table + b, 4), memcpy(&n, table + b + 4, 4), memcpy(&end, table + b + 8, 8);
        if ((uint64_t)idx != s || n < 0 || (uint64_t)n > per_set) { free(table); return WHY("bad compression table"); }
        for (int e = 0; e < n && r->nent < (uint64_t)count; e++) {
            const uint8_t *p = table + b + 0x10 + e * ENTRY;
            CEntry *c = &r->ent[r->nent++];
            memcpy(&c->virt, p, 8), memcpy(&c->phys, p + 8, 8), c->type = p[16], memcpy(&c->psize, p + 20, 4);
        }
    }
    free(table);
    if (r->nent != (uint64_t)count) return WHY("compression table: %llu entries of %d", (unsigned long long)r->nent, count);
    r->ent[r->nent].virt = end;  // (the sentinel: the end of the last entry)
    r->size = (uint64_t)end;
    return true;
}

static bool patch_load(NxRomfs *r, int base_fd, uint64_t base_nca, const Section *base_sec, const uint8_t base_key[16],
                      char *why, size_t why_size) {
    Section *s = &r->sec;
    if (s->enc_type != 4 || memcmp(s->indirect_hdr, "BKTR", 4) || memcmp(s->ctr_ex_hdr, "BKTR", 4))
        return WHY("the update's RomFS uses encryption type %u without the AES-CTR-Ex table this reads (type 4)",
                   s->enc_type);
    uint32_t icount, ccount;
    memcpy(&icount, s->indirect_hdr + 8, 4);
    memcpy(&ccount, s->ctr_ex_hdr + 8, 4);
    // (sizes capped so a damaged header can't ask for gigabytes; overflow-safe range checks)
    if (!icount || icount > 1000000 || !ccount || ccount > 1000000 || s->indirect_size > (256u << 20) ||
        s->ctr_ex_size > (256u << 20) || s->indirect_off >= s->ctr_ex_off ||
        s->indirect_size > s->ctr_ex_off - s->indirect_off || s->ctr_ex_off > s->size ||
        s->ctr_ex_size > s->size - s->ctr_ex_off)
        return WHY("the update BKTR table sizes or offsets are invalid");

    // A patch meta hash layer (NcaFsHeaderReader::ExistsPatchMetaHashLayer: meta hash data size != 0 and a relocation
    // table) puts both tables in one ordinary AES-CTR region, indirect_off .. end of the hash data, under an integrity
    // layer (fssystem_nca_file_system_driver.cpp CreatePatchMetaStorage). Hashes aren't verified here (as for every
    // layer: the header comment); without that layer the relocation table is CTR-Ex data.
    bool meta_layer = s->meta_hash_size != 0;
    if (meta_layer && (s->meta_hash_type != 1 || s->ctr_ex_off + s->ctr_ex_size > s->meta_hash_off ||
                       s->meta_hash_off > s->size || s->meta_hash_size > s->size - s->meta_hash_off))
        return WHY("the update's patch meta hash layer (type %u) is not the supported HierarchicalIntegrity layout",
                   s->meta_hash_type);
    NxPatch *p = calloc(1, sizeof *p);
    if (!p) return WHY("not enough memory for the update BKTR map");
    p->base_fd = base_fd;
    p->base_nca = base_nca;
    p->base_sec = *base_sec;
    memcpy(p->base_key, base_key, 16);
    // CTR-Ex backs both patch bytes and the encrypted indirect table; the
    // counter table itself begins at ctr_ex_off and uses ordinary CTR.
    p->patch_data_size = s->ctr_ex_off;
    p->indirect_data_size = s->indirect_off;

    uint8_t *table = malloc((size_t)s->ctr_ex_size);
    uint8_t *sets = NULL;
    uint32_t set_count = 0;
    uint64_t table_end = 0;
    if (!table || !section_read_ctr(r->fd, r->nca_base, s, r->key, table, s->ctr_ex_off, (size_t)s->ctr_ex_size) ||
        !bktr_nodes(table, (size_t)s->ctr_ex_size, ccount, 16, &sets, &set_count, &table_end)) {
        free(table);
        free(p);
        return WHY("the AES-CTR-Ex table is incomplete or malformed");
    }
    p->ctr = calloc(ccount, sizeof *p->ctr);
    if (!p->ctr) { free(table); free(p); return WHY("not enough memory for the AES-CTR-Ex table"); }
    uint32_t ctr_read = 0;
    for (uint32_t set = 0; set < set_count; set++) {
        uint8_t *node = sets + (uint64_t)set * BKTR_NODE_SIZE;
        int32_t n;
        memcpy(&n, node + 4, 4);
        for (int32_t i = 0; i < n && ctr_read < ccount; i++, ctr_read++) {
            const uint8_t *e = node + 0x10 + (uint64_t)i * 16;
            memcpy(&p->ctr[ctr_read].phys, e, 8);
            memcpy(&p->ctr[ctr_read].generation, e + 12, 4);
            p->ctr[ctr_read].plain = e[8];
            // (entries are AES-block aligned, increasing, inside the data, and either encrypted or not)
            if ((ctr_read && p->ctr[ctr_read].phys <= p->ctr[ctr_read - 1].phys) ||
                p->ctr[ctr_read].phys > p->patch_data_size || (p->ctr[ctr_read].phys & 15) || e[8] > 1) {
                free(table); free(p->ctr); free(p);
                return WHY("the AES-CTR-Ex entries are out of order");
            }
        }
    }
    free(table);
    if (ctr_read != ccount) { free(p->ctr); free(p); return WHY("the AES-CTR-Ex table entry count is inconsistent"); }
    if (p->ctr[0].phys != 0) { free(p->ctr); free(p); return WHY("the AES-CTR-Ex table does not start at offset zero"); }
    p->nctr = ccount;
    r->patch = p;

    uint8_t *ind = malloc((size_t)s->indirect_size);
    bool ind_ok = ind && (meta_layer ? section_read_ctr(r->fd, r->nca_base, s, r->key, ind, s->indirect_off,
                                                        (size_t)s->indirect_size)
                                     : ctr_ex_read(r, ind, s->indirect_size, s->indirect_off));
    if (!ind_ok) {
        free(ind); r->patch = NULL; free(p->ctr); free(p);
        return WHY("can't decrypt the update relocation table");
    }
    // IndirectStorage entries are 20 bytes (u64 virtual, u64 physical,
    // u32 storage index); the bucket capacity in this package is 818.
    if (!bktr_nodes(ind, (size_t)s->indirect_size, icount, 20, &sets, &set_count, &p->virtual_size)) {
        free(ind); r->patch = NULL; free(p->ctr); free(p);
        return WHY("the update relocation table is incomplete or malformed");
    }
    p->indirect = calloc(icount, sizeof *p->indirect);
    if (!p->indirect) { free(ind); r->patch = NULL; free(p->ctr); free(p); return WHY("not enough memory for the update relocation map"); }
    uint64_t nread = 0;
    for (uint32_t set = 0; set < set_count; set++) {
        uint8_t *node = sets + (uint64_t)set * BKTR_NODE_SIZE;
        int32_t n;
        memcpy(&n, node + 4, 4);
        for (int32_t i = 0; i < n && nread < icount; i++, nread++) {
            const uint8_t *e = node + 0x10 + (uint64_t)i * 20;
            memcpy(&p->indirect[nread].virt, e, 8);
            memcpy(&p->indirect[nread].phys, e + 8, 8);
            memcpy(&p->indirect[nread].storage, e + 16, 4);
            if (nread && p->indirect[nread].virt <= p->indirect[nread - 1].virt) {
                uint64_t prev = p->indirect[nread - 1].virt, cur = p->indirect[nread].virt;
                free(ind); r->patch = NULL; free(p->indirect); free(p->ctr); free(p);
                return WHY("the update relocation offsets are not increasing at entry %llu (%llx after %llx)",
                           (unsigned long long)nread, (unsigned long long)cur, (unsigned long long)prev);
            }
            if (p->indirect[nread].storage > 1) {
                uint32_t storage = p->indirect[nread].storage;
                uint64_t phys = p->indirect[nread].phys, virt = p->indirect[nread].virt;
                free(ind); r->patch = NULL; free(p->indirect); free(p->ctr); free(p);
                return WHY("unsupported update relocation source %u at entry %llu (virtual %llx, physical %llx)",
                           storage, (unsigned long long)nread, (unsigned long long)virt, (unsigned long long)phys);
            }
        }
    }
    free(ind);
    if (nread != icount || !p->virtual_size || p->indirect[0].virt != 0 || p->virtual_size < s->data_off + s->data_size) {
        r->patch = NULL; free(p->indirect); free(p->ctr); free(p);
        return WHY("the update relocation map does not cover the RomFS payload");
    }
    p->nindirect = icount;
    // (layout facts only, no key or data bytes: which table layout this update has is in its encrypted header)
    fprintf(stderr, "[nxpkg] update RomFS: %u relocation entries, %u AES-CTR-Ex entries, %s\n", icount, ccount,
            meta_layer ? "tables under a patch meta hash layer (ordinary AES-CTR)" : "relocation table in AES-CTR-Ex data");
    return true;
}

// Decrypt update patch bytes using the generation selected by the AES-CTR-Ex bucket tree.
static bool ctr_ex_read(NxRomfs *r, uint8_t *dst, uint64_t size, uint64_t off) {
    NxPatch *p = r->patch;
    if (!p || off > p->patch_data_size || size > p->patch_data_size - off) return false;
    uint64_t lo = 0, hi = p->nctr;
    while (hi - lo > 1) {
        uint64_t mid = (lo + hi) / 2;
        if (p->ctr[mid].phys <= off) lo = mid;
        else hi = mid;
    }
    uint64_t done = 0;
    while (done < size) {
        const CtrEntry *e = &p->ctr[lo];
        uint64_t end = lo + 1 < p->nctr ? p->ctr[lo + 1].phys : p->patch_data_size;
        uint64_t cur = off + done;
        if (cur < e->phys || cur >= end) return false;
        uint64_t n = end - cur < size - done ? end - cur : size - done;
        uint64_t aligned = cur & ~15ull, pre = cur - aligned;
        uint8_t *tmp = pre ? malloc((size_t)n + (size_t)pre) : dst + done;
        if (!tmp || !pread_all(r->fd, tmp, (size_t)n + (size_t)pre, r->nca_base + r->sec.off + aligned)) {
            if (pre) free(tmp);
            return false;
        }
        uint8_t iv[16];
        // NcaAesCtrUpperIv is {u32 generation; u32 secure_value} (fs header 0x140 / 0x144); the IV's upper half is that
        // u64 big-endian, so the header's secure_value (ctr_hi[0..3]) comes first, then the entry's generation
        // (fssystem_aes_ctr_counter_extended_storage.cpp, AesCtrStorage::MakeIv).
        memcpy(iv, r->sec.ctr_hi, 4);
        iv[4] = (uint8_t)(e->generation >> 24), iv[5] = (uint8_t)(e->generation >> 16);
        iv[6] = (uint8_t)(e->generation >> 8), iv[7] = (uint8_t)e->generation;
        uint64_t block = (r->sec.off + aligned) >> 4;
        for (int k = 0; k < 8; k++) iv[15 - k] = (uint8_t)(block >> (8 * k));
        if (!e->plain) nx_aes128_ctr(r->key, iv, tmp, (size_t)n + (size_t)pre);
        if (pre) memcpy(dst + done, tmp + pre, (size_t)n), free(tmp);
        done += n;
        if (cur + n == end && lo + 1 < p->nctr) lo++;
    }
    return true;
}

// the cached, decompressed block of entry i
static const uint8_t *comp_block(NxRomfs *r, uint64_t i, uint32_t *len) {
    CBlock *victim = &r->cache[0];
    for (int k = 0; k < NCACHE; k++) {
        CBlock *c = &r->cache[k];
        if (c->data && c->entry == i) { c->stamp = ++r->stamp; *len = c->len; return c->data; }
        if (!c->data || c->stamp < victim->stamp) victim = c;
    }
    const CEntry *e = &r->ent[i];
    uint32_t vlen = (uint32_t)(r->ent[i + 1].virt - e->virt);
    uint8_t *raw = malloc(e->psize ? e->psize : 1), *out = malloc(vlen ? vlen : 1);
    if (!romfs_storage_read(r, raw, e->psize, r->sec.data_off + (uint64_t)e->phys) ||
        nx_lz4_decompress(raw, (int)e->psize, out, (int)vlen) != (int)vlen) {
        free(raw), free(out);
        return NULL;
    }
    free(raw);
    free(victim->data);
    victim->data = out, victim->entry = i, victim->len = vlen, victim->stamp = ++r->stamp;
    *len = vlen;
    return out;
}

ssize_t nx_romfs_pread(NxRomfs *r, void *dst_, uint64_t size, uint64_t off) {
    uint8_t *dst = dst_;
    if (off >= r->size) return 0;
    if (size > r->size - off) size = r->size - off;
    if (!r->ent)
        return romfs_storage_read(r, dst, size, r->sec.data_off + off) ? (ssize_t)size : -1;
    pthread_mutex_lock(&r->lock);
    uint64_t lo = 0, hi = r->nent;  // last entry with virt <= off
    while (hi - lo > 1) {
        uint64_t mid = (lo + hi) / 2;
        if ((uint64_t)r->ent[mid].virt <= off) lo = mid;
        else hi = mid;
    }
    uint64_t done = 0;
    bool ok = true;
    for (uint64_t i = lo; done < size && i < r->nent && ok; i++) {
        const CEntry *e = &r->ent[i];
        uint64_t v = (uint64_t)e->virt, vend = (uint64_t)r->ent[i + 1].virt, o = off + done;
        if (o >= vend) continue;
        uint64_t n = vend - o < size - done ? vend - o : size - done;
        if (e->type == 0) ok = romfs_storage_read(r, dst + done, n, r->sec.data_off + (uint64_t)e->phys + (o - v));
        else if (e->type == 1) memset(dst + done, 0, n);
        else if (e->type == 3) {
            uint32_t len;
            const uint8_t *b = comp_block(r, i, &len);
            if (b) memcpy(dst + done, b + (o - v), n);
            else ok = false;
        } else ok = false;
        done += n;
    }
    pthread_mutex_unlock(&r->lock);
    return ok ? (ssize_t)done : -1;
}

uint64_t nx_romfs_size(const NxRomfs *r) { return r->size; }

void nx_romfs_close(NxRomfs *r) {
    if (!r) return;
    for (int k = 0; k < NCACHE; k++) free(r->cache[k].data);
    free(r->ent);
    if (r->patch) {  // (its base_fd belongs to the caller's NxProgram)
        free(r->patch->ctr);
        free(r->patch->indirect);
        free(r->patch);
    }
    if (r->fd >= 0) close(r->fd);
    pthread_mutex_destroy(&r->lock);
    free(r);
}

// ------------------------------------------------------------------ romfs.src
// One "name=value" per line: package (path), size (its size in bytes: a changed file is refused), nca (the program
// NCA's offset in it), section (offset, size, enc, ctr_hi), key, data (offset, size), comp (offset, size, header).
static void hex(char *out, const uint8_t *b, int n) {
    for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
}

NxRomfs *nx_romfs_open(const char *src_path, char *why, size_t why_size) {
    FILE *f = fopen(src_path, "r");
    if (!f) { snprintf(why, why_size, "no %s", src_path); return NULL; }
    NxRomfs *r = calloc(1, sizeof *r);
    r->fd = -1;
    pthread_mutex_init(&r->lock, NULL);
    char line[2048], package[1800] = "";
    unsigned long long pkg_size = 0, a, b;
    unsigned enc;
    char h1[64], h2[64];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "package=", 8)) snprintf(package, sizeof package, "%s", line + 8);
        else if (sscanf(line, "size=%llu", &a) == 1) pkg_size = a;
        else if (sscanf(line, "nca=%llu", &a) == 1) r->nca_base = a;
        else if (sscanf(line, "section=%llu %llu %u %63s", &a, &b, &enc, h1) == 4)
            r->sec.off = a, r->sec.size = b, r->sec.enc_type = (uint8_t)enc, unhex(h1, r->sec.ctr_hi, 8);
        else if (sscanf(line, "key=%63s", h1) == 1) unhex(h1, r->key, 16);
        else if (sscanf(line, "data=%llu %llu", &a, &b) == 2) r->sec.data_off = a, r->sec.data_size = b;
        else if (sscanf(line, "comp=%llu %llu %63s", &a, &b, h2) == 3)
            r->sec.comp_off = a, r->sec.comp_size = b, unhex(h2, r->sec.comp_hdr, 16);
    }
    fclose(f);
    struct stat sb;
    r->fd = package[0] ? open_package(package) : -1;
    if (r->fd < 0 || fstat(r->fd, &sb) || (unsigned long long)sb.st_size != pkg_size) {
        snprintf(why, why_size, "the game package moved or changed (%s)", package);
        nx_romfs_close(r);
        return NULL;
    }
    r->size = r->sec.data_size;
    if (r->sec.comp_size && !comp_load(r, why, why_size)) { nx_romfs_close(r); return NULL; }
    return r;
}

// ------------------------------------------------------------------ setup
#define PERSONALIZED_TICKET                                                                                       \
    "the package's ticket is personalized (tied to the console it was bought on), so prod.keys can't decrypt it: " \
    "dump it again with your dump tool's option to remove console-specific data (a common ticket)"

// The ticket's data after its signature block (Switchbrew "Ticket": signature type u32, signature, padding to 0x40):
// 0 = a signature type this doesn't know.
static size_t ticket_data_offset(uint32_t sig_type) {
    switch (sig_type) {
    case 0x10000: case 0x10003: return 4 + 0x200 + 0x3C;  // RSA-4096
    case 0x10001: case 0x10004: return 4 + 0x100 + 0x3C;  // RSA-2048 (what eShop tickets use: data at 0x140)
    case 0x10002: case 0x10005: return 4 + 0x3C + 0x40;   // ECDSA
    default: return 0;
    }
}

// The section key from the ticket (or title.keys). *personalized: the NSP's ticket for this rights ID is a
// personalized one (title key type 1): its key block is RSA-OAEP encrypted for the console that bought the game, not
// AES with titlekek, so it can't be read with prod.keys alone; taking its first 16 bytes gave a wrong key that showed
// up as "ExeFS doesn't decrypt (wrong keys?)".
static bool title_key(const Nca *nca, int fd, const Entries *files, const Keys *ks, const char *keys_path, int keygen,
                      uint8_t out[16], bool *personalized) {
    const uint8_t *rights = nca->hdr + 0x230;
    uint8_t enc[16];
    bool found = false;
    *personalized = false;
    for (int i = 0; i < files->n && !found; i++) {  // the NSP's ticket: <rights id>.tik
        size_t l = strlen(files->e[i].name);
        if (l < 4 || strcmp(files->e[i].name + l - 4, ".tik")) continue;
        uint8_t tik[0x400];
        uint32_t sig;
        if (files->e[i].size < 4 || !pread_all(fd, &sig, 4, files->e[i].off)) continue;
        size_t d = ticket_data_offset(sig);
        // (data: title key block +0x40, title key type +0x141, rights ID +0x160; 0x180 bytes in all)
        if (!d || files->e[i].size < d + 0x180 || !pread_all(fd, tik, d + 0x180, files->e[i].off) ||
            memcmp(tik + d + 0x160, rights, 16))
            continue;
        if (tik[d + 0x141] != 0) { *personalized = true; continue; }  // (title.keys may still have it)
        memcpy(enc, tik + d + 0x40, 16), found = true;
    }
    if (!found) {  // title.keys beside prod.keys: <rights id> = <encrypted title key>
        char tk[1200], name[33];
        snprintf(tk, sizeof tk, "%s", keys_path);
        char *sl = strrchr(tk, '/');
        snprintf(sl ? sl + 1 : tk, sizeof tk - (sl ? (size_t)(sl + 1 - tk) : 0), "title.keys");
        Keys t = {0};
        hex(name, rights, 16);
        if (keys_load(tk, &t)) {
            const uint8_t *v = key_get(&t, name, 16);
            if (v) memcpy(enc, v, 16), found = true;
        }
        free(t.k);
    }
    if (!found) return false;
    *personalized = false;
    char kn[32];
    snprintf(kn, sizeof kn, "titlekek_%02x", keygen);
    const uint8_t *kek = key_get(ks, kn, 16);
    if (!kek) return false;
    memcpy(out, enc, 16);
    nx_aes128_ecb_decrypt(kek, out, 16);
    return true;
}

typedef struct {
    int fd;
    uint64_t size, title_id;
    Entries files;
    Nca nca;
    Section exefs, romfs;
} NxProgram;

static void program_close(NxProgram *p) {
    if (p->fd >= 0) close(p->fd);
    free(p->files.e);
    memset(p, 0, sizeof *p);
    p->fd = -1;
}

// Open one package's internal program NCA, independent of its filename. This also
// handles an update NSP whose program NCA is title-key encrypted by its ticket.
static bool program_open(const char *package, const char *keys_path, const Keys *ks, NxProgram *p, char *why,
                         size_t why_size) {
    memset(p, 0, sizeof *p);
    p->fd = -1;
    p->fd = open_package(package);
    struct stat sb;
    if (p->fd < 0 || fstat(p->fd, &sb)) return WHY("can't open package %s", package);
    p->size = (uint64_t)sb.st_size;
    uint8_t head[0x200];
    if (!pread_all(p->fd, head, sizeof head, 0)) return WHY("package is too small");
    if (!memcmp(head, "PFS0", 4)) {
        if (!read_fs0(p->fd, 0, &p->files)) return WHY("bad NSP package");
    } else if (!memcmp(head + 0x100, "HEAD", 4)) {
        uint64_t root;
        memcpy(&root, head + 0x130, 8);
        Entries parts = {0};
        if (!read_fs0(p->fd, root, &parts)) { free(parts.e); return WHY("bad XCI package"); }
        for (int i = 0; i < parts.n; i++)
            if (!strcmp(parts.e[i].name, "secure")) read_fs0(p->fd, parts.e[i].off, &p->files);
        free(parts.e);
        if (!p->files.n) return WHY("XCI has no secure partition");
    } else {
        entries_add(&p->files, "program.nca", 0, p->size);
    }
    for (int i = 0; i < p->files.n; i++)
        if (p->files.e[i].off > p->size || p->files.e[i].size > p->size - p->files.e[i].off)
            return WHY("package is truncated");
    bool found = false, any_nca = false;
    for (int i = 0; i < p->files.n && !found; i++) {
        size_t len = strlen(p->files.e[i].name);
        if (len < 4 || strcmp(p->files.e[i].name + len - 4, ".nca")) continue;
        Nca nca;
        if (!nca_header(p->fd, p->files.e[i].off, ks, &nca)) continue;
        any_nca = true;
        if (nca.hdr[0x205] == 0) p->nca = nca, found = true;
    }
    if (!found) return WHY(any_nca ? "package has no program NCA" : "can't decrypt package; check prod.keys");
    memcpy(&p->title_id, p->nca.hdr + 0x210, 8);
    int gen = p->nca.hdr[0x206] > p->nca.hdr[0x220] ? p->nca.hdr[0x206] : p->nca.hdr[0x220];
    int keygen = gen ? gen - 1 : 0;
    if (memcmp(p->nca.hdr + 0x230, (const uint8_t[16]){0}, 16)) {
        bool personalized;
        if (!title_key(&p->nca, p->fd, &p->files, ks, keys_path, keygen, p->nca.ctr_key, &personalized))
            return WHY(personalized ? PERSONALIZED_TICKET : "package needs a title key not present in its ticket or title.keys");
    } else {
        char kn[48];
        snprintf(kn, sizeof kn, "key_area_key_application_%02x", keygen);
        const uint8_t *kaek = key_get(ks, kn, 16);
        if (!kaek) return WHY("prod.keys has no %s", kn);
        uint8_t area[0x40];
        memcpy(area, p->nca.hdr + 0x300, sizeof area);
        nx_aes128_ecb_decrypt(kaek, area, sizeof area);
        memcpy(p->nca.ctr_key, area + 0x20, 16);
    }
    if (!section_info(&p->nca, 0, &p->exefs) || !section_info(&p->nca, 1, &p->romfs) || p->exefs.fs_type != 1)
        return WHY("program NCA has an unexpected ExeFS/RomFS layout");
    return true;
}

static bool program_is_patch(const NxProgram *p);

// A package's program by content: title ID, whether it is a patch (BKTR RomFS), and the build ID of its ExeFS main
// (the NSO header's module ID at 0x40). Reads the NCA header, the ExeFS directory and 0x60 bytes of main; nothing is
// written. Used to tell base from update and v1.0.0 from v1.1.0 regardless of file names.
bool nx_identify(const char *package, const char *keys_path, NxPackageInfo *info, char *why, size_t why_size) {
    memset(info, 0, sizeof *info);
    Keys ks = {0};
    if (!keys_load(keys_path, &ks)) return WHY("can't read the keys file");
    NxProgram p = {.fd = -1};
    bool ok = false;
    uint8_t *meta = NULL;
    if (!program_open(package, keys_path, &ks, &p, why, why_size)) goto out;
    info->title_id = p.title_id;
    info->is_patch = program_is_patch(&p);
    uint8_t h[16];
    if (!section_read(p.fd, p.nca.base, &p.exefs, p.nca.ctr_key, h, p.exefs.data_off, sizeof h) || memcmp(h, "PFS0", 4)) {
        WHY("the package's ExeFS doesn't decrypt (wrong keys?)");
        goto out;
    }
    uint32_t count, strsz;
    memcpy(&count, h + 4, 4), memcpy(&strsz, h + 8, 4);
    size_t meta_size = 16u + (size_t)count * 0x18u + strsz;
    if (!count || count > 4096 || strsz > (1u << 20) || meta_size > p.exefs.data_size) {
        WHY("the package's ExeFS directory is invalid");
        goto out;
    }
    meta = malloc(meta_size);
    if (!meta || !section_read(p.fd, p.nca.base, &p.exefs, p.nca.ctr_key, meta, p.exefs.data_off, meta_size)) {
        WHY("can't read the package's ExeFS directory");
        goto out;
    }
    const char *strs = (const char *)meta + 16 + count * 0x18;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t off, size;
        uint32_t no;
        memcpy(&off, meta + 16 + i * 0x18, 8), memcpy(&size, meta + 24 + i * 0x18, 8), memcpy(&no, meta + 32 + i * 0x18, 4);
        if (no >= strsz || strncmp(strs + no, "main", strsz - no) || size < 0x60) continue;
        uint64_t room = p.exefs.data_size - meta_size;
        if (off > room || 0x60 > room - off) break;
        uint8_t nso[0x60];
        if (!section_read(p.fd, p.nca.base, &p.exefs, p.nca.ctr_key, nso, p.exefs.data_off + meta_size + off, sizeof nso) ||
            memcmp(nso, "NSO0", 4))
            break;
        memcpy(info->main_build_id, nso + 0x40, sizeof info->main_build_id);
        info->has_main = true;
        break;
    }
    if (!info->has_main) { WHY("the package's ExeFS has no readable main module"); goto out; }
    ok = true;
out:
    free(meta);
    program_close(&p);
    free(ks.k);
    return ok;
}

static bool write_file(const char *path, int fd, uint64_t nca_base, const Section *s, const uint8_t key[16], uint64_t off,
                       uint64_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint8_t *buf = malloc(4 << 20);
    bool ok = true;
    for (uint64_t done = 0; done < size && ok;) {
        size_t n = size - done < (4u << 20) ? (size_t)(size - done) : (4u << 20);
        ok = section_read(fd, nca_base, s, key, buf, off + done, n) && fwrite(buf, 1, n, f) == n;
        done += n;
    }
    free(buf);
    return fclose(f) == 0 && ok;
}

// Materialize the decrypted RomFS beside ExeFS. The game reads this image directly, so neither the package nor the
// temporary romfs.src (which contains a derived title key) is needed after setup.
static bool write_romfs_image(NxRomfs *r, const char *path, const char *src, NxProgress progress, char *why,
                              size_t why_size) {
    char tmp[1240];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return WHY("can't create the extracted game data (%s)", strerror(errno));
    const uint64_t total = nx_romfs_size(r);
    uint8_t *buf = malloc(4u << 20);
    if (!buf) { fclose(f); remove(tmp); return WHY("not enough memory to extract the game data"); }
    if (progress) progress(0, total);
    bool ok = true;
    for (uint64_t done = 0; done < total;) {
        size_t n = total - done < (4u << 20) ? (size_t)(total - done) : (4u << 20);
        ssize_t got = nx_romfs_pread(r, buf, n, done);
        if (got != (ssize_t)n || fwrite(buf, 1, n, f) != n) {
            snprintf(why, why_size, "couldn't extract the game data at %llu MB (disk full or damaged package)",
                     (unsigned long long)(done >> 20));
            ok = false;
            break;
        }
        done += n;
        if (progress) progress(done, total);
    }
    free(buf);
    if (fclose(f) != 0 && ok) {
        snprintf(why, why_size, "couldn't finish writing the extracted game data (%s)", strerror(errno));
        ok = false;
    }
    if (ok && rename(tmp, path) != 0) {
        snprintf(why, why_size, "couldn't save the extracted game data (%s)", strerror(errno));
        ok = false;
    }
    if (!ok) remove(tmp);
    else if (remove(src) != 0 && errno != ENOENT) {
        snprintf(why, why_size, "couldn't remove the temporary package and key reference (%s)", strerror(errno));
        remove(path);
        ok = false;
    }
    return ok;
}

static bool extract_exefs(const NxProgram *program, const char *out_dir, char *why, size_t why_size) {
    uint8_t h[16];
    if (!section_read(program->fd, program->nca.base, &program->exefs, program->nca.ctr_key, h,
                      program->exefs.data_off, sizeof h) || memcmp(h, "PFS0", 4))
        return WHY("the update's ExeFS doesn't decrypt as PFS0");
    uint32_t count, strsz;
    memcpy(&count, h + 4, 4), memcpy(&strsz, h + 8, 4);
    if (!count || count > 4096 || strsz > (1u << 20)) return WHY("the update's ExeFS directory is invalid");
    size_t meta_size = 16u + (size_t)count * 0x18u + strsz;
    if (meta_size > program->exefs.data_size) return WHY("the update's ExeFS directory is invalid");
    uint8_t *meta = malloc(meta_size);
    if (!meta) return WHY("not enough memory to read the update's ExeFS");
    if (!section_read(program->fd, program->nca.base, &program->exefs, program->nca.ctr_key, meta,
                      program->exefs.data_off, meta_size)) {
        free(meta);
        return WHY("can't read the update's ExeFS directory");
    }
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/exefs", out_dir);
    mkdir(out_dir, 0755), mkdir(dir, 0755);
    const char *strs = (const char *)meta + 16 + count * 0x18;
    bool ok = true;
    for (uint32_t i = 0; i < count && ok; i++) {
        uint64_t off, size;
        uint32_t no;
        memcpy(&off, meta + 16 + i * 0x18, 8), memcpy(&size, meta + 24 + i * 0x18, 8);
        memcpy(&no, meta + 32 + i * 0x18, 4);
        if (no >= strsz) { ok = false; break; }
        size_t name_len = strnlen(strs + no, strsz - no);
        if (!name_len || name_len == strsz - no || name_len >= 128 || memchr(strs + no, '/', name_len) ||
            memchr(strs + no, '\\', name_len) || strs[no] == '.') { ok = false; break; }
        char path[1400];
        snprintf(path, sizeof path, "%s/%.*s", dir, (int)name_len, strs + no);
        // (bounds within the ExeFS payload, checked without overflow)
        uint64_t room = program->exefs.data_size - meta_size;
        uint64_t file_off = program->exefs.data_off + meta_size + off;
        if (off > room || size > room - off)
            ok = false;
        else
            ok = write_file(path, program->fd, program->nca.base, &program->exefs, program->nca.ctr_key, file_off, size);
    }
    free(meta);
    if (!ok) return WHY("can't write the game code files (bad ExeFS or disk full)");
    return true;
}

static NxRomfs *romfs_update_open(const NxProgram *base, const NxProgram *update, char *why, size_t why_size) {
    NxRomfs *r = calloc(1, sizeof *r);
    if (!r) { snprintf(why, why_size, "not enough memory for the update RomFS"); return NULL; }
    r->fd = -1;
    r->fd = dup(update->fd);
    r->nca_base = update->nca.base;
    r->sec = update->romfs;
    memcpy(r->key, update->nca.ctr_key, 16);
    pthread_mutex_init(&r->lock, NULL);
    if (r->fd < 0) { snprintf(why, why_size, "can't reopen the update package"); nx_romfs_close(r); return NULL; }
    if (!patch_load(r, base->fd, base->nca.base, &base->romfs, base->nca.ctr_key, why, why_size)) {
        nx_romfs_close(r);
        return NULL;
    }
    r->size = r->sec.data_size;
    if (r->sec.comp_size && !comp_load(r, why, why_size)) { nx_romfs_close(r); return NULL; }
    return r;
}

// A patch program NCA: its RomFS section has an IndirectStorage (BKTR) relocation table. (patch_load then refuses the
// encryption types it doesn't read, AES-CTR-Ex with a skipped or extra hash layer, instead of a silent base-only setup.)
static bool program_is_patch(const NxProgram *p) { return !memcmp(p->romfs.indirect_hdr, "BKTR", 4); }

// Which of two candidate packages is the base and which its update, from what is inside them
typedef enum { ROLES_A_BASE, ROLES_B_BASE, ROLES_BASE_ONLY, ROLES_BAD } UpdateRoles;
static UpdateRoles update_roles(bool a_patch, bool b_ok, bool b_patch, uint64_t a_tid, uint64_t b_tid) {
    if (!b_ok) return a_patch ? ROLES_BAD : ROLES_BASE_ONLY;  // (an update can't be set up without its base)
    if (a_patch && b_patch) return ROLES_BAD;
    if (!a_patch && !b_patch) return ROLES_BASE_ONLY;          // a DLC-less second game, another base: a alone
    if (a_tid != b_tid) return ROLES_BAD;                      // an update of another program
    return a_patch ? ROLES_B_BASE : ROLES_A_BASE;
}

bool nx_setup_update(const char *package_a, const char *package_b, const char *keys_path, const char *out_dir,
                     uint64_t *title_id, bool *used_update, NxProgress progress, char *why, size_t why_size) {
    if (used_update) *used_update = false;
    Keys ks = {0};
    if (!keys_load(keys_path, &ks)) return WHY("can't read the keys file");
    bool ok = false;
    NxProgram base = {.fd = -1}, update = {.fd = -1};
    NxRomfs *romfs = NULL;
    if (!program_open(package_a, keys_path, &ks, &base, why, why_size)) goto out;
    // The second package is only a candidate (the builder picks it by size): a DLC, another game or a damaged file
    // there must not stop a base-only setup, so anything but an update falls back to nx_setup.
    char why_b[256] = "";
    bool b_ok = program_open(package_b, keys_path, &ks, &update, why_b, sizeof why_b);
    switch (update_roles(program_is_patch(&base), b_ok, b_ok && program_is_patch(&update), base.title_id,
                         update.title_id)) {
    case ROLES_A_BASE: break;
    case ROLES_B_BASE: { NxProgram t = base; base = update, update = t; } break;
    case ROLES_BASE_ONLY:
        fprintf(stderr, "[nxpkg] %s is not an update (%s): setting up the base package alone\n", package_b,
                b_ok ? "no relocation table in its RomFS" : why_b);
        program_close(&base), program_close(&update);
        free(ks.k);
        return nx_setup(package_a, keys_path, out_dir, title_id, progress, why, why_size);
    case ROLES_BAD:
        WHY("the packages aren't a base program and its update (an update alone, two updates, or another program's)");
        goto out;
    }
    if (base.title_id != 0x01008CF01BAAC000ull) {
        WHY("the base and update packages aren't Echoes of Wisdom program NCAs");
        goto out;
    }
    if (title_id) *title_id = update.title_id;
    // A patch program's ExeFS replaces the base's whole (as Horizon loads it): no base module may be left behind
    // beside the update's, so only the update's ExeFS is written.
    if (!extract_exefs(&update, out_dir, why, why_size)) goto out;
    romfs = romfs_update_open(&base, &update, why, why_size);
    if (!romfs) goto out;
    uint64_t magic = 0;
    if (nx_romfs_pread(romfs, &magic, sizeof magic, 0) != sizeof magic || magic != 0x50) {
        WHY("the merged update RomFS doesn't have a valid RomFS header");
        goto out;
    }
    char out_image[1200], staging_image[1240], staging_src[1240];
    snprintf(out_image, sizeof out_image, "%s/romfs.bin", out_dir);
    snprintf(staging_image, sizeof staging_image, "%s/romfs.update.bin", out_dir);
    snprintf(staging_src, sizeof staging_src, "%s/romfs.update.src", out_dir);
    remove(staging_src);
    if (!write_romfs_image(romfs, staging_image, staging_src, progress, why, why_size)) goto out;
    // Publish only after the full merged image has been written successfully.
    char backup[1240];
    snprintf(backup, sizeof backup, "%s/romfs.previous.bin", out_dir);
    remove(backup);
    bool had_old = rename(out_image, backup) == 0;
    if (rename(staging_image, out_image) != 0) {
        if (had_old) rename(backup, out_image);
        remove(staging_image);
        WHY("can't install the merged update RomFS (%s)", strerror(errno));
        goto out;
    }
    if (had_old) remove(backup);
    // (an earlier base-only setup's romfs.src holds a derived title key and would now describe the wrong data)
    char old_src[1240];
    snprintf(old_src, sizeof old_src, "%s/romfs.src", out_dir);
    remove(old_src);
    if (used_update) *used_update = true;
    ok = true;
out:
    if (romfs) nx_romfs_close(romfs);
    program_close(&base), program_close(&update);
    free(ks.k);
    return ok;
}

bool nx_romfs_extract(const char *src_path, const char *image_path, NxProgress progress, char *why, size_t why_size) {
    NxRomfs *r = nx_romfs_open(src_path, why, why_size);
    if (!r) return false;
    uint64_t header = 0;
    bool ok = nx_romfs_pread(r, &header, sizeof header, 0) == sizeof header && header == 0x50;
    if (ok) ok = write_romfs_image(r, image_path, src_path, progress, why, why_size);
    else WHY("the game data doesn't decrypt (wrong keys?)");
    nx_romfs_close(r);
    return ok;
}

bool nx_setup(const char *package, const char *keys_path, const char *out_dir, uint64_t *title_id, NxProgress progress,
              char *why, size_t why_size) {
    Keys ks = {0};
    if (!keys_load(keys_path, &ks)) return WHY("can't read the keys file");
    bool ok = false;
    Entries files = {0};
    int fd = open_package(package);
    struct stat sb;
    if (fd < 0 || fstat(fd, &sb)) { WHY("can't open the game package"); goto out; }
    uint8_t head[0x200];
    if (!pread_all(fd, head, sizeof head, 0)) { WHY("the game package is too small"); goto out; }
    if (!memcmp(head, "PFS0", 4)) {  // NSP
        if (!read_fs0(fd, 0, &files)) { WHY("bad NSP"); goto out; }
    } else if (!memcmp(head + 0x100, "HEAD", 4)) {  // XCI: the root HFS0's "secure" partition
        uint64_t root;
        memcpy(&root, head + 0x130, 8);
        Entries parts = {0};
        if (!read_fs0(fd, root, &parts)) { WHY("bad XCI"); goto out; }
        for (int i = 0; i < parts.n; i++)
            if (!strcmp(parts.e[i].name, "secure")) read_fs0(fd, parts.e[i].off, &files);
        free(parts.e);
        if (!files.n) { WHY("XCI without a secure partition"); goto out; }
    } else {
        entries_add(&files, "program.nca", 0, (uint64_t)sb.st_size);  // a bare NCA
    }
    for (int i = 0; i < files.n; i++)
        if (files.e[i].off + files.e[i].size > (uint64_t)sb.st_size) {
            WHY("the game package is incomplete (%llu MB missing): copy it again",
                (unsigned long long)((files.e[i].off + files.e[i].size - (uint64_t)sb.st_size) >> 20));
            goto out;
        }
    Nca nca;
    bool found = false;
    bool any_nca = false;
    for (int i = 0; i < files.n && !found; i++) {
        size_t l = strlen(files.e[i].name);
        if (l < 4 || strcmp(files.e[i].name + l - 4, ".nca")) continue;
        if (!nca_header(fd, files.e[i].off, &ks, &nca)) continue;
        any_nca = true;
        found = nca.hdr[0x205] == 0;  // content type: program
    }
    if (!found) { WHY(any_nca ? "no game program in the package" : "can't decrypt the package (wrong or old keys?)"); goto out; }
    uint64_t tid;
    memcpy(&tid, nca.hdr + 0x210, 8);
    if (title_id) *title_id = tid;
    int gen = nca.hdr[0x206] > nca.hdr[0x220] ? nca.hdr[0x206] : nca.hdr[0x220];
    int keygen = gen ? gen - 1 : 0;
    static const uint8_t zero[16];
    if (memcmp(nca.hdr + 0x230, zero, 16)) {
        bool personalized;
        if (!title_key(&nca, fd, &files, &ks, keys_path, keygen, nca.ctr_key, &personalized)) {
            WHY(personalized ? PERSONALIZED_TICKET : "the package needs a title key (no ticket in it, none in title.keys)");
            goto out;
        }
    } else {
        char kn[48];
        snprintf(kn, sizeof kn, "key_area_key_application_%02x", keygen);
        const uint8_t *kaek = key_get(&ks, kn, 16);
        if (!kaek) { WHY("the keys file has no %s (keys from an older system?)", kn); goto out; }
        uint8_t area[0x40];
        memcpy(area, nca.hdr + 0x300, 0x40);
        nx_aes128_ecb_decrypt(kaek, area, 0x40);
        memcpy(nca.ctr_key, area + 0x20, 16);  // index 2: AES-CTR
    }
    Section exefs, romfs;
    if (!section_info(&nca, 0, &exefs) || !section_info(&nca, 1, &romfs) || exefs.fs_type != 1) {
        WHY("unexpected program layout");
        goto out;
    }
    // ExeFS: a PFS0 of the NSOs and main.npdm
    {
        uint8_t h[16];
        if (!section_read(fd, nca.base, &exefs, nca.ctr_key, h, exefs.data_off, 16) || memcmp(h, "PFS0", 4)) {
            WHY("can't decrypt the game (wrong keys?)");
            goto out;
        }
        uint32_t n, strsz;
        memcpy(&n, h + 4, 4), memcpy(&strsz, h + 8, 4);
        size_t meta = 16 + n * 0x18 + strsz;
        uint8_t *m = malloc(meta);
        section_read(fd, nca.base, &exefs, nca.ctr_key, m, exefs.data_off, meta);
        char dir[1200];
        snprintf(dir, sizeof dir, "%s/exefs", out_dir);
        mkdir(out_dir, 0755), mkdir(dir, 0755);
        bool wrote = true;
        for (uint32_t i = 0; i < n && wrote; i++) {
            uint64_t off, size;
            uint32_t no;
            memcpy(&off, m + 16 + i * 0x18, 8), memcpy(&size, m + 24 + i * 0x18, 8), memcpy(&no, m + 32 + i * 0x18, 4);
            const char *name = (const char *)m + 16 + n * 0x18 + no;
            if (no >= strsz || strchr(name, '/') || name[0] == '.') continue;
            char path[1400];
            snprintf(path, sizeof path, "%s/%.*s", dir, (int)strnlen(name, strsz - no), name);
            wrote = write_file(path, fd, nca.base, &exefs, nca.ctr_key, exefs.data_off + meta + off, size);
        }
        free(m);
        if (!wrote) { WHY("can't write the game code files (disk full?)"); goto out; }
    }
    // RomFS: validate its source, then extract a local image that the game can use without the package.
    {
        char path[1200], tmp[1300], image[1200], ctr[17], key[33], comp[33];
        snprintf(path, sizeof path, "%s/romfs.src", out_dir);
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        snprintf(image, sizeof image, "%s/romfs.bin", out_dir);
        hex(ctr, romfs.ctr_hi, 8), hex(key, nca.ctr_key, 16), hex(comp, romfs.comp_hdr, 16);
        FILE *f = fopen(tmp, "w");
        if (!f) { WHY("can't write %s", path); goto out; }
        fprintf(f, "package=%s\nsize=%llu\nnca=%llu\nsection=%llu %llu %u %s\nkey=%s\ndata=%llu %llu\ncomp=%llu %llu %s\n",
                package, (unsigned long long)sb.st_size, (unsigned long long)nca.base, (unsigned long long)romfs.off,
                (unsigned long long)romfs.size, romfs.enc_type, ctr, key, (unsigned long long)romfs.data_off,
                (unsigned long long)romfs.data_size, (unsigned long long)romfs.comp_off, (unsigned long long)romfs.comp_size,
                comp);
        if (fclose(f) || rename(tmp, path)) { WHY("can't write %s", path); goto out; }
        ok = nx_romfs_extract(path, image, progress, why, why_size);
        if (!ok) goto out;
    }
out:
    if (fd >= 0) close(fd);
    free(files.e);
    free(ks.k);
    return ok;
}
