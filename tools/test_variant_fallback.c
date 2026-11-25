// Variant fallback decisions (runtime/nvn/vulkan/variant_fallback.h): which built variant of a guest program may
// draw for a variant still building (exec.c draw_). No game data.
//
//  1. Directed cases: each rule accepts or rejects as documented, including the ones that must reject (other code,
//     unknown identity, clip-space flags, a read vertex input or written target of another kind, stencil samplers,
//     fields no rule covers).
//  2. Against the translator: programs synthesized by tools/test_translate.c are translated (GLSL) with a key pair A,
//     then with a mutated pair B. Whenever vf_check accepts A for B, both translations must be identical - GLSL text
//     and every ShaderInfo field - since the stand-in's shaders, reflection and descriptor layouts are used as the
//     exact variant's. Rejections are counted by rule, with how many of them really changed the output.
//
// usage: test_variant_fallback <translate.c> <count> <seed>     (tools/test_variant_fallback.sh builds and runs it)
#define main test_translate_main
#include "test_translate.c"
#undef main

#include "../runtime/nvn/vulkan/variant_fallback.h"

ShaderInfo *old_maxwell_translate(const uint8_t *c, size_t m, int s, const ShaderKey *k) { (void)c, (void)m, (void)s, (void)k; return NULL; }
void old_maxwell_free(ShaderInfo *s) { (void)s; }
uint64_t old_maxwell_gmem_base, old_maxwell_gmem_size;

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static VfVariant base_variant(void) {
    VfVariant v;
    memset(&v, 0, sizeof v);
    v.gid[0] = 0x1111222233334444ull;
    v.gid[1] = 0x5555666677778888ull;
    v.gext[0] = 0x400;
    v.gext[1] = 0x600;
    v.vkey.glsl = v.fkey.glsl = 1;
    v.vkey.flip_y = v.fkey.flip_y = 1;
    v.vkey.depth_minus_one = 1;
    return v;
}

static void expect(const char *what, const VfVariant *want, const VfVariant *cand, uint32_t attr_mask, uint32_t out_mask,
                   VfVerdict v) {
    VfVerdict got = vf_check(want, cand, attr_mask, out_mask);
    CHECK(got == v, "%s: got '%s', expected '%s'", what, vf_verdict_names[got], vf_verdict_names[v]);
}

