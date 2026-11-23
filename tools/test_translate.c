// Fuzz test for the Maxwell shader translator (runtime/nvn/maxwell/translate.c).
//
//  1. MSL must not change: every program is translated by the current translate.c (key.glsl = 0) and by the
//     reference MSL-only translator (an older translate.c compiled with old_* symbol names, see
//     tools/test_translate.sh); the MSL text and all ShaderInfo fields must be identical.
//  2. GLSL must compile: the same program with key.glsl = 1 is compiled in-process with shaderc (Vulkan 1.3) and
//     the SPIR-V validated with SPIRV-Tools.
//  3. (GLSL) directed SUST cases: each surface-store form either translates to the expected imageStore or fails
//     with its own diagnostic (sust_cases); likewise TMML / TLD4 / TLD4S (texq_cases).
//
// Programs are synthesized from the opcode patterns translate.c recognizes (every OPM(value, mask) and the
// half-precision `(op & mask) == value` tests, read from the source file): the unmasked bits are random, with
// register / predicate / cbuf / texture / attribute / branch fields biased towards meaningful values.
//
// usage: test_translate <translate.c> <count> <seed> [msl|glsl|both] [dump_dir]
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/nvn/maxwell/shader.h"

#ifdef WITH_GLSL
#include <shaderc/shaderc.h>
#include <spirv-tools/libspirv.h>
#endif

// the reference translator (same ShaderKey prefix and ShaderInfo layout)
ShaderInfo *old_maxwell_translate(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key);
void old_maxwell_free(ShaderInfo *s);
extern uint64_t old_maxwell_gmem_base, old_maxwell_gmem_size;

