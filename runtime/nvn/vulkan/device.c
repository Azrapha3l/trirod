// Vulkan device, window, swapchain and presentation (counterpart of the window/present half of metal.m).
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <math.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../paths.h"
#include "../../platform.h"
#include "../../settings.h"
#include "../../ui/menu.h"
#include "../../ui/touch.h"
#include "vk.h"
#include "../output_scale.h"
#include "../renderer.h"
#include "prep.h"

VkG g_vk;
PFN_vkCmdInsertDebugUtilsLabelEXT vk_draw_label_fn;
PFN_vkCmdSetCheckpointNV vk_checkpoint_fn;
static PFN_vkGetQueueCheckpointDataNV checkpoint_data_fn;
static SDL_Window *window;
static atomic_bool ui_ready;
static VkDebugUtilsMessengerEXT messenger;
static bool g_headless;  // Trirod-ShaderPrep: a device without a window (vk_headless_init)

// ================================================================== instance / device
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type, (void)user;
    static int n;
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && n++ < 500) LOG("[vk] %s\n", d->pMessage);
    return VK_FALSE;
}

// EOWRECOMP_VK_VALIDATION=1: Khronos validation layer (MSYS2 package vulkan-validation-layers)
static void create_instance(void) {
#ifdef VK_NO_PROTOTYPES
    if (volkInitialize() != VK_SUCCESS) FATAL("no Vulkan driver\n");
#endif
    Uint32 n_sdl = 0;
    // (headless: no surface extensions, there is no window to present to)
    const char *const *sdl_ext = g_headless ? NULL : SDL_Vulkan_GetInstanceExtensions(&n_sdl);
    const char *ext[16];
    uint32_t n_ext = 0;
    for (Uint32 i = 0; i < n_sdl && n_ext < 14; i++) ext[n_ext++] = sdl_ext[i];
    VkInstanceCreateFlags flags = 0;
#ifdef __APPLE__
    // MoltenVK is a portability driver: the loader lists it only to instances that ask for those
    ext[n_ext++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
    bool validate = getenv("EOWRECOMP_VK_VALIDATION") != NULL;
    if (validate) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    // EOWRECOMP_VK_DRAW_LABELS=1: a debug label before every draw (frame, draw index, guest VS / FS code addresses,
    // the numbers EOWRECOMP_VK_TRACE_FS logs) so a draw picked in RenderDoc's event browser or pixel history can be
    // matched to its guest shaders. Needs VK_EXT_debug_utils, which RenderDoc and the validation layer provide.
    bool labels = getenv("EOWRECOMP_VK_DRAW_LABELS") != NULL;
    if (labels && !validate) {
        uint32_t ne = 0;
        vkEnumerateInstanceExtensionProperties(NULL, &ne, NULL);
        VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
        vkEnumerateInstanceExtensionProperties(NULL, &ne, ep);
        bool have = false;
        for (uint32_t i = 0; i < ne; i++) have |= !strcmp(ep[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        free(ep);
        if (have) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        else { LOG("[vk] EOWRECOMP_VK_DRAW_LABELS: VK_EXT_debug_utils unavailable; no draw labels\n"); labels = false; }
    }
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "TriRod",
                             .pEngineName = "TriRod", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .flags = flags, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    VkResult r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    if (r != VK_SUCCESS && validate) {
        LOG("[vk] validation layer unavailable (%d); continuing without\n", r);
        ci.enabledLayerCount = 0;
        ci.enabledExtensionCount--;
        validate = false;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
    if (r != VK_SUCCESS) FATAL("vkCreateInstance: %d (is a Vulkan 1.3 driver installed?)\n", r);
#ifdef VK_NO_PROTOTYPES
    volkLoadInstance(g_vk.inst);
#endif
    if (labels) {
        vk_draw_label_fn = (PFN_vkCmdInsertDebugUtilsLabelEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCmdInsertDebugUtilsLabelEXT");
        LOG("[vk] EOWRECOMP_VK_DRAW_LABELS: draw labels %s\n", vk_draw_label_fn ? "on" : "unavailable");
    }
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb};
        if (f) f(g_vk.inst, &mi, NULL, &messenger);
    }
}

static bool has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

// Keep only Vulkan 1.3 devices with the graphics+compute+present queue this renderer needs. UUIDs make the user's
// saved choice independent of Vulkan's enumeration order (which can change when drivers or cards change).
#define GPU_MAX 32
typedef struct { VkPhysicalDevice phys; VkPhysicalDeviceProperties props; uint8_t uuid[VK_UUID_SIZE]; uint32_t qfam; } GpuChoice;
static GpuChoice g_gpus[GPU_MAX];
static uint32_t g_gpu_count;
static int g_active_gpu = -1;

static void gpu_uuid(const GpuChoice *g, char out[33]) {
    for (int i = 0; i < VK_UUID_SIZE; i++) snprintf(out + i * 2, 3, "%02x", g->uuid[i]);
    out[32] = 0;
}

int vk_gpu_count(void) { return (int)g_gpu_count; }
int vk_gpu_active_index(void) { return g_active_gpu; }
const char *vk_gpu_name(int i) { return i >= 0 && (uint32_t)i < g_gpu_count ? g_gpus[i].props.deviceName : ""; }
const char *vk_gpu_id(int i) {
    static char ids[GPU_MAX][33];
    if (i < 0 || (uint32_t)i >= g_gpu_count) return "";
    gpu_uuid(&g_gpus[i], ids[i]);
    return ids[i];
}

static void pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.inst, &n, NULL);
    if (!n) FATAL("no Vulkan device\n");
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    for (uint32_t i = 0; i < n && g_gpu_count < GPU_MAX; i++) {
        VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id};
        vkGetPhysicalDeviceProperties2(pd[i], &p2);
        if (p2.properties.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &nq, NULL);
        VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp);
        vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &nq, qp);
        uint32_t qfam = UINT32_MAX;
        for (uint32_t q = 0; q < nq; q++) {
            VkBool32 present = g_headless;  // (headless: the game's window decides presenting; same family rule)
            if (!g_headless) vkGetPhysicalDeviceSurfaceSupportKHR(pd[i], q, g_vk.surface, &present);
            if ((qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[q].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) { qfam = q; break; }
        }
        free(qp);
        if (qfam == UINT32_MAX) continue;
        GpuChoice *g = &g_gpus[g_gpu_count++];
        g->phys = pd[i];
        g->props = p2.properties;
        memcpy(g->uuid, id.deviceUUID, VK_UUID_SIZE);
        g->qfam = qfam;
        LOG("[vk] gpu %u: %s (api %u.%u.%u, UUID %s)\n", g_gpu_count - 1, g->props.deviceName,
            VK_API_VERSION_MAJOR(g->props.apiVersion), VK_API_VERSION_MINOR(g->props.apiVersion),
            VK_API_VERSION_PATCH(g->props.apiVersion), vk_gpu_id((int)g_gpu_count - 1));
    }
    free(pd);
    if (!g_gpu_count) FATAL("no Vulkan 1.3 device with a graphics+compute+present queue\n");

    int chosen = -1;
    const char *env = getenv("EOWRECOMP_GPU");
    if (env && *env) {
        int i = atoi(env);
        if (i >= 0 && (uint32_t)i < g_gpu_count) chosen = i;
        else LOG("[vk] EOWRECOMP_GPU=%s is unavailable; using the saved/automatic choice\n", env);
    }
    if (chosen < 0 && g_settings.gpu_id[0]) {
        for (uint32_t i = 0; i < g_gpu_count; i++) if (!strcmp(g_settings.gpu_id, vk_gpu_id((int)i))) { chosen = (int)i; break; }
        if (chosen < 0) {
            LOG("[vk] saved Vulkan device %s is unavailable; falling back to Auto detect\n", g_settings.gpu_id);
            g_settings.gpu_id[0] = 0;
            settings_save();
        }
    }
    if (chosen < 0) {
        int best_score = -1;
        for (uint32_t i = 0; i < g_gpu_count; i++) {
            VkPhysicalDeviceType t = g_gpus[i].props.deviceType;
            int score = t == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : t == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
            if (score > best_score) best_score = score, chosen = (int)i;
        }
    }
    g_active_gpu = chosen;
    g_vk.phys = g_gpus[chosen].phys;
    g_vk.props = g_gpus[chosen].props;
    g_vk.qfam = g_gpus[chosen].qfam;
    LOG("[vk] using %s\n", g_vk.props.deviceName);
}

