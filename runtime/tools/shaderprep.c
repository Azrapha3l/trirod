// Trirod-ShaderPrep: the build's last step, "Preparing shaders for the graphics device", started by
// Trirod-Launcher.exe after the game code is built (docs/RELEASE-WORKFLOW.md). Headless: no window, no game. It is
// linked from the same runtime objects as Trirod.exe, so it translates with the runtime's own Maxwell translator
// (runtime/nvn/maxwell/translate.c), compiles with its shaderc path and creates pipelines on the device the game
// will choose, into the same caches (<data root>/cache):
//
//  1. Recorded recipes (exec.c vk_warm_start): every program and pipeline that earlier game sessions on this GPU
//     built, re-translated from their cached guest code and created with their recorded render state. These are
//     complete pipelines; the driver's pipeline cache (vk_pipelines.bin) keeps them for the next start.
//  2. The portable shader catalog (extracted/shader_catalog, from the game's BNSH archives: trbuild files): each
//     vertex+fragment pair and each compute shader translated to GLSL and compiled to SPIR-V into <cache>/spirv. The
//     archives carry the shader code but not the draw state a pipeline needs (vertex formats, render-target formats,
//     blend / depth state) nor the runtime ShaderKey of each draw, so no pipelines are created from them. Pairs are
//     translated with the key a plain draw uses (float vertex inputs and colour targets, and the game's window-origin
//     / depth-mode flags as the recipes recorded them); a draw whose key differs translates again in game. Compute
//     shaders have a single key ({glsl}), so theirs match the runtime exactly.
//
// Anything not prepared here is built by the runtime when first drawn, as before (a draw waits or is skipped while
// it builds: exec.c compile_wait). The GPU caches stay on this PC: they are specific to the device and driver.
//
//   Trirod-ShaderPrep [--data <root>] [--catalog <dir>] [--no-catalog] [--no-recipes] [--jobs <n>]
//
// Exit status: 0 prepared, 1 failed, 2 bad arguments, 3 skipped (no usable Vulkan 1.3 device: left to the game).
// Progress: runtime/tools/protocol.h. Report: <cache>/shaderprep.json.
#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <vulkan/vulkan.h>

#include "../nvn/maxwell/shader.h"
#include "../nvn/vulkan/prep.h"
#include "../paths.h"
#include "../rt.h"
#include "protocol.h"

#ifndef TRIROD_VERSION
#define TRIROD_VERSION "dev"
#endif
#ifndef GAMEBUILD_ID
#define GAMEBUILD_ID "dev"
#endif

// ---- what runtime/main.c defines for the runtime objects (no game runs here)
const char *g_romfs_dir;
void rt_request_reboot(int kind) { (void)kind; }
int rt_reboot_requested(void) { return 0; }
void reboot_test_frame(uint64_t frame) { (void)frame; }

// ------------------------------------------------------------------ Vulkan present?
// The backend's own start FATALs without a driver; this asks first, so a PC without Vulkan 1.3 leaves the shaders to
// the game (which then says why) instead of failing the build.
static bool vulkan_usable(char *why, size_t n) {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    VkResult r = vkCreateInstance(&ci, NULL, &inst);
    if (r != VK_SUCCESS) { snprintf(why, n, "no Vulkan driver (vkCreateInstance %d)", r); return false; }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(inst, &count, NULL);
    VkPhysicalDevice pd[16];
    if (count > 16) count = 16;
    vkEnumeratePhysicalDevices(inst, &count, pd);
    bool ok = false;
    for (uint32_t i = 0; i < count && !ok; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        ok = p.apiVersion >= VK_API_VERSION_1_3;
    }
    vkDestroyInstance(inst, NULL);
    if (!ok) snprintf(why, n, count ? "no Vulkan 1.3 device" : "no Vulkan device");
    return ok;
}

// ------------------------------------------------------------------ the catalog (tools/shader_catalog.py)
// catalog.bin: header "EOWSCAT1", schema, stage count, program count, record sizes, payload offset; then the stage
// index (stage, control bytes, code bytes, 0, 32-byte identity, file offset of control + code), the program index
// (variation, the six stage indices vs hs ds gs ps cs, 0xffffffff = none), and the records themselves.
enum { CAT_VS, CAT_HS, CAT_DS, CAT_GS, CAT_PS, CAT_CS, CAT_NSTAGES };
#define CAT_NONE 0xffffffffu

