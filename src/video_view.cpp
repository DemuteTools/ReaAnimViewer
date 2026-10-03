// SPDX-License-Identifier: MIT
//
// See video_view.h.

#include "video_view.h"

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>

#include "console_log.h"
#include "reaper_actions.h"
#include "reaper_api.h"
#include "video_camera.h"
#include "video_fx_host.h"
#include "video_fx_track.h"
#include "video_render.h"
#include "video_shot_timing.h"
#include "video_timeline.h"

namespace rav {
namespace {

constexpr double kRefreshSeconds    = 0.5;   // model refresh floor (edits without an undo point)
constexpr double kWheelQuietSeconds = 0.4;   // wheel steps closer than this are one gesture
constexpr float  kMaxTweenStep      = 0.1f;  // seconds: a stalled frame cannot jump the tween
constexpr double kNoticeSeconds     = 3.0;   // how long a notice stays (Story 11-5)

enum class Gesture { None, Orbit, Pan, Zoom, Slider };

// The gesture in progress. It edits the shot that was under the playhead when it began.
struct Live {
    Gesture     kind = Gesture::None;
    MediaTrack* track = nullptr;
    int         fx = -1;
    double      shot_time = 0.0;
    glm::vec3   aabb_min{0.0f};
    glm::vec3   aabb_max{0.0f};
    OrbitCamera cam;
    double      values[vcam::kParamCount] = {};
    int         slider_param = -1;
    double      wheel_deadline = 0.0;
    bool        written = false;  // queued for writing: shown until the queue has run
};

enum class CmdKind {
    WriteShot, Rename, Transition, AddFx, EnableFx, SetOnline,
    // Story 11-5
    Seek, Cut, DeleteShot, SaveAngle, DeleteAngle, OutputSize, CustomOutputSize, Background, OpenMatrix,
    OpenRenderDialog,
    // Feedback 11-fb-2
    MoveShot,
};

struct Cmd {
    CmdKind     kind = CmdKind::WriteShot;
    MediaTrack* track = nullptr;
    int         fx = -1;
    double      shot_time = 0.0;
    double      values[vcam::kParamCount] = {};
    bool        flag = false;
    std::string text;
    std::string undo;
    double      fps = 0.0;  // Cut: the project frame rate when it was asked
    double      new_time = 0.0;  // MoveShot: where the shot at shot_time goes
    int         width = 0;  // OutputSize
    int         height = 0;
};

bool        g_active = false;
bool        g_panel = false;
bool        g_panel_opened_once = false;
MediaTrack* g_pinned = nullptr;
MediaTrack* g_followed = nullptr;

VideoViewModel g_model;
int            g_last_state_count = -1;
double         g_last_refresh = -1.0e9;
bool           g_dirty = true;
MediaTrack*    g_model_key = nullptr;

bool      g_has_item = false;
bool      g_has_model = false;
glm::vec3 g_min{0.0f};
glm::vec3 g_max{0.0f};
double    g_playhead = 0.0;
int       g_shot_index = -1;

bool        g_frame_cam_valid = false;
OrbitCamera g_frame_cam;

Live        g_live;
bool        g_tweening = false;
OrbitCamera g_tween;
double      g_last_now = -1.0;

std::vector<Cmd> g_queue;

// Story 11-5
VideoStripRange g_range;
std::string     g_notice;
double          g_notice_until = -1.0;

double Now()
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

bool TrackValid(MediaTrack* t)
{
    return t && ValidatePtr2(nullptr, t, "MediaTrack*");
}

bool FxValid(MediaTrack* t, int fx)
{
    return TrackValid(t) && fx >= 0 && fx < TrackFX_GetCount(t) && FindVideoFxOnTrack(t) == fx;
}

double Playhead()
{
    const bool playing = (GetPlayStateEx(nullptr) & 1) != 0;
    return playing ? GetPlayPosition2Ex(nullptr) : GetCursorPositionEx(nullptr);
}

std::string TrackLabel(MediaTrack* t)
{
    const int number = static_cast<int>(GetMediaTrackInfo_Value(t, "IP_TRACKNUMBER"));
    char name[256] = {};
    GetSetMediaTrackInfo_String(t, "P_NAME", name, false);
    char out[320];
    if (name[0]) std::snprintf(out, sizeof(out), "%d \xC2\xB7 %s", number, name);
    else std::snprintf(out, sizeof(out), "Track %d", number);
    return out;
}

MediaTrack* CurrentTrack()
{
    if (g_pinned && !TrackValid(g_pinned)) {
        g_pinned = nullptr;  // the pinned track is gone: back to Follow
        g_dirty = true;
    }
    if (g_pinned) return g_pinned;
    if (g_followed && !TrackValid(g_followed)) g_followed = nullptr;
    return g_followed;
}

void Refresh()
{
    VideoViewModel m;
    MediaTrack* tr = CurrentTrack();
    m.track = tr;
    m.pinned = (g_pinned != nullptr);

    bool drop = false;
    const double fps = TimeMap_curFrameRate(nullptr, &drop);
    m.fps = (std::isfinite(fps) && fps > 0.0) ? fps : 0.0;

    int ow = 0;
    int oh = 0;
    if (tr) {
        m.track_label = TrackLabel(tr);
        m.fx = FindVideoFxOnTrack(tr);
        if (m.fx < 0) {
            m.status = VideoFxStatus::Missing;
        } else {
            if (VideoFxApiMismatchedVersion() != 0) m.status = VideoFxStatus::OutOfDate;
            else if (TrackFX_GetOffline(tr, m.fx)) m.status = VideoFxStatus::Offline;
            else if (!TrackFX_GetEnabled(tr, m.fx)) m.status = VideoFxStatus::Bypassed;
            else m.status = VideoFxStatus::Active;
            m.shots = ReadVideoShots(tr, m.fx);
            VideoFxState st;
            if (ReadVideoFxState(tr, m.fx, &st)) {
                ow = st.override_width;
                oh = st.override_height;
                m.angles = std::move(st.angles);  // Story 11-5: the panel's saved angles
            }
        }
        m.transparent = VideoBackgroundTransparent(tr);
    }
    int pw = 0;
    int ph = 0;
    ProjectVideoSizeOf(nullptr, &pw, &ph);
    VideoOutputSizeFor(ow, oh, pw, ph, &m.out_w, &m.out_h);
    m.override_w = VideoFxOverrideValid(ow, oh) ? ow : 0;
    m.override_h = VideoFxOverrideValid(ow, oh) ? oh : 0;
    m.project_w = (pw > 0 && ph > 0) ? pw : 0;
    m.project_h = (pw > 0 && ph > 0) ? ph : 0;

    // The tracks the panel offers to pin: those carrying the FX, plus the followed one.
    for (int i = 0, n = CountTracks(nullptr); i < n; ++i) {
        MediaTrack* t = GetTrack(nullptr, i);
        if (!t) continue;
        if (t == g_followed || t == g_pinned || FindVideoFxOnTrack(t) >= 0) m.pin_choices.push_back({t, TrackLabel(t)});
    }

    g_model = std::move(m);
    g_model_key = tr;
    g_dirty = false;
}

// Writes the six values into the shot at `time` (kept: its name and transition), as one
// named undo point.
void WriteShotValues(MediaTrack* tr, int fx, double time, const double values[vcam::kParamCount], const char* undo)
{
    const std::vector<VideoShot> shots = ReadVideoShots(tr, fx);
    VideoShot s;
    bool found = false;
    for (const VideoShot& x : shots) {
        if (std::fabs(x.time - time) <= kVideoShotTimeTolerance) {
            s = x;
            found = true;
            break;
        }
    }
    if (!found) {
        s.name.clear();
        s.move_to_next = false;
    }
    s.time = time;
    s.implicit = false;
    for (int p = 0; p < vcam::kParamCount; ++p) s.values[p] = vcam::Sanitize(p, values[p]);

    Undo_BeginBlock2(nullptr);
    const bool ok = WriteVideoShot(tr, fx, s);
    Undo_EndBlock2(nullptr, undo, UNDO_STATE_FX);
    if (!ok) LogWarn("video view: the shot at %.3f s could not be written", time);
}

// The live camera, kept inside the parameter ranges (what the write will store).
void ClampLiveCamera()
{
    VideoCameraValuesFromCamera(g_live.cam, g_live.aabb_min, g_live.aabb_max, g_live.values);
    g_live.cam = VideoCameraFromValues(g_live.values, g_live.aabb_min, g_live.aabb_max);
}

bool BeginLive(Gesture kind)
{
    if (!VideoViewCanEdit() || g_shot_index < 0 || g_shot_index >= static_cast<int>(g_model.shots.size())) return false;
    OrbitCamera cam;
    if (!VideoViewCamera(&cam)) return false;
    g_live = Live();
    g_live.kind = kind;
    g_live.track = g_model.track;
    g_live.fx = g_model.fx;
    g_live.shot_time = g_model.shots[static_cast<size_t>(g_shot_index)].time;
    g_live.aabb_min = g_min;
    g_live.aabb_max = g_max;
    g_live.cam = cam;
    g_live.cam.animating = false;
    if (kind == Gesture::Slider) {
        for (int p = 0; p < vcam::kParamCount; ++p)
            g_live.values[p] = g_model.shots[static_cast<size_t>(g_shot_index)].values[p];
    } else {
        ClampLiveCamera();
    }
    g_tweening = false;
    return true;
}

void QueueLiveWrite(const char* undo)
{
    if (g_live.kind == Gesture::None || g_live.written) return;
    Cmd c;
    c.kind = CmdKind::WriteShot;
    c.track = g_live.track;
    c.fx = g_live.fx;
    c.shot_time = g_live.shot_time;
    for (int p = 0; p < vcam::kParamCount; ++p) c.values[p] = g_live.values[p];
    c.undo = undo;
    g_queue.push_back(std::move(c));
    g_live.written = true;  // keep showing it until the queue has run (no one-frame flicker)
}

const char* UndoNameFor(Gesture g)
{
    switch (g) {
        case Gesture::Orbit: return "RAV: Orbit video shot";
        case Gesture::Pan:   return "RAV: Pan video shot";
        case Gesture::Zoom:  return "RAV: Zoom video shot";
        default:             return "RAV: Edit video shot";
    }
}

// Ends an unwritten gesture as the view showed it (one undo point): a view switch ends a
// drag or a wheel gesture (view_gestures), hiding the panel ends a slider.
void CommitLive(bool view_gestures)
{
    if (g_live.kind == Gesture::None || g_live.written) return;
    if ((g_live.kind == Gesture::Slider) == view_gestures) return;
    QueueLiveWrite(UndoNameFor(g_live.kind));
}

// Queues a command on the shot under the playhead of the model's FX.
bool CurrentShot(Cmd* c)
{
    if (!g_model.track || g_model.fx < 0) return false;
    if (g_shot_index < 0 || g_shot_index >= static_cast<int>(g_model.shots.size())) return false;
    c->track = g_model.track;
    c->fx = g_model.fx;
    c->shot_time = g_model.shots[static_cast<size_t>(g_shot_index)].time;
    for (int p = 0; p < vcam::kParamCount; ++p) c->values[p] = g_model.shots[static_cast<size_t>(g_shot_index)].values[p];
    return true;
}

// The shot strip's span (Story 11-5): the RAV item under the playhead on the model's
// track, else the nearest one, read from the video FX's timeline snapshot (the items of
// the tracks that carry the FX: the strip only shows on such a track).
void UpdateStripRange()
{
    g_range = VideoStripRange();
    if (!g_model.track || g_model.fx < 0) return;
    const std::shared_ptr<const VideoTimelineSnapshot> snap = CurrentVideoTimeline();
    const VideoTrackTimeline* tl = snap ? snap->Find(g_model.track) : nullptr;
    if (!tl || tl->items.empty()) return;
    const VideoItemSpan* best = nullptr;
    double best_gap = 0.0;
    for (const VideoItemSpan& it : tl->items) {
        if (!(it.end > it.start)) continue;
        if (g_playhead >= it.start && g_playhead < it.end) {  // the viewer's rule: first spanning item
            best = &it;
            break;
        }
        const double gap = (g_playhead < it.start) ? it.start - g_playhead : g_playhead - it.end;
        if (!best || gap < best_gap) {
            best = &it;
            best_gap = gap;
        }
    }
    if (!best) return;
    g_range.valid = true;
    g_range.start = best->start;
    g_range.end = best->end;
}

void SetNotice(const char* text)
{
    g_notice = text ? text : "";
    g_notice_until = Now() + kNoticeSeconds;
}

// A Cut shot at the playhead holding the camera shown there (video_shot_timing.h says
// where its points go), one undo point. Refused when a shot already starts on that frame.
void RunCut(const Cmd& c)
{
    if (!FxValid(c.track, c.fx)) return;
    const std::vector<VideoShot> shots = ReadVideoShots(c.track, c.fx);
    std::vector<double> times;
    times.reserve(shots.size());
    for (const VideoShot& s : shots) times.push_back(s.time);
    if (VideoCutWouldDuplicate(times, c.shot_time, c.fps, kVideoShotTimeTolerance)) {
        SetNotice("A shot already starts on this frame");
        return;
    }
    VideoShot shot;
    shot.time = VideoCutTime(c.shot_time, c.fps);
    shot.move_to_next = false;
    shot.name.clear();  // automatic "Shot N"
    ReadVideoCameraAt(c.track, c.fx, c.shot_time, shot.values);

    Undo_BeginBlock2(nullptr);
    const bool ok = WriteVideoShot(c.track, c.fx, shot);
    Undo_EndBlock2(nullptr, "RAV: Cut at playhead", UNDO_STATE_FX);
    if (!ok) {
        LogWarn("video view: the cut at %.3f s could not be written", shot.time);
        SetNotice("The cut could not be written (see Copy error log)");
    }
}

// A junction dragged in the shot strip: the shot at c.shot_time moves to c.new_time, one
// undo point. Nothing is written when the shot is gone, or when its neighbours changed
// since the drag began so that the move would now pass one (no reordering).
void RunMoveShot(const Cmd& c)
{
    if (!FxValid(c.track, c.fx)) return;
    const std::vector<VideoShot> shots = ReadVideoShots(c.track, c.fx);
    int index = -1;
    for (size_t i = 0; i < shots.size(); ++i) {
        if (!shots[i].implicit && std::fabs(shots[i].time - c.shot_time) <= kVideoShotTimeTolerance) {
            index = static_cast<int>(i);
            break;
        }
    }
    if (index < 0) {
        LogWarn("video view: no shot at %.3f s any more, the move was not written", c.shot_time);
        return;
    }
    const size_t i = static_cast<size_t>(index);
    const bool after_prev = index > 0 && c.new_time > shots[i - 1].time + kVideoShotTimeTolerance;
    const bool before_next = i + 1 >= shots.size() || c.new_time < shots[i + 1].time - kVideoShotTimeTolerance;
    if (!after_prev || !before_next) {
        LogWarn("video view: the shots changed during the drag, the move of %.3f s to %.3f s was not written",
                c.shot_time, c.new_time);
        return;
    }

    Undo_BeginBlock2(nullptr);
    const bool ok = MoveVideoShot(c.track, c.fx, c.shot_time, c.new_time);
    Undo_EndBlock2(nullptr, "RAV: Move video shot", UNDO_STATE_FX);
    if (!ok) LogWarn("video view: the shot at %.3f s had no envelope point to move", c.shot_time);
}

void WriteOutputSize(MediaTrack* track, int fx, int width, int height)
{
    if (!FxValid(track, fx)) return;
    Undo_BeginBlock2(nullptr);
    const bool ok = SetVideoOutputOverride(track, fx, width, height);
    Undo_EndBlock2(nullptr, "RAV: Video output size", UNDO_STATE_FX);
    if (!ok) LogWarn("video view: the output size %dx%d could not be written", width, height);
}

// "Custom..." in the Output size list: REAPER's input dialog (a modal loop: this runs
// from the window procedure, never inside the ImGui frame).
void RunCustomOutputSize(const Cmd& c)
{
    if (!FxValid(c.track, c.fx)) return;
    static const char* const kTitle = "RAV: Video output size";
    char buf[128] = {};  // empty while the FX follows the project (OK keeps following it)
    if (c.width > 0 && c.height > 0) std::snprintf(buf, sizeof(buf), "%d x %d", c.width, c.height);
    if (!GetUserInputs(kTitle, 1, "Size (W x H; empty = project size):,extrawidth=80", buf, sizeof(buf))) return;

    int width = 0;
    int height = 0;
    if (!CleanVideoFxName(buf).empty()) {
        char* end = nullptr;
        const long w = std::strtol(buf, &end, 10);
        while (*end == ' ' || *end == 'x' || *end == 'X' || *end == '*') ++end;
        const char* hs = end;
        const long h = std::strtol(hs, &end, 10);
        if (end == hs || w < 16 || h < 16 || w > kVideoFxMaxSize || h > kVideoFxMaxSize) {
            ShowMessageBox("Type the size as W x H, 16 to 8192 pixels each (for example 1080 x 1920), or leave it "
                           "empty to follow the project's video size.",
                           kTitle, 0);
            return;
        }
        width = static_cast<int>(w);
        height = static_cast<int>(h);
    }
    WriteOutputSize(c.track, c.fx, width, height);
}

void RunOne(const Cmd& c)
{
    switch (c.kind) {
        case CmdKind::AddFx: {
            if (!TrackValid(c.track)) return;
            Undo_BeginBlock2(nullptr);
            const AddVideoFxResult r = AddVideoFxToTrack(c.track, nullptr);
            Undo_EndBlock2(nullptr, "RAV: Add video FX to track", UNDO_STATE_FX);
            if (r == AddVideoFxResult::NotInstalled) ShowVideoFxNotInstalledMessage();
            return;
        }
        case CmdKind::EnableFx:
            if (!FxValid(c.track, c.fx)) return;
            Undo_BeginBlock2(nullptr);
            TrackFX_SetEnabled(c.track, c.fx, true);
            Undo_EndBlock2(nullptr, "RAV: Enable video FX", UNDO_STATE_FX);
            return;
        case CmdKind::SetOnline:
            if (!FxValid(c.track, c.fx)) return;
            Undo_BeginBlock2(nullptr);
            TrackFX_SetOffline(c.track, c.fx, false);
            Undo_EndBlock2(nullptr, "RAV: Set video FX online", UNDO_STATE_FX);
            return;
        case CmdKind::WriteShot:
            if (!FxValid(c.track, c.fx)) return;
            WriteShotValues(c.track, c.fx, c.shot_time, c.values, c.undo.c_str());
            return;
        case CmdKind::Rename:
            if (!FxValid(c.track, c.fx)) return;
            Undo_BeginBlock2(nullptr);
            RenameVideoShot(c.track, c.fx, c.shot_time, c.text);
            Undo_EndBlock2(nullptr, "RAV: Rename video shot", UNDO_STATE_FX);
            return;
        case CmdKind::Transition:
            if (!FxValid(c.track, c.fx)) return;
            Undo_BeginBlock2(nullptr);
            SetVideoShotTransition(c.track, c.fx, c.shot_time, c.flag);
            Undo_EndBlock2(nullptr, c.flag ? "RAV: Video shot: move to next" : "RAV: Video shot: cut to next",
                           UNDO_STATE_FX);
            return;
        // ---- Story 11-5 ----------------------------------------------------------------------
        case CmdKind::Seek:
            SetEditCurPos(std::max(0.0, c.shot_time), /*moveview=*/true, /*seekplay=*/true);
            return;
        case CmdKind::Cut:
            RunCut(c);
            return;
        case CmdKind::MoveShot:
            RunMoveShot(c);
            return;
        case CmdKind::DeleteShot: {
            if (!FxValid(c.track, c.fx)) return;
            Undo_BeginBlock2(nullptr);
            const bool ok = DeleteVideoShot(c.track, c.fx, c.shot_time);
            Undo_EndBlock2(nullptr, "RAV: Delete video shot", UNDO_STATE_FX);
            if (!ok) LogWarn("video view: the shot at %.3f s had no envelope point to delete", c.shot_time);
            return;
        }
        case CmdKind::SaveAngle: {
            if (!FxValid(c.track, c.fx)) return;
            Undo_BeginBlock2(nullptr);
            const bool ok = SaveVideoAngle(c.track, c.fx, c.text, c.values);
            Undo_EndBlock2(nullptr, "RAV: Save video angle", UNDO_STATE_FX);
            if (ok) {
                SetNotice("Angle saved");
            } else {
                LogWarn("video view: the angle \"%s\" could not be saved", c.text.c_str());
                SetNotice("The angle could not be saved (see Copy error log)");
            }
            return;
        }
        case CmdKind::DeleteAngle: {
            if (!FxValid(c.track, c.fx)) return;
            Undo_BeginBlock2(nullptr);
            const bool ok = DeleteVideoAngle(c.track, c.fx, c.text);
            Undo_EndBlock2(nullptr, "RAV: Delete video angle", UNDO_STATE_FX);
            if (!ok) LogWarn("video view: the angle \"%s\" could not be deleted", c.text.c_str());
            return;
        }
        case CmdKind::OutputSize:
            WriteOutputSize(c.track, c.fx, c.width, c.height);
            RefreshVideoFxPictures();  // REAPER cannot see the FX state's override
            return;
        case CmdKind::CustomOutputSize:
            RunCustomOutputSize(c);
            RefreshVideoFxPictures();
            return;
        case CmdKind::Background:
            if (!TrackValid(c.track)) return;
            Undo_BeginBlock2(nullptr);
            SetVideoBackgroundTransparent(c.track, c.flag);
            Undo_EndBlock2(nullptr, c.flag ? "RAV: Video background transparent" : "RAV: Video background as viewer",
                           UNDO_STATE_TRACKCFG);
            VideoTimelineTick();       // the video thread sees the new option first...
            RefreshVideoFxPictures();  // ...then REAPER re-asks for its cached frames
            return;
        case CmdKind::OpenMatrix:
            if (!OpenRegionRenderMatrix())
                ShowMessageBox("RAV could not find REAPER's Region Render Matrix action. Open it from REAPER's "
                               "View menu (Region Render Matrix), or with the Render Matrix button of the "
                               "Region/Marker Manager.",
                               "RAV: Render", 0);
            return;
        case CmdKind::OpenRenderDialog:
            if (!OpenRenderDialog())
                ShowMessageBox("RAV could not find REAPER's Render action. Open it from File > Render...",
                               "RAV: Render", 0);
            return;
    }
}

}  // namespace

const VideoViewModel& GetVideoViewModel()
{
    return g_model;
}

bool VideoViewActive()
{
    return g_active;
}

void SetVideoViewActive(bool on)
{
    if (on && !g_panel_opened_once) {
        g_panel_opened_once = true;
        g_panel = true;
    }
    if (g_active != on) {
        CommitLive(/*view_gestures=*/true);  // a drag or wheel zoom never outlives its view
        if (!on) CommitLive(/*view_gestures=*/false);  // nor a panel slider: the panel goes too
        g_dirty = true;
    }
    g_active = on;
}

// The panel belongs to Video view: g_panel is only its open/closed choice, kept while
// RAV view shows (Antho 2026-10-03).
bool VideoPanelVisible()
{
    return g_active && g_panel;
}

void SetVideoPanelVisible(bool visible)
{
    // Opening it from RAV view (P, Tools > Video) enters Video view, where it lives.
    if (visible && !g_active) SetVideoViewActive(true);
    g_panel_opened_once = true;  // the user chose: never force it open again
    if (!visible) CommitLive(/*view_gestures=*/false);  // a slider never outlives the panel
    if (g_panel != visible) g_dirty = true;
    g_panel = visible;
}

void VideoViewPin(MediaTrack* track)
{
    MediaTrack* t = TrackValid(track) ? track : nullptr;
    if (t != g_pinned) g_dirty = true;
    g_pinned = t;
}

MediaTrack* VideoViewPinnedTrack()
{
    if (g_pinned && !TrackValid(g_pinned)) {
        g_pinned = nullptr;
        g_dirty = true;
    }
    return g_pinned;
}

void VideoViewFrame(MediaTrack* item_track, bool has_item, bool has_model, const glm::vec3& aabb_min,
                    const glm::vec3& aabb_max)
{
    if (item_track && !g_pinned && item_track != g_followed) {
        g_followed = item_track;
        g_dirty = true;
    }
    g_has_item = has_item;
    g_has_model = has_model;
    g_min = aabb_min;
    g_max = aabb_max;
    g_frame_cam_valid = false;

    const double now = Now();
    float dt = (g_last_now >= 0.0) ? static_cast<float>(now - g_last_now) : 0.0f;
    g_last_now = now;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > kMaxTweenStep) dt = kMaxTweenStep;

