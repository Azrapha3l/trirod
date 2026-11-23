// Builds the game code library on the player's machine (runtime/gamebuild.h). Phones: the same, with the generator's
// Python and zig's clang carried in the APK (docs/BUILD-android.md, tools/android/ondevice/; after
// LASRecomp-Android-Plus, MIT).
#if defined(__ANDROID__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE  // (sched_setaffinity, cpu_set_t)
#endif
#include "gamebuild.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "gamelib.h"
#include "paths.h"
#include "platform.h"
#include "rt.h"
#ifdef __ANDROID__
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

#include "platform/android.h"
#endif

// From the Makefile: the flags the game code is compiled and linked with (as `make` builds it in a source checkout)
#ifndef GAMELIB_CFLAGS
#define GAMELIB_CFLAGS "-O1 -fno-strict-aliasing -fvisibility=hidden"
#endif
#ifndef GAMELIB_SHARED
#define GAMELIB_SHARED "-shared"
#endif
#ifndef GAMEBUILD_ID
#define GAMEBUILD_ID GAMELIB_ID
#endif

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

// ------------------------------------------------------------------ child processes
// Every one running, so that quitting mid-build ends them too (atexit) instead of leaving compilers behind.
#define MAX_PROCS 80
static pthread_mutex_t g_plock = PTHREAD_MUTEX_INITIALIZER;
static struct { PlatProc p; int kind; char log[1100]; } g_procs[MAX_PROCS];
static int g_nprocs;
static bool g_quitting;
// The running build's options: its tool callback hears about every child (the Windows build tool reports them)
static const GameBuildOpts *g_opts;

static void end_all(void) {
    pthread_mutex_lock(&g_plock);
    g_quitting = true;
    for (int i = 0; i < g_nprocs; i++) plat_proc_kill(g_procs[i].p);
    g_nprocs = 0;
    pthread_mutex_unlock(&g_plock);
}

static PlatProc spawn(int kind, char **argv, const char *log) {
    if (g_opts && g_opts->tool) g_opts->tool(kind, (const char *const *)argv, log, -1);
    pthread_mutex_lock(&g_plock);
    PlatProc p = g_quitting ? 0 : plat_spawn((const char *const *)argv, log, false);
    if (p && g_nprocs < MAX_PROCS) {
        g_procs[g_nprocs].p = p;
        g_procs[g_nprocs].kind = kind;
        snprintf(g_procs[g_nprocs].log, sizeof g_procs[g_nprocs].log, "%s", log);
        g_nprocs++;
    }
    pthread_mutex_unlock(&g_plock);
    if (!p && g_opts && g_opts->tool) g_opts->tool(kind, NULL, log, 255);  // (didn't start)
    return p;
}

// plat_proc_wait without blocking, then forgotten once it has ended
static int finished(PlatProc p) {
    pthread_mutex_lock(&g_plock);
    int r = plat_proc_wait(p, false), kind = -1;
    char log[1100] = "";
    if (r >= 0)
        for (int i = 0; i < g_nprocs; i++)
            if (g_procs[i].p == p) {
                kind = g_procs[i].kind;
                snprintf(log, sizeof log, "%s", g_procs[i].log);
                g_procs[i] = g_procs[--g_nprocs];
                break;
            }
    pthread_mutex_unlock(&g_plock);
    if (kind >= 0 && g_opts && g_opts->tool) g_opts->tool(kind, NULL, log, r);
    return r;
}

static void stop(PlatProc p) {  // ends one that is still running, then forgotten
    pthread_mutex_lock(&g_plock);
    bool ours = false;
    for (int i = 0; i < g_nprocs; i++)
        if (g_procs[i].p == p) { g_procs[i] = g_procs[--g_nprocs]; ours = true; break; }
    pthread_mutex_unlock(&g_plock);
    if (ours) plat_proc_kill(p);
}

static void sleep_ms(int ms) { plat_sleep_until_ns(plat_now_ns() + (uint64_t)ms * 1000000ull); }

static int wait_for(PlatProc p) {
    int r;
    while ((r = finished(p)) < 0) sleep_ms(20);
    return r;
}

// ------------------------------------------------------------------ argument lists
typedef struct { char **v; int n, cap; } Args;
static void add(Args *a, const char *s) {
    if (a->n + 2 > a->cap) {
        a->cap = a->cap ? a->cap * 2 : 32;
        a->v = (char **)realloc(a->v, sizeof *a->v * a->cap);
    }
    a->v[a->n++] = strdup(s);
    a->v[a->n] = NULL;
}
static void add_split(Args *a, const char *flags) {  // (space-separated, no quoting)
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", flags);
    for (char *t = strtok(buf, " "); t; t = strtok(NULL, " ")) add(a, t);
}
static void args_free(Args *a) {
    for (int i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v);
    memset(a, 0, sizeof *a);
}

// ------------------------------------------------------------------ the tools
typedef struct {
    char gen[1100], gen_script[1100];  // generator: trgen, or (source checkout) the venv's python + tools/gen.py
    char cc[1100];                     // compiler: zig (as `zig cc`), or EOWRECOMP_CC / clang
    bool zig;
    char inc[1100], entry[1100], hle[1100];  // headers the game code includes, tools/gamelib_entry.c, HLE names
#ifdef _WIN32
    char runtime_lib[1100];  // runtime objects Setup links into the runtime-only game executable
    char link_args[1100];    // platform libraries and linker options shipped with the Setup package
    char runtime_import[1100]; // import library used by Trirod.dll to call the runtime executable
#endif
#ifdef __ANDROID__
    // the app's native library folder (programs run only from there: libeowpython.so, libzig.so; and libmain.so, to
    // link against) and the NDK pieces unpacked from the APK's assets (files/ondevice/cckit: tools/android/ondevice/)
    char nlib[1100], kit[1100], ondev[1100];
#endif
} Tools;

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