static bool g_fault_ext;
// After VK_ERROR_DEVICE_LOST: what the driver says faulted (VK_EXT_device_fault), into the log.
void vk_device_fault_report(void) {
    static int once;
    if (once++ || !g_fault_ext) return;
    PFN_vkGetDeviceFaultInfoEXT get = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetDeviceFaultInfoEXT");
    if (!get) return;
    VkDeviceFaultCountsEXT c = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (get(g_vk.dev, &c, NULL) != VK_SUCCESS) { LOG("[vk] device fault: no info\n"); return; }
    VkDeviceFaultAddressInfoEXT addr[16];
    VkDeviceFaultVendorInfoEXT vend[16];
    if (c.addressInfoCount > 16) c.addressInfoCount = 16;
    if (c.vendorInfoCount > 16) c.vendorInfoCount = 16;
    c.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = addr, .pVendorInfos = vend};
    if (get(g_vk.dev, &c, &info) != VK_SUCCESS && 0) return;
    LOG("[vk] device fault: %s\n", info.description);
    static const char *const kind[] = {"none", "read invalid", "write invalid", "execute invalid", "ip unknown", "ip invalid", "ip valid"};
    for (uint32_t i = 0; i < c.addressInfoCount; i++)
        LOG("[vk]   address %s %#llx (precision %#llx)\n", addr[i].addressType < 7 ? kind[addr[i].addressType] : "?",
            (unsigned long long)addr[i].reportedAddress, (unsigned long long)addr[i].addressPrecision);
    for (uint32_t i = 0; i < c.vendorInfoCount; i++)
        LOG("[vk]   vendor: %s (code %#llx data %#llx)\n", vend[i].description, (unsigned long long)vend[i].vendorFaultCode,
            (unsigned long long)vend[i].vendorFaultData);
}

// After VK_ERROR_DEVICE_LOST with EOWRECOMP_VK_CHECKPOINTS: the last checkpoint the GPU reached at each pipeline stage
// it reports (top of pipe: the command had started; bottom of pipe: it had finished). The markers are sequence numbers
// whose descriptions exec.c keeps (vk_checkpoint_name).
void vk_checkpoint_report(void) {
    static int once;
    if (once++ || !checkpoint_data_fn) return;
    uint32_t n = 0;
    checkpoint_data_fn(g_vk.queue, &n, NULL);
    VkCheckpointDataNV cp[16];
    if (n > 16) n = 16;
    for (uint32_t i = 0; i < n; i++) cp[i] = (VkCheckpointDataNV){.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV};
    checkpoint_data_fn(g_vk.queue, &n, cp);
    LOG("[vk] checkpoints reached before the device loss: %u\n", n);
    uint64_t lo = UINT64_MAX, hi = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t seq = (uint64_t)(uintptr_t)cp[i].pCheckpointMarker;
        if (seq < lo) lo = seq;
        if (seq > hi) hi = seq;
        LOG("[vk]   stage %#x: checkpoint %llu %s\n", (unsigned)cp[i].stage, (unsigned long long)seq, vk_checkpoint_name(seq));
    }
    // the commands between the last finished and the last started one were in flight: one of them hung or faulted
    for (uint64_t s = lo + 1; n > 1 && s < hi && s - lo <= 32; s++)
        LOG("[vk]   in flight: checkpoint %llu %s\n", (unsigned long long)s, vk_checkpoint_name(s));
}

// ---- pipeline cache identity: <cache>/vk_pipelines.tag
// vk_pipelines.bin starts with the driver's header: vendor, device and pipelineCacheUUID (which changes with the
// driver). Vulkan ignores incompatible data by itself; it is checked here as well so the reason can be told. The tag
// adds what the header can't say: the Trirod pipeline scheme and whether Trirod-ShaderPrep (Setup) prepared it.
// Scheme 3: shader-stage libraries without RETAIN info, and optimized pipelines that are complete pipelines (exec.c
// optimize_gpl_pipeline). Older caches are not used: untagged ones can hold link-time-optimized links of libraries
// without RETAIN (invalid usage; on NVIDIA they drew nothing: the black screen of 2026-11-24), scheme 2 ones hold
// RETAIN libraries (1.19 GB). Scheme 4: GLSL of shaders with branches returns at EXIT (translate.c glsl_finish, the
// RTX 5060 illegal-instruction fix), so most stage libraries and pipelines of a scheme 3 cache no longer match: prepare
// again rather than compile them during play. When the cache wasn't prepared for this GPU and driver, the game writes
// the reason to <cache>/shaders-stale.txt; the launcher shows it and offers "Prepare shaders".
#define PIPELINE_SCHEME 4
static bool g_pc_prepared;  // the loaded cache was prepared by Trirod-ShaderPrep for this device and driver
static bool g_prep_in_game;  // prepare_shaders_in_game: Setup prepared shaders before, but not for this GPU / driver
static char g_prep_why[320], g_prep_key[160];

// The device + driver + scheme an in-game preparation was last tried for (<cache>/prep-attempt.txt): finished, failed
// or skipped, it is not started again by itself for the same ones (the launcher's notice and button remain).
static void prep_attempt_key(char *out, size_t n) {
    const VkPhysicalDeviceProperties *p = &g_vk.props;
    int k = snprintf(out, n, "%d %u %u %u ", PIPELINE_SCHEME, p->vendorID, p->deviceID, p->driverVersion);
    for (int i = 0; i < VK_UUID_SIZE && k + 2 < (int)n; i++) k += snprintf(out + k, n - k, "%02x", p->pipelineCacheUUID[i]);
}

static void stale_note(const char *why) {
    char path[600];
    snprintf(path, sizeof path, "%s/shaders-stale.txt", vk_cache_dir());
    if (!why) { remove(path); return; }
    FILE *f = fopen(path, "wb");
    if (f) { fprintf(f, "%s\n", why); fclose(f); }
}

static bool pipeline_cache_check(const void *data, size_t sz) {
    const VkPhysicalDeviceProperties *p = &g_vk.props;
    char path[600], line[200], gpu[160] = "", why[320] = "";
    int scheme = 0, prepared = 0;
    snprintf(path, sizeof path, "%s/vk_pipelines.tag", vk_cache_dir());
    FILE *f = fopen(path, "rb");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            sscanf(line, "trirod-pipelines %d", &scheme);
            sscanf(line, "prepared %d", &prepared);
            if (!strncmp(line, "gpu ", 4)) snprintf(gpu, sizeof gpu, "%s", line + 4);
        }
        fclose(f);
    }
    const VkPipelineCacheHeaderVersionOne *h = data;
    if (!data || !sz) snprintf(why, sizeof why, "no shader cache yet");
    else if (sz < sizeof *h || h->headerSize < sizeof *h || h->headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE)
        snprintf(why, sizeof why, "the shader cache file is damaged");
    else if (h->vendorID != p->vendorID || h->deviceID != p->deviceID)
        snprintf(why, sizeof why, "shaders were prepared for another graphics card (%s)", gpu[0] ? gpu : "unknown");
    else if (memcmp(h->pipelineCacheUUID, p->pipelineCacheUUID, VK_UUID_SIZE))
        snprintf(why, sizeof why, "the graphics driver changed since the shaders were prepared");
    else if (scheme != PIPELINE_SCHEME)
        snprintf(why, sizeof why, "the shader cache is from an older Trirod build");
    bool use = !why[0];
    g_pc_prepared = use && prepared;
    if (!use && data && sz) LOG("[vk] pipeline cache not used: %s\n", why);
    if (!g_headless) {
        if (use && !prepared) snprintf(why, sizeof why, "shaders were not prepared by Setup for this graphics card and driver");
        stale_note(g_pc_prepared ? NULL : why);
        if (!g_pc_prepared)
            LOG("[vk] %s: Setup's \"Prepare shaders\" builds them before Play; meanwhile the game builds them as it goes\n", why);
        else LOG("[vk] using the shader cache prepared for %s\n", p->deviceName);
        // Setup prepared shaders on this PC before (its report is there), but not for this GPU / driver / scheme: the
        // game prepares them itself at start, once per device and driver (backend_init)
        snprintf(path, sizeof path, "%s/shaderprep.json", vk_cache_dir());
        struct stat st;
        if (!g_pc_prepared && stat(path, &st) == 0) {
            char last[160] = "";
            snprintf(path, sizeof path, "%s/prep-attempt.txt", vk_cache_dir());
            if ((f = fopen(path, "rb"))) {
                if (!fgets(last, sizeof last, f)) last[0] = 0;
                last[strcspn(last, "\r\n")] = 0;
                fclose(f);
            }
            prep_attempt_key(g_prep_key, sizeof g_prep_key);
            const char *e = getenv("EOWRECOMP_PREPARE_SHADERS");  // 0: never in game; 1: also when tried before
            g_prep_in_game = e && *e ? atoi(e) != 0 && (atoi(e) == 1 || strcmp(last, g_prep_key)) : strcmp(last, g_prep_key) != 0;
            snprintf(g_prep_why, sizeof g_prep_why, "%s", why);
            if (!g_prep_in_game) LOG("[vk] in-game shader preparation already tried for this GPU and driver: not repeated\n");
        }
    }
    return use;
}