    if (!g_active) {
        // RAV view (the panel is Video view's): nothing reads the model, keep it cheap. Drop a tween; a wheel gesture cannot run.
        g_tweening = false;
        return;
    }

    const int count = GetProjectStateChangeCount(nullptr);
    if (g_dirty || count != g_last_state_count || CurrentTrack() != g_model_key ||
        now - g_last_refresh >= kRefreshSeconds) {
        Refresh();
        g_last_state_count = count;
        g_last_refresh = now;
    }

    g_playhead = Playhead();
    g_shot_index = VideoShotIndexAt(g_model.shots, g_playhead);
    UpdateStripRange();

    // A wheel gesture that went quiet becomes one undo point.
    if (g_live.kind == Gesture::Zoom && !g_live.written && now >= g_live.wheel_deadline)
        QueueLiveWrite(UndoNameFor(Gesture::Zoom));
    // The gesture's track or FX is gone: drop it.
    if (g_live.kind != Gesture::None && (g_live.track != g_model.track || g_live.fx != g_model.fx)) g_live = Live();

    if (g_tweening) {
        g_tween.AdvanceAnim(dt);
        if (!g_tween.animating) g_tweening = false;
    }

    if (VideoViewCanEdit()) {
        double v[vcam::kParamCount];
        ReadVideoCameraAt(g_model.track, g_model.fx, g_playhead, v);
        g_frame_cam = VideoCameraFromValues(v, g_min, g_max);
        g_frame_cam_valid = true;
    } else {
        g_tweening = false;
    }
}