// Stable content keys for reusing completed generator and compiler outputs across setup runs.
static uint64_t hash_bytes(uint64_t h, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    while (n--) h = (h ^ *p++) * 1099511628211ull;
    return h;
}

static uint64_t hash_file(uint64_t h, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) h = hash_bytes(h, buf, n);
    bool ok = !ferror(f);
    fclose(f);
    return ok ? h : 0;
}

static void key_path(char *out, size_t n, const char *path) { snprintf(out, n, "%s.key", path); }

static bool key_matches(const char *path, uint64_t key) {
    char kp[1200], value[32], saved[32] = {0};
    key_path(kp, sizeof kp, path);
    FILE *f = fopen(kp, "rb");
    if (!f || !file_exists(path)) { if (f) fclose(f); return false; }
    bool read_ok = fgets(saved, sizeof saved, f) != NULL;
    fclose(f);
    saved[strcspn(saved, "\r\n")] = 0;
    snprintf(value, sizeof value, "%016llx", (unsigned long long)key);
    return read_ok && !strcmp(saved, value);
}

static void save_key(const char *path, uint64_t key) {
    char kp[1200], tmp[1230], value[32];
    key_path(kp, sizeof kp, path);
    snprintf(tmp, sizeof tmp, "%s.tmp", kp);
    snprintf(value, sizeof value, "%016llx\n", (unsigned long long)key);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fputs(value, f) >= 0;
    if (fclose(f) != 0) ok = false;
    if (ok) { remove(kp); rename(tmp, kp); }
    else remove(tmp);
}

static uint64_t module_key(const char *nso, const char *base) {
    uint64_t h = hash_file(1469598103934665603ull, nso);
    if (!h) return 0;
    h = hash_bytes(h, GAMELIB_ID, sizeof GAMELIB_ID);
    return hash_bytes(h, base, strlen(base));
}

static int generated_chunks(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int chunks = 0;
    bool table = false;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 3 || strcmp(e->d_name + n - 2, ".c")) continue;
        if (n >= 8 && !strcmp(e->d_name + n - 8, "_table.c")) table = true;
        else chunks++;
    }
    closedir(d);
    return table ? chunks : -1;
}

static uint64_t unit_key(const Tools *t, const char *src) {
    uint64_t h = hash_file(1469598103934665603ull, src);
    if (!h) return 0;
    h = hash_bytes(h, GAMELIB_ID, sizeof GAMELIB_ID);
    h = hash_bytes(h, GAMELIB_CFLAGS, sizeof GAMELIB_CFLAGS);
    return hash_bytes(h, t->cc, strlen(t->cc));
}

// One-time migration for old .gamebuild folders made before .key stamps existed. Generated chunks carry no
// GAMELIB_ID-dependent data; their object can be kept when it is newer than its generated source and compile headers.
// The entry object is always rebuilt because it embeds the current GAMELIB_ID.
static bool legacy_unit_current(const Tools *t, const char *src_path, const char *obj_path) {
    if (!strcmp(src_path, t->entry)) return false;
    struct stat src, obj;
    if (stat(src_path, &src) != 0 || stat(obj_path, &obj) != 0 || obj.st_mtime <= src.st_mtime) return false;
    static const char *const headers[] = {"cpu.h", "simd.h", "simd_fp.h", "gamelib.h", "modules.h"};
    for (size_t i = 0; i < sizeof headers / sizeof *headers; i++) {
        char path[1200];
        struct stat h;
        snprintf(path, sizeof path, "%s" SEP "%s", t->inc, headers[i]);
        if (stat(path, &h) != 0 || obj.st_mtime <= h.st_mtime) return false;
    }
    return true;
}

#ifdef __ANDROID__
// The folder libmain.so was loaded from: the native library folder when the APK's libraries are extracted at install
// (android:extractNativeLibs, the on-phone build's packaging); from inside the APK ("base.apk!/lib/...") there are
// no programs to run.
static bool native_lib_dir(char *out, size_t n) {
    Dl_info di;
    if (!dladdr((void *)&native_lib_dir, &di) || !di.dli_fname || strchr(di.dli_fname, '!')) return false;
    snprintf(out, n, "%s", di.dli_fname);
    char *s = strrchr(out, '/');
    if (!s) return false;
    *s = 0;
    return true;
}

// Phones: the generator is libeowpython.so running tools/gen.py, the compiler zig's clang and ld.lld (libzig.so) with
// the NDK pieces (cckit/), all from the on-phone build's packaging. ondev: where EowActivity.buildTools unpacked the
// asset archive; NULL = not asked (only checks that the programs are there).
static bool find_tools_android(Tools *t, const char *ondev) {
    memset(t, 0, sizeof *t);
    if (!native_lib_dir(t->nlib, sizeof t->nlib)) return false;
    snprintf(t->gen, sizeof t->gen, "%s/libeowpython.so", t->nlib);
    snprintf(t->cc, sizeof t->cc, "%s/libzig.so", t->nlib);
    t->zig = true;
    if (!file_exists(t->gen) || !file_exists(t->cc)) return false;
    if (!ondev) return true;
    snprintf(t->ondev, sizeof t->ondev, "%s", ondev);
    snprintf(t->gen_script, sizeof t->gen_script, "%s/tools/gen.py", ondev);
    snprintf(t->kit, sizeof t->kit, "%s/cckit", ondev);
    snprintf(t->inc, sizeof t->inc, "%s/gamelib-src", t->kit);
    snprintf(t->entry, sizeof t->entry, "%s/gamelib_entry.c", t->inc);
    snprintf(t->hle, sizeof t->hle, "%s/hle_symbols.txt", t->inc);
    return file_exists(t->gen_script) && file_exists(t->entry) && file_exists(t->hle);
}