typedef struct {
    uint8_t *data;
    size_t size;
    uint32_t nstages, nprogs, stage_rec, prog_rec;
} Catalog;

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static bool catalog_open(const char *dir, Catalog *c, char *why, size_t n) {
    memset(c, 0, sizeof *c);
    char path[1200];
    snprintf(path, sizeof path, "%s/catalog.bin", dir);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(why, n, "no shader catalog at %s", path); return false; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    c->data = size > 32 ? malloc((size_t)size) : NULL;
    bool ok = c->data && fread(c->data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok || memcmp(c->data, "EOWSCAT1", 8) || rd32(c->data + 8) != 1) {
        snprintf(why, n, "%s is not a schema-1 shader catalog", path);
        free(c->data);
        c->data = NULL;
        return false;
    }
    c->size = (size_t)size;
    c->nstages = rd32(c->data + 12);
    c->nprogs = rd32(c->data + 16);
    c->stage_rec = rd32(c->data + 20);
    c->prog_rec = rd32(c->data + 24);
    if (c->stage_rec < 56 || c->prog_rec < 28 ||
        32 + (uint64_t)c->nstages * c->stage_rec + (uint64_t)c->nprogs * c->prog_rec > c->size) {
        snprintf(why, n, "%s: damaged index", path);
        free(c->data);
        c->data = NULL;
        return false;
    }
    return true;
}

// A stage's Maxwell code (the NVN header, SPH and instructions: what maxwell_translate reads) and its stage. len is
// what the runtime lets the translator scan: up to the compiler constants the code blob carries after the program
// (control + 0x700, NVN's const offset: runtime/nvn/objects.c set_shaders, exec.c cmax) - past them the scan would
// read constants as instructions.
static const uint8_t *catalog_code(const Catalog *c, uint32_t i, uint32_t *len, uint32_t *stage) {
    if (i >= c->nstages) return NULL;
    const uint8_t *r = c->data + 32 + (size_t)i * c->stage_rec;
    uint32_t ctl = rd32(r + 4), code = rd32(r + 8);
    uint64_t off = rd64(r + 48);
    if (off + ctl + code > c->size || ctl < 0x704) return NULL;
    uint32_t const_offset = rd32(c->data + off + 0x700);
    *len = const_offset && const_offset < code ? const_offset : code;
    *stage = rd32(r);
    return c->data + off + ctl;
}

static uint32_t catalog_prog_stage(const Catalog *c, uint32_t p, int s) {
    return rd32(c->data + 32 + (size_t)c->nstages * c->stage_rec + (size_t)p * c->prog_rec + 4 + 4 * s);
}

// ------------------------------------------------------------------ translating the catalog
typedef struct { uint32_t vs, ps; } Job;  // ps == CAT_NONE: a compute shader (vs = its stage)
static Job *g_jobs;
static int g_njobs;
static atomic_int g_next, g_done, g_ok, g_translate_failed, g_spirv_failed;
static ShaderKey g_vkey, g_fkey;
static Catalog g_cat;
static pthread_mutex_t g_translate_lock = PTHREAD_MUTEX_INITIALIZER;  // (the translator keeps global state)

// Translation failures by the translator's own message (its error text), for the log: which Maxwell features the
// catalog's shaders need that translate.c lacks
#define MAX_REASONS 24
static struct { char text[160]; int count; } g_reasons[MAX_REASONS];
static int g_nreasons;
// EOWRECOMP_SHADERPREP_DUMP=<dir>: each stage that fails to translate is also written there as <stage>_<n>.bin
// (the code blob as the translator gets it), for nvdisasm / tools/test_translate
static void note_failure(const char *stage, const ShaderInfo *si, const uint8_t *code, uint32_t len) {
    static atomic_int dumped;
    const char *dir = getenv("EOWRECOMP_SHADERPREP_DUMP");
    if (dir && *dir && code) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s_%03d.bin", dir, stage, atomic_fetch_add(&dumped, 1));
        FILE *f = fopen(path, "wb");
        if (f) fwrite(code, 1, len, f), fclose(f);
    }
    char t[160];
    snprintf(t, sizeof t, "%s: %s", stage, si ? si->error : "no code in the catalog");
    pthread_mutex_lock(&g_translate_lock);
    int i = 0;
    while (i < g_nreasons && strcmp(g_reasons[i].text, t)) i++;
    if (i == g_nreasons && g_nreasons < MAX_REASONS) snprintf(g_reasons[g_nreasons++].text, sizeof g_reasons[0].text, "%s", t);
    if (i < g_nreasons) g_reasons[i].count++;
    pthread_mutex_unlock(&g_translate_lock);
}