static void create_device(void) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
    const char *ext[16];
    uint32_t n_ext = 0;
    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    g_vk.custom_border = has_ext(ep, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (g_vk.custom_border) ext[n_ext++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    g_vk.divisor = has_ext(ep, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    if (g_vk.divisor) ext[n_ext++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    g_vk.host_import = has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);  // guest heap as a GPU buffer (exec.c)
    // EOWRECOMP_VK_NO_HOST_IMPORT=1 (debug): run as if the driver lacked the extension - the staged compute path
    // with copy-back that a RenderDoc capture also gets (RenderDoc does not expose VK_EXT_external_memory_host).
    // The void/rift effect drew under RenderDoc but not live: this reproduces that path without the capture tool.
    if (g_vk.host_import && getenv("EOWRECOMP_VK_NO_HOST_IMPORT")) {
        LOG("[vk] EOWRECOMP_VK_NO_HOST_IMPORT: guest heap import disabled\n");
        g_vk.host_import = false;
    }
    if (g_vk.host_import) ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    // a portability implementation (MoltenVK) must have its subset extension enabled when it lists it
    if (has_ext(ep, ne, "VK_KHR_portability_subset")) ext[n_ext++] = "VK_KHR_portability_subset";
    bool fault = has_ext(ep, ne, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);  // device-lost diagnostics (vk_device_fault_report)
    if (fault) ext[n_ext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    g_fault_ext = fault;
    // EOWRECOMP_VK_CHECKPOINTS=1: a marker before every draw and dispatch (VK_NV_device_diagnostic_checkpoints); after
    // a device loss the driver says which markers the GPU had reached (vk_checkpoint_report). VK_EXT_device_fault only
    // gives a GPU address ("ip invalid" for the Jabu-Jabu splash hang), and submitting each pass on its own moved the
    // hang elsewhere; this names the draw without changing the submissions.
    bool checkpoints = getenv("EOWRECOMP_VK_CHECKPOINTS") && has_ext(ep, ne, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    if (checkpoints) ext[n_ext++] = VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME;
    else if (getenv("EOWRECOMP_VK_CHECKPOINTS")) LOG("[vk] EOWRECOMP_VK_CHECKPOINTS: VK_NV_device_diagnostic_checkpoints unavailable\n");
    VkPhysicalDeviceVulkan12Features have12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &have12};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
#ifdef VK_EXT_graphics_pipeline_library
    bool gpl_ext = has_ext(ep, ne, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
    bool pipeline_lib_ext = has_ext(ep, ne, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT have_gpl = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    if (gpl_ext && pipeline_lib_ext) {
        have12.pNext = &have_gpl;
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
    }
    // EOWRECOMP_VK_GPL=0: monolithic pipelines only, also on a GPU with graphics pipeline libraries (A/B tests)
    const char *gpl_env = getenv("EOWRECOMP_VK_GPL");
    g_vk.graphics_pipeline_library = gpl_ext && pipeline_lib_ext && have_gpl.graphicsPipelineLibrary &&
                                     !(gpl_env && atoi(gpl_env) == 0);
    if (g_vk.graphics_pipeline_library) {
        ext[n_ext++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gpl_props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &gpl_props};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &props);
        g_vk.gpl_fast_linking = gpl_props.graphicsPipelineLibraryFastLinking;
        if (!g_vk.gpl_fast_linking)
            LOG("[vk] graphics pipeline library available, but fast linking is not reported by the driver\n");
    }
#endif
    free(ep);

    // Query the descriptor-indexing and formatless-storage-image features used by bindless SUSTB.
    g_vk.bindless_storage_images = have12.runtimeDescriptorArray && have12.descriptorBindingPartiallyBound &&
                                   have12.descriptorBindingVariableDescriptorCount &&
                                   have12.shaderStorageImageArrayNonUniformIndexing && have.features.shaderStorageImageWriteWithoutFormat &&
                                   g_vk.props.limits.maxDescriptorSetStorageImages > 0 &&
                                   g_vk.props.limits.maxPerStageDescriptorStorageImages > 0;
    if (!g_vk.bindless_storage_images)
        LOG("[vk] bindless storage-image writes unavailable (descriptor indexing / formatless storage-write feature missing)\n");
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT,
        .graphicsPipelineLibrary = g_vk.graphics_pipeline_library};
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
        .vertexAttributeInstanceRateDivisor = VK_TRUE, .vertexAttributeInstanceRateZeroDivisor = VK_FALSE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE,
        .pNext = g_vk.divisor ? (void *)&div : g_vk.graphics_pipeline_library ? (void *)&gpl : NULL};
    if (g_vk.divisor) div.pNext = g_vk.graphics_pipeline_library ? &gpl : NULL;
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE,
                                            .shaderDemoteToHelperInvocation = VK_TRUE, .shaderTerminateInvocation = VK_TRUE,
                                            .maintenance4 = VK_TRUE,
                                            .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div :
                                                     g_vk.graphics_pipeline_library ? (void *)&gpl : NULL};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .timelineSemaphore = VK_TRUE, .shaderInt8 = VK_TRUE, .samplerMirrorClampToEdge = VK_TRUE,
                                            .uniformAndStorageBuffer8BitAccess = VK_TRUE, .storageBuffer8BitAccess = VK_TRUE,
                                            .scalarBlockLayout = VK_TRUE,
                                            .runtimeDescriptorArray = have12.runtimeDescriptorArray,
                                            .descriptorBindingPartiallyBound = have12.descriptorBindingPartiallyBound,
                                            .descriptorBindingVariableDescriptorCount = have12.descriptorBindingVariableDescriptorCount,
                                            .shaderStorageImageArrayNonUniformIndexing = have12.shaderStorageImageArrayNonUniformIndexing,
                                            .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .storageBuffer16BitAccess = VK_TRUE, .pNext = &f12};
    VkPhysicalDeviceFaultFeaturesEXT ff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .deviceFault = VK_TRUE,
                                           .pNext = &f11};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = fault ? (void *)&ff : (void *)&f11};
    VkPhysicalDeviceFeatures *b = &f.features, *h = &have.features;
    b->independentBlend = h->independentBlend;
    b->dualSrcBlend = h->dualSrcBlend;
    b->fillModeNonSolid = h->fillModeNonSolid;
    b->samplerAnisotropy = h->samplerAnisotropy;
    b->textureCompressionBC = h->textureCompressionBC;
    b->shaderInt64 = h->shaderInt64;
    b->shaderStorageImageWriteWithoutFormat = h->shaderStorageImageWriteWithoutFormat;
    b->depthBiasClamp = h->depthBiasClamp;
    b->depthClamp = h->depthClamp;
    b->largePoints = h->largePoints;
    b->wideLines = h->wideLines;
    b->imageCubeArray = h->imageCubeArray;
    b->sampleRateShading = h->sampleRateShading;
    b->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics;
    b->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    b->robustBufferAccess = h->robustBufferAccess;  // guest shaders index cbufs freely: keep OOB reads harmless
    b->shaderClipDistance = h->shaderClipDistance;
    if (!h->shaderInt64) LOG("[vk] warning: no shaderInt64 (64-bit address math in shaders)\n");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    VK_CHECK(vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev));
#ifdef VK_NO_PROTOTYPES
    volkLoadDevice(g_vk.dev);
#endif
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);
    if (checkpoints) {
        vk_checkpoint_fn = (PFN_vkCmdSetCheckpointNV)vkGetDeviceProcAddr(g_vk.dev, "vkCmdSetCheckpointNV");
        checkpoint_data_fn = (PFN_vkGetQueueCheckpointDataNV)vkGetDeviceProcAddr(g_vk.dev, "vkGetQueueCheckpointDataNV");
        if (!checkpoint_data_fn) vk_checkpoint_fn = NULL;
        LOG("[vk] EOWRECOMP_VK_CHECKPOINTS: draw checkpoints %s\n", vk_checkpoint_fn ? "on" : "unavailable");
    }
    LOG("[vk] graphics pipeline libraries: %s%s\n", g_vk.graphics_pipeline_library ? "enabled" : "unavailable",
        g_vk.graphics_pipeline_library ? (g_vk.gpl_fast_linking ? " (fast linking reported)" : " (fast linking not reported)") : "; using monolithic pipelines");

    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.dev, .instance = g_vk.inst,
                                 .vulkanApiVersion = VK_API_VERSION_1_3};
#ifdef VK_NO_PROTOTYPES
    VmaVulkanFunctions vf = {.vkGetInstanceProcAddr = vkGetInstanceProcAddr, .vkGetDeviceProcAddr = vkGetDeviceProcAddr};
    ai.pVulkanFunctions = &vf;
#endif
    VK_CHECK(vmaCreateAllocator(&ai, &g_vk.vma));

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.timeline));
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_vk.pool));

    // pipeline cache: <cache>/vk_pipelines.bin (written by vk_pipeline_cache_save), used only when it was made for
    // this device and driver and by this pipeline scheme (pipeline_cache_check)
    size_t sz = 0;
    void *data = NULL;
    char pcpath[600];
    snprintf(pcpath, sizeof pcpath, "%s/vk_pipelines.bin", vk_cache_dir());
    FILE *fp = fopen(pcpath, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        sz = (size_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data = malloc(sz);
        if (fread(data, 1, sz, fp) != sz) sz = 0;
        fclose(fp);
    }
    if (!pipeline_cache_check(data, sz)) sz = 0;
    VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = sz,
                                    .pInitialData = data};
    if (vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache) != VK_SUCCESS) {
        pc.initialDataSize = 0;
        VK_CHECK(vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache));
    }
    free(data);
}

