// Tests for runtime/nxpkg.c: AES (FIPS-197, SP 800-38A CTR, IEEE 1619 XTS vectors), LZ4 (blocks made by the lz4
// Python module), and with a package: the program NCA's header, keys and first RomFS bytes (compare with
// tools/nca_extract.py). Build and run: tools/test_nxpkg.sh [prod.keys package]
#include "../runtime/nxpkg.c"

static int fails;
static void unhex_s(const char *s, uint8_t *o) { unhex(s, o, 512); }
static void check(const char *what, const uint8_t *got, const char *want_hex) {
    uint8_t w[256];
    size_t n = strlen(want_hex) / 2;
    unhex_s(want_hex, w);
    bool ok = !memcmp(got, w, n);
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fails += !ok;
}

#ifdef _WIN32
// (the runtime's pread lives in runtime/platform/windows.c, which this test doesn't link)
#include <io.h>
#include <windows.h>
ssize_t pread(int fd, void *buf, size_t n, off_t off) {
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)((uint64_t)off & 0xffffffffu), ov.OffsetHigh = (DWORD)((uint64_t)off >> 32);
    DWORD got = 0;
    if (!ReadFile((HANDLE)_get_osfhandle(fd), buf, (DWORD)n, &got, &ov)) return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    return (ssize_t)got;
}
#endif
#ifndef O_BINARY
#define O_BINARY 0
#endif

static void ok_if(const char *what, bool ok) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fails += !ok;
}

