// The audio renderer's voice input in both layouts (runtime/services/audren_voice.h): buffers built at the field offsets
// yuzu documents for VoiceInfo::InParameter (0x170) and InParameter2 (0x188, REV15), parsed back. No game data.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../runtime/services/audren_voice.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void put32(uint8_t *p, uint32_t off, uint32_t v) { memcpy(p + off, &v, 4); }
static void put64(uint8_t *p, uint32_t off, uint64_t v) { memcpy(p + off, &v, 8); }
static void putf(uint8_t *p, uint32_t off, float v) { memcpy(p + off, &v, 4); }
static void put16(uint8_t *p, uint32_t off, int16_t v) { memcpy(p + off, &v, 2); }

// The offsets below are copied from voice_info.h's /* 0x... */ comments, not from audren_voice.h
static void fill(uint8_t *p, bool v2) {
    memset(p, 0, 0x188);
    put32(p, 0x000, 7);                // id
    p[0x008] = 1, p[0x009] = 1;        // is_new, in_use
    p[0x00A] = 1, p[0x00B] = 2;        // play_state Started, sample_format PcmInt16
    put32(p, 0x00C, 32000);
    put32(p, 0x018, 2);                // channel_count
    putf(p, 0x01C, 1.25f), putf(p, 0x020, 0.5f);
    if (v2) {  // BiquadFilterParameter2 at 0x24, 0x18 each: enabled, 3 reserved, f32 b[3], f32 a[2]
        p[0x024] = 1;
        putf(p, 0x028, 0.5f), putf(p, 0x02C, 0.25f), putf(p, 0x030, -0.125f), putf(p, 0x034, 1.5f), putf(p, 0x038, -0.75f);
        p[0x03C] = 0;
    } else {   // BiquadFilterParameter at 0x24, 0xC each: enabled, s16 b[3] at +2, s16 a[2] at +8 (Q14)
        p[0x024] = 1;
        put16(p, 0x026, 8192), put16(p, 0x028, 4096), put16(p, 0x02A, -2048), put16(p, 0x02C, 24576), put16(p, 0x02E, -12288);
    }
    uint32_t wbc = v2 ? 0x054 : 0x03C, src = v2 ? 0x060 : 0x048, mix = v2 ? 0x070 : 0x058, spl = v2 ? 0x074 : 0x05C;
    uint32_t wbs = v2 ? 0x078 : 0x060, chans = v2 ? 0x158 : 0x140, drop = v2 ? 0x170 : 0x158, flush = v2 ? 0x171 : 0x159;
    uint32_t flags = v2 ? 0x174 : 0x15C;
    put32(p, wbc, 3);
    put64(p, src, 0x7012345000ull);
    put32(p, mix, 1);
    put32(p, spl, 0xFFFFFFFF);
    for (int i = 0; i < 4; i++) {  // WaveBufferInternal, 0x38
        uint8_t *w = p + wbs + i * 0x38;
        put64(w, 0x00, 0x7100000000ull + i * 0x1000);
        put64(w, 0x08, 0x800 + i);
        put32(w, 0x10, 10 + i), put32(w, 0x14, 1000 + i);
        w[0x18] = i == 1, w[0x19] = i == 3, w[0x1A] = i == 2;
        put32(w, 0x1C, (uint32_t)(i - 1));
        put64(w, 0x20, i == 0 ? 0x7200000000ull : 0);
        put32(w, 0x30, 20 + i), put32(w, 0x34, 900 + i);
    }
    for (int c = 0; c < 6; c++) put32(p, chans + c * 4, 40 + c);
    p[drop] = 1, p[flush] = 2, p[flags] = 0x5;
}

static void check_voice(const AudrenVoiceIn *v, bool v2) {
    CHECK(v->is_new == 1 && v->in_use == 1 && v->play_state == 1 && v->format == 2);
    CHECK(v->sample_rate == 32000 && v->channels == 2 && v->pitch == 1.25f && v->volume == 0.5f);
    CHECK(v->bq_on[0] && !v->bq_on[1]);
    CHECK(fabsf(v->bq[0][0] - 0.5f) < 1e-6f && fabsf(v->bq[0][1] - 0.25f) < 1e-6f && fabsf(v->bq[0][2] + 0.125f) < 1e-6f);
    CHECK(fabsf(v->bq[0][3] - 1.5f) < 1e-6f && fabsf(v->bq[0][4] + 0.75f) < 1e-6f);
    CHECK(v->wb_count == 3 && v->src_addr == 0x7012345000ull && v->mix_id == 1 && v->splitter_id == 0xFFFFFFFF);
    for (int i = 0; i < 4; i++) {
        const AudrenWaveBufferIn *w = &v->wb[i];
        CHECK(w->addr == 0x7100000000ull + i * 0x1000 && w->size == 0x800u + i);
        CHECK(w->start == 10u + i && w->end == 1000u + i && w->loop == (i == 1) && w->eos == (i == 3) && w->sent == (i == 2));
        CHECK(w->loop_count == i - 1 && w->ctx == (i == 0 ? 0x7200000000ull : 0));
        CHECK(w->loop_start == 20u + i && w->loop_end == 900u + i);
    }
    for (int c = 0; c < 6; c++) CHECK(v->channel_ids[c] == 40u + c);
    CHECK(v->clear_drop == 1 && v->flush == 2 && (v->flags & 0xFF) == 0x5);
    printf("  %s layout: all fields read\n", v2 ? "REV15 (InParameter2)" : "REV11 (InParameter)");
}

int main(void) {
    uint8_t buf[0x188];
    AudrenVoiceIn v;
    // layout choice by revision magic: "REV" + digit ('0' + n)
    CHECK(audren_voice_layout(0x3B564552u) == &AUDREN_VOICE_V1);  // REV11 ("REV;")
    CHECK(audren_voice_layout(0x3E564552u) == &AUDREN_VOICE_V1);  // REV14
    CHECK(audren_voice_layout(0x3F564552u) == &AUDREN_VOICE_V2);  // REV15 ("REV?")
    CHECK(AUDREN_VOICE_V1.size == 0x170 && AUDREN_VOICE_V2.size == 0x188);

    fill(buf, false);
    audren_voice_read(buf, &AUDREN_VOICE_V1, &v);
    check_voice(&v, false);
    fill(buf, true);
    audren_voice_read(buf, &AUDREN_VOICE_V2, &v);
    check_voice(&v, true);

    // What v1.1.0 got before: a REV15 voice read with the old layout has no wave buffer where the game put one
    audren_voice_read(buf, &AUDREN_VOICE_V1, &v);
    CHECK(v.wb[0].addr != 0x7100000000ull && v.wb_count != 3);

    if (failures) { printf("%d of %d checks failed\n", failures, checks); return 1; }
    printf("test_audren_voice: all %d checks passed\n", checks);
    return 0;
}
