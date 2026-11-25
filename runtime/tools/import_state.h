// What extracted/ was imported from, and whether it still matches the game files the player has chosen (trbuild
// check / files). The record (extracted/import.txt) holds the chosen packages' and keys file's path, size and
// modification time, and the extracted main's build ID; never key material or game data. A changed selection
// (another base, an update added, removed or replaced, another keys file) makes the extraction stale; trbuild then
// imports into a temporary folder and swaps the new files in only after the import succeeded (import_swap), so a
// failed import leaves the working extraction as it was. Header-only so tools/test_import_state.c can test it alone.
#pragma once
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define import_mkdir(p) _mkdir(p)
#else
#define import_mkdir(p) mkdir(p, 0755)
#endif

#define IMPORT_RECORD "import.txt"
#define IMPORT_FORMAT 1

typedef struct {
    char path[1100];  // "" = none (no update chosen)
    uint64_t size;
    int64_t mtime;
} FileStamp;

typedef struct {
    FileStamp base, update, keys;
    char build_id[17];  // main's build ID, first 8 bytes in hex
    char version[16];   // "v1.0.0", "v1.1.0", ...
} ImportRecord;

typedef enum { EXTRACT_NONE, EXTRACT_CURRENT, EXTRACT_STALE, EXTRACT_INCOMPLETE } ExtractState;

static inline const char *extract_state_name(ExtractState s) {
    return s == EXTRACT_CURRENT ? "current" : s == EXTRACT_STALE ? "stale" : s == EXTRACT_INCOMPLETE ? "incomplete" : "none";
}

static inline bool file_stamp(const char *path, FileStamp *s) {
    memset(s, 0, sizeof *s);
    if (!path || !*path) return true;
    struct stat st;
    if (stat(path, &st)) return false;
    snprintf(s->path, sizeof s->path, "%s", path);
    s->size = (uint64_t)st.st_size;
    s->mtime = (int64_t)st.st_mtime;
    return true;
}

// Windows paths: one file whatever the case or slash direction
static inline bool import_path_same(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a == '\\' ? '/' : (char)tolower((unsigned char)*a), y = *b == '\\' ? '/' : (char)tolower((unsigned char)*b);
        if (x != y) return false;
        if (!x) return true;
    }
}

static inline bool stamp_same(const FileStamp *a, const FileStamp *b) {
    if (!a->path[0] || !b->path[0]) return !a->path[0] && !b->path[0];
    return import_path_same(a->path, b->path) && a->size == b->size && a->mtime == b->mtime;
}

// ---- the record file
static inline bool import_record_write(const char *path, const ImportRecord *r) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    const FileStamp *s[3] = {&r->base, &r->update, &r->keys};
    static const char *const name[3] = {"base", "update", "keys"};
    fprintf(f, "format=%d\n", IMPORT_FORMAT);
    for (int i = 0; i < 3; i++)
        fprintf(f, "%s=%s\n%s_size=%llu\n%s_mtime=%lld\n", name[i], s[i]->path, name[i], (unsigned long long)s[i]->size,
                name[i], (long long)s[i]->mtime);
    fprintf(f, "build_id=%s\nversion=%s\n", r->build_id, r->version);
    bool ok = fclose(f) == 0;
    remove(path);
    if (ok && rename(tmp, path) != 0) ok = false;
    if (!ok) remove(tmp);
    return ok;
}

// false = no record, or not one this format reads (the extraction is then judged by its version alone)
static inline bool import_record_read(const char *path, ImportRecord *r) {
    memset(r, 0, sizeof *r);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[1400];
    int format = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        FileStamp *s = !strncmp(k, "base", 4) ? &r->base : !strncmp(k, "update", 6) ? &r->update
                     : !strncmp(k, "keys", 4) ? &r->keys : NULL;
        const char *suffix = s ? strchr(k, '_') : NULL;
        if (!strcmp(k, "format")) format = atoi(v);
        else if (!strcmp(k, "build_id")) snprintf(r->build_id, sizeof r->build_id, "%s", v);
        else if (!strcmp(k, "version")) snprintf(r->version, sizeof r->version, "%s", v);
        else if (s && !suffix) snprintf(s->path, sizeof s->path, "%s", v);
        else if (s && !strcmp(suffix, "_size")) s->size = strtoull(v, NULL, 10);
        else if (s && !strcmp(suffix, "_mtime")) s->mtime = strtoll(v, NULL, 10);
    }
    fclose(f);
    return format == IMPORT_FORMAT && r->build_id[0] && r->base.path[0];
}

