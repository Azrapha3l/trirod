// Executes recorded NVN command lists with Vulkan (port of renderer.m; same structure, same behaviour).
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../../kernel.h"
#include "shaders.h"
#include "vk.h"
#include "../renderer.h"
#include "../../settings.h"
#include "prep.h"

static uint64_t stat_draws, stat_skipped, stat_passes, stat_dispatches, stat_wait_ns, stat_flushes, stat_feedback_copies;
static uint64_t stat_gstore_waits;  // (gstore_sync_cbufs)
static uint64_t stat_indirect, stat_indirect_waits, stat_indirect_empty;  // (draw_indirect)
// why draws / dispatches were skipped (the fps log line: "skipped N (why...)")
enum { SKIP_NO_PROGRAM, SKIP_PROG_PENDING, SKIP_PROG_FAILED, SKIP_PIPE_PENDING, SKIP_PIPE_FAILED, SKIP_TOPOLOGY,
       SKIP_COMPUTE_PROG, SKIP_LIST, SKIP_NREASONS };
static const char *const skip_names[SKIP_NREASONS] = {"no program", "program compiling", "program failed",
    "pipeline compiling", "pipeline failed", "topology", "compute program", "skip list"};
static uint64_t stat_skip_why[SKIP_NREASONS];
// draws skipped since the last present because their program / pipeline was still building (backend_present holds
// the previous image instead of showing a frame with holes: vk_frame_pending_skips)
static uint32_t frame_pending_skips;
#define SKIP(why) (stat_skipped++, stat_skip_why[why]++, frame_pending_skips += (why) == SKIP_PROG_PENDING || (why) == SKIP_PIPE_PENDING)
uint32_t vk_frame_pending_skips(void) { uint32_t n = frame_pending_skips; frame_pending_skips = 0; return n; }
static uint64_t stat_upload_bytes;
// EOWRECOMP_VK_PROF=1: render-thread time per category in the fps log (exec = whole command lists, draw = draw(),
// sets = descriptor sets incl. cbuf copies, vtx = index scan + vertex copies, submit = vkQueueSubmit2)
static int prof = -1;
static uint64_t prof_exec, prof_draw, prof_sets, prof_vtx, prof_submit, prof_pre, prof_key, prof_dyn, prof_tex, prof_up, prof_gpu;
static VkQueryPool prof_qp;
static uint64_t cum_exec, cum_wait, cum_draws, cum_submits;  // (profiling) never reset: hitch log
static uint32_t prof_end_line[4096];  // passes ended, by the exec.c line that ended them (0: submit / present)
uint64_t prof_views_created;  // textures.c vk_image_view cache misses
uint64_t prof_up_ns = ~0ull;  // textures.c upload checks (~0: profiling off)
#define PROF_T0() uint64_t prof_t0_ = prof ? SDL_GetTicksNS() : 0
#define PROF_ADD(v) do { if (prof) v += SDL_GetTicksNS() - prof_t0_; } while (0)
static _Atomic int stat_compiles;
static uint64_t op_counts[CMD_COUNT];
bool g_debug_frame;
static char stats_text[2048];
void vk_collect_dead(void);

// ================================================================== submissions
// A submission = optional upload command buffer + the main one, signalling the timeline with its number.
#define SUBS 8
typedef struct {
    VkCommandBuffer main, upload;
    bool main_begun, upload_begun, ts;  // ts: GPU timestamps written (profiling)
    uint64_t seq;
    VkDescriptorPool pools[16];
    int npools, cur_pool;
} Sub;
static Sub subs[SUBS];
static int cur_sub;
static void ss_forget(void);
static uint64_t submit_seq;  // last submitted
static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;
static bool in_pass;

uint64_t vk_completed_seq(void) {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(g_vk.dev, g_vk.timeline, &v);
    return v;
}

// (EOWRECOMP_VK_PROF) waits on the GPU by cause: the caller names it in wait_why just before waiting
static __thread const char *wait_why;
static struct { const char *why; uint64_t ns, n; } wait_tab[12];
static pthread_mutex_t wait_tab_lock = PTHREAD_MUTEX_INITIALIZER;
static void wait_account(uint64_t ns) {
    const char *why = wait_why ? wait_why : "other";
    wait_why = NULL;
    pthread_mutex_lock(&wait_tab_lock);
    for (int i = 0; i < 12; i++)
        if (!wait_tab[i].why || wait_tab[i].why == why) { wait_tab[i].why = why; wait_tab[i].ns += ns; wait_tab[i].n++; break; }
    pthread_mutex_unlock(&wait_tab_lock);
}

static void cq_resolve_done(uint64_t done);
void vk_devlost_dump(void);

void vk_wait_seq(uint64_t seq) {
    if (!seq) { wait_why = NULL; return; }
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_vk.timeline,
                              .pValues = &seq};
    uint64_t t0 = SDL_GetTicksNS();
    VkResult r = vkWaitSemaphores(g_vk.dev, &wi, 10ull * 1000000000ull);
    stat_wait_ns += SDL_GetTicksNS() - t0;
    cum_wait += SDL_GetTicksNS() - t0;
    wait_account(SDL_GetTicksNS() - t0);
    if (r == VK_TIMEOUT) {
        LOG("[vk] GPU did not finish submission %llu within 10 s (device lost?)\n", (unsigned long long)seq);
        vk_devlost_dump();
    }
    else if (r == VK_SUCCESS) cq_resolve_done(seq);  // (the game reads its timing counters after waiting)
    else {
        extern void vk_devlost_dump(void);
        vk_devlost_dump();
        vk_device_fault_report();
        FATAL("vkWaitSemaphores: %d (device lost)\n", r);
    }
}

uint64_t vk_pending_seq(void) { return submit_seq + 1; }

// ---- the game's GPU timing counters (nvnCommandBufferReportCounter, two per frame)
// The game times its GPU work with these and drops to 30 fps when frames look slow. They used to get the host clock
// at translation time, so our command translation counted as GPU time: busy scenes (outdoors, ~400 draws) looked slow
// and the game locked itself at 30 even on fast GPUs, at any resolution. Now each counter also records a GPU
// timestamp at its place in the command stream; once that submission is done (the game waits on its fence before
// reading - on the Switch the GPU writes them too) the GPU time replaces the provisional host value, in the host
// clock's terms (offset calibrated at each resolve: a timestamp can't be later than the moment its submission is seen
// done). EOWRECOMP_VK_COUNTER=cpu: host clock only; =frozen: one value per presented frame (test); =legacy: the host
// clock in ns instead of GPU ticks, as before (test: reproduces the locked 30).
#define CQ_MAX 64
typedef struct { VkQueryPool pool; uint32_t n; uint64_t seq; uint64_t *out[CQ_MAX]; } CounterQ;
static CounterQ cq[SUBS];
static pthread_mutex_t cq_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t cq_offset;  // host ns - GPU ns (the smallest seen)
// Counter timestamps are in the Switch GPU's ticks (614.4 MHz): the game converts the difference to ns itself
// (x 625 / 384). Writing ns made every GPU time look 1.63x as long.
static uint64_t ns_to_gpu_ticks(uint64_t ns) { return (uint64_t)((unsigned __int128)ns * 384 / 625); }
static bool cq_calibrated;
static int counter_mode = -1;  // 0 cpu, 1 frozen, 2 gpu
static uint64_t stat_counters_gpu;

static int vk_counter_mode(void) {
    if (counter_mode < 0) {
        const char *e = getenv("EOWRECOMP_VK_COUNTER");
        counter_mode = e && !strcmp(e, "cpu") ? 0 : e && !strcmp(e, "frozen") ? 1 : e && !strcmp(e, "legacy") ? 3 : 2;
        if (counter_mode == 2 && !g_vk.props.limits.timestampComputeAndGraphics) counter_mode = 0;
    }
    return counter_mode;
}

// ---- frame pacing: the game's own 30 fps mode when we can't hold 60
// The game advances its world one fixed step per frame in its 60 fps mode, and two steps per frame in its 30 fps mode
// (measured: Link walks twice as far per frame there) - so a 60-mode frame that takes 20 ms plays in slow motion, while
// the 30 mode plays at full speed. The game picks its mode from these counters (GPU time per frame), as on the Switch;
// but our frames can be slow for reasons its GPU time doesn't show (command translation, a slower CPU). So each
// presented frame's busy time is measured (present to present, minus the time the presenting thread was blocked in
// kernel waits and sleeps: idle - the game waits for vsync through its own threads); when
// frames don't fit 60 (12 of the last 60 over 16.2 ms), the counters report a frame of at least PACE_SLOW_NS: the game
// takes its 30 fps mode, at full speed. Back to the real times after 2 s with headroom (busy under 13 ms on average,
// at most 2 of the last 60 frames over). The game itself also tries 60 again now and then.
// EOWRECOMP_PACE=0: off.
#define PACE_SLOW_NS 20000000ull
extern __thread uint64_t g_blocked_ns;  // kernel.c: this thread's time blocked in waits / sleeps
static bool pace_slow;
static uint64_t stat_pace_over;

void vk_pace_frame(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("EOWRECOMP_PACE"); on = !e || atoi(e) != 0; }
    static uint64_t last, last_wait;
    static uint8_t over[60];
    static int idx, nover, calm;
    static double avg;  // busy ms, moving average (~20 frames)
    uint64_t now = plat_now_ns(), w = g_blocked_ns;
    uint64_t p = now - last, wt = w - last_wait;
    bool first = !last;
    last = now, last_wait = w;
    if (first || p > 100000000ull) return;  // (loading, a hitch: not a pace)
    double busy = (p > wt ? p - wt : 0) / 1e6;
    avg = avg ? avg + (busy - avg) * 0.05 : busy;
    static int log = -1;  // EOWRECOMP_LOG_PACE=1: each frame's period, vsync wait and busy time
    if (log < 0) log = getenv("EOWRECOMP_LOG_PACE") != NULL;
    if (log) LOG("[pace] period %.2f blocked %.2f busy %.2f ms\n", p / 1e6, wt / 1e6, busy);
    int o = busy > 16.2;
    stat_pace_over += o;
    nover += o - over[idx];
    over[idx] = (uint8_t)o;
    idx = (idx + 1) % 60;
    if (!on) return;  // (measured and counted all the same: EOWRECOMP_LOG_PACE, the stats)
    if (!pace_slow && nover >= 12) {
        pace_slow = true, calm = 0;
        LOG("[pace] frames don't fit 60 fps (%d of the last 60 over 16.2 ms busy): the game's 30 fps mode\n", nover);
    } else if (pace_slow) {
        // headroom: busy well under a 60 fps frame on average, hardly a frame over (single spikes are normal play)
        calm = avg < 13.0 && nover <= 2 ? calm + 1 : 0;
        if (calm >= 120) {
            pace_slow = false, nover = 0;
            memset(over, 0, sizeof over);
            LOG("[pace] 2 s with headroom: back to the game's own choice\n");
        }
    }
}

// a pair's end counter (the 16 bytes after its start) is at least PACE_SLOW_NS after it while pace_slow
static uint64_t pace_end_counter(const uint64_t *start_entry, const uint64_t *entry, uint64_t value) {
    if (!pace_slow || !start_entry || (const uint8_t *)entry - (const uint8_t *)start_entry != 16) return value;
    uint64_t min = start_entry[1] + ns_to_gpu_ticks(PACE_SLOW_NS);
    return value < min ? min : value;
}

static void cq_resolve(int i) {  // (cq_lock held) submission slot i is done: its counters get their GPU times
    CounterQ *q = &cq[i];
    if (!q->n) return;
    uint64_t t[CQ_MAX];
    if (vkGetQueryPoolResults(g_vk.dev, q->pool, 0, q->n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        double per = g_vk.props.limits.timestampPeriod;
        int64_t cand = (int64_t)plat_now_ns() - (int64_t)((double)t[q->n - 1] * per);
        if (!cq_calibrated || cand < cq_offset) cq_offset = cand, cq_calibrated = true;
        static int log = -1;  // EOWRECOMP_LOG_COUNTER=1: the host (provisional) and GPU times of each counter
        if (log < 0) log = getenv("EOWRECOMP_LOG_COUNTER") != NULL;
        for (uint32_t k = 0; k < q->n; k++) {
            uint64_t gpu = ns_to_gpu_ticks((uint64_t)((int64_t)((double)t[k] * per) + cq_offset));
            if (log) LOG("[counter] seq %llu #%u at %llx host %.3f gpu %.3f ms\n", (unsigned long long)q->seq, k,
                         (unsigned long long)(uintptr_t)q->out[k], q->out[k][1] * 625.0 / 384 / 1e6, gpu * 625.0 / 384 / 1e6);
            q->out[k][1] = pace_end_counter(k ? q->out[k - 1] : NULL, q->out[k], gpu);
        }
        stat_counters_gpu += q->n;
    }
    q->n = 0;
}

static void cq_resolve_done(uint64_t done) {  // every slot whose submission is done
    pthread_mutex_lock(&cq_lock);
    for (int i = 0; i < SUBS; i++) if (cq[i].n && cq[i].seq <= done) cq_resolve(i);
    pthread_mutex_unlock(&cq_lock);
}

static Sub *sub(void) {
    Sub *s = &subs[cur_sub];
    if (s->seq != submit_seq + 1) {  // slot reused: its previous submission must be done
        wait_why = "ring slot";
        vk_wait_seq(s->seq);
        cq_resolve_done(s->seq);
        if (s->ts) {  // (profiling) GPU time of that submission
            uint64_t t[2];
            if (vkGetQueryPoolResults(g_vk.dev, prof_qp, (uint32_t)cur_sub * 2, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0])
                prof_gpu += (uint64_t)((double)(t[1] - t[0]) * g_vk.props.limits.timestampPeriod);
            s->ts = false;
        }
        for (int i = 0; i < s->npools; i++) vkResetDescriptorPool(g_vk.dev, s->pools[i], 0);
        s->cur_pool = 0;
        s->seq = submit_seq + 1;
        s->main_begun = s->upload_begun = false;
    }
    return s;
}

static void begin_cb(VkCommandBuffer *cb, bool *begun) {
    if (*begun) return;
    if (!*cb) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_vk.pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, cb));
    }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(*cb, &bi));
    *begun = true;
}

static void gp_mark(VkCommandBuffer cb, const char *fmt, ...);
VkCommandBuffer vk_cmd(void) {
    Sub *s = sub();
    if (!s->main_begun) {
        begin_cb(&s->main, &s->main_begun);
        ss_forget();  // (a new command buffer starts with no state)
        if (vk_counter_mode() == 2) {  // this slot's timing counter queries (reset outside any render pass)
            CounterQ *q = &cq[cur_sub];
            if (!q->pool) {
                VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                            .queryCount = CQ_MAX};
                if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &q->pool) != VK_SUCCESS) q->pool = VK_NULL_HANDLE;
            }
            pthread_mutex_lock(&cq_lock);
            q->n = 0;  // (resolved when the slot was freed: sub())
            q->seq = s->seq;
            pthread_mutex_unlock(&cq_lock);
            if (q->pool) vkCmdResetQueryPool(s->main, q->pool, 0, CQ_MAX);
        }
        gp_mark(s->main, "(submission start; includes GPU idle before it)");
        if (prof && prof_qp) {
            vkCmdResetQueryPool(s->main, prof_qp, (uint32_t)cur_sub * 2, 2);
            vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_NONE, prof_qp, (uint32_t)cur_sub * 2);
            s->ts = true;
        }
    }
    return s->main;
}

VkCommandBuffer vk_upload_cmd(void) {
    Sub *s = sub();
    if (!s->upload_begun) {
        begin_cb(&s->upload, &s->upload_begun);
        // after everything submitted before it: a submission's commands may start while the previous one's still run,
        // and these copies overwrite what those may read (textures being replaced, vertex cache pages)
        vk_full_barrier(s->upload);
    }
    return s->upload;
}

void vk_full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// A depth/stencil transfer write must be visible before a later shader samples that subresource. Keep this
// dependency resource-scoped and limited to shader reads; the source-side barrier cannot order the copy's later write.
static void image_transfer_write_barrier(VkCommandBuffer cb, VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mip,
                                         uint32_t layer) {
    if (!(aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) return;
    // With separateDepthStencilLayouts disabled, Vulkan requires both aspects of a combined depth/stencil image
    // in the barrier range, even when the copy itself only touched depth. The device in the user's validation log
    // reports D24S8 and that feature disabled; a depth-only range makes this dependency invalid.
    if (format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
        format == VK_FORMAT_D32_SFLOAT_S8_UINT)
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .image = image,
                               .subresourceRange = {aspect, mip, 1, layer, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

static void ring_retire(void);
void vc_flush(void);

// ---- EOWRECOMP_GPUPROF_AT=<frame>: GPU time per pass / dispatch / copy of that frame, logged as "[gpuprof]".
// A timestamp (ALL_COMMANDS: after everything before it) at each mark; a row = the work since the previous mark.
bool g_gpuprof_frame;
static VkQueryPool gp_qp;
static int gp_n;
static char gp_label[512][112];
static uint32_t gp_draws[512];
// EOWRECOMP_VK_DEVLOST_TRACE=1: the last 64 GPU operations, dumped on device loss (with EOWRECOMP_SYNC_SUBMIT=1 the
// dump ends with the command list that faulted).
static char dl_ring[64][128];
static unsigned dl_n;
static int dl_on = -1;
static void dl_note(const char *fmt, ...) {
    if (dl_on < 0) dl_on = getenv("EOWRECOMP_VK_DEVLOST_TRACE") ? atoi(getenv("EOWRECOMP_VK_DEVLOST_TRACE")) : 0;
    if (!dl_on) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dl_ring[dl_n++ & 63], sizeof dl_ring[0], fmt, ap);
    va_end(ap);
}
void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv) { dl_note("  upload %p %ux%u lv%u", img, w, h, lv); }
void vk_devlost_dump(void) {
    if (!dl_on) return;
    LOG("[vk] last GPU operations before the device loss (oldest first):\n");
    for (unsigned i = dl_n > 64 ? dl_n - 64 : 0; i < dl_n; i++) LOG("[vk]   %s\n", dl_ring[i & 63]);
}
static void gp_mark(VkCommandBuffer cb, const char *fmt, ...) {
    if (!g_gpuprof_frame || gp_n >= 511) return;
    if (!gp_qp) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 512};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &gp_qp) != VK_SUCCESS) { g_gpuprof_frame = false; return; }
    }
    if (!gp_n) vkCmdResetQueryPool(cb, gp_qp, 0, 512);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gp_qp, (uint32_t)gp_n);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gp_label[gp_n], sizeof gp_label[0], fmt, ap);
    va_end(ap);
    gp_draws[gp_n] = 0;
    gp_n++;
}
void renderer_flush(void);
void vk_gpuprof_report(void) {
    if (!gp_n) return;
    renderer_flush();
    uint64_t t[512];
    if (vkGetQueryPoolResults(g_vk.dev, gp_qp, 0, (uint32_t)gp_n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
        double per = g_vk.props.limits.timestampPeriod / 1e6, total = (t[gp_n - 1] - t[0]) * per;
        LOG("[gpuprof] %d marks, %.2f ms first to last (rows: the work after each mark, until the next)\n", gp_n, total);
        for (int i = 0; i + 1 < gp_n; i++) {
            double ms = (t[i + 1] - t[i]) * per;
            LOG("[gpuprof] %6.3f ms %5.1f%%  %s%s", ms, 100 * ms / (total > 0 ? total : 1), gp_label[i], gp_draws[i] ? "" : "\n");
            if (gp_draws[i]) LOG(", %u draws\n", gp_draws[i]);
        }
    }
    gp_n = 0;
}

// Submit the pending work (if any). wait/signal: binary semaphores for presentation (may be null).
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra) {
    (void)extra;
    vk_end_pass();
    Sub *s = sub();
    if (!s->main_begun && !s->upload_begun && !wait && !signal) return;
    VkCommandBufferSubmitInfo cbs[2];
    int n = 0;
    vc_flush();
    if (s->upload_begun) {
        vk_full_barrier(s->upload);  // uploads complete before anything in the main buffer reads them
        VK_CHECK(vkEndCommandBuffer(s->upload));
        cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->upload};
    }
    vk_cmd();  // (an empty main buffer still carries the semaphores)
    if (s->ts) vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_qp, (uint32_t)cur_sub * 2 + 1);
    VK_CHECK(vkEndCommandBuffer(s->main));
    cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->main};
    VkSemaphoreSubmitInfo sig[2] = {{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_vk.timeline,
                                     .value = submit_seq + 1, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                    {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = signal,
                                     .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    VkSemaphoreSubmitInfo wt = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wait,
                                .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = (uint32_t)n, .pCommandBufferInfos = cbs,
                        .signalSemaphoreInfoCount = signal ? 2 : 1, .pSignalSemaphoreInfos = sig,
                        .waitSemaphoreInfoCount = wait ? 1 : 0, .pWaitSemaphoreInfos = &wt};
    {
        PROF_T0();
        dl_note("submit %llu", (unsigned long long)submit_seq + 1);
        VkResult sr = vkQueueSubmit2(g_vk.queue, 1, &si, VK_NULL_HANDLE);
        if (sr == VK_ERROR_DEVICE_LOST) vk_devlost_dump();
        VK_CHECK(sr);
        PROF_ADD(prof_submit);
    }
    pthread_mutex_lock(&seq_lock);
    submit_seq++;
    pthread_mutex_unlock(&seq_lock);
    cur_sub = (cur_sub + 1) % SUBS;
    stat_flushes++;
    cum_submits++;
    ring_retire();
    vk_collect_dead();
}

void renderer_submit(void) { vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE); }

void renderer_flush(void) {
    renderer_submit();
    if (!wait_why) wait_why = "flush";
    vk_wait_seq(submit_seq);
}

void renderer_sync(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    wait_why = "QueueFinish / sync all";
    vk_wait_seq(s);
}

uint64_t renderer_fence_seq(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t q = submit_seq + (subs[cur_sub].main_begun || subs[cur_sub].upload_begun ? 1 : 0);
    pthread_mutex_unlock(&seq_lock);
    return q;
}

void renderer_wait_seq(uint64_t seq) {
    if (!seq) return;
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    wait_why = seq > s ? "SyncWait (fence not submitted yet)" : "SyncWait";
    vk_wait_seq(seq > s ? s : seq);  // not submitted yet (other thread): everything submitted so far
}

VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout, uint32_t variable_count) {
    Sub *s = sub();
    for (;;) {
        if (s->cur_pool == s->npools) {
            if (s->npools == 16) FATAL("descriptor pools exhausted\n");
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768},
                                            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1024},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 32768}};
            VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8192,
                                             .poolSizeCount = 6, .pPoolSizes = sizes};
            VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &s->pools[s->npools++]));
        }
        VkDescriptorSetVariableDescriptorCountAllocateInfo vc = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
            .descriptorSetCount = 1, .pDescriptorCounts = &variable_count};
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = s->pools[s->cur_pool], .descriptorSetCount = 1, .pSetLayouts = &layout};
        if (variable_count) ai.pNext = &vc;
        VkDescriptorSet set;
        VkResult r = vkAllocateDescriptorSets(g_vk.dev, &ai, &set);
        if (r == VK_SUCCESS) return set;
        s->cur_pool++;
    }
}

// ================================================================== transient memory ring
// 64 MB host-visible chunks (device-local when the GPU exposes resizable BAR); a chunk is reused once every
// submission that allocated from it has completed.
#define CHUNK_SIZE (64ull << 20)
typedef struct { VkBuffer buf; VmaAllocation mem; uint8_t *ptr; uint64_t size, used, seq; } Chunk;
static Chunk *chunks;
static int nchunks, active = -1;

static int new_chunk(uint64_t size) {
    chunks = realloc(chunks, (nchunks + 1) * sizeof *chunks);
    Chunk *c = &chunks[nchunks];
    memset(c, 0, sizeof *c);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    if (getenv("EOWRECOMP_VK_RING_SYSMEM")) ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;  // (test: a GPU without resizable BAR)
    VmaAllocationInfo info;
    VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &c->buf, &c->mem, &info));
    c->ptr = info.pMappedData;
    c->size = size;
    if (!nchunks) {  // where the ring lives: device-local (resizable BAR) or system memory read across PCIe
        VkMemoryPropertyFlags f;
        vmaGetMemoryTypeProperties(g_vk.vma, info.memoryType, &f);
        LOG("[vk] transient ring: memory type %u (%s%s%s)\n", info.memoryType, f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "device-local " : "system memory ",
            f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? "host-visible " : "", f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "cached" : "");
    }
    return nchunks++;
}

static void ring_retire(void) {}  // chunks are recycled lazily in vk_transient

VkSpan vk_transient(uint64_t size, uint64_t align) {
    if (align < 16) align = 16;
    uint64_t pending = vk_pending_seq();
    if (active >= 0) {
        Chunk *c = &chunks[active];
        uint64_t off = (c->used + align - 1) & ~(align - 1);
        if (off + size <= c->size) {
            c->used = off + size;
            c->seq = pending;
            stat_upload_bytes += size;
            return (VkSpan){c->buf, off, c->ptr + off};
        }
    }
    uint64_t done = vk_completed_seq();
    int pick = -1;
    for (int i = 0; i < nchunks; i++)
        if (i != active && chunks[i].seq <= done && chunks[i].size >= size) { pick = i; break; }
    if (pick < 0) pick = new_chunk(size > CHUNK_SIZE ? size : CHUNK_SIZE);
    active = pick;
    chunks[pick].used = size;
    chunks[pick].seq = pending;
    stat_upload_bytes += size;
    return (VkSpan){chunks[pick].buf, 0, chunks[pick].ptr};
}

// GPU -> CPU copies: the ring is write-combined (often device-local over the BAR), where CPU reads crawl (a
// 1280x720 screenshot took 7 s). Readbacks use one host-cached buffer instead. Every user waits for the GPU and
// reads the data before the next readback (render thread), so a single grow-only buffer suffices.
VkSpan vk_readback(uint64_t size) {
    static VkBuffer buf;
    static VmaAllocation mem;
    static uint8_t *ptr;
    static uint64_t cap;
    if (size > cap) {
        if (buf) { vk_wait_seq(submit_seq); vmaDestroyBuffer(g_vk.vma, buf, mem); }
        cap = (size + (16 << 20) - 1) & ~((16ull << 20) - 1);
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &buf, &mem, &info));
        ptr = info.pMappedData;
    }
    return (VkSpan){buf, 0, ptr};
}

VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    VkSpan s = vk_transient(size ? size : 16, align);
    if (size) memcpy(s.ptr, data, size);
    return s;
}

// Per-draw constants that usually repeat the previous draw's (scale table, tint, driver constants): reuse that copy
// while it's identical, within one submission. slot: one per kind of data.
enum { DEDUPE_RSCALE_V, DEDUPE_RSCALE_F, DEDUPE_TINT, DEDUPE_DRV_V, DEDUPE_DRV_F, DEDUPE_DRV_C, DEDUPE_SLOTS };
static VkSpan copy_dedupe(int slot, const void *data, uint64_t size, uint64_t align) {
    static struct { uint64_t seq, size; uint8_t data[0x800]; VkSpan span; } last[DEDUPE_SLOTS];
    uint64_t seq = vk_pending_seq();
    if (size > sizeof last[0].data) return vk_transient_copy(data, size, align);
    if (last[slot].seq == seq && last[slot].size == size && !memcmp(last[slot].data, data, size)) return last[slot].span;
    VkSpan s = vk_transient_copy(data, size, align);
    last[slot].seq = seq; last[slot].size = size; memcpy(last[slot].data, data, size); last[slot].span = s;
    return s;
}

// Guest memory copied once per submission: draws binding the same range share the copy.
#define GCOPY_TAB 4096
static struct { uint64_t addr, size, seq; VkSpan span; } gcopy[GCOPY_TAB];
static uint64_t gcopy_gen;  // bumped to drop every shared copy (guest memory written mid-list)
static void guest_copy_flush(void) { gcopy_gen++; }
static VkSpan guest_copy(uint64_t addr, uint64_t size) {
    uint64_t seq = vk_pending_seq() + (gcopy_gen << 40), h = (addr * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (GCOPY_TAB - 1);
    if (gcopy[slot].seq == seq && gcopy[slot].addr == addr && gcopy[slot].size == size) return gcopy[slot].span;
    VkSpan s = vk_transient_copy((const void *)(uintptr_t)addr, size, 256);
    gcopy[slot].addr = addr; gcopy[slot].size = size; gcopy[slot].seq = seq; gcopy[slot].span = s;
    return s;
}

// ================================================================== worker threads (shader / pipeline builds)
// First-use compiles run off the render thread; draws needing them are skipped until ready (as on Metal).
// EOWRECOMP_SYNC_SHADERS=1 builds on first use, blocking (deterministic tests).
typedef struct Job { void (*fn)(void *); void *arg; struct Job *next; } Job;
static _Atomic uint32_t pending_jobs;
static pthread_mutex_t pending_jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pending_jobs_cond = PTHREAD_COND_INITIALIZER;
// Each queue has a second, low-priority list: the startup shader warmup (vk_warm_start) queues there, so builds the
// game needs right now go first; a draw waiting for a warm build moves it to the front (jobs_bump).
// And a third, idle list: the optimized (complete) replacements of fast-linked pipelines. They only make a working
// pipeline faster, so they never delay a build something is waiting for: the startup warmup's pipelines waited behind
// 1133 of them in a cold run (137 s instead of seconds).
typedef struct { Job *head, *tail, *low_head, *low_tail, *idle_head, *idle_tail; pthread_mutex_t lock; pthread_cond_t cond; } JobQueue;
static JobQueue jobs = {.lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};

static void queue_push(JobQueue *q, Job *j, int tier) {  // 0 now, 1 low (warmup), 2 idle (optimization)
    atomic_fetch_add(&pending_jobs, 1);
    pthread_mutex_lock(&q->lock);
    Job **head = tier == 2 ? &q->idle_head : tier ? &q->low_head : &q->head;
    Job **tail = tier == 2 ? &q->idle_tail : tier ? &q->low_tail : &q->tail;
    if (*tail) (*tail)->next = j; else *head = j;
    *tail = j;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

static Job *queue_pop(JobQueue *q) {
    pthread_mutex_lock(&q->lock);
    while (!q->head && !q->low_head && !q->idle_head) pthread_cond_wait(&q->cond, &q->lock);
    Job **head = q->head ? &q->head : q->low_head ? &q->low_head : &q->idle_head;
    Job **tail = q->head ? &q->tail : q->low_head ? &q->low_tail : &q->idle_tail;
    Job *j = *head;
    *head = j->next;
    if (!*head) *tail = NULL;
    pthread_mutex_unlock(&q->lock);
    return j;
}

static void queue_job_finished(void) {
    if (atomic_fetch_sub(&pending_jobs, 1) == 1) {
        pthread_mutex_lock(&pending_jobs_lock);
        pthread_cond_broadcast(&pending_jobs_cond);
        pthread_mutex_unlock(&pending_jobs_lock);
    }
}

// backend_reset (tables cleared) and Trirod-ShaderPrep (all pipelines created before the cache is saved). Saving the
// pipeline cache itself needs no wait: a VkPipelineCache created without EXTERNALLY_SYNCHRONIZED is thread-safe.
void vk_exec_wait_jobs(void) {
    pthread_mutex_lock(&pending_jobs_lock);
    while (atomic_load(&pending_jobs)) pthread_cond_wait(&pending_jobs_cond, &pending_jobs_lock);
    pthread_mutex_unlock(&pending_jobs_lock);
}
uint32_t vk_exec_jobs_pending(void) { return atomic_load(&pending_jobs); }

// Drops the queued (not yet started) idle jobs that run `fn`, each passed to `drop` (backend_reset: the
// optimized-pipeline replacements; the fast-linked pipelines they would replace already work).
static void queue_drop_idle(JobQueue *q, void (*fn)(void *), void (*drop)(void *)) {
    pthread_mutex_lock(&q->lock);
    Job *prev = NULL, *j = q->idle_head;
    while (j) {
        Job *next = j->next;
        if (j->fn == fn) {
            if (prev) prev->next = next; else q->idle_head = next;
            if (q->idle_tail == j) q->idle_tail = prev;
            if (drop) drop(j->arg);
            free(j);
            queue_job_finished();
        } else prev = j;
        j = next;
    }
    pthread_mutex_unlock(&q->lock);
}

static void *worker(void *a) {
    (void)a;
    for (;;) {
        Job *j = queue_pop(&jobs);
        j->fn(j->arg);
        free(j);
        queue_job_finished();
    }
    return NULL;
}

static bool sync_shaders(void) {
    static int v = -1;
    if (v < 0) v = getenv("EOWRECOMP_SYNC_SHADERS") != NULL;
    return v;
}

// Shader translation: one dedicated thread (the translator isn't re-entrant - translate.c's RSC2 string ring - and
// translating on the game thread cost up to ~90 ms per frame on entering an area). Compiles go on to the workers.
static pthread_mutex_t translate_lock = PTHREAD_MUTEX_INITIALIZER;  // (compute shaders translate on the game thread)
static JobQueue tjobs = {.lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};
static void *translator(void *a) {
    (void)a;
    for (;;) {
        Job *j = queue_pop(&tjobs);
        j->fn(j->arg);
        free(j);
        queue_job_finished();
    }
    return NULL;
}
static bool sync_shaders(void);

// Moves the queued job for `arg` (a Prog: translation, then its compile; a Pipe: its pipeline) to the front of the
// queue's normal list, from wherever it is (also the warmup's low list): draws that wait for their build
// (compile_wait) must not sit behind an area's worth of other shaders.
static void jobs_bump(JobQueue *q, void *arg) {
    pthread_mutex_lock(&q->lock);
    for (int low = 0; low < 2; low++) {
        Job **head = low ? &q->low_head : &q->head, **tail = low ? &q->low_tail : &q->tail;
        Job *prev = NULL, *j = *head;
        while (j && j->arg != arg) { prev = j; j = j->next; }
        if (!j) continue;
        if (!low && !prev) break;  // (already first)
        if (prev) prev->next = j->next; else *head = j->next;
        if (*tail == j) *tail = prev;
        j->next = q->head;
        q->head = j;
        if (!q->tail) q->tail = j;
        break;
    }
    pthread_mutex_unlock(&q->lock);
}

static void start_translator(void) { pthread_t t; pthread_create(&t, NULL, translator, NULL); }
static void run_translate_q(void (*fn)(void *), void *arg, bool low) {
    if (sync_shaders()) { fn(arg); return; }
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, start_translator);
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg; j->next = NULL;
    queue_push(&tjobs, j, low);
}
static void run_translate(void (*fn)(void *), void *arg) { run_translate_q(fn, arg, false); }

static void start_workers(void) {
    // first entry into an area builds dozens of pipelines at once: use most of the CPU (draws wait for them)
    int n = SDL_GetNumLogicalCPUCores() - 4;
    if (n < 4) n = 4;
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) { pthread_t t; pthread_create(&t, NULL, worker, NULL); }
}
static void run_job_q(void (*fn)(void *), void *arg, int tier) {
    if (sync_shaders()) { fn(arg); return; }
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, start_workers);
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg; j->next = NULL;
    queue_push(&jobs, j, tier);
}
static void run_job(void (*fn)(void *), void *arg) { run_job_q(fn, arg, 0); }

