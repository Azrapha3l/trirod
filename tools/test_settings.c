#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/settings.h"

bool bind_settings_load(const char *key, const char *val) { (void)key; (void)val; return false; }
void bind_settings_save(FILE *f) { (void)f; }

int main(void) {
    assert(settings_frame_rate_hz(FPS_GAME, 144) == 30);  // 30 FPS (Game) presentation cap
    assert(settings_frame_rate_hz(FPS_60, 144) == 60);
    assert(settings_frame_rate_hz(FPS_90, 144) == 90);
    assert(settings_frame_rate_hz(FPS_120, 144) == 120);
    assert(settings_frame_rate_hz(FPS_150, 144) == 150);
    assert(settings_frame_rate_hz(FPS_180, 144) == 180);
    assert(settings_frame_rate_hz(FPS_210, 144) == 210);
    assert(settings_frame_rate_hz(FPS_240, 144) == 240);
    assert(settings_frame_rate_hz(FPS_MONITOR, 144) == 144);
    assert(settings_frame_rate_hz(FPS_MONITOR, 0) == 60);  // safe display-query fallback
    assert(settings_frame_rate_hz(-1, 144) == 0);
    assert(putenv("EOWRECOMP_SETTINGS=build/video-options-check/settings-roundtrip.ini") == 0);
    strcpy(g_settings.gpu_id, "00112233445566778899aabbccddeeff");
    g_settings.frame_rate = FPS_210;
    settings_save();
    g_settings.gpu_id[0] = 0;
    g_settings.frame_rate = FPS_GAME;
    settings_load();
    assert(!strcmp(g_settings.gpu_id, "00112233445566778899aabbccddeeff"));
    assert(g_settings.frame_rate == FPS_210);
    remove("build/video-options-check/settings-roundtrip.ini");
    puts("settings frame-rate choices: PASS");
    return 0;
}
