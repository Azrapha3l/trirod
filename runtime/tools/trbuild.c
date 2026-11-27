// trbuild: Trirod's headless game builder, started by Trirod-Launcher.exe (docs/RELEASE-WORKFLOW.md). It owns the
// work on the player's own game files that the old start screen did in-process: finding and checking them, extracting
// ExeFS and the RomFS image, reading the shader archives into the portable catalog, and building the game code
// (runtime/gamebuild.c: trgen + zig cc into Trirod.dll and the runtime-only Trirod.exe). Progress goes out as the
// protocol of runtime/tools/protocol.h (docs/TOOL-PROTOCOL.md); diagnostics go to stderr.
//
//   trbuild check --source <folder | .xci/.nsp/.nca> [--update <.nsp>] [--keys <prod.keys>] [--data <root>]
//   trbuild files --source <...> [--update <.nsp>] [--keys <prod.keys>] [--data <root>] [--force]
//                                                             stage "files": Preparing game files
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
#include "import_state.h"
#include "protocol.h"

#ifndef TRIROD_VERSION
#define TRIROD_VERSION "dev"
#endif
#ifndef GAMEBUILD_ID
#define GAMEBUILD_ID GAMELIB_ID
#endif

#define TITLE_ID 0x01008CF01BAAC000ull
// The game versions this build translates, by their ExeFS main's build ID (the NSO module ID's first 8 bytes): what a
// package is comes from its contents (nx_identify), never its file name. Later entries are newer; with several
// updates in a folder the newest supported one is used. v1.0.2 (an update) isn't listed: no v1.0.2 package has been
// read to learn its main's build ID, so such an update is reported by its build ID and not imported.
typedef struct {
    uint8_t id[8];
    const char *version;
    bool update;  // (only an update's ExeFS has it: a base package is v1.0.0)
} GameVersion;
static const GameVersion VERSIONS[] = {
    {{0xa5, 0x81, 0x40, 0x6b, 0xc9, 0x85, 0x86, 0x51}, "v1.0.0", false},
    {{0x9f, 0x22, 0x1e, 0x1a, 0xfa, 0x65, 0xba, 0x54}, "v1.1.0", true},
};
#define NVERSIONS ((int)(sizeof VERSIONS / sizeof *VERSIONS))
static const char *const SUPPORTED = "v1.0.0, or v1.1.0 with its update";
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

static int version_index(const uint8_t id[8]) {
    for (int i = 0; i < NVERSIONS; i++)
        if (!memcmp(id, VERSIONS[i].id, 8)) return i;
    return -1;
}
static void id_hex(const uint8_t id[8], char out[17]) {
    for (int i = 0; i < 8; i++) snprintf(out + 2 * i, 3, "%02x", id[i]);
}

// An extracted main's build ID (hex) and version (-1: not one this build supports); false = no readable main
static bool main_id(const char *main_path, char hex[17], int *ver) {
    hex[0] = 0, *ver = -1;
    FILE *f = fopen(main_path, "rb");
    if (!f) return false;
    uint8_t id[8] = {0};
    fseek(f, 0x40, SEEK_SET);
    size_t n = fread(id, 1, 8, f);
    fclose(f);
    if (n != 8) return false;
    id_hex(id, hex);
    *ver = version_index(id);
    return true;
}

static const char *game_version(const char *main_path) {
    char hex[17];
    int ver;
    return main_id(main_path, hex, &ver) && ver >= 0 ? VERSIONS[ver].version : NULL;
}

#define NEEDS_LOCAL_EXTRACTION "the game data still needs local extraction"

// The extracted files (not the game code): NULL = there and the supported version, else what is wrong
static const char *extracted_state(void) {
    char p[1200];
    snprintf(p, sizeof p, "%s/exefs/main", GAME_DIR);
    if (!exists(p)) return "no extracted game files";
    if (!game_version(p)) return "the extracted game files are not a supported game version (v1.0.0 or v1.1.0)";
    snprintf(p, sizeof p, "%s/romfs.bin", GAME_DIR);
    if (!exists(p)) {
        snprintf(p, sizeof p, "%s/romfs.src", GAME_DIR);
        return exists(p) ? NEEDS_LOCAL_EXTRACTION : "the game data image (romfs.bin) is missing";
    }
    return NULL;
}