bool VideoViewCanEdit()
{
    return g_model.status == VideoFxStatus::Active && g_model.track && g_model.fx >= 0 && g_has_item &&
           g_has_model;
}

bool VideoViewCamera(OrbitCamera* out)
{
    if (!out || !VideoViewCanEdit()) return false;
    if (g_live.kind == Gesture::Slider) {
        *out = VideoCameraFromValues(g_live.values, g_min, g_max);
        return true;
    }
    if (g_live.kind != Gesture::None) {
        *out = g_live.cam;
        return true;
    }
    if (g_tweening) {
        *out = g_tween;
        return true;
    }
    if (!g_frame_cam_valid) return false;
    *out = g_frame_cam;
    return true;
}

int VideoViewShotIndex()
{
    return g_shot_index;
}

bool VideoViewBeginDrag(bool pan)
{
    if (g_live.kind == Gesture::Zoom) QueueLiveWrite(UndoNameFor(Gesture::Zoom));  // a wheel gesture ends here
    if (g_live.kind != Gesture::None && !g_live.written) return false;            // a slider is moving
    return BeginLive(pan ? Gesture::Pan : Gesture::Orbit);
}

void VideoViewDrag(int dx, int dy)
{
    if (g_live.written) return;
    if (g_live.kind == Gesture::Orbit) g_live.cam.Orbit(dx, dy);
    else if (g_live.kind == Gesture::Pan) g_live.cam.Pan(dx, dy);
    else return;
    ClampLiveCamera();
}

