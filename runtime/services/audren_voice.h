// The audio renderer's per-voice input (VoiceInfo::InParameter) in both layouts the game's SDKs send. Revisions up to
// REV14 send InParameter (0x170 bytes, Q14 biquad coefficients); REV15 (SupportTags::VoiceInParameterV2, the v1.1.0
// update's nnSdk 20) sends InParameter2 (0x188 bytes, float biquads, so every field after them is 0x18 further on).
// Layouts: yuzu audio_core/renderer/voice/voice_info.h 152-220 (static_asserts on both sizes), feature revisions in
// audio_core/common/feature_support.h (VoiceInParameterV2 = 15). Reading REV15 voices with the old layout found no wave
// buffers and the renderer played silence (v1.1.0, 2026-11-25). Header-only for tools/test_audren_voice.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define AUDREN_WB_COUNT 4
#define AUDREN_VOICE_CHANNELS 6

typedef struct {
    uint32_t size;                  // stride of the voice section's entries
    uint32_t biquad, biquad_size;   // the two BiquadFilterParameter(2)
    bool float_biquad;              // InParameter2: f32 b[3] at +4, a[2] at +0x10; else s16 Q14 b[3] at +2, a[2] at +8
    uint32_t wb_count, src_addr, mix_id, splitter_id, wave_buffers, channel_ids, clear_drop, flush, flags;
} AudrenVoiceLayout;

static const AudrenVoiceLayout AUDREN_VOICE_V1 = {0x170, 0x24, 0x0C, false, 0x3C, 0x48, 0x58, 0x5C, 0x060, 0x140, 0x158, 0x159, 0x15C};
static const AudrenVoiceLayout AUDREN_VOICE_V2 = {0x188, 0x24, 0x18, true,  0x54, 0x60, 0x70, 0x74, 0x078, 0x158, 0x170, 0x171, 0x174};

// revision: the renderer's "REVn" magic as the game passed it to OpenAudioRenderer
static inline uint32_t audren_revision_number(uint32_t revision) { return (revision >> 24) - '0'; }
static inline const AudrenVoiceLayout *audren_voice_layout(uint32_t revision) {
    return audren_revision_number(revision) >= 15 ? &AUDREN_VOICE_V2 : &AUDREN_VOICE_V1;
}

// One wave buffer (WaveBufferInternal, 0x38 bytes: the same in both layouts)
typedef struct {
    uint64_t addr, size, ctx;
    uint32_t start, end, loop_start, loop_end;
    int32_t loop_count;
    uint8_t loop, eos, sent;
} AudrenWaveBufferIn;

// The fields the renderer uses, from either layout
typedef struct {
    uint8_t is_new, in_use, play_state, format;
    uint32_t sample_rate, channels, wb_count;
    float pitch, volume;
    bool bq_on[2];
    float bq[2][5];  // b0 b1 b2 a1 a2, as floats
    uint64_t src_addr;
    int32_t mix_id;
    uint32_t splitter_id;
    AudrenWaveBufferIn wb[AUDREN_WB_COUNT];
    uint32_t channel_ids[AUDREN_VOICE_CHANNELS];
    uint8_t clear_drop, flush;
    uint16_t flags;
} AudrenVoiceIn;

static inline void audren_voice_read(const uint8_t *p, const AudrenVoiceLayout *L, AudrenVoiceIn *v) {
    memset(v, 0, sizeof *v);
    v->is_new = p[8], v->in_use = p[9], v->play_state = p[10], v->format = p[11];
    memcpy(&v->sample_rate, p + 0x0C, 4);
    memcpy(&v->channels, p + 0x18, 4);
    memcpy(&v->pitch, p + 0x1C, 4);
    memcpy(&v->volume, p + 0x20, 4);
    for (int f = 0; f < 2; f++) {
        const uint8_t *b = p + L->biquad + f * L->biquad_size;
        v->bq_on[f] = b[0] != 0;
        if (L->float_biquad) memcpy(v->bq[f], b + 4, 5 * sizeof(float));
        else {
            int16_t c[5];
            memcpy(c, b + 2, sizeof c);
            for (int k = 0; k < 5; k++) v->bq[f][k] = c[k] / 16384.0f;
        }
    }
    memcpy(&v->wb_count, p + L->wb_count, 4);
    memcpy(&v->src_addr, p + L->src_addr, 8);
    memcpy(&v->mix_id, p + L->mix_id, 4);
    memcpy(&v->splitter_id, p + L->splitter_id, 4);
    for (int i = 0; i < AUDREN_WB_COUNT; i++) {
        const uint8_t *w = p + L->wave_buffers + i * 0x38;
        AudrenWaveBufferIn *b = &v->wb[i];
        memcpy(&b->addr, w, 8);
        memcpy(&b->size, w + 8, 8);
        memcpy(&b->start, w + 16, 4);
        memcpy(&b->end, w + 20, 4);
        b->loop = w[24], b->eos = w[25], b->sent = w[26];
        memcpy(&b->loop_count, w + 28, 4);
        memcpy(&b->ctx, w + 32, 8);
        memcpy(&b->loop_start, w + 48, 4);
        memcpy(&b->loop_end, w + 52, 4);
    }
    memcpy(v->channel_ids, p + L->channel_ids, sizeof v->channel_ids);
    v->clear_drop = p[L->clear_drop], v->flush = p[L->flush];
    memcpy(&v->flags, p + L->flags, 2);
}