// ================================================================== state
typedef struct {
    NvnTexture *color[8];
    NvnTextureView cview[8];
    uint8_t has_cview[8];
    int ncolor;
    NvnTexture *depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtState;

typedef struct { uint64_t addr, size; } Range;

static struct {
    RtState rt;
    bool rt_dirty;
    NvnProgram *program;
    uint8_t blend[8][8];
    uint32_t channel_mask;
    uint8_t color_state[4], ds[8], ms[0x18], poly[4];
    float poly_offset[3];
    uint32_t attrib[16];
    int nattrib;
    uint32_t stream[16][2];
    int nstream;
    Range vbuf[16], ubuf[6][18], sbuf[6][16];
    uint64_t tex[6][32], img[6][8];
    NvnDescPool *tpool, *spool;
    float viewport[16][4];
    int32_t scissor[16][4];
    float depth_range[16][2];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    bool prim_restart;
    uint32_t restart_index;
    uint64_t copy_row_stride, copy_image_stride;
    float alpha_ref;  // nvnCommandBufferSetAlphaRef (logged by draw_diag; the alpha test itself is not implemented)
} st;

uint32_t nvn_window_origin(void);
uint32_t nvn_depth_mode(void);
bool renderer_origin_lower_left(void) { return nvn_window_origin() == 0; }

// (EOWRECOMP_VK_PROF) called once per presented frame: frames over 50 ms are logged with where their time went.
// present_ns: time in backend_present's acquire/blit/present.
void vk_prof_frame(uint64_t frame, uint64_t present_ns) {
    if (!prof) return;
    static uint64_t last_t, l_exec, l_wait, l_draws, l_subs;
    uint64_t now = SDL_GetTicksNS();
    if (last_t && now - last_t > 50000000) {
        uint64_t dt = now - last_t, ex = cum_exec - l_exec, wt = cum_wait - l_wait;
        LOG("[hitch] frame %llu: %.0f ms = backend exec %.1f (of which GPU waits %.1f) + present %.1f + other %.1f; %llu draws, "
            "%llu submits, compiles %d pending\n", (unsigned long long)frame, dt / 1e6, ex / 1e6, wt / 1e6, present_ns / 1e6,
            ((double)dt - ex - present_ns) / 1e6, (unsigned long long)(cum_draws - l_draws), (unsigned long long)(cum_submits - l_subs),
            atomic_load(&stat_compiles));
    }
    last_t = now; l_exec = cum_exec; l_wait = cum_wait; l_draws = cum_draws; l_subs = cum_submits;
}

const char *renderer_stats(void) {
    static int dumps;
    if (dumps++ < 3) {
        LOG_VERBOSE("[vk] command counts:");
        for (int i = 0; i < CMD_COUNT; i++) if (op_counts[i]) LOG_VERBOSE(" %d:%llu", i, (unsigned long long)op_counts[i]);
        LOG_VERBOSE("\n");
    }
    snprintf(stats_text, sizeof stats_text,
             "%llu draws, %llu skipped, %llu passes, %llu dispatches, %llu flushes, wait %.0fms, compile %d, fbcopy %llu, upload %.1fMB",
             (unsigned long long)stat_draws, (unsigned long long)stat_skipped, (unsigned long long)stat_passes,
             (unsigned long long)stat_dispatches, (unsigned long long)stat_flushes, (double)stat_wait_ns / 1e6,
             atomic_exchange(&stat_compiles, 0), (unsigned long long)stat_feedback_copies, (double)stat_upload_bytes / 1048576.0);
    stat_feedback_copies = stat_upload_bytes = 0;
    if (stat_skipped) {
        size_t n = strlen(stats_text);
        n += snprintf(stats_text + n, sizeof stats_text - n, " | skipped:");
        for (int i = 0; i < SKIP_NREASONS && n < sizeof stats_text; i++)
            if (stat_skip_why[i]) n += snprintf(stats_text + n, sizeof stats_text - n, " %s %llu", skip_names[i], (unsigned long long)stat_skip_why[i]);
    }
    memset(stat_skip_why, 0, sizeof stat_skip_why);
    {   // the session's pipeline totals every 30 s (verbose; device.c logs them at exit too)
        static uint64_t last_ns;
        uint64_t now = SDL_GetTicksNS();
        if (rt_verbose() && now - last_ns > 30000000000ull) {
            char sum[512];
            vk_pipeline_session_summary(sum, sizeof sum);
            LOG("[vk] session %s\n", sum);
            last_ns = now;
        }
    }
    if (atomic_load(&pending_jobs)) {  // (translations, compiles and pipelines not finished yet, optimized ones included)
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | shader jobs queued %u", atomic_load(&pending_jobs));
    }
    {   // the startup warmup and the pipelines first met this interval
        extern void warm_stats(char *out, size_t n);
        size_t n = strlen(stats_text);
        warm_stats(stats_text + n, sizeof stats_text - n);
    }
    if (stat_indirect) {  // (draw_indirect)
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | indirect draws %llu (%llu empty, %llu waits)", (unsigned long long)stat_indirect,
                 (unsigned long long)stat_indirect_empty, (unsigned long long)stat_indirect_waits);
        stat_indirect = stat_indirect_empty = stat_indirect_waits = 0;
    }
    if (stat_gstore_waits) {  // (gstore_sync_cbufs: waits for GPU-stored constant buffers)
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | gpu-stored cbuf waits %llu", (unsigned long long)stat_gstore_waits);
        stat_gstore_waits = 0;
    }
    {
        extern void vk_alias_stats(void);
        vk_alias_stats();
    }
    if (prof) {
        prof_up = prof_up_ns;
        prof_up_ns = 0;
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | gpu %.0f cpu exec %.0f draw %.0f (pre %.0f key %.0f dyn %.0f sets %.0f tex %.0f (upload %.0f) vtx %.0f) submit %.0f ms",
                 prof_gpu / 1e6, prof_exec / 1e6, prof_draw / 1e6, prof_pre / 1e6, prof_key / 1e6, prof_dyn / 1e6, prof_sets / 1e6,
                 prof_tex / 1e6, prof_up / 1e6, prof_vtx / 1e6, prof_submit / 1e6);
        extern uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn;
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | tex checks %llu samples %llu full %llu (%.1f MB, dynamic %llu)",
                 (unsigned long long)prof_tex_checks, (unsigned long long)prof_tex_samples, (unsigned long long)prof_tex_full,
                 prof_tex_full_bytes / 1048576.0, (unsigned long long)prof_tex_dyn);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | pass ends:");
        for (int l = 0; l < 4096; l++)
            if (prof_end_line[l] > 20) { n = strlen(stats_text); snprintf(stats_text + n, sizeof stats_text - n, " %d:%u", l, prof_end_line[l]); }
        memset(prof_end_line, 0, sizeof prof_end_line);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | views created %llu | counters gpu %llu | pace %s, %llu frames over", (unsigned long long)prof_views_created,
                 (unsigned long long)stat_counters_gpu, pace_slow ? "30" : "60", (unsigned long long)stat_pace_over);
        stat_counters_gpu = stat_pace_over = 0;
        {
            extern uint64_t prof_vc_hits, prof_vc_bypass, prof_vc_copied, prof_vc_queries, prof_vc_scan_pages, prof_vc_scan_ns;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n,  " | vcache %llu hit %llu in place %.1f MB copied, %llu queries %.2f ms, %llu pages written",
                     (unsigned long long)prof_vc_hits, (unsigned long long)prof_vc_bypass, prof_vc_copied / 1048576.0,
                     (unsigned long long)prof_vc_queries, prof_vc_scan_ns / 1e6, (unsigned long long)prof_vc_scan_pages);
            prof_vc_hits = prof_vc_bypass = prof_vc_copied = prof_vc_queries = prof_vc_scan_pages = prof_vc_scan_ns = 0;
            extern uint64_t prof_tex_watch_skips;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | tex unwritten %llu", (unsigned long long)prof_tex_watch_skips);
            prof_tex_watch_skips = 0;
        }
        pthread_mutex_lock(&wait_tab_lock);
        for (int i = 0; i < 12 && wait_tab[i].why; i++) {
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, "%s%s %llux %.0f ms", i ? "," : " | waits:", wait_tab[i].why,
                     (unsigned long long)wait_tab[i].n, wait_tab[i].ns / 1e6);
        }
        memset(wait_tab, 0, sizeof wait_tab);
        pthread_mutex_unlock(&wait_tab_lock);
        prof_views_created = 0;
        prof_tex_checks = prof_tex_samples = prof_tex_full = prof_tex_full_bytes = prof_tex_dyn = 0;
        prof_exec = prof_draw = prof_sets = prof_vtx = prof_submit = prof_pre = prof_key = prof_dyn = prof_tex = prof_up = prof_gpu = 0;
    }
    stat_draws = stat_skipped = stat_passes = stat_dispatches = stat_flushes = stat_wait_ns = 0;
    return stats_text;
}

static void st_defaults(void) {
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 16; i++) {
        st.viewport[i][2] = 1280; st.viewport[i][3] = 720;
        st.scissor[i][2] = 16384; st.scissor[i][3] = 16384;
        st.depth_range[i][1] = 1.0f;
    }
    st.channel_mask = 0xFFFFFFFF;
    st.stencil_mask[0] = st.stencil_mask[1] = 0xFF;
    st.stencil_vmask[0] = st.stencil_vmask[1] = 0xFF;
    st.ds[1] = 1; st.ds[2] = 2;
    st.poly[1] = 1; st.poly[2] = 2;
    for (int i = 0; i < 8; i++) { st.blend[i][1] = 2; st.blend[i][2] = 1; st.blend[i][3] = 2; st.blend[i][4] = 1; st.blend[i][5] = 1; st.blend[i][6] = 1; }
}

void vk_exec_init(void) {
    prof = getenv("EOWRECOMP_VK_PROF") != NULL;
    if (prof) {
        prof_up_ns = 0;
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = SUBS * 2};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &prof_qp) != VK_SUCCESS) prof_qp = VK_NULL_HANDLE;
    }
    st_defaults();
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy);
bool renderer_texture_presented(NvnTexture *t);
bool renderer_texture_first_write(NvnTexture *t);
bool renderer_texture_level_fresh(NvnTexture *t);
bool renderer_texture_frame_fresh(NvnTexture *t);
void renderer_texture_mark_presented(NvnTexture *t);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);
void renderer_save_texture_tex_alpha(TexBackend *b, const char *path);
static float pass_sx = 1, pass_sy = 1;
static float vp_sx = 1, vp_sy = 1, vp_ox = 0, vp_oy = 0;
static float tex_sc[2][64][2];
static uint32_t tex_wh[2][64][2];

// ================================================================== render passes
static VkFormat pass_color[8], pass_depth, pass_stencil;
static uint32_t pass_ncolor, pass_samples = 1, pass_w, pass_h;
static int debug_pass_no;
// Draw-to-draw attachment hazards within one dynamic-rendering instance. Vulkan orders
// attachment writes with each other, but a later draw's texture reads need a dependency.
static uint16_t pass_sampled_targets, pass_written_targets;
static uint32_t pass_draw_count;

// Redundant-state filter: what draw() last set in the current command buffer, so unchanged pipeline binds, dynamic
// state and vertex / index buffer binds are not recorded again (a draw used to issue ~20 vkCmd* calls; most repeat the
// previous draw's). Forgotten (ss_forget) whenever something else may have recorded state: a new command buffer, a new
// render pass (present, ImGui, post-processing and copies all run between game passes) and the clear pipeline.
// Descriptor sets are not filtered: every draw writes new ones (sharing identical sets within a submission was tried:
// a third matched, but comparing them cost more than allocating and writing them on NVIDIA).
// EOWRECOMP_VK_STATE_FILTER=0: every draw sets everything, as before.
enum { SS_PIPE = 1 << 0, SS_DT = 1 << 1, SS_DW = 1 << 2, SS_DOP = 1 << 3, SS_STEN = 1 << 4, SS_SFACE0 = 1 << 5, SS_SFACE1 = 1 << 6,
       SS_BC = 1 << 7, SS_CULL = 1 << 8, SS_FF = 1 << 9, SS_TOPO = 1 << 10, SS_PRE = 1 << 11, SS_DBE = 1 << 12, SS_DB = 1 << 13,
       SS_VP = 1 << 14, SS_SC = 1 << 15, SS_IB = 1 << 16 };
static struct {
    uint32_t known;  // SS_* bits: the value below is what the command buffer has
    uint32_t vb_known;  // per vertex binding
    VkPipeline pipe;
    VkBool32 dt, dw, sten, pre, dbe;
    VkCompareOp dop;
    uint32_t sface[2][7];  // fail, pass, depth fail, compare, compare mask, write mask, reference
    float bc[4], db[3];
    VkCullModeFlags cull;
    VkFrontFace ff;
    VkPrimitiveTopology topo;
    VkViewport vp;
    VkRect2D sc;
    VkBuffer ib; VkDeviceSize io; VkIndexType it;
    VkBuffer vb[17]; VkDeviceSize vo[17];
} ss;
static int ss_on = -1;
static void ss_forget(void) { ss.known = 0; ss.vb_known = 0; }
static bool ss_enabled(void) {
    if (ss_on < 0) { const char *e = getenv("EOWRECOMP_VK_STATE_FILTER"); ss_on = !e || atoi(e) != 0; }
    return ss_on;
}
// true: the state must be recorded (and is remembered as known)
static inline bool ss_need(uint32_t bit, bool same) {
    if (ss_on && (ss.known & bit) && same) return false;
    ss.known |= bit;
    return true;
}

static void vk_end_pass_at(int line);
void vk_end_pass(void) { vk_end_pass_at(0); }
#define vk_end_pass() vk_end_pass_at(__LINE__)
static void vk_end_pass_at(int line) {
    if (!in_pass) return;
    if (prof && line < 4096) prof_end_line[line]++;
    vkCmdEndRendering(vk_cmd());
    in_pass = false;
    // F9 pass dumps read every attachment back and call renderer_flush(), which can hide a
    // synchronization bug by waiting after each pass. This opt-in probe waits after only one
    // zero-based render-pass ordinal per presented frame, without taking a readback.
    static int wait_pass = -2;
    static uint64_t wait_pass_frame = ~0ull;
    static int pass_ordinal;
    if (wait_pass == -2) {
        const char *e = getenv("EOWRECOMP_VK_WAIT_PASS");
        wait_pass = e ? atoi(e) : -1;
        if (wait_pass >= 0) LOG("[vk] diagnostic: wait after render pass %d each frame (no readback)\n", wait_pass);
    }
    if (wait_pass >= 0 && st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->backend) {
        uint64_t frame = renderer_present_count();
        if (frame != wait_pass_frame) { wait_pass_frame = frame; pass_ordinal = 0; }
        int ended_pass = pass_ordinal++;
        if (ended_pass == wait_pass) {
            LOG("[vk] diagnostic: GPU wait after render pass %d at frame %llu\n", ended_pass, (unsigned long long)frame);
            renderer_flush();
        }
    }
    // F9's all-pass dump combines readback work with a GPU wait. This probe captures exactly
    // one color pass per frame so we can distinguish the readback path from waiting alone.
    static int dump_pass = -2;
    static uint64_t dump_pass_frame = ~0ull;
    static int dump_pass_ordinal;
    if (dump_pass == -2) {
        const char *e = getenv("EOWRECOMP_VK_DUMP_PASS");
        dump_pass = e ? atoi(e) : -1;
        if (dump_pass >= 0) LOG("[vk] diagnostic: read back color pass %d each frame\n", dump_pass);
    }
    if (dump_pass >= 0 && st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->backend) {
        uint64_t frame = renderer_present_count();
        if (frame != dump_pass_frame) { dump_pass_frame = frame; dump_pass_ordinal = 0; }
        int ended_pass = dump_pass_ordinal++;
        if (ended_pass == dump_pass) {
            char path[128];
            static bool reported;
            snprintf(path, sizeof path, "build/probe_pass.png");
            if (!reported) {
                LOG("[vk] diagnostic: read back pass %d each frame to %s\n", ended_pass, path);
                reported = true;
            }
            renderer_save_texture_tex(st.rt.color[0]->backend, path);
        }
    }
    if (g_debug_frame && st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->backend) {
        char path[256];
        static uint64_t debug_pass_frame = ~0ull;
        static int multi = -1;
        if (multi < 0) multi = getenv("EOWRECOMP_DEBUG_FRAMES") != NULL;
        if (multi && renderer_present_count() != debug_pass_frame) { debug_pass_frame = renderer_present_count(); debug_pass_no = 0; }
        if (multi) snprintf(path, sizeof path, "build/pass_%llu_%03d.png", (unsigned long long)debug_pass_frame, debug_pass_no++);
        else snprintf(path, sizeof path, "build/pass_%03d.png", debug_pass_no++);
        renderer_save_texture_tex(st.rt.color[0]->backend, path);
    }
}

static uint32_t level_extent(uint32_t v, uint32_t lv) { v >>= lv; return v ? v : 1; }

static void begin_pass(void) {
    if (in_pass && !st.rt_dirty) return;
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (EOWRECOMP_VK_DEVLOST_TRACE=2: each pass its own submission)
    else {
        // Diagnostic A/B: split at the same pass boundaries without waiting for the GPU. This
        // separates the effects of a new submission/snapshot from the CPU stall in renderer_flush.
        static int split_passes = -1;
        if (split_passes < 0) {
            const char *e = getenv("EOWRECOMP_VK_SPLIT_PASSES");
            split_passes = e && atoi(e) != 0;
            if (split_passes) LOG("[vk] diagnostic: submit at each render-pass boundary without waiting\n");
        }
        if (split_passes) renderer_submit();
    }
    st.rt_dirty = false;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);  // passes, copies and dispatches see each other's results
    {
        NvnTexture *t0n = st.rt.ncolor ? st.rt.color[0] : NULL;
        dl_note("pass %d color %ux%u fmt %#x depth %p", st.rt.ncolor, t0n ? t0n->width : 0, t0n ? t0n->height : 0, t0n ? t0n->format : 0,
                (void *)st.rt.depth);
    }
    if (g_gpuprof_frame) {
        NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : NULL;
        gp_mark(cb, "pass: %d color %ux%u fmt %#x, depth %s", st.rt.ncolor, t0 ? t0->width : 0, t0 ? t0->height : 0,
                t0 ? t0->format : 0, st.rt.depth ? "yes" : "no");
    }
    VkRenderingAttachmentInfo ca[8], da = {0}, sa = {0};
    uint32_t w = 0, minw = UINT32_MAX, minh = UINT32_MAX;  // w: first attachment seen
    pass_ncolor = (uint32_t)st.rt.ncolor;
    pass_samples = 1;
    for (int i = 0; i < st.rt.ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        pass_color[i] = VK_FORMAT_UNDEFINED;
        if (!t) continue;
        renderer_mark_gpu_owned(t);
        VkImageView v = vk_view(t, &st.rt.cview[i], st.rt.has_cview[i], true, TEXTYPE_2D);
        TexBackend *b = t->backend;
        if (!v) continue;
        ca[i].imageView = v;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        bool fresh = (int)renderer_texture_level_fresh(t) | (int)renderer_texture_frame_fresh(t);  // both: each marks its state
        if (renderer_texture_first_write(t) || fresh) {
            ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ca[i].clearValue.color.float32[3] = fresh ? 0.0f : 1.0f;
        }
        pass_color[i] = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 4) && vk_format(t->format) == b->format
                      ? vk_format(st.rt.cview[i].format) : b->format;
        uint32_t lv = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0;
        uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
        if (!w) { renderer_texture_scale(t, &pass_sx, &pass_sy); w = ew; }
        if (ew < minw) minw = ew;
        if (eh < minh) minh = eh;
        pass_samples = b->samples;
    }
    pass_depth = pass_stencil = VK_FORMAT_UNDEFINED;
    bool has_d = false, has_s = false;
    if (st.rt.depth) {
        renderer_mark_gpu_owned(st.rt.depth);
        VkImageView v = vk_view(st.rt.depth, &st.rt.dview, st.rt.has_dview, true, TEXTYPE_2D);
        TexBackend *b = st.rt.depth->backend;
        if (v && renderer_format_is_depth(st.rt.depth->format)) {
            VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v,
                                           .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                           .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            if (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { da = a; has_d = true; pass_depth = b->format; }
            if (b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) { sa = a; has_s = true; pass_stencil = b->format; }
            uint32_t lv = st.rt.has_dview && (st.rt.dview.setmask & 1) ? st.rt.dview.base_level : 0;
            uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
            if (!w) { renderer_texture_scale(st.rt.depth, &pass_sx, &pass_sy); w = ew; }
            if (ew < minw) minw = ew;
            if (eh < minh) minh = eh;
            if (!pass_ncolor) pass_samples = b->samples;
        }
    }
    if (!w) { pass_sx = pass_sy = 1; minw = minh = 16; }
    pass_w = minw; pass_h = minh;
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {minw, minh}}, .layerCount = 1,
                          .colorAttachmentCount = pass_ncolor, .pColorAttachments = ca,
                          .pDepthAttachment = has_d ? &da : NULL, .pStencilAttachment = has_s ? &sa : NULL};
    vkCmdBeginRendering(cb, &ri);
    ss_forget();  // (whatever ran between the passes may have set its own state)
    in_pass = true;
    pass_sampled_targets = pass_written_targets = 0;
    pass_draw_count = 0;
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    stat_passes++;
}

static void pass_size(int *w, int *h) {
    *w = *h = 0;
    for (int i = 0; i < st.rt.ncolor && !*w; i++)
        if (st.rt.color[i]) { *w = (int)st.rt.color[i]->width; *h = (int)st.rt.color[i]->height; }
    if (!*w && st.rt.depth) { *w = (int)st.rt.depth->width; *h = (int)st.rt.depth->height; }
    *w = (int)lroundf(*w * pass_sx);
    *h = (int)lroundf(*h * pass_sy);
}

static VkRect2D scissor_rect(void) {
    int w, h;
    pass_size(&w, &h);
    if (!w) return (VkRect2D){{0, 0}, {pass_w, pass_h}};
    int x = st.scissor[0][0], y = st.scissor[0][1], sw = st.scissor[0][2], sh = st.scissor[0][3];
    if (vp_sx != 1 || vp_sy != 1 || vp_ox != 0 || vp_oy != 0) {
        double x0 = floor(x * (double)vp_sx + vp_ox), y0 = floor(y * (double)vp_sy + vp_oy);
        double x1 = ceil(((double)x + sw) * vp_sx + vp_ox), y1 = ceil(((double)y + sh) * vp_sy + vp_oy);
        if (sw >= 16384) x1 = 1e9;
        if (sh >= 16384) y1 = 1e9;
        if (x1 > 1e9) x1 = 1e9;
        if (y1 > 1e9) y1 = 1e9;
        x = (int)x0; y = (int)y0; sw = (int)(x1 - x0); sh = (int)(y1 - y0);
    }
    if (x < 0) { sw += x; x = 0; }
    if (y < 0) { sh += y; y = 0; }
    if (w > (int)pass_w) w = (int)pass_w;
    if (h > (int)pass_h) h = (int)pass_h;
    if (x >= w || y >= h) { x = y = 0; sw = sh = 0; }
    if (x + sw > w) sw = w - x;
    if (y + sh > h) sh = h - y;
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    return (VkRect2D){{x, y}, {(uint32_t)sw, (uint32_t)sh}};
}

static void apply_scissor(void) {
    VkRect2D r = scissor_rect();
    if (!ss_need(SS_SC, !memcmp(&r, &ss.sc, sizeof r))) return;
    ss.sc = r;
    vkCmdSetScissor(vk_cmd(), 0, 1, &r);
}

// Metal convention (y up in NDC, origin top-left) through a negative-height viewport.
static void set_viewport(float x, float y, float w, float h, float zn, float zf) {
    if (w <= 0) w = 1;
    if (h == 0) h = 1;
    VkViewport vp = {x, y + h, w, -h, fminf(fmaxf(zn, 0), 1), fminf(fmaxf(zf, 0), 1)};
    if (!ss_need(SS_VP, !memcmp(&vp, &ss.vp, sizeof vp))) return;
    ss.vp = vp;
    vkCmdSetViewport(vk_cmd(), 0, 1, &vp);
}

// ================================================================== clears
static VkPipelineLayout pc_layout(void) {  // push constants only (clears)
    static VkPipelineLayout l;
    if (!l) {
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    }
    return l;
}

static const VkDynamicState DYN[] = {
    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
#define NDYN (sizeof DYN / sizeof DYN[0])

// Fixed-function state that isn't part of a pipeline is set explicitly on every bind (it persists across pipelines).
static void dyn_defaults(VkCommandBuffer cb) {
    vkCmdSetDepthBias(cb, 0, 0, 0);
    vkCmdSetDepthBiasEnable(cb, VK_FALSE);
    float bc[4] = {0, 0, 0, 0};
    vkCmdSetBlendConstants(cb, bc);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetDepthTestEnable(cb, VK_FALSE);
    vkCmdSetDepthWriteEnable(cb, VK_FALSE);
    vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilTestEnable(cb, VK_FALSE);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_COMPARE_OP_ALWAYS);
    vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
}

typedef struct { uint64_t key; VkPipeline pipe; } PipeEnt;
#define CLEAR_TAB 256
static PipeEnt clear_tab[CLEAR_TAB];

static VkPipeline clear_pipeline(int index, int kind, uint32_t mask, bool depth, bool stencil) {
    uint64_t key = 0xcbf29ce484222325ull;
#define MIX(v) do { uint64_t v_ = (uint64_t)(v); for (int b_ = 0; b_ < 8; b_++) { key ^= (v_ >> (b_ * 8)) & 0xFF; key *= 0x100000001B3ull; } } while (0)
    MIX(index); MIX(kind); MIX(mask); MIX(depth); MIX(stencil); MIX(pass_ncolor); MIX(pass_depth); MIX(pass_stencil); MIX(pass_samples);
    for (uint32_t i = 0; i < pass_ncolor; i++) MIX(pass_color[i]);
    uint32_t slot = (uint32_t)key & (CLEAR_TAB - 1);
    for (int p = 0; p < CLEAR_TAB; p++, slot = (slot + 1) & (CLEAR_TAB - 1)) {
        if (clear_tab[slot].key == key) return clear_tab[slot].pipe;
        if (!clear_tab[slot].key) break;
    }
    char fsname[32];
    if (index >= 0) snprintf(fsname, sizeof fsname, "clear.frag.%c%d", kind == 1 ? 'i' : kind == 2 ? 'u' : 'f', index);
    else snprintf(fsname, sizeof fsname, "clear.frag.none");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("clear.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fsname), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)pass_samples};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < pass_ncolor; i++)
        if ((int)i == index) cba[i].colorWriteMask = mask & 0xF;
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = pass_ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo rinfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = pass_ncolor,
                                           .pColorAttachmentFormats = pass_color, .depthAttachmentFormat = pass_depth,
                                           .stencilAttachmentFormat = pass_stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rinfo, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pc_layout()};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) LOG("[vk] clear pipeline failed\n");
    clear_tab[slot].key = key;
    clear_tab[slot].pipe = p;
    return p;
}

static void clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil, uint32_t sval,
                  uint32_t smask) {
    // An in-pass attachment clear is not ordered against texture reads by earlier draws.
    if (in_pass && pass_draw_count) vk_end_pass();
    begin_pass();
    VkCommandBuffer cb = vk_cmd();
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    VkRect2D rect = scissor_rect();
    if (!rect.extent.width || !rect.extent.height) return;
    if (index >= 0 && (index >= (int)pass_ncolor || pass_color[index] == VK_FORMAT_UNDEFINED)) return;
    uint16_t clear_writes = 0;
    if (index >= 0 && (mask & 0xF)) clear_writes |= (uint16_t)(1u << index);
    if ((depth && pass_depth) || (stencil && pass_stencil)) clear_writes |= (uint16_t)(1u << 8);
    bool simple = (index < 0 || (mask & 0xF) == 0xF) && (!stencil || (smask & 0xFF) == 0xFF);
    if (simple) {
        VkClearAttachment a[2];
        uint32_t n = 0;
        if (index >= 0) {
            a[n] = (VkClearAttachment){.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)index};
            memcpy(a[n].clearValue.color.float32, color, 16);
            n++;
        }
        VkImageAspectFlags dsa = (depth && pass_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil && pass_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (dsa) a[n++] = (VkClearAttachment){.aspectMask = dsa, .clearValue.depthStencil = {dval, sval}};
        VkClearRect cr = {rect, 0, 1};
        if (n) vkCmdClearAttachments(cb, n, a, 1, &cr);
        pass_written_targets |= clear_writes;
        return;
    }
    VkPipeline p = clear_pipeline(index, kind, mask, depth, stencil);
    if (!p) return;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    ss_forget();  // (its own pipeline and state: the next draw sets everything again)
    dyn_defaults(cb);
    if (depth) { vkCmdSetDepthTestEnable(cb, VK_TRUE); vkCmdSetDepthWriteEnable(cb, VK_TRUE); }
    if (stencil) {
        vkCmdSetStencilTestEnable(cb, VK_TRUE);
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,
                          VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, smask);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, sval);
    }
    int w, h;
    pass_size(&w, &h);
    VkViewport vp = {0, 0, (float)(w ? w : (int)pass_w), (float)(h ? h : (int)pass_h), 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &rect);
    float pc[8] = {0};
    if (color) memcpy(pc, color, 16);
    pc[4] = dval;
    vkCmdPushConstants(cb, pc_layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    pass_written_targets |= clear_writes;
}

// ================================================================== programs
#define MAX_BIND 96
typedef struct {
    ShaderInfo *vi, *fi;
    uint64_t code[2], cmax[2];  // translation inputs (copied: translated on the translator thread)
    uint64_t source_key, source_hash[2];  // stable identity for disk pipeline recipes
    uint64_t gid[2];      // guest code identity of the VS / FS (warm_code_id), 0 = unknown: what recipes are keyed by
    uint32_t gext[2];     // bytes of guest code the translator reads (maxwell_code_extent)
    bool warm;            // built by the startup warmup from cached guest code (code[] then point at host copies)
    int warm_first, warm_count;  // its recipe pipelines: warm_pipes[warm_first ..]
    ShaderKey vkey, fkey;
    VkShaderModule vs, fs;
    VkBindingInfo binds[MAX_BIND];
    int nbinds;
    VkDescriptorSetLayout set_layout[2];
    VkPipelineLayout layout;
    VkPipeline gpl_pre, gpl_frag; // reusable shader libraries when VK_EXT_graphics_pipeline_library is available
    bool vs_uses_vid;
    _Atomic int state;  // 0 building, 1 ready, 2 failed
} Prog;

static uint64_t glsl_source_hash(const char *src, int stage) {
    uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)stage;
    for (const uint8_t *p = (const uint8_t *)src; *p; p++) { h ^= *p; h *= 0x100000001B3ull; }
    return h ? h : 1;
}

static void save_glsl_source(uint64_t h, const char *src) {
    if (!getenv("EOWRECOMP_SAVE_GLSL_CACHE")) return;
    char path[600], tmp[640];
    snprintf(path, sizeof path, "%s/glsl", vk_cache_dir());
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/glsl/%016llx.glsl", vk_cache_dir(), (unsigned long long)h);
    FILE *existing = fopen(path, "rb");
    if (existing) { fclose(existing); return; }
    snprintf(tmp, sizeof tmp, "%s.%llx", path, (unsigned long long)(uintptr_t)src);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(src, 1, strlen(src), f) == strlen(src);
    fclose(f);
    if (!ok || rename(tmp, path) != 0) remove(tmp);
}

static VkPipelineLayout make_layout(Prog *p, int nsets, VkShaderStageFlags s0, VkShaderStageFlags s1) {
    p->set_layout[0] = vk_set_layout(p->binds, p->nbinds, 0, s0);
    if (nsets > 1) p->set_layout[1] = vk_set_layout(p->binds, p->nbinds, 1, s1);
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = (uint32_t)nsets,
                                     .pSetLayouts = p->set_layout};
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

#ifdef VK_EXT_graphics_pipeline_library
// The libraries are only ever fast-linked (no LINK_TIME_OPTIMIZATION link, so no RETAIN info: optimize_gpl_pipeline).
// Trirod-ShaderPrep creates the catalog's with these same flags, so the game's own creation finds them in the cache.
#define GPL_LIB_FLAGS VK_PIPELINE_CREATE_LIBRARY_BIT_KHR
static VkResult create_counted(VkGraphicsPipelineCreateInfo *ci, VkPipeline *out, int kind);
static bool make_program_libraries(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout layout, VkPipeline *pre_out,
                                   VkPipeline *frag_out) {
    if (!g_vk.graphics_pipeline_library || !g_vk.gpl_fast_linking) return false;
    VkPipelineShaderStageCreateInfo vst = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                           .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"};
    VkPipelineShaderStageCreateInfo fst = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                           .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    const VkDynamicState pre_dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
                                      VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
                                      VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE};
    const VkDynamicState frag_dyn[] = {VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                       VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                                       VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                                       VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP};
    VkPipelineDynamicStateCreateInfo pre_dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                               .dynamicStateCount = sizeof pre_dyn / sizeof pre_dyn[0], .pDynamicStates = pre_dyn};
    VkPipelineDynamicStateCreateInfo frag_dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                .dynamicStateCount = sizeof frag_dyn / sizeof frag_dyn[0], .pDynamicStates = frag_dyn};
    VkGraphicsPipelineLibraryCreateInfoEXT pre_gpl = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
        .flags = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT};
    VkGraphicsPipelineCreateInfo pre = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                        .pNext = &pre_gpl, .flags = GPL_LIB_FLAGS,
                                        .stageCount = 1, .pStages = &vst, .pViewportState = &vp,
                                        .pRasterizationState = &rs, .pDynamicState = &pre_dy, .layout = layout};
    VkGraphicsPipelineLibraryCreateInfoEXT frag_gpl = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
        .flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT};
    VkGraphicsPipelineCreateInfo frag = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                         .pNext = &frag_gpl, .flags = GPL_LIB_FLAGS,
                                         .stageCount = 1, .pStages = &fst, .pDepthStencilState = &ds,
                                         .pDynamicState = &frag_dy, .layout = layout};
    VkPipeline pre_lib = VK_NULL_HANDLE, frag_lib = VK_NULL_HANDLE;
    VkResult a = create_counted(&pre, &pre_lib, 1);
    VkResult b = a == VK_SUCCESS ? create_counted(&frag, &frag_lib, 1) : a;
    if (a != VK_SUCCESS || b != VK_SUCCESS) {
        if (pre_lib) vkDestroyPipeline(g_vk.dev, pre_lib, NULL);
        if (frag_lib) vkDestroyPipeline(g_vk.dev, frag_lib, NULL);
        return false;
    }
    *pre_out = pre_lib;
    *frag_out = frag_lib;
    return true;
}
#endif

bool vk_gpl_available(void) { return g_vk.graphics_pipeline_library && g_vk.gpl_fast_linking; }

// ShaderPrep uses the same translator and Vulkan driver cache as the game. Creating the reusable
// shader-stage libraries here gives the driver compiled stage material before Play; runtime recreates
// the small library handles from that cache and links them with the current draw's state.
bool vk_gpl_prepare_spirv_pair(const uint32_t *vs_words, size_t vs_size, const uint32_t *fs_words, size_t fs_size) {
#ifdef VK_EXT_graphics_pipeline_library
    if (!vk_gpl_available() || !vs_words || !fs_words || !vs_size || !fs_size) return false;
    VkBindingInfo bindings[MAX_BIND];
    int nb = vk_reflect(vs_words, vs_size, bindings, MAX_BIND);
    nb += vk_reflect(fs_words, fs_size, bindings + nb, MAX_BIND - nb);
    Prog p = {.binds = {0}, .nbinds = nb};
    memcpy(p.binds, bindings, (size_t)nb * sizeof bindings[0]);
    VkPipelineLayout layout = make_layout(&p, 2, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
    VkShaderModule vs = vk_module(vs_words, vs_size), fs = vk_module(fs_words, fs_size);
    VkPipeline pre = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    bool ok = vs && fs && make_program_libraries(vs, fs, layout, &pre, &frag);
    if (pre) vkDestroyPipeline(g_vk.dev, pre, NULL);
    if (frag) vkDestroyPipeline(g_vk.dev, frag, NULL);
    if (vs) vkDestroyShaderModule(g_vk.dev, vs, NULL);
    if (fs) vkDestroyShaderModule(g_vk.dev, fs, NULL);
    vkDestroyPipelineLayout(g_vk.dev, layout, NULL);
    return ok;
#else
    (void)vs_words; (void)vs_size; (void)fs_words; (void)fs_size;
    return false;
#endif
}

static void save_glsl(const char *src, uint64_t addr, const char *suffix) {
    if (!getenv("EOWRECOMP_SAVE_MSL")) return;
    // The packaged game directory may not contain a build/ directory yet.
    // Create each level so shader diagnostics work on a clean install.
    mkdir("build", 0755);
    mkdir("build/glsl", 0755);
    char path[256];
    snprintf(path, sizeof path, "build/glsl/%llx_%s.glsl", (unsigned long long)addr, suffix);
    FILE *f = fopen(path, "w");
    if (f) { fputs(src, f); fclose(f); }
}

// Save the guest NVN shader blob used for translation (header + Maxwell code, bounded by the
// shader's constant-data offset where available). This mirrors the Metal backend's dump hook.
static void save_shader_blob(uint64_t addr, size_t size, const char *suffix) {
    if (!getenv("EOWRECOMP_DUMP_SHADERS")) return;
    mkdir("build", 0755);
    mkdir("build/shaders", 0755);
    char path[256];
    snprintf(path, sizeof path, "build/shaders/%llx_%s_code.bin", (unsigned long long)addr, suffix);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite((const void *)(uintptr_t)addr, 1, size, f); fclose(f); }
}

// Shader warmup (below build_pipeline): recipes recorded from this run's draws, rebuilt at the next start.
static void warm_prog_done(Prog *p);
static void warm_save_code(const Prog *p);
static void warm_code_ids(const NvnProgram *np, uint64_t gid[2], uint32_t gext[2]);
static Prog *warm_prog_find(const uint64_t gid[2], const uint32_t gext[2], const ShaderKey *vkey, const ShaderKey *fkey);
static uint64_t stat_warm_prog_hits, stat_warm_pipe_hits, stat_new_pipes;
// The session's totals (vk_pipeline_session_summary). First draw of each pipeline / program: built from a recorded
// recipe and already ready, or still building; or not in the recipes at all (missed: a new build, recorded for the
// next preparation). Driver side, from VK_PIPELINE_CREATION_FEEDBACK (core 1.3): each vkCreateGraphicsPipelines that
// came out of the pipeline cache (prepared by Setup or saved by an earlier run) or had to be compiled.
static struct {
    uint64_t reused_ready, reused_building, missed, prog_reused, prog_missed;
    _Atomic uint64_t cache_hit, compiled, linked, lib_hit, lib_compiled;
} sess;

// vkCreateGraphicsPipelines for one pipeline, counted: kind 0 a complete pipeline, 1 a shader-stage library, 2 a fast
// link of libraries (not a compile)
static VkResult create_counted(VkGraphicsPipelineCreateInfo *ci, VkPipeline *out, int kind) {
    VkPipelineCreationFeedback fb = {0};
    VkPipelineCreationFeedbackCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
                                               .pNext = ci->pNext, .pPipelineCreationFeedback = &fb};
    const void *next = ci->pNext;
    ci->pNext = &fi;
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, ci, NULL, out);
    ci->pNext = next;
    if (r != VK_SUCCESS) return r;
    bool hit = (fb.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) &&
               (fb.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT);
    if (kind == 2) atomic_fetch_add(&sess.linked, 1);
    else if (kind == 1) atomic_fetch_add(hit ? &sess.lib_hit : &sess.lib_compiled, 1);
    else atomic_fetch_add(hit ? &sess.cache_hit : &sess.compiled, 1);
    return r;
}

// "prepared" (recipes built at the start), first draws, the driver's cache: one line, for the fps log and the exit
void vk_pipeline_session_summary(char *out, size_t n) {
    VkWarmSummary w;
    vk_warm_summary(&w);
    snprintf(out, n, "pipelines: %d prepared from recipes (%d built); first draws: %llu reused ready, %llu reused still "
             "building, %llu missed (new); programs %llu reused, %llu new; driver: %llu from cache, %llu compiled, %llu "
             "fast-linked; stage libraries %llu from cache, %llu compiled",
             w.pipelines, w.pipelines_ok, (unsigned long long)sess.reused_ready, (unsigned long long)sess.reused_building,
             (unsigned long long)sess.missed, (unsigned long long)sess.prog_reused, (unsigned long long)sess.prog_missed,
             (unsigned long long)atomic_load(&sess.cache_hit), (unsigned long long)atomic_load(&sess.compiled),
             (unsigned long long)atomic_load(&sess.linked), (unsigned long long)atomic_load(&sess.lib_hit),
             (unsigned long long)atomic_load(&sess.lib_compiled));
}

