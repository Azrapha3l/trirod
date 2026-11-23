// trbuild: Trirod's headless game builder, started by Trirod-Launcher.exe (docs/RELEASE-WORKFLOW.md). It owns the
// work on the player's own game files that the old start screen did in-process: finding and checking them, extracting
// ExeFS and the RomFS image, reading the shader archives into the portable catalog, and building the game code
// (runtime/gamebuild.c: trgen + zig cc into Trirod.dll and the runtime-only Trirod.exe). Progress goes out as the
// protocol of runtime/tools/protocol.h (docs/TOOL-PROTOCOL.md); diagnostics go to stderr.
//
//   trbuild check --source <folder | .xci/.nsp/.nca> [--data <root>]
//   trbuild files --source <...> [--data <root>] [--force]    stage "files": Preparing game files
//   trbuild code  [--data <root>] [--jobs <n>]                stages "translate" and "compile"
//   trbuild --version
//
// The data root holds extracted/ (game files, Trirod.dll, .gamebuild/), Trirod.exe, saves and caches. The source is
// only read. Exit status: 0 done, 1 failed (an @@error line says why), 2 bad arguments.
#include <SDL3/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../gamebuild.h"
#include "../gamelib.h"
#include "../nxpkg.h"
#include "../paths.h"
#include "../platform.h"
#include "../romfs_build.h"
#include "protocol.h"

#ifndef TRIROD_VERSION
#define TRIROD_VERSION "dev"
#endif
#ifndef GAMEBUILD_ID
#define GAMEBUILD_ID GAMELIB_ID
#endif

#define TITLE_ID 0x01008CF01BAAC000ull
// Echoes of Wisdom v1.0.0 main (docs/STATUS.md): the only version the patches and HLE addresses are for
static const uint8_t BUILD_ID[8] = {0xa5, 0x81, 0x40, 0x6b, 0xc9, 0x85, 0x86, 0x51};
static const char *const GAME_DIR = "extracted";

// ---- the crash hooks runtime/platform/windows.c calls into the runtime (dispatch.c): no guest code runs here
bool rt_fault_watch(uint64_t fault_addr, uint64_t host_pc) { (void)fault_addr, (void)host_pc; return false; }
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset) { (void)pc, (void)offset; return NULL; }
void rt_fault_report(const char *what, uint64_t fault_addr, uint64_t host_pc, uint64_t host_lr) {
    fprintf(stderr, "trbuild: %s at %016llx (pc %016llx, lr %016llx)\n", what, (unsigned long long)fault_addr,
            (unsigned long long)host_pc, (unsigned long long)host_lr);
    tp_event("error trbuild crashed (%s)", what);
    fflush(stdout);
    _exit(1);
}

static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}
static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool build_id_ok(const char *main_path) {
    FILE *f = fopen(main_path, "rb");
    if (!f) return false;
    uint8_t id[8] = {0};
    fseek(f, 0x40, SEEK_SET);
    size_t n = fread(id, 1, 8, f);
    fclose(f);
    return n == 8 && !memcmp(id, BUILD_ID, 8);
}

// The extracted files (not the game code): NULL = there and the supported version, else what is wrong
static const char *extracted_state(void) {
    char p[1200];
    snprintf(p, sizeof p, "%s/exefs/main", GAME_DIR);
    if (!exists(p)) return "no extracted game files";
    if (!build_id_ok(p)) return "the extracted game files are another version (v1.0.0 is needed)";
    snprintf(p, sizeof p, "%s/romfs.bin", GAME_DIR);
    if (!exists(p)) {
        snprintf(p, sizeof p, "%s/romfs.src", GAME_DIR);
        return exists(p) ? "the game data still needs local extraction" : "the game data image (romfs.bin) is missing";
    }
    return NULL;
}

// ------------------------------------------------------------------ finding the game in what the player chose
typedef struct {
    char exefs[1100], romfs[1100], package[1100], keys[1100];
    bool compressed;  // a .nsz / .xcz was there (those can't be read)
} Found;