// EOWRECOMP_CACHE_DIR: where the shader and pipeline caches live (default "cache" in the data folder). A test points
// it at an empty folder for a cold run without touching the player's caches.
const char *vk_cache_dir(void) {
    static char dir[512];
    if (!dir[0]) {
        const char *e = getenv("EOWRECOMP_CACHE_DIR");
        snprintf(dir, sizeof dir, "%s", e && *e ? e : "cache");
        mkdir(dir, 0755);
    }
    return dir;
}

void vk_pipeline_cache_save(void) {
    extern void vk_warm_save(void);
    // (no wait for running shader jobs: the cache is internally synchronized, and what they add later is saved next
    // time. Trirod-ShaderPrep waits for its jobs itself, with progress, before calling this.)
    vk_warm_save();  // (exec.c: the pipeline recipes recorded this run)
    size_t sz = 0;
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) != VK_SUCCESS || !sz) return;
    void *data = malloc(sz);
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, data) == VK_SUCCESS) {
        char path[600], tmp[620];
        snprintf(path, sizeof path, "%s/vk_pipelines.bin", vk_cache_dir());
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE *f = fopen(tmp, "wb");
        bool ok = false;
        if (f) {
            ok = fwrite(data, 1, sz, f) == sz;
            ok = fclose(f) == 0 && ok;
            if (ok) { remove(path); ok = rename(tmp, path) == 0; }
        }
        snprintf(path, sizeof path, "%s/vk_pipelines.tag", vk_cache_dir());
        if (ok && (f = fopen(path, "wb"))) {  // (pipeline_cache_check: Setup's preparation stays "prepared" until the
            const VkPhysicalDeviceProperties *p = &g_vk.props;  // GPU, driver or pipeline scheme changes)
            fprintf(f, "trirod-pipelines %d\nprepared %d\nvendor %u\ndevice %u\ndriver %u\ngpu %s\n", PIPELINE_SCHEME,
                    g_headless || g_pc_prepared, p->vendorID, p->deviceID, p->driverVersion, p->deviceName);
            fclose(f);
            if (g_headless) stale_note(NULL);
        }
    }
    free(data);
}

// ================================================================== swapchain
static bool g_sc_turned;  // the compositor turns our images (IDENTITY on a rotated display): presents say "suboptimal"
static VkPresentModeKHR present_mode(void) {
    // MAILBOX by default: the game paces itself (vi.c's emulated 60 Hz vsync), and with FIFO every submission
    // queued behind the one waiting for a window image, so the game's fence waits followed the presentation engine:
    // a throttled / captured / busy compositor slowed the whole game (4-21 fps with the GPU mostly idle; mailbox:
    // 60). No tearing. EOWRECOMP_VK_PRESENT=fifo|immediate|mailbox to choose.
    const char *e = getenv("EOWRECOMP_VK_PRESENT");
    VkPresentModeKHR want = !e || !strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    for (uint32_t i = 0; i < n; i++) if (m[i] == want) return want;
    return VK_PRESENT_MODE_FIFO_KHR;
}

static void create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps));
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){(uint32_t)w, (uint32_t)h};
    // A rotated display (a phone held in landscape: currentTransform 90 / 270): the images are made in the window's
    // own (landscape) shape - the extent it reports - and the compositor turns them (IDENTITY). (preTransform =
    // currentTransform would say they were turned already: the picture came out sideways. LASRecomp-Android-Plus
    // pre-rotates everything it draws instead, which saves the compositor's pass: a later optimisation.)
    VkSurfaceTransformFlagBitsKHR xf = caps.currentTransform;
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) xf = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    g_vk.sc_quarter = 0;
    bool turned_by_system = xf != caps.currentTransform;
#ifdef __ANDROID__
    // Pre-rotation: the images in the panel's own (portrait) orientation with its transform, everything drawn into
    // them turned (vk_sc_rect, the present and UI vertex shaders). Otherwise the compositor turns the whole landscape
    // layer with the GPU at every present: ~13% of the GPU's time on a Snapdragon 865 (LASRecomp-Android-Plus).
    // EOWRECOMP_PRETRANSFORM=0: the system turns them.
    static int pre = -1;
    if (pre < 0) pre = getenv("EOWRECOMP_PRETRANSFORM") ? atoi(getenv("EOWRECOMP_PRETRANSFORM")) != 0 : 1;
    if (pre && (caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
                caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) {
        xf = caps.currentTransform;
        g_vk.sc_quarter = xf == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ? 1 : 3;
        turned_by_system = false;
    }
#endif
    g_sc_turned = turned_by_system;
    if (!ext.width || !ext.height) return;  // minimised
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
    // scanout shows the bytes the game wrote (see the raw views in the present pass): a UNORM swapchain like Metal's
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; break; }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
    VkSwapchainKHR old = g_vk.swapchain;
    // (Android's compositors often take INHERIT only)
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha))
        for (uint32_t b = 1; b <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; b <<= 1)
            if (caps.supportedCompositeAlpha & b) { alpha = (VkCompositeAlphaFlagBitsKHR)b; break; }
    VkExtent2D phys = g_vk.sc_quarter ? (VkExtent2D){ext.height, ext.width} : ext;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_vk.surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = phys, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = xf,
        .compositeAlpha = alpha, .presentMode = present_mode(), .clipped = VK_TRUE,
        .oldSwapchain = old};
    VK_CHECK(vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain));
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    g_vk.sc_format = sf.format;
    g_vk.sc_extent = ext;
    g_vk.sc_phys = phys;
    if (g_vk.sc_quarter) LOG("[vk] swapchain pre-rotated %d degrees (images %ux%u)\n", g_vk.sc_quarter * 90, phys.width, phys.height);
    g_vk.sc_count = 8;
    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
    }
}

static int active_refresh_hz(void) {
    SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    const SDL_DisplayMode *mode = display ? SDL_GetCurrentDisplayMode(display) : NULL;
    int hz = mode && mode->refresh_rate > 0 ? (int)lroundf(mode->refresh_rate) : 60;
    static int last;
    if (hz != last) { LOG("[video] active display refresh: %d Hz\n", hz); last = hz; }
    return hz;
}

VkRect2D vk_sc_rect(int x, int y, int w, int h) {
    int W = (int)g_vk.sc_extent.width, H = (int)g_vk.sc_extent.height;
    if (g_vk.sc_quarter == 1) return (VkRect2D){{H - y - h, x}, {(uint32_t)h, (uint32_t)w}};
    if (g_vk.sc_quarter == 3) return (VkRect2D){{y, W - x - w}, {(uint32_t)h, (uint32_t)w}};
    return (VkRect2D){{x, y}, {(uint32_t)w, (uint32_t)h}};
}

static void recreate_swapchain(void) {
    vkDeviceWaitIdle(g_vk.dev);
    create_swapchain();
}

// Phones: an app in the background has no window - its surface is gone, and drawing waits until it's in front again,
// then the window gets a new surface and swapchain (after LASRecomp-Android-Plus). Desktop windows never get these.
static atomic_bool g_background, g_surface_stale;

static bool SDLCALL lifecycle_watch(void *user, SDL_Event *e) {
    (void)user;
    if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND || e->type == SDL_EVENT_DID_ENTER_BACKGROUND) {
        if (!atomic_exchange(&g_background, true)) {
            LOG("[vk] in the background: drawing paused\n");
            if (g_vk.dev) vk_pipeline_cache_save();  // (phones end apps in the background without exit())
        }
    } else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND && atomic_load(&g_background)) {
        LOG("[vk] in front again\n");
        atomic_store(&g_surface_stale, true);
        atomic_store(&g_background, false);
    }
    return true;
}

static void new_surface(void) {
    vkDeviceWaitIdle(g_vk.dev);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    g_vk.sc_count = 0;
    if (g_vk.swapchain) vkDestroySwapchainKHR(g_vk.dev, g_vk.swapchain, NULL);
    g_vk.swapchain = VK_NULL_HANDLE;
    if (g_vk.surface) vkDestroySurfaceKHR(g_vk.inst, g_vk.surface, NULL);
    g_vk.surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface)) {
        LOG("[vk] new surface: %s\n", SDL_GetError());
        atomic_store(&g_surface_stale, true);  // (the window isn't back yet: next frame)
        return;
    }
    create_swapchain();
}

// ================================================================== frames (start screen and game)
// Acquire semaphores rotate per frame in flight; a slot is reused once the submission that waited on it is done.
#define FRAMES 3
static VkSemaphore acquire_sem[FRAMES], done_sem[8];
static uint64_t slot_seq[FRAMES];
static int slot;

static VkSemaphore binary_semaphore(void) {
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s;
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &s));
    return s;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

typedef struct { uint32_t image; VkSemaphore acquired; bool ok; } Frame;
static Frame cur_frame;

