// SPDX-License-Identifier: MIT
//
// See video_timeline.h.

#include "video_timeline.h"

#ifdef _WIN32

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <utility>

#include "asset_cache.h"
#include "console_log.h"
#include "pcm_source_anim.h"
#include "reaper_api.h"
#include "video_fx_track.h"

namespace rav {
namespace {

constexpr char      kBackgroundKey[]   = "P_EXT:RAV_VIDEO_BG";
constexpr char      kTransparentValue[] = "transparent";
constexpr ULONGLONG kFullRebuildMs     = 1000;  // floor for edits without an undo point

std::mutex                                   g_mutex;
std::shared_ptr<const VideoTimelineSnapshot> g_snapshot;  // guarded by g_mutex

// Main-thread bookkeeping.
std::vector<std::pair<ReaProject*, int>> g_last_counts;  // per open project
ULONGLONG                                g_last_full = 0;
std::atomic<bool>                        g_dirty{true};
bool                                     g_logged_size_fallback = false;

std::shared_ptr<const VideoTimelineSnapshot> EmptySnapshot()
{
    static const std::shared_ptr<const VideoTimelineSnapshot> empty =
        std::make_shared<const VideoTimelineSnapshot>();
    return empty;
}

void Publish(std::shared_ptr<const VideoTimelineSnapshot> snap)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_snapshot = std::move(snap);
}

// One int project-config variable (Project Settings), 0 when unknown. The names are
// REAPER's (projectconfig_var_getoffs); a size other than an int is not trusted.
int ProjectConfigInt(ReaProject* proj, const char* name)
{
    int size = 0;
    const int offs = projectconfig_var_getoffs(name, &size);
    if (offs <= 0 || size != static_cast<int>(sizeof(int))) return 0;
    const void* addr = projectconfig_var_addr(proj, offs);
    if (!addr) return 0;
    int v = 0;
    std::memcpy(&v, addr, sizeof(v));
    return v;
}

// The project's video size (Project Settings > Video). 0 x 0 when not set; the video
// renderer then uses its default.
void ProjectVideoSize(ReaProject* proj, int& w, int& h)
{
    w = ProjectConfigInt(proj, "projvidw");
    h = ProjectConfigInt(proj, "projvidh");
    if (w <= 0 || h <= 0) {
        if (!g_logged_size_fallback) {
            g_logged_size_fallback = true;
            LogInfo("video FX: project video size not found (projvidw/projvidh = %d x %d), "
                    "using the default", w, h);
        }
        w = h = 0;
    }
}

void AddTrack(MediaTrack* tr, int vw, int vh, VideoTimelineSnapshot& out,
              std::vector<std::string>& paths)
{
    VideoTrackTimeline tl;
    tl.track        = tr;
    tl.video_width  = vw;
    tl.video_height = vh;
    tl.transparent  = VideoBackgroundTransparent(tr);
    const int n = CountTrackMediaItems(tr);
    for (int j = 0; j < n; ++j) {
        MediaItem* it = GetTrackMediaItem(tr, j);
        if (!it) continue;
        MediaItem_Take* tk = GetActiveTake(it);
        if (!tk) continue;
        PCM_source* src = GetMediaItemTake_Source(tk);
        if (!IsRavAnimSource(src)) continue;
        const char* fn = src->GetFileName();
        if (!fn || !fn[0]) continue;
        VideoItemSpan span;
        span.start      = GetMediaItemInfo_Value(it, "D_POSITION");
        span.end        = span.start + GetMediaItemInfo_Value(it, "D_LENGTH");
        span.start_offs = GetMediaItemTakeInfo_Value(tk, "D_STARTOFFS");
        span.path       = fn;
        paths.push_back(span.path);
        tl.items.push_back(std::move(span));
    }
    out.tracks.push_back(std::move(tl));
}

std::shared_ptr<const VideoTimelineSnapshot> Build(std::vector<std::string>& paths)
{
    auto snap = std::make_shared<VideoTimelineSnapshot>();
    for (int p = 0;; ++p) {
        ReaProject* proj = EnumProjects(p, nullptr, 0);
        if (!proj) break;
        int vw = 0, vh = 0;
        bool size_read = false;
        const int nt = CountTracks(proj);
        for (int i = 0; i < nt; ++i) {
            MediaTrack* tr = GetTrack(proj, i);
            if (!tr || TrackFX_GetCount(tr) <= 0) continue;
            if (FindVideoFxOnTrack(tr) < 0) continue;
            if (!size_read) {
                ProjectVideoSize(proj, vw, vh);
                size_read = true;
            }
            AddTrack(tr, vw, vh, *snap, paths);
        }
    }
    return snap;
}

}  // namespace

