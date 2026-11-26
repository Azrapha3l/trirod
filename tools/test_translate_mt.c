// The translator from several threads at once (runtime/nvn/maxwell/translate.c): exec.c translates on a pool of
// threads and Trirod-ShaderPrep on its workers, without a lock. No game data.
//
// Programs synthesized by tools/test_translate.c (vertex, fragment and compute, GLSL and MSL, random keys) are
// translated once on one thread for the reference, then by THREADS threads at the same time, each going through all
// of them in its own order, several rounds. Every result must equal the reference: GLSL / MSL text and every
// ShaderInfo field. (Until 2026-11-25 RSC2 returned its texel coordinates in a static 4-entry ring: two threads
// translating texel fetches overwrote each other's coordinates. `REF=f85ef0a tools/test_translate_mt.sh` builds that
// commit's translator instead: its second run, only programs with fetches on 16 threads, had 193 of 960,000
// translations differ.)
//
// usage: test_translate_mt <translate.c> <programs> <seed> <threads> <rounds>   (tools/test_translate_mt.sh)
#define main test_translate_main
#include "test_translate.c"
#undef main

#include <pthread.h>

ShaderInfo *old_maxwell_translate(const uint8_t *c, size_t m, int s, const ShaderKey *k) { (void)c, (void)m, (void)s, (void)k; return NULL; }
void old_maxwell_free(ShaderInfo *s) { (void)s; }
uint64_t old_maxwell_gmem_base, old_maxwell_gmem_size;

typedef struct {
    uint8_t code[4096];
    size_t len;
    int stage;
    ShaderKey key;
    ShaderInfo *ref;
} Case;

static Case *cases;
static long ncases;
static int rounds;
static _Atomic long mismatches, translated;
static _Atomic int go;

static void *worker(void *arg) {
    uint64_t seed = (uint64_t)(uintptr_t)arg * 0x9E3779B97F4A7C15ull + 1;
    while (!go) {}  // (start together)
    for (int r = 0; r < rounds; r++) {
        long start = (long)(seed % (uint64_t)ncases), step = 1 + (long)((seed >> 32) % (uint64_t)(ncases - 1 ? ncases - 1 : 1));
        while (ncases > 1 && step % ncases == 0) step++;
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        for (long n = 0, i = start; n < ncases; n++, i = (i + step) % ncases) {
            Case *c = &cases[i];
            ShaderInfo *si = maxwell_translate(c->code, c->len, c->stage, &c->key);
            translated++;
            if (!same_info(si, c->ref)) {
                if (mismatches++ < 8)
                    printf("FAIL: case %ld (stage %d) differs from the single-thread translation:\n--- reference\n%.400s\n--- thread\n%.400s\n",
                           i, c->stage, c->ref->msl ? c->ref->msl : c->ref->error, si->msl ? si->msl : si->error);
            }
            maxwell_free(si);
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: %s <translate.c> <programs> <seed> <threads> <rounds>\n", argv[0]); return 2; }
    load_patterns(argv[1]);
    ncases = atol(argv[2]);
    rng_state = g_seed = strtoull(argv[3], NULL, 0);
    int threads = atoi(argv[4]);
    rounds = atoi(argv[5]);
    if (ncases < 1 || threads < 1 || threads > 64 || rounds < 1) return 2;
    maxwell_gmem_base = 0x8000000000ull;  // (exec.c translator_globals: set once, before any translation)
    maxwell_gmem_size = 0x100000000ull;
    // A quarter of the cases are programs with scaled texel fetches (RSC2, the state that wasn't re-entrant): about 1
    // in 250 random programs has one, so they are searched for (at most 400 random programs per one wanted).
    cases = calloc((size_t)ncases, sizeof *cases);
    long ok = 0, fetches = 0, want_fetches = getenv("FETCH_ONLY") ? ncases : ncases / 4;  // (FETCH_ONLY=1: all of them)
    for (long i = 0, tries = 0; i < ncases; i++) {
        Case *c = &cases[i];
        for (;;) {
            int pick = rnd(10);
            bool hunt = fetches < want_fetches && tries < want_fetches * 400;
            c->stage = hunt || pick < 4 ? STAGE_FRAGMENT : pick < 8 ? STAGE_VERTEX : STAGE_COMPUTE;
            c->len = gen_program(c->code, sizeof c->code, c->stage);
            gen_key(&c->key, c->stage);
            c->key.glsl = rnd(4) != 0;  // (mostly the Vulkan path, some MSL)
            if (c->stage == STAGE_COMPUTE) { memset(&c->key, 0, sizeof c->key); c->key.glsl = 1; }
            c->ref = maxwell_translate(c->code, c->len, c->stage, &c->key);
            bool fetch = c->ref->msl && (strstr(c->ref->msl, ", rscale[") != NULL);
            if (!hunt || fetch) { fetches += fetch; break; }
            tries++;
            maxwell_free(c->ref);
        }
        ok += c->ref->msl != NULL;
    }
    pthread_t th[64];
    for (int t = 0; t < threads; t++) pthread_create(&th[t], NULL, worker, (void *)(uintptr_t)(t + 1));
    go = 1;
    for (int t = 0; t < threads; t++) pthread_join(th[t], NULL);
    printf("translator on %d threads: %ld programs (%ld translate, %ld with scaled texel fetches), %d rounds, %ld translations, %ld differ from one thread: %s\n",
           threads, ncases, ok, fetches, rounds, (long)translated, (long)mismatches, mismatches ? "FAIL" : "PASS");
    return mismatches ? 1 : 0;
}
