// The launcher's / trbuild's decision whether extracted/ still matches the chosen game files, the import record, and
// the swap that replaces an extraction only after a new import succeeded (runtime/tools/import_state.h). No game files:
// records and folders are synthetic, made in a temporary folder.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../runtime/tools/import_state.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static const char *ID_100 = "a581406bc9858651", *ID_110 = "9f221e1afa65ba54";

static void stamp(FileStamp *s, const char *path, uint64_t size, int64_t mtime) {
    snprintf(s->path, sizeof s->path, "%s", path);
    s->size = size, s->mtime = mtime;
}
static ImportRecord sel(const char *base, const char *update, const char *keys, const char *id, const char *version) {
    ImportRecord r;
    memset(&r, 0, sizeof r);
    stamp(&r.base, base, 1000, 10);
    if (update) stamp(&r.update, update, 500, 20);
    stamp(&r.keys, keys, 30, 30);
    snprintf(r.build_id, sizeof r.build_id, "%s", id);
    snprintf(r.version, sizeof r.version, "%s", version);
    return r;
}

static ExtractState decide(bool have, const char *problem, const char *id, const char *ver, const ImportRecord *rec,
                           const ImportRecord *want, bool *matches, char *why) {
    ExtractState s = import_decide(have, problem, id, ver, rec, want, matches, why, 300);
    printf("  %-10s %s\n", extract_state_name(s), why);
    return s;
}

static void test_decide(void) {
    char why[300];
    bool m;
    ImportRecord base_only = sel("C:/Rom/base.nsp", NULL, "C:/Rom/prod.keys", ID_100, "v1.0.0");
    ImportRecord with_update = sel("C:/Rom/base.nsp", "C:/Rom/update.nsp", "C:/Rom/prod.keys", ID_110, "v1.1.0");

    // nothing extracted
    CHECK(decide(false, "no extracted game files", "", "", NULL, &with_update, &m, why) == EXTRACT_NONE);
    // nothing chosen: whatever is extracted is used (the earlier behaviour)
    CHECK(decide(true, NULL, ID_110, "v1.1.0", NULL, NULL, &m, why) == EXTRACT_CURRENT && m);

    // The reported case: a base-only extraction from before records, then an update is chosen. The extracted main
    // (v1.0.0) is not what the selection makes (v1.1.0): stale, though exefs/main exists.
    CHECK(decide(true, NULL, ID_100, "v1.0.0", NULL, &with_update, &m, why) == EXTRACT_STALE && !m);
    CHECK(strstr(why, "v1.0.0") && strstr(why, "v1.1.0"));
    // ... the same with a record of the base-only import
    CHECK(decide(true, NULL, ID_100, "v1.0.0", &base_only, &with_update, &m, why) == EXTRACT_STALE && !m);
    // and the other way: an update's extraction, the update then removed from the selection
    CHECK(decide(true, NULL, ID_110, "v1.1.0", &with_update, &base_only, &m, why) == EXTRACT_STALE && !m);

    // An extraction from before records whose version is what the selection makes: kept (no needless re-import)
    CHECK(decide(true, NULL, ID_110, "v1.1.0", NULL, &with_update, &m, why) == EXTRACT_CURRENT && m);
    CHECK(decide(true, NULL, ID_100, "v1.0.0", NULL, &base_only, &m, why) == EXTRACT_CURRENT && m);

    // With a record: the same selection is current; any change of an input is stale
    CHECK(decide(true, NULL, ID_110, "v1.1.0", &with_update, &with_update, &m, why) == EXTRACT_CURRENT && m);
    ImportRecord w = with_update;  // (path case and slashes don't matter on Windows)
    snprintf(w.base.path, sizeof w.base.path, "c:\\ROM\\Base.nsp");
    CHECK(decide(true, NULL, ID_110, "v1.1.0", &with_update, &w, &m, why) == EXTRACT_CURRENT);
    w = with_update, w.update.size = 501;  // another v1.1.0 update file
    CHECK(decide(true, NULL, ID_110, "v1.1.0", &with_update, &w, &m, why) == EXTRACT_STALE && strstr(why, "update package changed"));
    w = with_update, w.update.mtime = 21;
    CHECK(decide(true, NULL, ID_110, "v1.1.0", &with_update, &w, &m, why) == EXTRACT_STALE);
    w = with_update, w.base.mtime = 11;
    CHECK(decide(true, NULL, ID_110, "v1.1.0", &with_update, &w, &m, why) == EXTRACT_STALE && strstr(why, "base game package changed"));
    w = with_update, stamp(&w.keys, "D:/other/prod.keys", 30, 30);
    CHECK(decide(true, NULL, ID_110, "v1.1.0", &with_update, &w, &m, why) == EXTRACT_STALE && strstr(why, "keys file changed"));
    // an update added to a base-only record of the same version (not a real game case, but the record must notice)
    w = base_only, stamp(&w.update, "C:/Rom/update.nsp", 500, 20);
    CHECK(decide(true, NULL, ID_100, "v1.0.0", &base_only, &w, &m, why) == EXTRACT_STALE && strstr(why, "update was added"));
    w = with_update, memset(&w.update, 0, sizeof w.update);
    CHECK(decide(true, NULL, ID_110, "v1.1.0", &with_update, &w, &m, why) == EXTRACT_STALE && strstr(why, "update was removed"));
    // files replaced behind the record's back
    CHECK(decide(true, NULL, ID_110, "v1.1.0", &base_only, &with_update, &m, why) == EXTRACT_STALE);

    // incomplete: from this selection (finish in place) or not (re-import)
    CHECK(decide(true, "the game data still needs local extraction", ID_100, "v1.0.0", &base_only, &base_only, &m, why)
          == EXTRACT_INCOMPLETE && m);
    CHECK(decide(true, "the game data image (romfs.bin) is missing", ID_110, "v1.1.0", NULL, &with_update, &m, why)
          == EXTRACT_INCOMPLETE && m);
    CHECK(decide(true, "the game data image (romfs.bin) is missing", ID_100, "v1.0.0", NULL, &with_update, &m, why)
          == EXTRACT_STALE && !m);
    // an unsupported main: stale against any selection
    CHECK(decide(true, "not a supported version", "0123456789abcdef", "", NULL, &with_update, &m, why) == EXTRACT_STALE);
}