static uint64_t rng_state, g_seed;
static uint64_t rnd64(void) {  // splitmix64
    uint64_t z = (rng_state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static uint32_t rnd(uint32_t n) { return (uint32_t)(rnd64() % n); }

typedef struct { uint64_t val, mask; } Pat;
static Pat pats[512];
static int npats;

static void load_patterns(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc(n + 1);
    fread(s, 1, n, f);
    s[n] = 0;
    fclose(f);
    for (char *p = s; (p = strstr(p, "OPM(0x")); p += 4) {
        unsigned long long v, m;
        if (sscanf(p, "OPM(0x%llxull, 0x%llxull)", &v, &m) == 2 && npats < 512) {
            bool dup = false;
            for (int i = 0; i < npats; i++) dup |= pats[i].val == v && pats[i].mask == m;
            if (!dup) pats[npats++] = (Pat){v, m};
        }
    }
    for (char *p = s; (p = strstr(p, "(op & 0x")); p += 4) {  // emit_half: 16-bit opcode tests
        unsigned m, v;
        if (sscanf(p, "(op & 0x%x) == 0x%x", &m, &v) == 2 && npats < 512) pats[npats++] = (Pat){(uint64_t)v << 48, (uint64_t)m << 48};
    }
    free(s);
    if (npats < 50) { fprintf(stderr, "only %d patterns found in %s\n", npats, path); exit(1); }
}

static uint64_t setf(uint64_t w, int lo, int n, uint64_t v) {
    uint64_t m = ((1ull << n) - 1) << lo;
    return (w & ~m) | ((v << lo) & m);
}
static uint64_t getf(uint64_t w, int lo, int n) { return (w >> lo) & ((1ull << n) - 1); }
static bool is(uint64_t w, uint64_t v, uint64_t m) { return (w & m) == v; }

static uint32_t rreg(void) { uint32_t x = rnd(100); return x < 8 ? 255 : x < 12 ? 250 + rnd(5) : rnd(24); }

static const uint32_t attr_addrs[] = {0x70, 0x74, 0x78, 0x7c, 0x80, 0x84, 0x88, 0x8c, 0x90, 0x94, 0x98, 0x9c, 0xa0,
                                      0xb0, 0xc0, 0x100, 0x270, 0x27c, 0x2f8, 0x2fc, 0x3fc, 0x6c, 0x2e0, 0x2e4, 0x40};

// one random instruction at byte offset pc of a program of `total` bytes
static uint64_t gen_insn(uint32_t pc, uint32_t total) {
    if (rnd(400) == 0) {  // unknown instruction (but no cbuf index >= 17: the translators index arrays of 18)
        uint64_t w = rnd64();
        w = setf(w, 34, 5, rnd(17));
        return setf(w, 36, 5, rnd(17)) & ~(1ull << 38);
    }
    const Pat *p = &pats[rnd(npats)];
    uint64_t w = rnd64();
    if (rnd(100) < 90) w = setf(w, 0, 8, rreg());
    if (rnd(100) < 90) w = setf(w, 8, 8, rreg());
    if (rnd(100) < 70) w = setf(w, 20, 8, rreg());
    if (rnd(100) < 80) w = setf(w, 39, 8, rreg());
    w = setf(w, 16, 4, rnd(100) < 70 ? 7 : rnd(16));  // predicate guard (mostly PT)
    w = (w & ~p->mask) | p->val;
    // cbuf index fields: hardware has 18 buffers; 17 clashes with the gmem binding (both translators reject it)
    if (is(w, 0xef90000000000000ull, 0xfff8000000000000ull)) w = setf(w, 36, 5, rnd(17));  // LDC
    else if (getf(w, 34, 5) >= 17) w = setf(w, 34, 5, rnd(17));
    // texture index: few textures so that slots get reused
    if (is(w, 0xd000000000000000ull, 0xf400000000000000ull) || is(w, 0xc038000000000000ull, 0xfc38000000000000ull) ||
        is(w, 0xdf48000000000000ull, 0xfff8000000000000ull) || is(w, 0xdeb8000000000000ull, 0xfff8000000000000ull))
        w = setf(w, 36, 13, rnd(100) < 90 ? rnd(4) : rnd(8192));
    if (is(w, 0xdf48000000000000ull, 0xfff8000000000000ull) && rnd(100) < 90) w = setf(w, 22, 6, 1);  // TXQ dimension
    // attribute addresses (LD / ST a[], IPA)
    uint32_t aa = attr_addrs[rnd(sizeof attr_addrs / sizeof attr_addrs[0])] + (rnd(100) < 30 ? 4 * rnd(4) : 0);
    if (is(w, 0xefd8000000000000ull, 0xfff8000000000000ull) || is(w, 0xeff0000000000000ull, 0xfff8000000000000ull)) {
        w = setf(w, 20, 10, aa);
        if (rnd(100) < 85) w = setf(w, 8, 8, 255);  // not indexed
    }
    if (is(w, 0xe000000000000000ull, 0xff00000000000000ull)) {
        w = setf(w, 28, 10, aa);
        if (rnd(100) < 90) w = setf(w, 38, 1, 0);
        w = (w & ~p->mask) | p->val;
    }
    if (is(w, 0xf0c8000000000000ull, 0xfff8000000000000ull)) {  // S2R
        static const uint32_t srs[] = {0x00, 0x12, 0x21, 0x22, 0x23, 0x25, 0x26, 0x27, 0x50};
        w = setf(w, 20, 8, srs[rnd(sizeof srs / sizeof srs[0])]);
    }
    if (is(w, 0x5080000000000000ull, 0xfff8000000000000ull) && rnd(100) < 90) {  // MUFU: valid functions
        static const uint32_t fns[] = {0, 1, 2, 3, 4, 5, 8};
        w = setf(w, 20, 4, fns[rnd(7)]);
    }
    if (is(w, 0x5bc0000000000000ull, 0xffe0000000000000ull) || is(w, 0x4bc0000000000000ull, 0xffe0000000000000ull) ||
        is(w, 0x36c0000000000000ull, 0xfee0000000000000ull) || is(w, 0x53c0000000000000ull, 0xffe0000000000000ull))
        if (rnd(100) < 90) w = setf(w, 48, 4, 0);  // PRMT default mode
    // P2R / R2P .CC and BFE .BREV are unsupported: mostly off
    static const uint64_t bit40_ops[] = {0x38e8, 0x5ce8, 0x4ce8, 0x38f0, 0x5cf0, 0x4cf0, 0x5c00, 0x4c00, 0x3800};
    for (int i = 0; i < 9; i++)
        if ((w >> 48 & (bit40_ops[i] & 0xff00 ? (bit40_ops[i] >> 8 == 0x38 ? 0xfef8 : 0xfff8) : 0)) == bit40_ops[i] && rnd(100) < 90)
            w = setf(w, 40, 1, 0);
    if (is(w, 0x5098000000000000ull, 0xfff8000000000000ull) && rnd(100) < 90) return gen_insn(pc, total);  // CSET fails
    // branch targets inside the program
    uint64_t top12 = w & 0xfff0000000000000ull;
    if (top12 == 0xe240000000000000ull || top12 == 0xe290000000000000ull || top12 == 0xe2a0000000000000ull ||
        top12 == 0xe2b0000000000000ull || top12 == 0xe250000000000000ull) {
        if (rnd(100) < 90) {
            int64_t target = (int64_t)rnd(total / 8) * 8;
            int64_t off = target - (int64_t)pc - 8;
            if (off == -8) off = 8;  // not a self-branch (that ends the program)
            w = setf(w, 20, 24, (uint64_t)off & 0xFFFFFF);
        }
        w = setf(w, 5, 1, 0);
        w = (w & ~p->mask) | p->val;
    }
    return w;
}

static size_t gen_program(uint8_t *buf, size_t cap, int stage) {
    memset(buf, 0, cap);
    size_t hdr = stage == STAGE_COMPUTE ? 0 : 0x80;
    if (stage == STAGE_FRAGMENT) {
        uint32_t omap = rnd(100) < 40 ? 0xF : rnd(100) < 50 ? (uint32_t)rnd64() : (uint32_t)rnd64() & 0xFF;
        memcpy(buf + 0x30 + 0x48, &omap, 4);
        uint32_t dw = rnd(4) == 0 ? 2 : 0;
        memcpy(buf + 0x30 + 0x4C, &dw, 4);
    }
    int groups = 1 + rnd(rnd(100) < 70 ? 3 : 10);
    uint32_t total = (uint32_t)(groups + 1) * 32;
    uint64_t *code = (uint64_t *)(buf + hdr);
    int i = 0;
    for (int g = 0; g < groups; g++) {
        code[i++] = rnd64();  // scheduling word
        for (int k = 0; k < 3; k++, i++) code[i] = gen_insn((uint32_t)i * 8, total);
    }
    code[i++] = 0x001f8000fc0007e0ull;
    code[i++] = 0xe30000000007000full;  // EXIT
    code[i++] = 0xe2400fffff87000full;  // BRA self (end of program)
    code[i++] = 0xe2400fffff87000full;
    return hdr + (size_t)i * 8 + 64;
}

static void gen_key(ShaderKey *k, int stage) {
    memset(k, 0, sizeof *k);
    for (int i = 0; i < 32; i++) k->attr_kind[i] = rnd(100) < 80 ? 0 : rnd(3);
    for (int i = 0; i < 8; i++) k->rt_kind[i] = rnd(100) < 80 ? 0 : rnd(3);
    k->depth_minus_one = rnd(2);
    k->flip_y = rnd(2);
    if (stage == STAGE_FRAGMENT && rnd(2)) k->vs_out_mask = (uint32_t)rnd64();
}

static bool same_info(const ShaderInfo *a, const ShaderInfo *b) {
    if ((a->msl == NULL) != (b->msl == NULL)) return false;
    if (a->msl && strcmp(a->msl, b->msl)) return false;
    ShaderInfo x = *a, y = *b;
    x.msl = y.msl = NULL;
    return !memcmp(&x, &y, sizeof x);
}

static void dump(const char *dir, const char *tag, long n, const char *src, const char *err, const uint8_t *prog, size_t len) {
    if (!dir) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s_%llu_%ld.txt", dir, tag, (unsigned long long)g_seed, n);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "// %s\n", err ? err : "");
    if (prog) {
        fprintf(f, "// program:");
        for (size_t i = 0; i < len; i += 8) fprintf(f, " %016llx", (unsigned long long)*(const uint64_t *)(prog + i));
        fprintf(f, "\n");
    }
    fputs(src ? src : "(null)", f);
    fclose(f);
}

#ifdef WITH_GLSL
// Directed SUST cases (GLSL compute). The base word is the store the game's compute shader 72a4723000 issues at
// +0x358 (SUST.P bindless, 2D, RGBA, handle r39, coordinates r0/r1, payload r24..r27); the other forms set the
// bound (51) / typed (52) bits as Ryujinx's InstSust* decoders define them. want: NULL = must translate, else a
// substring of the expected failure message.
static uint64_t sust_word(bool bound, bool typed, uint32_t rgba, uint32_t dim, uint32_t tidb) {
    uint64_t w = 0xeb20000000000000ull | (uint64_t)bound << 51 | (uint64_t)typed << 52 | (uint64_t)dim << 33 |
                 (uint64_t)rgba << 20 | 7ull << 16 | 0x18;
    return bound ? w | (uint64_t)tidb << 36 : w | 39ull << 39;
}

static int sust_cases(shaderc_compiler_t comp, shaderc_compile_options_t opt, spv_context spv) {
    const uint64_t mov_c2_120 = 0x4c98000000000000ull | 0xFull << 39 | 2ull << 34 | (0x120ull / 4) << 20 | 39;  // MOV r39, c[2][0x120]
    const uint64_t nop = 0x50b0000000000f00ull;
    struct { const char *name; uint64_t pre, sust; const char *want, *glsl; uint32_t handle; bool bindless; } cases[] = {
        {"bindless, handle from MOV c[2]", mov_c2_120, 0xeb20138600f70018ull, NULL, "imageStore(t0,", 0x120, false},
        {"bindless, dynamic handle", nop, 0xeb20138600f70018ull, NULL, "bindless_images[nonuniformEXT(r39", 0, true},
        {"bound P, slot 1", nop, sust_word(true, false, 0xF, 3, 0x128 / 4), NULL, "imageStore(t0,", 0x128, false},
        {"bound P, not an NVN image entry", nop, sust_word(true, false, 0xF, 3, 0x20 / 4), "bound SUST.P image handle", NULL, 0, false},
        {"bound P, partial RGBA", nop, sust_word(true, false, 0x3, 3, 0x120 / 4), "unsupported SUST.P form", NULL, 0, false},
        {"bindless, 3D", nop, sust_word(false, false, 0xF, 5, 0), "unsupported SUST.B form", NULL, 0, false},
        {"typed bindless (D.B)", nop, sust_word(false, true, 4, 3, 0), "SUST.D typed store (bindless", NULL, 0, false},
        {"typed bound (D)", nop, sust_word(true, true, 4, 3, 0x120 / 4), "SUST.D typed store (bound", NULL, 0, false},
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint64_t code[] = {0x001f8000fc0007e0ull, cases[i].pre, cases[i].sust, nop,
                           0x001f8000fc0007e0ull, 0xe30000000007000full, 0xe2400fffff87000full, 0xe2400fffff87000full, 0, 0, 0, 0};
        ShaderKey key;
        memset(&key, 0, sizeof key);
        key.glsl = 1;
        ShaderInfo *g = maxwell_translate((const uint8_t *)code, sizeof code, STAGE_COMPUTE, &key);
        const char *why = NULL;
        if (cases[i].want) {
            if (g->msl) why = "translated, expected a failure";
            else if (!strstr(g->error, cases[i].want)) why = "wrong failure message";
        } else if (!g->msl) {
            why = "translation failed";
        } else if (!strstr(g->msl, cases[i].glsl)) {
            why = "expected imageStore not emitted";
        } else if (g->uses_bindless_image != cases[i].bindless) {
            why = "wrong bindless flag";
        } else if (!cases[i].bindless && (g->ntex != 1 || !g->tex[0].is_image || g->tex[0].handle_offset != cases[i].handle)) {
            why = "wrong image slot";
        } else {
            shaderc_compilation_result_t res = shaderc_compile_into_spv(comp, g->msl, strlen(g->msl), shaderc_compute_shader, "sust", "main", opt);
            if (shaderc_result_get_compilation_status(res) != shaderc_compilation_status_success) {
                why = "GLSL does not compile";
                fprintf(stderr, "%s\n%s\n", shaderc_result_get_error_message(res), g->msl);
            } else {
                spv_const_binary_t bin = {(const uint32_t *)shaderc_result_get_bytes(res), shaderc_result_get_length(res) / 4};
                spv_diagnostic diag = NULL;
                if (spvValidate(spv, &bin, &diag) != SPV_SUCCESS) why = "SPIR-V does not validate";
                spvDiagnosticDestroy(diag);
            }
            shaderc_result_release(res);
        }
        printf("SUST %-34s %016llx: %s%s%s\n", cases[i].name, (unsigned long long)cases[i].sust, why ? "FAIL (" : "ok",
               why ? why : "", why ? ")" : "");
        if (why) { printf("    error: %s\n", g->error); bad++; }
        maxwell_free(g);
    }
    return bad;
}

// S2R of the lane id and lane masks (GLSL): the subgroup builtins, compiled and validated. Program: S2R r8, SR; then
// STG.E.128 [r2], r8 (the store word Echoes of Wisdom's culling compute 72a4711000 uses) so the value is live.
static int s2r_cases(shaderc_compiler_t comp, shaderc_compile_options_t opt, spv_context spv) {
    struct { int sr; const char *glsl; } cases[] = {
        {0x00, "gl_SubgroupInvocationID"}, {0x38, "gl_SubgroupEqMask.x"}, {0x39, "gl_SubgroupLtMask.x"},
        {0x3a, "gl_SubgroupLeMask.x"},     {0x3b, "gl_SubgroupGtMask.x"}, {0x3c, "gl_SubgroupGeMask.x"},
    };
    const uint64_t nop = 0x50b0000000000f00ull, stg128 = 0xeede200000070208ull;
    int bad = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint64_t s2r = 0xf0c8000000000000ull | (uint64_t)cases[i].sr << 20 | 8;  // S2R r8, SR
        uint64_t code[] = {0x001f8000fc0007e0ull, s2r, stg128, nop,
                           0x001f8000fc0007e0ull, 0xe30000000007000full, 0xe2400fffff87000full, 0xe2400fffff87000full, 0, 0, 0, 0};
        ShaderKey key;
        memset(&key, 0, sizeof key);
        key.glsl = 1;
        ShaderInfo *g = maxwell_translate((const uint8_t *)code, sizeof code, STAGE_COMPUTE, &key);
        const char *why = NULL;
        if (!g->msl) why = "translation failed";
        else if (!strstr(g->msl, cases[i].glsl)) why = "lane builtin not emitted";
        else {
            shaderc_compilation_result_t res = shaderc_compile_into_spv(comp, g->msl, strlen(g->msl), shaderc_compute_shader, "s2r", "main", opt);
            if (shaderc_result_get_compilation_status(res) != shaderc_compilation_status_success) {
                why = "GLSL does not compile";
                fprintf(stderr, "%s\n", shaderc_result_get_error_message(res));
            } else {
                spv_const_binary_t bin = {(const uint32_t *)shaderc_result_get_bytes(res), shaderc_result_get_length(res) / 4};
                spv_diagnostic diag = NULL;
                if (spvValidate(spv, &bin, &diag) != SPV_SUCCESS) why = "SPIR-V does not validate";
                spvDiagnosticDestroy(diag);
            }
            shaderc_result_release(res);
        }
        // MSL output stays as before (0) for these registers
        key.glsl = 0;
        ShaderInfo *m = maxwell_translate((const uint8_t *)code, sizeof code, STAGE_COMPUTE, &key);
        if (!why && m->msl && strstr(m->msl, cases[i].glsl)) why = "MSL changed";
        maxwell_free(m);
        printf("S2R sr %#04x %-26s %016llx: %s%s%s\n", cases[i].sr, cases[i].glsl, (unsigned long long)s2r, why ? "FAIL (" : "ok",
               why ? why : "", why ? ")" : "");
        if (why) { printf("    error: %s\n", g->error); bad++; }
        maxwell_free(g);
    }
    return bad;
}
// TMML / TLD4 / TLD4S (GLSL fragment): the words Echoes of Wisdom's catalog shaders use (TMML.LOD.P R4, R10, 0x8, 2D,
// 0x2; TLD4.R.P R0, R4, 0x22, 2D, 0xd; TLD4S.R.NODEP.P R2, R0, R6, R7, 0x22, all from nvdisasm) and variants with the
// fields Eden's decoders define. want: NULL = must translate, compile and validate with `glsl` in it; else a substring
// of the expected failure. msl: translate with key.glsl = 0 (these stay GLSL-only, so the MSL output is unchanged).
static int texq_cases(shaderc_compiler_t comp, shaderc_compile_options_t opt, spv_context spv) {
    const uint64_t tmml = 0xdf5800812ff70a04ull, tld4 = 0xc8380226aff70400ull, tld4s = 0xdf02022020770600ull;
    const uint64_t tld4_g_array = (tld4 & ~(3ull << 56) & ~(7ull << 28)) | 1ull << 56 | 3ull << 28;
    struct { const char *name; uint64_t w; int stage; bool msl; const char *want, *glsl; } cases[] = {
        {"TMML 2D element 1", tmml, STAGE_FRAGMENT, false, NULL, "r4 = uint(max(q.x, 0.0f) * 256.0f) & 0xFFFFu"},
        {"TMML elements 0+1", (tmml & ~(0xFull << 31)) | 3ull << 31, STAGE_FRAGMENT, false, NULL, "q.y * 256.0f"},
        {"TMML in a vertex shader", tmml, STAGE_VERTEX, false, "TMML outside a fragment shader", NULL},
        {"TMML elements 2-3", tmml | 4ull << 31, STAGE_FRAGMENT, false, "TMML elements 2-3", NULL},
        {"TMML MSL", tmml, STAGE_FRAGMENT, true, "TMML (Vulkan GLSL only)", NULL},
        {"TLD4.R 2D", tld4, STAGE_FRAGMENT, false, NULL, "textureGather(t0, vec2(as_f(r4), as_f(r5)), 0)"},
        {"TLD4.G 2D array", tld4_g_array, STAGE_FRAGMENT, false, NULL, "textureGather(t0, vec3(vec2(as_f(r5), as_f(r6)), float(min(r4"},
        {"TLD4.DC 2D", tld4 | 1ull << 50, STAGE_FRAGMENT, false, NULL, "sampler2DShadow t0"},
        {"TLD4 AOFFI", tld4 | 1ull << 54, STAGE_FRAGMENT, false, "TLD4 with offsets (AOFFI)", NULL},
        {"TLD4 MSL", tld4, STAGE_FRAGMENT, true, "TLD4 (Vulkan GLSL only)", NULL},
        {"TLD4S.R", tld4s, STAGE_FRAGMENT, false, NULL, "textureGather(t0, vec2(as_f(r6), as_f(r7)), 0)"},
        {"TLD4S.A in a vertex shader", tld4s | 3ull << 52, STAGE_VERTEX, false, NULL, "textureGather(t0, vec2(as_f(r6), as_f(r7)), 3)"},
        {"TLD4S AOFFI", tld4s | 1ull << 51, STAGE_FRAGMENT, false, "TLD4S with an offset", NULL},
    };
    const uint64_t nop = 0x50b0000000000f00ull;
    int bad = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        static uint8_t prog[0x80 + 12 * 8];
        memset(prog, 0, sizeof prog);
        uint32_t omap = 0xF;  // (fragment: colour target 0 written)
        memcpy(prog + 0x30 + 0x48, &omap, 4);
        uint64_t code[] = {0x001f8000fc0007e0ull, cases[i].w, nop, nop,
                           0x001f8000fc0007e0ull, 0xe30000000007000full, 0xe2400fffff87000full, 0xe2400fffff87000full, 0, 0, 0, 0};
        memcpy(prog + 0x80, code, sizeof code);
        ShaderKey key;
        memset(&key, 0, sizeof key);
        key.glsl = !cases[i].msl;
        ShaderInfo *g = maxwell_translate(prog, sizeof prog, cases[i].stage, &key);
        const char *why = NULL;
        if (cases[i].want) {
            if (g->msl) why = "translated, expected a failure";
            else if (!strstr(g->error, cases[i].want)) why = "wrong failure message";
        } else if (!g->msl) {
            why = "translation failed";
        } else if (!strstr(g->msl, cases[i].glsl)) {
            why = "expected GLSL not emitted";
            fprintf(stderr, "%s\n", g->msl);
        } else {
            shaderc_shader_kind kind = cases[i].stage == STAGE_VERTEX ? shaderc_vertex_shader : shaderc_fragment_shader;
            shaderc_compilation_result_t res = shaderc_compile_into_spv(comp, g->msl, strlen(g->msl), kind, "texq", "main", opt);
            if (shaderc_result_get_compilation_status(res) != shaderc_compilation_status_success) {
                why = "GLSL does not compile";
                fprintf(stderr, "%s\n%s\n", shaderc_result_get_error_message(res), g->msl);
            } else {
                spv_const_binary_t bin = {(const uint32_t *)shaderc_result_get_bytes(res), shaderc_result_get_length(res) / 4};
                spv_diagnostic diag = NULL;
                if (spvValidate(spv, &bin, &diag) != SPV_SUCCESS) why = "SPIR-V does not validate";
                spvDiagnosticDestroy(diag);
            }
            shaderc_result_release(res);
        }
        printf("TEXQ %-30s %016llx: %s%s%s\n", cases[i].name, (unsigned long long)cases[i].w, why ? "FAIL (" : "ok",
               why ? why : "", why ? ")" : "");
        if (why) { printf("    error: %s\n", g->error); bad++; }
        maxwell_free(g);
    }
    return bad;
}
#endif

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <translate.c> <count> <seed> [msl|glsl|both] [dump_dir]\n", argv[0]); return 2; }
    load_patterns(argv[1]);
    long count = atol(argv[2]);
    rng_state = g_seed = strtoull(argv[3], NULL, 0);
    const char *mode = argc > 4 ? argv[4] : "both";
    const char *dir = argc > 5 ? argv[5] : NULL;
    bool do_msl = strcmp(mode, "glsl") != 0, do_glsl = strcmp(mode, "msl") != 0;
    maxwell_gmem_base = old_maxwell_gmem_base = 0x80000000ull;
    maxwell_gmem_size = old_maxwell_gmem_size = 0x10000000ull;