void VideoViewEndDrag()
{
    if (g_live.kind == Gesture::Orbit || g_live.kind == Gesture::Pan) QueueLiveWrite(UndoNameFor(g_live.kind));
}

bool VideoViewDragging()
{
    return (g_live.kind == Gesture::Orbit || g_live.kind == Gesture::Pan) && !g_live.written;
}

void VideoViewWheel(float delta)
{
    if (g_live.kind != Gesture::Zoom || g_live.written) {
        if (g_live.kind != Gesture::None && !g_live.written) return;  // a drag or a slider owns the camera
        if (!BeginLive(Gesture::Zoom)) return;
    }
    g_live.cam.Zoom(delta);
    ClampLiveCamera();
    g_live.wheel_deadline = Now() + kWheelQuietSeconds;
}

bool VideoViewInspectorValues(double out[vcam::kParamCount])
{
    if (g_live.kind == Gesture::Slider ||
        ((g_live.kind == Gesture::Orbit || g_live.kind == Gesture::Pan || g_live.kind == Gesture::Zoom))) {
        for (int p = 0; p < vcam::kParamCount; ++p) out[p] = g_live.values[p];
        return true;
    }
    if (g_shot_index < 0 || g_shot_index >= static_cast<int>(g_model.shots.size())) return false;
    for (int p = 0; p < vcam::kParamCount; ++p) out[p] = g_model.shots[static_cast<size_t>(g_shot_index)].values[p];
    return true;
}

