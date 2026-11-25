// The audio renderer's delay effect (EffectType 3), in floats like the reverb in audio.c. Parameters (the effect
// entry's data at +0x20; DelayInfo::ParameterVersion1/2, yuzu audio_core/renderer/effect/delay.h): input / output mix
// buffer indices [6], channel count max / count, delay time max / delay time (ms), and Q14 sample rate, in gain,
// feedback gain, wet gain, dry gain, channel spread, lowpass amount; ParameterState at 0x34. Processing follows the
// DSP's description (yuzu command/effect/delay.cpp): per channel one delay line; what enters it is the input times the
// in gain plus the delayed samples mixed across channels by a feedback matrix (direct gain feedback * (1 - spread),
// cross gain feedback * spread, halved for 4 / 6 channels), through a one-pole lowpass; the output is the input times
// the dry gain plus the delayed sample times the wet gain. Header-only for tools/test_audren_delay.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Longest line allocated (the parameter asks for its own maximum; this only bounds a corrupt one). Echoes of Wisdom asks
// for 160 ms with a 2500 ms maximum (6 channels at 48 kHz: 2.9 MB of floats).
#define AUDREN_DELAY_MAX_MS 10000u

typedef struct {
    uint8_t input[6], output[6];
    uint16_t ch_max, ch;
    uint32_t time_max_ms, time_ms;
    int32_t sample_rate, in_gain, feedback, wet, dry, spread, lowpass;  // Q14
} AudrenDelayParam;

typedef struct {
    bool ready;
    uint8_t raw[0x34];  // the parameter bytes last applied (the state byte at 0x34 excluded)
    AudrenDelayParam p;
    float *line[6];
    uint32_t len, cap, pos;
    float direct, cross, feedback, lp_gain, lp_fb, in_gain, wet, dry;
    float lp_z[6];
} AudrenDelay;

static inline float audren_q14(int32_t v) { return v / 16384.0f; }

static inline bool audren_delay_channels_ok(uint16_t n) { return n == 1 || n == 2 || n == 4 || n == 6; }

static inline void audren_delay_param_read(const uint8_t *d, AudrenDelayParam *p) {
    memcpy(p->input, d, 6), memcpy(p->output, d + 6, 6);
    memcpy(&p->ch_max, d + 0x0C, 2), memcpy(&p->ch, d + 0x0E, 2);
    memcpy(&p->time_max_ms, d + 0x10, 4), memcpy(&p->time_ms, d + 0x14, 4);
    int32_t *q[7] = {&p->sample_rate, &p->in_gain, &p->feedback, &p->wet, &p->dry, &p->spread, &p->lowpass};
    for (int i = 0; i < 7; i++) memcpy(q[i], d + 0x18 + 4 * i, 4);
}

// Samples per millisecond: the parameter is the rate in Hz (Q14); a value under 1000 is taken as kHz
static inline uint32_t audren_delay_spms(const AudrenDelayParam *p) {
    float r = audren_q14(p->sample_rate);
    uint32_t spms = r >= 1000 ? (uint32_t)(r / 1000 + 0.5f) : (uint32_t)(r + 0.5f);
    return spms && spms <= 192 ? spms : 48;
}

// Gains (SetDelayEffectParameter): kept below 1 so the loop decays
static inline void audren_delay_gains(AudrenDelay *dl) {
    const AudrenDelayParam *p = &dl->p;
    float spread = audren_q14(p->spread);
    dl->feedback = audren_q14(p->feedback) * 0.97998046875f;
    dl->direct = dl->feedback * (1.0f - spread);
    if (p->ch == 4 || p->ch == 6) spread *= 0.5f;
    dl->cross = spread * dl->feedback;
    dl->lp_fb = audren_q14(p->lowpass) * 0.949951171875f;
    dl->lp_gain = 1.0f - dl->lp_fb;
    dl->in_gain = audren_q14(p->in_gain), dl->wet = audren_q14(p->wet), dl->dry = audren_q14(p->dry);
}

static inline void audren_delay_free(AudrenDelay *dl) {
    for (int c = 0; c < 6; c++) free(dl->line[c]), dl->line[c] = NULL;
    dl->ready = false, dl->cap = 0;
}