static bool spirv(const char *src, int stage, const char *name) {
    size_t n = 0;
    uint32_t *w = vk_glsl_to_spirv(src, stage, name, &n);
    free(w);
    return w != NULL;
}

static void *translate_worker(void *arg) {
    (void)arg;
    for (int i; (i = atomic_fetch_add(&g_next, 1)) < g_njobs;) {
        const Job *j = &g_jobs[i];
        uint32_t vlen, flen, vst, fst;
        const uint8_t *vc = catalog_code(&g_cat, j->vs, &vlen, &vst);
        bool ok = false;
        if (j->ps == CAT_NONE) {
            ShaderKey k = {0};
            k.glsl = 1;  // (exec.c compute_program: the only key a compute shader has)
            pthread_mutex_lock(&g_translate_lock);
            ShaderInfo *si = vc ? maxwell_translate(vc, vlen, STAGE_COMPUTE, &k) : NULL;
            pthread_mutex_unlock(&g_translate_lock);
            if (!si || !si->msl) atomic_fetch_add(&g_translate_failed, 1), note_failure("cs", si, vc, vlen);
            else if (!(ok = spirv(si->msl, STAGE_COMPUTE, "cs"))) atomic_fetch_add(&g_spirv_failed, 1);
            if (si) maxwell_free(si);
        } else {
            const uint8_t *fc = catalog_code(&g_cat, j->ps, &flen, &fst);
            ShaderKey fk = g_fkey;
            pthread_mutex_lock(&g_translate_lock);
            ShaderInfo *vi = vc && fc ? maxwell_translate(vc, vlen, STAGE_VERTEX, &g_vkey) : NULL, *fi = NULL;
            if (vi && vi->msl) {
                fk.vs_out_mask = vi->out_mask;  // (exec.c translate_program)
                fi = maxwell_translate(fc, flen, STAGE_FRAGMENT, &fk);
            }
            pthread_mutex_unlock(&g_translate_lock);
            if (!vi || !vi->msl || !fi || !fi->msl) {
                atomic_fetch_add(&g_translate_failed, 1);
                note_failure(!vi || !vi->msl ? "vs" : "fs", !vi || !vi->msl ? vi : fi, !vi || !vi->msl ? vc : fc,
                             !vi || !vi->msl ? vlen : flen);
            } else if (!(ok = spirv(vi->msl, STAGE_VERTEX, "vs") && spirv(fi->msl, STAGE_FRAGMENT, "fs")))
                atomic_fetch_add(&g_spirv_failed, 1);
            if (vi) maxwell_free(vi);
            if (fi) maxwell_free(fi);
        }
        if (ok) atomic_fetch_add(&g_ok, 1);
        atomic_fetch_add(&g_done, 1);
    }
    return NULL;
}

static int cmp_job(const void *a, const void *b) {
    const Job *x = a, *y = b;
    return x->vs != y->vs ? (x->vs < y->vs ? -1 : 1) : x->ps != y->ps ? (x->ps < y->ps ? -1 : 1) : 0;
}

typedef struct {
    bool found;
    char why[256];
    uint32_t stages, programs;
    int pairs, compute, unsupported, incomplete;
    const char *key_source;
} CatalogReport;