static void compile_program(void *arg) {
    Prog *p = arg;
    size_t nv = 0, nf = 0;
    uint32_t *sv = vk_glsl_to_spirv(p->vi->msl, STAGE_VERTEX, "vs", &nv);
    const char *fs_src = p->fi->msl;
    char *debug_fs = NULL;
    if (sv && p->code[1] == 0x72a71a8500ull && getenv("EOWRECOMP_DEBUG_VOID_SOLID")) {
        // Replace this shader's final output with opaque magenta to test raster/depth/blend state
        // independently from its texture sampling and Maxwell math.
        size_t len = strlen(fs_src);
        const char *end = strrchr(fs_src, '}');
        if (end) {
            static const char output[] = "\n  c0 = vec4(1.0, 0.0, 1.0, 1.0);\n";
            size_t prefix = (size_t)(end - fs_src);
            debug_fs = malloc(len + sizeof output);
            if (debug_fs) {
                memcpy(debug_fs, fs_src, prefix);
                memcpy(debug_fs + prefix, output, sizeof output - 1);
                memcpy(debug_fs + prefix + sizeof output - 1, end, len - prefix + 1);
                fs_src = debug_fs;
                LOG("[debug] void fragment shader %llx output forced to opaque magenta\n",
                    (unsigned long long)p->code[1]);
            }
        }
    }
    uint32_t *sf = sv ? vk_glsl_to_spirv(fs_src, STAGE_FRAGMENT, "fs", &nf) : NULL;
    if (sv && sf) {
        p->source_hash[0] = glsl_source_hash(p->vi->msl, STAGE_VERTEX);
        p->source_hash[1] = glsl_source_hash(fs_src, STAGE_FRAGMENT);
        uint64_t pair[2] = {p->source_hash[0], p->source_hash[1]};
        p->source_key = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < sizeof pair; i++) { p->source_key ^= ((const uint8_t *)pair)[i]; p->source_key *= 0x100000001B3ull; }
        if (!p->source_key) p->source_key = 1;
        save_glsl_source(p->source_hash[0], p->vi->msl);
        save_glsl_source(p->source_hash[1], fs_src);
    }
    free(debug_fs);
    if (!sv || !sf) { free(sv); free(sf); atomic_store(&p->state, 2); warm_prog_done(p); return; }
    p->nbinds = vk_reflect(sv, nv, p->binds, MAX_BIND);
    p->nbinds += vk_reflect(sf, nf, p->binds + p->nbinds, MAX_BIND - p->nbinds);
    p->vs = vk_module(sv, nv);
    p->fs = vk_module(sf, nf);
    free(sv);
    free(sf);
    p->layout = make_layout(p, 2, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
#ifdef VK_EXT_graphics_pipeline_library
    if (p->vs && p->fs && vk_gpl_available() &&
        !make_program_libraries(p->vs, p->fs, p->layout, &p->gpl_pre, &p->gpl_frag))
        LOG("[vk] shader-library creation failed; using monolithic pipeline fallback\n");
#endif
    atomic_fetch_add(&stat_compiles, 1);
    atomic_store(&p->state, p->vs && p->fs ? 1 : 2);
    warm_prog_done(p);
}

static void translate_program(void *arg) {
    Prog *p = arg;
    { static int g = -1; if (g < 0) { const char *e = getenv("EOWRECOMP_SHADER_GUARD"); g = e ? atoi(e) : 0; if (g > 0) maxwell_loop_guard = (uint32_t)g; } }
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    save_shader_blob(p->code[0], p->cmax[0], "vs");
    save_shader_blob(p->code[1], p->cmax[1], "fs");
    pthread_mutex_lock(&translate_lock);
    p->vi = maxwell_translate((const uint8_t *)(uintptr_t)p->code[0], p->cmax[0], STAGE_VERTEX, &p->vkey);
    if (p->vi->msl) {
        p->fkey.vs_out_mask = p->vi->out_mask;
        p->fi = maxwell_translate((const uint8_t *)(uintptr_t)p->code[1], p->cmax[1], STAGE_FRAGMENT, &p->fkey);
    }
    pthread_mutex_unlock(&translate_lock);
    if (!p->vi->msl) {
        LOG("[shader] VS translate failed: %s (code %llx)\n", p->vi->error, (unsigned long long)p->code[0]);
        atomic_store(&p->state, 2);
        warm_prog_done(p);
        return;
    }
    if (!p->fi->msl) {
        LOG("[shader] FS translate failed: %s\n", p->fi->error);
        atomic_store(&p->state, 2);
        warm_prog_done(p);
        return;
    }
    p->vs_uses_vid = strstr(p->vi->msl, "gl_VertexIndex") != NULL;
    save_glsl(p->vi->msl, p->code[0], "vs");
    save_glsl(p->fi->msl, p->code[1], "fs");
    if (!p->warm) warm_save_code(p);  // (the guest code, for the next start's warmup)
    run_job_q(compile_program, p, p->warm);
}

#define PROG_TAB 4096
static struct { uint64_t key; Prog *prog; } prog_tab[PROG_TAB];

static Prog *get_program(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey) {
    if (!(np->stage_mask & 1) || !(np->stage_mask & 2)) return NULL;
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 2; i++) { uint64_t a = np->stages[i].code_addr; for (int k = 0; k < 8; k++) { h ^= (a >> (k * 8)) & 0xFF; h *= 0x100000001B3ull; } }
    const uint8_t *vb = (const uint8_t *)vkey;
    for (size_t i = 0; i < sizeof *vkey; i++) { h ^= vb[i]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 8; i++) { h ^= fkey->rt_kind[i]; h *= 0x100000001B3ull; }
    if (fkey->stencil_tex) { h ^= fkey->stencil_tex; h *= 0x100000001B3ull; }
    if (!h) h = 1;
    uint32_t slot = (uint32_t)(h ^ (h >> 31)) & (PROG_TAB - 1);
    while (prog_tab[slot].key && prog_tab[slot].key != h) slot = (slot + 1) & (PROG_TAB - 1);
    if (prog_tab[slot].key) return prog_tab[slot].prog;
    // Guest addresses change between runs, the code doesn't: a program the startup warmup built from the same code
    // and keys is this one (its translation reads only the gext bytes, maxwell_code_extent).
    uint64_t gid[2];
    uint32_t gext[2];
    warm_code_ids(np, gid, gext);
    Prog *w = warm_prog_find(gid, gext, vkey, fkey);
    if (w) {
        prog_tab[slot].key = h;
        prog_tab[slot].prog = w;
        stat_warm_prog_hits++;
        sess.prog_reused++;
        return w;
    }
    sess.prog_missed++;
    Prog *p = calloc(1, sizeof *p);
    prog_tab[slot].key = h;
    prog_tab[slot].prog = p;
    for (int i = 0; i < 2; i++) {
        p->code[i] = np->stages[i].code_addr;
        p->cmax[i] = np->stages[i].const_offset ? np->stages[i].const_offset : 0x10000;
        p->gid[i] = gid[i];
        p->gext[i] = gext[i];
    }
    p->vkey = *vkey;
    p->fkey = *fkey;
    run_translate(translate_program, p);
    return p;
}

// ================================================================== vertex formats / fixed function tables
static int attr_kind(uint32_t fmt) {
    switch (fmt) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
    case 0x27: case 0x2C: case 0x2F: case 0x3E:
        return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
    case 0x28: case 0x2D: case 0x30:
        return 1;
    default: return 0;
    }
}

static VkFormat vertex_format(uint32_t f) {
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM;
    case 0x02: return VK_FORMAT_R8_SNORM;
    case 0x03: return VK_FORMAT_R8_UINT;
    case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT;
    case 0x06: return VK_FORMAT_R16_UNORM;
    case 0x07: return VK_FORMAT_R16_SNORM;
    case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT;
    case 0x0A: return VK_FORMAT_R32_SFLOAT;
    case 0x0B: return VK_FORMAT_R32_UINT;
    case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM;
    case 0x0E: return VK_FORMAT_R8G8_SNORM;
    case 0x0F: return VK_FORMAT_R8G8_UINT;
    case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT;
    case 0x12: return VK_FORMAT_R16G16_UNORM;
    case 0x13: return VK_FORMAT_R16G16_SNORM;
    case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT;
    case 0x16: return VK_FORMAT_R32G32_SFLOAT;
    case 0x17: return VK_FORMAT_R32G32_UINT;
    case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x19: return VK_FORMAT_R8G8B8_UNORM;
    case 0x1A: return VK_FORMAT_R8G8B8_SNORM;
    case 0x1B: return VK_FORMAT_R8G8B8_UINT;
    case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT;
    case 0x1E: return VK_FORMAT_R16G16B16_UNORM;
    case 0x1F: return VK_FORMAT_R16G16B16_SNORM;
    case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT;
    case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x23: return VK_FORMAT_R32G32B32_UINT;
    case 0x24: return VK_FORMAT_R32G32B32_SINT;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x51: return VK_FORMAT_R8_UINT;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

// 3-component 8/16-bit vertex formats aren't universally supported: read 4 components instead.
static VkFormat vertex_format_supported(VkFormat f) {
    static uint8_t known[256];  // 1 ok, 2 not
    uint32_t i = f < 256 ? f : 0;
    if (!known[i]) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
        known[i] = (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : 2;
    }
    if (known[i] == 1) return f;
    { static uint8_t told[256]; if (!told[i]++) LOG("[vk] vertex format %d unsupported: read as 4 components\n", f); }
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

static VkBlendFactor blend_factor(uint8_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: {
        static uint8_t warned[256];
        if (!warned[f]++) LOG("[vk] unknown blend factor %#x\n", f);
        return VK_BLEND_FACTOR_ONE;
    }
    }
}

static VkBlendOp blend_op(uint8_t e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp compare_fn(uint8_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static VkStencilOp stencil_op(uint8_t o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static int rt_kind(uint32_t f) {
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x27: case 0x2C: case 0x2F: case 0x3E: return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x28: case 0x2D: case 0x30: return 1;
    default: return 0;
    }
}

// ================================================================== graphics pipelines
// Everything a pipeline bakes in, captured by value so it can be built on a worker thread.
#define ZERO_BINDING 16
typedef struct {
    Prog *prog;
    uint64_t prog_key, vs_source_hash, fs_source_hash;
    VkFormat color[8], depth, stencil;
    uint32_t ncolor, samples;
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t attr_mask;
    uint32_t stride[17], divisor[17], used_streams;
    uint8_t blend[8][8];
    uint8_t blend_on, a2c, topo_class, poly_fill;
    uint32_t write_mask[8];
} PipeKey;

typedef struct {
    PipeKey k;
    uint64_t hash;
    VkPipeline pipe, optimized, gpl_vi, gpl_fo;
    _Atomic int state, optimized_state;
    bool warm;
} Pipe;  // state: 0 building, 1 ready, 2 failed
#define PIPE_TAB 65536
static struct { uint64_t key; Pipe *pipe; } pipe_tab[PIPE_TAB];

// (Pipeline recipes on disk: the shader warmup below build_pipeline, field by field rather than raw PipeKey structs.)

// 8 bytes a step (PipeKey is ~700 bytes, hashed per draw); in-memory tables only - hmix values are compared with
// stored constants (fragment-shader hashes) and must not change.
static uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) { h = (h ^ *b) * 0x100000001B3ull; }
    return h ^ (h >> 32);
}

static uint64_t hmix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

static uint64_t pipe_key_hash(const PipeKey *key) {
    PipeKey stable = *key;
    stable.prog = NULL;
    return hash_words(0xcbf29ce484222325ull, &stable, sizeof stable) ?: 1;
}

static VkPrimitiveTopology topo_class_base(uint8_t c) {
    return c == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : c == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

// Optimized (complete, monolithic) replacements queued and finished: Trirod-ShaderPrep reports them as a stage of their own.
static _Atomic int gpl_opt_queued, gpl_opt_done, gpl_opt_ok;
void vk_gpl_optimize_progress(int *done, int *ok, int *total) {
    *done = atomic_load(&gpl_opt_done);
    *ok = atomic_load(&gpl_opt_ok);
    *total = atomic_load(&gpl_opt_queued);
}

#ifdef VK_EXT_graphics_pipeline_library
static void optimize_dropped(void *arg) {
    atomic_store(&((Pipe *)arg)->optimized_state, 2);  // (keeps the fast-linked pipeline)
    atomic_fetch_add(&gpl_opt_done, 1);
}
static void optimize_gpl_pipeline(void *arg);
void vk_exec_drop_optimize_jobs(void) { queue_drop_idle(&jobs, optimize_gpl_pipeline, optimize_dropped); }

// The optimized replacement of a fast-linked pipeline is the complete (monolithic) pipeline for the same key, built on
// a low-priority worker from the program's shader modules, as DXVK does. Not a link-time-optimized link of the four
// libraries: that needs every library created with RETAIN_LINK_TIME_OPTIMIZATION_INFO, which made the prepared
// vk_pipelines.bin 1.19 GB instead of ~9 MB (15k shader-stage libraries keeping their compiler input). Without RETAIN
// such a link is invalid usage, and on NVIDIA its pipelines drew nothing: the black title screen of 2026-11-24.
static void build_pipeline_mode(Pipe *pp, bool optimize);
static void optimize_gpl_pipeline(void *arg) { build_pipeline_mode(arg, true); }
#else
void vk_exec_drop_optimize_jobs(void) {}
#endif

static void build_pipeline(void *arg) { build_pipeline_mode(arg, false); }

static void build_pipeline_mode(Pipe *pp, bool optimize) {
    PipeKey *k = &pp->k;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = k->prog->vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = k->prog->fs, .pName = "main"}};
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkVertexInputBindingDivisorDescriptionEXT vd[17];
    uint32_t na = 0, nb = 0, nd = 0;
    for (int i = 0; i < 32; i++)
        if (k->attr_mask & (1u << i)) va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, k->attr_bind[i], k->attr_fmt[i], k->attr_off[i]};
    for (uint32_t s = 0; s < 17; s++) {
        if (!(k->used_streams & (1u << s))) continue;
        bool inst = k->divisor[s] != 0;
        vb[nb++] = (VkVertexInputBindingDescription){s, k->stride[s], inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (inst && k->divisor[s] > 1 && g_vk.divisor) vd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, k->divisor[s]};
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                          .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = vd};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nd ? &dvi : NULL,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = topo_class_base(k->topo_class)};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = k->poly_fill ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples, .alphaToCoverageEnable = k->a2c};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < k->ncolor; i++) {
        cba[i].colorWriteMask = k->write_mask[i];
        if ((k->blend_on >> i) & 1) {
            const uint8_t *b = k->blend[i];
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = blend_factor(b[1]);
            cba[i].dstColorBlendFactor = blend_factor(b[2]);
            cba[i].srcAlphaBlendFactor = blend_factor(b[3]);
            cba[i].dstAlphaBlendFactor = blend_factor(b[4]);
            cba[i].colorBlendOp = blend_op(b[5]);
            cba[i].alphaBlendOp = blend_op(b[6]);
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = k->ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor,
                                        .pColorAttachmentFormats = k->color, .depthAttachmentFormat = k->depth,
                                        .stencilAttachmentFormat = k->stencil};
#ifdef VK_EXT_graphics_pipeline_library
    // Polygon mode is part of the pre-raster subset. The common fill mode reuses the per-program
    // pre-raster library; the uncommon wireframe mode stays on the exact monolithic path.
    if (!optimize && vk_gpl_available() && k->poly_fill && k->prog->gpl_pre && k->prog->gpl_frag) {
        const VkDynamicState vi_dyn[] = {VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
        const VkDynamicState fo_dyn[] = {VK_DYNAMIC_STATE_BLEND_CONSTANTS};
        VkPipelineDynamicStateCreateInfo vi_dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                  .dynamicStateCount = sizeof vi_dyn / sizeof vi_dyn[0], .pDynamicStates = vi_dyn};
        VkPipelineDynamicStateCreateInfo fo_dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                  .dynamicStateCount = sizeof fo_dyn / sizeof fo_dyn[0], .pDynamicStates = fo_dyn};
        VkGraphicsPipelineLibraryCreateInfoEXT vi_gpl = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
            .flags = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT};
        VkGraphicsPipelineCreateInfo vi_pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                              .pNext = &vi_gpl, .flags = GPL_LIB_FLAGS,
                                              .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                              .pDynamicState = &vi_dy, .layout = k->prog->layout};
        VkGraphicsPipelineLibraryCreateInfoEXT fo_gpl = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
            .pNext = &ri, .flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};
        VkGraphicsPipelineCreateInfo fo_pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                              .pNext = &fo_gpl, .flags = GPL_LIB_FLAGS,
                                              .pMultisampleState = &ms, .pColorBlendState = &cb,
                                              .pDynamicState = &fo_dy, .layout = k->prog->layout};
        VkPipeline libs[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkResult lr = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &vi_pi, NULL, &libs[0]);
        if (lr == VK_SUCCESS) lr = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &fo_pi, NULL, &libs[1]);
        if (lr == VK_SUCCESS) {
            VkPipeline all[4] = {libs[0], k->prog->gpl_pre, k->prog->gpl_frag, libs[1]};
            VkPipelineLibraryCreateInfoKHR li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
                                                 .libraryCount = 4, .pLibraries = all};
            ri.pNext = &li;
            VkGraphicsPipelineCreateInfo link = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                                 .pNext = &ri, .layout = k->prog->layout};
            lr = create_counted(&link, &pp->pipe, 2);
        }
        if (lr != VK_SUCCESS) {
            if (libs[0]) vkDestroyPipeline(g_vk.dev, libs[0], NULL);
            if (libs[1]) vkDestroyPipeline(g_vk.dev, libs[1], NULL);
            if (pp->pipe) vkDestroyPipeline(g_vk.dev, pp->pipe, NULL);
            pp->pipe = VK_NULL_HANDLE;
            static _Atomic int ngpl;
            if (atomic_fetch_add(&ngpl, 1) < 10) LOG("[vk] pipeline-library link failed: %d; retrying monolithic\n", lr);
        } else {
            pp->gpl_vi = libs[0];
            pp->gpl_fo = libs[1];
            atomic_fetch_add(&stat_compiles, 1);
            atomic_store(&pp->state, 1);
            static int lto = -1;  // EOWRECOMP_VK_GPL_OPTIMIZE=0: keep the fast-linked pipeline (no optimized replacement)
            if (lto < 0) lto = !(getenv("EOWRECOMP_VK_GPL_OPTIMIZE") && atoi(getenv("EOWRECOMP_VK_GPL_OPTIMIZE")) == 0);
            if (lto) atomic_fetch_add(&gpl_opt_queued, 1), run_job_q(optimize_gpl_pipeline, pp, 2);
            if (pp->warm) { extern void warm_pipe_done(bool ok); warm_pipe_done(true); }
            return;
        }
    }
#endif
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = k->prog->layout};
    if (optimize) {  // (optimize_gpl_pipeline: draw_ binds it once optimized_state is 1)
        VkPipeline opt = VK_NULL_HANDLE;
        VkResult r = create_counted(&pi, &opt, 0);
        if (r != VK_SUCCESS) {
            static _Atomic int n;
            if (atomic_fetch_add(&n, 1) < 10) LOG("[vk] optimized pipeline creation failed: %d; keeping the fast-linked one\n", r);
        } else pp->optimized = opt, atomic_fetch_add(&gpl_opt_ok, 1);
        atomic_store(&pp->optimized_state, r == VK_SUCCESS ? 1 : 2);
        atomic_fetch_add(&gpl_opt_done, 1);
        return;
    }
    VkResult r = create_counted(&pi, &pp->pipe, 0);
    if (r != VK_SUCCESS) {
        static int n;
        if (n++ < 10) LOG("[vk] pipeline creation failed: %d\n", r);
    }
    atomic_fetch_add(&stat_compiles, 1);
    atomic_store(&pp->state, r == VK_SUCCESS ? 1 : 2);
    if (pp->warm) {
        extern void warm_pipe_done(bool ok);
        warm_pipe_done(r == VK_SUCCESS);
    }
}

// ================================================================== shader warmup: pipeline recipes
// The first time a draw meets a program or a pipeline, it is built off the render thread, and the draw is skipped until
// then (or waits: compile_wait). The existing caches only make those builds cheaper:
//  - <cache>/spirv (shaders.c): GLSL -> SPIR-V, keyed by the GLSL text. Saves glslang, not the Maxwell translation.
//  - <cache>/vk_pipelines.bin (device.c): the driver's pipeline cache. vkCreateGraphicsPipelines gets faster, but must
//    still be called for every pipeline, and only once its program exists.
//  - <cache>/glsl (save_glsl_source): translated sources, for diagnostics.
// So each session still translated every program and created every pipeline on first use: entering Hyrule Town
// skipped 1684 draws in two seconds, and a skipped draw that fed a later pass flashed black. The warmup records what a
// run builds and rebuilds it at the next start, before the game runs:
//  - <cache>/warm/code/<id>-<bytes>.bin: the guest VS / FS code the translator reads (maxwell_code_extent bytes),
//    named by its content hash. A curated, portable seed set may ship under tools/shader_catalog/warm; its code
//    records are copied into the player's cache as they are loaded.
//  - <cache>/warm/recipes.bin: one record per pipeline - the two code ids and sizes, both shader keys and every PipeKey
//    field, written field by field (no pointers, handles or raw structs) with a checksum. Local files are tagged with
//    the GPU's vendor / device id. Shared schema-2 catalogs use zero IDs; each device validates formats and rebuilds
//    compatible pipelines with its own driver.
// Trirod-ShaderPrep calls vk_warm_start to translate recorded programs and create their pipelines before Play. The game
// loads the resulting driver's pipeline cache and builds any missing pipeline on demand. The runtime still records new
// recipes for a later Setup run. A record that is corrupt, lacks code, or no longer matches the translated shader is
// ignored by ShaderPrep. The translator version is not part of the key: every warm program uses the current translator.
// EOWRECOMP_VK_WARM=0: disable recipe preparation / recording (used by ShaderPrep and diagnostics).
#define WARM_SCHEMA 2
#define WARM_PAYLOAD 800             // record: code ids and sizes (24), vertex / fragment keys (2 x 50), PipeKey (676)
#define WARM_REC (WARM_PAYLOAD + 8)  // + checksum
#define WARM_MAX 65536               // records kept
#define WARM_PROG_TAB 16384
#define WARM_PIPE_TAB 131072
_Static_assert(sizeof(VkFormat) == 4, "recipes store formats as 32 bits");

static bool warm_off(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("EOWRECOMP_VK_WARM"); v = e && atoi(e) == 0; }
    return v;
}

static uint64_t fnv64(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

static uint64_t warm_code_id(const uint8_t *code, uint32_t ext) {
    uint64_t h = fnv64(0xcbf29ce484222325ull, code, ext);
    h = fnv64(h, &ext, sizeof ext);
    return h ? h : 1;
}

static void warm_code_ids(const NvnProgram *np, uint64_t gid[2], uint32_t gext[2]) {
    for (int i = 0; i < 2; i++) {
        gid[i] = 0;
        gext[i] = 0;
        if (warm_off() || !np->stages[i].code_addr) continue;
        const uint8_t *code = (const uint8_t *)(uintptr_t)np->stages[i].code_addr;
        size_t cmax = np->stages[i].const_offset ? np->stages[i].const_offset : 0x10000;  // (as get_program's cmax)
        gext[i] = (uint32_t)maxwell_code_extent(code, cmax, i ? STAGE_FRAGMENT : STAGE_VERTEX);
        gid[i] = warm_code_id(code, gext[i]);
    }
}

// ---- record fields (little-endian, as every target is)
static void wput(uint8_t **w, const void *v, size_t n) { memcpy(*w, v, n); *w += n; }
static void wget(const uint8_t **r, void *v, size_t n) { memcpy(v, *r, n); *r += n; }

static void put_skey(uint8_t **w, const ShaderKey *k) {  // 50 bytes
    wput(w, k->attr_kind, 32);
    wput(w, k->rt_kind, 8);
    uint8_t f[4] = {k->depth_minus_one, k->flip_y, k->flip_x, k->glsl};
    wput(w, f, 4);
    wput(w, &k->stencil_tex, 2);
    wput(w, &k->vs_out_mask, 4);
}

static bool get_skey(const uint8_t **r, ShaderKey *k) {
    memset(k, 0, sizeof *k);
    uint8_t f[4];
    wget(r, k->attr_kind, 32);
    wget(r, k->rt_kind, 8);
    wget(r, f, 4);
    wget(r, &k->stencil_tex, 2);
    wget(r, &k->vs_out_mask, 4);
    k->depth_minus_one = f[0];
    k->flip_y = f[1];
    k->flip_x = f[2];
    k->glsl = f[3];
    for (int i = 0; i < 32; i++) if (k->attr_kind[i] > 2) return false;
    for (int i = 0; i < 8; i++) if (k->rt_kind[i] > 2) return false;
    return f[0] <= 1 && f[1] <= 1 && f[2] <= 1 && f[3] == 1;
}

static void put_pkey(uint8_t **w, const PipeKey *k) {  // 676 bytes (prog and the unused source-hash fields: not stored)
    wput(w, k->color, 32);
    wput(w, &k->depth, 4);
    wput(w, &k->stencil, 4);
    wput(w, &k->ncolor, 4);
    wput(w, &k->samples, 4);
    wput(w, k->attr_fmt, 128);
    wput(w, k->attr_off, 128);
    wput(w, k->attr_bind, 128);
    wput(w, &k->attr_mask, 4);
    wput(w, k->stride, 68);
    wput(w, k->divisor, 68);
    wput(w, &k->used_streams, 4);
    wput(w, k->blend, 64);
    uint8_t f[4] = {k->blend_on, k->a2c, k->topo_class, k->poly_fill};
    wput(w, f, 4);
    wput(w, k->write_mask, 32);
}

// A format value this device supports for `need` (vertex: buffer features, else optimal tiling). Memoized: formats
// are few. A recipe from a corrupt file or another driver must not reach vkCreateGraphicsPipelines with garbage.
static bool fmt_ok(VkFormat f, VkFormatFeatureFlags need, bool vertex) {
    if (!((f > 0 && f <= 184) || (f >= 1000000000 && f < 1001000000))) return false;
    static struct { VkFormat f; VkFormatProperties p; } memo[64];
    static int nmemo;
    VkFormatProperties fp = {0};
    int i = 0;
    while (i < nmemo && memo[i].f != f) i++;
    if (i < nmemo) fp = memo[i].p;
    else {
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &fp);
        if (nmemo < 64) { memo[nmemo].f = f; memo[nmemo++].p = fp; }
    }
    return ((vertex ? fp.bufferFeatures : fp.optimalTilingFeatures) & need) == need;
}

static bool get_pkey(const uint8_t **r, PipeKey *k) {
    memset(k, 0, sizeof *k);  // (as draw_: padding included, the key is hashed bytewise)
    uint8_t f[4];
    wget(r, k->color, 32);
    wget(r, &k->depth, 4);
    wget(r, &k->stencil, 4);
    wget(r, &k->ncolor, 4);
    wget(r, &k->samples, 4);
    wget(r, k->attr_fmt, 128);
    wget(r, k->attr_off, 128);
    wget(r, k->attr_bind, 128);
    wget(r, &k->attr_mask, 4);
    wget(r, k->stride, 68);
    wget(r, k->divisor, 68);
    wget(r, &k->used_streams, 4);
    wget(r, k->blend, 64);
    wget(r, f, 4);
    wget(r, k->write_mask, 32);
    k->blend_on = f[0];
    k->a2c = f[1];
    k->topo_class = f[2];
    k->poly_fill = f[3];
    if (k->ncolor > 8 || (k->samples != 1 && k->samples != 2 && k->samples != 4 && k->samples != 8)) return false;
    if (k->topo_class > 2 || k->a2c > 1 || k->poly_fill > 1 || (k->used_streams >> 17)) return false;
    for (uint32_t i = 0; i < 8; i++) {
        if (k->write_mask[i] > 0xF || (i >= k->ncolor && (k->color[i] || k->write_mask[i] || ((k->blend_on >> i) & 1)))) return false;
        if (i < k->ncolor && k->color[i] != VK_FORMAT_UNDEFINED && !fmt_ok(k->color[i], VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, false)) return false;
    }
    if (k->depth != VK_FORMAT_UNDEFINED && !fmt_ok(k->depth, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, false)) return false;
    if (k->stencil != VK_FORMAT_UNDEFINED && !fmt_ok(k->stencil, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, false)) return false;
    for (int i = 0; i < 32; i++) {
        if (!(k->attr_mask & (1u << i))) continue;
        if (k->attr_bind[i] > ZERO_BINDING || !(k->used_streams & (1u << k->attr_bind[i])) || k->attr_off[i] > 2048) return false;
        if (!fmt_ok((VkFormat)k->attr_fmt[i], VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT, true)) return false;
    }
    for (int s = 0; s < 17; s++) if ((k->used_streams & (1u << s)) && k->stride[s] > 4096) return false;
    return true;
}

static void rec_build(uint8_t *rec, const Prog *p, const PipeKey *k) {
    uint8_t *w = rec;
    wput(&w, p->gid, 16);
    wput(&w, p->gext, 8);
    put_skey(&w, &p->vkey);
    ShaderKey fk = p->fkey;
    fk.vs_out_mask = 0;  // (set by the translation; get_program's input has none)
    put_skey(&w, &fk);
    put_pkey(&w, k);
    uint64_t c = fnv64(0x84222325cbf29ce4ull, rec, WARM_PAYLOAD);
    wput(&w, &c, 8);
}

// ---- the recipe set: loaded records plus this run's new ones, saved together (vk_warm_save)
static pthread_mutex_t rec_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *recs;
static uint32_t nrecs, cap_recs;
static uint64_t rec_set[WARM_MAX * 2];  // payload hashes
static bool rec_dirty;
static uint64_t rec_saved_ns;
static char warm_catalog_dir[1024];

void vk_warm_catalog_set(const char *dir) {
    snprintf(warm_catalog_dir, sizeof warm_catalog_dir, "%s", dir && *dir ? dir : "");
}

static bool rec_add_locked(const uint8_t *rec) {
    uint64_t h = fnv64(0xcbf29ce484222325ull, rec, WARM_PAYLOAD);
    if (!h) h = 1;
    uint32_t i = (uint32_t)(h ^ (h >> 32)) & (WARM_MAX * 2 - 1);
    while (rec_set[i]) {
        if (rec_set[i] == h) return false;
        i = (i + 1) & (WARM_MAX * 2 - 1);
    }
    if (nrecs >= WARM_MAX) return false;
    if (nrecs == cap_recs) {
        uint32_t cap = cap_recs ? cap_recs * 2 : 1024;
        uint8_t *n = realloc(recs, (size_t)cap * WARM_REC);
        if (!n) return false;
        recs = n;
        cap_recs = cap;
    }
    rec_set[i] = h;
    memcpy(recs + (size_t)nrecs++ * WARM_REC, rec, WARM_REC);
    return true;
}

static void recipe_note(Prog *p, const PipeKey *k) {
    if (warm_off() || !p->gid[0] || !p->gid[1]) return;
    uint8_t rec[WARM_REC];
    rec_build(rec, p, k);
    pthread_mutex_lock(&rec_lock);
    if (rec_add_locked(rec)) rec_dirty = true;
    pthread_mutex_unlock(&rec_lock);
}

// Written to a temporary file and renamed over the old one; a run that dies before saving keeps the previous set.
void vk_warm_save(void) {
    if (warm_off()) return;
    pthread_mutex_lock(&rec_lock);
    if (!rec_dirty || !nrecs) { pthread_mutex_unlock(&rec_lock); return; }
    char path[640], tmp[680];
    snprintf(path, sizeof path, "%s/warm", vk_cache_dir());
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/warm/recipes.bin", vk_cache_dir());
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = false;
    if (f) {
        uint8_t hdr[32] = {0};
        memcpy(hdr, "EOWWARM", 8);
        uint32_t v[5] = {WARM_SCHEMA, WARM_REC, g_vk.props.vendorID, g_vk.props.deviceID, nrecs};
        memcpy(hdr + 8, v, sizeof v);
        ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr && fwrite(recs, WARM_REC, nrecs, f) == nrecs;
        ok = fclose(f) == 0 && ok;
    }
    if (ok && rename(tmp, path) != 0) {  // (Windows: rename does not replace)
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) remove(tmp);
    else rec_dirty = false;
    rec_saved_ns = SDL_GetTicksNS();
    uint32_t n = nrecs;
    pthread_mutex_unlock(&rec_lock);
    LOG(ok ? "[warm] saved %u pipeline recipes\n" : "[warm] could not save the %u pipeline recipes\n", n);
}

// The guest code of a program translated from guest memory, once per code id (translator thread).
static void warm_save_code(const Prog *p) {
    if (warm_off()) return;
    for (int i = 0; i < 2; i++) {
        if (!p->gid[i] || !p->gext[i]) continue;
        char path[640], tmp[700];
        snprintf(path, sizeof path, "%s/warm/code/%016llx-%x.bin", vk_cache_dir(), (unsigned long long)p->gid[i], p->gext[i]);
        FILE *e = fopen(path, "rb");
        if (e) { fclose(e); continue; }
        snprintf(tmp, sizeof tmp, "%s/warm", vk_cache_dir());
        mkdir(tmp, 0755);
        snprintf(tmp, sizeof tmp, "%s/warm/code", vk_cache_dir());
        mkdir(tmp, 0755);
        snprintf(tmp, sizeof tmp, "%s.%llx", path, (unsigned long long)(uintptr_t)p);
        FILE *f = fopen(tmp, "wb");
        if (!f) continue;
        bool ok = fwrite((const void *)(uintptr_t)p->code[i], 1, p->gext[i], f) == p->gext[i];
        ok = fclose(f) == 0 && ok;
        if (!ok || rename(tmp, path) != 0) remove(tmp);  // (another program with the same code may have written it)
    }
}

// ---- the warm set
static struct { uint64_t key; Prog *p; } warm_progs[WARM_PROG_TAB];
static struct { uint64_t key; Pipe *pp; } warm_pipe_tab[WARM_PIPE_TAB];
static Pipe *warm_pipes;
static int warm_nprogs, warm_npipes;
static bool warm_active;
static _Atomic int warm_progs_done, warm_progs_ok, warm_pipes_done, warm_pipes_ok;
// what vk_warm_start read (vk_warm_summary: the shader preparation tool's report)
static VkWarmSummary warm_read;
static atomic_bool warm_reported;
static uint64_t warm_t0;

static uint64_t prog_ident(const uint64_t gid[2], const uint32_t gext[2], const ShaderKey *vk, const ShaderKey *fk) {
    uint8_t b[24 + 2 * 50], *w = b;
    wput(&w, gid, 16);
    wput(&w, gext, 8);
    put_skey(&w, vk);
    ShaderKey f = *fk;
    f.vs_out_mask = 0;
    put_skey(&w, &f);
    uint64_t h = fnv64(0xcbf29ce484222325ull, b, (size_t)(w - b));
    return h ? h : 1;
}

static Prog *warm_prog_find(const uint64_t gid[2], const uint32_t gext[2], const ShaderKey *vkey, const ShaderKey *fkey) {
    if (!warm_nprogs || !gid[0] || !gid[1]) return NULL;
    uint64_t key = prog_ident(gid, gext, vkey, fkey);
    for (uint32_t i = (uint32_t)key & (WARM_PROG_TAB - 1); warm_progs[i].key; i = (i + 1) & (WARM_PROG_TAB - 1))
        if (warm_progs[i].key == key) return atomic_load(&warm_progs[i].p->state) == 2 ? NULL : warm_progs[i].p;
    return NULL;
}

static Pipe *warm_pipe_find(uint64_t ph) {
    if (!warm_npipes) return NULL;
    for (uint32_t i = (uint32_t)(ph ^ (ph >> 29)) & (WARM_PIPE_TAB - 1); warm_pipe_tab[i].key; i = (i + 1) & (WARM_PIPE_TAB - 1))
        if (warm_pipe_tab[i].key == ph) return atomic_load(&warm_pipe_tab[i].pp->state) == 2 ? NULL : warm_pipe_tab[i].pp;
    return NULL;
}

static void warm_check_finished(void) {
    if (atomic_load(&warm_progs_done) < warm_nprogs || atomic_load(&warm_pipes_done) < warm_npipes) return;
    if (atomic_exchange(&warm_reported, true)) return;
    LOG("[warm] ready in %.1f s: %d of %d programs, %d of %d pipelines built\n", (SDL_GetTicksNS() - warm_t0) / 1e9,
        atomic_load(&warm_progs_ok), warm_nprogs, atomic_load(&warm_pipes_ok), warm_npipes);
}

void warm_pipe_done(bool ok) {
    if (ok) atomic_fetch_add(&warm_pipes_ok, 1);
    atomic_fetch_add(&warm_pipes_done, 1);
    warm_check_finished();
}

// A warm program is built (or failed): queue its recipe pipelines. One whose vertex inputs no longer match the
// translated shader (a translator change) would describe some other pipeline: dropped.
static void warm_prog_done(Prog *p) {
    if (!p->warm) return;
    bool ok = atomic_load(&p->state) == 1;
    for (int i = 0; i < p->warm_count; i++) {
        Pipe *pp = &warm_pipes[p->warm_first + i];
        if (ok && pp->k.attr_mask == p->vi->attr_mask) run_job_q(build_pipeline, pp, true);
        else { atomic_store(&pp->state, 2); warm_pipe_done(false); }
    }
    if (ok) atomic_fetch_add(&warm_progs_ok, 1);
    atomic_fetch_add(&warm_progs_done, 1);
    warm_check_finished();
}

void vk_warm_summary(VkWarmSummary *s) {
    *s = warm_read;
    s->programs_done = atomic_load(&warm_progs_done);
    s->programs_ok = atomic_load(&warm_progs_ok);
    s->pipelines_done = atomic_load(&warm_pipes_done);
    s->pipelines_ok = atomic_load(&warm_pipes_ok);
}