// ---- the decision
// have_files: anything extracted at all (exefs/main). files_problem: NULL = complete, else what is missing.
// extracted_id / extracted_version: the extracted main's build ID (hex) and version label ("" = not a supported one).
// rec: the import record (NULL = none, an extraction from before records). want: the current selection, with the
// build ID its main will have (NULL = nothing chosen).
// *matches: the extraction (complete or not) comes from this selection, so finishing it in place is right.
static inline ExtractState import_decide(bool have_files, const char *files_problem, const char *extracted_id,
                                         const char *extracted_version, const ImportRecord *rec,
                                         const ImportRecord *want, bool *matches, char *why, size_t n) {
    *matches = false;
    const char *was = extracted_version && *extracted_version ? extracted_version : "an unsupported version";
    if (!have_files) { snprintf(why, n, "nothing extracted yet"); return EXTRACT_NONE; }
    if (!want) {  // (no files chosen: what is there is what the player has)
        *matches = true;
        if (files_problem) { snprintf(why, n, "%s", files_problem); return EXTRACT_INCOMPLETE; }
        snprintf(why, n, "extracted %s", was);
        return EXTRACT_CURRENT;
    }
    if (extracted_id && *extracted_id && strcmp(extracted_id, want->build_id)) {
        snprintf(why, n, "the extracted files are %s, the chosen files make %s", was, want->version);
        return EXTRACT_STALE;
    }
    if (rec) {
        if (strcmp(rec->build_id, extracted_id ? extracted_id : "")) {
            snprintf(why, n, "the extracted files don't match their import record");
            return EXTRACT_STALE;
        }
        const char *changed = !stamp_same(&rec->base, &want->base) ? "the base game package changed"
                            : !rec->update.path[0] && want->update.path[0] ? "an update was added"
                            : rec->update.path[0] && !want->update.path[0] ? "the update was removed"
                            : !stamp_same(&rec->update, &want->update) ? "the update package changed"
                            : !stamp_same(&rec->keys, &want->keys) ? "the keys file changed" : NULL;
        if (changed) { snprintf(why, n, "%s since the files were extracted", changed); return EXTRACT_STALE; }
    }
    *matches = true;
    if (files_problem) { snprintf(why, n, "%s", files_problem); return EXTRACT_INCOMPLETE; }
    snprintf(why, n, rec ? "%s, from the chosen files" : "%s, the version the chosen files make (no import record yet)",
             want->version);
    return EXTRACT_CURRENT;
}

// ---- replacing the extraction
typedef bool (*ImportRemoveTree)(const char *path);  // rm -rf (a file or a folder)

static inline bool import_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

// Move names[] from new_dir into game_dir, the old ones aside into prev_dir first; on any failure everything moved is
// put back (the old extraction stays). Renames only: new_dir and prev_dir must be on game_dir's volume (inside it).
static inline bool import_swap(const char *game_dir, const char *new_dir, const char *prev_dir, const char *const *names,
                               int count, ImportRemoveTree remove_tree, char *why, size_t n) {
    char a[1300], b[1300];
    if (import_exists(prev_dir) && !remove_tree(prev_dir)) {
        snprintf(why, n, "couldn't clear %s from an earlier import", prev_dir);
        return false;
    }
    if (import_mkdir(prev_dir) != 0) { snprintf(why, n, "couldn't create %s (%s)", prev_dir, strerror(errno)); return false; }
    bool moved[32] = {0}, placed[32] = {0};
    if (count > 32) count = 32;
    int i = 0;
    bool ok = true;
    for (; i < count && ok; i++) {  // the old files aside
        snprintf(a, sizeof a, "%s/%s", game_dir, names[i]);
        snprintf(b, sizeof b, "%s/%s", prev_dir, names[i]);
        if (!import_exists(a)) continue;
        if (rename(a, b) != 0) { snprintf(why, n, "couldn't move the old %s aside (%s)", names[i], strerror(errno)); ok = false; }
        else moved[i] = true;
    }
    for (i = 0; i < count && ok; i++) {  // the new ones in
        snprintf(a, sizeof a, "%s/%s", new_dir, names[i]);
        snprintf(b, sizeof b, "%s/%s", game_dir, names[i]);
        if (!import_exists(a)) continue;
        if (rename(a, b) != 0) { snprintf(why, n, "couldn't move the new %s into place (%s)", names[i], strerror(errno)); ok = false; }
        else placed[i] = true;
    }
    if (!ok) {  // back as it was
        for (i = 0; i < count; i++) {
            if (placed[i]) {
                snprintf(a, sizeof a, "%s/%s", game_dir, names[i]);
                snprintf(b, sizeof b, "%s/%s", new_dir, names[i]);
                rename(a, b);
            }
            if (moved[i]) {
                snprintf(a, sizeof a, "%s/%s", prev_dir, names[i]);
                snprintf(b, sizeof b, "%s/%s", game_dir, names[i]);
                rename(a, b);
            }
        }
        remove_tree(prev_dir);
        return false;
    }
    if (!remove_tree(prev_dir)) fprintf(stderr, "[import] couldn't remove %s (the replaced files)\n", prev_dir);
    return true;
}
