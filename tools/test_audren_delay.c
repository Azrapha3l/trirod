// The audio renderer's delay effect (runtime/services/audren_delay.h): impulse responses with known answers. No game
// data: parameter blocks are built at DelayInfo::ParameterVersion1's offsets (yuzu audio_core/renderer/effect/delay.h).
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../runtime/services/audren_delay.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)

static int32_t q14(float v) { return (int32_t)lrintf(v * 16384.0f); }

typedef struct { uint16_t ch; uint32_t time, time_max; float rate_hz, in, fb, wet, dry, spread, lowpass; } Spec;

static void block(uint8_t *d, const Spec *s) {
    memset(d, 0, 0x40);
    for (int c = 0; c < 6; c++) d[c] = (uint8_t)c, d[6 + c] = (uint8_t)(6 + c);  // inputs 0.., outputs 6..
    memcpy(d + 0x0C, &s->ch, 2), memcpy(d + 0x0E, &s->ch, 2);
    memcpy(d + 0x10, &s->time_max, 4), memcpy(d + 0x14, &s->time, 4);
    int32_t v[7] = {q14(s->rate_hz), q14(s->in), q14(s->fb), q14(s->wet), q14(s->dry), q14(s->spread), q14(s->lowpass)};
    memcpy(d + 0x18, v, sizeof v);
}

#define N 4096
static float bin[6][N], bout[6][N];
static void run(AudrenDelay *dl, int impulse_ch) {
    memset(bin, 0, sizeof bin), memset(bout, 0, sizeof bout);
    bin[impulse_ch][0] = 1.0f;
    float *in[6], *out[6];
    for (int c = 0; c < 6; c++) in[c] = bin[c], out[c] = bout[c];
    audren_delay_process(dl, in, out, N);
}

int main(void) {
    uint8_t d[0x40];
    AudrenDelay dl;
    memset(&dl, 0, sizeof dl);

    // Parameter block read at the reference offsets
    Spec s = {1, 10, 100, 48000, 1, 0.5f, 1, 0, 0, 0};
    block(d, &s);
    AudrenDelayParam p;
    audren_delay_param_read(d, &p);
    CHECK(p.ch == 1 && p.ch_max == 1 && p.time_ms == 10 && p.time_max_ms == 100 && p.input[2] == 2 && p.output[2] == 8);
    CHECK(p.feedback == q14(0.5f) && p.wet == q14(1) && p.dry == 0);
    CHECK(audren_delay_spms(&p) == 48);  // 48 kHz in Hz
    p.sample_rate = q14(48);
    CHECK(audren_delay_spms(&p) == 48);  // ... or given in kHz

    // Mono echo: 10 ms at 48 kHz = 480 samples; the line holds in_gain * x, then decays by feedback * 0.97998 a lap
    CHECK(audren_delay_update(&dl, d, true));
    run(&dl, 0);
    float fb = 0.5f * 0.97998046875f;
    CHECK(NEAR(bout[0][0], 0) && NEAR(bout[0][479], 0));
    CHECK(NEAR(bout[0][480], 1.0f) && NEAR(bout[0][481], 0));
    CHECK(NEAR(bout[0][960], fb) && NEAR(bout[0][1440], fb * fb) && NEAR(bout[0][1920], fb * fb * fb));
    printf("  mono echo: %.4f at 480, %.4f at 960, %.4f at 1440\n", bout[0][480], bout[0][960], bout[0][1440]);

    // Dry only: the input passes, nothing else
    s.dry = 1, s.wet = 0;
    block(d, &s);
    audren_delay_update(&dl, d, true);
    run(&dl, 0);
    CHECK(NEAR(bout[0][0], 1) && NEAR(bout[0][480], 0) && NEAR(bout[0][960], 0));

    // Feedback 1 still decays (0.97998 a lap): the loop can't grow
    s = (Spec){1, 5, 100, 48000, 1, 1.0f, 1, 0, 0, 0};
    block(d, &s);
    audren_delay_update(&dl, d, true);
    run(&dl, 0);
    float peak_late = 0;  // (samples 3000.. are laps 13..17 of 240 samples: 0.97998^12 at the very most)
    for (int i = 3000; i < N; i++) peak_late = fmaxf(peak_late, fabsf(bout[0][i]));
    CHECK(peak_late <= powf(0.97998046875f, 12) + 1e-4f && peak_late >= powf(0.97998046875f, 17) - 1e-4f);
    CHECK(NEAR(bout[0][240 * 13], powf(0.97998046875f, 12)));

    // Stereo, full spread: the second echo crosses to the other channel (direct gain feedback * (1 - 1) = 0)
    s = (Spec){2, 10, 100, 48000, 1, 0.5f, 1, 0, 1.0f, 0};
    block(d, &s);
    audren_delay_update(&dl, d, true);
    run(&dl, 0);
    CHECK(NEAR(bout[0][480], 1) && NEAR(bout[1][480], 0));
    CHECK(NEAR(bout[0][960], 0) && NEAR(bout[1][960], fb));
    CHECK(NEAR(bout[0][1440], fb * fb) && NEAR(bout[1][1440], 0));
    // ... no spread: it stays
    s.spread = 0;
    block(d, &s);
    audren_delay_update(&dl, d, true);
    run(&dl, 0);
    CHECK(NEAR(bout[0][960], fb) && NEAR(bout[1][960], 0));

    // Lowpass: the echo is smoothed (first sample scaled by 1 - 0.95 * amount, then a decaying tail)
    s = (Spec){1, 10, 100, 48000, 1, 0, 1, 0, 0, 0.5f};
    block(d, &s);
    audren_delay_update(&dl, d, true);
    run(&dl, 0);
    float g = 1.0f - 0.5f * 0.949951171875f;
    CHECK(NEAR(bout[0][480], g) && NEAR(bout[0][481], g * (1 - g)) && bout[0][482] < bout[0][481]);

    // 6 channels: the LFE (3) feeds only itself; channel 0 feeds 2 and 4 (cross) and itself (direct)
    s = (Spec){6, 10, 100, 48000, 1, 0.5f, 1, 0, 0.5f, 0};
    block(d, &s);
    audren_delay_update(&dl, d, true);
    run(&dl, 0);
    CHECK(bout[2][960] > 0 && bout[4][960] > 0 && NEAR(bout[1][960], 0) && NEAR(bout[3][960], 0) && NEAR(bout[5][960], 0));
    CHECK(NEAR(bout[0][960], fb * 0.5f) && NEAR(bout[2][960], fb * 0.25f));  // direct fb*(1-0.5), cross halved spread

    // Updates: a gain change keeps the line (no re-initialization); a new delay time re-initializes it
    s = (Spec){1, 10, 100, 48000, 1, 0.5f, 1, 0, 0, 0};
    block(d, &s);
    CHECK(audren_delay_update(&dl, d, true));
    s.wet = 0.5f;
    block(d, &s);
    CHECK(!audren_delay_update(&dl, d, false) && NEAR(dl.wet, 0.5f));
    s.time = 20;
    block(d, &s);
    CHECK(audren_delay_update(&dl, d, false) && dl.len == 960);
    // a delay time over its maximum is held at the maximum; invalid channel counts are refused
    s.time = 500, s.time_max = 100;
    block(d, &s);
    CHECK(audren_delay_update(&dl, d, false) && dl.len == 500 * 48);
    s.ch = 3;
    block(d, &s);
    CHECK(!audren_delay_update(&dl, d, true));

    audren_delay_free(&dl);
    if (failures) { printf("%d of %d checks failed\n", failures, checks); return 1; }
    printf("test_audren_delay: all %d checks passed\n", checks);
    return 0;
}
