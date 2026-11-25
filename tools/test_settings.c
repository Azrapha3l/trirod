#include <assert.h>
#ifdef _WIN32
#include <fcntl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/settings.h"

bool bind_settings_load(const char *key, const char *val) { (void)key; (void)val; return false; }
void bind_settings_save(FILE *f) { (void)f; }

int main(void) {
#ifdef _WIN32
    _fmode = _O_BINARY;  // as the game (runtime/platform/windows.c): "r" keeps CRs, so the CRLF case below is real
#endif
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
    g_settings.res_mode = RES_DYNAMIC;
    settings_save();
    {   // the same file with CRLF line ends (as Notepad or PowerShell write it): every value must still load
        FILE *f = fopen("build/video-options-check/settings-roundtrip.ini", "rb");
        assert(f);
        static char buf[8192], crlf[16384];
        size_t n = fread(buf, 1, sizeof buf, f), m = 0;
        fclose(f);
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == '\n' && (i == 0 || buf[i - 1] != '\r')) crlf[m++] = '\r';
            crlf[m++] = buf[i];
        }
        f = fopen("build/video-options-check/settings-roundtrip.ini", "wb");
        assert(f && fwrite(crlf, 1, m, f) == m);
        fclose(f);
    }
    g_settings.gpu_id[0] = 0;
    g_settings.frame_rate = FPS_GAME;
    g_settings.res_mode = RES_MATCH;
    settings_load();
    assert(!strcmp(g_settings.gpu_id, "00112233445566778899aabbccddeeff"));
    assert(g_settings.frame_rate == FPS_210);
    assert(g_settings.res_mode == RES_DYNAMIC);
    remove("build/video-options-check/settings-roundtrip.ini");
    puts("settings frame-rate choices, CRLF file: PASS");
    return 0;
}