// ------------------------------------------------------------------ finding the game in what the player chose
#define MAX_PACKAGES 16
typedef struct {
    char exefs[1100], romfs[1100], keys[1100];
    char pkg[MAX_PACKAGES][1100];  // every .xci / .nsp / .nca found (what each is comes from classify)
    int npkg;
    bool compressed;              // a .nsz / .xcz was there (those can't be read)
    char update_arg[1100];        // --update: the update the player chose explicitly
    // classify(): the base and update by content, and their versions (VERSIONS index)
    char base[1100], update[1100];
    int base_ver, update_ver;
} Found;

static void lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

static void add_package(Found *g, const char *path) {
    for (int i = 0; i < g->npkg; i++)
        if (import_path_same(g->pkg[i], path)) return;
    if (g->npkg < MAX_PACKAGES) snprintf(g->pkg[g->npkg++], sizeof g->pkg[0], "%s", path);
}

// The same rules the start screen used (runtime/ui/launcher.mm find_game): a folder with exefs files (main and
// main.npdm) and a romfs folder, or the packages (.xci / .nsp / .nca) with the player's prod.keys beside them; a few
// levels down. Which package is the base and which an update is decided by classify, from their contents.
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
        if ((!strcmp(ext, ".xci") || !strcmp(ext, ".nsp") || !strcmp(ext, ".nca")) && !S_ISDIR(st.st_mode))
            add_package(g, full);
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

static void keys_beside(Found *g, const char *file) {
    if (g->keys[0]) return;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", file);
    char *s = strrchr(dir, '/'), *b = strrchr(dir, '\\');
    if (b > s) s = b;
    if (s) *s = 0;
    else snprintf(dir, sizeof dir, ".");
    char keys[1200];
    snprintf(keys, sizeof keys, "%s/prod.keys", dir);
    if (exists(keys)) snprintf(g->keys, sizeof g->keys, "%s", keys);
}

// The source as given: a folder (searched), or a package file itself (its prod.keys beside it); plus the update the
// player chose separately, if any. --keys names the player's keys file wherever it is (RecompLauncher passes its own
// copy of the shared switch.prod-keys); then no prod.keys beside the packages is looked for.
static const char *g_keys_arg;
static void locate(const char *source, const char *update, Found *g) {
    memset(g, 0, sizeof *g);
    g->base_ver = g->update_ver = -1;
    if (g_keys_arg && *g_keys_arg) snprintf(g->keys, sizeof g->keys, "%s", g_keys_arg);
    if (is_dir(source)) find_game(source, 0, g);
    else if (exists(source)) {
        add_package(g, source);
        keys_beside(g, source);
    }
    if (update && *update) {
        snprintf(g->update_arg, sizeof g->update_arg, "%s", update);
        if (exists(update)) add_package(g, update);
        keys_beside(g, update);
    }
}

static const char *file_name(const char *p) {
    const char *s = strrchr(p, '/'), *b = strrchr(p, '\\');
    return (b > s ? b : s) ? (b > s ? b : s) + 1 : p;
}