static void translate_catalog(const char *dir, int jobs, const VkWarmSummary *warm, double p0, double p1,
                              CatalogReport *rep) {
    memset(rep, 0, sizeof *rep);
    if (!catalog_open(dir, &g_cat, rep->why, sizeof rep->why)) return;
    rep->found = true;
    rep->stages = g_cat.nstages;
    rep->programs = g_cat.nprogs;
    g_jobs = malloc(sizeof *g_jobs * (g_cat.nprogs ? g_cat.nprogs : 1));
    for (uint32_t p = 0; p < g_cat.nprogs; p++) {
        uint32_t s[CAT_NSTAGES];
        for (int k = 0; k < CAT_NSTAGES; k++) s[k] = catalog_prog_stage(&g_cat, p, k);
        if (s[CAT_CS] != CAT_NONE) { g_jobs[g_njobs++] = (Job){s[CAT_CS], CAT_NONE}; continue; }
        // (the Vulkan backend builds vertex + fragment programs only: exec.c Prog)
        if (s[CAT_HS] != CAT_NONE || s[CAT_DS] != CAT_NONE || s[CAT_GS] != CAT_NONE) { rep->unsupported++; continue; }
        if (s[CAT_VS] == CAT_NONE || s[CAT_PS] == CAT_NONE) { rep->incomplete++; continue; }
        g_jobs[g_njobs++] = (Job){s[CAT_VS], s[CAT_PS]};
    }
    qsort(g_jobs, (size_t)g_njobs, sizeof *g_jobs, cmp_job);
    int n = 0;
    for (int i = 0; i < g_njobs; i++)
        if (!n || cmp_job(&g_jobs[i], &g_jobs[n - 1])) g_jobs[n++] = g_jobs[i];
    g_njobs = n;
    for (int i = 0; i < n; i++) *(g_jobs[i].ps == CAT_NONE ? &rep->compute : &rep->pairs) += 1;

    // The key a plain draw gets (exec.c draw_): GLSL, float inputs and targets, flip_y = window origin 0 (lower left),
    // depth_minus_one = depth mode 0. EoW v1.0.0 sets depth mode 1 ("[nvn] depth mode 1" in its logs) and never sets
    // the window origin, so flip_y 1 and depth_minus_one 0 - unless this PC's recipes recorded otherwise.
    g_vkey = (ShaderKey){0};
    g_vkey.glsl = 1;
    g_vkey.flip_y = 1;
    g_vkey.depth_minus_one = 0;
    rep->key_source = "game defaults";
    if (warm->found && warm->recipes) {
        g_vkey.flip_y = warm->flip_y[1] >= warm->flip_y[0];
        g_vkey.depth_minus_one = warm->depth_minus_one[1] > warm->depth_minus_one[0];
        rep->key_source = "this PC's recipes";
    }
    g_fkey = (ShaderKey){0};
    g_fkey.glsl = 1;
    g_fkey.flip_y = g_vkey.flip_y;
    fprintf(stderr, "[shaderprep] catalog %s: %u stages, %u program variations -> %d vertex+fragment pairs, %d compute "
                    "shaders (%d with geometry/tessellation stages, %d without a fragment stage: skipped); key flip_y %d "
                    "depth_minus_one %d (%s); %d threads\n",
            dir, g_cat.nstages, g_cat.nprogs, rep->pairs, rep->compute, rep->unsupported, rep->incomplete, g_vkey.flip_y,
            g_vkey.depth_minus_one, rep->key_source, jobs);
    maxwell_gmem_base = HEAP_BASE;  // (exec.c translate_program: the GLSL addresses global memory from here)
    maxwell_gmem_size = HEAP_MAX;
    pthread_t th[64];
    if (jobs > 64) jobs = 64;
    for (int i = 0; i < jobs; i++) pthread_create(&th[i], NULL, translate_worker, NULL);
    while (atomic_load(&g_done) < g_njobs) {
        char text[160];
        snprintf(text, sizeof text, "Translating the game's shaders to SPIR-V: %d of %d", atomic_load(&g_done), g_njobs);
        tp_progress(p0 + (p1 - p0) * (g_njobs ? (double)atomic_load(&g_done) / g_njobs : 1), text);
        SDL_Delay(100);
    }
    for (int i = 0; i < jobs; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < g_nreasons; i++)
        fprintf(stderr, "[shaderprep] not translated (%d): %s\n", g_reasons[i].count, g_reasons[i].text);
}

// ------------------------------------------------------------------ report
static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fprintf(f, "\\%c", *s);
        else if ((unsigned char)*s < 0x20) fprintf(f, "\\u%04x", *s);
        else fputc(*s, f);
    }
    fputc('"', f);
}