// Update RomFS reads (BKTR) on made-up data: a plain base section and an AES-CTR-Ex patch section in one file, an
// indirect map that switches between them mid-read, two counter generations, and the bucket-tree node walk. This
// checks the routing and offsets; the counter layout itself is only shown right by a real update's tables.
static void test_bktr(void) {
    const char *path = "build/test_nxpkg/bktr.bin";
    enum { BASE_OFF = 0, BASE_SIZE = 0x3000, UPD_OFF = 0x4000, PATCH_SIZE = 0x2000, VSIZE = 0x2400 };
    static uint8_t file[UPD_OFF + PATCH_SIZE], plain[PATCH_SIZE], want[VSIZE], got[VSIZE];
    srand(7);
    for (size_t i = 0; i < sizeof file; i++) file[i] = (uint8_t)rand();
    for (size_t i = 0; i < sizeof plain; i++) plain[i] = (uint8_t)rand();

    NxRomfs *r = calloc(1, sizeof *r);
    pthread_mutex_init(&r->lock, NULL);
    r->sec.off = UPD_OFF, r->sec.size = PATCH_SIZE, r->sec.enc_type = 4;
    for (int k = 0; k < 8; k++) r->sec.ctr_hi[k] = (uint8_t)(0xa0 + k);
    for (int k = 0; k < 16; k++) r->key[k] = (uint8_t)(0x10 + k);
    // (the third range is stored unencrypted: encryption_value 1)
    static CtrEntry ctr[3] = {{0, 5, 0}, {0x1000, 9, 0}, {0x1800, 0, 1}};
    static IndirectEntry ind[4] = {{0, 0x100, 0}, {0x800, 0x10, 1}, {0x1800, 0x2000, 0}, {0x1c00, 0x1800, 1}};
    NxPatch p = {.base_nca = 0, .ctr = ctr, .nctr = 3, .patch_data_size = PATCH_SIZE, .indirect_data_size = PATCH_SIZE,
                 .indirect = ind, .nindirect = 4, .virtual_size = VSIZE};
    p.base_sec.off = BASE_OFF, p.base_sec.size = BASE_SIZE, p.base_sec.enc_type = 1;  // plain: no key needed
    // encrypt the patch bytes as Atmosphere's AesCtrCounterExtendedStorage reads them: IV = {secure_value (the
    // header's upper word, ctr_hi[0..3]), entry generation} big-endian, then the absolute AES block number
    memcpy(file + UPD_OFF, plain, PATCH_SIZE);
    for (int e = 0; e < 3; e++) {
        uint64_t a = ctr[e].phys, b = e + 1 < 3 ? ctr[e + 1].phys : PATCH_SIZE;
        if (ctr[e].plain) continue;
        uint8_t iv[16];
        memcpy(iv, r->sec.ctr_hi, 4);
        iv[4] = (uint8_t)(ctr[e].generation >> 24), iv[5] = (uint8_t)(ctr[e].generation >> 16);
        iv[6] = (uint8_t)(ctr[e].generation >> 8), iv[7] = (uint8_t)ctr[e].generation;
        uint64_t block = (UPD_OFF + a) >> 4;
        for (int k = 0; k < 8; k++) iv[15 - k] = (uint8_t)(block >> (8 * k));
        nx_aes128_ctr(r->key, iv, file + UPD_OFF + a, (size_t)(b - a));
    }
    for (int i = 0; i < 4; i++) {
        uint64_t v = ind[i].virt, end = i + 1 < 4 ? ind[i + 1].virt : VSIZE;
        memcpy(want + v, ind[i].storage ? plain + ind[i].phys : file + BASE_OFF + ind[i].phys, (size_t)(end - v));
    }
    FILE *f = fopen(path, "wb");
    if (!f) { ok_if("bktr: write the scratch file (run from the checkout, build/test_nxpkg existing)", false); return; }
    fwrite(file, 1, sizeof file, f);
    fclose(f);
    r->fd = open(path, O_RDONLY | O_BINARY);
    p.base_fd = r->fd;
    r->patch = &p;

    bool all = romfs_storage_read(r, got, VSIZE, 0) && !memcmp(got, want, VSIZE);
    ok_if("bktr: whole virtual image across base / patch / two generations / an unencrypted range", all);
    bool parts = true;  // unaligned reads that start and end inside entries and AES blocks
    static const uint32_t cuts[][2] = {{0x7f3, 0x21}, {0xffd, 0x9}, {0x17f9, 0x413}, {1, VSIZE - 2}, {0x1c00, 0x400}};
    for (size_t i = 0; i < sizeof cuts / sizeof *cuts; i++) {
        memset(got, 0, VSIZE);
        parts &= romfs_storage_read(r, got, cuts[i][1], cuts[i][0]) && !memcmp(got, want + cuts[i][0], cuts[i][1]);
    }
    ok_if("bktr: unaligned reads over entry and block boundaries", parts);
    ok_if("bktr: a read past the virtual end is refused", !romfs_storage_read(r, got, 0x10, VSIZE - 8));
    p.indirect_data_size = 0x1c00;  // the last entry's patch bytes would now overlap the relocation table
    ok_if("bktr: patch data at or past the relocation table is refused", !romfs_storage_read(r, got, 0x10, 0x23f0));
    p.indirect_data_size = PATCH_SIZE;

    // base / update roles from the packages' contents (a, b: patch?, b readable?, title ids)
    const uint64_t T = 0x01008CF01BAAC000ull;
    ok_if("roles: base then update", update_roles(false, true, true, T, T) == ROLES_A_BASE);
    ok_if("roles: update then base (either order)", update_roles(true, true, false, T, T) == ROLES_B_BASE);
    ok_if("roles: a second package that isn't an update -> base alone", update_roles(false, true, false, T, 1) ==
                                                                          ROLES_BASE_ONLY);
    ok_if("roles: an unreadable second package -> base alone", update_roles(false, false, false, T, 0) == ROLES_BASE_ONLY);
    ok_if("roles: another program's update is refused", update_roles(false, true, true, T, T + 0x1000) == ROLES_BAD);
    ok_if("roles: two updates are refused", update_roles(true, true, true, T, T) == ROLES_BAD);
    ok_if("roles: an update without a readable base is refused", update_roles(true, false, false, T, 0) == ROLES_BAD);

    // bucket tree: one L1 node, one entry set {index 0, count 3, end VSIZE}
    static uint8_t table[2 * BKTR_NODE_SIZE];
    int32_t idx = 0, cnt = 3;
    uint64_t end = VSIZE, out_end = 0;
    memcpy(table + BKTR_NODE_SIZE, &idx, 4), memcpy(table + BKTR_NODE_SIZE + 4, &cnt, 4);
    memcpy(table + BKTR_NODE_SIZE + 8, &end, 8);
    uint8_t *sets = NULL;
    uint32_t nsets = 0;
    ok_if("bktr: node walk finds the entry set and the end offset",
          bktr_nodes(table, sizeof table, 3, 20, &sets, &nsets, &out_end) && sets == table + BKTR_NODE_SIZE &&
              nsets == 1 && out_end == VSIZE);
    ok_if("bktr: a table too short for its entry sets is refused",
          !bktr_nodes(table, BKTR_NODE_SIZE, 3, 20, &sets, &nsets, &out_end));

    // without a patch: offsets are section offsets (data_off added once, by the caller)
    r->patch = NULL;
    r->sec.off = BASE_OFF, r->sec.size = BASE_SIZE, r->sec.enc_type = 1, r->sec.data_off = 0x40;
    r->sec.data_size = r->size = 0x800;
    memset(got, 0, VSIZE);
    ok_if("romfs: an unpatched section reads from data_off",
          nx_romfs_pread(r, got, 0x100, 0x10) == 0x100 && !memcmp(got, file + BASE_OFF + 0x50, 0x100));
    nx_romfs_close(r);
    remove(path);
}