static void directed_cases(void) {
    const uint32_t attrs = 0x7;     // the VS reads a0..a2
    const uint32_t outs = 0x0F;     // the FS writes c0 only
    VfVariant a = base_variant(), b;

    expect("same keys", &a, &a, attrs, outs, VF_OK);

    b = a; b.gid[1] = 0;
    expect("unknown FS code id (want)", &b, &a, attrs, outs, VF_NO_IDENTITY);
    expect("unknown FS code id (candidate)", &a, &b, attrs, outs, VF_NO_IDENTITY);
    b = a; b.gid[0] = 0;
    expect("unknown VS code id", &b, &a, attrs, outs, VF_NO_IDENTITY);
    b = a; b.gid[1] ^= 1;
    expect("other FS code", &b, &a, attrs, outs, VF_OTHER_CODE);
    b = a; b.gid[0] ^= 1;
    expect("other VS code", &b, &a, attrs, outs, VF_OTHER_CODE);
    b = a; b.gext[1] += 8;
    expect("same id, other FS extent", &b, &a, attrs, outs, VF_OTHER_CODE);
    b = a; b.gext[0] += 8;
    expect("same id, other VS extent", &b, &a, attrs, outs, VF_OTHER_CODE);

    b = a; b.vkey.flip_y = 0;
    expect("VS flip_y", &b, &a, attrs, outs, VF_FLAGS);
    b = a; b.fkey.flip_y = 0;
    expect("FS flip_y", &b, &a, attrs, outs, VF_FLAGS);
    b = a; b.vkey.depth_minus_one = 0;
    expect("depth mapping", &b, &a, attrs, outs, VF_FLAGS);
    b = a; b.vkey.flip_x = 1;
    expect("VS flip_x", &b, &a, attrs, outs, VF_FLAGS);
    b = a; b.fkey.flip_x = 1;
    expect("FS flip_x (frag coord)", &b, &a, attrs, outs, VF_FLAGS);
    b = a; b.fkey.glsl = 0;
    expect("dialect", &b, &a, attrs, outs, VF_FLAGS);

    for (int kind = 1; kind <= 2; kind++) {
        b = a; b.vkey.attr_kind[1] = (uint8_t)kind;
        expect("read vertex input of another kind", &b, &a, attrs, outs, VF_ATTR_KIND);
        b = a; b.vkey.attr_kind[5] = (uint8_t)kind;
        expect("unread vertex input of another kind", &b, &a, attrs, outs, VF_OK);
        b = a; b.vkey.attr_kind[31] = (uint8_t)kind;
        expect("attribute 31 read", &b, &a, attrs | 0x80000000u, outs, VF_ATTR_KIND);
        b = a; b.fkey.rt_kind[0] = (uint8_t)kind;
        expect("written target of another kind", &b, &a, attrs, outs, VF_RT_KIND);
        b = a; b.fkey.rt_kind[1] = (uint8_t)kind;
        expect("unwritten target of another kind", &b, &a, attrs, outs, VF_OK);
        b = a; b.fkey.rt_kind[7] = (uint8_t)kind;
        expect("target 7, one component written", &b, &a, attrs, outs | 0x80000000u, VF_RT_KIND);
    }
    b = a; b.vkey.attr_kind[3] = 2; b.vkey.attr_kind[9] = 1; b.fkey.rt_kind[2] = 1; b.fkey.rt_kind[6] = 2;
    expect("several unused differences together", &b, &a, attrs, outs, VF_OK);

    b = a; b.fkey.stencil_tex = 1;
    expect("stencil sampler wanted", &b, &a, attrs, outs, VF_STENCIL);
    expect("stencil sampler in the candidate only", &a, &b, attrs, outs, VF_STENCIL);
    VfVariant c = b;
    c.fkey.stencil_tex = 2;
    expect("other stencil slots", &c, &b, attrs, outs, VF_STENCIL);
    expect("same stencil slots", &b, &b, attrs, outs, VF_OK);

    b = a; b.fkey.vs_out_mask = 0x1F;  // (the built candidate has its VS's outputs; the lookup key has 0)
    expect("FS vs_out_mask from the translation", &a, &b, attrs, outs, VF_OK);
    b = a; b.vkey.rt_kind[0] = 1;
    expect("VS key rt_kind (no rule)", &b, &a, attrs, outs, VF_OTHER_FIELD);
    b = a; b.fkey.attr_kind[0] = 1;
    expect("FS key attr_kind (no rule)", &b, &a, attrs, outs, VF_OTHER_FIELD);
    b = a; b.vkey.stencil_tex = 1;
    expect("VS key stencil_tex (no rule)", &b, &a, attrs, outs, VF_OTHER_FIELD);
    b = a; b.vkey.vs_out_mask = 1;
    expect("VS key vs_out_mask (no rule)", &b, &a, attrs, outs, VF_OTHER_FIELD);
}

typedef struct { ShaderInfo *vi, *fi; } Pair;

static Pair translate_pair(const uint8_t *vs, size_t vlen, const uint8_t *fs, size_t flen, const ShaderKey *vk, ShaderKey *fk) {
    Pair p = {maxwell_translate(vs, vlen, STAGE_VERTEX, vk), NULL};
    if (p.vi->msl) {
        fk->vs_out_mask = p.vi->out_mask;  // (as exec.c translate_program)
        p.fi = maxwell_translate(fs, flen, STAGE_FRAGMENT, fk);
    }
    return p;
}

static void free_pair(Pair *p) {
    if (p->vi) maxwell_free(p->vi);
    if (p->fi) maxwell_free(p->fi);
}

// One random difference (or a few), biased towards the fields that matter and towards both used and unused slots.
static void mutate(VfVariant *b, const ShaderInfo *vi, const ShaderInfo *fi) {
    int n = 1 + (int)rnd(3);
    for (int m = 0; m < n; m++) {
        switch (rnd(9)) {
        case 0: case 1: {  // a vertex input, read by the VS half of the time
            int a = (int)rnd(32);
            if (vi->attr_mask && rnd(2)) do a = (int)rnd(32); while (!((vi->attr_mask >> a) & 1));
            b->vkey.attr_kind[a] = (uint8_t)((b->vkey.attr_kind[a] + 1 + rnd(2)) % 3);
            break;
        }
        case 2: case 3: {  // a target, written by the FS half of the time
            int rt = (int)rnd(8);
            if (fi->out_mask && rnd(2)) do rt = (int)rnd(8); while (!((fi->out_mask >> (rt * 4)) & 0xF));
            b->fkey.rt_kind[rt] = (uint8_t)((b->fkey.rt_kind[rt] + 1 + rnd(2)) % 3);
            break;
        }
        case 4: b->vkey.flip_y ^= 1, b->fkey.flip_y ^= 1; break;
        case 5: b->vkey.depth_minus_one ^= 1; break;
        case 6: {  // a stencil sampler slot: one of the FS's textures when it has some
            int slot = fi->ntex && rnd(4) ? (int)rnd((uint32_t)(fi->ntex < 16 ? fi->ntex : 16)) : (int)rnd(16);
            b->fkey.stencil_tex ^= (uint16_t)(1u << slot);
            break;
        }
        case 7: b->vkey.flip_x ^= 1, b->fkey.flip_x ^= 1; break;
        default: break;  // (same keys: must be accepted and identical)
        }
    }
}