// Which package is the base and which the update, from their program NCAs (nx_identify); false = no usable base
// (why). With several updates the newest supported one is used, unless the player chose one (--update).
static bool classify(Found *g, char *why, size_t n) {
    char unknown_update[1300] = "", unknown_base[1300] = "", first_error[600] = "", update_error[600] = "";
    bool other_title = false, any_update = false;
    for (int i = 0; i < g->npkg; i++) {
        NxPackageInfo info;
        char err[256];
        bool chosen = g->update_arg[0] && import_path_same(g->pkg[i], g->update_arg);
        if (!nx_identify(g->pkg[i], g->keys, &info, err, sizeof err)) {
            fprintf(stderr, "[trbuild] %s: %s\n", g->pkg[i], err);
            if (!first_error[0]) snprintf(first_error, sizeof first_error, "%s: %s", file_name(g->pkg[i]), err);
            if (chosen) snprintf(update_error, sizeof update_error, "the chosen update %s can't be read: %s", file_name(g->pkg[i]), err);
            continue;
        }
        char hex[17];
        id_hex(info.main_build_id, hex);
        int ver = version_index(info.main_build_id);
        fprintf(stderr, "[trbuild] %s: title %016llx, %s, main build id %s (%s)\n", g->pkg[i],
                (unsigned long long)info.title_id, info.is_patch ? "update" : "base program", hex,
                ver >= 0 ? VERSIONS[ver].version : "not a supported version");
        if (info.title_id != TITLE_ID) {
            other_title = true;
            if (chosen) snprintf(update_error, sizeof update_error, "the chosen update %s isn't for Echoes of Wisdom", file_name(g->pkg[i]));
            continue;
        }
        if (info.is_patch) {
            any_update = true;
            if (ver < 0 || !VERSIONS[ver].update) {
                snprintf(unknown_update, sizeof unknown_update, "%s (main build id %s)", file_name(g->pkg[i]), hex);
                if (chosen) snprintf(update_error, sizeof update_error, "the chosen update %s has main build id %s, a version "
                                     "this build doesn't support (it supports %s)", file_name(g->pkg[i]), hex, SUPPORTED);
                continue;
            }
            if (g->update_arg[0] ? chosen : ver > g->update_ver) {
                snprintf(g->update, sizeof g->update, "%s", g->pkg[i]);
                g->update_ver = ver;
            }
        } else if (ver >= 0 && !VERSIONS[ver].update) {
            if (g->base_ver < 0) snprintf(g->base, sizeof g->base, "%s", g->pkg[i]), g->base_ver = ver;
        } else {
            snprintf(unknown_base, sizeof unknown_base, "%s (main build id %s)", file_name(g->pkg[i]), hex);
        }
    }
    if (g->update_arg[0] && !g->update[0]) {
        snprintf(why, n, "%s", update_error[0] ? update_error : !exists(g->update_arg) ? "the chosen update file is missing"
                                                                                      : "the chosen file isn't a game update");
        return false;
    }
    if (g->base_ver < 0) {
        if (unknown_base[0]) snprintf(why, n, "the base game package %s isn't v1.0.0, the base version this build supports", unknown_base);
        else if (any_update) snprintf(why, n, "an update needs its base game: choose the folder with both, or the base .xci / .nsp");
        else if (other_title) snprintf(why, n, "these packages aren't The Legend of Zelda: Echoes of Wisdom");
        else snprintf(why, n, "%s", first_error[0] ? first_error : "no game package could be read (check prod.keys)");
        return false;
    }
    if (!g->update[0] && unknown_update[0]) {
        snprintf(why, n, "the update %s is a version this build doesn't support (it supports %s): remove it to use v1.0.0 alone",
                 unknown_update, SUPPORTED);
        return false;
    }
    if (g->update[0] && unknown_update[0])
        fprintf(stderr, "[trbuild] ignoring the unsupported update %s: using %s\n", unknown_update, g->update);
    return true;
}

// NULL = usable (desc says what was found), else why not. Packages are identified by their contents here.
static const char *judge(Found *g, char *desc, size_t n) {
    if (g->exefs[0] && g->romfs[0]) {
        char main[1200];
        snprintf(main, sizeof main, "%s/main", g->exefs);
        const char *version = game_version(main);
        if (!version) return "these extracted game files are not a supported version (v1.0.0 or v1.1.0)";
        snprintf(desc, n, "extracted game files: %s and %s (%s)", g->exefs, g->romfs, version);
        return NULL;
    }
    if (g->npkg) {
        if (!g->keys[0]) return "no prod.keys beside the game package: put the keys file from your own Switch next to it";
        if (g_keys_arg && *g_keys_arg && !exists(g_keys_arg)) return "the keys file given with --keys is missing";
        static char why[700];
        if (!classify(g, why, sizeof why)) return why;
        if (g->update[0])
            snprintf(desc, n, "base game %s (%s) and update %s (%s)", VERSIONS[g->base_ver].version, file_name(g->base),
                     VERSIONS[g->update_ver].version, file_name(g->update));
        else snprintf(desc, n, "base game %s (%s), no update", VERSIONS[g->base_ver].version, file_name(g->base));
        return NULL;
    }
    if (g->compressed) return "compressed packages (.nsz / .xcz) can't be read: decompress them to .nsp / .xci first";
    return "no game files found there: choose the folder (or the .xci / .nsp) that holds your copy of the game";
}