void ProjectVideoSizeOf(ReaProject* proj, int* w, int* h)
{
    int vw = 0;
    int vh = 0;
    ProjectVideoSize(proj, vw, vh);
    if (w) *w = vw;
    if (h) *h = vh;
}

const VideoTrackTimeline* VideoTimelineSnapshot::Find(const MediaTrack* track) const
{
    if (!track) return nullptr;
    for (const VideoTrackTimeline& t : tracks)
        if (t.track == track) return &t;
    return nullptr;
}

bool FindVideoItemAt(const VideoTimelineSnapshot& snap, const MediaTrack* track, double t,
                     VideoItemHit& out)
{
    const VideoTrackTimeline* tl = snap.Find(track);
    if (!tl) return false;
    for (const VideoItemSpan& it : tl->items) {
        // The viewer's "under the playhead" (start <= t < end), written so that a
        // non-finite t matches nothing.
        if (!(t >= it.start && t < it.end)) continue;
        out.track     = tl;
        out.item      = &it;
        out.anim_time = ItemAnimTime(t, it.start, it.start_offs);
        return true;
    }
    return false;
}

std::shared_ptr<const VideoTimelineSnapshot> CurrentVideoTimeline()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_snapshot ? g_snapshot : EmptySnapshot();
}

void VideoTimelineTick()
{
    try {
        // Which projects are open, and has any of them changed since the last build?
        std::vector<std::pair<ReaProject*, int>> counts;
        for (int p = 0;; ++p) {
            ReaProject* proj = EnumProjects(p, nullptr, 0);
            if (!proj) break;
            counts.emplace_back(proj, GetProjectStateChangeCount(proj));
        }
        const ULONGLONG now = GetTickCount64();
        const bool periodic = (now - g_last_full >= kFullRebuildMs);
        const bool changed  = (counts != g_last_counts);
        if (!g_dirty.exchange(false) && !changed && !periodic) return;

        std::vector<std::string> paths;
        Publish(Build(paths));
        g_last_counts = std::move(counts);
        if (periodic) g_last_full = now;
        // Keep the parsed files the FX tracks use; re-check them on disk once a second.
        TrimCpuAssetCache(paths, periodic);
    } catch (...) {
        // bad_alloc: keep the previous snapshot, try again next tick.
    }
}

void ClearVideoTimeline()
{
    Publish(nullptr);
    g_last_counts.clear();
    g_last_full = 0;
    g_dirty.store(true);
}

bool VideoBackgroundTransparent(MediaTrack* track)
{
    if (!track) return false;
    // The get copies the whole stored value: REAPER's API asks for a big buffer
    // (stringNeedBig), so a long hand-edited value cannot overflow it.
    char buf[4096] = {};
    if (!GetSetMediaTrackInfo_String(track, kBackgroundKey, buf, false)) return false;
    return std::strcmp(buf, kTransparentValue) == 0;
}

void SetVideoBackgroundTransparent(MediaTrack* track, bool transparent)
{
    if (!track) return;
    char value[32] = {};
    if (transparent) std::strcpy(value, kTransparentValue);  // "" deletes the key
    GetSetMediaTrackInfo_String(track, kBackgroundKey, value, true);
    g_dirty.store(true);
}

void ToggleVideoBackgroundOnSelectedTracks()
{
    const int n = CountSelectedTracks(nullptr);
    if (n <= 0) {
        ShowMessageBox("Select the track that carries the RAV video FX first.",
                       "RAV: Video FX transparent background", 0);
        return;
    }
    bool all_on = true;
    for (int i = 0; i < n; ++i)
        if (!VideoBackgroundTransparent(GetSelectedTrack(nullptr, i))) all_on = false;
    const bool turn_on = !all_on;
    Undo_BeginBlock2(nullptr);
    for (int i = 0; i < n; ++i) SetVideoBackgroundTransparent(GetSelectedTrack(nullptr, i), turn_on);
    Undo_EndBlock2(nullptr, turn_on ? "RAV: video FX background transparent"
                                    : "RAV: video FX background opaque",
                   UNDO_STATE_TRACKCFG);
    VideoTimelineTick();       // the video thread sees the new option first...
    RefreshVideoFxPictures();  // ...then REAPER re-asks for its cached frames
}

int VideoBackgroundToggleState()
{
    const int n = CountSelectedTracks(nullptr);
    if (n <= 0) return 0;
    for (int i = 0; i < n; ++i)
        if (!VideoBackgroundTransparent(GetSelectedTrack(nullptr, i))) return 0;
    return 1;
}

}  // namespace rav

#endif  // _WIN32