// MemAvailable in MB (/proc/meminfo), -1 if unknown
static long android_mem_available_mb(void) {
    FILE *f = fopen("/proc/meminfo", "rb");
    if (!f) return -1;
    char line[128];
    long kb = -1;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) break;
    fclose(f);
    return kb < 0 ? -1 : kb / 1024;
}

static bool copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
    bool ok = in && out;
    static char buf[1 << 20];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    if (in) ok = ok && !ferror(in), fclose(in);
    if (out && fclose(out) != 0) ok = false;
    return ok;
}

// The library goes where runtime/gamelib.c looks on a phone: the app's internal storage (the game folder is on the
// shared storage, mounted without exec). ondev = <internal>/ondevice. The external copy is removed.
static bool install_internal(const char *game_dir, const char *ondev, char *why, size_t why_size) {
    char internal[1100], from[1200], to[1200], tmp[1300];
    snprintf(internal, sizeof internal, "%s", ondev);
    char *s = strrchr(internal, '/');
    if (s) *s = 0;
    char a[PATH_MAX], b[PATH_MAX];
    if (realpath(game_dir, a) && realpath(internal, b) && !strcmp(a, b)) return true;  // (the same folder)
    static const char *const ext[2] = {"", ".id"};  // (the library first: a matching .id says it's complete)
    for (int i = 0; i < 2; i++) {
        snprintf(from, sizeof from, "%s/%s%s", game_dir, GAMELIB_NAME, ext[i]);
        snprintf(to, sizeof to, "%s/%s%s", internal, GAMELIB_NAME, ext[i]);
        snprintf(tmp, sizeof tmp, "%s.tmp", to);
        if (i == 0) {
            char id[1200];  // (an old pair: its id goes first, so a half-copied library never looks current)
            snprintf(id, sizeof id, "%s/%s.id", internal, GAMELIB_NAME);
            remove(id);
        }
        if (!copy_file(from, tmp) || rename(tmp, to) != 0) {
            remove(tmp);
            LOG("[gamebuild] installing %s: %s\n", to, strerror(errno));
            snprintf(why, why_size, "the game code couldn't be installed (storage full?)");
            return false;
        }
    }
    snprintf(from, sizeof from, "%s/%s", game_dir, GAMELIB_NAME);
    remove(from);
    LOG("[gamebuild] installed %s/%s\n", internal, GAMELIB_NAME);
    return true;
}
#endif

static bool find_tools(Tools *t) {
#ifdef __ANDROID__
    return find_tools_android(t, NULL);
#endif
    memset(t, 0, sizeof *t);
    const char *e = getenv("EOWRECOMP_GEN");
    if (e && *e) snprintf(t->gen, sizeof t->gen, "%s", e);
    else if (!rt_resource("trgen" SEP "trgen" PLAT_EXE_SUFFIX, NULL, t->gen, sizeof t->gen) &&
             !(rt_resource(NULL, "tools/" PLAT_VENV_PYTHON, t->gen, sizeof t->gen) &&
               rt_resource(NULL, "tools/gen.py", t->gen_script, sizeof t->gen_script)))
        return false;
    e = getenv("EOWRECOMP_CC");
    if (e && *e) snprintf(t->cc, sizeof t->cc, "%s", e);
    else if (rt_resource("zig" SEP "zig" PLAT_EXE_SUFFIX, NULL, t->cc, sizeof t->cc)) t->zig = true;
#ifndef _WIN32
    else if (rt_dev_mode() && file_exists("/usr/bin/clang")) snprintf(t->cc, sizeof t->cc, "/usr/bin/clang");
#endif
    else return false;
    if (!rt_resource("gamelib-src" SEP "cpu.h", "runtime/cpu.h", t->inc, sizeof t->inc) ||
        !rt_resource("gamelib-src" SEP "gamelib_entry.c", "tools/gamelib_entry.c", t->entry, sizeof t->entry) ||
        !rt_resource("gamelib-src" SEP "hle_symbols.txt", "runtime/hle/hle_symbols.txt", t->hle, sizeof t->hle))
        return false;
    char *s = strrchr(t->inc, '/'), *b = strrchr(t->inc, '\\');
    if (b > s) s = b;
    if (s) *s = 0;
#ifdef _WIN32
    if (!t->zig || !rt_resource("link-kit" SEP "eowruntime.a", NULL, t->runtime_lib, sizeof t->runtime_lib) ||
        !rt_resource("link-kit" SEP "lib" SEP "eowrecomp.lib", NULL, t->runtime_import, sizeof t->runtime_import) ||
        !rt_resource("link-kit" SEP "link.args", NULL, t->link_args, sizeof t->link_args)) return false;
#endif
    return true;
}

bool gamebuild_available(void) {
    Tools t;
    return find_tools(&t);
}

static void compiler(Args *a, const Tools *t) {
    add(a, t->cc);
#ifdef __ANDROID__
    // zig's own clang driver (not `zig cc`: zig has no libc for Android and would build its compiler_rt from source),
    // the headers from the kit (bionic's few + this clang's own: tools/android/ondevice/build_cckit.sh)
    static const char *const sub[] = {"/clang/include", "/sysroot/usr/include", "/sysroot/usr/include/aarch64-linux-android"};
    add(a, "clang");
    add(a, "-target");
    add(a, "aarch64-linux-android30");
    add(a, "-nostdinc");
    for (int i = 0; i < 3; i++) {
        char inc[1200];
        snprintf(inc, sizeof inc, "%s%s", t->kit, sub[i]);
        add(a, "-isystem");
        add(a, inc);
    }
    return;
#endif
    if (t->zig) add(a, "cc");
}

// ------------------------------------------------------------------ generator progress (its log)
typedef struct {
    const char *name;
    char log[1100], out[1100];
    PlatProc p;
    int exit;                // -1 while running
    int files;               // chunk files written so far (complete)
    unsigned long long ci, n;  // functions written / all
} Gen;