// ---- files
static char g_tmp[600];
static void path(char *out, const char *rel) { snprintf(out, 1200, "%s/%s", g_tmp, rel); }
static void put(const char *rel, const char *text) {
    char p[1200];
    path(p, rel);
    FILE *f = fopen(p, "wb");
    if (f) fputs(text, f), fclose(f);
}
static bool has(const char *rel, const char *text) {
    char p[1200], buf[200] = "";
    path(p, rel);
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return !strcmp(buf, text);
}
static bool there(const char *rel) {
    char p[1200];
    path(p, rel);
    return import_exists(p);
}
static void dir(const char *rel) {
    char p[1200];
    path(p, rel);
    import_mkdir(p);
}
static bool remove_tree(const char *p) {
    struct stat st;
    if (stat(p, &st)) return true;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char q[1300];
            snprintf(q, sizeof q, "%s/%s", p, e->d_name);
            remove_tree(q);
        }
        if (d) closedir(d);
        return rmdir(p) == 0;
    }
    return remove(p) == 0;
}

static void make_old(void) {
    char p[1200];
    path(p, "extracted");
    remove_tree(p);
    dir("extracted"), dir("extracted/exefs"), dir("extracted/shader_catalog");
    put("extracted/exefs/main", "old main"), put("extracted/romfs.bin", "old romfs");
    put("extracted/romfs.src", "old src"), put("extracted/shader_catalog/manifest.json", "old catalog");
    put("extracted/Trirod.dll", "game code");  // (not part of the import: stays)
    dir("extracted/.import"), dir("extracted/.import/exefs");
    put("extracted/.import/exefs/main", "new main"), put("extracted/.import/romfs.bin", "new romfs");
    put("extracted/.import/import.txt", "record");
}

