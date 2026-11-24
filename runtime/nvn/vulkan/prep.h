// What the Vulkan backend offers the headless shader preparation tool (runtime/tools/shaderprep.c,
// Trirod-ShaderPrep.exe): the device the game would use without a window, the startup warmup of the recorded
// pipeline recipes, the GLSL -> SPIR-V cache and the driver's pipeline cache. No Vulkan types: the tool only drives
// these, it does not render.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool found;               // compatible local or bundled recipes were found
    char why[96];             // else why they weren't used
    uint32_t recipes;         // records read
    uint32_t shared_recipes;  // records read from the bundled portable catalog
    int programs, pipelines;  // queued for building
    int programs_ok, pipelines_ok, programs_done, pipelines_done;
    int missing_code, invalid;  // records without their cached guest code / corrupt ones
    int flip_y[2], depth_minus_one[2];  // the recipes' vertex-key flags (votes for 0 and 1)
} VkWarmSummary;

// device.c: instance, the device the game would choose (settings' GPU, else the same automatic rule), queues,
// allocator and <cache>/vk_pipelines.bin; then the shader queues (exec.c, shaders.c). No window, surface or swapchain.
void vk_headless_init(void);
// the chosen device: name, PCI vendor / device ids, driver version and Vulkan API version
void vk_headless_device(char *name, size_t n, uint32_t *vendor, uint32_t *device, uint32_t *driver, uint32_t *api);
void vk_device_idle(void);
void vk_pipeline_cache_save(void);  // also saves the recipes (vk_warm_save)
const char *vk_cache_dir(void);
void vk_warm_catalog_set(const char *directory);  // ShaderPrep's bundled shader_catalog/warm directory

// exec.c: the recorded set (vk_warm_start queues it on the low-priority shader queues)
void vk_warm_start(void);
bool vk_warm_progress(int *done, int *total);  // true while it builds
void vk_warm_summary(VkWarmSummary *s);

// shaders.c: GLSL -> SPIR-V through <cache>/spirv (keyed by the GLSL text). malloc'd, NULL on error.
uint32_t *vk_glsl_to_spirv(const char *src, int stage, const char *name, size_t *nwords);
bool vk_gpl_available(void);
bool vk_gpl_prepare_spirv_pair(const uint32_t *vs, size_t vs_words, const uint32_t *fs, size_t fs_words);
// exec.c: optimized (link-time) pipeline replacements finished / succeeded / queued so far; shader jobs not finished
void vk_gpl_optimize_progress(int *done, int *ok, int *total);
uint32_t vk_exec_jobs_pending(void);
// exec.c: the session's pipeline totals (prepared, reused, missed, driver cache hits / compiles), one line
void vk_pipeline_session_summary(char *out, size_t n);