static void read_gen_log(Gen *g) {
    FILE *f = fopen(g->log, "rb");
    if (!f) return;
    static char buf[1 << 16];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long from = size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0;
    fseek(f, from, SEEK_SET);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *last = NULL;
    for (const char *p = strstr(buf, ": progress "); p; p = strstr(p + 1, ": progress ")) last = p;
    unsigned long long ci, all;
    int files;
    if (last && sscanf(last, ": progress %llu/%llu files %d", &ci, &all, &files) == 3) {
        g->ci = ci, g->n = all;
        if (files > g->files) g->files = files;
    }
}

static void log_tail(const char *path, char *out, size_t n) {  // the last line of a log (an error message)
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[4096];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0, SEEK_SET);
    size_t k = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[k] = 0;
    while (k && (buf[k - 1] == '\n' || buf[k - 1] == '\r')) buf[--k] = 0;
    char *nl = strrchr(buf, '\n');
    snprintf(out, n, "%s", nl ? nl + 1 : buf);
}

// ------------------------------------------------------------------ compiling
typedef struct {
    char src[1100], obj[1100], log[1100];
    unsigned long long bytes;
} Unit;

typedef struct {
    Unit *u;
    int n, cap;
} Units;

static void add_unit(Units *us, const char *src, const char *work, const char *stem) {
    if (us->n == us->cap) {
        us->cap = us->cap ? us->cap * 2 : 64;
        us->u = (Unit *)realloc(us->u, sizeof *us->u * us->cap);
    }
    Unit *u = &us->u[us->n++];
    snprintf(u->src, sizeof u->src, "%s", src);
    snprintf(u->obj, sizeof u->obj, "%s" SEP "%s.o", work, stem);
    snprintf(u->log, sizeof u->log, "%s" SEP "%s.log", work, stem);
    struct stat st;
    u->bytes = stat(src, &st) == 0 ? (unsigned long long)st.st_size : 0;
}

// The flags the game code was built with (GAMELIB_CFLAGS, part of its id). zig cc reads -march=<cpu> with its own CPU
// names (x86_64_v3) and takes "-<feature>" off the end, so x86-64-v3 would be CPU "x86": the same CPU as -mcpu there.
static void add_cflags(Args *a, const Tools *t) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", GAMELIB_CFLAGS);
    for (char *f = strtok(buf, " "); f; f = strtok(NULL, " ")) {
#ifdef __ANDROID__
        if (!strcmp(f, "-Iruntime")) continue;  // (gamelib_id.sh's, for a computer's checkout: the kit's -I instead)
        add(a, f);
        continue;
#endif
        if (t->zig && !strncmp(f, "-march=", 7)) {
            char cpu[128];
            snprintf(cpu, sizeof cpu, "-mcpu=%s", f + 7);
            for (char *c = cpu + 6; *c; c++) if (*c == '-') *c = '_';
            add(a, cpu);
        } else {
            add(a, f);
        }
    }
}

static PlatProc start_compile(const Tools *t, const Unit *u) {
    Args a = {0};
    compiler(&a, t);
    add_cflags(&a, t);
    char inc[1200];
    snprintf(inc, sizeof inc, "-I%s", t->inc);
    add(&a, inc);
    add(&a, "-DGAMELIB_ID=\"" GAMELIB_ID "\"");
    add(&a, "-c");
    add(&a, u->src);
    add(&a, "-o");
    add(&a, u->obj);
    LOG("[gamebuild] compiling %s -> %s\n", u->src, u->obj);
    PlatProc p = spawn(GAMEBUILD_TOOL_COMPILER, a.v, u->log);
    args_free(&a);
    return p;
}

// One module's generator: tools/gen.py <name> <nso> <base> <out> --hle <names> [--shift] (base: runtime/modules.h's
// C literal)
static bool start_gen(const Tools *t, Gen *g, const char *game_dir, const char *base) {
    char nso[1100], b[32];
    snprintf(nso, sizeof nso, "%s" SEP "exefs" SEP "%s", game_dir, g->name);
    snprintf(b, sizeof b, "%.*s", (int)strcspn(base, "u"), base);  // (the C literal without its ull)
    Args a = {0};
    add(&a, t->gen);
    if (t->gen_script[0]) add(&a, t->gen_script);
    add(&a, g->name);
    add(&a, nso);
    add(&a, b);
    add(&a, g->out);
    add(&a, "--hle");
    add(&a, t->hle);
    if (GUEST_SHIFT) {  // (Android: the layout runtime/cpu.h moves)
        char sh[32];
        snprintf(sh, sizeof sh, "0x%llx", (unsigned long long)GUEST_SHIFT);
        add(&a, "--shift");
        add(&a, sh);
    }
    LOG("[gamebuild] lifting %s -> %s\n", nso, g->out);
    g->p = spawn(GAMEBUILD_TOOL_GENERATOR, a.v, g->log);
    args_free(&a);
    return g->p != 0;
}

