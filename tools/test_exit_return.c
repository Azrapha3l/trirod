// EXIT in GLSL shaders with branches (runtime/nvn/maxwell/translate.c glsl_finish): the shader's epilogue, then
// return - not `exit_now = true; break;`. With the flag, the NVIDIA driver on an RTX 5060 (Blackwell, 621.14 /
// 621.40) compiled the Jabu-Jabu splash particles' vertex shader 72ea507300 into an instruction the GPU rejects
// ("Illegal Instruction Encoding"; the game lost the device). Synthetic programs, no game data: a vertex shader
// (flip_y epilogue) and a fragment shader (colour output epilogue), each with a predicated EXIT, a forward branch and
// the final EXIT; the GLSL must have the epilogue before every return, no flag-and-break EXIT, and compile (shaderc).
//
// usage: test_exit_return        (tools/test_exit_return.sh builds and runs it)
#include <shaderc/shaderc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../runtime/nvn/maxwell/shader.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static size_t count(const char *s, const char *what) {
    size_t n = 0;
    for (const char *p = s; (p = strstr(p, what)) != NULL; p += strlen(what)) n++;
    return n;
}

static void check(const char *name, int stage, const char *epilogue) {
    static uint8_t buf[0x80 + 8 * 8 + 64];
    memset(buf, 0, sizeof buf);
    if (stage == STAGE_FRAGMENT) {
        uint32_t omap = 0xF;  // writes colour target 0
        memcpy(buf + 0x30 + 0x48, &omap, 4);
    }
    uint64_t *code = (uint64_t *)(buf + 0x80);
    code[0] = 0x001f8000fc0007e0ull;  // scheduling word
    code[1] = 0xe30000000000000full;  // @P0 EXIT
    code[2] = 0xe24000000107000full;  // BRA +0x10: over the MOV, to pc 0x28
    code[3] = 0x0103f8000007f000ull;  // MOV32I r0, 1.0
    code[4] = 0x001f8000fc0007e0ull;
    code[5] = 0xe30000000007000full;  // EXIT
    code[6] = 0xe2400fffff87000full;  // BRA self (end of program)
    code[7] = 0xe2400fffff87000full;
    ShaderKey k;
    memset(&k, 0, sizeof k);
    k.glsl = 1;
    k.flip_y = 1;
    ShaderInfo *si = maxwell_translate(buf, sizeof buf, stage, &k);
    CHECK(si && si->msl, "%s: did not translate (%s)", name, si && si->error ? si->error : "?");
    if (!si || !si->msl) return;
    const char *g = si->msl;
    size_t rets = count(g, "return; }");
    CHECK(rets == 2, "%s: %zu returns, want 2 (the predicated EXIT and the last one)", name, rets);
    CHECK(!strstr(g, "exit_now = true; break;"), "%s: an EXIT still sets exit_now and breaks", name);
    // each return closes a block "    {\n<epilogue>    return; }" that contains the epilogue
    for (const char *p = g; (p = strstr(p, "return; }")) != NULL; p++) {
        const char *from = p;
        while (from > g && strncmp(from, "    {\n", 6)) from--;
        char block[1024];
        snprintf(block, sizeof block, "%.*s", (int)(p - from), from);
        CHECK(strstr(block, epilogue) != NULL, "%s: no \"%s\" before a return", name, epilogue);
    }
    CHECK(count(g, epilogue) == 3, "%s: epilogue %zu times, want 3 (two returns and the end of main)", name, count(g, epilogue));
    shaderc_compiler_t sc = shaderc_compiler_initialize();
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(sc, g, strlen(g), stage == STAGE_VERTEX ? shaderc_vertex_shader
                                                                                                     : shaderc_fragment_shader,
                                                              name, "main", o);
    bool ok = shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success;
    CHECK(ok, "%s: GLSL does not compile: %s", name, shaderc_result_get_error_message(r));
    shaderc_result_release(r);
    shaderc_compile_options_release(o);
    shaderc_compiler_release(sc);
    printf("%-16s returns %zu  %s\n", name, rets, fails ? "" : "ok");
    maxwell_free(si);
}

int main(void) {
    maxwell_gmem_base = 0x7240000000ull;
    maxwell_gmem_size = 0x100000000ull;
    check("vertex shader", STAGE_VERTEX, "gl_Position.y = -gl_Position.y;");
    check("fragment shader", STAGE_FRAGMENT, "c0[0] = as_f(r0);");
    printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