// Acquire the next window image and begin rendering into it (cleared to `clear`). Returns false when there is
// nothing to draw into (minimised window).
bool vk_frame_begin(const float clear[4]) {
    cur_frame.ok = false;
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (!w || !h) return false;  // minimised
    if (atomic_load(&g_background)) return false;
    if (atomic_exchange(&g_surface_stale, false)) new_surface();
    if (!g_vk.surface) return false;
    if (!g_vk.swapchain || (uint32_t)w != g_vk.sc_extent.width || (uint32_t)h != g_vk.sc_extent.height) recreate_swapchain();
    if (!g_vk.swapchain) return false;
    static int trace = -1;
    if (trace < 0) trace = getenv("EOWRECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] frame begin %dx%d (swapchain %ux%u)\n", w, h, g_vk.sc_extent.width, g_vk.sc_extent.height);
    slot = (slot + 1) % FRAMES;
    if (!acquire_sem[slot]) acquire_sem[slot] = binary_semaphore();
    vk_wait_seq(slot_seq[slot]);
    VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    }
    if (r == VK_ERROR_SURFACE_LOST_KHR) { atomic_store(&g_surface_stale, true); return false; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { LOG("[vk] acquire failed: %d\n", r); return false; }
    cur_frame.acquired = acquire_sem[slot];
    slot_seq[slot] = vk_pending_seq();
    VkCommandBuffer cb = vk_cmd();
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue.color.float32 = {clear[0], clear[1], clear[2], clear[3]}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_phys}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_phys.width, (float)g_vk.sc_phys.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_phys};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    extern void vk_imgui_prerotation(int quarter);  // (the UI drawn into this image turned like it: imgui_glue.cpp)
    vk_imgui_prerotation(g_vk.sc_quarter);
    cur_frame.ok = true;
    return true;
}

// Passes of their own between vk_frame_begin and vk_frame_end (exec.c post passes): end the window image's
// rendering, and begin it again keeping what was drawn.
void vk_frame_suspend(void) {
    if (cur_frame.ok) vkCmdEndRendering(vk_cmd());
}
void vk_frame_resume(void) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_phys}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_phys.width, (float)g_vk.sc_phys.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_phys};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
}

VkExtent2D vk_frame_extent(void) { return g_vk.sc_extent; }
void *vk_frame_cmd(void) { return (void *)vk_cmd(); }

static void save_window_image(VkCommandBuffer cb, VkSpan *out) {
    uint32_t w = g_vk.sc_phys.width, h = g_vk.sc_phys.height;
    *out = vk_readback((uint64_t)w * h * 4);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rg = {.bufferOffset = out->off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out->buf, 1, &rg);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

// End rendering into the window image and present it. shot_path: also save the window image as PNG.
void vk_frame_end(const char *shot_path) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    vkCmdEndRendering(cb);
    VkSpan shot = {0};
    if (shot_path) save_window_image(cb, &shot);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    uint32_t img = cur_frame.image;
    if (!done_sem[img]) done_sem[img] = binary_semaphore();
    uint64_t seq = vk_pending_seq();
    vk_submit_extra(cur_frame.acquired, done_sem[img], VK_NULL_HANDLE);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &done_sem[img],
                           .swapchainCount = 1, .pSwapchains = &g_vk.swapchain, .pImageIndices = &img};
    VkResult r = vkQueuePresentKHR(g_vk.queue, &pi);
    if (r == VK_ERROR_SURFACE_LOST_KHR) atomic_store(&g_surface_stale, true);
    // (a swapchain the system turns, IDENTITY on a rotated display, is "suboptimal" at every present: that's this,
    // not a new size - recreating it each frame would cost the frame)
    else if (r == VK_ERROR_OUT_OF_DATE_KHR || (r == VK_SUBOPTIMAL_KHR && !g_sc_turned)) recreate_swapchain();
    cur_frame.ok = false;
    if (shot_path) {
        vk_wait_seq(seq);
        uint32_t w = g_vk.sc_phys.width, h = g_vk.sc_phys.height;
        uint8_t *px = shot.ptr;
        bool bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
        for (uint64_t i = 0; bgr && i < (uint64_t)w * h; i++) { uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
        for (uint64_t i = 0; i < (uint64_t)w * h; i++) px[i * 4 + 3] = 255;
        if (g_vk.sc_quarter) {  // (the pre-rotated image turned back into the window's shape)
            uint32_t W = h, H = w;  // landscape
            uint8_t *t = malloc((size_t)W * H * 4);
            for (uint32_t y = 0; y < H; y++)
                for (uint32_t x = 0; x < W; x++) {
                    uint32_t qx = g_vk.sc_quarter == 1 ? H - 1 - y : y, qy = g_vk.sc_quarter == 1 ? x : W - 1 - x;
                    memcpy(t + ((size_t)y * W + x) * 4, px + ((size_t)qy * w + qx) * 4, 4);
                }
            vk_save_png(shot_path, t, W, H);
            free(t);
        } else {
            vk_save_png(shot_path, px, w, h);
        }
    }
}

void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void *)rgba, (int)w * 4);
    if (!s || !SDL_SavePNG(s, path)) LOG("[vk] saving %s failed: %s\n", path, SDL_GetError());
    else LOG("[vk] saved %s (%ux%u)\n", path, w, h);
    SDL_DestroySurface(s);
}

// ================================================================== game presentation (metal.m backend_present)
// Draws the game's image into the window with the widescreen crop, then the overlay menu.
extern void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]);
void renderer_submit(void);
void renderer_set_scale(float sx, float sy);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);

static uint64_t frames, total_frames, g_frame_counter;
uint64_t renderer_present_count(void) { return total_frames; }
uint64_t vk_frame_counter(void) { return g_frame_counter; }
static float g_shot_crop[4] = {0, 0, 1, 1};
static double last_fps_time;
static NvnTexture *g_last_presented;

const float *vk_shot_crop(void) { return g_shot_crop; }

// A frame with draws skipped because their shader program or pipeline was still building (exec.c compile_wait) has
// holes, or is black where a skipped draw fed a later pass. Such a frame is not shown: nothing is acquired or
// presented, so the window keeps the last complete image (the game's pacing is its own virtual vsync, vi.c). At most
// EOWRECOMP_VK_HOLD_MS in a row (default 500, 0 = off); after that frames are shown as they are until a complete one,
// so the game never looks frozen for long. The builds themselves run on the shader threads, not here.
static bool hold_frame(uint64_t now) {
    extern uint32_t vk_frame_pending_skips(void);
    static int hold_ms = -1;
    static long shot_frame = -1;
    static uint64_t since;
    static uint32_t held, held_skips;
    if (hold_ms < 0) {
        const char *e = getenv("EOWRECOMP_VK_HOLD_MS");
        hold_ms = e && *e ? atoi(e) : 500;
        shot_frame = getenv("EOWRECOMP_WINDOW_SHOT") ? atol(getenv("EOWRECOMP_WINDOW_SHOT")) : 0;
    }
    uint32_t pending = vk_frame_pending_skips();
    bool shot = shot_frame && (long)total_frames + 1 == shot_frame;  // (the test's window shot is always taken)
    if (pending && hold_ms > 0 && total_frames && !shot && (!since || now - since < (uint64_t)hold_ms * 1000000ull)) {
        if (!since) {
            since = now;
            static bool told;
            if (!told) told = true, LOG("[vk] shaders still building: keeping the last complete image (up to %d ms)\n", hold_ms);
        }
        held++;
        held_skips += pending;
        return true;
    }
    if (held)
        LOG_VERBOSE("[vk] kept the last complete image for %u frames (%.0f ms) while %u draws waited for shader or "
                    "pipeline builds (%u shader jobs queued)%s\n", held, (now - since) / 1e6, held_skips,
                    vk_exec_jobs_pending(), pending ? "; limit reached, showing frames with missing draws" : "");
    if (pending && since && now - since >= (uint64_t)hold_ms * 1000000ull) {  // (once per stretch: not verbose)
        static uint64_t told_for;
        if (told_for != since) {
            told_for = since;
            LOG("[vk] shaders still building after %d ms: showing frames with %u draws missing (%u shader jobs queued)\n",
                hold_ms, pending, vk_exec_jobs_pending());
        }
    }
    held = held_skips = 0;
    if (!pending) since = 0;  // (after the limit: hold again only once a complete frame was shown)
    return false;
}

void backend_present(NvnTexture *t);
void backend_represent(void) {
    if (g_last_presented) backend_present(g_last_presented);
    else SDL_Delay(16);
}

