// Temporary stand-in for a shader variant that is still building (exec.c draw_, "variant fallback").
//
// One guest program (a VS + FS pair) is translated once per ShaderKey pair: draws of the same program with other
// vertex formats, render-target formats or stencil views each get their own variant, and the first draw of each
// variant used to be skipped (or wait) until the translator, glslang and the pipeline were done. A variant of the
// SAME guest code that is already built may draw instead, but only when the translator gives both keys the same
// output - then the stand-in is not an approximation: its GLSL, ShaderInfo and descriptor layouts are those the
// exact variant will have (tools/test_variant_fallback.c translates both keys and compares).
//
// What each ShaderKey field changes in the GLSL translation (runtime/nvn/maxwell/translate.c, glsl_finish and the key reads it calls):
//  - attr_kind[i] (VS): only the declared type of input a<i> (vec4 / ivec4 / uvec4), and only for attributes the
//    VS reads (ShaderInfo.attr_mask). Vulkan requires that type to match the vertex format's numeric type, so a read
//    attribute must have the same kind; an attribute the VS doesn't read gets no declaration and no pipeline
//    attribute (exec.c PipeKey.attr_mask comes from the same mask).
//  - rt_kind[rt] (FS): the declared type and epilogue conversion of output c<rt>, and the per-draw tint (only when
//    target 0 is written), only for targets the FS writes (ShaderInfo.out_mask nibbles). Same Vulkan rule for the
//    attachment's numeric type. Targets the FS doesn't write: no output, write mask 0 in the pipeline.
//  - flip_y, flip_x, depth_minus_one (VS epilogue; flip_x also FS frag-coord), glsl (dialect): change the whole
//    output. Must match.
//  - stencil_tex (FS): integer samplers and raw-bit reads for the slots the draw binds as stencil views; the
//    descriptors the renderer writes depend on it. Must match (the draw computes it from the bound textures).
//  - vs_out_mask (FS): set by the translation from the VS's outputs (exec.c translate_program), a function of the
//    VS code only; the lookup key has 0. Ignored: the stand-in's own VS feeds its own FS.
// Every other field must be equal, even the ones the stage doesn't read today (rt_kind in the VS key, attr_kind
// in the FS key): when in doubt, no stand-in.
//
// Identity: both stages' guest code ids (content hash of the bytes the translator reads, warm_code_id) and extents
// must be equal and known. Code addresses are not compared (the same code may sit at other addresses); id 0 =
// unknown (recipes off, EOWRECOMP_VK_WARM=0): no stand-in.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "../maxwell/shader.h"

typedef enum {
    VF_OK,
    VF_NO_IDENTITY,  // a code id is 0 (unknown)
    VF_OTHER_CODE,   // another guest VS or FS (or extent)
    VF_FLAGS,        // glsl / flip_y / flip_x / depth_minus_one differ
    VF_ATTR_KIND,    // a vertex input the VS reads has another numeric type
    VF_RT_KIND,      // a render target the FS writes has another numeric type
    VF_STENCIL,      // other stencil-view samplers
    VF_OTHER_FIELD,  // a field no rule above allows to differ
    VF_NVERDICTS
} VfVerdict;

static const char *const vf_verdict_names[VF_NVERDICTS] = {
    "ok", "unknown code identity", "other guest code", "clip-space / dialect flags", "vertex input kind",
    "render target kind", "stencil samplers", "other key field"};

typedef struct {
    uint64_t gid[2];   // guest VS / FS code ids (Prog.gid)
    uint32_t gext[2];  // bytes the translator reads (Prog.gext)
    ShaderKey vkey, fkey;
} VfVariant;

static inline bool vf_flags_equal(const ShaderKey *a, const ShaderKey *b) {
    return a->glsl == b->glsl && a->flip_y == b->flip_y && a->flip_x == b->flip_x &&
           a->depth_minus_one == b->depth_minus_one;
}

// May `cand` (built; its VS reads `cand_vs_attr_mask`, its FS writes the targets in `cand_fs_out_mask`, as its
// ShaderInfo says) draw for `want`? Pure: tools/test_variant_fallback.c tests it against the translator.
static inline VfVerdict vf_check(const VfVariant *want, const VfVariant *cand, uint32_t cand_vs_attr_mask,
                                 uint32_t cand_fs_out_mask) {
    if (!want->gid[0] || !want->gid[1] || !cand->gid[0] || !cand->gid[1]) return VF_NO_IDENTITY;
    if (want->gid[0] != cand->gid[0] || want->gid[1] != cand->gid[1] || want->gext[0] != cand->gext[0] ||
        want->gext[1] != cand->gext[1])
        return VF_OTHER_CODE;
    const ShaderKey *wv = &want->vkey, *wf = &want->fkey, *cv = &cand->vkey, *cf = &cand->fkey;
    if (!vf_flags_equal(wv, cv) || !vf_flags_equal(wf, cf)) return VF_FLAGS;
    for (int i = 0; i < 32; i++)
        if (((cand_vs_attr_mask >> i) & 1) && wv->attr_kind[i] != cv->attr_kind[i]) return VF_ATTR_KIND;
    for (int rt = 0; rt < 8; rt++)
        if (((cand_fs_out_mask >> (rt * 4)) & 0xF) && wf->rt_kind[rt] != cf->rt_kind[rt]) return VF_RT_KIND;
    if (wf->stencil_tex != cf->stencil_tex) return VF_STENCIL;
    // (the rest: equal or no stand-in)
    for (int i = 0; i < 32; i++) if (wf->attr_kind[i] != cf->attr_kind[i]) return VF_OTHER_FIELD;
    for (int rt = 0; rt < 8; rt++) if (wv->rt_kind[rt] != cv->rt_kind[rt]) return VF_OTHER_FIELD;
    if (wv->stencil_tex != cv->stencil_tex || wv->vs_out_mask != cv->vs_out_mask) return VF_OTHER_FIELD;
    return VF_OK;
}