void VideoViewSliderEdit(int param, double norm)
{
    if (param < 0 || param >= vcam::kParamCount) return;
    if (g_live.kind != Gesture::Slider || g_live.written) {
        if (g_live.kind != Gesture::None && !g_live.written) return;  // a view gesture runs
        if (!BeginLive(Gesture::Slider)) return;
        g_live.slider_param = param;
    }
    g_live.values[param] = vcam::Sanitize(param, norm);
}

void VideoViewSliderCommit(int param)
{
    if (g_live.kind != Gesture::Slider || g_live.written) return;
    char undo[64];
    std::snprintf(undo, sizeof(undo), "RAV: Edit video shot %s", vcam::ParamName(param));
    QueueLiveWrite(undo);
}

void QueueVideoRename(MediaTrack* track, int fx, double shot_time, const std::string& name)
{
    if (!track || fx < 0) return;
    Cmd c;
    c.kind = CmdKind::Rename;
    c.track = track;
    c.fx = fx;
    c.shot_time = shot_time;
    c.text = name;
    g_queue.push_back(std::move(c));
}

void QueueVideoTransition(bool move_to_next)
{
    Cmd c;
    if (!CurrentShot(&c)) return;
    c.kind = CmdKind::Transition;
    c.flag = move_to_next;
    g_queue.push_back(std::move(c));
}