static void lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

// The same rules the start screen used (runtime/ui/launcher.mm find_game): a folder with exefs files (main and
// main.npdm) and a romfs folder, or the biggest .xci / .nsp / .nca (the base game, not an update or DLC) with the
// player's prod.keys beside it; a few levels down.
static void find_game(const char *dir, int depth, Found *g) {
    if (depth > 4 || (g->exefs[0] && g->romfs[0])) return;
    char p[1200], q[1200];
    snprintf(p, sizeof p, "%s/main", dir);
    snprintf(q, sizeof q, "%s/main.npdm", dir);
    if (!g->exefs[0] && exists(p) && exists(q)) snprintf(g->exefs, sizeof g->exefs, "%s", dir);
    DIR *d = opendir(dir);
    if (!d) return;
    char *sub[64];
    int nsub = 0;
    uint64_t best = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char name[512];
        snprintf(name, sizeof name, "%s", e->d_name);
        lower(name);
        size_t len = strlen(name);
        const char *ext = len > 4 ? name + len - 4 : "";
        char full[1200];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st)) continue;
        if (!strcmp(ext, ".nsz") || !strcmp(ext, ".xcz")) g->compressed = true;
        if ((!strcmp(ext, ".xci") || !strcmp(ext, ".nsp") || !strcmp(ext, ".nca")) && !S_ISDIR(st.st_mode) &&
            (uint64_t)st.st_size > best && !g->package[0]) {
            best = (uint64_t)st.st_size;
            snprintf(g->package, sizeof g->package, "%s", full);
        }
        if (!strcmp(name, "prod.keys") && !g->keys[0]) snprintf(g->keys, sizeof g->keys, "%s", full);
        if (S_ISDIR(st.st_mode)) {
            if (!strcmp(name, "romfs") && !g->romfs[0]) snprintf(g->romfs, sizeof g->romfs, "%s", full);
            else if (nsub < 64) sub[nsub++] = strdup(full);
        }
    }
    closedir(d);
    for (int i = 0; i < nsub; i++)
        for (int j = i + 1; j < nsub; j++)
            if (strcmp(sub[j], sub[i]) < 0) { char *t = sub[i]; sub[i] = sub[j]; sub[j] = t; }
    for (int i = 0; i < nsub; i++) { find_game(sub[i], depth + 1, g); free(sub[i]); }
}

// The source as given: a folder (searched), or a package file itself (its prod.keys beside it)
static void locate(const char *source, Found *g) {
    memset(g, 0, sizeof *g);
    if (is_dir(source)) { find_game(source, 0, g); return; }
    if (!exists(source)) return;
    snprintf(g->package, sizeof g->package, "%s", source);
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", source);
    char *s = strrchr(dir, '/'), *b = strrchr(dir, '\\');
    if (b > s) s = b;
    if (s) *s = 0;
    else snprintf(dir, sizeof dir, ".");
    char keys[1200];
    snprintf(keys, sizeof keys, "%s/prod.keys", dir);
    if (exists(keys)) snprintf(g->keys, sizeof g->keys, "%s", keys);
}

// NULL = usable (desc says what was found), else why not
static const char *judge(const Found *g, char *desc, size_t n) {
    if (g->exefs[0] && g->romfs[0]) {
        char main[1200];
        snprintf(main, sizeof main, "%s/main", g->exefs);
        if (!build_id_ok(main)) return "this is not version 1.0.0 of the game (TriRod needs v1.0.0 without updates)";
        snprintf(desc, n, "extracted game files: %s and %s (version 1.0.0)", g->exefs, g->romfs);
        return NULL;
    }
    if (g->package[0]) {
        if (!g->keys[0]) return "no prod.keys beside the game package: put the keys file from your own Switch next to it";
        // (the title and build id are inside the encrypted package: checked while preparing the files)
        snprintf(desc, n, "game package %s with %s (the version is checked while the files are prepared)", g->package,
                 g->keys);
        return NULL;
    }
    if (g->compressed) return "compressed packages (.nsz / .xcz) can't be read: decompress them to .nsp / .xci first";
    return "no game files found there: choose the folder (or the .xci / .nsp) that holds your copy of the game";
}

