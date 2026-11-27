// nn::pl shared system fonts. The Switch's system fonts are not part of the game; in order of preference:
//  1. EOWRECOMP_FONT=<file> (.ttf / .otf / .ttc face 0 / .bfttf)
//  2. a dump from the user's own console in fonts/: nintendo_udsg-r_std_003.bfttf (Standard, Morisawa UD Shin Go)
//     and nintendo_ext_003.bfttf (NintendoExt button glyphs); plain .ttf versions of those names also work
//  3. a Japanese gothic like UD Shin Go (much closer to the game's look than Arial): Hiragino Sans W4 on macOS,
//     Yu Gothic Regular / Medium on Windows
//  4. Arial
// Chinese and Korean text use their own shared font types (font_for), each found the same way: the console's font
// from fonts/, then a Windows font with those glyphs, then Standard. Standard alone (a Japanese font) lacks the
// simplified-only characters: in Simplified Chinese the file select showed 自'保存 / '除 / 要'取 for 自动保存 / 删除 /
// 要读取 (2026-11-27, Yu Gothic).
// .bfttf is the console's obfuscated TTF: 8-byte header (magic, size) then every u32 XORed with a fixed key.
// .ttc collections get face 0 copied out as a standalone font (the game's font engine wants one sfnt).
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../hle.h"
#include "../paths.h"

typedef struct { uint64_t addr, size; } Font;
// By SharedFontType (0 Standard, 1 ChineseSimple, 2 ChineseSimpleExt, 3 ChineseTraditional, 4 Korean, 5 NintendoExt);
// an empty one is served as Standard
static Font fonts[6];

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*len + 4);
    if (fread(p, 1, *len, f) != *len) { free(p); p = NULL; }
    fclose(f);
    return p;
}

// .bfttf -> TTF in place (returns the new length, 0 if it isn't one)
static size_t bfttf_decode(uint8_t *d, size_t len) {
    static const uint32_t KEY = 0x49621806;
    if (len < 12) return 0;
    for (int swap = 0; swap < 2; swap++) {  // the key's byte order differs between descriptions; accept both
        uint32_t k = swap ? __builtin_bswap32(KEY) : KEY;
        uint32_t w0, w1, w2;
        memcpy(&w0, d, 4); memcpy(&w1, d + 4, 4); memcpy(&w2, d + 8, 4);
        uint32_t size = w1 ^ k, ver = w2 ^ k;
        if (size == 0 || size > len - 8 || (ver != 0x00000100u && ver != 0x4F54544Fu)) continue;  // sfnt 1.0 / OTTO
        (void)w0;
        for (size_t i = 0; i + 4 <= size; i += 4) {
            uint32_t w;
            memcpy(&w, d + 8 + i, 4);
            w ^= k;
            memcpy(d + i, &w, 4);
        }
        return size;
    }
    return 0;
}

// .ttc -> face 0 as its own sfnt (table data copied, offsets rewritten)
static uint8_t *ttc_face0(const uint8_t *f, size_t len, size_t *out_len) {
    if (len < 16 || memcmp(f, "ttcf", 4)) return NULL;
    uint32_t o = be32(f + 12);
    if (o + 12 > len) return NULL;
    uint32_t nt = (uint32_t)f[o + 4] << 8 | f[o + 5];
    size_t total = 12 + 16 * nt;
    for (uint32_t i = 0; i < nt; i++) total += (be32(f + o + 12 + 16 * i + 12) + 3) & ~3u;
    uint8_t *out = calloc(1, total);
    memcpy(out, f + o, 12);
    size_t pos = 12 + 16 * nt;
    for (uint32_t i = 0; i < nt; i++) {
        const uint8_t *e = f + o + 12 + 16 * i;
        uint32_t off = be32(e + 8), n = be32(e + 12);
        if ((size_t)off + n > len) { free(out); return NULL; }
        memcpy(out + 12 + 16 * i, e, 8);  // tag, checksum
        put32(out + 12 + 16 * i + 8, (uint32_t)pos);
        put32(out + 12 + 16 * i + 12, n);
        memcpy(out + pos, f + off, n);
        pos += (n + 3) & ~3u;
    }
    *out_len = total;
    return out;
}