#ifdef WITH_GLSL
    shaderc_compiler_t comp = shaderc_compiler_initialize();
    shaderc_compile_options_t opt = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(opt, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_source_language(opt, shaderc_source_language_glsl);
    spv_context spv = spvContextCreate(SPV_ENV_VULKAN_1_3);
    int sust_bad = do_glsl ? sust_cases(comp, opt, spv) + s2r_cases(comp, opt, spv) + texq_cases(comp, opt, spv) : 0;
#else
    int sust_bad = 0;
    if (do_glsl) { fprintf(stderr, "built without WITH_GLSL: msl mode only\n"); do_glsl = false; }
#endif
    static uint8_t prog[0x80 + 64 * 32 + 128];
    long msl_ok = 0, msl_fail = 0, msl_diff = 0, gl_ok = 0, gl_tfail = 0, gl_tfail_only = 0, gl_cfail = 0, gl_vfail = 0;
    long ext_checked = 0, ext_diff = 0;
    long by_stage[3] = {0};
    char reasons[64][96];
    long reason_n[64];
    int nreasons = 0;
    for (long n = 0; n < count; n++) {
        int stage = rnd(3) == 0 ? STAGE_VERTEX : rnd(2) ? STAGE_FRAGMENT : STAGE_COMPUTE;
        size_t len = gen_program(prog, sizeof prog, stage);
        ShaderKey key;
        gen_key(&key, stage);
        bool msl_good = true;
        if (do_msl) {
            ShaderInfo *a = maxwell_translate(prog, len, stage, &key);
            ShaderInfo *b = old_maxwell_translate(prog, len, stage, &key);
            if (!same_info(a, b)) {
                if (msl_diff < 20) dump(dir, "msl_diff", n, a->msl, b->msl, prog, len);
                msl_diff++;
            }
            if (a->msl) msl_ok++; else msl_fail++;
            msl_good = a->msl != NULL;
            maxwell_free(a);
            old_maxwell_free(b);
        }
#ifdef WITH_GLSL
        if (do_glsl) {
            key.glsl = 1;
            ShaderInfo *g = maxwell_translate(prog, len, stage, &key);
            {   // maxwell_code_extent: the shader warmup cache stores only these bytes and translates them later, so a
                // copy of exactly them - and a copy whose bytes after them are garbage - must translate the same
                size_t ext = maxwell_code_extent(prog, len, stage);
                uint8_t *exact = malloc(ext ? ext : 1), *junk = malloc(len);
                memcpy(exact, prog, ext);
                memcpy(junk, prog, ext);
                memset(junk + ext, 0xA5, len - ext);
                ShaderInfo *e1 = maxwell_translate(exact, ext, stage, &key), *e2 = maxwell_translate(junk, len, stage, &key);
                bool same = true;
                for (int v = 0; v < 2; v++) {
                    ShaderInfo *e = v ? e2 : e1;
                    if (!g->msl != !e->msl || (g->msl && strcmp(g->msl, e->msl)) || (!g->msl && strcmp(g->error, e->error))) same = false;
                }
                if (!same) {
                    if (ext_diff < 10) {
                        fprintf(stderr, "code extent mismatch: program %ld stage %d len %zu extent %zu\n", n, stage, len, ext);
                        dump(dir, "extent_diff", n, g->msl ? g->msl : g->error, e1->msl ? e1->msl : e1->error, prog, len);
                    }
                    ext_diff++;
                }
                ext_checked++;
                maxwell_free(e1);
                maxwell_free(e2);
                free(exact);
                free(junk);
            }
            if (!g->msl) {
                gl_tfail++;
                if (msl_good && do_msl) gl_tfail_only++;
                {  // tally the reasons (GLSL-only: the MSL translation succeeded)
                    char r[96];
                    snprintf(r, sizeof r, "%s%.24s", msl_good && do_msl ? "GLSL-only " : "", g->error);
                    for (char *q = r; *q; q++) if (*q >= '0' && *q <= '9') *q = '#';
                    int i = 0;
                    while (i < nreasons && strcmp(reasons[i], r)) i++;
                    if (i == nreasons && nreasons < 64) { strcpy(reasons[nreasons], r); reason_n[nreasons++] = 0; }
                    if (i < 64) reason_n[i]++;
                }
            } else {
                shaderc_shader_kind kind = stage == STAGE_VERTEX ? shaderc_vertex_shader
                                         : stage == STAGE_FRAGMENT ? shaderc_fragment_shader : shaderc_compute_shader;
                shaderc_compilation_result_t res = shaderc_compile_into_spv(comp, g->msl, strlen(g->msl), kind, "fuzz", "main", opt);
                if (shaderc_result_get_compilation_status(res) != shaderc_compilation_status_success) {
                    if (gl_cfail < 50) dump(dir, "glsl_compile", n, g->msl, shaderc_result_get_error_message(res), prog, len);
                    gl_cfail++;
                } else {
                    spv_diagnostic diag = NULL;
                    spv_const_binary_t bin = {(const uint32_t *)shaderc_result_get_bytes(res), shaderc_result_get_length(res) / 4};
                    spv_validator_options vo = spvValidatorOptionsCreate();
                    spv_result_t vr = spvValidateWithOptions(spv, vo, &bin, &diag);
                    spvValidatorOptionsDestroy(vo);
                    if (vr != SPV_SUCCESS) {
                        if (gl_vfail < 50) dump(dir, "glsl_validate", n, g->msl, diag ? diag->error : "?", prog, len);
                        gl_vfail++;
                    } else {
                        gl_ok++;
                        by_stage[stage == STAGE_VERTEX ? 0 : stage == STAGE_FRAGMENT ? 1 : 2]++;
                        if (dir && gl_ok <= (getenv("TT_DUMP_OK") ? atol(getenv("TT_DUMP_OK")) : 40)) dump(dir, "glsl_ok", n, g->msl, NULL, prog, len);
                    }
                    spvDiagnosticDestroy(diag);
                }
                shaderc_result_release(res);
            }
            maxwell_free(g);
        }
#endif
    }
    if (do_msl) printf("MSL: %ld programs, %ld translated, %ld failed in both, %ld DIFFERENCES\n", count, msl_ok, msl_fail, msl_diff);
    if (do_glsl) {
        printf("GLSL: %ld compiled+validated (vs %ld / fs %ld / cs %ld), %ld compile failures, %ld validation failures, "
               "%ld translation failures (%ld where MSL translated)\n",
               gl_ok, by_stage[0], by_stage[1], by_stage[2], gl_cfail, gl_vfail, gl_tfail, gl_tfail_only);
        for (int i = 0; i < nreasons; i++) printf("  translation failure: %-36s %ld\n", reasons[i], reason_n[i]);
        printf("code extent: %ld programs re-translated from their extent bytes, %ld DIFFERENCES\n", ext_checked, ext_diff);
    }
    return (msl_diff || gl_cfail || gl_vfail || sust_bad || ext_diff) ? 1 : 0;
}