// ------------------------------------------------------------------ progress of the file steps
static double g_span0, g_span1;  // the part of the step's bar the current operation fills
static const char *g_what;
static void bytes_progress(uint64_t done, uint64_t total) {
    if (total) tp_progress(g_span0 + (g_span1 - g_span0) * (double)done / (double)total, g_what);
}
static void span(double a, double b, const char *what) {
    g_span0 = a, g_span1 = b, g_what = what;
    tp_progress(a, what);
}

static void tool_event(int kind, const char *const *argv, const char *log, int exit);

static int cmd_check(const char *source) {
    char desc[2600];
    if (!source || !*source) {
        const char *bad = extracted_state();
        if (bad) { tp_event("check bad %s", bad); return 1; }
        tp_event("check ok extracted game files in %s/%s (version 1.0.0)", rt_data_root(), GAME_DIR);
        return 0;
    }
    Found g;
    locate(source, &g);
    const char *bad = judge(&g, desc, sizeof desc);
    if (bad) { tp_event("check bad %s", bad); return 1; }
    tp_event("check ok %s", desc);
    return 0;
}

static int fail(const char *why) {
    tp_event("error %s", why);
    return 1;
}

static int cmd_files(const char *source, bool force) {
    tp_stage("files", "Preparing game files");
    mkdir(GAME_DIR, 0755);
    const char *state = extracted_state();
    char why[512] = "";
    if (!state && !force) {
        fprintf(stderr, "[trbuild] %s/%s already holds the v1.0.0 files: reusing them (--force extracts again)\n",
                rt_data_root(), GAME_DIR);
        tp_event("info Game files already prepared in %s/%s", rt_data_root(), GAME_DIR);
    } else if (state && !strcmp(state, "the game data still needs local extraction") && !force) {
        span(0, 0.85, "Extracting the game data");
        char src[1200], img[1200];
        snprintf(src, sizeof src, "%s/romfs.src", GAME_DIR);
        snprintf(img, sizeof img, "%s/romfs.bin", GAME_DIR);
        if (!nx_romfs_extract(src, img, bytes_progress, why, sizeof why)) return fail(why);
    } else {
        if (!source || !*source) return fail(state ? state : "no game files were chosen");
        Found g;
        locate(source, &g);
        char desc[2600];
        const char *bad = judge(&g, desc, sizeof desc);
        if (bad) return fail(bad);
        fprintf(stderr, "[trbuild] source: %s\n", desc);
        if (!rt_remove_game_files(GAME_DIR)) return fail("the earlier game files in extracted/ could not be removed");
        mkdir(GAME_DIR, 0755);
        if (g.exefs[0] && g.romfs[0]) {
            // (the chosen folder can't be inside extracted/: setting up again deletes that first)
            char a[1100], b[1100];
            if (!_fullpath(a, g.exefs, sizeof a) || !_fullpath(b, GAME_DIR, sizeof b) || !_strnicmp(a, b, strlen(b)))
                return fail("choose your own copy of the game files, not TriRod's extracted folder");
            span(0, 0.45, "Copying the game files");
            uint64_t bytes = romfs_dir_bytes(g.romfs);
            char exefs[1200], romfs[1200], img[1200];
            snprintf(exefs, sizeof exefs, "%s/exefs", GAME_DIR);
            snprintf(romfs, sizeof romfs, "%s/romfs", GAME_DIR);
            snprintf(img, sizeof img, "%s/romfs.bin", GAME_DIR);
            fprintf(stderr, "[trbuild] copying %s and %s (%llu bytes of data)\n", g.exefs, g.romfs, (unsigned long long)bytes);
            if (!romfs_copy_tree(g.exefs, exefs, NULL) || !romfs_copy_tree(g.romfs, romfs, bytes_progress))
                return fail("the game files couldn't be copied (about 12 GB must be free)");
            span(0.45, 0.85, "Building the game data image");
            if (!romfs_build(romfs, img, bytes_progress, why, sizeof why)) return fail(why);
        } else {
            span(0, 0.85, "Extracting the game files from the package");
            uint64_t tid = 0;
            fprintf(stderr, "[trbuild] extracting %s with %s\n", g.package, g.keys);
            if (!nx_setup(g.package, g.keys, GAME_DIR, &tid, bytes_progress, why, sizeof why)) return fail(why);
            char main[1200];
            snprintf(main, sizeof main, "%s/exefs/main", GAME_DIR);
            if (tid != TITLE_ID || !build_id_ok(main)) {
                fprintf(stderr, "[trbuild] title %016llx, main build id %s\n", (unsigned long long)tid,
                        build_id_ok(main) ? "v1.0.0" : "another version");
                rt_remove_game_files(GAME_DIR);
                return fail("this is not version 1.0.0 of The Legend of Zelda: Echoes of Wisdom (an update or "
                            "another game); TriRod needs v1.0.0 without updates");
            }
        }
        state = extracted_state();
        if (state) return fail(state);
    }
    // The shader archives' Maxwell records: a copy for the shader preparation tool, nothing translated here
    span(0.85, 0.99, "Reading the game's shader archives");
    GameBuildOpts o = {.tool = tool_event};
    if (!gamebuild_shader_catalog(GAME_DIR, &o, why, sizeof why)) return fail(why);
    tp_progress(1, "Game files ready");
    tp_event("info Game files ready: ExeFS, RomFS image and shader catalog in %s/%s", rt_data_root(), GAME_DIR);
    return 0;
}

