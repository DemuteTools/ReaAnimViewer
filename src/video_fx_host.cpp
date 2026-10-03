// SPDX-License-Identifier: MIT
//
// See video_fx_host.h and video_fx_api.h.

#include "video_fx_host.h"

#include <algorithm>
#include <atomic>
#include <memory>

#include "asset_cache.h"
#include "console_log.h"
#include "video_fx_api.h"
#include "video_gl_context.h"
#include "video_render.h"
#include "video_test_pattern.h"
#include "video_timeline.h"

namespace rav {
namespace {

std::atomic<bool> g_test_pattern{false};
std::atomic<bool> g_shutting_down{false};
std::atomic<bool> g_logged_cpu_fallback{false};
bool              g_registered = false;
bool              g_timer_registered = false;  // Story 11-2: the timeline snapshot timer
// Story 11-4: the last API version an FX asked for that is not ours (0 = none), so the
// Video panel can say "out of date" instead of showing an active FX that draws nothing.
std::atomic<int>  g_mismatched_version{0};

bool ValidRequest(const RavVideoFrameRequest* req)
{
    return req && req->struct_size >= static_cast<int>(sizeof(RavVideoFrameRequest));
}

// Test-pattern output size: the content path's own rule (VideoOutputSize: the FX's saved
// override, else the project's video size from the timeline snapshot, else 1920x1080).
void OutputSize(const RavVideoFrameRequest* req, int* w, int* h)
{
    const std::shared_ptr<const VideoTimelineSnapshot> snap = CurrentVideoTimeline();
    VideoOutputSize(req, snap->Find(req->track), w, h);
}

// ---- Content path (Story 11-2) -------------------------------------------
// The FX track's animation at req->project_time (video_render.h). No RAV item on the
// track at that time = no picture, so lower video tracks show through.
void ContentFrameSize(const RavVideoFrameRequest* req, int* w, int* h)
{
    VideoContentFrameSize(req, w, h);
}

int ContentRenderFrame(const RavVideoFrameRequest* req)
{
    return VideoContentRenderFrame(req);
}

// ---- Test pattern --------------------------------------------------------
int TestPatternRenderFrame(const RavVideoFrameRequest* req)
{
    const long long index = VideoFrameIndex(req->project_time, req->frame_rate);
    {
        VideoGlFrame gl;
        if (gl.Begin(req->width, req->height)) {
            DrawTestPatternGl(req->width, req->height, req->project_time, index);
            if (gl.ReadInto(req->pixels, req->row_bytes)) return 1;
        }
    }  // GL scope closed (previous context restored) before the CPU fallback
    if (!g_logged_cpu_fallback.exchange(true)) {
        LogWarn("video FX: test pattern drawn without OpenGL");
    }
    DrawTestPatternCpu(req->pixels, req->width, req->height, req->row_bytes, req->project_time, index);
    return 1;
}

// ---- API table -----------------------------------------------------------
void ApiFrameSize(const RavVideoFrameRequest* req, int* out_w, int* out_h)
{
    if (!out_w || !out_h) return;
    *out_w = 0;
    *out_h = 0;
    try {
        if (g_shutting_down.load() || !ValidRequest(req)) return;
        if (g_test_pattern.load()) {
            OutputSize(req, out_w, out_h);
            return;
        }
        ContentFrameSize(req, out_w, out_h);
    } catch (...) {
        *out_w = 0;
        *out_h = 0;
    }
}

int ApiRenderFrame(const RavVideoFrameRequest* req)
{
    try {
        if (g_shutting_down.load() || !ValidRequest(req)) return 0;
        if (!req->pixels || req->width <= 0 || req->height <= 0 || req->row_bytes < req->width * 4) return 0;
        if (g_test_pattern.load()) return TestPatternRenderFrame(req);
        return ContentRenderFrame(req);
    } catch (...) {
        return 0;
    }
}

void ApiReleaseInstance(void* /*fx_instance*/)
{
    // Nothing is cached per FX instance yet (Story 11-2 / 11-3 may).
}

const RavVideoFxApi kApi = {
    RAV_VIDEO_FX_API_VERSION,
    static_cast<int>(sizeof(RavVideoFxApi)),
    &ApiFrameSize,
    &ApiRenderFrame,
    &ApiReleaseInstance,
};

const RavVideoFxApi* GetVideoFxApi(int version)
{
    if (version == RAV_VIDEO_FX_API_VERSION) return &kApi;
    g_mismatched_version.store(version == 0 ? -1 : version);
    return nullptr;
}

// Compile-time check that the registered function has the ABI's signature.
constexpr RavGetVideoFxApiFn kGetApiFn = &GetVideoFxApi;

}  // namespace

bool RegisterVideoFxApi(int (*register_fn)(const char*, void*))
{
    if (!register_fn || g_registered) return g_registered;
    g_shutting_down.store(false);
    g_registered = register_fn("API_" RAV_VIDEO_FX_API_NAME, reinterpret_cast<void*>(kGetApiFn)) != 0;
    if (g_registered) LogInfo("video FX API v%d registered", RAV_VIDEO_FX_API_VERSION);
    else LogWarn("video FX: could not register the frame API, the video FX will show nothing");
    // Story 11-2: the main-thread timer that keeps the timeline snapshot the video thread
    // reads (video_timeline.h). Without it the FX finds no item and shows nothing.
    if (g_registered && !g_timer_registered) {
        g_timer_registered = register_fn("timer", reinterpret_cast<void*>(&VideoTimelineTick)) != 0;
        if (!g_timer_registered) LogWarn("video FX: could not register the timeline timer, the video FX will show nothing");
    }
    return g_registered;
}

void UnregisterVideoFxApi(int (*register_fn)(const char*, void*))
{
    g_shutting_down.store(true);
    if (!register_fn || !g_registered) return;
    if (g_timer_registered) {
        register_fn("-timer", reinterpret_cast<void*>(&VideoTimelineTick));
        g_timer_registered = false;
    }
    register_fn("-API_" RAV_VIDEO_FX_API_NAME, reinterpret_cast<void*>(kGetApiFn));
    g_registered = false;
    // A video-thread frame still running holds its own references to the snapshot and
    // the model it draws, so dropping ours here is safe.
    ClearVideoTimeline();
    ClearCpuAssetCache();
}

int VideoFxApiMismatchedVersion()
{
    return g_mismatched_version.load();
}

bool VideoTestPatternEnabled()
{
    return g_test_pattern.load();
}

void SetVideoTestPatternEnabled(bool enabled)
{
    g_test_pattern.store(enabled);
}

}  // namespace rav