// (Re)initialize: lines cleared and sized for the delay time. false = invalid parameters (the effect then passes through)
static inline bool audren_delay_init(AudrenDelay *dl) {
    const AudrenDelayParam *p = &dl->p;
    if (!audren_delay_channels_ok(p->ch)) return false;
    uint32_t spms = audren_delay_spms(p);
    uint32_t max_ms = p->time_max_ms > p->time_ms ? p->time_max_ms : p->time_ms;
    if (max_ms > AUDREN_DELAY_MAX_MS) max_ms = AUDREN_DELAY_MAX_MS;
    uint32_t cap = max_ms * spms + 1, len = (p->time_ms < max_ms ? p->time_ms : max_ms) * spms;
    if (cap != dl->cap) {
        audren_delay_free(dl);
        for (int c = 0; c < 6; c++)
            if (!(dl->line[c] = (float *)calloc(cap, sizeof(float)))) { audren_delay_free(dl); return false; }
        dl->cap = cap;
    } else
        for (int c = 0; c < 6; c++) memset(dl->line[c], 0, cap * sizeof(float));
    dl->len = len ? len : 1;
    dl->pos = 0;
    memset(dl->lp_z, 0, sizeof dl->lp_z);
    audren_delay_gains(dl);
    dl->ready = true;
    return true;
}

// The feedback matrix: how much of channel j's delayed sample goes back into channel c's line
static inline float audren_delay_matrix(const AudrenDelay *dl, int ch, int j, int c) {
    float d = dl->direct, x = dl->cross;
    if (ch == 1) return dl->feedback;
    if (ch == 2) return j == c ? d : x;
    if (ch == 4) {  // (front L, front R, rear L, rear R: diagonal neighbours only)
        static const int8_t m[4][4] = {{2, 1, 1, 0}, {1, 2, 0, 1}, {1, 0, 2, 1}, {0, 1, 1, 2}};
        return m[j][c] == 2 ? d : m[j][c] == 1 ? x : 0;
    }
    // 6 channels: the LFE (3) only feeds itself, with the plain feedback gain
    static const int8_t m[6][6] = {{2, 0, 1, 0, 1, 0}, {0, 2, 1, 0, 0, 1}, {1, 1, 2, 0, 0, 0},
                                   {0, 0, 0, 3, 0, 0}, {1, 0, 0, 0, 2, 1}, {0, 1, 0, 0, 1, 2}};
    return m[j][c] == 3 ? audren_q14(dl->p.feedback) : m[j][c] == 2 ? d : m[j][c] == 1 ? x : 0;
}

static inline void audren_delay_process(AudrenDelay *dl, float *const *in, float *const *out, uint32_t n) {
    int ch = dl->p.ch;
    for (uint32_t s = 0; s < n; s++) {
        float x[6], d[6];
        for (int c = 0; c < ch; c++) x[c] = in[c][s], d[c] = dl->line[c][dl->pos];
        for (int c = 0; c < ch; c++) {
            float fb = 0;
            for (int j = 0; j < ch; j++) fb += d[j] * audren_delay_matrix(dl, ch, j, c);
            dl->lp_z[c] = (x[c] * dl->in_gain + fb) * dl->lp_gain + dl->lp_z[c] * dl->lp_fb;
            dl->line[c][dl->pos] = dl->lp_z[c];
        }
        for (int c = 0; c < ch; c++) out[c][s] = x[c] * dl->dry + d[c] * dl->wet;
        if (++dl->pos >= dl->len) dl->pos = 0;
    }
}

// An effect entry's data (+0x20) seen this update: initialize on a new effect or new geometry, else retune the gains.
// Returns true when the line was (re)initialized.
static inline bool audren_delay_update(AudrenDelay *dl, const uint8_t *data, bool is_new) {
    AudrenDelayParam p;
    audren_delay_param_read(data, &p);
    if (!audren_delay_channels_ok(p.ch) || !audren_delay_channels_ok(p.ch_max) || p.ch > p.ch_max) return false;
    bool geometry = p.ch != dl->p.ch || p.time_ms != dl->p.time_ms || p.time_max_ms != dl->p.time_max_ms ||
                    p.sample_rate != dl->p.sample_rate;
    if (is_new || !dl->ready || geometry) {
        memcpy(dl->raw, data, sizeof dl->raw);
        dl->p = p;
        return audren_delay_init(dl);
    }
    if (memcmp(dl->raw, data, sizeof dl->raw)) {
        memcpy(dl->raw, data, sizeof dl->raw);
        dl->p = p;
        audren_delay_gains(dl);
    }
    return false;
}