static void write_report(const char *status, const char *reason, const char *dev, uint32_t vendor, uint32_t device,
                         uint32_t driver, const VkWarmSummary *w, const CatalogReport *c, double seconds) {
    char path[700], tmp[720];
    snprintf(path, sizeof path, "%s/shaderprep.json", vk_cache_dir());
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    fprintf(f, "{\n  \"format\": \"trirod-shaderprep\",\n  \"schema\": 1,\n  \"status\": ");
    json_str(f, status);
    fprintf(f, ",\n  \"reason\": ");
    json_str(f, reason ? reason : "");
    fprintf(f, ",\n  \"tool_version\": ");
    json_str(f, TRIROD_VERSION);
    fprintf(f, ",\n  \"runtime_build\": ");
    json_str(f, GAMEBUILD_ID);
    fprintf(f, ",\n  \"seconds\": %.1f,\n  \"device\": {\"name\": ", seconds);
    json_str(f, dev ? dev : "");
    fprintf(f, ", \"vendor\": %u, \"device\": %u, \"driver\": %u},\n", vendor, device, driver);
    fprintf(f, "  \"recipes\": {\"found\": %s, \"why\": ", w && w->found ? "true" : "false");
    json_str(f, w ? w->why : "");
    fprintf(f, ", \"read\": %u, \"programs\": %d, \"programs_ok\": %d, \"pipelines\": %d, \"pipelines_ok\": %d, "
               "\"missing_code\": %d, \"invalid\": %d},\n",
            w ? w->recipes : 0, w ? w->programs : 0, w ? w->programs_ok : 0, w ? w->pipelines : 0,
            w ? w->pipelines_ok : 0, w ? w->missing_code : 0, w ? w->invalid : 0);
    fprintf(f, "  \"catalog\": {\"found\": %s, \"why\": ", c && c->found ? "true" : "false");
    json_str(f, c ? c->why : "");
    fprintf(f, ", \"stages\": %u, \"program_variations\": %u, \"graphics_pairs\": %d, \"compute\": %d, "
               "\"skipped_geometry_tessellation\": %d, \"skipped_incomplete\": %d, \"spirv_ok\": %d, "
               "\"translate_failed\": %d, \"spirv_failed\": %d, \"pipelines_created\": 0, \"key_source\": ",
            c ? c->stages : 0, c ? c->programs : 0, c ? c->pairs : 0, c ? c->compute : 0, c ? c->unsupported : 0,
            c ? c->incomplete : 0, atomic_load(&g_ok), atomic_load(&g_translate_failed), atomic_load(&g_spirv_failed));
    json_str(f, c && c->key_source ? c->key_source : "");
    fprintf(f, "}\n}\n");
    bool ok = fclose(f) == 0;
    if (ok) { remove(path); rename(tmp, path); }
    else remove(tmp);
}

static int usage(void) {
    fprintf(stderr, "usage: Trirod-ShaderPrep [--data <root>] [--catalog <dir>] [--no-catalog] [--no-recipes] [--jobs <n>]\n");
    return 2;
}