// patch_load on a made-up update section laid out like a real one, described by a real fs header parsed by
// section_info: patch data, the relocation (indirect) table, the CTR-Ex table, and (meta_layer) the patch meta hash
// data. Every IV here is built from the raw header bytes as the reference defines it, not from Section.ctr_hi:
// NcaAesCtrUpperIv {u32 generation @0x140; u32 secure_value @0x144} stored big-endian as one u64, then the AES block
// number (yuzu src/core/file_sys/fssystem/fssystem_nca_header.h NcaAesCtrUpperIv, fssystem_aes_ctr_storage.cpp
// AesCtrStorage::MakeIv). CTR-Ex data replaces generation with the entry's (fssystem_aes_ctr_counter_extended_
// storage.cpp); the CTR-Ex table, and with a meta hash layer the relocation table too, use the header's whole upper IV
// (fssystem_nca_file_system_driver.cpp CreateAesCtrExStorageMetaStorage / CreatePatchMetaStorage).
static void ref_iv(uint8_t iv[16], const uint8_t *fh, const uint32_t *entry_generation, uint64_t abs_off) {
    uint32_t generation, secure_value;
    memcpy(&generation, fh + 0x140, 4), memcpy(&secure_value, fh + 0x144, 4);
    if (entry_generation) generation = *entry_generation;
    uint64_t upper = (uint64_t)secure_value << 32 | generation, block = abs_off >> 4;  // NcaAesCtrUpperIv.value
    for (int k = 0; k < 8; k++) iv[k] = (uint8_t)(upper >> (56 - 8 * k)), iv[8 + k] = (uint8_t)(block >> (56 - 8 * k));
}