static void test_record(void) {
    char p[1200];
    path(p, "record.txt");
    ImportRecord a = sel("C:/Rom/My base = 1.nsp", "C:/Rom/update.nsp", "C:/Rom/prod.keys", ID_110, "v1.1.0"), b;
    a.base.size = 13534035968ull, a.base.mtime = 1764000000;
    CHECK(import_record_write(p, &a));
    CHECK(import_record_read(p, &b));
    CHECK(stamp_same(&a.base, &b.base) && stamp_same(&a.update, &b.update) && stamp_same(&a.keys, &b.keys));
    CHECK(!strcmp(b.build_id, ID_110) && !strcmp(b.version, "v1.1.0"));
    ImportRecord c = sel("C:/Rom/base.nsp", NULL, "C:/Rom/prod.keys", ID_100, "v1.0.0");
    CHECK(import_record_write(p, &c) && import_record_read(p, &b) && !b.update.path[0]);
    put("record.txt", "format=99\nbase=x\nbuild_id=y\n");  // a format this build doesn't read: no record
    CHECK(!import_record_read(p, &b));
    // the record never holds key material: only the keys file's name, size and time
    CHECK(import_record_write(p, &a));
    FILE *f = fopen(p, "r");
    char line[1400];
    int keys_lines = 0;
    while (f && fgets(line, sizeof line, f)) keys_lines += !strncmp(line, "keys", 4);
    if (f) fclose(f);
    CHECK(keys_lines == 3);
}

static const char *const SET[] = {"exefs", "romfs", "romfs.bin", "romfs.src", "shader_catalog", "import.txt"};

static void test_swap(void) {
    char game[1200], tmp[1200], prev[1200], why[300];
    path(game, "extracted"), path(tmp, "extracted/.import"), path(prev, "extracted/.previous");

    make_old();
    CHECK(import_swap(game, tmp, prev, SET, 6, remove_tree, why, sizeof why));
    CHECK(has("extracted/exefs/main", "new main") && has("extracted/romfs.bin", "new romfs"));
    CHECK(has("extracted/import.txt", "record"));
    CHECK(!there("extracted/romfs.src") && !there("extracted/shader_catalog"));  // (replaced: derived from the old files)
    CHECK(has("extracted/Trirod.dll", "game code") && !there("extracted/.previous"));

    // A failure while moving the new files in (the 2nd set entry can't be placed: its folder doesn't exist) puts
    // everything back: the old extraction is untouched and the new files are still in .import
    make_old();
    dir("extracted/.import/sub"), put("extracted/.import/sub/x", "new x");
    static const char *const BAD[] = {"exefs", "sub/x", "romfs.bin", "romfs.src", "shader_catalog"};
    put("extracted/.import/sub/x", "new x");
    CHECK(!import_swap(game, tmp, prev, BAD, 5, remove_tree, why, sizeof why));
    printf("  (expected failure: %s)\n", why);
    CHECK(has("extracted/exefs/main", "old main") && has("extracted/romfs.bin", "old romfs"));
    CHECK(has("extracted/romfs.src", "old src") && has("extracted/shader_catalog/manifest.json", "old catalog"));
    CHECK(has("extracted/.import/exefs/main", "new main") && has("extracted/.import/romfs.bin", "new romfs"));
    CHECK(!there("extracted/.previous"));

    // A .previous left by an interrupted run is cleared first, not mixed into the swap
    make_old();
    dir("extracted/.previous"), dir("extracted/.previous/exefs"), put("extracted/.previous/exefs/main", "stale leftover");
    CHECK(import_swap(game, tmp, prev, SET, 6, remove_tree, why, sizeof why));
    CHECK(has("extracted/exefs/main", "new main") && !there("extracted/.previous"));
}

int main(void) {
    const char *t = getenv("TEMP");
    snprintf(g_tmp, sizeof g_tmp, "%s/test_import_state", t ? t : "/tmp");
    remove_tree(g_tmp);
    import_mkdir(g_tmp);
    test_decide();
    test_record();
    test_swap();
    remove_tree(g_tmp);
    if (failures) { printf("%d of %d checks failed\n", failures, checks); return 1; }
    printf("test_import_state: all %d checks passed\n", checks);
    return 0;
}