void QueueVideoCopyRavView(const OrbitCamera& free_camera)
{
    Cmd c;
    if (!VideoViewCanEdit() || !CurrentShot(&c)) return;
    c.kind = CmdKind::WriteShot;
    VideoCameraValuesFromCamera(free_camera, g_min, g_max, c.values);
    c.undo = "RAV: Copy RAV view to video shot";
    g_queue.push_back(std::move(c));
}

void QueueVideoFrameModel()
{
    Cmd c;
    if (!VideoViewCanEdit() || !CurrentShot(&c)) return;
    c.kind = CmdKind::WriteShot;
    c.values[vcam::kDistance] = vcam::DefaultNorm(vcam::kDistance);
    c.values[vcam::kTargetX] = vcam::DefaultNorm(vcam::kTargetX);
    c.values[vcam::kTargetY] = vcam::DefaultNorm(vcam::kTargetY);
    c.values[vcam::kTargetZ] = vcam::DefaultNorm(vcam::kTargetZ);
    c.undo = "RAV: Frame model in video shot";
    g_queue.push_back(std::move(c));
}

void QueueVideoSnap(const glm::vec3& dir)
{
    if (g_live.kind != Gesture::None && !g_live.written) return;  // a gesture owns the camera
    OrbitCamera start;
    Cmd c;
    if (!VideoViewCamera(&start) || !CurrentShot(&c)) return;
    OrbitCamera end = start;
    end.SnapToDirection(dir);
    if (!end.animating) return;  // degenerate direction
    end.yaw = end.targetYaw;
    end.pitch = end.targetPitch;
    end.animating = false;
    c.kind = CmdKind::WriteShot;
    VideoCameraValuesFromCamera(end, g_min, g_max, c.values);
    c.undo = "RAV: ViewCube video shot";
    g_queue.push_back(std::move(c));
    // The view eases to the new angle while REAPER stores it.
    g_tween = start;
    g_tween.SnapToDirection(dir);
    g_tweening = true;
}