// UI thread (window title): true while the warmup is building
bool vk_warm_progress(int *done, int *total) {
    if (!warm_active) return false;
    *total = warm_nprogs + warm_npipes;
    *done = atomic_load(&warm_progs_done) + atomic_load(&warm_pipes_done);
    return *done < *total;
}

static void *warm_enqueue(void *a) {  // (EOWRECOMP_SYNC_SHADERS: these run inline, so not on the UI thread)
    (void)a;
    for (int i = 0; i < WARM_PROG_TAB; i++)
        if (warm_progs[i].key) run_translate_q(translate_program, warm_progs[i].p, true);
    return NULL;
}

static uint8_t *warm_code_read(const char *path, uint64_t gid, uint32_t ext) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *code = malloc(ext ? ext : 1);
    bool ok = code && fread(code, 1, ext, f) == ext && fgetc(f) == EOF;
    fclose(f);
    if (ok && warm_code_id(code, ext) != gid) ok = false;
    if (!ok) { free(code); return NULL; }
    return code;
}

static void warm_code_cache_shared(uint64_t gid, uint32_t ext, const uint8_t *code) {
    char dir[700], path[800], tmp[840];
    snprintf(dir, sizeof dir, "%s/warm", vk_cache_dir());
    mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/warm/code", vk_cache_dir());
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/%016llx-%x.bin", dir, (unsigned long long)gid, ext);
    FILE *existing = fopen(path, "rb");
    if (existing) {
        fclose(existing);
        const uint8_t *cached = warm_code_read(path, gid, ext);
        if (cached) { free((void *)cached); return; }
        remove(path);  // replace a corrupt local copy with the verified bundled code
    }
    snprintf(tmp, sizeof tmp, "%s.%llx.tmp", path, (unsigned long long)SDL_GetTicksNS());
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(code, 1, ext, f) == ext;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) remove(tmp);
}

static const uint8_t *warm_code_load(uint64_t gid, uint32_t ext) {
    static struct { uint64_t gid; uint32_t ext; const uint8_t *code; bool tried; } tab[WARM_PROG_TAB];
    uint32_t i = (uint32_t)(gid ^ ext) & (WARM_PROG_TAB - 1);
    while (tab[i].tried && (tab[i].gid != gid || tab[i].ext != ext)) i = (i + 1) & (WARM_PROG_TAB - 1);
    if (tab[i].tried) return tab[i].code;
    tab[i].tried = true;
    tab[i].gid = gid;
    tab[i].ext = ext;
    char path[1100];
    snprintf(path, sizeof path, "%s/warm/code/%016llx-%x.bin", vk_cache_dir(), (unsigned long long)gid, ext);
    uint8_t *code = warm_code_read(path, gid, ext);
    if (!code && warm_catalog_dir[0]) {
        snprintf(path, sizeof path, "%s/code/%016llx-%x.bin", warm_catalog_dir, (unsigned long long)gid, ext);
        code = warm_code_read(path, gid, ext);
        if (code) warm_code_cache_shared(gid, ext, code);
    }
    return tab[i].code = code;
}

static Prog *warm_prog_get(const uint64_t gid[2], const uint32_t gext[2], const ShaderKey *vk, const ShaderKey *fk,
                           const uint8_t *code[2]) {
    uint64_t key = prog_ident(gid, gext, vk, fk);
    uint32_t i = (uint32_t)key & (WARM_PROG_TAB - 1);
    while (warm_progs[i].key && warm_progs[i].key != key) i = (i + 1) & (WARM_PROG_TAB - 1);
    if (warm_progs[i].key) return warm_progs[i].p;
    if (warm_nprogs >= WARM_PROG_TAB / 2) return NULL;
    Prog *p = calloc(1, sizeof *p);
    for (int s = 0; s < 2; s++) {
        p->code[s] = (uint64_t)(uintptr_t)code[s];
        p->cmax[s] = gext[s];  // (translates exactly what it read from guest memory: maxwell_code_extent)
        p->gid[s] = gid[s];
        p->gext[s] = gext[s];
    }
    p->vkey = *vk;
    p->fkey = *fk;
    p->fkey.vs_out_mask = 0;
    p->warm = true;
    warm_progs[i].key = key;
    warm_progs[i].p = p;
    warm_nprogs++;
    return p;
}

typedef struct { Prog *p; PipeKey k; } WarmTmp;

// The game (ui_window_init): the recipes already saved, kept for the next vk_warm_save without building anything.
// Since the game no longer runs the warmup itself (Trirod-ShaderPrep does), a save wrote only the recipes this run
// had met: the 1133 recipes ShaderPrep had merged into the cache became 126 after one session ("[warm] saved 126
// pipeline recipes", 2026-11-24). Records of another GPU are kept too (Trirod-ShaderPrep checks each recipe's formats
// on the device it prepares for); the file is tagged for this GPU when saved.
void vk_warm_load_records(void) {
    if (warm_off()) return;
    char path[640];
    snprintf(path, sizeof path, "%s/warm/recipes.bin", vk_cache_dir());
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = size >= 32 ? malloc((size_t)size) : NULL;
    bool ok = data && fread(data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    uint32_t hv[5] = {0};
    if (ok) memcpy(hv, data + 8, sizeof hv);
    ok = ok && !memcmp(data, "EOWWARM", 8) && (hv[0] == 1 || hv[0] == WARM_SCHEMA) && hv[1] == WARM_REC;
    uint32_t kept = 0, bad = 0;
    if (ok) {
        uint32_t count = hv[4], fit = (uint32_t)((size - 32) / WARM_REC);
        if (count > fit) count = fit;
        pthread_mutex_lock(&rec_lock);
        for (uint32_t n = 0; n < count; n++) {
            const uint8_t *rec = data + 32 + (size_t)n * WARM_REC;
            uint64_t c;
            memcpy(&c, rec + WARM_PAYLOAD, 8);
            if (c != fnv64(0x84222325cbf29ce4ull, rec, WARM_PAYLOAD)) { bad++; continue; }
            kept += rec_add_locked(rec);
        }
        rec_dirty = false;  // (only new recipes make a save necessary)
        pthread_mutex_unlock(&rec_lock);
    }
    free(data);
    LOG("[warm] %u saved pipeline recipes kept for the next save%s\n", kept, ok ? "" : " (unreadable recipe file)");
    (void)bad;
}

static bool warm_load_file(const char *path, bool shared, WarmTmp **tmp, int *ntmp, int *tmpcap,
                           int *bad, int *missing, uint32_t *read_count) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = size >= 32 ? malloc((size_t)size) : NULL;
    bool read_ok = data && fread(data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    uint32_t hv[5] = {0};
    if (read_ok) memcpy(hv, data + 8, sizeof hv);
    bool local_match = (hv[2] == g_vk.props.vendorID && hv[3] == g_vk.props.deviceID);
    bool portable = hv[0] == 2 && !hv[2] && !hv[3];
    const char *why = !read_ok ? "unreadable" : memcmp(data, "EOWWARM", 8) ? "not a recipe file"
                    : (hv[0] != 1 && hv[0] != WARM_SCHEMA) || hv[1] != WARM_REC ? "unsupported recipe format"
                    : (shared ? !portable : (!local_match && !portable)) ? "recorded for another GPU" : NULL;
    if (why) {
        LOG("[warm] %s: %s; ignoring this recipe set\n", path, why);
        free(data);
        return false;
    }
    uint32_t count = hv[4], fit = (uint32_t)((size - 32) / WARM_REC);
    if (count > fit) count = fit;
    if (count > WARM_MAX) count = WARM_MAX;
    *read_count += count;
    if (shared) warm_read.shared_recipes += count;
    for (uint32_t n = 0; n < count; n++) {
        const uint8_t *rec = data + 32 + (size_t)n * WARM_REC, *r = rec;
        uint64_t c;
        memcpy(&c, rec + WARM_PAYLOAD, 8);
        uint64_t gid[2];
        uint32_t gext[2];
        ShaderKey vk, fk;
        PipeKey k;
        if (c != fnv64(0x84222325cbf29ce4ull, rec, WARM_PAYLOAD)) { (*bad)++; continue; }
        wget(&r, gid, 16);
        wget(&r, gext, 8);
        if (!gid[0] || !gid[1] || !gext[0] || !gext[1] || gext[0] > 0x10000 || gext[1] > 0x10000 ||
            !get_skey(&r, &vk) || !get_skey(&r, &fk) || !get_pkey(&r, &k)) { (*bad)++; continue; }
        pthread_mutex_lock(&rec_lock);
        bool added = rec_add_locked(rec);  // (merged into this player's device-tagged cache on save)
        if (shared && added) rec_dirty = true;
        pthread_mutex_unlock(&rec_lock);
        warm_read.flip_y[vk.flip_y ? 1 : 0]++;
        warm_read.depth_minus_one[vk.depth_minus_one ? 1 : 0]++;
        const uint8_t *code[2] = {warm_code_load(gid[0], gext[0]), warm_code_load(gid[1], gext[1])};
        if (!code[0] || !code[1]) { (*missing)++; continue; }
        Prog *p = warm_prog_get(gid, gext, &vk, &fk, code);
        if (!p) { (*missing)++; continue; }
        if (fk.stencil_tex) {
            ShaderKey base = fk;
            base.stencil_tex = 0;
            warm_prog_get(gid, gext, &vk, &base, code);
        }
        if (*ntmp >= WARM_MAX) { (*bad)++; continue; }
        if (*ntmp == *tmpcap) {
            int cap = *tmpcap ? *tmpcap * 2 : 256;
            if (cap > WARM_MAX) cap = WARM_MAX;
            WarmTmp *grown = realloc(*tmp, sizeof **tmp * cap);
            if (!grown) { (*bad)++; continue; }
            *tmp = grown;
            *tmpcap = cap;
        }
        (*tmp)[*ntmp].p = p;
        (*tmp)[(*ntmp)++].k = k;
    }
    free(data);
    return true;
}

// Device start-up (UI thread, before the game runs): load the recipes and queue the warm set.
void vk_warm_start(void) {
    if (warm_off()) { LOG("[warm] off (EOWRECOMP_VK_WARM=0)\n"); return; }
    uint64_t t0 = SDL_GetTicksNS();
    char local[700], shared[1200];
    snprintf(local, sizeof local, "%s/warm/recipes.bin", vk_cache_dir());
    snprintf(shared, sizeof shared, "%s/recipes.bin", warm_catalog_dir);
    WarmTmp *tmp = NULL;
    int ntmp = 0, tmpcap = 0, bad = 0, missing = 0;
    uint32_t count = 0;
    bool found = warm_load_file(local, false, &tmp, &ntmp, &tmpcap, &bad, &missing, &count);
    if (warm_catalog_dir[0]) found |= warm_load_file(shared, true, &tmp, &ntmp, &tmpcap, &bad, &missing, &count);
    if (!found) {
        snprintf(warm_read.why, sizeof warm_read.why, "no compatible local or bundled recipes");
        LOG("[warm] no compatible pipeline recipes yet: this run records them for the next start\n");
        free(tmp);
        return;
    }
    // pipelines grouped by program (warm_prog_done queues a program's range), each under draw_'s own key hash
    warm_pipes = calloc(ntmp ? ntmp : 1, sizeof *warm_pipes);
    for (int i = 0; i < WARM_PROG_TAB; i++) {
        Prog *p = warm_progs[i].p;
        if (!warm_progs[i].key) continue;
        p->warm_first = warm_npipes;
        for (int t = 0; t < ntmp; t++) {
            if (tmp[t].p != p) continue;
            PipeKey k = tmp[t].k;
            k.prog = p;
            uint64_t ph = hash_words(0xcbf29ce484222325ull, &k, sizeof k);
            if (!ph) ph = 1;
            uint32_t s = (uint32_t)(ph ^ (ph >> 29)) & (WARM_PIPE_TAB - 1);
            while (warm_pipe_tab[s].key && warm_pipe_tab[s].key != ph) s = (s + 1) & (WARM_PIPE_TAB - 1);
            if (warm_pipe_tab[s].key || warm_npipes >= WARM_PIPE_TAB / 2) continue;
            Pipe *pp = &warm_pipes[warm_npipes++];
            pp->k = k;
            pp->hash = ph;
            pp->warm = true;
            warm_pipe_tab[s].key = ph;
            warm_pipe_tab[s].pp = pp;
        }
        p->warm_count = warm_npipes - p->warm_first;
    }
    free(tmp);
    warm_read.found = true;
    warm_read.recipes = count;
    warm_read.programs = warm_nprogs;
    warm_read.pipelines = warm_npipes;
    warm_read.missing_code = missing;
    warm_read.invalid = bad;
    LOG("[warm] %u recipes (%u shared): %d programs and %d pipelines to build (%d without cached code, %d invalid), read in %.0f ms\n",
        count, warm_read.shared_recipes, warm_nprogs, warm_npipes, missing, bad, (SDL_GetTicksNS() - t0) / 1e6);
    if (!warm_nprogs) return;
    warm_active = true;
    warm_t0 = SDL_GetTicksNS();
    if (sync_shaders()) { pthread_t t; pthread_create(&t, NULL, warm_enqueue, NULL); }
    else warm_enqueue(NULL);
}

// renderer_stats (every two seconds): this interval's pipelines, and a save of new recipes every 30 s
void warm_stats(char *out, size_t n) {
    if (warm_off()) return;
    if (stat_new_pipes || stat_warm_pipe_hits || stat_warm_prog_hits)
        snprintf(out, n, " | pipelines: %llu new, %llu from warmup (programs %llu)", (unsigned long long)stat_new_pipes,
                 (unsigned long long)stat_warm_pipe_hits, (unsigned long long)stat_warm_prog_hits);
    stat_new_pipes = stat_warm_pipe_hits = stat_warm_prog_hits = 0;
    if (rec_dirty && SDL_GetTicksNS() - rec_saved_ns > 30000000000ull) vk_warm_save();
}

// ================================================================== feedback snapshots (renderer.m resolve_feedback)
static NvnTexture *feedback_tex[4];
static VkImageView feedback_view[4], feedback_stencil_view[4];
static TexBackend *feedback_snap[4];  // the snapshot image itself, for a 2D-array view of it (texture_info)
static VkFormat feedback_fmt[4];
static VkImageAspectFlags feedback_aspect[4];
static int nfeedback;
static uint64_t g_cur_fs_hash;
static NvnTexture *snap_tex[8];
static bool snap_ok[8];
static const char *snap_why[8];
static const char *snap_reason = "?";
static void snapshots_invalidate(NvnTexture *t) {
    for (int i = 0; i < 8; i++) if ((!t || snap_tex[i] == t) && snap_ok[i]) { snap_ok[i] = false; snap_why[i] = snap_reason; }
}

typedef struct { NvnTexture *key; TexBackend copy; } Snap;
static Snap snaps[16];
static int nsnaps;

static NvnTexture *shader_texture_object(int stage, ShaderTexture *st_) {
    int slot = ((int)st_->handle_offset - 0x20) / 8;
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    uint32_t tic = h & 0xFFFFF;
    NvnTexEntry entry;
    if (!h || !nvn_texture_entry_acquire(st.tpool, tic, &entry)) return NULL;
    return entry.tex;
}

// Return fragment texture slots whose NVN texture or view selects the stencil aspect.
// Maxwell expects the stencil value in the sampled register as raw integer bits, so these slots
// need a distinct GLSL shader variant with unsigned samplers.
static uint16_t stencil_texture_slots(int stage, const ShaderInfo *info) {
    uint16_t slots = 0;
    for (int i = 0; i < info->ntex && i < 16; i++) {
        const ShaderTexture *shader_tex = &info->tex[i];
        if (shader_tex->is_image || shader_tex->shadow || shader_tex->bindless) continue;

        int handle_slot = ((int)shader_tex->handle_offset - 0x20) / 8;
        uint64_t handle = handle_slot >= 0 && handle_slot < 32 ? st.tex[stage][handle_slot] : 0;
        uint32_t texture_index = (uint32_t)(handle & 0xFFFFF);
        if (!handle || !st.tpool || texture_index >= (uint32_t)st.tpool->count) continue;

        NvnTexEntry entry;
        if (!nvn_texture_entry_acquire(st.tpool, texture_index, &entry) || !entry.tex ||
            !renderer_format_has_stencil(entry.tex->format)) continue;
        uint32_t mode = entry.has_view && (entry.view.setmask & 16) ? entry.view.ds_mode : entry.tex->ds_mode;
        if (mode == 1 || entry.tex->format == 0x31) slots |= (uint16_t)(1u << i);
    }
    return slots;
}

static bool is_attachment(NvnTexture *t) {
    if (!t) return false;
    if (t == st.rt.depth) return true;
    for (int i = 0; i < st.rt.ncolor; i++) if (t == st.rt.color[i]) return true;
    return false;
}

static TexBackend *snapshot_image(NvnTexture *t, TexBackend *src) {
    Snap *s = NULL;
    for (int i = 0; i < nsnaps; i++) if (snaps[i].key == t) s = &snaps[i];
    if (!s) {
        if (nsnaps == 16) { static int rr; s = &snaps[rr++ & 15]; } else s = &snaps[nsnaps++];
        s->key = t;
    }
    TexBackend *d = &s->copy;
    if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format)) {
        vk_views_forget(d);  // (resolution change)
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = src->format,
                                .extent = {src->width, src->height, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = (VkSampleCountFlagBits)src->samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = src->format; d->width = src->width; d->height = src->height; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = src->samples; d->aspect = src->aspect; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
        if (getenv("EOWRECOMP_VK_VIEW_TRACE"))
            LOG("[vk] feedback snapshot created: tex=%p backend=%p image=%p fmt=%d aspect=%#x size=%ux%u\n",
                (void *)t, (void *)d, (void *)(uintptr_t)d->image, d->format, d->aspect, d->width, d->height);
    }
    return d;
}

// (debug frames) a depth image's depth values as a greyscale PGM, stretched over its own min..max
static void debug_save_depth(TexBackend *b, const char *path) {
    if (!b || !b->image || !(b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || b->format != VK_FORMAT_D32_SFLOAT_S8_UINT) return;
    uint64_t n = (uint64_t)b->width * b->height;
    VkSpan out = vk_readback(n * 4);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                           .imageExtent = {b->width, b->height, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    const float *d = out.ptr;
    float lo = 1e30f, hi = -1e30f;
    for (uint64_t i = 0; i < n; i++) { if (d[i] < lo) lo = d[i]; if (d[i] > hi) hi = d[i]; }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5 %u %u 255\n", b->width, b->height);
    for (uint64_t i = 0; i < n; i++) fputc(hi > lo ? (int)((d[i] - lo) / (hi - lo) * 255.0f) : 0, f);
    fclose(f);
    LOG("[vk] saved %s (%ux%u depth %.6f..%.6f)\n", path, b->width, b->height, lo, hi);
}

// (debug) EOWRECOMP_COPY_TRACE=<frame>: draws' attachments / sampled textures, copies and snapshots of that frame ("[ct]")
static bool ct_on(void) {
    static const char *e = (const char *)1;
    if (e == (const char *)1) e = getenv("EOWRECOMP_COPY_TRACE");
    if (!e) return false;
    static uint64_t memo_f = ~0ull;
    static bool memo;
    uint64_t f = renderer_present_count();
    if (f == memo_f) return memo;
    memo_f = f;
    memo = false;
    for (const char *p = e; *p;) {
        if ((uint64_t)strtoll(p, (char **)&p, 10) == f) { memo = true; break; }
        if (*p == ',') p++; else break;
    }
    if (memo) LOG("[ct] ===== frame %llu\n", (unsigned long long)f);
    return memo;
}
static void ct_draw(Prog *pr, const char *what) {
    LOG("[ct] %s fs %016llx rt", what, (unsigned long long)g_cur_fs_hash);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) LOG(" c%d=%p/%#x", i, (void *)st.rt.color[i], st.rt.color[i]->format);
    if (st.rt.depth) LOG(" d=%p/%#x%s", (void *)st.rt.depth, st.rt.depth->format, st.ds[0] && st.ds[1] ? "W" : "");
    LOG(" cm %08x vp %.0f %.0f %.0f %.0f sc %d %d %d %d | tex", st.channel_mask, st.viewport[0][0], st.viewport[0][1], st.viewport[0][2],
        st.viewport[0][3], st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3]);
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < (infos[sg] ? infos[sg]->ntex : 0); i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (t) LOG(" %s%p/%#x", sg ? "" : "v:", (void *)t, t->format);
        }
    LOG("\n");
}

// ================================================================== copy elision (depth copies)
// The game copies its main depth buffer whole, every frame, into textures that are then only sampled while the
// source keeps its depth (the water / transparency depth input), or that get their depth cleared right away
// (stencil kept). At 3x internal resolution each such copy is ~0.15 ms of pure bandwidth. Instead of copying, the
// destination becomes an alias: sampling it (and copying from it) reads the source's image. The alias ends before
// anything changes either side:
//   - the source is about to be overwritten whole (both aspects cleared): the two images swap - the destination
//     keeps the contents without a copy, the source is cleared in the destination's old image;
//   - the destination is about to be overwritten in some aspects (its depth cleared): only the other aspects are
//     copied (stencil, a quarter of the bytes);
//   - anything else (a draw writing depth / stencil, a partial clear, a copy or upload into either, the
//     destination bound as an attachment, a texture freed or reallocated): the real copy is made first.
// Only same-size, same-format, single-level, single-sample depth/stencil images, whole-image copies.
// EOWRECOMP_VK_COPY_OPT=0: always copy (the old behaviour).
typedef struct { NvnTexture *src, *dst; VkImage simg, dimg; } CopyAlias;
extern uint32_t vk_view_gen;
static CopyAlias aliases[8];
static int naliases;
static uint64_t stat_alias_made, stat_alias_renamed, stat_alias_partial, stat_alias_copied;

static int copy_opt(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("EOWRECOMP_VK_COPY_OPT"); on = e && *e ? atoi(e) : 1; }
    return on;
}

// still valid: both textures keep the images the alias was made with (a resolution change reallocates them)
static bool alias_valid(const CopyAlias *a) {
    const TexBackend *s = a->src->backend, *d = a->dst->backend;
    return s && d && s->image == a->simg && d->image == a->dimg;
}

static void alias_drop(int i) {
    aliases[i] = aliases[--naliases];
    vk_view_gen++;  // (exec.c's per-frame view memo: the destination's views change)
}

// the real copy of the aspects asked for, into the destination's own image
static void alias_copy(CopyAlias *a, VkImageAspectFlags aspects, const char *why) {
    TexBackend *s = a->src->backend, *d = a->dst->backend;
    aspects &= d->aspect;
    if (!aspects) return;
    vk_end_pass();
    st.rt_dirty = true;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    gp_mark(cb, "copy texture (deferred, %s%s) %ux%u", why, aspects == d->aspect ? "" : ", stencil only", a->dst->width, a->dst->height);
    VkImageCopy c = {.srcSubresource = {aspects, 0, 0, 1}, .dstSubresource = {aspects, 0, 0, 1}, .extent = {d->width, d->height, 1}};
    vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    image_transfer_write_barrier(cb, d->image, d->format, aspects, 0, 0);
    if (aspects == d->aspect) stat_alias_copied++; else stat_alias_partial++;
    if (ct_on()) LOG("[ct] alias copy %p -> %p aspects %#x (%s)\n", (void *)a->src, (void *)a->dst, aspects, why);
}

// t is about to be written; full: the aspects of it the write overwrites whole (0: unknown / partial)
static void alias_before_write(NvnTexture *t, VkImageAspectFlags full, const char *why) {
    if (!naliases || !t) return;
    for (int i = naliases - 1; i >= 0; i--) {
        CopyAlias *a = &aliases[i];
        if (!alias_valid(a)) { alias_drop(i); continue; }
        if (a->dst != t) continue;
        // (EOWRECOMP_VK_COPY_OPT=2: all aspects even then - for comparing the stencil-only copy's cost)
        alias_copy(a, ((TexBackend *)t->backend)->aspect & (copy_opt() == 2 ? ~0u : ~full), why);
        alias_drop(i);
    }
    NvnTexture *heir = NULL;  // t overwritten whole: one destination can take its image instead of a copy
    if (t->backend && (((TexBackend *)t->backend)->aspect & ~full) == 0)
        for (int i = 0; i < naliases && !heir; i++) if (aliases[i].src == t) heir = aliases[i].dst;
    for (int i = naliases - 1; i >= 0; i--) {
        CopyAlias *a = &aliases[i];
        if (a->src != t || a->dst == heir) continue;
        alias_copy(a, ((TexBackend *)a->dst->backend)->aspect, why);
        alias_drop(i);
    }
    int rename = -1;
    for (int i = 0; i < naliases; i++) if (heir && aliases[i].src == t && aliases[i].dst == heir) rename = i;
    if (rename >= 0) {
        CopyAlias *a = &aliases[rename];
        TexBackend *s = a->src->backend, *d = a->dst->backend;
        vk_end_pass();
        st.rt_dirty = true;
        VkImage im = s->image; s->image = d->image; d->image = im;
        VmaAllocation m = s->mem; s->mem = d->mem; d->mem = m;
        vk_views_forget(s);
        vk_views_forget(d);
        if (getenv("EOWRECOMP_VK_VIEW_TRACE"))
            LOG("[vk] depth alias images swapped: src=%p backend=%p image=%p fmt=%d aspect=%#x dst=%p backend=%p image=%p fmt=%d aspect=%#x\n",
                (void *)a->src, (void *)s, (void *)(uintptr_t)s->image, s->format, s->aspect,
                (void *)a->dst, (void *)d, (void *)(uintptr_t)d->image, d->format, d->aspect);
        stat_alias_renamed++;
        if (ct_on()) LOG("[ct] alias rename %p -> %p (%s)\n", (void *)a->src, (void *)a->dst, why);
        alias_drop(rename);
    }
}

// the texture whose image holds t's contents (t itself unless it is an alias)
static NvnTexture *alias_read(NvnTexture *t) {
    for (int i = 0; i < naliases; i++)
        if (aliases[i].dst == t) {
            if (alias_valid(&aliases[i])) return aliases[i].src;
            alias_drop(i);
            return t;
        }
    return t;
}

// copy_texture: a whole-image copy that can be an alias instead (true: done)
static bool alias_make(NvnTexture *src, NvnTexture *dst, TexBackend *s, TexBackend *d) {
    if (!copy_opt() || src == dst) return false;
    if (!(s->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) return false;
    if (s->format != d->format || s->width != d->width || s->height != d->height || s->aspect != d->aspect || s->levels != 1 ||
        d->levels != 1 || s->layers != 1 || d->layers != 1 || s->depth != 1 || d->depth != 1 || s->samples != 1 || d->samples != 1 ||
        s->base_type != d->base_type || s->presented || d->presented || !s->image || !d->image || !s->gpu_owned || !d->gpu_owned)
        return false;
    // the destination's previous contents (or alias) are replaced whole; a destination that is itself the source of
    // aliases gets those resolved first
    alias_before_write(dst, d->aspect, "copy dst");
    src = alias_read(src);  // (a copy of an alias: of its source)
    s = src->backend;
    if (src == dst) return true;  // (copied back onto itself)
    if (naliases == (int)(sizeof aliases / sizeof aliases[0])) return false;
    aliases[naliases++] = (CopyAlias){src, dst, s->image, d->image};
    vk_view_gen++;
    stat_alias_made++;
    return true;
}

// textures.c: t is being freed - an alias source hands its contents over first
void vk_alias_texture_destroyed(NvnTexture *t) {
    if (naliases) alias_before_write(t, 0, "freed");
}
void vk_alias_reset(void) { naliases = 0; }  // (reboot: every texture goes)

// (renderer_stats, once per stats interval) copies made aliases / renamed / partly or fully copied later
void vk_alias_stats(void) {
    static int n;
    if (!stat_alias_made || (n++ > 5 && n % 30)) return;
    LOG_VERBOSE("[vk] copy alias: %llu made, %llu renamed, %llu stencil-only, %llu copied later (live %d)\n",
        (unsigned long long)stat_alias_made, (unsigned long long)stat_alias_renamed, (unsigned long long)stat_alias_partial,
        (unsigned long long)stat_alias_copied, naliases);
    stat_alias_made = stat_alias_renamed = stat_alias_partial = stat_alias_copied = 0;
}

static bool draw_samples_texture(Prog *pr, NvnTexture *t) {
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; infos[sg] && i < infos[sg]->ntex; i++)
            if (!infos[sg]->tex[i].is_image && shader_texture_object(sg, &infos[sg]->tex[i]) == t) return true;
    return false;
}

// draws: attachments written (or a destination bound at all) end the aliases involved
static void alias_draw_check(Prog *pr) {
    if (!naliases) return;
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i]) alias_before_write(st.rt.color[i], 0, "color target");
    NvnTexture *d = st.rt.depth;
    if (!d) return;
    bool writes = (st.ds[0] && st.ds[1]) || (st.ds[3] && ((st.stencil_mask[0] | st.stencil_mask[1]) & 0xFF));
    bool is_dst = false;
    for (int i = 0; i < naliases; i++) is_dst |= aliases[i].dst == d;
    if (writes || is_dst) { alias_before_write(d, 0, writes ? "depth write" : "dst bound"); return; }
    // The source is the (read-only) depth attachment and the draw samples one of its destinations: texture_info
    // reads a destination through alias_read, i.e. the attachment's own image, while resolve_feedback and the pass
    // hazard tracking only see the destination texture - no snapshot, no pass split. Vulkan's synchronization
    // validation reported this as a WRITE_AFTER_READ against the pass's depth store (void / rift, NVIDIA). Give the
    // destination its real copy before the pass; aliases whose destinations are not sampled here stay.
    for (int i = naliases - 1; i >= 0; i--) {
        CopyAlias *a = &aliases[i];
        if (a->src != d || !alias_valid(a) || !draw_samples_texture(pr, a->dst)) continue;
        static int nlog;
        if (nlog++ < 8)
            LOG_VERBOSE("[vk] copy alias: draw samples %p while its source %p is the depth attachment; copying\n", (void *)a->dst, (void *)d);
        alias_copy(a, ((TexBackend *)a->dst->backend)->aspect, "dst sampled, src attached");
        alias_drop(i);
    }
}

static void resolve_feedback(Prog *pr) {
    nfeedback = 0;
    static int off = -1;
    if (off < 0) off = getenv("EOWRECOMP_NO_FEEDBACK") != NULL;
    if (off) return;
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < infos[sg]->ntex; i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (!is_attachment(t)) continue;
            bool have = false;
            for (int k = 0; k < nfeedback; k++) have |= feedback_tex[k] == t;
            if (have || nfeedback == 4) continue;
            static int nosnap = -1;
            if (nosnap < 0) nosnap = getenv("EOWRECOMP_NO_SNAPSHOT_REUSE") != NULL;
            int si = -1;
            for (int k = 0; k < 8; k++) if (snap_tex[k] == t) si = k;
            bool self_write = t == st.rt.depth && st.ds[0] && st.ds[1];
            TexBackend *src = vk_tex(t, true);
            Snap *existing = NULL;
            for (int k = 0; k < nsnaps; k++) if (snaps[k].key == t && snaps[k].copy.image) existing = &snaps[k];
            VkImageAspectFlags sa = src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : src->aspect;
            static const VkComponentMapping id = {0};
            if (!nosnap && si >= 0 && (snap_ok[si] || self_write) && existing) {
                feedback_tex[nfeedback] = t;
                feedback_snap[nfeedback] = &existing->copy; feedback_fmt[nfeedback] = src->format; feedback_aspect[nfeedback] = sa;
                feedback_view[nfeedback] = vk_image_view(&existing->copy, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa, "reused feedback snapshot");
                feedback_stencil_view[nfeedback++] = (src->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ?
                    vk_image_view(&existing->copy, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_STENCIL_BIT, "reused feedback stencil snapshot") : VK_NULL_HANDLE;
                continue;
            }
            stat_feedback_copies++;
            if (ct_on()) LOG("[ct] snapshot %p %ux%u (was %s, si %d)\n", (void *)t, src->width, src->height, si >= 0 ? snap_why[si] : "new", si);
            vk_end_pass();
            TexBackend *dst = snapshot_image(t, src);
            VkCommandBuffer cb = vk_cmd();
            vk_full_barrier(cb);
            gp_mark(cb, "feedback snapshot %ux%u fmt %d", src->width, src->height, src->format);
            VkImageCopy rg = {.srcSubresource = {src->aspect, 0, 0, 1}, .dstSubresource = {src->aspect, 0, 0, 1},
                              .extent = {src->width, src->height, 1}};
            vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
            image_transfer_write_barrier(cb, dst->image, dst->format, src->aspect, 0, 0);
            if (g_debug_frame && (src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
                static int nd;
                char path[128];
                snprintf(path, sizeof path, "build/fbdepth_%03d_src.pgm", nd);
                debug_save_depth(src, path);
                snprintf(path, sizeof path, "build/fbdepth_%03d_snap.pgm", nd++);
                debug_save_depth(dst, path);
                cb = vk_cmd();
            }
            feedback_tex[nfeedback] = t;
            feedback_snap[nfeedback] = dst; feedback_fmt[nfeedback] = src->format; feedback_aspect[nfeedback] = sa;
            feedback_view[nfeedback] = vk_image_view(dst, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa, "new feedback snapshot");
            feedback_stencil_view[nfeedback++] = (src->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ?
                vk_image_view(dst, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_STENCIL_BIT, "new feedback stencil snapshot") : VK_NULL_HANDLE;
            if (si < 0) { static int rr; si = rr++ & 7; snap_tex[si] = t; }
            snap_ok[si] = true;
            st.rt_dirty = true;
        }
}

// Triforce's draw_hazards equivalent. A render pass orders attachment writes with each
// other, but a sampled read of an attachment written earlier in that pass (or a write after
// an earlier draw sampled the target) needs a pass boundary and memory dependency.
static uint16_t draw_sampled_targets(Prog *pr) {
    uint16_t sampled = 0;
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++) {
        ShaderInfo *info = infos[sg];
        if (!info) continue;
        for (int i = 0; i < info->ntex && i < 64; i++) {
            if (info->tex[i].is_image) continue;  // storage images are not sampled descriptors
            NvnTexture *t = shader_texture_object(sg, &info->tex[i]);
            if (!t) continue;
            for (int c = 0; c < st.rt.ncolor && c < 8; c++)
                if (t == st.rt.color[c]) sampled |= (uint16_t)(1u << c);
            if (t == st.rt.depth) sampled |= (uint16_t)(1u << 8);
        }
    }
    return sampled;
}

static uint16_t draw_written_targets(Prog *pr) {
    uint16_t writes = 0;
    for (int c = 0; c < st.rt.ncolor && c < 8; c++) {
        uint32_t outputs = (pr->fi->out_mask >> (c * 4)) & 0xF;
        uint32_t channels = (st.channel_mask >> (c * 4)) & 0xF;
        if (st.rt.color[c] && outputs && channels) writes |= (uint16_t)(1u << c);
    }

    static int no_depth_test = -1;
    if (no_depth_test < 0) no_depth_test = getenv("EOWRECOMP_NO_DEPTH_TEST") != NULL;
    if (st.rt.depth && renderer_format_is_depth(st.rt.depth->format)) {
        bool depth_write = st.ds[0] && !no_depth_test && st.ds[1];
        bool stencil_write = false;
        if (st.ds[3] && renderer_format_has_stencil(st.rt.depth->format) &&
            ((st.stencil_mask[0] | st.stencil_mask[1]) & 0xFF)) {
            uint32_t ops = st.ds[5] | (uint32_t)st.ds[6] << 8 | (uint32_t)st.ds[7] << 16;
            for (int face = 0; face < 2 && !stencil_write; face++) {
                if (!(st.stencil_mask[face] & 0xFF)) continue;
                uint32_t o = face ? ops >> 12 : ops;
                stencil_write = stencil_op(o & 0xF) != VK_STENCIL_OP_KEEP ||
                                stencil_op((o >> 8) & 0xF) != VK_STENCIL_OP_KEEP ||
                                stencil_op((o >> 4) & 0xF) != VK_STENCIL_OP_KEEP;
            }
        }
        if (depth_write || stencil_write) writes |= (uint16_t)(1u << 8);
    }
    return writes;
}

static void check_draw_hazards(uint16_t sampled, uint16_t writes) {
    if (!in_pass || st.rt_dirty) return;  // begin_pass already closes the old targets with a barrier

    uint16_t read_after_write = sampled & pass_written_targets & (uint16_t)~writes;
    uint16_t write_after_read = writes & pass_sampled_targets;
    if (!read_after_write && !write_after_read) return;

    static unsigned logged;
    if (logged++ < 12) {
        const char *kind = read_after_write ? "samples a target written earlier in this pass" :
                                              "writes a target sampled earlier in this pass";
        LOG("[vk] hazard: %s (draw %u, read %#x write %#x); splitting pass with a GPU memory dependency\n",
            kind, pass_draw_count, sampled, writes);
    }
    vk_end_pass();
}

// ================================================================== resource binding
static VkDescriptorBufferInfo ubo_span(VkSpan s, uint64_t size) {
    uint64_t max = g_vk.props.limits.maxUniformBufferRange;
    return (VkDescriptorBufferInfo){s.buf, s.off, size > max ? max : size};
}

// Constant buffer k of a stage (renderer.m bind_cbufs), copied into transient memory.
static VkDescriptorBufferInfo cbuf_info(int stage, ShaderInfo *si, int k, uint32_t gx, uint32_t gy, uint32_t gz) {
    static const uint8_t zeros[0x10000];
    if (k == 0) {
        uint32_t drv[0x800 / 4] = {0};
        if (stage == 5) {
            drv[0x200 / 4] = gx; drv[0x204 / 4] = gy; drv[0x208 / 4] = gz;
            for (int i = 0; i < 16; i++) {
                uint32_t *u = &drv[(0x210 + 16 * i) / 4];
                u[0] = (uint32_t)st.ubuf[5][i].addr; u[1] = (uint32_t)(st.ubuf[5][i].addr >> 32); u[2] = (uint32_t)st.ubuf[5][i].size;
                uint32_t *e = &drv[(0x310 + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[5][i].addr; e[1] = (uint32_t)(st.sbuf[5][i].addr >> 32); e[2] = (uint32_t)st.sbuf[5][i].size;
            }
        } else {
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            for (int i = 0; i < 16; i++) {
                uint32_t *e = &drv[(base + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[stage][i].addr;
                e[1] = (uint32_t)(st.sbuf[stage][i].addr >> 32);
                e[2] = (uint32_t)st.sbuf[stage][i].size;
            }
        }
        uint64_t size = si->cbuf_size[0] > sizeof drv ? sizeof drv : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
        if (size < 0x500 && stage == 5) size = 0x500;
        return ubo_span(copy_dedupe(stage == 5 ? DEDUPE_DRV_C : stage == 1 ? DEDUPE_DRV_F : DEDUPE_DRV_V, drv, size, 256), size);
    }
    if (k == 1) {  // compiler constants stored after the shader code
        NvnShaderStage *ss = &st.program->stages[stage];
        uint64_t size = ss->const_size ? ss->const_size : 16;
        if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
        return ubo_span(guest_copy(ss->code_addr + ss->const_offset, size), size);  // (constant: shared per submission)
    }
    int idx = k - 3;  // cbuf_base
    Range r = (idx >= 0 && idx < 18) ? st.ubuf[stage][idx] : (Range){0, 0};
    uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
    if (!r.addr) return ubo_span(vk_transient_copy(zeros, size, 256), size);
    if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;
    if (size > r.size && r.size) size = r.size;
    if (size < 16) size = 16;
    // Diagnostic A/B test for the EoW void composite. Override only shader CB12[2].y
    // for this fragment shader, and only in the transient Vulkan copy. Guest memory stays untouched.
    if (stage == 1 && k == 12 && r.addr && size >= 0x28 && st.program &&
        st.program->stages[1].code_addr == 0x72a71a8500ull && getenv("EOWRECOMP_DEBUG_FORCE_VOID_FLAG")) {
        uint8_t *copy = (uint8_t *)malloc((size_t)size);
        if (copy) {
            memcpy(copy, (const void *)(uintptr_t)r.addr, (size_t)size);
            const uint32_t enabled = 1;
            memcpy(copy + 0x24, &enabled, sizeof enabled);
            VkSpan forced = vk_transient_copy(copy, size, 256);
            free(copy);
            LOG("[debug] forced void fragment shader CB12[2].y to 1 for code %llx\n",
                (unsigned long long)st.program->stages[1].code_addr);
            return ubo_span(forced, size);
        }
    }
    // snapshot: later CPU writes must not leak in. Shared by the draws of one submission binding the same range:
    // within a command list nothing writes guest memory (no inline UpdateUniformBuffer: that writes through below
    // and drops the shared copies).
    return ubo_span(guest_copy(r.addr, size), size);
}

// Global memory (shader LDG/STG, maxwell_gmem_base = HEAP_BASE): the guest heap imported as a GPU buffer
// (VK_EXT_external_memory_host), zero-copy like metal.m's g_guest_buffer, so compute results land in guest
// memory. The import pins its pages, so it covers only the heap the game set up (SetHeapSize), made on first use.
// EOWRECOMP_VK_NO_GMEM=1: no import, reads return zero.
static VkBuffer gmem_buf;
static VkDeviceMemory gmem_mem;
static uint64_t gmem_size;

static bool gmem_debug_off(void) {
    static int off = -1;
    if (off < 0) off = getenv("EOWRECOMP_VK_NO_GMEM") != NULL;
    return off;
}

static bool gmem_import(uint64_t size) {
    PFN_vkGetMemoryHostPointerPropertiesEXT props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT mp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (!props || props(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)(uintptr_t)HEAP_BASE, &mp) != VK_SUCCESS ||
        !mp.memoryTypeBits)
        return false;
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = (void *)(uintptr_t)HEAP_BASE};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size,
                               .memoryTypeIndex = (uint32_t)__builtin_ctz(mp.memoryTypeBits)};
    VkDeviceMemory mem;
    if (vkAllocateMemory(g_vk.dev, &ai, NULL, &mem) != VK_SUCCESS) return false;
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    VkBuffer buf;
    if (vkCreateBuffer(g_vk.dev, &bi, NULL, &buf) != VK_SUCCESS) { vkFreeMemory(g_vk.dev, mem, NULL); return false; }
    vkBindBufferMemory(g_vk.dev, buf, mem, 0);
    if (gmem_buf) {  // the heap grew: the old import may still be in use by submitted work
        vk_wait_seq(submit_seq);
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
    }
    gmem_buf = buf;
    gmem_mem = mem;
    gmem_size = size;
    LOG("[vk] guest heap imported for shader global memory: %llu MB\n", (unsigned long long)(size >> 20));
    return true;
}

static uint64_t gmem_tried;  // heap size the import was last attempted for (gmem_info imports / re-imports)

static VkDescriptorBufferInfo gmem_info(void) {
    static int off = -1;
    if (off < 0) off = gmem_debug_off() || !g_vk.host_import;
    uint64_t want = (kernel_heap_size() + 0xFFFF) & ~0xFFFFull;
    if (want > HEAP_MAX) want = HEAP_MAX;
    if (!off && want > gmem_size && !gmem_import(want)) {
        LOG("[vk] guest heap import failed (%llu MB): shader global memory reads return zero\n", (unsigned long long)(want >> 20));
        off = 1;
    }
    if (gmem_buf) return (VkDescriptorBufferInfo){gmem_buf, 0, gmem_size};
    static uint32_t zero[1024];
    VkSpan s = vk_transient_copy(zero, sizeof zero, 256);
    return (VkDescriptorBufferInfo){s.buf, s.off, sizeof zero};
}

// Compute dispatches that use global memory: the imported heap is host memory across PCIe on a discrete GPU, and a
// shader storing to it in a loop crawls (the little chain chomps in Madam MeowMeow's house: ~8 ms a frame on a test GPU
// Super and on an test GPU alike, which the game then waits for on its fence - 49-53 fps there on the 1660). Before
// such a dispatch its bound storage buffers (sorted, merged) are copied into a device-local buffer, the address table
// (binding GMAP_BUFFER_INDEX) tells the shader (translate.c, GLSL compute) where each range sits, loads and stores
// inside them use that copy, and a shader that stores gets the ranges copied back to the heap right after it (the
// game alternates its buffer sets per frame: the CPU isn't writing them meanwhile). Accesses outside the table use
// the heap as before. EOWRECOMP_VK_GMEM_FAST=0: no copy (empty table: everything goes to the heap, the old behaviour);
// off by default on unified-memory GPUs (gfast_prepare).
#define GFAST_SIZE (32ull << 20)
static VkBuffer gfast_buf;
static VmaAllocation gfast_mem;
static uint32_t gmap_tab[(GMAP_MAX + 1) * 4];  // this dispatch's table (stage_set copies it into binding 20)
static VkBufferCopy gfast_cp[GMAP_MAX];       // its copies heap -> gfast_buf (reversed after a storing dispatch)
static int gfast_n;
// Without VK_EXT_external_memory_host, stage each compute dispatch's bounded heap ranges in coherent memory.
// A storing dispatch is waited out before only its bound bytes are copied back to guest memory.
static VkSpan gfast_host;
static uint64_t gfast_host_size;
static VkBufferCopy gfast_host_back[16];
static int gfast_host_n;

static VkDescriptorBufferInfo gfast_info(void) {
    if (gfast_host.buf) return (VkDescriptorBufferInfo){gfast_host.buf, gfast_host.off, gfast_host_size};
    if (!gfast_buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = GFAST_SIZE,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &gfast_buf, &gfast_mem, NULL));
    }
    return (VkDescriptorBufferInfo){gfast_buf, 0, GFAST_SIZE};
}