// The game's original BNSH archives, parsed once into the portable shader catalog (tools/shader_catalog.py through the
// generator helper's --shader-catalog). This copies the Maxwell control/code records and their program pairing; it
// does not translate or compile anything. Translation and pipeline building are the shader preparation tool's
// (runtime/tools/shaderprep.c) and the runtime's.
static bool prepare_shader_catalog(const Tools *t, const char *game_dir, const GameBuildOpts *o, char *why, size_t why_size) {
    char manifest[1200], romfs[1200], main[1200], out[1200], log[1200], work[1200];
    snprintf(manifest, sizeof manifest, "%s" SEP "shader_catalog" SEP "manifest.json", game_dir);
    if (file_exists(manifest)) return true;  // the game version is fixed and extraction never mixes updates
    snprintf(romfs, sizeof romfs, "%s" SEP "romfs", game_dir);
    if (!file_exists(romfs)) snprintf(romfs, sizeof romfs, "%s" SEP "romfs.bin", game_dir);
    snprintf(main, sizeof main, "%s" SEP "exefs" SEP "main", game_dir);
    snprintf(out, sizeof out, "%s" SEP "shader_catalog", game_dir);
    snprintf(work, sizeof work, "%s" SEP ".gamebuild", game_dir);
    mkdir(work, 0755);
    snprintf(log, sizeof log, "%s" SEP "shader_catalog.log", work);
    if (o && o->progress) o->progress(0, "Reading the game's shader archives...");
    Args a = {0};
    add(&a, t->gen);
    if (t->gen_script[0]) add(&a, t->gen_script);
    add(&a, "--shader-catalog");
    add(&a, romfs);
    add(&a, out);
    if (file_exists(main)) { add(&a, "--main"); add(&a, main); }
    LOG("[gamebuild] scanning game shaders from %s -> %s\n", romfs, out);
    PlatProc p = spawn(GAMEBUILD_TOOL_SHADER_CATALOG, a.v, log);
    args_free(&a);
    if (!p || wait_for(p) != 0 || !file_exists(manifest)) {
        snprintf(why, why_size, "the original game shader catalog could not be built (see .gamebuild/shader_catalog.log)");
        return false;
    }
    return true;
}

bool gamebuild_shader_catalog(const char *game_dir, const GameBuildOpts *o, char *why, size_t why_size) {
    static bool registered;
    if (!registered) registered = true, atexit(end_all);
    Tools t;
    if (!find_tools(&t)) {
        snprintf(why, why_size, "the game code builder is missing from this copy of TriRod");
        return false;
    }
    g_opts = o;
    bool ok = prepare_shader_catalog(&t, game_dir, o, why, why_size);
    g_opts = NULL;
    return ok;
}