static long n_pairs, n_accepted, n_accepted_changed, n_identical_bad, n_reject[VF_NVERDICTS], n_reject_differs[VF_NVERDICTS];

static void translator_cases(long count) {
    static uint8_t vs[0x80 + 64 * 32 + 128], fs[0x80 + 64 * 32 + 128];
    for (long n = 0; n < count; n++) {
        size_t vlen = gen_program(vs, sizeof vs, STAGE_VERTEX);
        size_t flen = gen_program(fs, sizeof fs, STAGE_FRAGMENT);
        VfVariant a = base_variant();
        gen_key(&a.vkey, STAGE_VERTEX);
        gen_key(&a.fkey, STAGE_FRAGMENT);
        a.vkey.glsl = a.fkey.glsl = 1;
        a.fkey.flip_y = a.vkey.flip_y;  // (draw_ sets both from the window origin; depth mapping only in the VS key)
        a.fkey.depth_minus_one = 0;
        memset(a.vkey.rt_kind, 0, sizeof a.vkey.rt_kind);
        memset(a.fkey.attr_kind, 0, sizeof a.fkey.attr_kind);
        a.fkey.vs_out_mask = 0;
        Pair pa = translate_pair(vs, vlen, fs, flen, &a.vkey, &a.fkey);
        if (!pa.vi->msl || !pa.fi || !pa.fi->msl) { free_pair(&pa); continue; }
        n_pairs++;
        // the candidate is the built pair A (its fkey carries the VS outputs); the wanted variant's lookup key has none
        VfVariant b = a;
        b.fkey.vs_out_mask = 0;
        mutate(&b, pa.vi, pa.fi);
        VfVerdict v = vf_check(&b, &a, pa.vi->attr_mask, pa.fi->out_mask);
        ShaderKey bf = b.fkey;
        Pair pb = translate_pair(vs, vlen, fs, flen, &b.vkey, &bf);
        bool ok_b = pb.vi->msl && pb.fi && pb.fi->msl;
        bool same = ok_b && same_info(pa.vi, pb.vi) && same_info(pa.fi, pb.fi);
        if (v == VF_OK) {
            n_accepted++;
            ShaderKey av = a.vkey, af = a.fkey;
            af.vs_out_mask = 0;
            if (memcmp(&av, &b.vkey, sizeof av) || memcmp(&af, &b.fkey, sizeof af)) n_accepted_changed++;
            if (!same) {
                n_identical_bad++;
                if (n_identical_bad <= 5)
                    printf("FAIL program %ld: stand-in accepted, but the exact variant translates differently%s\n", n,
                           ok_b ? "" : " (it fails)");
            }
            checks++;
            if (!same) failures++;
        } else {
            n_reject[v]++;
            if (!same) n_reject_differs[v]++;
        }
        free_pair(&pa);
        free_pair(&pb);
    }
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <translate.c> <count> <seed>\n", argv[0]); return 2; }
    load_patterns(argv[1]);
    long count = atol(argv[2]);
    rng_state = g_seed = strtoull(argv[3], NULL, 0);
    maxwell_gmem_base = 0x80000000ull;
    maxwell_gmem_size = 0x10000000ull;
    directed_cases();
    int directed = checks, directed_bad = failures;
    printf("directed: %d checks, %d failed\n", directed, directed_bad);
    translator_cases(count);
    printf("translator: %ld translated pairs; %ld stand-ins accepted (%ld with other keys), %ld of them not identical\n",
           n_pairs, n_accepted, n_accepted_changed, n_identical_bad);
    for (int v = 1; v < VF_NVERDICTS; v++)
        if (n_reject[v]) printf("  rejected, %s: %ld (output really differed in %ld)\n", vf_verdict_names[v], n_reject[v], n_reject_differs[v]);
    // the property is only meaningful if keys that differ were accepted, and the rules reject real differences
    CHECK(n_accepted_changed > count / 50, "too few accepted stand-ins with other keys: %ld", n_accepted_changed);
    CHECK(n_reject_differs[VF_ATTR_KIND] > 0 && n_reject_differs[VF_RT_KIND] > 0 && n_reject_differs[VF_FLAGS] > 0,
          "the attribute / target / flag rules never rejected a real difference");
    printf("%s: %d checks, %d failed\n", failures ? "FAILED" : "passed", checks, failures);
    return failures ? 1 : 0;
}
