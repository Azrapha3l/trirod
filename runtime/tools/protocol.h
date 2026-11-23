// The progress protocol of Trirod's headless tools (trbuild.exe, Trirod-ShaderPrep.exe), read by Trirod-Launcher.exe.
// docs/TOOL-PROTOCOL.md is the reference. Events are whole lines on stdout that start with "@@"; anything else on
// stdout and everything on stderr is diagnostic output, which the launcher only shows and keeps in its log. Keeping
// the two apart lets the launcher trust the events (stages, percentages, errors, completion) without parsing logs.
#pragma once
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define TOOL_PROTOCOL 1

static pthread_mutex_t tp_lock = PTHREAD_MUTEX_INITIALIZER;

// One event line: "@@<fmt>". Newlines inside a value would start a new line: they become spaces.
static void tp_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tp_event(const char *fmt, ...) {
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    for (char *c = line; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
    pthread_mutex_lock(&tp_lock);
    fprintf(stdout, "@@%s\n", line);
    fflush(stdout);
    pthread_mutex_unlock(&tp_lock);
}

// "@@progress <0..1> [text]", at most every 1/200 of the step unless the text changes (a compile loop calls it 20
// times a second)
static void tp_progress(double f, const char *text) {
    static double last = -1;
    static char last_text[256];
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    const char *t = text ? text : "";
    if (f - last < 0.005 && f != 1 && !strcmp(t, last_text)) return;
    last = f;
    snprintf(last_text, sizeof last_text, "%s", t);
    if (*t) tp_event("progress %.3f %s", f, t);
    else tp_event("progress %.3f", f);
}

static void tp_stage(const char *id, const char *text) {
    tp_event("stage %s %s", id, text);
    tp_progress(0, NULL);
}

// The argument list as one readable line (quoted where it has spaces), for "@@tool ... start" and the log
static void tp_join(const char *const *argv, char *out, size_t n) {
    size_t used = 0;
    out[0] = 0;
    for (int i = 0; argv && argv[i] && used + 4 < n; i++) {
        bool q = strchr(argv[i], ' ') != NULL;
        int w = snprintf(out + used, n - used, "%s%s%s%s", i ? " " : "", q ? "\"" : "", argv[i], q ? "\"" : "");
        if (w < 0) break;
        used += (size_t)w;
        if (used >= n) { used = n - 1; break; }
    }
}

// The last `lines` lines of a log file to stderr (a failed child's own words, for the launcher's details)
static void tp_log_tail(const char *path, int lines) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    static char buf[16384];
    fseek(f, 0, SEEK_END);
    long size = ftell(f), from = size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0;
    fseek(f, from, SEEK_SET);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *start = buf + n;
    for (int k = 0; start > buf && k <= lines;) if (*--start == '\n') k++;
    if (*start == '\n') start++;
    fprintf(stderr, "---- %s (last lines) ----\n%s%s---- end of %s ----\n", path, start,
            n && buf[n - 1] != '\n' ? "\n" : "", path);
}