// ------------------------------------------------------------------ the game code
static int g_phase = -1;
static int g_compiles, g_compiles_done, g_compile_fails;

static const char *kind_name(int kind) {
    switch (kind) {
    case GAMEBUILD_TOOL_GENERATOR: return "trgen";
    case GAMEBUILD_TOOL_COMPILER: return "cc";
    case GAMEBUILD_TOOL_LINKER: return "link";
    default: return "shader-catalog";
    }
}

// Every child of the build: the generator, catalog and links as tool events with their command lines; the
// thousands of compiles only as a count (the first command line in full), and a failed one with its log.
static void tool_event(int kind, const char *const *argv, const char *log, int exit) {
    static char line[4096];
    if (exit < 0) {
        tp_join(argv, line, sizeof line);
        if (kind == GAMEBUILD_TOOL_COMPILER) {
            if (!g_compiles++) tp_event("tool cc start %s", line);
            return;
        }
        tp_event("tool %s start %s", kind_name(kind), line);
        fprintf(stderr, "[trbuild] %s: output in %s\n", kind_name(kind), log);
        return;
    }
    if (kind == GAMEBUILD_TOOL_COMPILER) {
        g_compiles_done++;
        if (exit) g_compile_fails++, tp_log_tail(log, 40);
        return;
    }
    tp_event("tool %s end %d %s", kind_name(kind), exit, log);
    if (exit) tp_log_tail(log, 60);
}

static void phase(int ph, double f) {
    if (ph != g_phase) {
        if (ph == 0) tp_stage("translate", "Translating game code for PC");
        else {
            if (g_phase < 0) tp_stage("translate", "Translating game code for PC");
            tp_progress(1, NULL);
            tp_stage("compile", "Compiling and linking the generated code");
        }
        g_phase = ph;
    }
    char text[128];
    if (ph == 0) snprintf(text, sizeof text, "trgen: writing C for the game's modules (%d files compiled meanwhile)",
                          g_compiles_done);
    // (the bar is bytes of C compiled, gamebuild.c's measure: the biggest chunks finish last)
    else if (f < 0.9) snprintf(text, sizeof text, "zig cc: %d files compiled, %d running", g_compiles_done,
                               g_compiles - g_compiles_done);
    else snprintf(text, sizeof text, "Linking Trirod.dll and Trirod.exe");
    tp_progress(f, text);
}