static void gfast_prepare(VkCommandBuffer cb) {
    static int on = -1;
    // (default: discrete GPUs only - with unified memory (Apple Silicon, phones, integrated GPUs) the heap import is
    // the GPU's own memory already, and the copies would only add work)
    if (on < 0) {
        const char *e = getenv("EOWRECOMP_VK_GMEM_FAST");
        on = e ? atoi(e) != 0 : g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    }
    memset(gmap_tab, 0, sizeof gmap_tab);
    gfast_n = 0;
    gfast_host = (VkSpan){0};
    gfast_host_n = 0;
    gmem_info();  // (the heap import, made on first use)
    bool host = !gmem_buf && !gmem_debug_off();
    if (!host && !on) return;
    uint64_t heap = host ? kernel_heap_size() : gmem_size;
    if (heap > HEAP_MAX) heap = HEAP_MAX;
    struct { uint64_t lo, hi; } r[16];
    int n = 0;
    for (int i = 0; i < 16; i++) {  // heap offsets of the bound storage buffers, 256-aligned
        uint64_t a = st.sbuf[5][i].addr, sz = st.sbuf[5][i].size;
        if (!a || !sz || a < HEAP_BASE || a - HEAP_BASE > heap || sz > heap - (a - HEAP_BASE)) continue;
        uint64_t lo = (a - HEAP_BASE) & ~255ull, hi = (a - HEAP_BASE + sz + 255) & ~255ull;
        if (hi > heap) hi = heap;
        int k = n++;
        while (k > 0 && r[k - 1].lo > lo) { r[k] = r[k - 1]; k--; }  // (insertion sort by start)
        r[k].lo = lo; r[k].hi = hi;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {  // merge overlapping / nearby ranges (a small gap costs less than another table entry)
        if (m && r[i].lo <= r[m - 1].hi + 4096) { if (r[i].hi > r[m - 1].hi) r[m - 1].hi = r[i].hi; }
        else r[m++] = r[i];
    }
    VkBufferCopy cp[16];
    uint64_t off = 0;
    for (int i = 0; i < m; i++) {
        uint64_t len = r[i].hi - r[i].lo;
        if (m > GMAP_MAX || off + len > GFAST_SIZE || off + len > g_vk.props.limits.maxStorageBufferRange) return;
        cp[i] = (VkBufferCopy){r[i].lo, off, len};
        gmap_tab[i * 4 + 0] = (uint32_t)r[i].lo;
        gmap_tab[i * 4 + 1] = (uint32_t)len;
        gmap_tab[i * 4 + 2] = (uint32_t)(off >> 2);
        off += (len + 255) & ~255ull;
    }
    if (!m) return;
    if (host) {
        gfast_host = vk_transient(off, 256);
        gfast_host_size = off;
        for (int i = 0; i < m; i++)
            memcpy((uint8_t *)gfast_host.ptr + cp[i].dstOffset, (const void *)(uintptr_t)(HEAP_BASE + cp[i].srcOffset), (size_t)cp[i].size);
        // The aligned ranges may include unrelated bytes. Copy back only the original storage-buffer bindings.
        for (int i = 0; i < 16; i++) {
            Range b = st.sbuf[5][i];
            if (!b.addr || !b.size || b.addr < HEAP_BASE || b.addr - HEAP_BASE > heap || b.size > heap - (b.addr - HEAP_BASE)) continue;
            uint64_t start = b.addr - HEAP_BASE;
            for (int j = 0; j < m; j++)
                if (start >= cp[j].srcOffset && start + b.size <= cp[j].srcOffset + cp[j].size) {
                    gfast_host_back[gfast_host_n++] = (VkBufferCopy){cp[j].dstOffset + start - cp[j].srcOffset, start, b.size};
                    break;
                }
        }
        gmap_tab[GMAP_MAX * 4] = (uint32_t)m;
        vk_full_barrier(cb);
        static bool logged;
        if (!logged) { LOG("[vk] compute global memory: coherent staged storage with copy-back (no host import)\n"); logged = true; }
        return;
    }
    gfast_info();
    if (getenv("EOWRECOMP_LOG_GFAST")) {  // (debug) =<shader code address hex>: that shader's first tables
        static int nlog;
        if (st.program->stages[5].code_addr == strtoull(getenv("EOWRECOMP_LOG_GFAST"), NULL, 16) && nlog++ < 40) {
            LOG("[gfast] cs %llx: %d ranges, %llu KB:", (unsigned long long)st.program->stages[5].code_addr, m, (unsigned long long)(off >> 10));
            for (int i = 0; i < m; i++) LOG(" %llx+%llx", (unsigned long long)(r[i].lo + HEAP_BASE), (unsigned long long)(r[i].hi - r[i].lo));
            LOG("\n");
        }
    }
    vkCmdCopyBuffer(cb, gmem_buf, gfast_buf, (uint32_t)m, cp);
    vk_full_barrier(cb);
    gmap_tab[GMAP_MAX * 4] = (uint32_t)m;
    memcpy(gfast_cp, cp, (size_t)m * sizeof cp[0]);
    gfast_n = m;
}

// After a storing dispatch: its ranges back to the heap (before anything else reads them: draws, the CPU after the fence).
static void gfast_finish(VkCommandBuffer cb) {
    if (gfast_host.buf) {
        VkMemoryBarrier ready = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                  .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &ready, 0, NULL, 0, NULL);
        renderer_flush();  // later guest-memory reads must see this dispatch's results
        for (int i = 0; i < gfast_host_n; i++) {
            VkBufferCopy b = gfast_host_back[i];
            memcpy((void *)(uintptr_t)(HEAP_BASE + b.dstOffset), (uint8_t *)gfast_host.ptr + b.srcOffset, (size_t)b.size);
        }
        guest_copy_flush();
        st.rt_dirty = true;
        gfast_host = (VkSpan){0};
        gfast_host_n = 0;
        return;
    }
    if (!gfast_n) return;
    VkBufferCopy back[GMAP_MAX];
    for (int i = 0; i < gfast_n; i++) back[i] = (VkBufferCopy){gfast_cp[i].dstOffset, gfast_cp[i].srcOffset, gfast_cp[i].size};
    vk_full_barrier(cb);
    vkCmdCopyBuffer(cb, gfast_buf, gmem_buf, (uint32_t)gfast_n, back);
    vk_full_barrier(cb);
    gfast_n = 0;
}

// ---- vertex / index cache
// Reading vertex / index data in place from the imported heap crosses PCIe for every vertex fetched on a discrete GPU
// (outdoors at 3x internal resolution on an test GPU: ~0.7-1.9 ms of GPU a frame; more on PCIe 3.0), and copying it
// per draw instead moves about as much onto the CPU. So each range bound for drawing gets a device-local copy, refreshed
// only where the CPU wrote since (the OS's page write tracking, plat_written_pages, asked at submission: vc_flush): one
// sequential GPU copy of the written pages in the submission's upload buffer (which completes before its draws) instead
// of fetches across the bus. Overlapping ranges are separate entries; the per-page generation keeps them all current.
// Pages the GPU writes itself (a compute dispatch's copy-back, gfast_finish) are never cached: draws over them read the
// heap in place as before, and so does everything when the OS gives no write tracking. EOWRECOMP_VK_VCACHE=0: off.
#define VC_PAGE 4096ull
#define VC_TAB 8192                   // entries (open addressing)
#define VC_MAX_RANGE (64ull << 20)    // larger bound ranges: read in place
#define VC_BUDGET (768ull << 20)      // device-local bytes kept; the least recently used go first
typedef struct { uint64_t lo, hi, used_seq; uint32_t gen, epoch; VkBuffer buf; VmaAllocation mem; bool gpu; } VcEnt;
static VcEnt vc_tab[VC_TAB];
static uint32_t vc_pend[VC_TAB];  // entries the pending submission draws from (vc_flush brings them up to date)
static int vc_npend;
static uint32_t *vc_page_gen;  // per heap page: the generation of its last CPU write seen (vc_gen when observed)
static uint8_t *vc_page_gpu;   // per heap page: written by the GPU
static uint32_t vc_gen = 1, vc_epoch = 1;  // (epoch: bumped when pages turn GPU-written: entries check again)
static uint64_t vc_bytes;
#define VC_DEAD 1024
static struct { VkBuffer b; VmaAllocation m; uint64_t seq; } vc_dead[VC_DEAD];
static int vc_ndead;
static int vc_on = -1;
// (profiler: draws served / read in place, bytes refreshed, write tracking queries, written pages seen, query time)
uint64_t prof_vc_hits, prof_vc_bypass, prof_vc_copied, prof_vc_queries, prof_vc_scan_pages, prof_vc_scan_ns;

static void vc_free_dead(bool all) {
    uint64_t done = vk_completed_seq();
    int k = 0;
    for (int i = 0; i < vc_ndead; i++) {
        if (!all && vc_dead[i].seq > done) { vc_dead[k++] = vc_dead[i]; continue; }
        vmaDestroyBuffer(g_vk.vma, vc_dead[i].b, vc_dead[i].m);
    }
    vc_ndead = k;
}

static bool vc_drop(VcEnt *e) {  // (freed once the submissions that may read it are done)
    if (vc_ndead == VC_DEAD) { vk_wait_seq(vk_pending_seq() - 1); vc_free_dead(false); }
    if (vc_ndead == VC_DEAD) return false;  // (all read by the pending submission: keep it)
    vc_dead[vc_ndead].b = e->buf; vc_dead[vc_ndead].m = e->mem; vc_dead[vc_ndead++].seq = e->used_seq;
    vc_bytes -= e->hi - e->lo;
    memset(e, 0, sizeof *e);
    return true;
}

// The page tables (vc_page_gen / vc_page_gpu) exist whenever the OS gives write tracking: the vertex cache and the
// texture upload checks (vk_pages_changed) share them. Each write tracking query resets the OS bits, so every query
// stamps the pages it saw written with a new generation, and each user compares against the generation it last saw.
static int pt_state = -1;  // 1: tracking, 0: none
static bool pt_init(void) {
    if (pt_state >= 0) return pt_state;
    uint64_t pg = VC_PAGE, one;
    int n = plat_written_pages(HEAP_BASE, VC_PAGE, &one, 1, &pg);  // (is there write tracking?)
    pt_state = n >= 0 && pg == VC_PAGE;
    if (!pt_state) { LOG("[vk] no page write tracking (%d, page %llu)\n", n, (unsigned long long)pg); return false; }
    vc_page_gen = calloc(HEAP_MAX / VC_PAGE, sizeof *vc_page_gen);
    vc_page_gpu = calloc(HEAP_MAX / VC_PAGE, 1);
    vc_page_gen[0] = ++vc_gen;  // (the probe took page 0's tracking)
    return true;
}

// Texture upload checks (textures.c): may guest memory [addr, addr + size) have changed since generation `since`?
// 0: no CPU write since, 1: written, -1: unknown (no tracking, outside the heap, or pages the GPU writes).
// *now: the generation to pass next time.
int vk_pages_changed(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now) {
    if (!size || addr < HEAP_BASE || addr - HEAP_BASE + size > HEAP_MAX || !pt_init()) return -1;
    uint64_t lo = (addr - HEAP_BASE) & ~(VC_PAGE - 1), hi = (addr - HEAP_BASE + size + VC_PAGE - 1) & ~(VC_PAGE - 1);
    static uint64_t pages[4096];
    uint64_t pg = VC_PAGE;
    int n = plat_written_pages(HEAP_BASE + lo, hi - lo, pages, 4096, &pg);
    if (n < 0) return -1;
    uint32_t g = ++vc_gen;
    if (n >= 4096) for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) vc_page_gen[p] = g;
    else for (int k = 0; k < n; k++) vc_page_gen[(pages[k] - HEAP_BASE) / VC_PAGE] = g;
    *now = g;
    int changed = 0;
    for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) {
        if (vc_page_gpu[p]) return -1;
        if (vc_page_gen[p] > since) changed = 1;
    }
    return changed;
}

// Heap pages [lo, hi) (offsets) written by the GPU: never cached from now on.
static void vc_mark_gpu(uint64_t lo, uint64_t hi) {
    if (!pt_init()) return;
    for (uint64_t p = lo / VC_PAGE; p < (hi + VC_PAGE - 1) / VC_PAGE && p < HEAP_MAX / VC_PAGE; p++) vc_page_gpu[p] = 1;
    vc_epoch++;
}

// A storing shader (stage 0 / 1 / 5) may write any of its bound storage buffers.
static void vc_mark_sbufs(int stage) {
    for (int i = 0; i < 16; i++) {
        uint64_t a = st.sbuf[stage][i].addr, sz = st.sbuf[stage][i].size;
        if (a >= HEAP_BASE && sz && a - HEAP_BASE < HEAP_MAX) vc_mark_gpu(a - HEAP_BASE, a - HEAP_BASE + sz);
    }
}

// ---- GPU stores into the imported heap, read back by the CPU's snapshots
// Constant buffers are copied from guest memory on the CPU while the draw / dispatch is recorded (cbuf_info,
// guest_copy). With the heap import a storing shader (STG) writes guest memory only when the GPU runs it, so a
// later draw in the same frame that binds that memory as a uniform buffer got the bytes from before the dispatch.
// Without the import (the staged path, gfast_finish; also every RenderDoc capture, which doesn't expose the
// extension) the dispatch is waited out and copied back first - and there the void / rift effect drew, while live
// it stayed fully transparent. So: the bound storage ranges of each storing shader are remembered until their
// submission completes, and a draw / dispatch about to snapshot an overlapping constant buffer waits for the GPU
// first (before its render pass starts: the submission ends the pass). EOWRECOMP_VK_GSTORE_SYNC=0: off (A/B test).
#define GSTORE_MAX 64
static struct { uint64_t lo, hi; } gstore[GSTORE_MAX];  // guest addresses [lo, hi)
static int ngstore;
static bool gstore_all;      // more ranges than the table holds: any heap snapshot waits
static uint64_t gstore_seq;  // the submission holding the latest of them

static bool gstore_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("EOWRECOMP_VK_GSTORE_SYNC"); on = !e || atoi(e) != 0; }
    return on;
}

// After recording a storing shader of `stage`: its bound storage buffers may be written by the GPU from now on.
// (EOWRECOMP_VK_TRACE_FS) the last storage ranges compute dispatches with global stores had bound, and which shader /
// frame: the trace names the dispatch that may have produced a draw's storage-buffer data.
static struct { uint64_t lo, hi, cs, frame; } gwriter[64];
static unsigned gwriter_n;
static void gwriter_note(uint64_t cs) {
    extern uint64_t vk_frame_counter(void);
    for (int i = 0; i < 16; i++) {
        uint64_t a = st.sbuf[5][i].addr, sz = st.sbuf[5][i].size;
        if (!a || !sz) continue;
        unsigned k = gwriter_n++ & 63;
        gwriter[k].lo = a; gwriter[k].hi = a + sz; gwriter[k].cs = cs; gwriter[k].frame = vk_frame_counter();
    }
}

static void gstore_note(int stage) {
    if (!gmem_buf || !gstore_on()) return;  // (staged path: gfast_finish waits and copies back itself)
    for (int i = 0; i < 16; i++) {
        uint64_t a = st.sbuf[stage][i].addr, sz = st.sbuf[stage][i].size;
        if (a < HEAP_BASE || !sz || a - HEAP_BASE >= gmem_size) continue;
        bool merged = false;
        for (int k = 0; k < ngstore && !merged; k++)
            if (a <= gstore[k].hi && a + sz >= gstore[k].lo) {
                if (a < gstore[k].lo) gstore[k].lo = a;
                if (a + sz > gstore[k].hi) gstore[k].hi = a + sz;
                merged = true;
            }
        if (merged) continue;
        if (ngstore == GSTORE_MAX) gstore_all = true;
        else { gstore[ngstore].lo = a; gstore[ngstore].hi = a + sz; ngstore++; }
    }
    gstore_seq = vk_pending_seq();
    guest_copy_flush();  // (copies shared within this submission were made before the store)
}

static bool gstore_pending(void) {
    if (!ngstore && !gstore_all) return false;
    if (vk_completed_seq() >= gstore_seq) { ngstore = 0; gstore_all = false; return false; }
    return true;
}

// The constant buffers `si` (stage) is about to snapshot: wait for the GPU if one overlaps a pending store.
static void gstore_sync_cbufs(int stage, const ShaderInfo *si) {
    if (!si || !gstore_pending()) return;
    for (int k = 3; k < 18; k++) {
        if (!(si->cbuf_mask & (1u << k))) continue;
        Range r = st.ubuf[stage][k - 3];
        uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
        if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;
        if (r.size && size > r.size) size = r.size;
        if (!r.addr || r.addr < HEAP_BASE) continue;
        bool hit = gstore_all;
        for (int i = 0; i < ngstore && !hit; i++) hit = r.addr < gstore[i].hi && r.addr + size > gstore[i].lo;
        if (!hit) continue;
        static int nlog;
        if (nlog++ < 16)
            LOG_VERBOSE("[vk] gpu-stored memory read as a constant buffer: stage %d c%d %llx+%llx (%s %llx); waiting for the GPU\n",
                stage, k, (unsigned long long)r.addr, (unsigned long long)size, stage == 5 ? "cs" : stage ? "fs" : "vs",
                (unsigned long long)(st.program ? st.program->stages[stage].code_addr : 0));
        stat_gstore_waits++;
        wait_why = "GPU-stored memory read as a constant buffer";
        renderer_flush();  // (ends the pass; the caller begins its own afterwards)
        st.rt_dirty = true;
        ngstore = 0;
        gstore_all = false;
        return;
    }
}

static bool vcache_lookup(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    if (vc_on < 0) {
        const char *e = getenv("EOWRECOMP_VK_VCACHE");
        vc_on = !e || atoi(e) != 0;
        if (vc_on && !pt_init()) {
            LOG("[vk] vertex cache off: no page write tracking\n");
            vc_on = 0;
        }
    }
    if (!vc_on || !gmem_buf || size > VC_MAX_RANGE) return false;
    uint64_t lo = (addr - HEAP_BASE) & ~(VC_PAGE - 1), hi = (addr - HEAP_BASE + size + VC_PAGE - 1) & ~(VC_PAGE - 1);
    if (hi > gmem_size) return false;
    uint64_t pend = vk_pending_seq();
    uint64_t h = (lo * 0x9E3779B97F4A7C15ull) ^ (hi * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (VC_TAB - 1), free_slot = ~0u, old_slot = ~0u;
    VcEnt *e = NULL;
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VC_TAB - 1)) {
        VcEnt *c = &vc_tab[slot];
        if (c->buf && c->lo == lo && c->hi == hi) { e = c; break; }
        if (!c->buf && free_slot == ~0u) free_slot = slot;
        if (c->buf && c->used_seq < pend && (old_slot == ~0u || c->used_seq < vc_tab[old_slot].used_seq)) old_slot = slot;
    }
    if (!e) {  // a new range: its own device-local buffer (gen 0: copied whole at the submission)
        if (vc_npend == VC_TAB) return false;
        if (vc_ndead) vc_free_dead(false);
        if (free_slot == ~0u) {
            if (old_slot == ~0u || !vc_drop(&vc_tab[old_slot])) return false;
            free_slot = old_slot;
        }
        while (vc_bytes + (hi - lo) > VC_BUDGET) {  // over budget: the least recently used entry goes
            int lru = -1;
            for (int i = 0; i < VC_TAB; i++)
                if (vc_tab[i].buf && vc_tab[i].used_seq < pend && (lru < 0 || vc_tab[i].used_seq < vc_tab[lru].used_seq)) lru = i;
            if (lru < 0 || !vc_drop(&vc_tab[lru])) return false;
        }
        e = &vc_tab[free_slot];
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = hi - lo,
                                 .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &e->buf, &e->mem, NULL) != VK_SUCCESS) { memset(e, 0, sizeof *e); return false; }
        e->lo = lo; e->hi = hi;
        vc_bytes += hi - lo;
    }
    if (e->epoch != vc_epoch) {  // (pages turned GPU-written since: is this range one of them?)
        e->gpu = false;
        for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE && !e->gpu; p++) e->gpu = vc_page_gpu[p];
        e->epoch = vc_epoch;
    }
    if (e->gpu) { prof_vc_bypass++; return false; }
    if (e->used_seq != pend) {  // first use in this submission: brought up to date by vc_flush before it runs
        e->used_seq = pend;
        vc_pend[vc_npend++] = (uint32_t)(e - vc_tab);
    }
    prof_vc_hits++;
    *buf = e->buf;
    *off = addr - HEAP_BASE - lo;
    return true;
}

static int vc_pend_cmp(const void *a, const void *b) {
    uint64_t x = vc_tab[*(const uint32_t *)a].lo, y = vc_tab[*(const uint32_t *)b].lo;
    return x < y ? -1 : x > y;
}

// At submission (vk_submit_extra, before the upload buffer closes): the pages the CPU wrote in the ranges this
// submission draws from - one write tracking query per run of nearby ranges (a query costs about as much as scanning
// ~64 pages: ranges closer than that share one) - and their copies into the cache, ahead of the draws.
void vc_flush(void) {
    if (!vc_npend) return;
    qsort(vc_pend, (size_t)vc_npend, sizeof vc_pend[0], vc_pend_cmp);
    uint32_t g = ++vc_gen;
    static uint64_t pages[4096];
    uint64_t t0 = SDL_GetTicksNS();
    for (int i = 0; i < vc_npend;) {
        uint64_t lo = vc_tab[vc_pend[i]].lo, hi = vc_tab[vc_pend[i]].hi;
        int j = i + 1;
        for (; j < vc_npend && vc_tab[vc_pend[j]].lo <= hi + 64 * VC_PAGE; j++)
            if (vc_tab[vc_pend[j]].hi > hi) hi = vc_tab[vc_pend[j]].hi;
        uint64_t pg = VC_PAGE;
        int n = plat_written_pages(HEAP_BASE + lo, hi - lo, pages, 4096, &pg);
        prof_vc_queries++;
        if (n < 0 || n >= 4096) for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) vc_page_gen[p] = g;  // (all)
        else for (int k = 0; k < n; k++) vc_page_gen[(pages[k] - HEAP_BASE) / VC_PAGE] = g;
        if (n > 0) prof_vc_scan_pages += (uint64_t)n;
        i = j;
    }
    prof_vc_scan_ns += SDL_GetTicksNS() - t0;
    VkCommandBuffer cb = vk_upload_cmd();
    for (int i = 0; i < vc_npend; i++) {
        VcEnt *e = &vc_tab[vc_pend[i]];
        VkBufferCopy cp[64];
        int nc = 0;
        for (uint64_t p = e->lo / VC_PAGE; p < e->hi / VC_PAGE; p++) {
            if (e->gen && vc_page_gen[p] <= e->gen) continue;
            uint64_t o = p * VC_PAGE;
            if (nc && cp[nc - 1].srcOffset + cp[nc - 1].size == o) { cp[nc - 1].size += VC_PAGE; continue; }
            if (nc == 64) { nc = 1; cp[0] = (VkBufferCopy){e->lo, 0, e->hi - e->lo}; break; }  // (scattered: the whole range)
            cp[nc++] = (VkBufferCopy){o, o - e->lo, VC_PAGE};
        }
        if (nc) {
            vkCmdCopyBuffer(cb, gmem_buf, e->buf, (uint32_t)nc, cp);
            for (int k = 0; k < nc; k++) prof_vc_copied += cp[k].size;
        }
        e->gen = g;
    }
    vc_npend = 0;
}

// Vertex / index data from the imported guest heap (renderer.m bind_guest reads g_guest_buffer the same way): the
// device-local cache above, else read in place - no per-draw copy, no index scan for the copy size. The game fences its
// CPU writes as on the Switch. Falls back to copies outside the heap or without the import; EOWRECOMP_VK_COPY_VERTICES=1
// forces copies.
static bool guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    static int copy = -1;
    if (copy < 0) copy = getenv("EOWRECOMP_VK_COPY_VERTICES") != NULL;
    if (copy || addr < HEAP_BASE) return false;
    if (addr + size > HEAP_BASE + gmem_size && kernel_heap_size() != gmem_tried) { gmem_tried = kernel_heap_size(); gmem_info(); }
    if (!gmem_buf || addr + size > HEAP_BASE + gmem_size) return false;
    if (vcache_lookup(addr, size, buf, off)) return true;
    *buf = gmem_buf;
    *off = addr - HEAP_BASE;
    return true;
}

static int g_cs_n;  // (compute program table below)

// In-place reboot (device.c backend_reset; GPU idle): drop what is keyed by guest addresses - programs by code
// address (the old Prog / Pipe objects are leaked, so pipeline keys holding their pointers can't collide), compute
// programs, shared copies - and the heap import: its pages are replaced (re-imported on first use).
void vk_exec_reset(void) {
    memset(prog_tab, 0, sizeof prog_tab);
    memset(pipe_tab, 0, sizeof pipe_tab);
    g_cs_n = 0;
    gcopy_gen++;
    if (gmem_buf) {
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
        gmem_buf = VK_NULL_HANDLE;
        gmem_mem = VK_NULL_HANDLE;
    }
    gmem_size = gmem_tried = 0;
    ngstore = 0;  // (pending GPU stores: the old heap's)
    gstore_all = false;
    for (int i = 0; i < VC_TAB; i++)  // (the vertex cache: copies of the old pages)
        if (vc_tab[i].buf) vmaDestroyBuffer(g_vk.vma, vc_tab[i].buf, vc_tab[i].mem);
    memset(vc_tab, 0, sizeof vc_tab);
    vc_npend = 0;
    vc_free_dead(true);
    vc_bytes = 0;
    if (vc_page_gpu) memset(vc_page_gpu, 0, HEAP_MAX / VC_PAGE);
    vc_epoch++;
    st_defaults();
}