void backend_present(NvnTexture *t) {
    g_last_presented = t;
    TexBackend *b = vk_tex(t, true);
    b->presented = true;
    vk_end_pass();
    // The presentation shader samples the just-rendered game image in a separate rendering scope.
    // Pass switches inside exec.c establish this dependency in begin_pass(), but presentation starts
    // its own scope in vk_frame_begin() and bypasses that path. Keep attachment writes visible to the
    // fragment shader before the sampled read (the image stays in GENERAL throughout).
    vk_full_barrier(vk_cmd());
    static const float black[4] = {0, 0, 0, 1};
    uint64_t t0 = SDL_GetTicksNS();
    bool have = !hold_frame(t0) && vk_frame_begin(black);
    uint64_t nd = SDL_GetTicksNS() - t0;
    if (nd > 30000000) {
        static int warned;
        if (warned++ < 20) LOG("[vk] acquire blocked %llums\n", (unsigned long long)(nd / 1000000));
    }
    if (have) {
        double dw = g_vk.sc_extent.width, dh = g_vk.sc_extent.height;
        extern void widescreen_frame_end(uint64_t frame);
        widescreen_frame_end(total_frames);
        extern void widescreen_set_factor(float f);
        extern bool widescreen_allowed(void);
        extern uint64_t rt_frame_limit_sleep_ns(void);
        // render scale + placement in the window: runtime/nvn/output_scale.c (Video tab "Internal resolution")
        // no GPU timer here (-1): dynamic resolution goes by the frame time
        OutScale os;
        double scale_time = (double)SDL_GetTicksNS() / 1e9 - (double)rt_frame_limit_sleep_ns() / 1e9;
        output_scale(dw, dh, widescreen_allowed(), scale_time, -1, &os);
        double sx = os.sx, sy = os.sy, vx = os.vx, vy = os.vy, vw = os.vw, vh = os.vh, shape = os.shape;
        // interiors / dungeons: their 16:9 centre, or the part inside a room wider than one screen
        extern void widescreen_present_crop(uint64_t, double, float *, double *, double *, double *, double *);
        float crop[4];  // uv origin and size
        widescreen_present_crop(total_frames, shape, crop, &vx, &vy, &vw, &vh);
        widescreen_set_factor((float)shape);
        renderer_set_scale((float)sx, (float)sy);
        VkCommandBuffer cb = vk_cmd();
        float pv[8] = {renderer_origin_lower_left() ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
        memcpy(g_shot_crop, crop, sizeof crop);
        vk_present_draw(cb, b, (float)vx, (float)vy, (float)vw, (float)vh, pv);
        static long menu_at = -1;
        if (menu_at < 0) menu_at = getenv("EOWRECOMP_MENU_AT") ? atol(getenv("EOWRECOMP_MENU_AT")) : 0;
        if (menu_at && (long)total_frames + 1 == menu_at) { extern void menu_test_open(void); menu_test_open(); }
        menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
        static long win_shot = -1;
        if (win_shot < 0) win_shot = getenv("EOWRECOMP_WINDOW_SHOT") ? atol(getenv("EOWRECOMP_WINDOW_SHOT")) : 0;
        bool shot = win_shot && (long)total_frames + 1 == win_shot;
        vk_frame_end(shot ? getenv("EOWRECOMP_SHOT_PATH") ? getenv("EOWRECOMP_SHOT_PATH") : "build/window.png" : NULL);
        if (shot && getenv("EOWRECOMP_EXIT_AFTER_SHOT")) {
            vk_pipeline_cache_save();  // (test runs keep their caches and recipes: cold / warm comparisons)
            fflush(stderr);
            _exit(0);
        }
    } else {
        renderer_submit();
    }
    extern void vk_prof_frame(uint64_t frame, uint64_t present_ns);
    vk_prof_frame(total_frames, SDL_GetTicksNS() - t0);
    frames++;
    total_frames++;
    g_frame_counter++;
    extern void rt_frame_limit_wait(int monitor_hz);
    rt_frame_limit_wait(active_refresh_hz());
    extern void vk_pace_frame(void);
    vk_pace_frame();  // (exec.c: the game's 30 fps mode when frames don't fit 60)
    extern void input_frame(uint64_t frame);
    input_frame(total_frames);
    static long slow_ms = -1;
    if (slow_ms < 0) slow_ms = getenv("EOWRECOMP_SLOW_FRAME_MS") ? atol(getenv("EOWRECOMP_SLOW_FRAME_MS")) : 0;
    static long slow_until = -1;  // EOWRECOMP_SLOW_FRAME_UNTIL=<frame>: the stall ends there (frame pacing recovery tests)
    if (slow_until < 0) slow_until = getenv("EOWRECOMP_SLOW_FRAME_UNTIL") ? atol(getenv("EOWRECOMP_SLOW_FRAME_UNTIL")) : 0;
    if (slow_ms > 0 && total_frames > 2600 && (!slow_until || (long)total_frames < slow_until)) SDL_Delay((Uint32)slow_ms);
    extern void fnprof_frame(uint64_t frame);
    fnprof_frame(total_frames);
    extern void reboot_test_frame(uint64_t frame);  // runtime/main.c (EOWRECOMP_REBOOT_AT)
    reboot_test_frame(total_frames);
    extern void vk_level_change_check(void);
    vk_level_change_check();
    static long shot_every = -1;
    if (shot_every < 0) { const char *e = getenv("EOWRECOMP_SCREENSHOT_EVERY"); shot_every = e ? atol(e) : 0; }
    extern bool g_debug_frame;
    static long debug_at = -1;
    if (debug_at < 0) { const char *e = getenv("EOWRECOMP_DEBUG_FRAME"); debug_at = e ? atol(e) : 0; }
    extern volatile int g_debug_frame_request;
    if (g_debug_frame_request) { g_debug_frame_request = 0; debug_at = (long)total_frames + 1; }
    // EOWRECOMP_DEBUG_FRAMES=<n> (with EOWRECOMP_DEBUG_FRAME / F9): dump n consecutive frames (build/pass_<frame>_NNN.png)
    static long debug_n = -1;
    if (debug_n < 0) { const char *e = getenv("EOWRECOMP_DEBUG_FRAMES"); debug_n = e && atol(e) > 1 ? atol(e) : 1; }
    g_debug_frame = debug_at && total_frames >= (uint64_t)debug_at && total_frames < (uint64_t)debug_at + (uint64_t)debug_n;
    {   // EOWRECOMP_GPUPROF_AT=<frame>[,<frame>...]: GPU time per pass of those frames ("[gpuprof]" in the log)
        extern bool g_gpuprof_frame;
        extern void vk_gpuprof_report(void);
        if (g_gpuprof_frame) { g_gpuprof_frame = false; vk_gpuprof_report(); }
        const char *e = getenv("EOWRECOMP_GPUPROF_AT");
        for (const char *p = e; p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            if (at == (long)total_frames) { g_gpuprof_frame = true; break; }
            if (*p == ',') p++; else break;
        }
    }
    if (g_debug_frame) LOG("===== debug frame %llu =====\n", (unsigned long long)total_frames);
    {   // EOWRECOMP_SHOT_AT=<frame>,<frame>,... (ascending): the game image at those frames -> build/frames/frame_N.png
        static const char *next;
        static int init;
        if (!init) { init = 1; next = getenv("EOWRECOMP_SHOT_AT"); if (next) mkdir("build/frames", 0755); }
        while (next && *next) {
            long at = strtol(next, (char **)&next, 10);
            if ((long)total_frames < at) { while (next > getenv("EOWRECOMP_SHOT_AT") && next[-1] != ',') next--; break; }
            if ((long)total_frames == at) {
                char path[256];
                snprintf(path, sizeof path, "build/frames/frame_%05llu.png", (unsigned long long)total_frames);
                renderer_save_texture_tex(b, path);
            }
            if (*next == ',') next++;
        }
    }
    static long shot_from = -1;
    if (shot_from < 0) { const char *e = getenv("EOWRECOMP_SCREENSHOT_FROM"); shot_from = e ? atol(e) : 0; }
    if (shot_every && total_frames % shot_every == 0 && (long)total_frames >= shot_from) {
        char path[256];
        if (shot_from) mkdir("build/frames", 0755);
        snprintf(path, sizeof path, shot_from ? "build/frames/frame_%05llu.png" : "build/frame_%05llu.png",
                 (unsigned long long)total_frames);
        renderer_save_texture_tex(b, path);
        // a series (SCREENSHOT_FROM) runs on until EOWRECOMP_WINDOW_SHOT exits
        if (getenv("EOWRECOMP_EXIT_AFTER_SHOT") && !shot_from) { fflush(stderr); _exit(0); }
    }
    double now = SDL_GetTicks() / 1000.0;
    static double fps_iv = -1, prev_present, max_gap;
    if (fps_iv < 0) fps_iv = getenv("EOWRECOMP_FPS_INTERVAL") ? atof(getenv("EOWRECOMP_FPS_INTERVAL")) : 2.0;
    if (prev_present > 0 && now - prev_present > max_gap) max_gap = now - prev_present;
    prev_present = now;
    if (now - last_fps_time >= fps_iv) {
        extern const char *game_level_name(void);
        if (last_fps_time > 0) {
            // (always run: renderer_stats also closes the interval's counters and saves new pipeline recipes; only
            // the line itself is a verbose diagnostic)
            const char *stats = renderer_stats();
            LOG_VERBOSE("[vk] t=%.1f frame %llu %.1f fps max %.0fms level %s (%s)\n", now, (unsigned long long)total_frames,
                frames / (now - last_fps_time), max_gap * 1000.0, game_level_name(), stats);
        }
        max_gap = 0;
        frames = 0;
        last_fps_time = now;
    }
}

// ================================================================== UI thread
void vk_exec_init(void);
void vk_shaders_init(void);

// exit() from any thread: let the GPU finish, keep the pipeline cache, then end the process without the DLL
// teardown (driver / validation-layer threads can stall it for minutes while the game's threads still run).
static void quick_exit_handler(void) {
    static int trace = -1;
    if (trace < 0) trace = getenv("EOWRECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] exit: waiting for the GPU\n");
    if (g_vk.dev) {
        char sum[1024];
        vk_pipeline_session_summary(sum, sizeof sum);
        LOG("[vk] session %s\n", sum);
        vkDeviceWaitIdle(g_vk.dev);
        if (trace) LOG("[vk] exit: saving the pipeline cache\n");
        vk_pipeline_cache_save();
    }
    if (trace) LOG("[vk] exit: terminating\n");
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate();
#endif
}

// macOS: the .app's own MoltenVK (Contents/Resources/vulkan/icd.d, tools/bundle_macos.sh), not another one installed
// on the machine (Homebrew's: a different version, and the device listed twice); a source build has none. Before
// SDL_Init: SDL's Vulkan loading scans the drivers too.
static void use_bundled_driver(void) {
#ifdef __APPLE__
    char icd[1100];
    if (!getenv("VK_DRIVER_FILES") && !getenv("VK_ICD_FILENAMES") &&
        rt_resource("vulkan/icd.d/MoltenVK_icd.json", NULL, icd, sizeof icd))
        setenv("VK_DRIVER_FILES", icd, 1);
#endif
}

void ui_window_init(void) {
    if (window) return;
    atexit(quick_exit_handler);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");  // (Back opens the menu, not "close the app")
#endif
    settings_load();
    use_bundled_driver();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) FATAL("SDL_Init: %s\n", SDL_GetError());
    int win_w = 1280, win_h = 720;
    if (getenv("EOWRECOMP_WINDOW")) sscanf(getenv("EOWRECOMP_WINDOW"), "%dx%d", &win_w, &win_h);
    SDL_WindowFlags wflags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (getenv("EOWRECOMP_HIDDEN")) wflags |= SDL_WINDOW_HIDDEN;
#ifdef __ANDROID__
    wflags |= SDL_WINDOW_FULLSCREEN;  // (immersive: no status / navigation bars over the game)
#endif
    {   // scripted test runs (user input ignored: runtime/hle/input.c input_user_blocked) open without taking the
        // keyboard focus from whatever the player is doing
        const char *v = getenv("EOWRECOMP_NO_USER_INPUT");
        bool scripted = v ? atoi(v) != 0 : (getenv("EOWRECOMP_INPUT") || getenv("EOWRECOMP_DOORJUMP_TEST") || getenv("EOWRECOMP_PLACE") ||
                                            getenv("EOWRECOMP_HOST_INPUT") || getenv("EOWRECOMP_BIND_TEST"));
        if (scripted) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    }
    window = SDL_CreateWindow("TriRod", win_w, win_h, wflags);
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    SDL_AddEventWatch(lifecycle_watch, NULL);
    rt_window_icon(window);
    // the Fullscreen setting applies from the start screen on (not only once the game runs); test windows stay windows
    if (g_settings.fullscreen && !getenv("EOWRECOMP_WINDOW") && !getenv("EOWRECOMP_HIDDEN")) SDL_SetWindowFullscreen(window, true);
    create_instance();
    if (!SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface)) FATAL("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    pick_device();
    create_device();
    create_swapchain();
    vk_exec_init();
    vk_shaders_init();
    extern void vk_warm_load_records(void);
    vk_warm_load_records();  // (exec.c: saved recipes stay in the file the game rewrites; nothing is built here)
}