static int cmd_code(int jobs) {
    const char *state = extracted_state();
    if (state) {
        tp_stage("translate", "Translating game code for PC");
        return fail(state);
    }
    if (jobs < 1) {
        int cores = SDL_GetNumLogicalCPUCores(), ram_gb = SDL_GetSystemRAM() / 1024;
        jobs = cores < ram_gb - 2 ? cores : ram_gb - 2 > 1 ? ram_gb - 2 : 1;
    }
    fprintf(stderr, "[trbuild] game code id %s, runtime build id %s, %d compiler jobs\n", GAMELIB_ID, GAMEBUILD_ID, jobs);
    GameBuildOpts o = {.jobs = jobs, .phase = phase, .tool = tool_event, .skip_shader_catalog = true};
    char why[256] = "";
    phase(0, 0);
    bool ok = gamebuild_run(GAME_DIR, &o, why, sizeof why);
    if (!ok) {
        if (g_compile_fails) fprintf(stderr, "[trbuild] %d compile(s) failed; their logs are above\n", g_compile_fails);
        char msg[600];
        snprintf(msg, sizeof msg, "%s (logs: %s/%s/.gamebuild)", why, rt_data_root(), GAME_DIR);
        return fail(msg);
    }
    tp_event("info Game code built: %s/%s/%s (id %s) and %s/Trirod.exe (id %s), %d files compiled", rt_data_root(),
             GAME_DIR, GAMELIB_NAME, GAMELIB_ID, rt_data_root(), GAMEBUILD_ID, g_compiles_done);
    return 0;
}

static int usage(void) {
    fprintf(stderr, "usage: trbuild check|files|code [--source <path>] [--data <root>] [--jobs <n>] [--force]\n"
                    "       trbuild --version\n");
    return 2;
}

int main(int argc, char **argv) {
    plat_init();
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc < 2) return usage();
    const char *cmd = argv[1], *source = NULL, *data = NULL;
    int jobs = 0;
    bool force = false;
    if (!strcmp(cmd, "--version")) {
        printf("trbuild %s game-code %s runtime %s protocol %d\n", TRIROD_VERSION, GAMELIB_ID, GAMEBUILD_ID, TOOL_PROTOCOL);
        return 0;
    }
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--source") && i + 1 < argc) source = argv[++i];
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else if (!strcmp(argv[i], "--jobs") && i + 1 < argc) jobs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--force")) force = true;
        else return usage();
    }
    // (rt_paths_init: EOWRECOMP_DATA_ROOT first; the data root becomes the working directory)
    if (data && *data) {
        char abs[1100];
        if (!_fullpath(abs, data, sizeof abs)) snprintf(abs, sizeof abs, "%s", data);
        _putenv_s("EOWRECOMP_DATA_ROOT", abs);  // (the CRT environment rt_paths_init reads)
    }
    static char src_abs[1100];  // (before the working directory changes)
    if (source && *source && _fullpath(src_abs, source, sizeof src_abs)) source = src_abs;
    tp_event("protocol %d trbuild %s", TOOL_PROTOCOL, TRIROD_VERSION);
    rt_paths_init();
    if (strcmp(cmd, "check") && !gamebuild_available()) {
        tp_event("error the game code builder (trgen, zig, link-kit) is missing beside trbuild.exe");
        return 1;
    }
    int r;
    if (!strcmp(cmd, "check")) r = cmd_check(source);
    else if (!strcmp(cmd, "files")) r = cmd_files(source, force);
    else if (!strcmp(cmd, "code")) r = cmd_code(jobs);
    else return usage();
    if (!r && strcmp(cmd, "check")) tp_event("done");
    fflush(stdout);
    return r;
}