// The selection as an import record: what the extraction must come from to be current
static bool selection_record(const Found *g, ImportRecord *want) {
    memset(want, 0, sizeof *want);
    if (g->exefs[0] && g->romfs[0]) {  // (a folder of extracted files: its main identifies it)
        char main[1200];
        snprintf(main, sizeof main, "%s/main", g->exefs);
        int ver;
        if (!file_stamp(main, &want->base) || !main_id(main, want->build_id, &ver) || ver < 0) return false;
        snprintf(want->version, sizeof want->version, "%s", VERSIONS[ver].version);
        return true;
    }
    int ver = g->update[0] ? g->update_ver : g->base_ver;
    if (ver < 0 || !file_stamp(g->base, &want->base) || !file_stamp(g->update, &want->update) || !file_stamp(g->keys, &want->keys))
        return false;
    id_hex(VERSIONS[ver].id, want->build_id);
    snprintf(want->version, sizeof want->version, "%s", VERSIONS[ver].version);
    return true;
}

// The extraction in the data folder, judged against the selection (want: NULL = nothing chosen)
static ExtractState extraction(const ImportRecord *want, ImportRecord *rec, bool *have_rec, bool *matches, char *why,
                               size_t n) {
    char main[1200], record[1200], hex[17] = "";
    snprintf(main, sizeof main, "%s/exefs/main", GAME_DIR);
    snprintf(record, sizeof record, "%s/%s", GAME_DIR, IMPORT_RECORD);
    int ver;
    bool have_files = main_id(main, hex, &ver);
    *have_rec = import_record_read(record, rec);
    if (*have_rec) fprintf(stderr, "[trbuild] import record: %s from %s%s%s\n", rec->version, rec->base.path,
                           rec->update.path[0] ? " + " : "", rec->update.path);
    return import_decide(have_files, extracted_state(), hex, ver >= 0 ? VERSIONS[ver].version : "",
                         *have_rec ? rec : NULL, want, matches, why, n);
}

static bool rm_tree(const char *p) { return !exists(p) || (is_dir(p) ? plat_remove_tree(p) : remove(p) == 0); }

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

// "@@extracted <none|current|stale|incomplete> <text>": what is in extracted/ against the selection (docs/TOOL-PROTOCOL.md)
static void report_extraction(ExtractState st, const char *why) { tp_event("extracted %s %s", extract_state_name(st), why); }

static int cmd_check(const char *source, const char *update) {
    char desc[2600], why[700];
    ImportRecord rec, want;
    bool have_rec, matches;
    if (!source || !*source) {
        ExtractState st = extraction(NULL, &rec, &have_rec, &matches, why, sizeof why);
        report_extraction(st, why);
        const char *bad = extracted_state();
        if (bad) { tp_event("check bad %s", bad); return 1; }
        char main[1200];
        snprintf(main, sizeof main, "%s/%s/exefs/main", rt_data_root(), GAME_DIR);
        tp_event("check ok extracted game files in %s/%s (%s)", rt_data_root(), GAME_DIR, game_version(main));
        return 0;
    }
    Found g;
    locate(source, update, &g);
    const char *bad = judge(&g, desc, sizeof desc);
    if (bad) { tp_event("check bad %s", bad); return 1; }
    if (!selection_record(&g, &want)) { tp_event("check bad the chosen files can't be read"); return 1; }
    report_extraction(extraction(&want, &rec, &have_rec, &matches, why, sizeof why), why);
    tp_event("check ok %s", desc);
    return 0;
}

static int fail(const char *why) {
    tp_event("error %s", why);
    return 1;
}

// What a new extraction replaces: the game files and what is derived from them (the shader catalog comes from the
// RomFS, so it goes with it and is read again). romfs.src (an older base-only setup's package reference) and romfs
// (a copied folder) go too when the new import has none.
static const char *const IMPORT_SET[] = {"exefs", "romfs", "romfs.bin", "romfs.src", "shader_catalog", IMPORT_RECORD};
#define IMPORT_TMP "extracted/.import"
#define IMPORT_PREV "extracted/.previous"