static int load_one(const char *path, Font *ft) {
    size_t len;
    uint8_t *d = read_file(path, &len);
    if (!d) return 0;
    size_t n = strlen(path);
    if (n > 6 && !strcasecmp(path + n - 6, ".bfttf")) {
        len = bfttf_decode(d, len);
        if (!len) { LOG("[pl] %s: not a valid .bfttf\n", path); free(d); return 0; }
    } else if (len >= 4 && !memcmp(d, "ttcf", 4)) {
        size_t l2;
        uint8_t *f0 = ttc_face0(d, len, &l2);
        free(d);
        if (!f0) { LOG("[pl] %s: bad font collection\n", path); return 0; }
        d = f0;
        len = l2;
    }
    void *p = hle_alloc(len + 16);
    memcpy(p, d, len);
    free(d);
    ft->addr = (uint64_t)p;
    ft->size = len;
    LOG("[pl] system font %s (%zu bytes)\n", path, len);
    return 1;
}

static int g_fonts_loaded;
RT_ON_RESET(pl_reset) { g_fonts_loaded = 0; memset(fonts, 0, sizeof fonts); }  // (they were in the HLE heap)

static void load_fonts(void) {
    if (g_fonts_loaded) return;
    g_fonts_loaded = 1;
    const char *env = getenv("EOWRECOMP_FONT");
    // (the copy the app ships: third_party/fonts/DejaVu - Android has no TrueType system font to rely on)
    static char shipped[1100];
    if (!rt_resource("DejaVuSans.ttf", "third_party/fonts/DejaVu/DejaVuSans.ttf", shipped, sizeof shipped)) shipped[0] = 0;
    const char *std[] = {env, "fonts/nintendo_udsg-r_std_003.bfttf", "fonts/nintendo_udsg-r_std_003.ttf",
                         "fonts/nintendo_udsgr_std_003.ttf", "fonts/nintendo_udsgr_std_003.bfttf",
                         "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc", "C:/Windows/Fonts/YuGothR.ttc",
                         "C:/Windows/Fonts/YuGothM.ttc", "/System/Library/Fonts/Supplemental/Arial.ttf",
                         "C:/Windows/Fonts/arial.ttf",
                         // Linux: DejaVu Sans (Debian / Ubuntu, Arch, Fedora). Not Noto Sans CJK: CFF outlines,
                         // which the game's font engine crashes on (it wants TrueType)
                         "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                         "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", shipped[0] ? shipped : NULL};
    for (size_t i = 0; i < sizeof std / sizeof *std && !fonts[0].addr; i++)
        if (std[i]) load_one(std[i], &fonts[0]);
    if (!fonts[0].addr) FATAL("no usable system font substitute\n");
    // The other types: the console's file names (dumped by the player), then a Windows font with the same script
    // (all TrueType outlines, which the game's engine needs: Microsoft YaHei, JhengHei, Malgun Gothic). Not found:
    // Standard. ChineseSimpleExt is the extra simplified glyphs; YaHei has them, so it serves both Simple types.
    static const char *const per_type[6][5] = {
        [1] = {"fonts/nintendo_udsg-r_org_zh-cn_003.bfttf", "fonts/nintendo_udsg-r_org_zh-cn_003.ttf",
               "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simsun.ttc"},
        [2] = {"fonts/nintendo_udsg-r_ext_zh-cn_003.bfttf", "fonts/nintendo_udsg-r_ext_zh-cn_003.ttf",
               "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simsun.ttc"},
        [3] = {"fonts/nintendo_udjxh-db_zh-tw_003.bfttf", "fonts/nintendo_udjxh-db_zh-tw_003.ttf",
               "C:/Windows/Fonts/msjh.ttc", "C:/Windows/Fonts/mingliu.ttc"},
        [4] = {"fonts/nintendo_udsg-r_ko_003.bfttf", "fonts/nintendo_udsg-r_ko_003.ttf", "C:/Windows/Fonts/malgun.ttf"},
        [5] = {"fonts/nintendo_ext_003.bfttf", "fonts/nintendo_ext_003.ttf"},
    };
    const char *from[6] = {0};  // the file each type was loaded from
    for (int t = 1; t < 6; t++)
        for (int i = 0; i < 5 && per_type[t][i] && !fonts[t].addr; i++) {
            for (int u = 1; u < t; u++)  // (the same file for two types is loaded once: YaHei is 19 MB)
                if (from[u] && !strcmp(from[u], per_type[t][i])) fonts[t] = fonts[u];
            if (fonts[t].addr || load_one(per_type[t][i], &fonts[t])) from[t] = per_type[t][i];
        }
}

static const Font *font_for(uint64_t type) {
    load_fonts();
    return type < 6 && fonts[type].addr ? &fonts[type] : &fonts[0];
}

HLE_FUNC("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", pl_get_load_state) { c->x[0] = 1; }
HLE_FUNC("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", pl_get_address) { c->x[0] = font_for(c->x[0])->addr; }
HLE_FUNC("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", pl_get_size) { c->x[0] = font_for(c->x[0])->size; }