static void test_bktr_tables(bool meta_layer) {
    const char *path = "build/test_nxpkg/bktr_tables.bin";
    enum { BASE_SIZE = 0x3000, UPD_OFF = 0x4000, PATCH = 0x2000, IND_OFF = PATCH, IND_SIZE = 2 * BKTR_NODE_SIZE,
           CTR_OFF = IND_OFF + IND_SIZE, CTR_SIZE = 2 * BKTR_NODE_SIZE, MH_OFF = CTR_OFF + CTR_SIZE, MH_SIZE = 0x200,
           SEC = MH_OFF + MH_SIZE, VSIZE = 0x2400 };
    static uint8_t file[UPD_OFF + SEC], sec_plain[SEC], want[VSIZE], got[VSIZE];
    const char *tag = meta_layer ? "bktr tables (patch meta hash layer)" : "bktr tables";
    char what[160];
    srand(meta_layer ? 13 : 11);
    for (size_t i = 0; i < sizeof file; i++) file[i] = (uint8_t)rand();
    memset(sec_plain, 0, sizeof sec_plain);
    for (size_t i = 0; i < PATCH; i++) sec_plain[i] = (uint8_t)rand();
    // relocation table: {virt, phys, source}, 20 bytes each, in the second node
    static const struct { uint64_t v, p; uint32_t s; } ind[4] = {{0, 0x100, 0}, {0x800, 0x10, 1}, {0x1800, 0x2000, 0},
                                                                 {0x1c00, 0x1800, 1}};
    uint8_t *node = sec_plain + IND_OFF + BKTR_NODE_SIZE;
    int32_t zero = 0, four = 4;
    uint64_t vend = VSIZE;
    memcpy(node, &zero, 4), memcpy(node + 4, &four, 4), memcpy(node + 8, &vend, 8);
    for (int i = 0; i < 4; i++) {
        memcpy(node + 0x10 + 20 * i, &ind[i].v, 8), memcpy(node + 0x18 + 20 * i, &ind[i].p, 8);
        memcpy(node + 0x20 + 20 * i, &ind[i].s, 4);
    }
    // CTR-Ex table: {offset, encryption_value, generation}, 16 bytes each; the last range covers the relocation table
    // (only read as CTR-Ex data without a meta hash layer)
    static const struct { uint64_t off; uint8_t plain; uint32_t gen; } ce[4] = {{0, 0, 5}, {0x1000, 0, 9},
                                                                                {0x1800, 1, 0}, {0x2000, 0, 3}};
    node = sec_plain + CTR_OFF + BKTR_NODE_SIZE;
    uint64_t cend = CTR_OFF;
    memcpy(node, &zero, 4), memcpy(node + 4, &four, 4), memcpy(node + 8, &cend, 8);
    for (int i = 0; i < 4; i++) {
        memcpy(node + 0x10 + 16 * i, &ce[i].off, 8), node[0x18 + 16 * i] = ce[i].plain;
        memcpy(node + 0x1c + 16 * i, &ce[i].gen, 4);
    }

    // the NCA header: section 1 at UPD_OFF; its fs header with distinct generation / secure_value words
    static Nca nca;
    memset(&nca, 0, sizeof nca);
    uint32_t st = UPD_OFF / 0x200, en = (UPD_OFF + SEC) / 0x200, layers = 2, gen_word = 0x11223344,
             secure_word = 0xaabbccdd, ver = 1;
    memcpy(nca.hdr + 0x250, &st, 4), memcpy(nca.hdr + 0x254, &en, 4);
    uint8_t *fh = nca.hdr + 0x600;
    fh[2] = 0, fh[3] = 2, fh[4] = 4, fh[5] = meta_layer ? 1 : 0;  // RomFS, HierarchicalSha256, AES-CTR-Ex
    uint64_t zero64 = 0, vsize64 = VSIZE;
    memcpy(fh + 0x8 + 0x24, &layers, 4);
    memcpy(fh + 0x8 + 0x38, &zero64, 8), memcpy(fh + 0x8 + 0x40, &vsize64, 8);  // last layer: the data
    uint64_t io = IND_OFF, is = IND_SIZE, co = CTR_OFF, cs = CTR_SIZE, mo = MH_OFF, ms = meta_layer ? MH_SIZE : 0;
    memcpy(fh + 0x100, &io, 8), memcpy(fh + 0x108, &is, 8);
    memcpy(fh + 0x110, "BKTR", 4), memcpy(fh + 0x114, &ver, 4), memcpy(fh + 0x118, &four, 4);
    memcpy(fh + 0x120, &co, 8), memcpy(fh + 0x128, &cs, 8);
    memcpy(fh + 0x130, "BKTR", 4), memcpy(fh + 0x134, &ver, 4), memcpy(fh + 0x138, &four, 4);
    memcpy(fh + 0x140, &gen_word, 4), memcpy(fh + 0x144, &secure_word, 4);
    memcpy(fh + 0x1A0, &mo, 8), memcpy(fh + 0x1A8, &ms, 8);
    Section sec;
    bool parsed = section_info(&nca, 1, &sec);
    snprintf(what, sizeof what, "%s: section_info reads the patch info and meta hash fields", tag);
    ok_if(what, parsed && sec.enc_type == 4 && sec.indirect_off == IND_OFF && sec.ctr_ex_off == CTR_OFF &&
                    sec.meta_hash_size == ms && sec.meta_hash_type == (meta_layer ? 1 : 0) && sec.data_size == VSIZE);

    uint8_t key[16], iv[16];
    for (int k = 0; k < 16; k++) key[k] = (uint8_t)(0x31 * k + 7);
    memcpy(file + UPD_OFF, sec_plain, SEC);
    for (int i = 0; i < 4; i++) {  // CTR-Ex data (the relocation table too, unless the meta layer holds it)
        uint64_t a = ce[i].off, b = i + 1 < 4 ? ce[i + 1].off : CTR_OFF;
        if (meta_layer && i == 3) b = IND_OFF;
        if (ce[i].plain || a >= b) continue;
        ref_iv(iv, fh, &ce[i].gen, UPD_OFF + a);
        nx_aes128_ctr(key, iv, file + UPD_OFF + a, (size_t)(b - a));
    }
    uint64_t ord_from = meta_layer ? IND_OFF : CTR_OFF;  // ordinary AES-CTR with the header's whole upper IV
    ref_iv(iv, fh, NULL, UPD_OFF + ord_from);
    nx_aes128_ctr(key, iv, file + UPD_OFF + ord_from, (size_t)(SEC - ord_from));
    for (int i = 0; i < 4; i++) {
        uint64_t v = ind[i].v, end = i + 1 < 4 ? ind[i + 1].v : VSIZE;
        memcpy(want + v, ind[i].s ? sec_plain + ind[i].p : file + ind[i].p, (size_t)(end - v));
    }
    FILE *f = fopen(path, "wb");
    if (!f) { ok_if("bktr tables: write the scratch file", false); return; }
    fwrite(file, 1, sizeof file, f);
    fclose(f);

    NxRomfs *r = calloc(1, sizeof *r);
    pthread_mutex_init(&r->lock, NULL);
    r->fd = open(path, O_RDONLY | O_BINARY);
    memcpy(r->key, key, 16);
    r->sec = sec;
    Section base = {.off = 0, .size = BASE_SIZE, .enc_type = 1};
    char why[256] = "";
    bool loaded = parsed && patch_load(r, r->fd, 0, &base, key, why, sizeof why);
    if (!loaded) printf("     patch_load: %s\n", why);
    snprintf(what, sizeof what, "%s: patch_load parses both tables", tag);
    ok_if(what, loaded && r->patch->nctr == 4 && r->patch->nindirect == 4 && r->patch->virtual_size == VSIZE &&
                    r->patch->ctr[2].plain && r->patch->ctr[3].generation == 3);
    snprintf(what, sizeof what, "%s: the merged image matches (IV = secure_value, generation, block)", tag);
    ok_if(what, loaded && romfs_storage_read(r, got, VSIZE, 0) && !memcmp(got, want, VSIZE));
    nx_romfs_close(r);  // (base_fd is the same descriptor here; nx_romfs_close closes it once)

    if (!meta_layer) {
        // an encryption_value other than 0 / 1 is refused
        node = sec_plain + CTR_OFF + BKTR_NODE_SIZE;
        node[0x18 + 16 * 1] = 2;
        memcpy(file + UPD_OFF + CTR_OFF, sec_plain + CTR_OFF, CTR_SIZE);
        ref_iv(iv, fh, NULL, UPD_OFF + CTR_OFF);
        nx_aes128_ctr(key, iv, file + UPD_OFF + CTR_OFF, CTR_SIZE);
        f = fopen(path, "wb");
        fwrite(file, 1, sizeof file, f);
        fclose(f);
        r = calloc(1, sizeof *r);
        pthread_mutex_init(&r->lock, NULL);
        r->fd = open(path, O_RDONLY | O_BINARY);
        memcpy(r->key, key, 16);
        r->sec = sec;
        ok_if("bktr tables: a bad encryption_value is refused", !patch_load(r, r->fd, 0, &base, key, why, sizeof why));
        nx_romfs_close(r);
    } else {
        // a meta hash type other than HierarchicalIntegrity is refused rather than misread
        r = calloc(1, sizeof *r);
        pthread_mutex_init(&r->lock, NULL);
        r->fd = open(path, O_RDONLY | O_BINARY);
        r->sec = sec;
        r->sec.meta_hash_type = 2;
        ok_if("bktr tables (patch meta hash layer): an unknown meta hash type is refused",
              !patch_load(r, r->fd, 0, &base, key, why, sizeof why));
        nx_romfs_close(r);
    }
    remove(path);
}

