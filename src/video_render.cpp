// SPDX-License-Identifier: MIT
//
// See video_render.h.

#include "video_render.h"

#ifdef _WIN32

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "asset_cache.h"
#include "console_log.h"
#include "display_settings.h"
#include "gl_loader.h"
#include "renderer.h"
#include "video_camera_seam.h"
#include "video_fx_api.h"
#include "video_gl_context.h"
#include "video_timeline.h"

namespace rav {
namespace {

constexpr int    kDefaultWidth  = 1920;
constexpr int    kDefaultHeight = 1080;
constexpr int    kMinSize       = 16;
constexpr int    kMaxSize       = 8192;
// Models kept uploaded on the video context: at least this many, and at least one per FX
// track (each track draws its own model every frame, so a smaller cache would evict and
// re-upload every model on every frame once there are more tracks than slots).
constexpr size_t kGpuCacheSize  = 3;

// One model uploaded to this thread's context. `src` is the parse it was made from: a
// reloaded file comes back as a different parse and is uploaded again. Held weakly, so a
// parse the shared cache dropped (file changed, item removed) is freed, not kept alive
// here; an expired `src` never equals a live parse, so it is re-uploaded if needed.
struct GpuEntry {
    std::string                     path;
    std::weak_ptr<const CpuAsset>   src;
    Asset                           gpu;            // empty while it is inside the renderer
    bool                            upload_failed = false;
    unsigned long long              last_used = 0;
};

// Per video thread. Heap-allocated and NEVER destroyed: its GL objects belong to the
// thread's context, which is never torn down either (video_gl_context.h); freeing them
// at thread or process exit, without that context current, would be undefined.
struct ThreadRender {
    bool      attempted = false;
    bool      ok        = false;
    unsigned  gl_generation = 0;
    Renderer* renderer  = nullptr;
    int       max_samples = 0;
    std::vector<std::unique_ptr<GpuEntry>> entries;
    GpuEntry* current   = nullptr;   // the entry whose Asset is inside the renderer
    unsigned long long clock = 0;

    // Per-frame cost, rolled up once a second (LogInfo: debuglog builds only).
    LARGE_INTEGER window_start{};
    int    frames   = 0;
    double sum_ms   = 0.0;
    double max_ms   = 0.0;
    double sum_draw = 0.0;
    double sum_read = 0.0;
    int    last_w   = 0;
    int    last_h   = 0;
};

thread_local ThreadRender* t_render = nullptr;

double QpcMs(const LARGE_INTEGER& from, const LARGE_INTEGER& to)
{
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    return static_cast<double>(to.QuadPart - from.QuadPart) * 1000.0 / freq;
}

ThreadRender& State()
{
    if (!t_render) t_render = new ThreadRender();  // see ThreadRender: never deleted
    return *t_render;
}

// With this thread's context current. False = no picture from this thread (logged once).
bool EnsureRenderer(ThreadRender& tr)
{
    if (tr.attempted) {
        if (!tr.ok) return false;
        // The viewer reloaded the process-wide GL table (a different driver context):
        // check again that it is still valid from this context before drawing with it.
        if (GlFunctionsGeneration() != tr.gl_generation) {
            std::string err;
            unsigned gen = 0;
            if (!CheckGlFunctionsForCurrentContext(err, &gen)) {
                tr.ok = false;
                LogWarn("video FX: the animation is not drawn any more (%s)", err.c_str());
                return false;
            }
            tr.gl_generation = gen;
        }
        return true;
    }
    tr.attempted = true;
    std::string err;
    unsigned gen = 0;
    if (!CheckGlFunctionsForCurrentContext(err, &gen)) {
        LogWarn("video FX: the animation cannot be drawn on the video thread (%s)", err.c_str());
        return false;
    }
    std::unique_ptr<Renderer> r(new Renderer());
    if (!r->Init(err)) {
        LogWarn("video FX: renderer init failed on the video thread (%s)", err.c_str());
        r->Shutdown();  // context is current: free what Init created
        return false;
    }
    GLint samples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &samples);
    tr.max_samples   = samples;
    tr.renderer      = r.release();  // see ThreadRender: never deleted
    tr.gl_generation = gen;
    tr.ok            = true;
    LogInfo("video FX: renderer ready on the video thread (max MSAA %d)", samples);
    return true;
}

// Takes the renderer's model back into its cache entry.
void ParkCurrent(ThreadRender& tr)
{
    if (!tr.current) return;
    tr.current->gpu = tr.renderer->TakeAsset();
    tr.current = nullptr;
}

void Evict(ThreadRender& tr, GpuEntry* e)
{
    if (e == tr.current) ParkCurrent(tr);
    for (auto it = tr.entries.begin(); it != tr.entries.end(); ++it) {
        if (it->get() == e) {
            tr.entries.erase(it);   // frees the GPU copy (context is current)
            return;
        }
    }
}

// Puts the model of `path` (parse `cpu`) inside the renderer, uploading it if this
// thread has no GPU copy of that parse yet. With the context current. `capacity` = how
// many GPU copies may stay uploaded.
bool SelectModel(ThreadRender& tr, const std::string& path, const std::shared_ptr<const CpuAsset>& cpu,
                 size_t capacity)
{
    ++tr.clock;
    GpuEntry* e = nullptr;
    for (auto& p : tr.entries)
        if (p->path == path) { e = p.get(); break; }

    if (e && e->src.lock() != cpu) {   // the file was reloaded: drop the old copy
        Evict(tr, e);
        e = nullptr;
    }
    if (!e) {
        if (tr.entries.size() >= capacity) {
            GpuEntry* lru = nullptr;
            for (auto& p : tr.entries)
                if (!lru || p->last_used < lru->last_used) lru = p.get();
            Evict(tr, lru);
        }
        std::unique_ptr<GpuEntry> fresh(new GpuEntry());
        fresh->path = path;
        fresh->src  = cpu;
        if (!UploadAsset(*cpu, fresh->gpu, nullptr)) {
            fresh->upload_failed = true;
            LogWarn("video FX: %s could not be uploaded to the GPU", path.c_str());
        }
        e = fresh.get();
        tr.entries.push_back(std::move(fresh));
    }
    e->last_used = tr.clock;
    if (e->upload_failed) return false;   // remembered: no upload retry every frame
    if (e != tr.current) {
        ParkCurrent(tr);
        tr.renderer->SetAsset(std::move(e->gpu));
        tr.current = e;
    }
    return true;
}

void CountFrame(ThreadRender& tr, int w, int h, double total, double draw, double read)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (tr.frames == 0 && tr.window_start.QuadPart == 0) tr.window_start = now;
    ++tr.frames;
    tr.sum_ms   += total;
    tr.sum_draw += draw;
    tr.sum_read += read;
    tr.max_ms    = std::max(tr.max_ms, total);
    tr.last_w = w;
    tr.last_h = h;
    const double window = QpcMs(tr.window_start, now);
    if (window >= 1000.0) {
        const double n = static_cast<double>(tr.frames);
        LogInfo("video FX cost: %d frames %dx%d in %.1f s, avg %.2f ms (draw %.2f, read %.2f), max %.2f ms",
                tr.frames, tr.last_w, tr.last_h, window / 1000.0, tr.sum_ms / n, tr.sum_draw / n,
                tr.sum_read / n, tr.max_ms);
        tr.frames = 0;
        tr.sum_ms = tr.sum_draw = tr.sum_read = tr.max_ms = 0.0;
        tr.window_start = now;
    }
}

}  // namespace