// A shadow (depth-compare) sampler needs a depth image: Vulkan rejects a compare on a colour format (VUID
// vkCmdDrawIndexed-None-06479, seen 20x on the void / rift material shaders' sampler2DArrayShadow t4 with an
// R8G8B8A8_UNORM view). view: what texture_info resolved (NULL = nothing). Slots without an image of their own
// (no handle, pool miss, a fallback dummy from vk_view or the feedback path) get the depth-1.0 dummy; a real colour
// texture is kept as is - comparing against its red channel would be a guess about what the game means - and only
// logged. Each route is logged (first 24) with the texture so the binding can be traced.
static VkImageView shadow_view(int stage, int i, const ShaderTexture *st_, uint64_t h, NvnTexture *tex, VkImageView view) {
    const char *route = NULL;
    NvnTexture *sampled = tex && naliases ? alias_read(tex) : tex;
    TexBackend *b = sampled ? sampled->backend : NULL;
    if (!h) route = "no texture handle bound";
    else if (!tex) route = "texture pool entry missing";
    else if (!view) route = "image view rejected";
    else if (view == vk_dummy_view(st_->type)) route = "fallback dummy (type mismatch, multisample or failed feedback view)";
    else if (b && !(b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) route = "colour texture bound (kept)";
    if (!route) return view;
    static const char *const types[] = {"1d", "2d", "3d", "cube", "2d array", "cube array", "1d array", "buffer", "2d ms"};
    bool colour = route[0] == 'c';
    static int nlog;
    if (nlog++ < 24)
        LOG("[vk] shadow sampler stage %d t%d (handle c[%#x] tic %u tsc %u, %s): %s; tex %p nvn fmt %#x %ux%ux%u target %u "
            "samples %u vk fmt %d -> %s\n", stage, i, st_->handle_offset, (unsigned)(h & 0xFFFFF), (unsigned)(h >> 20) & 0xFFF,
            st_->type < 9 ? types[st_->type] : "?", route, (void *)tex, tex ? tex->format : 0, tex ? tex->width : 0, tex ? tex->height : 0,
            tex ? tex->depth : 0, tex ? tex->target : 0, b ? b->samples : 0, b ? (int)b->format : 0,
            colour ? "colour view" : "depth dummy");
    if (colour) return view;
    VkImageView d = vk_dummy_depth_view(st_->type);
    return d ? d : view;
}

static const char *textype_name(int t) {
    static const char *const names[] = {"1d", "2d", "3d", "cube", "2d array", "cube array", "1d array", "buffer", "2d ms"};
    return t >= 0 && t < 9 ? names[t] : "?";
}

// A bound texture handle that still samples the black 1x1 dummy: a vk_view type mismatch (the 1408x792 D24S8 depth
// sampled as a cube in the void run: "[tex] type mismatch: want 3 have 1", which names no shader or slot), a
// multisampled image, a pool miss or a rejected view. Once per (shader, stage, slot), first 32: which shader and
// slot ask for what, and which texture (and pass) they get.
static void dummy_slot_note(int stage, int i, const ShaderTexture *st_, uint64_t h, NvnTexture *tex) {
    static struct { uint64_t code; int stage, slot; } seen[64];
    static int nseen, nlog;
    if (nlog >= 32) return;
    uint64_t code = st.program ? st.program->stages[stage].code_addr : 0;
    for (int k = 0; k < nseen; k++) if (seen[k].code == code && seen[k].stage == stage && seen[k].slot == i) return;
    if (nseen < 64) { seen[nseen].code = code; seen[nseen].stage = stage; seen[nseen++].slot = i; }
    nlog++;
    NvnTexture *sampled = tex && naliases ? alias_read(tex) : tex;
    TexBackend *b = sampled ? sampled->backend : NULL;
    bool attached = tex && (tex == st.rt.depth || (st.rt.ncolor > 0 && tex == st.rt.color[0]));
    LOG_VERBOSE("[vk] texture slot samples the dummy: stage %d shader %llx t%d (handle c[%#x] tic %u tsc %u) wants %s%s; tex %p "
        "nvn fmt %#x %ux%ux%u target %u layers %u samples %u%s | pass: %d colour, depth %p\n", stage, (unsigned long long)code, i,
        st_->handle_offset, (unsigned)(h & 0xFFFFF), (unsigned)(h >> 20) & 0xFFF, textype_name(st_->type), st_->shadow ? " (shadow)" : "",
        (void *)tex, tex ? tex->format : 0, tex ? tex->width : 0, tex ? tex->height : 0, tex ? tex->depth : 0, tex ? tex->target : 0,
        b ? b->layers : 0, b ? b->samples : 0, !tex ? " (no pool entry)" : attached ? " (an attachment of this pass)" : "",
        st.rt.ncolor, (void *)st.rt.depth);
}

static void texture_info(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out, float sc[2], uint32_t wh[2]) {
    ShaderTexture *st_ = &si->tex[i];
    int slot = ((int)st_->handle_offset - 0x20) / 8;  // 64-bit handles from offset 0x20
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler smp = VK_NULL_HANDLE;
    uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
    sc[0] = sc[1] = 1;
    wh[0] = wh[1] = 0;
    NvnTexEntry entry = {0};
    if (h && nvn_texture_entry_acquire(st.tpool, tic, &entry)) {
        NvnTexEntry *e = &entry;
        if (e->tex) {
            // Per-frame memo: ~2400 lookups per overworld frame recomputed the view (format / range math, cache
            // probe). The first lookup of a frame still runs vk_view (upload check); a view that may have been
            // destroyed (eviction, image re-created) bumps vk_view_gen. EOWRECOMP_VK_NO_TEX_MEMO=1 disables.
            extern uint32_t vk_view_gen;
            extern uint64_t vk_frame_counter(void);
            static int nomemo = -1;
            if (nomemo < 0) nomemo = getenv("EOWRECOMP_VK_NO_TEX_MEMO") != NULL;
            static struct { NvnTexture *tex; uint64_t vh, frame; uint32_t gen; uint8_t type, has_view; VkImageView view; } memo[1024];
            uint64_t vh = e->has_view ? hash_words(0, &e->view, sizeof e->view) : 0, frame = vk_frame_counter();
            uint32_t ms = (uint32_t)((((uintptr_t)e->tex >> 4) ^ vh ^ (uint64_t)st_->type * 0x9E3779B97F4A7C15ull) & 1023);
            if (!nomemo && memo[ms].tex == e->tex && memo[ms].vh == vh && memo[ms].frame == frame && memo[ms].gen == vk_view_gen &&
                memo[ms].type == st_->type && memo[ms].has_view == e->has_view) {
                view = memo[ms].view;
            } else {
                view = vk_view(naliases ? alias_read(e->tex) : e->tex, &e->view, e->has_view, false, st_->type);
                memo[ms].tex = e->tex; memo[ms].vh = vh; memo[ms].frame = frame; memo[ms].gen = vk_view_gen;
                memo[ms].type = (uint8_t)st_->type; memo[ms].has_view = e->has_view; memo[ms].view = view;
            }
            renderer_texture_scale(e->tex, &sc[0], &sc[1]);
            wh[0] = e->tex->width; wh[1] = e->tex->height;
            for (int k = 0; k < nfeedback; k++)
                if (e->tex == feedback_tex[k] && (st_->type == TEXTYPE_2D || st_->type == TEXTYPE_2D_ARRAY)) {
                    // A feedback texture can be sampled through its depth or stencil aspect in the same draw.
                    // Redirecting a stencil read to the snapshot's depth view returns the wrong values.
                    uint32_t mode = e->has_view && (e->view.setmask & 16) ? e->view.ds_mode : e->tex->ds_mode;
                    bool stencil = mode == 1 || e->tex->format == 0x31;
                    view = stencil && feedback_stencil_view[k] ? feedback_stencil_view[k] : feedback_view[k];
                    // Sampled as a 2D array (the main materials' sampler2DArrayShadow t4 reading the 1024x1024 shadow
                    // map from inside the depth-only shadow pass that renders it): a one-layer 2D_ARRAY view of the
                    // snapshot, valid for a 2D image. This used to be the RGBA8 dummy, then the depth-1.0 dummy.
                    if (st_->type == TEXTYPE_2D_ARRAY) {
                        static const VkComponentMapping id = {0};
                        VkImageAspectFlags a = stencil && feedback_stencil_view[k] ? VK_IMAGE_ASPECT_STENCIL_BIT : feedback_aspect[k];
                        view = feedback_snap[k] ? vk_image_view(feedback_snap[k], VK_IMAGE_VIEW_TYPE_2D_ARRAY, feedback_fmt[k], 0, 1, 0, 1, id, a,
                                                                "feedback snapshot as 2D array") : VK_NULL_HANDLE;
                        if (!view) view = vk_dummy_view(TEXTYPE_2D_ARRAY);
                    }
                }
        }
    }
    if (h && st.spool && tsc < (uint32_t)st.spool->count) smp = vk_sampler(&((NvnSamplerDesc *)st.spool->entries)[tsc]);
    if (st_->shadow) view = shadow_view(stage, i, st_, h, entry.tex, view);
    if (!view) view = vk_dummy_view(st_->type);
    if (h && view == vk_dummy_view(st_->type)) dummy_slot_note(stage, i, st_, h, entry.tex);
    if (!smp) smp = vk_default_sampler(true);
    *out = (VkDescriptorImageInfo){smp, view, VK_IMAGE_LAYOUT_GENERAL};
}

// SUST.B image handles come from the separate NVN image table (c[2] offsets 0x120..0x158).
static VkImageView image_view(int stage, ShaderInfo *si, int i) {
    int slot = i < si->ntex ? ((int)si->tex[i].handle_offset - 0x120) / 8 : -1;
    uint64_t handle = slot >= 0 && slot < 8 ? st.img[stage][slot] : 0;
    uint32_t id = (uint32_t)(handle & 0xFFFFF);
    NvnTexEntry entry = {0};
    if (handle && nvn_texture_entry_acquire(st.tpool, id, &entry)) {
        if (entry.tex) {
            VkImageView view = vk_storage_view(entry.tex, &entry.view, entry.has_view, TEXTYPE_2D);
            if (view) {
                renderer_mark_gpu_owned(entry.tex);
                return view;
            }
        }
    }
    static int nlog;
    if (nlog++ < 10) {
        NvnTexEntry entry = {0};
        NvnTexture *tex = handle && nvn_texture_entry_acquire(st.tpool, id, &entry) ? entry.tex : NULL;
        LOG("[vk] image slot %d (id %u) not storable: NVN format %#x flags %#x\n", slot, id,
            tex ? tex->format : 0, tex ? tex->flags : 0);
    }
    return vk_dummy_storage_view();
}

// Writes the descriptor set of one stage. gx..gz: compute group counts (driver constants).
// pre_ii: texture descriptors already resolved by the caller (draw: fragment textures, needed earlier for the
// viewport decision), or NULL.
static VkDescriptorSet stage_set(Prog *p, VkDescriptorSetLayout layout, uint32_t set, int stage, ShaderInfo *si, const float *rscale_row0,
                                 const float tint[8], uint32_t gx, uint32_t gy, uint32_t gz, const VkDescriptorImageInfo *pre_ii) {
    uint32_t variable_count = 0;
    if (si->uses_bindless_image && set == 0) {
        variable_count = st.tpool ? (uint32_t)st.tpool->count : 0;
        // dispatch_compute rejects empty or oversized pools before allocating this descriptor set.
        if (!variable_count) FATAL("bindless SUSTB descriptor set has no texture pool\n");
    }
    VkDescriptorSet ds = vk_alloc_set(layout, variable_count);
    VkWriteDescriptorSet w[MAX_BIND];
    VkDescriptorBufferInfo bi[MAX_BIND];
    VkDescriptorImageInfo ii[MAX_BIND];
    VkDescriptorImageInfo *storage_ii = NULL;
    int n = 0;
    int sg = stage == 1 ? 1 : 0;
    float rs[66][4] = {{1, 1, 0, 0}};  // ([65]: a mirrored composite's width in LAS Recomp; unused, 0)
    if (rscale_row0) memcpy(rs[0], rscale_row0, 16);
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && b->binding == BINDLESS_IMAGE_BINDING && si->uses_bindless_image) {
            w[n].descriptorCount = variable_count;
            // Every element is written, so each needs a valid view: VK_NULL_HANDLE is only legal with the
            // nullDescriptor feature (VK_EXT_robustness2), which the device does not enable. Pool entries that
            // are not storable images get the 1x1 2D sink, matching the shader's image2D array.
            storage_ii = calloc(variable_count, sizeof *storage_ii);
            VkImageView sink = vk_dummy_storage_view();
            for (uint32_t ti = 0; ti < variable_count; ti++)
                storage_ii[ti] = (VkDescriptorImageInfo){VK_NULL_HANDLE, sink, VK_IMAGE_LAYOUT_GENERAL};
            if (st.tpool) {
                for (uint32_t ti = 0; ti < variable_count && ti < (uint32_t)st.tpool->count; ti++) {
                    NvnTexEntry entry;
                    if (!nvn_texture_entry_acquire(st.tpool, ti, &entry) || !entry.is_image || !entry.tex) continue;
                    VkImageView view = vk_storage_view(entry.tex, &entry.view, entry.has_view, TEXTYPE_2D);
                    if (!view) continue;
                    storage_ii[ti] = (VkDescriptorImageInfo){VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
                    renderer_mark_gpu_owned(entry.tex);
                }
            }
            w[n].pImageInfo = storage_ii;
            n++;
            continue;
        }
        if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->binding >= 32) {
            int ti = (int)b->binding - 32;
            float sc[2]; uint32_t wh[2];
            if (ti < si->ntex && pre_ii && ti < 64) { ii[n] = pre_ii[ti]; sc[0] = tex_sc[sg][ti][0]; sc[1] = tex_sc[sg][ti][1]; wh[0] = tex_wh[sg][ti][0]; wh[1] = tex_wh[sg][ti][1]; }
            else if (ti < si->ntex) texture_info(stage, si, ti, &ii[n], sc, wh);
            else { ii[n] = (VkDescriptorImageInfo){vk_default_sampler(true), vk_dummy_view(TEXTYPE_2D), VK_IMAGE_LAYOUT_GENERAL}; sc[0] = sc[1] = 1; wh[0] = wh[1] = 0; }
            if (ti < 64) {
                tex_sc[sg][ti][0] = sc[0]; tex_sc[sg][ti][1] = sc[1];
                tex_wh[sg][ti][0] = wh[0]; tex_wh[sg][ti][1] = wh[1];
            }
            w[n].pImageInfo = &ii[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && b->binding >= 32) {
            ii[n] = (VkDescriptorImageInfo){VK_NULL_HANDLE, image_view(stage, si, (int)b->binding - 32), VK_IMAGE_LAYOUT_GENERAL};
            w[n].pImageInfo = &ii[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding < 17) {
            bi[n] = cbuf_info(stage, si, (int)b->binding, gx, gy, gz);
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            bi[n] = b->binding == GFAST_BUFFER_INDEX ? gfast_info() : gmem_info();
            w[n].pBufferInfo = &bi[n];
        } else {
            continue;  // rscale / tint: written below, once the textures (and their scales) are known
        }
        n++;
    }
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set || b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
            (b->binding != 18 && b->binding != 19 && b->binding != GMAP_BUFFER_INDEX)) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->binding == GMAP_BUFFER_INDEX) {  // (compute) this dispatch's fast-copy table: gfast_prepare
            bi[n] = ubo_span(vk_transient_copy(gmap_tab, sizeof gmap_tab, 256), sizeof gmap_tab);
        } else if (b->binding == 18) {
            for (int t = 0; t < si->ntex && t < 64; t++) { rs[1 + t][0] = tex_sc[sg][t][0]; rs[1 + t][1] = tex_sc[sg][t][1]; }
            bi[n] = ubo_span(copy_dedupe(sg ? DEDUPE_RSCALE_F : DEDUPE_RSCALE_V, rs, sizeof rs, 256), sizeof rs);
        } else {
            float zero[8] = {0};
            bi[n] = ubo_span(copy_dedupe(DEDUPE_TINT, tint ? tint : zero, 32, 256), 32);
        }
        w[n].pBufferInfo = &bi[n];
        n++;
    }
    if (n) vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    free(storage_ii);
    return ds;
}

// ================================================================== compute
typedef struct { Prog p; ShaderInfo *si; VkPipeline pipe; uint32_t local[3]; } CsProg;

static void build_compute(void *arg) {
    CsProg *cp = arg;
    if (cp->si->uses_bindless_image && !g_vk.bindless_storage_images) {
        LOG("[compute] bindless SUSTB requires Vulkan descriptor indexing and formatless storage-image writes\n");
        atomic_store(&cp->p.state, 2);
        return;
    }
    size_t n = 0;
    uint32_t *spv = vk_glsl_to_spirv(cp->si->msl, STAGE_COMPUTE, "cs", &n);
    if (!spv) { atomic_store(&cp->p.state, 2); return; }
    cp->p.nbinds = vk_reflect(spv, n, cp->p.binds, MAX_BIND);
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    cp->p.layout = make_layout(&cp->p, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0);
    VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo spec = {3, me, 12, cp->local};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main",
                                                .pSpecializationInfo = &spec},
                                      .layout = cp->p.layout};
    VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &cp->pipe);
    atomic_fetch_add(&stat_compiles, 1);
    atomic_store(&cp->p.state, r == VK_SUCCESS ? 1 : 2);
    if (r != VK_SUCCESS) LOG("[compute] pipeline failed: %d\n", r);
}

static struct CsEnt { uint64_t addr; CsProg *cp; } g_cs_tab[256];  // compute programs by code address

static CsProg *compute_program(NvnProgram *np) {
    NvnShaderStage *ss = &np->stages[5];
    if (!ss->code_addr) return NULL;
    struct CsEnt *tab = g_cs_tab;
    int n = g_cs_n;
    for (int i = 0; i < n; i++) if (tab[i].addr == ss->code_addr) return tab[i].cp;
    CsProg *cp = calloc(1, sizeof *cp);
    if (n < 256) { tab[n].addr = ss->code_addr; tab[n].cp = cp; g_cs_n = n + 1; }
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    ShaderKey k = {0};
    k.glsl = 1;
    pthread_mutex_lock(&translate_lock);
    cp->si = maxwell_translate((const uint8_t *)(uintptr_t)ss->code_addr, ss->const_offset ? ss->const_offset : 0x10000, STAGE_COMPUTE, &k);
    pthread_mutex_unlock(&translate_lock);
    cp->p.vi = cp->si;
    if (!cp->si->msl) {
        LOG("[compute] translate failed: %s (code %llx)\n", cp->si->error, (unsigned long long)ss->code_addr);
        const uint32_t *wd = (const uint32_t *)(uintptr_t)ss->code_addr;  // (the blob's start: header layout)
        LOG("[compute]   blob:");
        for (int i = 0; i < 40; i++) LOG("%s%08x", i % 8 ? " " : "\n[compute]     ", wd[i]);
        int nz = 1;
        while (nz < 4096 && !wd[nz]) nz++;
        LOG("\n[compute]   first non-zero word after the magic at +%#x:", nz * 4);
        for (int i = nz & ~7; i < (nz & ~7) + 16; i++) LOG(" %08x", wd[i]);
        LOG("\n");
        atomic_store(&cp->p.state, 2);
        return cp;
    }
    for (int i = 0; i < 3; i++) {
        cp->local[i] = ss->control ? *(const uint32_t *)(ss->control + 0x748 + 4 * i) : 1;
        if (!cp->local[i]) cp->local[i] = 1;
    }
    save_glsl(cp->si->msl, ss->code_addr, "cs");
    run_job(build_compute, cp);
    return cp;
}

static void dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {
    snap_reason = "compute"; snapshots_invalidate(NULL);
    if (!st.program || !gx || !gy || !gz) return;
    CsProg *cp = compute_program(st.program);
    if (cp && atomic_load(&cp->p.state) == 0) {
        // A skipped dispatch leaves its output unwritten, and compute results feed later passes (session 16b: the 24
        // dispatches still skipped after the warmup). The game has 37 compute shaders; with the SPIR-V and driver
        // caches prepared their build takes milliseconds, so the dispatch waits for it (moved to the front), up to
        // 250 ms. EOWRECOMP_VK_COMPUTE_WAIT=0: skip as before.
        static int on = -1;
        if (on < 0) on = !(getenv("EOWRECOMP_VK_COMPUTE_WAIT") && atoi(getenv("EOWRECOMP_VK_COMPUTE_WAIT")) == 0);
        uint64_t t0 = SDL_GetTicksNS();
        while (on && atomic_load(&cp->p.state) == 0 && SDL_GetTicksNS() - t0 < 250000000ull) {
            jobs_bump(&jobs, cp);
            SDL_DelayNS(250000);
        }
    }
    if (!cp || atomic_load(&cp->p.state) != 1) { SKIP(SKIP_COMPUTE_PROG); return; }
    if (cp->si->uses_bindless_image) {
        uint32_t limit = g_vk.props.limits.maxDescriptorSetStorageImages;
        if (limit > g_vk.props.limits.maxPerStageDescriptorStorageImages) limit = g_vk.props.limits.maxPerStageDescriptorStorageImages;
        uint32_t pool_count = st.tpool ? (uint32_t)st.tpool->count : 0;
        if (!pool_count || pool_count > limit) {
            static int nlog;
            if (nlog++ < 8)
                LOG("[compute] skipping bindless SUSTB dispatch: texture pool has %u entries, storage-image descriptor limit is %u\n",
                    pool_count, limit);
            SKIP(SKIP_COMPUTE_PROG);
            return;
        }
    }
    {   // EOWRECOMP_SKIP_CS=<code address hex> (debug): skip that compute shader's dispatches, to see what it does
        static uint64_t skip = ~0ull;
        if (skip == ~0ull) { const char *e = getenv("EOWRECOMP_SKIP_CS"); skip = e ? strtoull(e, NULL, 16) : 0; }
        if (skip && st.program->stages[5].code_addr == skip) { SKIP(SKIP_LIST); return; }
    }
    if (cp->si->uses_gmem && getenv("EOWRECOMP_LOG_CS_SBUF")) {  // (debug) the storage buffers bound to gmem dispatches
        static uint64_t seen_cs[16];
        static int seen_n[16];
        int k = 0;
        while (k < 15 && seen_cs[k] && seen_cs[k] != st.program->stages[5].code_addr) k++;
        seen_cs[k] = st.program->stages[5].code_addr;
        if (seen_n[k]++ < 24) {
            LOG("[cs] %llx %ux%ux%u:", (unsigned long long)st.program->stages[5].code_addr, gx, gy, gz);
            for (int i = 0; i < 16; i++)
                if (st.sbuf[5][i].addr) LOG(" sb%d %llx+%llx", i, (unsigned long long)st.sbuf[5][i].addr, (unsigned long long)st.sbuf[5][i].size);
            LOG("\n");
        }
    }
    vk_end_pass();
    gstore_sync_cbufs(5, cp->si);  // (before taking the command buffer: a wait submits the current one)
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    gp_mark(cb, "dispatch %ux%ux%u (local %ux%ux%u) cs %llx gmem %d", gx, gy, gz, cp->local[0], cp->local[1], cp->local[2],
            (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem);
    dl_note("dispatch %ux%ux%u cs %llx gmem %d", gx, gy, gz, (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem);
    nfeedback = 0;
    if (ct_on()) {
        LOG("[ct] dispatch cs %llx %ux%ux%u | tex", (unsigned long long)st.program->stages[5].code_addr, gx, gy, gz);
        for (int i = 0; i < cp->si->ntex; i++) {
            NvnTexture *t = shader_texture_object(5, &cp->si->tex[i]);
            if (t) LOG(" %s%p/%#x", cp->si->tex[i].is_image ? "I:" : "", (void *)t, t->format);
        }
        LOG("\n");
    }
    if (cp->si->uses_gmem_map) gfast_prepare(cb);
    VkDescriptorSet ds = stage_set(&cp->p, cp->p.set_layout[0], 0, 5, cp->si, NULL, NULL, gx, gy, gz, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->p.layout, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, gx, gy, gz);
    if (cp->si->uses_bindless_image) {
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                               .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &di);
    }
    if (cp->si->uses_gmem_map && cp->si->uses_gmem_store) gfast_finish(cb);
    if (cp->si->uses_gmem_store) { vc_mark_sbufs(5); gstore_note(5); gwriter_note(st.program->stages[5].code_addr); }
    {   // EOWRECOMP_VK_CHECK_CS=<hex code address>: after a dispatch of that compute shader (one about every
        // 300 frames, at most 40: stand in the scene for a while), wait for the GPU and summarise its storage buffers as guest
        // memory now holds them: words, nonzero words, non-finite floats, the first floats. Diagnostic only (it
        // stalls): tells "the shader wrote zeros / garbage" from "wrote data a later draw does not see". Seen: the
        // Hyrule Town trees' vertex shader 72a5c2b000 reads its matrices from cs 72a4711000's sb1.
        static uint64_t check = ~0ull, next, sample_frame = ~0ull;
        static int nchecked, in_frame;
        if (check == ~0ull) {
            const char *e = getenv("EOWRECOMP_VK_CHECK_CS");
            check = e ? strtoull(e, NULL, 16) : 0;
        }
        extern uint64_t vk_frame_counter(void);
        // (every dispatch of a sampled frame, up to 8: the shader runs once per object set)
        bool sample = check && st.program->stages[5].code_addr == check &&
                      ((vk_frame_counter() == sample_frame && in_frame < 8) || (vk_frame_counter() >= next && nchecked < 10));
        if (sample) {
            if (vk_frame_counter() != sample_frame) { sample_frame = vk_frame_counter(); in_frame = 0; nchecked++; }
            in_frame++;
            next = vk_frame_counter() + 300;
            renderer_flush();
            st.rt_dirty = true;
            LOG("[checkcs] cs %llx %ux%ux%u frame %llu (gmem map %d, store %d):\n", (unsigned long long)check, gx, gy, gz,
                (unsigned long long)vk_frame_counter(), cp->si->uses_gmem_map, cp->si->uses_gmem_store);
            for (int i = 0; i < 16; i++) {
                Range b = st.sbuf[5][i];
                if (!b.addr || !b.size || b.addr < HEAP_BASE) continue;
                const uint32_t *w = (const uint32_t *)(uintptr_t)b.addr;
                uint64_t n = b.size / 4, nz = 0, bad = 0;
                for (uint64_t k = 0; k < n; k++) {
                    if (w[k]) nz++;
                    if (((w[k] >> 23) & 0xFF) == 0xFF) bad++;
                }
                LOG("[checkcs]   sb%d %llx+%llx: %llu words, %llu nonzero, %llu inf/nan;", i, (unsigned long long)b.addr,
                    (unsigned long long)b.size, (unsigned long long)n, (unsigned long long)nz, (unsigned long long)bad);
                for (uint64_t k = 0; k < n && k < (n <= 64 ? 64 : 16); k++) LOG(" %08x", w[k]);  // (small buffers: all)
                LOG("\n");
            }
            for (int k = 3; k < 18; k++) {  // the constant buffers it reads, as guest memory holds them now
                if (!((cp->si->cbuf_mask >> k) & 1)) continue;
                Range u = st.ubuf[5][k - 3];
                if (!u.addr || u.addr < HEAP_BASE) { LOG("[checkcs]   c%d unbound\n", k); continue; }
                const uint32_t *w = (const uint32_t *)(uintptr_t)u.addr;
                LOG("[checkcs]   c%d %llx+%llx:", k, (unsigned long long)u.addr, (unsigned long long)u.size);
                for (int j = 0; j < 20 && (uint64_t)j * 4 < (u.size ? u.size : 80); j++) LOG(" %08x", w[j]);
                LOG("\n");
            }
            // The test the shader is meant to make (cs 72a4711000, read from its GLSL): instance i's 3x4 matrix is
            // sb0 + 64 i, the local box sb3[0..2] .. sb3[4..6], the count sb3[16], c4 words 0..15 a 4x4 matrix; an
            // instance survives unless all 8 corners lie outside the same clip plane (x, y, z against +-w). Counted for
            // both matrix layouts. Survivors on the CPU with none on the GPU: the translated shader is wrong.
            Range s0 = st.sbuf[5][0], s3 = st.sbuf[5][3], c4 = st.ubuf[5][1];
            if (check == 0x72a4711000ull && s0.addr >= HEAP_BASE && s3.addr >= HEAP_BASE && s3.size >= 80 && c4.addr >= HEAP_BASE && c4.size >= 64) {
                const float *box = (const float *)(uintptr_t)s3.addr, *vp = (const float *)(uintptr_t)c4.addr;
                uint32_t count = ((const uint32_t *)(uintptr_t)s3.addr)[16];
                if ((uint64_t)count * 64 > s0.size) count = (uint32_t)(s0.size / 64);
                int vis[2] = {0, 0};
                for (uint32_t i = 0; i < count; i++) {
                    const float *m = (const float *)(uintptr_t)(s0.addr + 64ull * i);
                    for (int layout = 0; layout < 2; layout++) {
                        int out_all = 0x3F;
                        for (int c = 0; c < 8; c++) {
                            float l[3] = {c & 1 ? box[4] : box[0], c & 2 ? box[5] : box[1], c & 4 ? box[6] : box[2]}, wv[4], cl[4];
                            for (int r = 0; r < 3; r++) wv[r] = m[4 * r] * l[0] + m[4 * r + 1] * l[1] + m[4 * r + 2] * l[2] + m[4 * r + 3];
                            wv[3] = 1;
                            for (int r = 0; r < 4; r++) {
                                cl[r] = 0;
                                for (int j = 0; j < 4; j++) cl[r] += (layout ? vp[4 * j + r] : vp[4 * r + j]) * wv[j];
                            }
                            int code = (cl[0] < -cl[3]) | (cl[0] > cl[3]) << 1 | (cl[1] < -cl[3]) << 2 | (cl[1] > cl[3]) << 3 |
                                       (cl[2] < -cl[3]) << 4 | (cl[2] > cl[3]) << 5;
                            out_all &= code;
                        }
                        if (!out_all) vis[layout]++;
                    }
                }
                LOG("[checkcs]   instance positions (matrix column 3):");
                for (uint32_t i = 0; i < count && i < 96; i++) {
                    const float *m = (const float *)(uintptr_t)(s0.addr + 64ull * i);
                    LOG(" (%.1f %.1f %.1f)", m[3], m[7], m[11]);
                }
                LOG("\n");
                LOG("[checkcs]   CPU frustum test: %u instances, %d visible (c4 rows), %d visible (c4 columns); GPU wrote counter %u\n",
                    count, vis[0], vis[1], st.sbuf[5][2].addr >= HEAP_BASE ? ((const uint32_t *)(uintptr_t)st.sbuf[5][2].addr)[1] : 0u);
            }
        }
    }
    stat_dispatches++;
}

// ================================================================== draws
static void debug_draw(Prog *pr, CmdRec *r, bool indexed);

// Quads / fans / polygons as a 32-bit triangle index list.
static uint32_t *expand_indices(uint32_t mode, uint32_t count, const void *indices, int itype, uint32_t first, uint32_t *out_count) {
    uint32_t tris = mode == 7 ? count / 4 * 2 : count >= 3 ? count - 2 : 0;
    uint32_t *out = malloc(tris * 3 * 4 + 4);
    uint32_t n = 0;
#define IDX(i) (indices ? (itype == 0 ? ((const uint8_t *)indices)[i] : itype == 1 ? ((const uint16_t *)indices)[i] : ((const uint32_t *)indices)[i]) : first + (i))
    if (mode == 7) {
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            out[n++] = IDX(q); out[n++] = IDX(q + 1); out[n++] = IDX(q + 2);
            out[n++] = IDX(q); out[n++] = IDX(q + 2); out[n++] = IDX(q + 3);
        }
    } else {
        for (uint32_t i = 1; i + 1 < count; i++) { out[n++] = IDX(0); out[n++] = IDX(i); out[n++] = IDX(i + 1); }
    }
#undef IDX
    *out_count = n;
    return out;
}

static bool topology(uint32_t mode, VkPrimitiveTopology *t, uint8_t *cls) {
    switch (mode) {
    case 0: *t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; *cls = 0; return true;
    case 1: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; *cls = 1; return true;
    case 3: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1; return true;
    case 4: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return true;
    case 5: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; *cls = 2; return true;
    default: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return false;
    }
}

static void draw_(CmdRec *r, bool indexed);
static void draw(CmdRec *r, bool indexed);

// nvnCommandBufferDraw{Elements,Arrays}Indirect: the arguments live in GPU memory, usually written in the same frame by
// a compute pass. Echoes of Wisdom's culling compute 72a4711000 appends the visible instances of an object set and
// writes {index count, instance count, first index, base vertex, base instance} for the set's draws - the Hyrule Town
// trees are 1524 indices x 3 instances from it. These commands used to be dropped (no case in exec), so everything drawn
// that way was missing. Read the arguments once the GPU's stores to them have completed (wait for it when a storing
// shader in flight may cover them, as gstore_sync_cbufs does for constant buffers), then draw as the direct command
// would: CMD_DRAW_ELEMENTS {mode, type, count, index address, base vertex, base instance, instances},
// CMD_DRAW_ARRAYS {mode, first, count, base instance, instances}. An instance or index count of 0 draws nothing.
static bool gstore_overlaps(uint64_t lo, uint64_t hi);
static void draw_indirect(CmdRec *r, bool indexed) {
    uint64_t args = indexed ? r->x[3] : r->x[1];
    uint32_t nwords = indexed ? 5 : 4;
    if (args < HEAP_BASE || args % 4) return;
    if (gstore_overlaps(args, args + nwords * 4)) {
        stat_indirect_waits++;
        wait_why = "indirect draw arguments written by the GPU";
        renderer_flush();  // (ends the pass; draw begins its own)
        st.rt_dirty = true;
    }
    uint32_t a[5];
    memcpy(a, (const void *)(uintptr_t)args, nwords * 4);
    stat_indirect++;
    static int nlog;
    if (nlog < 16) {
        nlog++;
        LOG("[vk] indirect %s draw: args %llx = {%u, %u, %u, %d, %u} fs=%llx\n", indexed ? "indexed" : "array",
            (unsigned long long)args, a[0], a[1], a[2], indexed ? (int32_t)a[3] : (int32_t)a[3], indexed ? a[4] : 0u,
            (unsigned long long)(st.program ? st.program->stages[1].code_addr : 0));
    }
    if (!a[0] || !a[1]) { stat_indirect_empty++; return; }
    CmdRec d = *r;
    d.op = indexed ? CMD_DRAW_ELEMENTS : CMD_DRAW_ARRAYS;
    if (indexed) {
        uint32_t type = (uint32_t)r->x[1], isz = type == 0 ? 1 : type == 1 ? 2 : 4;
        d.x[0] = r->x[0];
        d.x[1] = type;
        d.x[2] = a[0];
        d.x[3] = r->x[2] + (uint64_t)a[2] * isz;
        d.x[4] = (uint64_t)(int64_t)(int32_t)a[3];
        d.x[5] = a[4];
        d.x[6] = a[1];
    } else {
        d.x[0] = r->x[0];
        d.x[1] = a[2];
        d.x[2] = a[0];
        d.x[3] = a[3];
        d.x[4] = a[1];
    }
    draw(&d, indexed);
}

static void draw(CmdRec *r, bool indexed) {
    PROF_T0();
    draw_(r, indexed);
    PROF_ADD(prof_draw);
}
static uint64_t prof_tp_;

// One-shot diagnostic for inspecting the textures bound to a selected fragment shader.
// Set EOWRECOMP_VK_DUMP_FS_INPUTS to the guest shader code address (hex) to save each
// color input to build/fs_input_<slot>.png and its alpha channel to
// build/fs_input_<slot>_alpha.png on its first draw. This runs before the draw begins,
// so the captures show the inputs the effect receives without requiring RenderDoc.
static void dump_fragment_shader_inputs(Prog *pr) {
    static uint64_t target_fs;
    static bool checked_env;
    static bool dumped;
    if (!checked_env) {
        const char *value = getenv("EOWRECOMP_VK_DUMP_FS_INPUTS");
        target_fs = value && *value ? strtoull(value, NULL, 0) : 0;
        checked_env = true;
    }
    if (!target_fs || dumped || !pr || !pr->fi || !st.program ||
        st.program->stages[1].code_addr != target_fs) return;

    dumped = true;
    LOG("[vk] diagnostic: dumping fragment inputs for fs=%llx (%d textures)\n",
        (unsigned long long)target_fs, pr->fi->ntex);
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
        NvnTexture *texture = shader_texture_object(1, &pr->fi->tex[i]);
        if (!texture) {
            LOG("[vk] diagnostic: fs input %d is unbound\n", i);
            continue;
        }
        TexBackend *backend = vk_tex(texture, false);
        LOG("[vk] diagnostic: fs input %d texture=%p guest=%ux%u format=%#x backend=%ux%u aspect=%#x\n",
            i, (void *)texture, texture->width, texture->height, texture->format,
            backend ? backend->width : 0, backend ? backend->height : 0,
            backend ? backend->aspect : 0);
        if (!backend || !backend->image || (backend->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) {
            LOG("[vk] diagnostic: fs input %d is not a color image; skipped PNG capture\n", i);
            continue;
        }
        char path[96];
        snprintf(path, sizeof path, "build/fs_input_%02d.png", i);
        renderer_save_texture_tex(backend, path);
        LOG("[vk] diagnostic: saved fs input %d to %s\n", i, path);
        snprintf(path, sizeof path, "build/fs_input_%02d_alpha.png", i);
        renderer_save_texture_tex_alpha(backend, path);
        LOG("[vk] diagnostic: saved fs input %d alpha to %s\n", i, path);
    }
}

// ================================================================== per-draw diagnostics (opt-in, rate-limited)
// Frame number and index of the draw within that frame, shared by the RenderDoc labels (EOWRECOMP_VK_DRAW_LABELS,
// device.c) and the EOWRECOMP_VK_TRACE_FS lines, so a draw found in a capture can be traced live and vice versa.
static uint64_t diag_frame;
static uint32_t diag_draw;

static bool gstore_overlaps(uint64_t lo, uint64_t hi) {
    if (!gstore_pending()) return false;
    if (gstore_all) return true;
    for (int k = 0; k < ngstore; k++) if (lo < gstore[k].hi && gstore[k].lo < hi) return true;
    return false;
}

// EOWRECOMP_VK_TRACE_FS=<hex>[,<hex>...]: guest VS or FS code addresses (as in the "[draw]", "fragment color output"
// and gpu-stored lines, or the draw labels). Each matching draw logs its frame / index, the "[draw]" state line, the
// alpha test, the constant buffers and vertex streams (and whether a shader in flight may still be writing them),
// and every texture slot of both stages: handle, texture object, size, format, target, and whether it is an
// attachment of the pass being drawn, plus the render targets' objects and guest addresses. First 96 matching draws,
// at most 12 per frame (a blur / downsample chain reuses one shader for several steps).
static void trace_draw(Prog *pr, CmdRec *r, bool indexed) {
    static uint64_t addr[8];
    static int naddr = -1;
    if (naddr < 0) {
        naddr = 0;
        const char *e = getenv("EOWRECOMP_VK_TRACE_FS");
        while (e && *e && naddr < 8) {
            char *end;
            uint64_t a = strtoull(e, &end, 16);
            if (end == e) break;
            if (a) addr[naddr++] = a;
            e = *end ? end + 1 : end;
        }
    }
    if (!naddr || !st.program) return;
    uint64_t vs = st.program->stages[0].code_addr, fs = st.program->stages[1].code_addr;
    bool hit = false;
    for (int k = 0; k < naddr; k++) hit |= addr[k] == vs || addr[k] == fs;
    static int total;
    static uint64_t frame_of_last;
    static int in_frame;
    if (!hit || total >= 96) return;
    if (frame_of_last != diag_frame) { frame_of_last = diag_frame; in_frame = 0; }
    if (in_frame++ >= 12) return;
    total++;
    LOG("[trace] frame %llu draw %u vs=%llx fs=%llx alpha test func %u ref %.3f (8 = always; not implemented)\n",
        (unsigned long long)diag_frame, diag_draw, (unsigned long long)vs, (unsigned long long)fs, st.color_state[2], st.alpha_ref);
    debug_draw(pr, r, indexed);
    for (int c = 0; c <= st.rt.ncolor && c < 9; c++) {  // the targets' objects and memory, to match against [tex] lines
        NvnTexture *t = c < st.rt.ncolor ? st.rt.color[c] : st.rt.depth;
        if (!t) continue;
        LOG("[trace]   rt %s%d %p addr %llx %ux%u fmt %#x levels %u\n", c < st.rt.ncolor ? "c" : "depth", c < st.rt.ncolor ? c : 0,
            (void *)t, (unsigned long long)t->addr, t->width, t->height, t->format, t->levels);
    }
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++) {
        ShaderInfo *si = infos[sg];
        if (!si) continue;
        for (int k = 3; k < 18; k++) {
            if (!((si->cbuf_mask >> k) & 1)) continue;
            Range u = st.ubuf[sg][k - 3];
            LOG("[trace]   %s c%d %llx+%llx%s\n", sg ? "fs" : "vs", k, (unsigned long long)u.addr, (unsigned long long)u.size,
                u.addr && gstore_overlaps(u.addr, u.addr + (u.size ? u.size : 256)) ? " (may be written by a shader in flight)" : "");
        }
        // Storage buffers: graphics shaders read them as global memory (LDG) through the driver constant buffer's
        // descriptors (c0 + 0x110, fragment c0 + 0x510: cbuf_info); an address outside the imported heap reads 0.
        // Seen: the Hyrule Town trees' vertex shader 72a5c2b000 loads its matrix rows from c0[0x110] + offset.
        if (si->uses_gmem) {
            int nb = 0;
            for (int i = 0; i < 16; i++) {
                Range b = st.sbuf[sg][i];
                if (!b.addr && !b.size) continue;
                nb++;
                bool in_heap = b.addr >= HEAP_BASE && b.addr + b.size <= HEAP_BASE + gmem_size;
                LOG("[trace]   %s s%d %llx+%llx%s%s\n", sg ? "fs" : "vs", i, (unsigned long long)b.addr, (unsigned long long)b.size,
                    in_heap ? "" : " (outside the imported heap: reads 0)",
                    gstore_overlaps(b.addr, b.addr + b.size) ? " (may be written by a shader in flight)" : "");
                for (unsigned w = 0; w < 64 && w < gwriter_n; w++) {  // newest first
                    unsigned k = (gwriter_n - 1 - w) & 63;
                    if (gwriter[k].lo < b.addr + b.size && b.addr < gwriter[k].hi) {
                        LOG("[trace]     last written by cs %llx in frame %llu (its storage %llx+%llx)\n", (unsigned long long)gwriter[k].cs,
                            (unsigned long long)gwriter[k].frame, (unsigned long long)gwriter[k].lo,
                            (unsigned long long)(gwriter[k].hi - gwriter[k].lo));
                        break;
                    }
                }
            }
            if (!nb) LOG("[trace]   %s reads global memory but has no storage buffer bound (c0 descriptors are 0)\n", sg ? "fs" : "vs");
        }
        for (int i = 0; i < si->ntex; i++) {
            ShaderTexture *st_ = &si->tex[i];
            int slot = ((int)st_->handle_offset - 0x20) / 8;
            uint64_t h = slot >= 0 && slot < 32 ? st.tex[sg][slot] : 0;
            NvnTexEntry entry = {0};
            NvnTexture *tex = h && nvn_texture_entry_acquire(st.tpool, h & 0xFFFFF, &entry) ? entry.tex : NULL;
            NvnTexture *sampled = tex && naliases ? alias_read(tex) : tex;
            TexBackend *b = sampled ? sampled->backend : NULL;
            bool attached = tex && tex == st.rt.depth;
            for (int c = 0; c < st.rt.ncolor && c < 8; c++) attached |= tex && tex == st.rt.color[c];
            LOG("[trace]   %s t%d c[%#x] %s%s tic %u tsc %u -> tex %p%s addr %llx nvn fmt %#x %ux%ux%u target %u layers %u samples %u%s%s%s\n",
                sg ? "fs" : "vs", i, st_->handle_offset, textype_name(st_->type), st_->shadow ? " shadow" : "", (unsigned)(h & 0xFFFFF),
                (unsigned)(h >> 20) & 0xFFF, (void *)tex, sampled != tex ? " (copy alias)" : "", tex ? (unsigned long long)tex->addr : 0ull, tex ? tex->format : 0,
                tex ? tex->width : 0, tex ? tex->height : 0, tex ? tex->depth : 0, tex ? tex->target : 0, b ? b->layers : 0,
                b ? b->samples : 0, !h ? " (no handle)" : !tex ? " (no pool entry)" : "", attached ? " (attachment of this pass)" : "",
                tex && !b ? " (no GPU image yet: filled from guest memory)" : b && !b->gpu_owned ? " (from guest memory)" : "");
        }
    }
    for (int i = 0; i < st.nstream && i < 16; i++) {
        Range v = st.vbuf[i];
        if (!v.addr) continue;
        LOG("[trace]   vbuf%d %llx+%llx stride %u%s\n", i, (unsigned long long)v.addr, (unsigned long long)v.size, st.stream[i][0],
            gstore_overlaps(v.addr, v.addr + v.size) ? " (may be written by a shader in flight)" : "");
    }
}