// Import the selection into extracted/.import, check it, then swap it in. The old extraction stays until the new one
// is complete: an import that fails (bad package, disk full, cancelled) leaves extracted/ as it was.
static int import_selection(const Found *g, const ImportRecord *want) {
    char why[700] = "", main[1200];
    if (!rm_tree(IMPORT_TMP) || !rm_tree(IMPORT_PREV)) return fail("files from an interrupted import in extracted/ couldn't be removed");
    mkdir(IMPORT_TMP, 0755);
    bool ok;
    if (g->exefs[0] && g->romfs[0]) {
        // (the chosen folder can't be inside extracted/: the swap replaces what is there)
        char a[1100], b[1100];
        if (!_fullpath(a, g->exefs, sizeof a) || !_fullpath(b, GAME_DIR, sizeof b) || !_strnicmp(a, b, strlen(b)))
            return rm_tree(IMPORT_TMP), fail("choose your own copy of the game files, not TriRod's extracted folder");
        span(0, 0.45, "Copying the game files");
        uint64_t bytes = romfs_dir_bytes(g->romfs);
        char exefs[1200], romfs[1200], img[1200];
        snprintf(exefs, sizeof exefs, "%s/exefs", IMPORT_TMP);
        snprintf(romfs, sizeof romfs, "%s/romfs", IMPORT_TMP);
        snprintf(img, sizeof img, "%s/romfs.bin", IMPORT_TMP);
        fprintf(stderr, "[trbuild] copying %s and %s (%llu bytes of data)\n", g->exefs, g->romfs, (unsigned long long)bytes);
        ok = romfs_copy_tree(g->exefs, exefs, NULL) && romfs_copy_tree(g->romfs, romfs, bytes_progress);
        if (!ok) snprintf(why, sizeof why, "the game files couldn't be copied (about 12 GB must be free)");
        if (ok) {
            span(0.45, 0.85, "Building the game data image");
            ok = romfs_build(romfs, img, bytes_progress, why, sizeof why);
        }
    } else {
        span(0, 0.85, g->update[0] ? "Extracting the game files from the base game and its update"
                                   : "Extracting the game files from the package");
        uint64_t tid = 0;
        bool used_update = false;
        fprintf(stderr, "[trbuild] extracting %s%s%s with %s into %s\n", g->base, g->update[0] ? " + " : "", g->update,
                g->keys, IMPORT_TMP);
        ok = g->update[0] ? nx_setup_update(g->base, g->update, g->keys, IMPORT_TMP, &tid, &used_update, bytes_progress,
                                            why, sizeof why)
                          : nx_setup(g->base, g->keys, IMPORT_TMP, &tid, bytes_progress, why, sizeof why);
        if (ok && (tid != TITLE_ID || (g->update[0] && !used_update))) {
            fprintf(stderr, "[trbuild] title %016llx, update used: %d\n", (unsigned long long)tid, used_update);
            snprintf(why, sizeof why, "the chosen packages didn't produce Echoes of Wisdom %s files", want->version);
            ok = false;
        }
    }
    // The new main must be the version the selection was identified as
    char hex[17] = "";
    int ver = -1;
    snprintf(main, sizeof main, "%s/exefs/main", IMPORT_TMP);
    if (ok && (!main_id(main, hex, &ver) || strcmp(hex, want->build_id))) {
        fprintf(stderr, "[trbuild] imported main build id %s, expected %s\n", hex, want->build_id);
        snprintf(why, sizeof why, "the imported game files aren't %s (main build id %s)", want->version, hex[0] ? hex : "unreadable");
        ok = false;
    }
    char record[1200];
    snprintf(record, sizeof record, "%s/%s", IMPORT_TMP, IMPORT_RECORD);
    if (ok && !import_record_write(record, want)) {
        snprintf(why, sizeof why, "couldn't write %s", record);
        ok = false;
    }
    if (!ok) {
        rm_tree(IMPORT_TMP);
        char msg[900];
        snprintf(msg, sizeof msg, "%s (the earlier extracted game files were kept)", why);
        return fail(msg);
    }
    tp_progress(0.85, "Replacing the earlier game files");
    if (!import_swap(GAME_DIR, IMPORT_TMP, IMPORT_PREV, IMPORT_SET, (int)(sizeof IMPORT_SET / sizeof *IMPORT_SET), rm_tree,
                     why, sizeof why)) {
        rm_tree(IMPORT_TMP);
        char msg[900];
        snprintf(msg, sizeof msg, "%s (the earlier extracted game files were kept)", why);
        return fail(msg);
    }
    rm_tree(IMPORT_TMP);
    fprintf(stderr, "[trbuild] imported %s into %s\n", want->version, GAME_DIR);
    return 0;
}