int main(int argc, char **argv) {
    plat_init();
    setvbuf(stderr, NULL, _IONBF, 0);
    const char *data = NULL, *catalog = "extracted/shader_catalog";
    bool use_catalog = true, use_recipes = true;
    int jobs = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else if (!strcmp(argv[i], "--catalog") && i + 1 < argc) catalog = argv[++i];
        else if (!strcmp(argv[i], "--no-catalog")) use_catalog = false;
        else if (!strcmp(argv[i], "--no-recipes")) use_recipes = false;
        else if (!strcmp(argv[i], "--jobs") && i + 1 < argc) jobs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--version")) {
            printf("Trirod-ShaderPrep %s runtime %s protocol %d\n", TRIROD_VERSION, GAMEBUILD_ID, TOOL_PROTOCOL);
            return 0;
        } else return usage();
    }
    static char catalog_abs[1100];  // (before the working directory becomes the data root)
    if (catalog && strcmp(catalog, "extracted/shader_catalog") && _fullpath(catalog_abs, catalog, sizeof catalog_abs))
        catalog = catalog_abs;
    if (data && *data) {
        char abs[1100];
        if (!_fullpath(abs, data, sizeof abs)) snprintf(abs, sizeof abs, "%s", data);
        _putenv_s("EOWRECOMP_DATA_ROOT", abs);  // (the CRT environment rt_paths_init reads)
    }
    if (jobs < 1) jobs = SDL_GetNumLogicalCPUCores() > 2 ? SDL_GetNumLogicalCPUCores() - 1 : 1;
    tp_event("protocol %d Trirod-ShaderPrep %s", TOOL_PROTOCOL, TRIROD_VERSION);
    rt_paths_init();
    tp_stage("shaders", "Preparing shaders for the graphics device");
    uint64_t t0 = SDL_GetTicksNS();
    char why[160];
    if (!vulkan_usable(why, sizeof why)) {
        write_report("skipped", why, NULL, 0, 0, 0, NULL, NULL, 0);
        tp_event("skip %s: the game builds its shaders when first drawn", why);
        return 3;
    }
    tp_progress(0.01, "Opening the graphics device");
    vk_headless_init();
    char dev[256];
    uint32_t vendor, device, driver, api;
    vk_headless_device(dev, sizeof dev, &vendor, &device, &driver, &api);
    tp_event("info Graphics device: %s (vendor 0x%04x, device 0x%04x, driver 0x%08x, Vulkan %u.%u)", dev, vendor, device,
             driver, VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api));

    // 1. the recorded recipes: whole pipelines
    VkWarmSummary warm = {0};
    if (use_recipes) {
        vk_warm_start();
        int done = 0, total = 0, last_done = -1;
        uint64_t last_change = SDL_GetTicksNS();
        while (vk_warm_progress(&done, &total)) {
            if (done != last_done) last_done = done, last_change = SDL_GetTicksNS();
            // (a driver that stops answering: what was built is saved, the rest is left to the game)
            if (SDL_GetTicksNS() - last_change > 600000000000ull) {
                tp_event("warn the recorded pipelines stopped progressing for 10 minutes; the rest is left to the game");
                break;
            }
            char text[160];
            snprintf(text, sizeof text, "Building recorded pipelines: %d of %d", done, total);
            tp_progress(0.02 + 0.58 * (total ? (double)done / total : 1), text);
            SDL_Delay(100);
        }
        vk_warm_summary(&warm);
        if (warm.found)
            tp_event("info Recorded on this PC: %d of %d programs and %d of %d pipelines built (%d recipes without "
                     "their cached code, %d invalid)", warm.programs_ok, warm.programs, warm.pipelines_ok, warm.pipelines,
                     warm.missing_code, warm.invalid);
        else
            tp_event("info No recorded pipelines to build (%s): the game records them while it runs", warm.why);
    }

    // 2. the game's shader catalog: SPIR-V only
    CatalogReport cat = {0};
    if (use_catalog) {
        translate_catalog(catalog, jobs, &warm, 0.6, 0.97, &cat);
        if (cat.found)
            tp_event("info Game shader catalog: %d of %d translated to SPIR-V (%d vertex+fragment pairs, %d compute; "
                     "%d translation and %d SPIR-V failures); no pipelines are created from it",
                     atomic_load(&g_ok), g_njobs, cat.pairs, cat.compute, atomic_load(&g_translate_failed),
                     atomic_load(&g_spirv_failed));
        else tp_event("warn %s: only recorded pipelines were prepared", cat.why);
        if (cat.unsupported)
            tp_event("info %d catalog programs have geometry/tessellation stages, which the Vulkan backend does not "
                     "build: skipped", cat.unsupported);
    }

    tp_progress(0.98, "Saving the pipeline cache");
    vk_device_idle();
    vk_pipeline_cache_save();
    double secs = (double)(SDL_GetTicksNS() - t0) / 1e9;
    write_report("prepared", "", dev, vendor, device, driver, &warm, &cat, secs);
    tp_progress(1, "Shaders prepared");
    tp_event("info Saved in %s/%s (vk_pipelines.bin, spirv/, shaderprep.json) in %.0f s; anything else is built by "
             "the game when first drawn", rt_data_root(), vk_cache_dir(), secs);
    tp_event("done");
    fflush(stdout);
    fflush(stderr);
    plat_terminate();  // (as the game's exit: no DLL teardown waiting on driver threads)
    return 0;
}