// Called for every draw that reaches recording: numbers it, and logs the opt-in traces. Also notes the NVN alpha test
// (ColorState byte 2, nvnColorStateSetAlphaTest; 8 = ALWAYS, the default), which the Vulkan backend does not
// implement: once per fragment shader / function, first 16, so it can be ruled in or out for missing or extra pixels.
static void draw_diag(Prog *pr, CmdRec *r, bool indexed) {
    extern uint64_t vk_frame_counter(void);
    uint64_t frame = vk_frame_counter();
    if (frame != diag_frame) { diag_frame = frame; diag_draw = 0; }
    diag_draw++;
    uint8_t func = st.color_state[2];
    if (func != 8 && func != 0 && pr->fi) {
        static struct { uint64_t fs; uint8_t func; } seen[16];
        static int nseen;
        uint64_t fs = st.program ? st.program->stages[1].code_addr : 0;
        bool known = false;
        for (int k = 0; k < nseen; k++) known |= seen[k].fs == fs && seen[k].func == func;
        if (!known && nseen < 16) {
            seen[nseen].fs = fs; seen[nseen++].func = func;
            LOG_VERBOSE("[vk] alpha test func %u ref %.3f ignored (not implemented in the Vulkan backend): fs=%llx frame %llu draw %u\n",
                func, st.alpha_ref, (unsigned long long)fs, (unsigned long long)frame, diag_draw);
        }
    }
    trace_draw(pr, r, indexed);
}

// A draw whose program or pipeline is still building is normally skipped for that frame. For a full-screen pass
// (2 triangles: bloom / blur / downsample chains, tone mapping, UI quads) that leaves its target unwritten, and every
// later pass sampling it reads an image that holds guest memory instead of the render: the black flash on entering
// Hyrule Town (the 176x99 blur chain of fs 72ef15f300 sampling a 352x198 target, 728c16e000, that no draw had
// rendered yet - frame 1496, while 1684 draws were skipped for compiling). Those draws wait for their own build
// instead, moved to the front of the queues; scene geometry keeps skipping (a missing prop for a frame is far less
// visible than a black screen). At most 400 ms of waiting per frame. EOWRECOMP_VK_SKIP_QUADS=1: skip them as before.
// Every other skipped draw's targets are logged (first 32), to find a producer that still flashes.
static bool compile_wait(CmdRec *r, bool indexed, _Atomic int *state, Prog *p, Pipe *pp) {
    static int off = -1;
    if (off < 0) off = getenv("EOWRECOMP_VK_SKIP_QUADS") != NULL;
    uint64_t count = r->x[2], inst = r->x[indexed ? 6 : 4];
    bool quad = count <= 6 && inst <= 1;
    // A build the startup warmup has queued is known to be coming (and moves to the front): waiting costs little,
    // skipping costs the draw.
    bool known = pp ? pp->warm : p && p->warm;
    // TOTK's rule (docs/architecture.md: "skipped when one of its pass's targets was rendered in the previous frame,
    // and waits otherwise, so one-shot passes are never lost"): a target nothing drew into last frame or this one is
    // a pass that may not come again - a scene's first frame after a load, a render-to-texture done once. Its draw
    // waits (same budget) instead of leaving the target unwritten. EOWRECOMP_VK_ONESHOT_WAIT=0: off.
    static int oneshot_on = -1;
    if (oneshot_on < 0) oneshot_on = !(getenv("EOWRECOMP_VK_ONESHOT_WAIT") && atoi(getenv("EOWRECOMP_VK_ONESHOT_WAIT")) == 0);
    bool oneshot = false;
    if (oneshot_on) {
        extern uint64_t vk_frame_counter(void);
        NvnTexture *t = st.rt.ncolor && st.rt.color[0] ? st.rt.color[0] : st.rt.depth;
        TexBackend *tb = t ? t->backend : NULL;
        oneshot = tb && tb->drawn_frame < vk_frame_counter();  // (drawn_frame = frame + 1: last frame -> == counter)
    }
    if ((!off && quad) || known || oneshot) {
        extern uint64_t vk_frame_counter(void);
        static uint64_t budget_frame = ~0ull;
        static uint64_t spent_ns;
        uint64_t frame = vk_frame_counter();
        if (frame != budget_frame) { budget_frame = frame; spent_ns = 0; }
        uint64_t t0 = SDL_GetTicksNS();
        while (atomic_load(state) == 0 && spent_ns + (SDL_GetTicksNS() - t0) < 400000000ull) {
            void *arg = pp ? (void *)pp : (void *)p;
            jobs_bump(&tjobs, arg);
            jobs_bump(&jobs, arg);
            SDL_DelayNS(250000);
        }
        uint64_t waited = SDL_GetTicksNS() - t0;
        spent_ns += waited;
        static int nlog;
        if (nlog++ < 32)
            LOG_VERBOSE("[vk] %s draw waited %.1f ms for its %s (fs=%llx, %ux%u target)%s\n",
                known ? "warmup" : oneshot && !quad ? "one-shot target" : "full-screen", waited / 1e6, pp ? "pipeline" : "program",
                (unsigned long long)(st.program ? st.program->stages[1].code_addr : 0), st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->width : 0,
                st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->height : 0, atomic_load(state) == 0 ? "; still building, skipped" : "");
        if (atomic_load(state) != 0) return atomic_load(state) == 1;
    } else if (atomic_load(state) == 0) {
        static int nlog;
        if (nlog++ < 32) {
            NvnTexture *c0 = st.rt.ncolor ? st.rt.color[0] : NULL;
            LOG_VERBOSE("[vk] draw skipped, %s building: fs=%llx count %llu inst %llu -> c0 %p %ux%u addr %llx, depth %p\n", pp ? "pipeline" : "program",
                (unsigned long long)(st.program ? st.program->stages[1].code_addr : 0), (unsigned long long)count, (unsigned long long)inst,
                (void *)c0, c0 ? c0->width : 0, c0 ? c0->height : 0, c0 ? (unsigned long long)c0->addr : 0ull, (void *)st.rt.depth);
        }
    }
    return false;
}

static void draw_(CmdRec *r, bool indexed) {
    if (prof) prof_tp_ = SDL_GetTicksNS();
    if (!st.program) { SKIP(SKIP_NO_PROGRAM); return; }
    ShaderKey vkey = {0}, fkey = {0};
    vkey.glsl = fkey.glsl = 1;
    vkey.flip_y = fkey.flip_y = nvn_window_origin() == 0;
    vkey.depth_minus_one = nvn_depth_mode() == 0;
    for (int i = 0; i < st.nattrib; i++) vkey.attr_kind[i] = (uint8_t)attr_kind(st.attrib[i] & 0xFF);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(st.rt.color[i]->format);
    Prog *pr = get_program(st.program, &vkey, &fkey);
    if (!pr || (atomic_load(&pr->state) != 1 && !compile_wait(r, indexed, &pr->state, pr, NULL))) {
        SKIP(pr && atomic_load(&pr->state) != 2 ? SKIP_PROG_PENDING : SKIP_PROG_FAILED);  // (it may just have become ready)
        return;
    }
    fkey.stencil_tex = stencil_texture_slots(1, pr->fi);
    if (fkey.stencil_tex) {
        pr = get_program(st.program, &vkey, &fkey);
        if (!pr || (atomic_load(&pr->state) != 1 && !compile_wait(r, indexed, &pr->state, pr, NULL))) {
            SKIP(pr && atomic_load(&pr->state) != 2 ? SKIP_PROG_PENDING : SKIP_PROG_FAILED);
            return;
        }
    }
    {   // fragment-shader hash: skip lists (EOWRECOMP_SKIP_FSHASH), debug logging
        static uint64_t last_fs, last_hash;
        uint64_t fs = st.program->stages[1].code_addr;
        if (fs != last_fs) {
            last_fs = fs;
            last_hash = fs ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(fs + 0x30), 0x200) : 0;
        }
        static uint64_t skip[16];
        static int nskip = -1;
        if (nskip < 0) {
            nskip = 0;
            for (const char *p = getenv("EOWRECOMP_SKIP_FSHASH"); p && *p && nskip < 16;) {
                skip[nskip++] = strtoull(p, (char **)&p, 16);
                while (*p == ',') p++;
            }
        }
        for (int i = 0; i < nskip; i++) if (last_hash == skip[i]) { SKIP(SKIP_LIST); return; }
        if (g_debug_frame && getenv("EOWRECOMP_LOG_FSHASH")) LOG("[fshash] fs=%llx hash %016llx\n", (unsigned long long)fs, (unsigned long long)last_hash);
        g_cur_fs_hash = last_hash;
    }
    // One line per guest draw, with the bound guest shader code addresses and render state.
    // Keep it opt-in: busy scenes can issue hundreds of draws per frame.
    static int log_draws = -1;
    if (log_draws < 0) log_draws = getenv("EOWRECOMP_LOG_DRAWS") != NULL;
    if (g_debug_frame || log_draws) debug_draw(pr, r, indexed);
    if (pr->fi && !st.rt.ncolor && (pr->fi->out_mask & 0xF)) {
        static uint64_t warned_fs[32];
        static unsigned warned_next;
        uint64_t fs = st.program->stages[1].code_addr;
        bool seen = false;
        for (unsigned i = 0; i < 32; i++) seen |= warned_fs[i] == fs;
        if (!seen) {
            warned_fs[warned_next++ & 31] = fs;
            LOG_VERBOSE("[vk] fragment color output has no color attachment: fs=%llx out_mask=%#x depth=%p depth_fmt=%#x draw=%s count=%llu instances=%llu\n",
                (unsigned long long)fs, pr->fi->out_mask, (void *)st.rt.depth, st.rt.depth ? st.rt.depth->format : 0,
                indexed ? "indexed" : "arrays", (unsigned long long)r->x[2],
                (unsigned long long)r->x[indexed ? 6 : 4]);
        }
    }
    draw_diag(pr, r, indexed);
    if (ct_on()) ct_draw(pr, "draw");
    alias_draw_check(pr);
    uint16_t sampled_targets = draw_sampled_targets(pr);
    uint16_t written_targets = draw_written_targets(pr);
    check_draw_hazards(sampled_targets, written_targets);
    resolve_feedback(pr);
    // resolve_feedback redirects direct attachment reads to a snapshot. Do not count those
    // reads against the original attachment when tracking later draws in this render pass.
    for (int k = 0; k < nfeedback; k++) {
        for (int c = 0; c < st.rt.ncolor && c < 8; c++)
            if (feedback_tex[k] == st.rt.color[c]) sampled_targets &= (uint16_t)~(1u << c);
        if (feedback_tex[k] == st.rt.depth) sampled_targets &= (uint16_t)~(1u << 8);
    }
    dump_fragment_shader_inputs(pr);
    snap_reason = "draw-depthwrite";
    if (st.rt.depth && st.ds[0] && st.ds[1]) snapshots_invalidate(st.rt.depth);
    snap_reason = "draw-colorwrite";
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i] && ((st.channel_mask >> (i * 4)) & 0xF)) snapshots_invalidate(st.rt.color[i]);
    // (constant buffers a storing shader earlier in flight may have written: wait before the pass, not inside it)
    gstore_sync_cbufs(0, pr->vi);
    gstore_sync_cbufs(1, pr->fi);
    begin_pass();
    VkCommandBuffer cb = vk_cmd();
    if (vk_draw_label_fn) {
        char name[96];
        snprintf(name, sizeof name, "draw f%llu #%u vs=%llx fs=%llx", (unsigned long long)diag_frame, diag_draw,
                 (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr);
        VkDebugUtilsLabelEXT label = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, .pLabelName = name};
        vk_draw_label_fn(cb, &label);
    }

    uint32_t mode = (uint32_t)r->x[0];
    VkPrimitiveTopology topo;
    uint8_t cls;
    bool native = topology(mode, &topo, &cls);
    if (!native && mode != 6 && mode != 7 && mode != 9) { SKIP(SKIP_TOPOLOGY); return; }

    uint64_t prof_tk_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_pre += prof_tk_ - prof_tp_;
    // ---- pipeline
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.prog = pr;
    k.ncolor = pass_ncolor;
    // Only the pass's own targets: pass_color keeps earlier passes' formats past ncolor, which build_pipeline never
    // reads but the key hash did - the same pipeline built again per leftover (and its recipe kept twice).
    memcpy(k.color, pass_color, sizeof k.color[0] * (pass_ncolor < 8 ? pass_ncolor : 8));
    k.depth = pass_depth;
    k.stencil = pass_stencil;
    k.samples = pass_samples;
    for (int i = 0; i < 32; i++) {
        if (!(pr->vi->attr_mask & (1u << i))) continue;
        k.attr_mask |= 1u << i;
        uint32_t a = i < st.nattrib ? st.attrib[i] : 0x2E;
        uint32_t fmt = a & 0xFF, stream = (a >> 8) & 0x1F, off = a >> 13;
        if (i >= st.nattrib || fmt == 0 || stream >= 16) {  // unbound attribute: (0, 0, 0, 1) from a constant stream
            k.attr_fmt[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
            k.attr_bind[i] = ZERO_BINDING;
            k.used_streams |= 1u << ZERO_BINDING;
            continue;
        }
        k.attr_fmt[i] = vertex_format_supported(vertex_format(fmt));
        k.attr_off[i] = off;
        k.attr_bind[i] = stream;
        k.used_streams |= 1u << stream;
    }
    for (int s = 0; s < 16; s++) {
        if (!(k.used_streams & (1u << s))) continue;
        k.stride[s] = s < st.nstream ? st.stream[s][0] : 0;
        k.divisor[s] = s < st.nstream && k.stride[s] ? st.stream[s][1] : 0;
        if (k.divisor[s] > 1 && !g_vk.divisor) k.divisor[s] = 1;
    }
    for (uint32_t i = 0; i < k.ncolor && i < 8; i++) {
        if (!st.rt.color[i] || k.color[i] == VK_FORMAT_UNDEFINED) continue;
        // Targets the fragment shader doesn't output keep their contents on Metal, but are undefined in Vulkan
        // (NVIDIA writes black): mask them. Foliage depth pre-passes left black silhouettes (user 2026-10-08).
        k.write_mask[i] = ((pr->fi->out_mask >> (i * 4)) & 0xF) ? (st.channel_mask >> (i * 4)) & 0xF : 0;
        if (((st.color_state[0] >> i) & 1) && !rt_kind(st.rt.color[i]->format)) {
            k.blend_on |= (uint8_t)(1u << i);
            memcpy(k.blend[i], st.blend[i], 8);
        }
    }
    k.a2c = st.ms[0xC] != 0;
    k.topo_class = cls;
    k.poly_fill = st.poly[2] != 1;
    uint64_t ph = hash_words(0xcbf29ce484222325ull, &k, sizeof k);
    if (!ph) ph = 1;
    uint32_t pslot = (uint32_t)(ph ^ (ph >> 29)) & (PIPE_TAB - 1);
    while (pipe_tab[pslot].key && pipe_tab[pslot].key != ph) pslot = (pslot + 1) & (PIPE_TAB - 1);
    Pipe *pp = pipe_tab[pslot].pipe;
    if (!pp) {
        pp = warm_pipe_find(ph);  // built (or being built) by the startup warmup from a recipe of an earlier run
        if (pp) {
            stat_warm_pipe_hits++;
            if (atomic_load(&pp->state) == 1) sess.reused_ready++;
            else sess.reused_building++;
        } else {
            pp = calloc(1, sizeof *pp);
            pp->k = k;
            run_job(build_pipeline, pp);
            recipe_note(pr, &k);  // (for the next start's warmup)
            stat_new_pipes++;
            sess.missed++;
        }
        pipe_tab[pslot].key = ph;
        pipe_tab[pslot].pipe = pp;
    }
    if (atomic_load(&pp->state) != 1 && !compile_wait(r, indexed, &pp->state, NULL, pp)) {
        SKIP(atomic_load(&pp->state) != 2 ? SKIP_PIPE_PENDING : SKIP_PIPE_FAILED);  // (it may just have become ready)
        return;
    }
    {   // (compile_wait: a target drawn in the previous frame is redrawn next frame; one that wasn't may not be)
        extern uint64_t vk_frame_counter(void);
        uint64_t f = vk_frame_counter() + 1;
        for (int i = 0; i < st.rt.ncolor; i++)
            if (st.rt.color[i] && st.rt.color[i]->backend) ((TexBackend *)st.rt.color[i]->backend)->drawn_frame = f;
        if (st.rt.depth && st.rt.depth->backend) ((TexBackend *)st.rt.depth->backend)->drawn_frame = f;
    }
    ss_enabled();
    VkPipeline draw_pipe = atomic_load(&pp->optimized_state) == 1 ? pp->optimized : pp->pipe;
    if (ss_need(SS_PIPE, ss.pipe == draw_pipe)) { ss.pipe = draw_pipe; vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, draw_pipe); }
    uint64_t prof_td_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_key += prof_td_ - prof_tk_;

    // ---- depth / stencil (dynamic)
    static int nodt = -1;
    if (nodt < 0) nodt = getenv("EOWRECOMP_NO_DEPTH_TEST") != NULL;
    bool dt = st.ds[0] && !nodt && pass_depth;
    if (ss_need(SS_DT, ss.dt == (VkBool32)dt)) { ss.dt = dt; vkCmdSetDepthTestEnable(cb, dt); }
    VkBool32 dw = dt && st.ds[1];
    if (ss_need(SS_DW, ss.dw == dw)) { ss.dw = dw; vkCmdSetDepthWriteEnable(cb, dw); }
    VkCompareOp dop = dt ? compare_fn(st.ds[2]) : VK_COMPARE_OP_ALWAYS;
    // EQUAL against a depth pre-pass written by a different vertex shader (foliage, trees) needs both shaders to land
    // on identical depths. They do since the translator stopped using unpackHalf2x16 (NVIDIA ran the half math after
    // it in fp16, differently per shader: black slashes in foliage). EOWRECOMP_VK_EQUAL_TOL=1 brings back the old
    // workaround: LESS_OR_EQUAL with a few ulps of bias towards the camera, which also let walls paint over the
    // plants stuck flat on them (beach) - so it is off.
    static int tol = -1;
    if (tol < 0) tol = getenv("EOWRECOMP_VK_EQUAL_TOL") != NULL;
    bool equal_tol = dop == VK_COMPARE_OP_EQUAL && tol;
    if (equal_tol) dop = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (ss_need(SS_DOP, ss.dop == dop)) { ss.dop = dop; vkCmdSetDepthCompareOp(cb, dop); }
    bool sten = st.ds[3] && pass_stencil;
    if (ss_need(SS_STEN, ss.sten == (VkBool32)sten)) { ss.sten = sten; vkCmdSetStencilTestEnable(cb, sten); }
    if (sten) {
        uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
        for (int f = 0; f < 2; f++) {
            uint32_t o = f ? ops >> 12 : ops;
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            uint32_t v[7] = {stencil_op(o & 0xF), stencil_op((o >> 8) & 0xF), stencil_op((o >> 4) & 0xF),
                             compare_fn(f ? st.ds[4] >> 4 : st.ds[4] & 0xF), st.stencil_vmask[f] & 0xFF, st.stencil_mask[f] & 0xFF,
                             st.stencil_ref[f]};
            if (!ss_need(f ? SS_SFACE1 : SS_SFACE0, !memcmp(v, ss.sface[f], sizeof v))) continue;
            memcpy(ss.sface[f], v, sizeof v);
            vkCmdSetStencilOp(cb, face, (VkStencilOp)v[0], (VkStencilOp)v[1], (VkStencilOp)v[2], (VkCompareOp)v[3]);
            vkCmdSetStencilCompareMask(cb, face, v[4]);
            vkCmdSetStencilWriteMask(cb, face, v[5]);
            vkCmdSetStencilReference(cb, face, v[6]);
        }
    }
    if (ss_need(SS_BC, !memcmp(ss.bc, st.blend_color, sizeof ss.bc))) { memcpy(ss.bc, st.blend_color, sizeof ss.bc); vkCmdSetBlendConstants(cb, st.blend_color); }

    // ---- rasterizer
    uint8_t cull = st.poly[0];
    VkCullModeFlags cm = cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    if (ss_need(SS_CULL, ss.cull == cm)) { ss.cull = cm; vkCmdSetCullMode(cb, cm); }
    bool ccw = st.poly[1] == 1;
    static int flipw = -1;  // EOWRECOMP_VK_FLIP_WINDING: invert the front-face mapping (A/B against Metal)
    if (flipw < 0) flipw = getenv("EOWRECOMP_VK_FLIP_WINDING") != NULL;
    bool front_ccw = (ccw != vkey.flip_y) != (flipw != 0);
    VkFrontFace ff = front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    if (ss_need(SS_FF, ss.ff == ff)) { ss.ff = ff; vkCmdSetFrontFace(cb, ff); }
    VkPrimitiveTopology tp = native ? topo : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    if (ss_need(SS_TOPO, ss.topo == tp)) { ss.topo = tp; vkCmdSetPrimitiveTopology(cb, tp); }
    VkBool32 pre = st.prim_restart && native && (mode == 3 || mode == 5);
    if (ss_need(SS_PRE, ss.pre == pre)) { ss.pre = pre; vkCmdSetPrimitiveRestartEnable(cb, pre); }
    {
        float f[3] = {0, 0, 0};
        static int off = -1;
        if (off < 0) off = getenv("EOWRECOMP_NO_DEPTH_BIAS") != NULL;
        bool on = (st.poly[3] & 4) && !off;
        if (on) memcpy(f, st.poly_offset, sizeof f);
        if (equal_tol) { f[1] -= 8; on = true; }  // (see the EQUAL comment above: a few depth ulps nearer)
        if (ss_need(SS_DBE, ss.dbe == (VkBool32)on)) { ss.dbe = on; vkCmdSetDepthBiasEnable(cb, on); }
        if (ss_need(SS_DB, !memcmp(ss.db, f, sizeof f))) { memcpy(ss.db, f, sizeof f); vkCmdSetDepthBias(cb, f[1], f[2], f[0]); }
    }

    // ---- textures first (their scales feed the widescreen decision and the rscale table), then the viewport
    float *vp = st.viewport[0];
    float zn = st.depth_range[0][0], zf = st.depth_range[0][1];
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    float tint[8] = {0};  // (per-draw tint, binding 19: no EoW feature uses it yet)
    // fragment textures are bound before deciding the viewport (tex_sc / tex_wh), rscale depends on the decision:
    // build the vertex set now and the fragment set after the decision (stage_set fills tex_sc for stage 1).
    if (prof) prof_dyn += SDL_GetTicksNS() - prof_td_;
    PROF_T0();
    VkDescriptorSet vset = stage_set(pr, pr->set_layout[0], 0, 0, pr->vi, NULL, NULL, 0, 0, 0, NULL);
    PROF_ADD(prof_sets);
    VkDescriptorImageInfo fii[64];
    uint64_t prof_tx_ = prof ? SDL_GetTicksNS() : 0;
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
        float sc[2]; uint32_t wh[2];
        texture_info(1, pr->fi, i, &fii[i], sc, wh);
        tex_sc[1][i][0] = sc[0]; tex_sc[1][i][1] = sc[1]; tex_wh[1][i][0] = wh[0]; tex_wh[1][i][1] = wh[1];
    }
    if (prof) prof_tex += SDL_GetTicksNS() - prof_tx_;
    if (fabsf(pass_sx - pass_sy) > 0.01f * pass_sy && st.rt.ncolor && renderer_texture_presented(st.rt.color[0])) {
        // Draws into the swapchain image (see renderer.m): composites fill the window, UI goes in a centred 16:9 box.
        bool reads_scene = false;
        for (int i = 0; i < pr->fi->ntex && i < 64; i++)
            if ((tex_sc[1][i][0] != 1 || tex_sc[1][i][1] != 1) && tex_wh[1][i][0] == st.rt.color[0]->width &&
                tex_wh[1][i][1] == st.rt.color[0]->height)
                reads_scene = true;
        bool fullscreen_quad = r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
                               st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height;
        bool fullscreen_fill = pr->fi->ntex == 0 && fullscreen_quad;
        if (fullscreen_quad && !reads_scene && pr->fi->ntex == 1 && (tex_sc[1][0][0] != 1 || tex_sc[1][0][1] != 1)) reads_scene = true;
        static uint64_t wide_frame = ~0ull;
        if (reads_scene) wide_frame = renderer_present_count();
        bool fade_fill = false;
        if (fullscreen_fill && st.ubuf[1][0].addr && st.ubuf[1][0].size >= 12) {
            const float *c = (const float *)(uintptr_t)st.ubuf[1][0].addr;
            bool black = c[0] < 0.01f && c[1] < 0.01f && c[2] < 0.01f, white = c[0] > 0.99f && c[1] > 0.99f && c[2] > 0.99f;
            fade_fill = black || white;
        }
        bool composite = reads_scene || fade_fill || (fullscreen_fill && wide_frame == renderer_present_count());
        if (!composite) wide_frame = ~0ull;
        if (!composite) {
            int pw, ph;
            pass_size(&pw, &ph);
            float u = fminf(pass_sx, pass_sy);
            vp_sx = vp_sy = u;
            vp_ox = (pw - st.rt.color[0]->width * u) * 0.5f;
            vp_oy = (ph - st.rt.color[0]->height * u) * 0.5f;
        }
    }
    {
        float k = 1.0f;  // (LAS Recomp widened 16:9 camera effects here; EoW's widescreen is not done yet)
        float vx = vp[0] * vp_sx + vp_ox, vw = vp[2] * vp_sx;
        set_viewport(vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, zn, zf);
        if (g_debug_frame) {
            VkRect2D sr = scissor_rect();
            LOG("[draw]   vp %.2f %.2f %.2f %.2f (stretch %.3f, fs hash %016llx) scissor %d %d %u %u | guest sc %d %d %d %d | pass %ux%u s %.4f %.4f\n",
                vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, k, (unsigned long long)g_cur_fs_hash, sr.offset.x, sr.offset.y,
                sr.extent.width, sr.extent.height, st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], pass_w, pass_h,
                vp_sx, vp_sy);
        }
    }
    apply_scissor();
    float rs0[4] = {vp_sx, vp_sy, vp_ox, vp_oy};
    uint64_t prof_t1_ = prof ? SDL_GetTicksNS() : 0;
    VkDescriptorSet fset = stage_set(pr, pr->set_layout[1], 1, 1, pr->fi, rs0, tint, 0, 0, 0, fii);
    if (prof) prof_sets += SDL_GetTicksNS() - prof_t1_;
    VkDescriptorSet sets[2] = {vset, fset};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 0, 2, sets, 0, NULL);  // (always new sets)
    if (pr->vi->uses_gmem_store) { vc_mark_sbufs(0); gstore_note(0); }
    if (pr->fi->uses_gmem_store) { vc_mark_sbufs(1); gstore_note(1); }

    uint64_t prof_t2_ = prof ? SDL_GetTicksNS() : 0;
    // ---- indices (copied; the largest index bounds the vertex data copied below)
    uint32_t first = 0, count = (uint32_t)r->x[2], inst, base_inst, max_index = 0;
    int32_t base_vertex = 0;
    VkSpan ib = {0};
    VkIndexType itype_vk = VK_INDEX_TYPE_UINT32;
    uint32_t icount = 0;
    bool use_index = indexed || !native;
    const void *scan_src = NULL;  // deferred index scan (direct index buffer)
    uint32_t scan_isz = 0, scan_restart = 0;
    if (!indexed) {
        first = (uint32_t)r->x[1]; base_inst = (uint32_t)r->x[3]; inst = (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 1;
        max_index = first + count;
        if (!native) {
            uint32_t *e = expand_indices(mode, count, NULL, 0, first, &icount);
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
    } else {
        uint32_t itype = (uint32_t)r->x[1];
        uint64_t iaddr = r->x[3];
        base_vertex = (int32_t)r->x[4];
        base_inst = (uint32_t)r->x[5];
        inst = (uint32_t)r->x[6] ? (uint32_t)r->x[6] : 1;
        const void *src = (const void *)(uintptr_t)iaddr;
        uint32_t restart = st.prim_restart ? st.restart_index : 0xFFFFFFFFu;
        if (native && itype != 0) {
            uint32_t isz = itype == 1 ? 2 : 4;
            itype_vk = itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            icount = count;
            if (iaddr % isz == 0 && guest_direct(iaddr, (uint64_t)count * isz, &ib.buf, &ib.off)) {
                scan_src = src; scan_isz = isz; scan_restart = restart;  // max_index only if a stream gets copied
            } else {
                ib = vk_transient_copy(src, (uint64_t)count * isz + 4, 4);
                if (isz == 2) { const uint16_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != (uint16_t)restart) max_index = s[i]; }
                else { const uint32_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != restart) max_index = s[i]; }
            }
        } else if (native) {  // 8-bit indices: widen to 16
            ib = vk_transient((uint64_t)count * 2 + 4, 4);
            uint16_t *w = ib.ptr;
            const uint8_t *s = src;
            for (uint32_t i = 0; i < count; i++) { w[i] = s[i] == 0xFF && st.prim_restart ? 0xFFFF : s[i]; if (s[i] > max_index && w[i] != 0xFFFF) max_index = s[i]; }
            itype_vk = VK_INDEX_TYPE_UINT16;
            icount = count;
        } else {
            uint32_t *e = expand_indices(mode, count, src, (int)itype, 0, &icount);
            for (uint32_t i = 0; i < icount; i++) if (e[i] > max_index) max_index = e[i];
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
        if (!scan_src) {
            max_index += 1;
            if (base_vertex > 0) max_index += (uint32_t)base_vertex;
        }
    }
    if (use_index && ss_need(SS_IB, ss.ib == ib.buf && ss.io == ib.off && ss.it == itype_vk)) {
        ss.ib = ib.buf; ss.io = ib.off; ss.it = itype_vk;
        vkCmdBindIndexBuffer(cb, ib.buf, ib.off, itype_vk);
    }

    // ---- vertex streams: the range this draw can read, copied once per submission
    for (int s = 0; s < 17; s++) {
        if (!(k.used_streams & (1u << s))) continue;
        VkSpan sp;
        if (s == ZERO_BINDING) {
            static const float zero[4] = {0, 0, 0, 1};
            sp = vk_transient_copy(zero, 16, 16);
        } else {
            Range v = st.vbuf[s];
            VkBuffer dbuf;
            VkDeviceSize doff;
            if (v.addr && v.size && guest_direct(v.addr, v.size, &dbuf, &doff)) {
                if (ss_on && (ss.vb_known >> s & 1) && ss.vb[s] == dbuf && ss.vo[s] == doff) continue;
                ss.vb[s] = dbuf; ss.vo[s] = doff; ss.vb_known |= 1u << s;
                vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &dbuf, &doff);
                continue;
            }
            if (scan_src) {  // copying after all: the index scan bounds the copy
                if (scan_isz == 2) { const uint16_t *q = scan_src; for (uint32_t i = 0; i < icount; i++) if (q[i] > max_index && q[i] != (uint16_t)scan_restart) max_index = q[i]; }
                else { const uint32_t *q = scan_src; for (uint32_t i = 0; i < icount; i++) if (q[i] > max_index && q[i] != scan_restart) max_index = q[i]; }
                max_index += 1;
                if (base_vertex > 0) max_index += (uint32_t)base_vertex;
                scan_src = NULL;
            }
            uint64_t stride = k.stride[s], need;
            if (!stride) need = 256;
            else if (k.divisor[s]) need = ((uint64_t)base_inst + inst + k.divisor[s] - 1) / k.divisor[s] * stride + 256;
            else need = (uint64_t)max_index * stride + 256;
            if (v.size && need > v.size) need = v.size;
            if (!v.addr) { static const uint8_t zero[256]; sp = vk_transient_copy(zero, 256, 16); }
            else sp = guest_copy(v.addr, need ? need : 16);
        }
        VkDeviceSize off = sp.off;
        if (ss_on && (ss.vb_known >> s & 1) && ss.vb[s] == sp.buf && ss.vo[s] == off) continue;
        ss.vb[s] = sp.buf; ss.vo[s] = off; ss.vb_known |= 1u << s;
        vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &sp.buf, &off);
    }

    if (prof) prof_vtx += SDL_GetTicksNS() - prof_t2_;
    // ---- draw
    // Draws can be split into runs of instances, each with its own fragment set (LAS Recomp tinted some; one run here.
    // gl_InstanceIndex includes the base instance, so a run [b + k, b + k + n) draws exactly those).
    enum { MAX_RUNS = 64 };
    uint32_t run_at[MAX_RUNS], run_n[MAX_RUNS];
    bool run_gold[MAX_RUNS];
    int nruns = 0;
    if (!nruns) { run_at[0] = 0; run_n[0] = inst; nruns = 1; run_gold[0] = false; }
    for (int ri = 0; ri < nruns; ri++) {
        if (nruns > 1 || run_gold[0]) {  // this run's tint: a fragment set of its own
            float rtint[8] = {0};
            VkDescriptorSet rset = stage_set(pr, pr->set_layout[1], 1, 1, pr->fi, rs0, rtint, 0, 0, 0, fii);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 1, 1, &rset, 0, NULL);
        }
        if (!use_index) vkCmdDraw(cb, count, run_n[ri], first, base_inst + run_at[ri]);
        else vkCmdDrawIndexed(cb, icount, run_n[ri], 0, base_vertex, base_inst + run_at[ri]);
    }
    pass_sampled_targets |= sampled_targets;
    pass_written_targets |= written_targets;
    pass_draw_count++;
    stat_draws++;
    cum_draws++;
    if (g_gpuprof_frame && gp_n) {
        if (!gp_draws[gp_n - 1]) {  // first draw of the row: its fragment shader hash
            size_t l = strlen(gp_label[gp_n - 1]);
            snprintf(gp_label[gp_n - 1] + l, sizeof gp_label[0] - l, ", first fs %016llx", (unsigned long long)g_cur_fs_hash);
        }
        gp_draws[gp_n - 1]++;
    }
}