void QueueVideoAddFx()
{
    if (!g_model.track) return;
    Cmd c;
    c.kind = CmdKind::AddFx;
    c.track = g_model.track;
    g_queue.push_back(std::move(c));
}

void QueueVideoEnableFx()
{
    if (!g_model.track || g_model.fx < 0) return;
    Cmd c;
    c.kind = CmdKind::EnableFx;
    c.track = g_model.track;
    c.fx = g_model.fx;
    g_queue.push_back(std::move(c));
}

void QueueVideoSetFxOnline()
{
    if (!g_model.track || g_model.fx < 0) return;
    Cmd c;
    c.kind = CmdKind::SetOnline;
    c.track = g_model.track;
    c.fx = g_model.fx;
    g_queue.push_back(std::move(c));
}

bool VideoViewHasPending()
{
    return !g_queue.empty();
}

void VideoViewRunPending()
{
    if (g_queue.empty()) return;
    // Swap first: a modal loop inside a command may run frames that queue more.
    std::vector<Cmd> run;
    run.swap(g_queue);
    for (const Cmd& c : run) RunOne(c);
    if (g_live.written) g_live = Live();  // the envelopes now hold what the gesture showed
    g_dirty = true;
}

void VideoViewOnViewerClosed()
{
    g_live = Live();
    g_tweening = false;
    g_queue.clear();
    g_has_item = false;
    g_has_model = false;
    g_frame_cam_valid = false;
}

// ---- Story 11-5 ----------------------------------------------------------------------------

VideoStripRange VideoViewStripRange()
{
    return g_range;
}

double VideoViewPlayhead()
{
    return g_playhead;
}

const char* VideoViewNotice()
{
    if (g_notice.empty() || Now() >= g_notice_until) return nullptr;
    return g_notice.c_str();
}

bool VideoViewCanCut()
{
    if (g_live.kind != Gesture::None && !g_live.written) return false;  // a gesture owns the shot
    return g_model.status == VideoFxStatus::Active && g_model.track && g_model.fx >= 0;
}

void QueueVideoSeek(double t)
{
    if (!std::isfinite(t)) return;
    if (!g_queue.empty() && g_queue.back().kind == CmdKind::Seek) {
        g_queue.back().shot_time = t;  // a drag in the strip: only the last position matters
        return;
    }
    Cmd c;
    c.kind = CmdKind::Seek;
    c.shot_time = t;
    g_queue.push_back(std::move(c));
}

void QueueVideoSeekToShot(int index)
{
    if (index < 0 || index >= static_cast<int>(g_model.shots.size())) return;
    QueueVideoSeek(VideoShotFirstFrameTime(g_model.shots[static_cast<size_t>(index)].time, g_model.fps));
}

void QueueVideoCut()
{
    if (g_live.kind == Gesture::Zoom) QueueLiveWrite(UndoNameFor(Gesture::Zoom));  // a wheel gesture ends here
    if (!VideoViewCanCut()) {
        if (g_model.status != VideoFxStatus::Active) SetNotice("Cut needs an active video FX on this track");
        else SetNotice("Finish the camera move first");
        return;
    }
    Cmd c;
    c.kind = CmdKind::Cut;
    c.track = g_model.track;
    c.fx = g_model.fx;
    c.shot_time = g_playhead;
    c.fps = g_model.fps;
    g_queue.push_back(std::move(c));
}

void QueueVideoDeleteShot(int index)
{
    if (!g_model.track || g_model.fx < 0) return;
    if (index <= 0 || index >= static_cast<int>(g_model.shots.size())) return;  // the first shot stays
    if (g_model.shots[static_cast<size_t>(index)].implicit) return;
    Cmd c;
    c.kind = CmdKind::DeleteShot;
    c.track = g_model.track;
    c.fx = g_model.fx;
    c.shot_time = g_model.shots[static_cast<size_t>(index)].time;
    g_queue.push_back(std::move(c));
}

void QueueVideoMoveShot(int index, double new_time)
{
    if (!std::isfinite(new_time)) return;
    if (g_live.kind == Gesture::Zoom) QueueLiveWrite(UndoNameFor(Gesture::Zoom));  // a wheel gesture ends here
    if (!VideoViewCanCut()) {
        if (g_model.status == VideoFxStatus::Active) SetNotice("Finish the camera move first");
        return;
    }
    if (index <= 0 || index >= static_cast<int>(g_model.shots.size())) return;  // the first shot's start stays
    const VideoShot& s = g_model.shots[static_cast<size_t>(index)];
    if (s.implicit) return;
    // Released on the frame it started on: nothing to write.
    if (g_model.fps > 0.0) {
        if (VideoShotFirstFrame(new_time, g_model.fps) == VideoShotFirstFrame(s.time, g_model.fps)) return;
    } else if (std::fabs(new_time - s.time) <= kVideoShotTimeTolerance) {
        return;
    }
    Cmd c;
    c.kind = CmdKind::MoveShot;
    c.track = g_model.track;
    c.fx = g_model.fx;
    c.shot_time = s.time;
    c.new_time = new_time;
    g_queue.push_back(std::move(c));
}

