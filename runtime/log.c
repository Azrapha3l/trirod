// Log levels (rt.h LOG_VERBOSE / LOG_FIRST). Normal play keeps the log to errors, warnings and one-time facts; the
// detailed diagnostics that investigations added (statistics every two seconds, notes on draws and textures, every
// call of a stubbed service command) come back with the diagnostic console or EOWRECOMP_VERBOSE=1. Only what is logged
// changes: nothing here decides how the game runs.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

static bool env_on(const char *name) {
    const char *e = getenv(name);
    return e && *e && strcmp(e, "0") != 0;
}

bool rt_verbose(void) {
    static int v = -1;  // (read once: the platform layer sets EOWRECOMP_CONSOLE for --console before anything logs)
    if (v < 0) v = env_on("EOWRECOMP_VERBOSE") || env_on("EOWRECOMP_CONSOLE");
    return v;
}

bool rt_log_first(const char *site, uint32_t key) {
    if (rt_verbose()) return true;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t seen[1024];  // (open addressing; once full, nothing more is logged)
    uint64_t h = 0xcbf29ce484222325ull;
    for (const char *s = site ? site : ""; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001b3ull;
    h = ((h ^ key) * 0x100000001b3ull) | 1;  // (0 marks an empty slot)
    pthread_mutex_lock(&lock);
    bool first = false;
    for (uint32_t i = (uint32_t)h & 1023, n = 0; n < 1024; i = (i + 1) & 1023, n++) {
        if (seen[i] == h) break;
        if (!seen[i]) { seen[i] = h; first = true; break; }
    }
    pthread_mutex_unlock(&lock);
    return first;
}