static int cmd_files(const char *source, const char *update, bool force) {
    tp_stage("files", "Preparing game files");
    mkdir(GAME_DIR, 0755);
    char why[700] = "";
    ImportRecord rec, want;
    bool have_rec = false, matches = false;
    if (!source || !*source) {  // nothing chosen: the files already there (the earlier behaviour)
        const char *state = extracted_state();
        if (!state) {
            tp_event("info Game files already prepared in %s/%s", rt_data_root(), GAME_DIR);
        } else if (!strcmp(state, NEEDS_LOCAL_EXTRACTION)) {
            span(0, 0.85, "Extracting the game data");
            char src[1200], img[1200];
            snprintf(src, sizeof src, "%s/romfs.src", GAME_DIR);
            snprintf(img, sizeof img, "%s/romfs.bin", GAME_DIR);
            if (!nx_romfs_extract(src, img, bytes_progress, why, sizeof why)) return fail(why);
        } else return fail(state);
    } else {
        Found g;
        locate(source, update, &g);
        char desc[2600];
        const char *bad = judge(&g, desc, sizeof desc);
        if (bad) return fail(bad);
        if (!selection_record(&g, &want)) return fail("the chosen files can't be read");
        fprintf(stderr, "[trbuild] chosen: %s\n", desc);
        tp_event("info Chosen: %s", desc);
        ExtractState st = extraction(&want, &rec, &have_rec, &matches, why, sizeof why);
        fprintf(stderr, "[trbuild] extracted files: %s (%s)\n", extract_state_name(st), why);
        const char *state = extracted_state();
        char record[1200];
        snprintf(record, sizeof record, "%s/%s", GAME_DIR, IMPORT_RECORD);
        if (st == EXTRACT_CURRENT && !force) {
            tp_event("info Game files already prepared in %s/%s: %s", rt_data_root(), GAME_DIR, why);
            if (!have_rec && !import_record_write(record, &want))  // (an extraction from before records: noted now)
                fprintf(stderr, "[trbuild] couldn't write %s\n", record);
        } else if (st == EXTRACT_INCOMPLETE && matches && !force && state && !strcmp(state, NEEDS_LOCAL_EXTRACTION)) {
            span(0, 0.85, "Extracting the game data");
            char src[1200], img[1200];
            snprintf(src, sizeof src, "%s/romfs.src", GAME_DIR);
            snprintf(img, sizeof img, "%s/romfs.bin", GAME_DIR);
            if (!nx_romfs_extract(src, img, bytes_progress, why, sizeof why)) return fail(why);
            if (!import_record_write(record, &want)) fprintf(stderr, "[trbuild] couldn't write %s\n", record);
        } else {
            tp_event("info %s: importing the chosen game files", st == EXTRACT_NONE ? "Nothing extracted yet" : why);
            int r = import_selection(&g, &want);
            if (r) return r;
        }
    }
    const char *state = extracted_state();
    if (state) return fail(state);
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
    fprintf(stderr, "usage: trbuild check|files|code [--source <path>] [--update <.nsp>] [--keys <prod.keys>] [--data <root>] [--jobs <n>] [--force]\n"
                    "       trbuild --version\n");
    return 2;
}

int main(int argc, char **argv) {
    plat_init();
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc < 2) return usage();
    const char *cmd = argv[1], *source = NULL, *update = NULL, *data = NULL;
    int jobs = 0;
    bool force = false;
    if (!strcmp(cmd, "--version")) {
        printf("trbuild %s game-code %s runtime %s protocol %d\n", TRIROD_VERSION, GAMELIB_ID, GAMEBUILD_ID, TOOL_PROTOCOL);
        return 0;
    }
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--source") && i + 1 < argc) source = argv[++i];
        else if (!strcmp(argv[i], "--update") && i + 1 < argc) update = argv[++i];
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) g_keys_arg = argv[++i];
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
    static char update_abs[1100];
    if (update && *update && _fullpath(update_abs, update, sizeof update_abs)) update = update_abs;
    static char keys_abs[1100];
    if (g_keys_arg && *g_keys_arg && _fullpath(keys_abs, g_keys_arg, sizeof keys_abs)) g_keys_arg = keys_abs;
    tp_event("protocol %d trbuild %s", TOOL_PROTOCOL, TRIROD_VERSION);
    rt_paths_init();
    if (strcmp(cmd, "check") && !gamebuild_available()) {
        tp_event("error the game code builder (trgen, zig, link-kit) is missing beside trbuild.exe");
        return 1;
    }
    int r;
    if (!strcmp(cmd, "check")) r = cmd_check(source, update);
    else if (!strcmp(cmd, "files")) r = cmd_files(source, update, force);
    else if (!strcmp(cmd, "code")) r = cmd_code(jobs);
    else return usage();
    if (!r && strcmp(cmd, "check")) tp_event("done");
    fflush(stdout);
    return r;
}