int main(int argc, char **argv) {
    uint8_t key[32], b[512];
    unhex_s("000102030405060708090a0b0c0d0e0f", key);
    unhex_s("00112233445566778899aabbccddeeff", b);
    uint8_t rk[176];
    aes_expand(key, rk);
    aes_encrypt_block(rk, b);
    check("aes encrypt (FIPS-197 C.1)", b, "69c4e0d86a7b0430d8cdb78070b4c55a");
    nx_aes128_ecb_decrypt(key, b, 16);
    check("aes decrypt", b, "00112233445566778899aabbccddeeff");

    uint8_t iv[16];
    unhex_s("2b7e151628aed2a6abf7158809cf4f3c", key);
    unhex_s("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", iv);
    unhex_s("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52ef", b);
    nx_aes128_ctr(key, iv, b, 45);  // (a partial last block)
    check("aes ctr (SP 800-38A F.5.1)", b, "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff5ae4df3edbd5d35e5b4f0902");

    memset(key, 0, 32);
    memset(b, 0, sizeof b);
    unhex_s("917cf69ebd68b2ec9b9fe9a3eadda692cd43d2f59598ed858c02c2652fbf922e", b);
    aes_xts_decrypt(key, b, 0x200, 0);
    check("aes xts (IEEE 1619 vector 1)", b, "0000000000000000000000000000000000000000000000000000000000000000");

    for (int i = 1; i + 1 < argc && !strcmp(argv[i], "--lz4"); i += 2) {  // --lz4 <file>: [u32 raw size][raw][block]
        FILE *f = fopen(argv[i + 1], "rb");
        uint32_t rn, cn;
        fread(&rn, 4, 1, f), fread(&cn, 4, 1, f);
        uint8_t *raw = malloc(rn), *comp = malloc(cn), *out = malloc(rn);
        fread(raw, 1, rn, f), fread(comp, 1, cn, f);
        fclose(f);
        int n = nx_lz4_decompress(comp, (int)cn, out, (int)rn);
        bool ok = n == (int)rn && !memcmp(raw, out, rn);
        printf("%s lz4 %s (%u -> %u)\n", ok ? "ok  " : "FAIL", argv[i + 1], cn, rn);
        fails += !ok;
        free(raw), free(comp), free(out);
    }

    if (argc >= 3 && strcmp(argv[1], "--lz4")) {  // <prod.keys> <package>: header, key, first RomFS bytes
        Keys ks = {0};
        keys_load(argv[1], &ks);
        int fd = open(argv[2], O_RDONLY);
        Entries files = {0};
        read_fs0(fd, 0, &files);
        for (int i = 0; i < files.n; i++) {
            Nca nca;
            if (!nca_header(fd, files.e[i].off, &ks, &nca)) { printf("     %s: not decrypted\n", files.e[i].name); continue; }
            uint64_t tid;
            memcpy(&tid, nca.hdr + 0x210, 8);
            printf("     %s: NCA3 type %d title %016llx\n", files.e[i].name, nca.hdr[0x205], (unsigned long long)tid);
            if (nca.hdr[0x205] != 0) continue;
            int gen = nca.hdr[0x206] > nca.hdr[0x220] ? nca.hdr[0x206] : nca.hdr[0x220];
            char kn[48];
            snprintf(kn, sizeof kn, "key_area_key_application_%02x", gen ? gen - 1 : 0);
            uint8_t area[0x40];
            memcpy(area, nca.hdr + 0x300, 0x40);
            nx_aes128_ecb_decrypt(key_get(&ks, kn, 16), area, 0x40);
            Section s;
            section_info(&nca, 1, &s);
            uint8_t first[32];
            section_read(fd, nca.base, &s, area + 0x20, first, s.data_off, 32);
            printf("     romfs section at %llx, data %llx, first bytes: ", (unsigned long long)s.off, (unsigned long long)s.data_off);
            for (int k = 0; k < 32; k++) printf("%02x", first[k]);
            printf("\n");
        }
    }
    test_bktr();
    test_bktr_tables(false);
    test_bktr_tables(true);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