void QueueVideoApplyAngle(int index)
{
    if (index < 0 || index >= static_cast<int>(g_model.angles.size())) return;
    if (g_model.status != VideoFxStatus::Active) return;
    if (g_live.kind != Gesture::None && !g_live.written) return;  // a gesture owns the shot
    Cmd c;
    if (!CurrentShot(&c)) return;
    c.kind = CmdKind::WriteShot;
    for (int p = 0; p < vcam::kParamCount; ++p) c.values[p] = g_model.angles[static_cast<size_t>(index)].values[p];
    c.undo = "RAV: Apply saved angle";
    g_queue.push_back(std::move(c));
}

void QueueVideoSaveAngle(const std::string& name)
{
    const std::string clean = CleanVideoFxName(name);
    if (clean.empty()) return;
    Cmd c;
    if (!CurrentShot(&c)) return;  // the six values of the shot under the playhead
    c.kind = CmdKind::SaveAngle;
    c.text = clean;
    g_queue.push_back(std::move(c));
}

void QueueVideoDeleteAngle(const std::string& name)
{
    if (!g_model.track || g_model.fx < 0 || name.empty()) return;
    Cmd c;
    c.kind = CmdKind::DeleteAngle;
    c.track = g_model.track;
    c.fx = g_model.fx;
    c.text = name;
    g_queue.push_back(std::move(c));
}

void QueueVideoOutputSize(int width, int height)
{
    if (!g_model.track || g_model.fx < 0) return;
    Cmd c;
    c.kind = CmdKind::OutputSize;
    c.track = g_model.track;
    c.fx = g_model.fx;
    c.width = width;
    c.height = height;
    g_queue.push_back(std::move(c));
}

void QueueVideoCustomOutputSize()
{
    if (!g_model.track || g_model.fx < 0) return;
    Cmd c;
    c.kind = CmdKind::CustomOutputSize;
    c.track = g_model.track;
    c.fx = g_model.fx;
    c.width = g_model.override_w;  // the dialog starts from the override (empty = follow the project)
    c.height = g_model.override_h;
    g_queue.push_back(std::move(c));
}

void QueueVideoBackground(bool transparent)
{
    if (!g_model.track) return;
    Cmd c;
    c.kind = CmdKind::Background;
    c.track = g_model.track;
    c.flag = transparent;
    g_queue.push_back(std::move(c));
}

void QueueVideoOpenRenderMatrix()
{
    Cmd c;
    c.kind = CmdKind::OpenMatrix;
    g_queue.push_back(std::move(c));
}

void QueueVideoOpenRenderDialog()
{
    Cmd c;
    c.kind = CmdKind::OpenRenderDialog;
    g_queue.push_back(std::move(c));
}

VideoFrameRect ComputeVideoFrameRect(float area_w, float area_h, float top_band, float cube_room, int out_w,
                                     int out_h)
{
    constexpr float kSide = 12.0f;
    constexpr float kBottom = 10.0f;
    const float aspect = (out_w > 0 && out_h > 0) ? static_cast<float>(out_w) / static_cast<float>(out_h) : 16.0f / 9.0f;

    auto fit = [aspect](float w, float h, float* fw, float* fh) {
        w = std::max(w, 1.0f);
        h = std::max(h, 1.0f);
        *fw = w;
        *fh = w / aspect;
        if (*fh > h) {
            *fh = h;
            *fw = h * aspect;
        }
    };

    // A: the ViewCube in a right column.
    const float aw = area_w - 2.0f * kSide - cube_room;
    const float ah = area_h - top_band - kBottom;
    float a_w = 0.0f;
    float a_h = 0.0f;
    fit(aw, ah, &a_w, &a_h);
    // B: the ViewCube in a bottom band.
    const float bw = area_w - 2.0f * kSide;
    const float bh = area_h - top_band - cube_room;
    float b_w = 0.0f;
    float b_h = 0.0f;
    fit(bw, bh, &b_w, &b_h);

    VideoFrameRect r;
    if (a_w * a_h >= b_w * b_h) {
        r.cube_right = true;
        r.w = a_w;
        r.h = a_h;
        r.x = kSide + (std::max(aw, 1.0f) - a_w) * 0.5f;
        r.y = top_band + (std::max(ah, 1.0f) - a_h) * 0.5f;
    } else {
        r.cube_right = false;
        r.w = b_w;
        r.h = b_h;
        r.x = kSide + (std::max(bw, 1.0f) - b_w) * 0.5f;
        r.y = top_band + (std::max(bh, 1.0f) - b_h) * 0.5f;
    }
    r.x = std::floor(r.x);
    r.y = std::floor(r.y);
    r.w = std::max(1.0f, std::floor(r.w));
    r.h = std::max(1.0f, std::floor(r.h));
    return r;
}

}  // namespace rav

#endif  // _WIN32