// ------------------------------------------------------------------ the build
bool gamebuild_run(const char *game_dir, const GameBuildOpts *o, char *why, size_t why_size) {
    uint64_t t0 = plat_now_ns();
    static bool registered;
    if (!registered) registered = true, atexit(end_all);
    Tools t;
#ifdef __ANDROID__
    // the generator and the NDK pieces out of the APK's assets (once per app version: EowActivity.buildTools)
    if (o->progress) o->progress(0, "Preparing the build tools...");
    // (test: EOWRECOMP_ONDEVICE_DIR=<an unpacked ondevice.zip>, without the app's Java side)
    const char *ondev = getenv("EOWRECOMP_ONDEVICE_DIR");
    if (!ondev || !*ondev) ondev = android_build_tools();
    if (!ondev || !find_tools_android(&t, ondev)) {
#else
    if (!find_tools(&t)) {
#endif
        snprintf(why, why_size, "the game code builder is missing from this copy of EoWRecomp");
        return false;
    }
#ifdef __ANDROID__
    int android_jobs;
    {
        // A phone stays usable: the compilers (they inherit this thread's niceness and CPUs) at a lower priority, one
        // per core, the little ones too (the build is heat-bound: little cores add throughput for little heat; the
        // biggest files go first). The generator's Python and its modules' libraries (ctypes) from the kit.
        setpriority(PRIO_PROCESS, (id_t)gettid(), 10);
        long n = sysconf(_SC_NPROCESSORS_CONF);
        int ncpu = n < 1 ? 1 : n > 64 ? 64 : (int)n;
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int i = 0; i < ncpu; i++) CPU_SET(i, &set);
        sched_setaffinity(0, sizeof set, &set);
        android_jobs = ncpu;
        char v[2400];
        snprintf(v, sizeof v, "%s/pyhome", ondev);
        setenv("EOWPY_HOME", v, 1);
        snprintf(v, sizeof v, "%s/tools:%s/pyhome/lib/python3.14/site-packages", ondev, ondev);
        setenv("EOWPY_PATH", v, 1);
        setenv("LIBCAPSTONE_PATH", t.nlib, 1);
        setenv("LIBUNICORN_PATH", t.nlib, 1);
        setenv("TMPDIR", ondev, 1);
    }
#endif
    char work[1100], lib[1100], tmp[1100];
    snprintf(work, sizeof work, "%s" SEP ".gamebuild", game_dir);
#ifdef _WIN32
    // Keep the runtime beside Setup and its dependency DLLs. Only the game-specific code belongs in extracted/.
    snprintf(lib, sizeof lib, "%s" SEP "%s", game_dir, GAMELIB_NAME);
    snprintf(tmp, sizeof tmp, "%s" SEP "%s.tmp", work, GAMELIB_NAME);
    char runtime_exe[1100], runtime_tmp[1100];
    snprintf(runtime_exe, sizeof runtime_exe, "%s" SEP ".." SEP "Trirod.exe", game_dir);
    snprintf(runtime_tmp, sizeof runtime_tmp, "%s" SEP "Trirod.exe.tmp", work);
#else
    snprintf(lib, sizeof lib, "%s" SEP "%s", game_dir, GAMELIB_NAME);
    snprintf(tmp, sizeof tmp, "%s" SEP "%s.tmp", work, GAMELIB_NAME);
#endif
    mkdir(work, 0755);
    if (t.zig) {  // keep its caches in the work folder with the rest of this build's output, not in ~/.cache
        char zc[1200], local[1300], global[1300];
        const char *override = getenv("EOWRECOMP_ZIG_CACHE_DIR");
        if (override && *override) snprintf(zc, sizeof zc, "%s", override);
        else snprintf(zc, sizeof zc, "%s" SEP "extracted" SEP ".gamebuild" SEP "zig-cache", rt_data_root());
        snprintf(local, sizeof local, "%s" SEP "local", zc);
        snprintf(global, sizeof global, "%s" SEP "global", zc);
        mkdir(zc, 0755), mkdir(local, 0755), mkdir(global, 0755);
#ifdef _WIN32
        _putenv_s("ZIG_GLOBAL_CACHE_DIR", global);
        _putenv_s("ZIG_LOCAL_CACHE_DIR", local);
#else
        setenv("ZIG_GLOBAL_CACHE_DIR", global, 1);
        setenv("ZIG_LOCAL_CACHE_DIR", local, 1);
#endif
    }
    LOG("[gamebuild] generator %s%s%s, compiler %s%s, %d jobs\n", t.gen, t.gen_script[0] ? " " : "", t.gen_script,
        t.cc, t.zig ? " cc" : "", o->jobs);
#ifdef _WIN32
    LOG("[gamebuild] runtime archive: %s\n", t.runtime_lib);
#endif
    g_opts = o;
    if (!o->skip_shader_catalog && !prepare_shader_catalog(&t, game_dir, o, why, why_size)) { g_opts = NULL; return false; }

    // 1. the generator, every module at once (phones: one after the other - the generator of main alone takes GBs);
    // their finished files are compiled while it goes on
#define GEN_MOD(mod, b) {.name = #mod},
#define GEN_BASE(mod, b) #b,
    Gen gens[GAME_NMODULES] = {GAME_MODULES(GEN_MOD)};
    static const char *const base[GAME_NMODULES] = {GAME_MODULES(GEN_BASE)};
#ifdef __ANDROID__
    const bool one_at_a_time = true;
#else
    const bool one_at_a_time = false;
#endif
    bool ok = true;
    for (int i = 0; i < GAME_NMODULES; i++) {
        gens[i].exit = -1;
        snprintf(gens[i].log, sizeof gens[i].log, "%s" SEP "gen_%s.log", work, gens[i].name);
        snprintf(gens[i].out, sizeof gens[i].out, "%s" SEP "%s", work, gens[i].name);
        char nso[1100];
        snprintf(nso, sizeof nso, "%s" SEP "exefs" SEP "%s", game_dir, gens[i].name);
        uint64_t key = module_key(nso, base[i]);
        int chunks = generated_chunks(gens[i].out);
        if (key && key_matches(gens[i].out, key) && chunks >= 0) {
            gens[i].exit = 0;
            gens[i].files = chunks;
            gens[i].ci = gens[i].n = 1;
            LOG("[gamebuild] reusing generated %s module (%d chunks)\n", gens[i].name, chunks);
        } else {
            // Keep old chunks: gen.py only rewrites changed output, allowing the object cache to migrate safely.
        }
    }
    int gen_next = 0;  // the next module whose generator starts
    for (; gen_next < GAME_NMODULES && ok && (!one_at_a_time || gen_next == 0); gen_next++)
        if (gens[gen_next].exit != 0 && !start_gen(&t, &gens[gen_next], game_dir, base[gen_next])) {
            snprintf(why, why_size, "the game code builder didn't start");
            ok = false;
        }

    Units units = {0};
    int queued[GAME_NMODULES] = {0};  // chunk files of each module put in units so far
    bool tables[GAME_NMODULES] = {false};
    int next = 0, jobs = o->jobs < 1 ? 1 : o->jobs > 64 ? 64 : o->jobs;
#ifdef __ANDROID__
    if (android_jobs > jobs) jobs = android_jobs;
    uint64_t last_start = 0;  // (memory: see below)
    long low_mem = -1;        // the lowest MemAvailable seen while starting compilers (the log, for tuning)
    int held = 0;             // compiler starts put off for memory
#endif
    {   // (testing / tuning: EOWRECOMP_BUILD_JOBS)
        const char *e = getenv("EOWRECOMP_BUILD_JOBS");
        if (e && atoi(e) > 0) jobs = atoi(e) > 64 ? 64 : atoi(e);
    }
    struct { PlatProc p; int unit; } run[64];
    int nrun = 0;
    unsigned long long done_bytes = 0;
    double shown = 0;
    {
        add_unit(&units, t.entry, work, "gamelib_entry");
    }
    while (ok) {
        // generator progress, newly finished files
        bool gens_done = true;
        double gen_frac = 0;
        unsigned long long written = 0;
        if (one_at_a_time && gen_next < GAME_NMODULES && gens[gen_next - 1].exit == 0) {  // (the previous one done)
            if (!start_gen(&t, &gens[gen_next], game_dir, base[gen_next])) {
                snprintf(why, why_size, "the game code builder didn't start");
                ok = false;
                break;
            }
            gen_next++;
        }
        for (int i = 0; i < GAME_NMODULES; i++) {
            Gen *g = &gens[i];
            if (g->exit < 0 && g->p) {  // (not a waiting one: no process yet)
                read_gen_log(g);
                int r = finished(g->p);
                if (r >= 0) {
                    g->exit = r;
                    read_gen_log(g);
                    if (r != 0) {
                        char last[300];
                        log_tail(g->log, last, sizeof last);
                        LOG("[gamebuild] generator (%s) failed: %s\n", g->name, last);
                        snprintf(why, why_size, "the game code couldn't be prepared");
                        ok = false;
                    } else {
                        char nso[1100];
                        int index = (int)(g - gens);
                        snprintf(nso, sizeof nso, "%s" SEP "exefs" SEP "%s", game_dir, g->name);
                        save_key(g->out, module_key(nso, base[index]));
                    }
                }
            }
            if (g->exit != 0) gens_done = false;
            for (; queued[i] < g->files; queued[i]++) {
                char src[1100], stem[64];
                snprintf(stem, sizeof stem, "%s_%03d", g->name, queued[i]);
                snprintf(src, sizeof src, "%s" SEP "%s.c", g->out, stem);
                add_unit(&units, src, work, stem);
            }
            if (g->exit == 0 && !tables[i]) {
                char src[1100], stem[64];
                snprintf(stem, sizeof stem, "%s_table", g->name);
                snprintf(src, sizeof src, "%s" SEP "%s.c", g->out, stem);
                add_unit(&units, src, work, stem);
                tables[i] = true;
            }
            gen_frac += (g->exit == 0 ? 1.0 : g->n ? (double)g->ci / (double)g->n : 0) / GAME_NMODULES;
        }
        if (!ok) break;
        for (int k = 0; k < units.n; k++) written += units.u[k].bytes;

        // compiles
        while (nrun < jobs && next < units.n) {
#ifdef __ANDROID__
            // Memory: the generators take GBs and the biggest compiles hundreds of MB each, on phones with 6-8 GB.
            // Below 1.5 GB available a new compiler starts at most once a second (its memory shows in MemAvailable by
            // then), below 700 MB not at all while two still run - the build slows down instead of the system
            // killing it.
            if (nrun >= 2) {
                long avail = android_mem_available_mb();
                uint64_t now = plat_now_ns();
                if (avail >= 0 && (low_mem < 0 || avail < low_mem)) low_mem = avail;
                if (avail >= 0 && (avail < 700 || (avail < 1500 && now - last_start < 1000000000ull))) { ++held; break; }
            }
#endif
            Unit *candidate = &units.u[next];
            uint64_t key = unit_key(&t, candidate->src);
            if (key && key_matches(candidate->obj, key)) {
                done_bytes += candidate->bytes;
                next++;
                continue;
            }
            char cache_key[1200];
            key_path(cache_key, sizeof cache_key, candidate->obj);
            if (key && !file_exists(cache_key) && legacy_unit_current(&t, candidate->src, candidate->obj)) {
                save_key(candidate->obj, key);
                LOG("[gamebuild] migrated existing object %s\n", candidate->obj);
                done_bytes += candidate->bytes;
                next++;
                continue;
            }
            PlatProc p = start_compile(&t, candidate);
            if (!p) { snprintf(why, why_size, "the compiler didn't start"); ok = false; break; }
            run[nrun].p = p, run[nrun].unit = next++;
            nrun++;
#ifdef __ANDROID__
            last_start = plat_now_ns();
#endif
        }
        for (int k = 0; k < nrun; k++) {
            int r = finished(run[k].p);
            if (r < 0) continue;
            Unit *u = &units.u[run[k].unit];
            if (r != 0) {
                char last[300];
                log_tail(u->log, last, sizeof last);
                LOG("[gamebuild] compiling %s failed: %s\n", u->src, last);
                snprintf(why, why_size, "the game code didn't compile");
                ok = false;
            } else save_key(u->obj, unit_key(&t, u->src));
            done_bytes += u->bytes;
            run[k--] = run[--nrun];
        }
        if (!ok) break;
        if (gens_done && next == units.n && nrun == 0) {
#ifdef __ANDROID__
            LOG("[gamebuild] lowest MemAvailable %ld MB, %d compiler starts put off for memory\n", low_mem, held);
#endif
            break;
        }

        // progress: the generator's share (its functions written), the compiler's (bytes of C compiled, of an
        // estimate of all of it while the generator still writes)
        double est_total = gens_done ? (double)written : gen_frac > 0.02 ? (double)written / gen_frac : 0;
        double comp_frac = est_total > 0 ? (double)done_bytes / est_total : 0;
        if (comp_frac > 1) comp_frac = 1;
        double f = 0.2 * gen_frac + 0.75 * comp_frac;
        double secs = (double)(plat_now_ns() - t0) / 1e9;
        if (gen_frac == 0) f = 0.05 * (secs / (secs + 20));  // (the generator's first pass: no count yet)
        if (f > shown) shown = f;
        if (o->progress) o->progress(shown, gen_frac < 1 && !gens_done ? "Reading the game code..." : "Recompiling...");
        if (o->phase) o->phase(gens_done ? 1 : 0, gens_done ? comp_frac * 0.9 : gen_frac);
        sleep_ms(50);
    }
    for (int k = 0; k < nrun; k++) stop(run[k].p);  // (a failure: the rest is of no use - the failure shows at once)
    for (int i = 0; i < GAME_NMODULES; i++)
        if (gens[i].p && gens[i].exit < 0) stop(gens[i].p);

    // 2. link
    if (ok) {
        if (o->progress) o->progress(shown > 0.95 ? shown : 0.95, "Finishing...");
        if (o->phase) o->phase(1, 0.9);
        char rsp[1100], log[1100];
        snprintf(rsp, sizeof rsp, "%s" SEP "link.rsp", work);
        snprintf(log, sizeof log, "%s" SEP "link.log", work);
        FILE *f = fopen(rsp, "wb");
        if (f) {
            for (int k = 0; k < units.n; k++) {  // (forward slashes: the response file's backslashes are escapes)
                fputc('"', f);
                for (const char *c = units.u[k].obj; *c; c++) fputc(*c == '\\' ? '/' : *c, f);
                fputs("\"\n", f);
            }
            fclose(f);
        }
        Args a = {0};
        char at[1200];
#ifdef _WIN32
        char opt[1200], kitdir[1200];
#endif
        snprintf(at, sizeof at, "@%s", rsp);
#ifdef __ANDROID__
        {   // zig's ld.lld as NDK clang runs it (cckit/link.rsp: tools/android/ondevice/build_cckit.sh), against the
            // app's own libmain.so (the runtime functions the game code calls: it is loaded first)
            char opt[1200];
            static const char *const sys = "/sysroot/usr/lib/aarch64-linux-android/30";
            add(&a, t.cc);
            add(&a, "ld.lld");
            snprintf(opt, sizeof opt, "@%s/link.rsp", t.kit);
            add(&a, opt);
            add(&a, "-o");
            add(&a, tmp);
            snprintf(opt, sizeof opt, "%s%s/crtbegin_so.o", t.kit, sys);
            add(&a, opt);
            add(&a, at);
            snprintf(opt, sizeof opt, "-L%s", t.nlib);
            add(&a, opt);
            add(&a, "-lmain");
            snprintf(opt, sizeof opt, "-L%s%s", t.kit, sys);
            add(&a, opt);
            snprintf(opt, sizeof opt, "%s/clang/lib/libclang_rt.builtins-aarch64-android.a", t.kit);
            add(&a, opt);
            add(&a, "-ldl");
            add(&a, "-lc");
            add(&a, opt);
            snprintf(opt, sizeof opt, "%s%s/crtend_so.o", t.kit, sys);
            add(&a, opt);
        }
        if (0)
#endif
#ifdef _WIN32
        {
            // Build the game DLL against the runtime's import library, then link the runtime EXE on its own.
            // The EXE stays beside Setup so Windows can find the SDL, FFmpeg and shader compiler DLLs there.
            add(&a, t.cc);
            add(&a, "c++");
            add(&a, "-shared");
            add(&a, "-o");
            add(&a, tmp);
            add(&a, at);
            snprintf(kitdir, sizeof kitdir, "%s", t.runtime_lib);
            char *slash = strrchr(kitdir, '\\');
            if (slash) *slash = 0;
            snprintf(opt, sizeof opt, "-L%s" SEP "lib", kitdir);
            add(&a, opt);
            add(&a, t.runtime_import);
        }
#else
        {
            compiler(&a, &t);
            add_split(&a, GAMELIB_SHARED);
            add(&a, "-o");
            add(&a, tmp);
            add(&a, at);
        }
#endif
        PlatProc p = f ? spawn(GAMEBUILD_TOOL_LINKER, a.v, log) : 0;
        args_free(&a);
        int r = p ? wait_for(p) : 255;
#ifdef _WIN32
        if (r != 0) {
            char last[300];
            log_tail(log, last, sizeof last);
            LOG("[gamebuild] linking game DLL failed: %s\n", last);
            snprintf(why, why_size, "the game code DLL didn't link");
            ok = false;
        } else {
            // Service implementations register through linker-section records and have no direct callers, so the
            // standalone executable must retain every runtime archive member.
            Args exe_args = {0};
            add(&exe_args, t.cc);
            add(&exe_args, "c++");
            add(&exe_args, "-o");
            add(&exe_args, runtime_tmp);
            add(&exe_args, "-Wl,--whole-archive");
            add(&exe_args, t.runtime_lib);
            add(&exe_args, "-Wl,--no-whole-archive");
            snprintf(kitdir, sizeof kitdir, "%s", t.runtime_lib);
            char *slash = strrchr(kitdir, '\\');
            if (slash) *slash = 0;
            snprintf(opt, sizeof opt, "-L%s" SEP "lib", kitdir);
            add(&exe_args, opt);
            FILE *flags = fopen(t.link_args, "rb");
            if (flags) {
                char line[512];
                while (fgets(line, sizeof line, flags)) {
                    line[strcspn(line, "\r\n")] = 0;
                    // (the Makefile names kit files as link-kit/...: relative to the kit's own folder, not to the
                    // data folder the build runs in - the launcher's installs keep the kit in tools\link-kit)
                    if (!strncmp(line, "link-kit/", 9) || !strncmp(line, "link-kit\\", 9)) {
                        char abs[1200];
                        snprintf(abs, sizeof abs, "%s" SEP "%s", kitdir, line + 9);
                        add(&exe_args, abs);
                    } else if (line[0]) add(&exe_args, line);
                }
                fclose(flags);
            }
            p = spawn(GAMEBUILD_TOOL_LINKER, exe_args.v, log);
            args_free(&exe_args);
            r = p ? wait_for(p) : 255;
            if (r != 0) {
                char last[300];
                log_tail(log, last, sizeof last);
                LOG("[gamebuild] linking runtime EXE failed: %s\n", last);
                snprintf(why, why_size, "the runtime executable didn't link");
                ok = false;
            }
        }
#endif
#ifndef _WIN32
        if (r != 0) {
            char last[300];
            log_tail(log, last, sizeof last);
            LOG("[gamebuild] linking failed: %s\n", last);
            snprintf(why, why_size, "the game code didn't link");
            ok = false;
        }
#endif
    }
    if (ok) {
#ifdef _WIN32
        char dll_id[1100], exe_id[1100];
        snprintf(dll_id, sizeof dll_id, "%s.id", lib);
        snprintf(exe_id, sizeof exe_id, "%s.id", runtime_exe);
        FILE *f = NULL;
        remove(lib);
        remove(runtime_exe);
        if (rename(tmp, lib) != 0 || !(f = fopen(dll_id, "wb")) || fprintf(f, "%s\n", GAMELIB_ID) < 0) {
            snprintf(why, why_size, "the game code DLL couldn't be saved");
            ok = false;
        }
        if (f) fclose(f);
        f = NULL;
        if (ok && (rename(runtime_tmp, runtime_exe) != 0 || !(f = fopen(exe_id, "wb")) ||
                   fprintf(f, "%s\n", GAMEBUILD_ID) < 0)) {
            snprintf(why, why_size, "the runtime executable couldn't be saved");
            ok = false;
        }
#else
        remove(lib);
        char idp[1100];
        snprintf(idp, sizeof idp, "%s.id", lib);
        FILE *f = NULL;
        if (rename(tmp, lib) != 0 || !(f = fopen(idp, "wb")) || fprintf(f, "%s\n", GAMEBUILD_ID) < 0) {
            snprintf(why, why_size, "the game code couldn't be saved");
            ok = false;
        }
#endif
        if (f) fclose(f);
    }
#ifdef __ANDROID__
    if (ok) ok = install_internal(game_dir, t.ondev, why, why_size);
#endif
    free(units.u);
    // Keep generated C, object files and logs so setup can reuse any stage whose input key still matches.
    LOG("[gamebuild] %s in %.0fs\n", ok ? "done" : "failed", (double)(plat_now_ns() - t0) / 1e9);
    if (ok && o->progress) o->progress(1, "Finishing...");
    if (ok && o->phase) o->phase(1, 1);
    g_opts = NULL;
    return ok;
}