SDL_Window *ui_window(void) { return window; }

// Trirod-ShaderPrep (runtime/tools/shaderprep.c): what ui_window_init does for the game, minus the window, surface and
// swapchain, so pipelines and SPIR-V land in the same caches for the same device. The device rule is the game's
// (settings' GPU, else discrete > integrated > other) except that presenting can't be checked without a window.
void vk_headless_init(void) {
    g_headless = true;
    settings_load();
    create_instance();
    pick_device();
    create_device();
    vk_exec_init();
    vk_shaders_init();
}

void vk_headless_device(char *name, size_t n, uint32_t *vendor, uint32_t *device, uint32_t *driver, uint32_t *api) {
    snprintf(name, n, "%s", g_vk.props.deviceName);
    *vendor = g_vk.props.vendorID;
    *device = g_vk.props.deviceID;
    *driver = g_vk.props.driverVersion;
    *api = g_vk.props.apiVersion;
}

void vk_device_idle(void) {
    if (g_vk.dev) vkDeviceWaitIdle(g_vk.dev);
}

// ---- shader preparation in game, after a GPU or driver change (pipeline_cache_check decided: g_prep_in_game)
// Setup runs Trirod-ShaderPrep before Play; choosing another GPU in the game's settings, or a driver update, leaves a
// cache for the old one. Then the game runs the same tool itself before it starts (the game thread waits here, as
// for the old startup warmup), shows its progress in the window (menu_set_progress) and merges the prepared cache into
// its own. Esc / B ends the tool: the game starts and builds what it meets, as without preparation.
#ifdef _WIN32
// Is `exe` built with the same runtime as this game? ("Trirod-ShaderPrep <version> runtime <id> ..." from --version
// against Trirod.exe.id, written beside the game when it was linked.) A data folder can hold a stale copy of the tool:
// the user's first in-game preparation ran one from 09:19 that predated this code.
static bool shaderprep_matches(const char *exe, const char *want) {
    char log[1200], line[300], id[64];
    snprintf(log, sizeof log, "%s/logs/shaderprep-version.txt", rt_data_root());
    const char *argv[] = {exe, "--version", NULL};
    PlatProc p = plat_spawn(argv, log, false);
    if (!p || plat_proc_wait(p, true) != 0) return false;
    FILE *f = fopen(log, "rb");
    bool ok = f && fgets(line, sizeof line, f) && sscanf(line, "Trirod-ShaderPrep %*s runtime %63s", id) == 1 &&
              !strcmp(id, want);
    if (f) fclose(f);
    if (!ok) LOG("[vk] %s is from another runtime build (wants %s): not used\n", exe, want);
    return ok;
}

static bool find_shaderprep(char *out, size_t n) {
    char want[64] = "", idp[1200];
    if (!plat_exe_path(idp, sizeof idp - 4)) return false;
    strcat(idp, ".id");
    FILE *f = fopen(idp, "rb");
    if (f) { if (fgets(want, sizeof want, f)) want[strcspn(want, "\r\n")] = 0; fclose(f); }
    if (!want[0]) { LOG("[vk] %s missing: cannot tell which Trirod-ShaderPrep matches this game\n", idp); return false; }
    // the install folder first (Play puts it first on PATH; Trirod.exe is in the data folder), then beside the game
    const char *path = getenv("PATH");
    struct stat st;
    for (const char *p = path; p && *p;) {
        const char *e = strchr(p, ';');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len && (size_t)snprintf(out, n, "%.*s\\Trirod-ShaderPrep.exe", (int)len, p) < n && stat(out, &st) == 0 &&
            shaderprep_matches(out, want))
            return true;
        p = e ? e + 1 : p + len;
    }
    return rt_resource("Trirod-ShaderPrep.exe", NULL, out, n) && shaderprep_matches(out, want);
}

static bool merge_prepared_cache(void) {
    char path[600];
    snprintf(path, sizeof path, "%s/vk_pipelines.bin", vk_cache_dir());
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    size_t sz = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    void *data = malloc(sz ? sz : 1);
    bool ok = fread(data, 1, sz, f) == sz;
    fclose(f);
    const VkPipelineCacheHeaderVersionOne *h = data;
    ok = ok && sz >= sizeof *h && h->vendorID == g_vk.props.vendorID && h->deviceID == g_vk.props.deviceID &&
         !memcmp(h->pipelineCacheUUID, g_vk.props.pipelineCacheUUID, VK_UUID_SIZE);
    VkPipelineCache tmp = VK_NULL_HANDLE;
    VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = sz,
                                    .pInitialData = data};
    ok = ok && vkCreatePipelineCache(g_vk.dev, &ci, NULL, &tmp) == VK_SUCCESS &&
         vkMergePipelineCaches(g_vk.dev, g_vk.pcache, 1, &tmp) == VK_SUCCESS;
    if (tmp) vkDestroyPipelineCache(g_vk.dev, tmp, NULL);
    free(data);
    return ok;
}

