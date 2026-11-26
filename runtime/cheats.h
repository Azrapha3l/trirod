// Optional single-player cheats (runtime/cheats.c). The flag word and the trace hook the patched game code uses are
// in cpu.h (g_cheats, rt_cheat_trace).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Boot, after the settings are loaded: is this build of the game one tools/patches.py has cheat sites for (exefs/main's
// build id)? Then cheats_sync.
void cheats_init(const char *exefs);
bool cheats_available(void);
// g_settings.cheat_* -> g_cheats (the menu calls it on every change: off takes effect at the next store)
void cheats_sync(void);

#ifdef __cplusplus
}
#endif