void VideoOutputSize(const RavVideoFrameRequest* req, const VideoTrackTimeline* tl, int* w, int* h)
{
    VideoOutputSizeFor(req->override_width, req->override_height, tl ? tl->video_width : 0,
                       tl ? tl->video_height : 0, w, h);
}

void VideoOutputSizeFor(int override_w, int override_h, int project_w, int project_h, int* w, int* h)
{
    int ow = override_w;
    int oh = override_h;
    if (ow <= 0 || oh <= 0) {
        ow = project_w;
        oh = project_h;
    }
    if (ow <= 0 || oh <= 0) {
        ow = kDefaultWidth;
        oh = kDefaultHeight;
    }
    *w = std::clamp(ow, kMinSize, kMaxSize);
    *h = std::clamp(oh, kMinSize, kMaxSize);
}

void VideoContentFrameSize(const RavVideoFrameRequest* req, int* out_width, int* out_height)
{
    *out_width  = 0;
    *out_height = 0;
    const std::shared_ptr<const VideoTimelineSnapshot> snap = CurrentVideoTimeline();
    VideoItemHit hit;
    if (!FindVideoItemAt(*snap, req->track, req->project_time, hit)) return;
    VideoOutputSize(req, hit.track, out_width, out_height);
}

int VideoContentRenderFrame(const RavVideoFrameRequest* req)
{
    const std::shared_ptr<const VideoTimelineSnapshot> snap = CurrentVideoTimeline();
    VideoItemHit hit;
    if (!FindVideoItemAt(*snap, req->track, req->project_time, hit)) return 0;

    // The model, complete or nothing (parsed here on a miss: this frame waits for it).
    const std::shared_ptr<const CpuAsset> cpu = AcquireCpuAsset(hit.item->path);
    if (!cpu) return 0;

    const int w = req->width;
    const int h = req->height;
    LARGE_INTEGER t0, t1, t2;
    QueryPerformanceCounter(&t0);

    VideoGlFrame gl;
    if (!gl.Begin(w, h)) return 0;   // no GL on this thread (logged once by Begin)
    ThreadRender& tr = State();
    if (!EnsureRenderer(tr)) return 0;
    if (!SelectModel(tr, hit.item->path, cpu, std::max(kGpuCacheSize, snap->tracks.size()))) return 0;

    Renderer& r = *tr.renderer;
    const DisplaySettings settings = LastDisplaySettings();
    r.ApplyDisplaySettings(settings, tr.max_samples);
    // Transparent option: clear to transparent BLACK, so the picture is valid whether
    // REAPER reads it as premultiplied or straight alpha (MSAA edges blend toward 0,0,0,0).
    if (hit.track->transparent) r.SetBackground(glm::vec3(0.0f), 0.0f);

    const Asset& model = r.CurrentAsset();
    PosedBounds bounds;
    bounds.min = model.aabbMin;
    bounds.max = model.aabbMax;
    const VideoCameraPose pose = ResolveVideoCamera(req->parms, req->nparms, bounds);
    // The test pattern draws on this same context and leaves depth test off and scissor
    // on/off (video_test_pattern.cpp). The renderer sets this state only in Init, so put
    // it back, or the model and floor would draw without occlusion.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    OrbitCamera& cam = r.Camera();
    cam.animating = false;
    cam.target    = pose.target;
    cam.distance  = pose.distance;
    cam.yaw       = pose.yaw;
    cam.pitch     = pose.pitch;

    r.RenderFrame(static_cast<float>(hit.anim_time), /*loop=*/false, w, h, gl.Fbo());
    QueryPerformanceCounter(&t1);
    const bool read = gl.ReadInto(req->pixels, req->row_bytes);
    QueryPerformanceCounter(&t2);
    if (!read) return 0;

    CountFrame(tr, w, h, QpcMs(t0, t2), QpcMs(t0, t1), QpcMs(t1, t2));
    return 1;
}

}  // namespace rav

#endif  // _WIN32