static void prepare_shaders_in_game(void) {
    if (!g_prep_in_game || g_headless) return;
    char path[700], exe[1200], log[1200], title[200], detail[420] = "", text[320] = "", line[700];
    snprintf(path, sizeof path, "%s/prep-attempt.txt", vk_cache_dir());  // (first: a crash doesn't retry every start)
    FILE *f = fopen(path, "wb");
    if (f) fprintf(f, "%s\n", g_prep_key), fclose(f);
    snprintf(log, sizeof log, "%s/logs", rt_data_root());
    mkdir(log, 0755);
    if (!find_shaderprep(exe, sizeof exe)) {
        LOG("[vk] %s; Trirod-ShaderPrep.exe not found (start the game with the launcher's Play): shaders are built "
            "as the game meets them\n", g_prep_why);
        return;
    }
    snprintf(log, sizeof log, "%s/logs", rt_data_root());
    mkdir(log, 0755);
    snprintf(log, sizeof log, "%s/logs/shaderprep-game.log", rt_data_root());
    const char *argv[] = {exe, "--data", rt_data_root(), NULL};
    PlatProc pr = plat_spawn(argv, log, false);
    if (!pr) { LOG("[vk] could not start %s: shaders are built as the game meets them\n", exe); return; }
    LOG("[vk] %s: preparing shaders for %s before the game starts (Esc / B skips; output in %s)\n", g_prep_why,
        g_vk.props.deviceName, log);
    snprintf(title, sizeof title, "Preparing shaders for %s", g_vk.props.deviceName);
    snprintf(text, sizeof text, "Starting the shader preparation");
    float fraction = 0;
    long pos = 0;
    int code = -1;
    bool skipped = false;
    uint64_t t0 = SDL_GetTicksNS();
    static const float black[4] = {0, 0, 0, 1};
    for (;;) {
        code = plat_proc_wait(pr, false);
        FILE *lf = fopen(log, "rb");
        if (lf) {  // the tool's events (runtime/tools/protocol.h), whole lines only
            fseek(lf, pos, SEEK_SET);
            while (fgets(line, sizeof line, lf) && strchr(line, '\n')) {
                pos = ftell(lf);
                line[strcspn(line, "\r\n")] = 0;
                float fr;
                int k = 0;
                if (sscanf(line, "@@progress %f %n", &fr, &k) == 1) {
                    fraction = fr < 0 ? 0 : fr > 1 ? 1 : fr;
                    if (k) snprintf(text, sizeof text, "%s", line + k);
                } else if (!strncmp(line, "@@info ", 7) || !strncmp(line, "@@warn ", 7) || !strncmp(line, "@@error ", 8))
                    LOG("[shaderprep] %s\n", line + 2);
            }
            fclose(lf);
        }
        uint64_t s = (SDL_GetTicksNS() - t0) / 1000000000ull;
        snprintf(detail, sizeof detail, "%s.\n%s (%llu:%02llu)", g_prep_why, text, (unsigned long long)(s / 60),
                 (unsigned long long)(s % 60));
        menu_set_progress(title, detail, fraction);
        if (code >= 0) break;
        if (menu_progress_skipped()) { plat_proc_kill(pr); skipped = true; break; }
        if (vk_frame_begin(black)) {
            menu_render((void *)vk_cmd(), g_vk.sc_extent.width, g_vk.sc_extent.height);
            vk_frame_end(NULL);
        }
        SDL_Delay(33);
    }
    menu_set_progress(NULL, NULL, -1);
    double secs = (SDL_GetTicksNS() - t0) / 1e9;
    if (skipped) LOG("[vk] shader preparation skipped after %.0f s: shaders are built as the game meets them\n", secs);
    else if (code == 0 && merge_prepared_cache()) {
        g_pc_prepared = true;  // (saved with "prepared 1" at exit)
        stale_note(NULL);
        LOG("[vk] shaders prepared for %s in %.0f s\n", g_vk.props.deviceName, secs);
    } else LOG("[vk] shader preparation ended with status %d after %.0f s (see %s): shaders are built as the game "
               "meets them\n", code, secs, log);
}
#else
static void prepare_shaders_in_game(void) {}
#endif

// ---- the recorded complete pipelines, built before the game starts (as TOTK's "pipeline keys seen in play are built
// at boot" and Triforce's precompile before the game starts)
// Trirod-ShaderPrep only fills the driver's cache: each pipeline still needs its program translated (one serialized
// translator thread) and vkCreateGraphicsPipelines called, and until then a draw that needs it is skipped. Without this
// step, loading a save showed the scene's skipped draws as a black or half-drawn picture while each was built
// (2026-11-24). With the caches prepared the build is quick: 164 recipes took 0.2 s in session 16b, and the same
// scripted save load went from 440 skipped draws to 26. The panel shows only when it takes longer than a moment;
// Esc / B starts the game anyway (the rest keeps building in the background, ahead of new pipelines' turn: they wait
// for it, compile_wait "known"). EOWRECOMP_VK_WARM=0: no recipes; EOWRECOMP_VK_WARM_WAIT=<s>: longest wait (0: none).
static void warm_boot(void) {
    vk_warm_start();
    int done = 0, total = 0;
    if (!vk_warm_progress(&done, &total)) return;
    const char *e = getenv("EOWRECOMP_VK_WARM_WAIT");
    double limit = e && *e ? atof(e) : 180;
    uint64_t t0 = SDL_GetTicksNS();
    bool shown = false;
    static const float black[4] = {0, 0, 0, 1};
    char title[200], detail[200];
    snprintf(title, sizeof title, "Loading shaders for %s", g_vk.props.deviceName);
    while (vk_warm_progress(&done, &total)) {
        double s = (SDL_GetTicksNS() - t0) / 1e9;
        if (s >= limit) { LOG("[warm] still building after %.0f s: the game starts, the rest builds meanwhile\n", s); break; }
        if (s > 0.3) {
            VkWarmSummary w;
            vk_warm_summary(&w);
            snprintf(detail, sizeof detail, "Recorded pipelines: programs %d/%d, pipelines %d/%d", w.programs_done, w.programs,
                     w.pipelines_done, w.pipelines);
            menu_set_progress(title, detail, total ? (float)done / total : 0);
            shown = true;
            if (menu_progress_skipped()) { LOG("[warm] skipped after %.1f s: the rest builds meanwhile\n", s); break; }
            if (vk_frame_begin(black)) {
                menu_render((void *)vk_cmd(), g_vk.sc_extent.width, g_vk.sc_extent.height);
                vk_frame_end(NULL);
            }
        }
        SDL_Delay(16);
    }
    if (shown) menu_set_progress(NULL, NULL, -1);
}

void backend_init(void) {
    while (!atomic_load(&ui_ready)) SDL_Delay(1);
    LOG("[vk] %s, swapchain %ux%u\n", g_vk.props.deviceName, g_vk.sc_extent.width, g_vk.sc_extent.height);
    prepare_shaders_in_game();
    warm_boot();  // (after a preparation: its merged recipes are in cache/warm now)
}

// ---- in-place reboot (runtime/main.c teardown; main thread, guest threads stopped)
void backend_reset(void) {
    if (!g_vk.dev) return;
    extern void vk_exec_wait_jobs(void), vk_exec_drop_optimize_jobs(void);
    vk_exec_drop_optimize_jobs();  // (queued optimized replacements are dropped, not waited for)
    vk_exec_wait_jobs();  // no background shader or pipeline job may use objects while exec tables are reset
    vkDeviceWaitIdle(g_vk.dev);
    extern void vk_textures_reset(void);
    extern void vk_exec_reset(void);
    vk_textures_reset();
    vk_exec_reset();
    g_last_presented = NULL;
}
void backend_guest_memory_reset(void) {}  // (exec.c re-imports the heap on first use)
void backend_clear_window(void) {
    static const float black[4] = {0, 0, 0, 1};
    if (g_vk.dev && vk_frame_begin(black)) vk_frame_end(NULL);
}

int ui_run(void) {
    ui_window_init();
    atomic_store(&ui_ready, true);
    for (;;) {
        if (rt_reboot_requested()) return rt_reboot_requested();  // (runtime/main.c tears the game down)
        {   // the shader warmup's progress in the window title
            extern bool vk_warm_progress(int *done, int *total);
            static int shown = -1;
            int done = 0, total = 0, pct = vk_warm_progress(&done, &total) && total ? done * 100 / total : -1;
            if (pct != shown) {
                char title[64];
                if (pct >= 0) snprintf(title, sizeof title, "TriRod - preparing shaders %d%%", pct);
                else snprintf(title, sizeof title, "TriRod");
                SDL_SetWindowTitle(window, title);
                shown = pct;
            }
        }
        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 16)) {
            if (e.type == SDL_EVENT_QUIT) {
                LOG("window closed\n");
                exit(0);
            }
            extern bool input_user_blocked(const SDL_Event *e);
            if (input_user_blocked(&e)) continue;  // scripted test runs: the real keyboard / mouse / pads do nothing (as metal.m)
            if (menu_handle_event(&e)) continue;
            if (touch_handle_event(&e)) continue;  // (phones: the on-screen controls)
            input_handle_event(&e);
        }
        menu_apply_window_requests(window);
    }
}