static void debug_draw(Prog *pr, CmdRec *r, bool indexed) {
    LOG("[draw] %s inst %llu mode %llu count %llu prog vs=%llx fs=%llx | vs cb %#x fs cb %#x | rt %d", indexed ? "e" : "a",
        (unsigned long long)(indexed ? r->x[6] : r->x[4]), (unsigned long long)r->x[0], (unsigned long long)r->x[2],
        (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
        pr->vi->cbuf_mask, pr->fi->cbuf_mask, st.rt.ncolor);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) LOG(" %ux%u/f%#x", st.rt.color[i]->width, st.rt.color[i]->height, st.rt.color[i]->format);
    if (st.rt.depth) LOG(" d%#x %p", st.rt.depth->format, (void *)st.rt.depth);
    LOG(" | vp %.0f %.0f %.0f %.0f | attrs %#x blend %#x ds %02x%02x%02x poly %02x%02x chmask %08x\n", st.viewport[0][0], st.viewport[0][1],
        st.viewport[0][2], st.viewport[0][3], pr->vi->attr_mask, st.color_state[0], st.ds[0], st.ds[1], st.ds[2], st.poly[0], st.poly[1],
        st.channel_mask);
    if (getenv("EOWRECOMP_DEBUG_DATA")) {  // vertex streams 0-1 and fragment cbufs: the first floats
        for (int s = 0; s < 2; s++) {
            if (!st.vbuf[s].addr) continue;
            const float *v = (const float *)(uintptr_t)st.vbuf[s].addr;
            LOG("[draw]   vbuf%d %llx size %llu:", s, (unsigned long long)st.vbuf[s].addr, (unsigned long long)st.vbuf[s].size);
            for (int k = 0; k < 16; k++) LOG(" %.4g", v[k]);
            LOG("\n");
        }
        for (int c = 0; c < 18; c++) {
            if (!((pr->fi->cbuf_mask >> c) & 1) || c < 3) continue;

            // Shader cbuf indices 3+ map to NVN uniform-buffer slots starting at slot 0.
            // Keep both indices in the log: using `c` directly here used to print a different
            // buffer from the one cbuf_info() actually bound to Vulkan.
            int ubo_slot = c - 3;
            Range range = ubo_slot < 18 ? st.ubuf[1][ubo_slot] : (Range){0, 0};
            uint64_t size = pr->fi->cbuf_size[c] ? pr->fi->cbuf_size[c] : 256;
            if ((pr->fi->cbuf_dynamic & (1u << c)) && range.size)
                size = range.size > 0x10000 ? 0x10000 : range.size;
            if (size > range.size && range.size) size = range.size;
            if (size < 16) size = 16;

            LOG("[draw]   fcb%d <- ubo%d addr %llx range %llu cbuf-size %llu:", c, ubo_slot,
                (unsigned long long)range.addr, (unsigned long long)range.size, (unsigned long long)size);
            if (range.addr) {
                const float *u = (const float *)(uintptr_t)range.addr;
                for (int k = 0; k < 96 && (uint64_t)k * 4 < size; k++) LOG(" %.6g", u[k]);
                if (c == 12 && range.size >= 0x28) {
                    const uint32_t *words = (const uint32_t *)(uintptr_t)range.addr;
                    LOG(" | cb12[2].y@0x24=0x%08x", words[0x24 / 4]);
                }
            } else {
                LOG(" <zero-filled>");
            }
            LOG("\n");
        }
    }
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {  // fragment textures: guest size / format, allocation, scale
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        if (!t) { LOG("[draw]   tex%d: none\n", i); continue; }
        LOG("[draw]   tex%d: %p %ux%u f%#x lv %u -> %ux%u scale %.4f %.4f%s\n", i, (void *)t, t->width, t->height, t->format,
            b ? b->levels : 0, b ? b->width : 0, b ? b->height : 0, b ? b->sx : 0, b ? b->sy : 0, b && b->scalable ? " scalable" : "");
    }
}

// ================================================================== texture copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRefRec;

static void copy_region_dims(NvnTexture *t, const TexRefRec *ref, const FormatInfo **fi, int32_t rg[6], int *level,
                             uint64_t *row, uint64_t *img) {
    *fi = nvn_format_info(t->format);
    memcpy(rg, ref->region, sizeof ref->region);
    if (!rg[3]) rg[3] = (int32_t)t->width;
    if (!rg[4]) rg[4] = (int32_t)t->height;
    if (!rg[5]) rg[5] = 1;
    *level = ref->has_view && (ref->view.setmask & 1) ? ref->view.base_level : 0;
    uint32_t wb = ((uint32_t)rg[3] + (*fi)->bw - 1) / (*fi)->bw, hb = ((uint32_t)rg[4] + (*fi)->bh - 1) / (*fi)->bh;
    *row = st.copy_row_stride ? st.copy_row_stride : (uint64_t)wb * (*fi)->bpb;
    *img = st.copy_image_stride ? st.copy_image_stride : *row * hb;
}

static void copy_buffer_to_texture(uint64_t src, const TexRefRec *dst) {
    snap_reason = "buf2tex"; snapshots_invalidate(NULL);
    NvnTexture *t = (NvnTexture *)(uintptr_t)dst->tex;
    if (!t || !src) return;
    alias_before_write(t, 0, "upload");
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, dst, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    TexBackend *b = vk_tex(t, false);  // prior CPU contents uploaded first
    renderer_mark_gpu_owned(t);
    if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return;
    if (b->astc_bw) {  // (blocks would need the CPU decode: not seen in the game yet)
        static int nlog;
        if (nlog++ < 5) LOG("[vk] buffer -> ASTC texture copy skipped (%ux%u)\n", t->width, t->height);
        return;
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t hb = ((uint32_t)rg[4] + fi->bh - 1) / fi->bh, wb = ((uint32_t)rg[3] + fi->bw - 1) / fi->bw;
    uint32_t out_bpb = fi->bpb == 3 ? 4 : fi->bpb;
    uint32_t nz = (uint32_t)rg[5];
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    uint64_t out_row = (uint64_t)wb * out_bpb;
    VkSpan s = vk_transient(out_row * hb * nz, 16);
    for (uint32_t z = 0; z < nz; z++)
        for (uint32_t y = 0; y < hb; y++) {
            const uint8_t *sr = (const uint8_t *)(uintptr_t)(src + (uint64_t)z * img + (uint64_t)y * row);
            uint8_t *dr = (uint8_t *)s.ptr + ((uint64_t)z * hb + y) * out_row;
            if (out_bpb == fi->bpb) memcpy(dr, sr, out_row);
            else for (uint32_t x = 0; x < wb; x++) { memcpy(dr + x * 4, sr + x * 3, 3); dr[x * 4 + 3] = 255; }
        }
    VkBufferImageCopy c = {.bufferOffset = s.off, .bufferRowLength = wb * fi->bw, .bufferImageHeight = hb * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

static void copy_texture_to_buffer(const TexRefRec *src, uint64_t dst) {
    NvnTexture *t = (NvnTexture *)(uintptr_t)src->tex;
    if (!t || !dst || dst < HEAP_BASE) return;
    t = alias_read(t);
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, src, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb || fi->bpb == 3 || row % fi->bpb) return;
    TexBackend *b = vk_tex(t, false);
    float tsx, tsy;
    renderer_texture_scale(t, &tsx, &tsy);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImage srcimg = b->image;
    VmaAllocation small_mem = NULL;
    if ((tsx != 1 || tsy != 1) && level == 0 && !renderer_format_is_depth(t->format)) {
        // the CPU expects guest-sized texels: filter the scaled allocation down first
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = b->format,
                                .extent = {t->width, t->height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &srcimg, &small_mem, NULL));
        vk_image_init_layout(cb, srcimg, VK_IMAGE_ASPECT_COLOR_BIT);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)b->width, (int32_t)b->height, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)t->width, (int32_t)t->height, 1}}};
        vkCmdBlitImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, srcimg, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        vk_full_barrier(cb);
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t nz = (uint32_t)rg[5];
    uint64_t total = img * nz;
    VkSpan out = vk_readback(total);
    VkBufferImageCopy c = {.bufferOffset = out.off, .bufferRowLength = (uint32_t)(row / fi->bpb) * fi->bw,
                           .bufferImageHeight = (uint32_t)(img / row) * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyImageToBuffer(cb, srcimg, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    wait_why = "texture -> buffer readback";
    renderer_flush();  // the CPU may read the result right after the fence
    memcpy((void *)(uintptr_t)dst, out.ptr, total);
    if (small_mem) vmaDestroyImage(g_vk.vma, srcimg, small_mem);
}

static void copy_texture(const TexRefRec *src, const TexRefRec *dst, uint64_t flags) {
    snap_reason = "tex2tex"; snapshots_invalidate(NULL);
    NvnTexture *st_ = (NvnTexture *)(uintptr_t)src->tex, *dt = (NvnTexture *)(uintptr_t)dst->tex;
    if (!st_ || !dt) return;
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (EOWRECOMP_VK_DEVLOST_TRACE=2: each copy its own submission, waited on)
    gp_mark(vk_cmd(), "copy texture %ux%u -> %ux%u", st_->width, st_->height, dt->width, dt->height);
    dl_note("copy texture %ux%u f%#x -> %ux%u f%#x (%p -> %p)", st_->width, st_->height, st_->format, dt->width, dt->height,
            dt->format, (void *)st_, (void *)dt);
    TexBackend *s = vk_tex(st_, false), *d = vk_tex(dt, true);
    renderer_mark_gpu_owned(dt);
    if (renderer_texture_presented(dt) && st_->width == dt->width && st_->height == dt->height) renderer_texture_mark_presented(st_);
    renderer_texture_first_write(dt);
    renderer_texture_level_fresh(dt);
    const int32_t *sr = src->region, *dr = dst->region;
    float ssx, ssy, dsx, dsy;
    renderer_texture_scale(st_, &ssx, &ssy);
    renderer_texture_scale(dt, &dsx, &dsy);
    int sx0 = (int)lroundf(sr[0] * ssx), sy0 = (int)lroundf(sr[1] * ssy);
    int dx0 = (int)lroundf(dr[0] * dsx), dy0 = (int)lroundf(dr[1] * dsy);
    int sw = sr[3] ? (int)lroundf(sr[3] * ssx) : (int)s->width, sh = sr[4] ? (int)lroundf(sr[4] * ssy) : (int)s->height;
    int dw = dr[3] ? (int)lroundf(dr[3] * dsx) : (int)d->width, dh = dr[4] ? (int)lroundf(dr[4] * dsy) : (int)d->height;
    uint32_t slv = src->has_view && (src->view.setmask & 1) ? src->view.base_level : 0;
    uint32_t dlv = dst->has_view && (dst->view.setmask & 1) ? dst->view.base_level : 0;
    uint32_t sly = src->has_view && (src->view.setmask & 2) ? src->view.min_layer : 0;
    uint32_t dly = dst->has_view && (dst->view.setmask & 2) ? dst->view.min_layer : 0;
    if (slv >= s->levels || dlv >= d->levels || sly >= s->layers || dly >= d->layers) return;
    {   // whole-image regions are the level's extent, and every region stays inside its level: scaled allocations
        // (internal resolution below 1x) round guest regions past the edge, and out-of-bounds copies / blits are
        // undefined in Vulkan (garbage that changed every frame on NVIDIA: flicker in the depth pyramid's users)
        int slw = (int)level_extent(s->width, slv), slh = (int)level_extent(s->height, slv);
        int dlw = (int)level_extent(d->width, dlv), dlh = (int)level_extent(d->height, dlv);
        if (!sr[3]) sw = slw - sx0;
        if (!sr[4]) sh = slh - sy0;
        if (!dr[3]) dw = dlw - dx0;
        if (!dr[4]) dh = dlh - dy0;
        if (sx0 + sw > slw) sw = slw - sx0;
        if (sy0 + sh > slh) sh = slh - sy0;
        if (dx0 + dw > dlw) dw = dlw - dx0;
        if (dy0 + dh > dlh) dh = dlh - dy0;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    }
    if (g_debug_frame || ct_on())
        LOG("[copytex] %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] -> %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] phys (%d,%d %dx%d) -> (%d,%d %dx%d) flags %llx\n",
            (void *)st_, st_->width, st_->height, st_->format, slv, sr[0], sr[1], sr[3], sr[4], s->width, s->height, (void *)dt, dt->width,
            dt->height, dt->format, dlv, dr[0], dr[1], dr[3], dr[4], d->width, d->height, sx0, sy0, sw, sh, dx0, dy0, dw, dh,
            (unsigned long long)flags);
    if (copy_opt() && sx0 == 0 && sy0 == 0 && dx0 == 0 && dy0 == 0 && sw == (int)s->width && sh == (int)s->height && dw == (int)d->width &&
        dh == (int)d->height && slv == 0 && dlv == 0 && sly == 0 && dly == 0 && alias_make(st_, dt, s, d)) {
        if (ct_on()) LOG("[ct] alias made %p -> %p\n", (void *)st_, (void *)dt);
        return;
    }
    alias_before_write(dt, 0, "copy dst");
    if (naliases) s = alias_read(st_)->backend;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageAspectFlags sa = s->aspect, da = d->aspect;
    if (s->samples > 1 && d->samples == 1 && sw == dw && sh == dh && s->format == d->format && !(sa & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        VkImageResolve rv = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                             .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdResolveImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rv);
        image_transfer_write_barrier(cb, d->image, d->format, da, dlv, dly);
        return;
    }
    if (s->samples != d->samples) return;
    if (sw == dw && sh == dh && s->format == d->format) {
        VkImageCopy c = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                         .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        image_transfer_write_barrier(cb, d->image, d->format, da, dlv, dly);
        return;
    }
    // scaled / converting copy: the game builds its depth pyramid this way (nearest), colour copies filter if asked
    bool depth = (sa | da) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (depth && s->format != d->format) return;
    VkImageBlit bl = {.srcSubresource = {depth ? (sa & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : sa) : sa, slv, sly, 1},
                      .srcOffsets = {{sx0, sy0, 0}, {sx0 + sw, sy0 + sh, 1}},
                      .dstSubresource = {depth ? (da & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : da) : da, dlv, dly, 1},
                      .dstOffsets = {{dx0, dy0, 0}, {dx0 + dw, dy0 + dh, 1}}};
    bool linear = (flags & 1) && !depth && !rt_kind(st_->format);
    dl_note("  blit %p lv%u/%u %ux%u (%d,%d %dx%d) -> %p lv%u/%u %ux%u (%d,%d %dx%d) %s", (void *)s->image, slv, s->levels, s->width,
            s->height, sx0, sy0, sw, sh, (void *)d->image, dlv, d->levels, d->width, d->height, dx0, dy0, dw, dh, linear ? "linear" : "nearest");
    vkCmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    image_transfer_write_barrier(cb, d->image, d->format, bl.dstSubresource.aspectMask, dlv, dly);
}

// ================================================================== present helpers (device.c)
static VkPipelineLayout present_layout;
static VkDescriptorSetLayout present_set_layout;

static VkPipeline present_pipeline(VkFormat fmt, bool alpha_only) {
    static struct { VkFormat f; VkPipeline p; } cache[2][4];
    int mode = alpha_only ? 1 : 0;
    for (int i = 0; i < 4; i++) if (cache[mode][i].p && cache[mode][i].f == fmt) return cache[mode][i].p;
    if (!present_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &present_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &present_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &present_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("present.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(alpha_only ? "present_alpha.frag" : "present.frag"), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = present_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 4; i++) if (!cache[mode][i].p) { cache[mode][i].f = fmt; cache[mode][i].p = p; break; }
    return p;
}

// Scanout shows the bytes the game wrote: sRGB images are read through a UNORM view (no decode), like metal.m.
static VkImageView raw_view(TexBackend *b) {
    VkFormat f = b->format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
               : b->format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : b->format;
    static const VkComponentMapping id = {0};
    return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT, "scanout raw view");
}

// ---- post passes (Video tab "Anti-aliasing" / "Upscaler"): SMAA 1x at the game image's size, then AMD FSR 1 (EASU
// upscale to the image's size in the window, RCAS sharpen) when it is smaller than its place in the window, as
// metal.m does with MetalFX. Each pass: a fullscreen triangle into an RGBA8 image (gamma-encoded, like the input);
// up to three inputs (bindings 0-2), sampled linear / clamp.
static VkPipelineLayout post_layout;
static VkDescriptorSetLayout post_set_layout;

static VkPipeline post_pipeline(const char *fs) {
    static struct { const char *fs; VkPipeline p; } cache[8];
    for (int i = 0; i < 8; i++) if (cache[i].p && cache[i].fs == fs) return cache[i].p;
    if (!post_layout) {
        VkDescriptorSetLayoutBinding b[3];
        for (uint32_t i = 0; i < 3; i++)
            b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &post_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &post_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &post_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("post.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fs), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = post_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 8; i++) if (!cache[i].p) { cache[i].fs = fs; cache[i].p = p; break; }
    return p;
}

// Post image `i` at w x h (RGBA8, kept until the size changes). 0, 1: SMAA edges / weights, 2: SMAA output,
// 3, 4: FSR EASU / RCAS.
static TexBackend *post_image(int i, uint32_t w, uint32_t h) {
    static TexBackend img[5];
    TexBackend *d = &img[i];
    if (d->image && (d->width != w || d->height != h)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = VK_FORMAT_R8G8B8A8_UNORM; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

static VkImageView post_view(TexBackend *d) {
    static const VkComponentMapping id = {0};
    return vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, d->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT, "post-process image");
}

// SMAA's lookup textures (runtime/nvn/smaa_data.c): 0 = area (160 x 560 R8G8), 1 = search (64 x 16 R8), uploaded once.
static VkImageView smaa_lut(int i) {
    static TexBackend t[2];
    TexBackend *d = &t[i];
    if (!d->image) {
        extern const unsigned char *smaa_area_tex(void);
        extern const unsigned char *smaa_search_tex(void);
        uint32_t w = i ? 64 : 160, h = i ? 16 : 560, bpp = i ? 1 : 2;
        VkFormat f = i ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8_UNORM;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = f; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        VkCommandBuffer cb = vk_cmd();
        vk_image_init_layout(cb, d->image, d->aspect);
        VkSpan s = vk_transient_copy(i ? smaa_search_tex() : smaa_area_tex(), (uint64_t)w * h * bpp, 16);
        VkBufferImageCopy c = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyBufferToImage(cb, s.buf, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    return post_view(d);
}

// One post pass (outside the window image's rendering): srcs (bindings 0..n-1) -> dst, fragment shader `fs`.
// clear: start from zero (SMAA's edge pass only writes edge pixels; its weights / output read them).
static void post_pass(VkCommandBuffer cb, const char *fs, const VkImageView *srcs, int n, TexBackend *dst, const float pc[8],
                      bool clear) {
    VkPipeline pipe = post_pipeline(fs);  // (before the rendering begins: created inside it, the NVIDIA driver crashed)
    vk_full_barrier(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = post_view(dst),
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {dst->width, dst->height}},
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkDescriptorSet ds = vk_alloc_set(post_set_layout, 0);
    VkDescriptorImageInfo ii[3];
    VkWriteDescriptorSet w[3];
    for (int i = 0; i < n; i++) {
        ii[i] = (VkDescriptorImageInfo){vk_default_sampler(true), srcs[i], VK_IMAGE_LAYOUT_GENERAL};
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .pImageInfo = &ii[i]};
    }
    vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)dst->width, (float)dst->height, 0, 1};
    VkRect2D sc = {{0, 0}, {dst->width, dst->height}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdPushConstants(cb, post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}

// The view the present samples: the game image, or its anti-aliased / upscaled version. ow x oh: the whole image's
// size in the window (its crop is drawn into the viewport).
static VkImageView post_process(VkCommandBuffer cb, TexBackend *b, double ow_d, double oh_d) {
    VkImageView v = raw_view(b);
    bool smaa = g_settings.antialias == AA_SMAA;
    uint32_t ow = (uint32_t)lround(ow_d), oh = (uint32_t)lround(oh_d);
    if (ow > 16384) ow = 16384;
    if (oh > 16384) oh = 16384;
    bool fsr = g_settings.upscaler == UPSCALE_FSR1 && (ow > b->width + 1 || oh > b->height + 1);  // (an upscale only)
    if (!smaa && !fsr) return v;
    extern void vk_frame_suspend(void);
    extern void vk_frame_resume(void);
    vk_frame_suspend();
    if (smaa) {  // SMAA 1x: luma edges -> blending weights -> neighbourhood blending
        float w = (float)b->width, h = (float)b->height;
        float m[8] = {1.0f / w, 1.0f / h, w, h};
        TexBackend *edges = post_image(0, b->width, b->height), *weights = post_image(1, b->width, b->height);
        TexBackend *out = post_image(2, b->width, b->height);
        VkImageView in[3] = {v};
        post_pass(cb, "smaa_edge.frag", in, 1, edges, m, true);
        VkImageView wi[3] = {post_view(edges), smaa_lut(0), smaa_lut(1)};
        post_pass(cb, "smaa_weight.frag", wi, 3, weights, m, true);
        VkImageView bi[3] = {v, post_view(weights)};
        post_pass(cb, "smaa_blend.frag", bi, 2, out, m, false);
        v = post_view(out);
    }
    if (fsr) {
        float iw = (float)b->width, ih = (float)b->height;
        TexBackend *e = post_image(3, ow, oh), *r = post_image(4, ow, oh);
        float pe[8] = {iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f, iw, ih};
        VkImageView ei[3] = {v};
        post_pass(cb, "easu.frag", ei, 1, e, pe, false);
        float pr[8] = {exp2f(-0.2f), 0, 0, 0, (float)ow, (float)oh};  // (sharpness 0.2 stops: AMD's default)
        VkImageView ri_[3] = {post_view(e)};
        post_pass(cb, "rcas.frag", ri_, 1, r, pr, false);
        v = post_view(r);
    }
    vk_full_barrier(cb);
    vk_frame_resume();
    {
        static uint32_t piw, pih, pow_, poh;
        static int psm = -1, pfs = -1;
        if (b->width != piw || b->height != pih || ow != pow_ || oh != poh || smaa != psm || fsr != pfs)
            LOG("[post] %ux%u%s%s -> %ux%u\n", b->width, b->height, smaa ? " smaa" : "", fsr ? " fsr1" : "", ow, oh);
        piw = b->width; pih = b->height; pow_ = ow; poh = oh; psm = smaa; pfs = fsr;
    }
    return v;
}

// Inside the window image's rendering (device.c vk_frame_begin): the game image into (vx, vy, vw, vh).
void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    VkImageView src = post_process(cb, b, pv[6] > 0 ? vw / pv[6] : vw, pv[7] > 0 ? vh / pv[7] : vh);
    VkPipeline p = present_pipeline(g_vk.sc_format, false);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout, 0);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    // (pre-rotated swapchain: the rectangle in the image's own coordinates, the triangle turned in the shader)
    VkRect2D r = vk_sc_rect((int)lroundf(vx), (int)lroundf(vy), (int)lroundf(vw), (int)lroundf(vh));
    VkViewport vp = {(float)r.offset.x, (float)r.offset.y, (float)r.extent.width, (float)r.extent.height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    float pq[8];
    memcpy(pq, pv, sizeof pq);
    pq[1] = (float)g_vk.sc_quarter;
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pq);
    vkCmdDraw(cb, 3, 1, 0, 0);
    VkViewport full = {0, 0, (float)g_vk.sc_phys.width, (float)g_vk.sc_phys.height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &full);
}

// Screenshot of a game texture (presented crop applied to build/frame_* shots), via the present shader into RGBA8.
static void renderer_save_texture_tex_mode(TexBackend *b, const char *path, bool alpha_only) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    extern const float *vk_shot_crop(void);
    float crop[4] = {0, 0, 1, 1};
    if (strstr(path, "frame_")) memcpy(crop, vk_shot_crop(), sizeof crop);
    uint32_t w = (uint32_t)(b->width * crop[2]), h = (uint32_t)(b->height * crop[3]);
    if (!w || !h) return;
    vk_end_pass();
    VkImage img;
    VmaAllocation mem;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL));
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView tv;
    VK_CHECK(vkCreateImageView(g_vk.dev, &vci, NULL, &tv));
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = tv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkPipeline p = present_pipeline(VK_FORMAT_R8G8B8A8_UNORM, alpha_only);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout, 0);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), raw_view(b), VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float pv[8] = {renderer_origin_lower_left() && !strstr(path, "window") ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_full_barrier(cb);
    VkSpan out = vk_readback((uint64_t)w * h * 4);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    vk_save_png(path, out.ptr, w, h);
    vkDestroyImageView(g_vk.dev, tv, NULL);
    vmaDestroyImage(g_vk.vma, img, mem);
}

void renderer_save_texture_tex(TexBackend *b, const char *path) {
    renderer_save_texture_tex_mode(b, path, false);
}

void renderer_save_texture_tex_alpha(TexBackend *b, const char *path) {
    renderer_save_texture_tex_mode(b, path, true);
}

// ================================================================== executor (renderer.m exec_list)
static void exec_list(uint64_t handle, int depth) {
    uint64_t p = handle;
    for (int guard = 0; guard < 10000000; guard++) {
        CmdRec *r = (CmdRec *)(uintptr_t)p;
        void *x = r->extra;
        if (r->op < CMD_COUNT) op_counts[r->op]++;
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            const uint64_t *h = x;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) exec_list(h[i], depth + 1);
            break;
        }
        case CMD_BIND_PROGRAM: st.program = (NvnProgram *)(uintptr_t)r->x[0]; break;
        case CMD_BIND_BLEND: { uint8_t *b = x; memcpy(st.blend[b[0] & 7], b, 8); break; }
        case CMD_BIND_CHANNEL_MASK: memcpy(&st.channel_mask, x, 4); break;
        case CMD_BIND_COLOR: memcpy(st.color_state, x, 4); break;
        case CMD_BIND_DEPTH_STENCIL: memcpy(st.ds, x, 8); break;
        case CMD_BIND_MULTISAMPLE: memcpy(st.ms, x, 0x18); break;
        case CMD_BIND_POLYGON: memcpy(st.poly, x, 4); break;
        case CMD_BIND_VERTEX_ATTRIB:
            st.nattrib = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.attrib, x, st.nattrib * 4);
            break;
        case CMD_BIND_VERTEX_STREAM:
            st.nstream = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.stream, x, st.nstream * 8);
            break;
        case CMD_BIND_VERTEX_BUFFERS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(&st.vbuf[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_UNIFORM_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 18 && r->x[0] < 6; i++)
                memcpy(&st.ubuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_STORAGE_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 16 && r->x[0] < 6; i++)
                memcpy(&st.sbuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_TEXTURES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 32 && r->x[0] < 6; i++)
                memcpy(&st.tex[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_BIND_IMAGES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 8 && r->x[0] < 6; i++)
                memcpy(&st.img[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_TEXTURE_POOL: st.tpool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_SAMPLER_POOL: st.spool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_RENDER_TARGETS: {
            const struct { uint64_t tex[8]; NvnTextureView views[8]; uint8_t has_view[8]; uint64_t depth;
                           NvnTextureView dview; uint8_t has_dview; } *rt = x;
            RtState prev = st.rt;  // the same targets again: keep the pass (a restart = full barrier + reload)
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = (int)r->x[0] > 8 ? 8 : (int)r->x[0];
            for (int i = 0; i < st.rt.ncolor; i++) {
                st.rt.color[i] = (NvnTexture *)(uintptr_t)rt->tex[i];
                st.rt.cview[i] = rt->views[i];
                st.rt.has_cview[i] = rt->has_view[i];
            }
            st.rt.depth = (NvnTexture *)(uintptr_t)rt->depth;
            st.rt.dview = rt->dview;
            st.rt.has_dview = rt->has_dview;
            static int always = -1;
            if (always < 0) always = getenv("EOWRECOMP_VK_PASS_RESTART") != NULL;
            if (always || memcmp(&prev, &st.rt, sizeof prev)) st.rt_dirty = true;
            break;
        }
        case CMD_SET_VIEWPORTS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.viewport[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_SCISSORS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.scissor[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_DEPTH_RANGES:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.depth_range[r->x[0] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_BLEND_COLOR: memcpy(st.blend_color, x, 16); break;
        case CMD_SET_STENCIL: {
            int face = (int)r->x[0];
            for (int f = 0; f < 2; f++) {
                if (!(face & (1 << f))) continue;
                if (r->x[6] == 0) st.stencil_ref[f] = (uint32_t)r->x[1];
                else if (r->x[6] == 1) st.stencil_mask[f] = (uint32_t)r->x[1];
                else st.stencil_vmask[f] = (uint32_t)r->x[1];
            }
            break;
        }
        case CMD_SET_POLYGON_OFFSET: memcpy(st.poly_offset, r->f, sizeof st.poly_offset); break;
        case CMD_SET_ALPHA_REF: st.alpha_ref = r->f[0]; break;
        case CMD_SET_PRIMITIVE_RESTART: st.prim_restart = r->x[0] != 0; st.restart_index = (uint32_t)r->x[1]; break;
        case CMD_SET_COPY_STRIDES: st.copy_row_stride = r->x[0]; st.copy_image_stride = r->x[1]; break;
        case CMD_CLEAR_COLOR: {
            snap_reason = "clear-color"; snapshots_invalidate(NULL);
            int index = (int)r->x[0];
            if (getenv("EOWRECOMP_LOG_CLEARS") && index < st.rt.ncolor && st.rt.color[index])
                LOG("[cclear] frame %llu idx %d %ux%u f%#x mask %#x\n", (unsigned long long)renderer_present_count(), index,
                    st.rt.color[index]->width, st.rt.color[index]->height, st.rt.color[index]->format, (uint32_t)r->x[2]);
            if (ct_on() && index < st.rt.ncolor && st.rt.color[index])
                LOG("[ct] clear-color %p mask %#x sc %d %d %d %d\n", (void *)st.rt.color[index], (uint32_t)r->x[2], st.scissor[0][0], st.scissor[0][1],
                    st.scissor[0][2], st.scissor[0][3]);
            if (index < st.rt.ncolor && st.rt.color[index]) alias_before_write(st.rt.color[index], 0, "clear");
            if (index < st.rt.ncolor && st.rt.color[index]) clear(index, (int)r->x[5], x, (uint32_t)r->x[2], false, 0, false, 0, 0);
            break;
        }
        case CMD_CLEAR_TEXTURE: {
            // ClearTexture(cmd, texture, view, region, color, mask): a render-target clear of the texture alone (renderer.m)
            const struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } *ct = x;
            NvnTexture *t = (NvnTexture *)(uintptr_t)ct->tex;
            if (!t) break;
            snap_reason = "clear-texture"; snapshots_invalidate(NULL);
            alias_before_write(t, 0, "clear");
            RtState saved = st.rt;
            int32_t sc[4];
            memcpy(sc, st.scissor[0], sizeof sc);
            vk_end_pass();
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = 1;
            st.rt.color[0] = t;
            if (ct->has_view) { st.rt.cview[0] = ct->view; st.rt.has_cview[0] = 1; }
            st.rt_dirty = true;
            const int32_t *rg = ct->region;
            if (rg[3] > 0 && rg[4] > 0) { st.scissor[0][0] = rg[0]; st.scissor[0][1] = rg[1]; st.scissor[0][2] = rg[3]; st.scissor[0][3] = rg[4]; }
            else { st.scissor[0][0] = st.scissor[0][1] = 0; st.scissor[0][2] = st.scissor[0][3] = 16384; }
            clear(0, (int)r->x[6], ct->color, (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 0xF, false, 0, false, 0, 0);
            vk_end_pass();
            st.rt = saved;
            memcpy(st.scissor[0], sc, sizeof sc);
            st.rt_dirty = true;
            break;
        }
        case CMD_CLEAR_DEPTH_STENCIL:
            snap_reason = "clear-depth"; snapshots_invalidate(NULL);
            if (ct_on() && st.rt.depth)
                LOG("[ct] clear-depth %p d%d s%d sc %d %d %d %d\n", (void *)st.rt.depth, r->x[0] != 0, r->x[2] != 0, st.scissor[0][0],
                    st.scissor[0][1], st.scissor[0][2], st.scissor[0][3]);
            if (st.rt.depth && naliases) {  // (copy aliases: which aspects of the depth target this clear replaces whole)
                NvnTexture *d = st.rt.depth;
                bool whole = st.scissor[0][0] <= 0 && st.scissor[0][1] <= 0 && (int64_t)st.scissor[0][0] + st.scissor[0][2] >= (int64_t)d->width &&
                             (int64_t)st.scissor[0][1] + st.scissor[0][3] >= (int64_t)d->height &&
                             !(st.rt.has_dview && (((st.rt.dview.setmask & 1) && st.rt.dview.base_level) ||
                                                   ((st.rt.dview.setmask & 2) && st.rt.dview.min_layer)));
                for (int i = 0; i < st.rt.ncolor; i++)  // (the clear covers the smallest attachment only)
                    if (st.rt.color[i] && (st.rt.color[i]->width < d->width || st.rt.color[i]->height < d->height)) whole = false;
                VkImageAspectFlags full = 0;
                if (whole && r->x[0] != 0) full |= VK_IMAGE_ASPECT_DEPTH_BIT;
                if (whole && renderer_format_has_stencil(d->format) && (r->x[2] & 0xFF) == 0xFF) full |= VK_IMAGE_ASPECT_STENCIL_BIT;
                if (!renderer_format_has_stencil(d->format)) full |= VK_IMAGE_ASPECT_STENCIL_BIT;
                if (ct_on())
                    LOG("[ct] alias clear %p full %#x (whole %d, x0 %llu x1 %#llx x2 %#llx, dview %d/%#x, ncolor %d)\n", (void *)d, full, whole,
                        (unsigned long long)r->x[0], (unsigned long long)r->x[1], (unsigned long long)r->x[2], st.rt.has_dview,
                        st.rt.dview.setmask, st.rt.ncolor);
                alias_before_write(d, full, "clear");
            }
            if (st.rt.depth) {
                bool has_stencil = renderer_format_has_stencil(st.rt.depth->format);
                clear(-1, 0, NULL, 0, r->x[0] != 0, r->f[0], has_stencil && r->x[2] != 0, (uint32_t)r->x[1], (uint32_t)r->x[2]);
            }
            break;
        case CMD_DISPATCH: dispatch_compute((uint32_t)r->x[0], (uint32_t)r->x[1], (uint32_t)r->x[2]); break;
        case CMD_DRAW_ARRAYS: draw(r, false); break;
        case CMD_DRAW_ELEMENTS: draw(r, true); break;
        case CMD_DRAW_ARRAYS_INDIRECT: draw_indirect(r, false); break;
        case CMD_DRAW_ELEMENTS_INDIRECT: draw_indirect(r, true); break;
        case CMD_UPDATE_UNIFORM_BUFFER:
            memcpy((void *)(uintptr_t)(r->x[0] + r->x[2]), x, r->x[3]);
            guest_copy_flush();  // (cbuf copies shared within the submission are stale now)
            break;
        case CMD_FENCE_SYNC: {
            static int fence_flush = -1;
            if (fence_flush < 0) fence_flush = getenv("EOWRECOMP_FENCE_FLUSH") ? 1 : 0;
            if (fence_flush) renderer_flush();
            extern void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
            if (r->x[1]) nvn_sync_set_seq(r->x[1], renderer_fence_seq());
            break;
        }
        case CMD_BARRIER:
            // NVN barriers separate memory hazards between draws, dispatches and copies. End dynamic
            // rendering before emitting the Vulkan dependency; use the conservative all-memory form
            // because NVN's barrier bits do not map one-to-one to Vulkan access masks.
            vk_end_pass();
            vk_full_barrier(vk_cmd());
            break;
        case CMD_SIGNAL_EVENT:
            // NVN event values are CPU-visible. Complete preceding work before publishing the value
            // so a guest thread polling the event cannot observe it ahead of the GPU commands.
            renderer_flush();
            nvn_event_signal(r->x[0], (uint32_t)r->x[1], (uint32_t)r->x[4]);
            break;
        case CMD_REPORT_COUNTER: {
            // The game times its GPU work with these (two per frame) and drops to 30 fps when that looks slow: the
            // host clock now, replaced by the GPU's time at this point once the submission is done (cq_resolve).
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            uint32_t type = (uint32_t)r->x[0];
            static uint32_t seen;
            if (type < 32 && !(seen & (1u << type))) { seen |= 1u << type; LOG("[nvn] ReportCounter type %u\n", type); }
            int mode = vk_counter_mode();
            static uint64_t frame_seen = ~0ull, frame_ts;
            extern uint64_t vk_frame_counter(void);
            if (mode == 1 && vk_frame_counter() != frame_seen) { frame_seen = vk_frame_counter(); frame_ts = ns_to_gpu_ticks(plat_now_ns()); }
            if (!out) break;
            out[0] = 0;
            out[1] = mode == 1 ? frame_ts : mode == 3 ? SDL_GetTicksNS() : ns_to_gpu_ticks(plat_now_ns());  // (3 legacy: ns, the old bug)
            static uint64_t *prev_out;  // (frame pacing: the pair's start)
            if (mode == 0 || mode == 2) out[1] = pace_end_counter(prev_out, out, out[1]);
            prev_out = out;
            {   // (test) EOWRECOMP_TEST_COUNTER_DELAY_MS=<ms>: stall the host after a frame's first counter - a slow CPU
                static double delay = -1;
                static uint64_t delayed_frame = ~0ull;
                if (delay < 0) delay = getenv("EOWRECOMP_TEST_COUNTER_DELAY_MS") ? atof(getenv("EOWRECOMP_TEST_COUNTER_DELAY_MS")) : 0;
                if (delay > 0 && vk_frame_counter() != delayed_frame) {
                    delayed_frame = vk_frame_counter();
                    SDL_DelayPrecise((Uint64)(delay * 1e6));
                }
            }
            if (mode == 2 && type == 0) {  // (type 0: timestamp)
                VkCommandBuffer cb = vk_cmd();  // (first: may begin the buffer, which resets the slot's queries)
                CounterQ *q = &cq[cur_sub];
                pthread_mutex_lock(&cq_lock);
                if (q->pool && q->n < CQ_MAX) {
                    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, q->pool, q->n);
                    q->out[q->n++] = out;
                }
                pthread_mutex_unlock(&cq_lock);
            }
            break;
        }
        case CMD_COPY_TEXTURE_TO_TEXTURE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], r->x[6]); break; }
        case CMD_DOWNSAMPLE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], 1); break; }
        case CMD_COPY_BUFFER_TO_TEXTURE: copy_buffer_to_texture(r->x[0], x); break;
        case CMD_COPY_TEXTURE_TO_BUFFER: copy_texture_to_buffer(x, r->x[3]); break;
        case CMD_COPY_BUFFER_TO_BUFFER:
            wait_why = "buffer -> buffer copy";
            renderer_flush();
            memmove((void *)(uintptr_t)r->x[1], (void *)(uintptr_t)r->x[0], r->x[2]);
            break;
        default: break;
        }
        p += r->size;
    }
}

void backend_execute(uint64_t handle) {
    PROF_T0();
    nvn_texture_refs_begin();
    exec_list(handle, 0);
    nvn_texture_refs_end();
    PROF_ADD(prof_exec);
    if (prof) cum_exec += SDL_GetTicksNS() - prof_t0_;
    static int sync_submit = -1;
    if (sync_submit < 0) sync_submit = getenv("EOWRECOMP_SYNC_SUBMIT") ? 1 : 0;
    if (sync_submit) renderer_flush();
    else renderer_submit();
}
