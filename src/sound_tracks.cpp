// SPDX-License-Identifier: MIT
//
// See sound_tracks.h (13-11 decision 12, story 13-13).

#include "sound_tracks.h"

#ifdef _WIN32

#include "ravlib.h"

namespace rav {

namespace {

using InsertTrackAtIndexFn = void (*)(int idx, bool want_defaults);
using SetMediaTrackInfoValueFn = bool (*)(MediaTrack* tr, const char* parm, double v);

InsertTrackAtIndexFn     f_insert_track = nullptr;
SetMediaTrackInfoValueFn f_set_track = nullptr;

// The mark of a Switch-tree folder (IsSwitchTreeTrack): saved with the track in the project.
constexpr const char* kSwitchTreeExt = "P_EXT:RAV_SWITCH_TREE";

double DepthOf(MediaTrack* tr)
{
    return tr ? GetMediaTrackInfo_Value(tr, "I_FOLDERDEPTH") : 0.0;
}

void MarkSwitchTree(MediaTrack* tr)
{
    if (!tr || IsSwitchTreeTrack(tr)) return;
    char one[] = "1";
    GetSetMediaTrackInfo_String(tr, kSwitchTreeExt, one, true);
}

}  // namespace

void InitSoundTracks(void* (*get_func)(const char* name))
{
    if (!get_func) return;
    f_insert_track = reinterpret_cast<InsertTrackAtIndexFn>(get_func("InsertTrackAtIndex"));
    f_set_track = reinterpret_cast<SetMediaTrackInfoValueFn>(get_func("SetMediaTrackInfo_Value"));
}

bool CanMakeTracks()
{
    return f_insert_track && f_set_track;
}

std::string TrackName(MediaTrack* tr)
{
    char buf[512] = {};
    if (!tr || !GetSetMediaTrackInfo_String(tr, "P_NAME", buf, false)) return {};
    return buf;
}

int TrackIndex(ReaProject* proj, MediaTrack* tr)
{
    const int n = CountTracks(proj);
    for (int i = 0; i < n; ++i)
        if (GetTrack(proj, i) == tr) return i;
    return -1;
}

std::vector<int> DirectChildren(ReaProject* proj, int fi, int* last, int* beyond)
{
    std::vector<int> depths;
    const int        n = CountTracks(proj);
    for (int j = 0; j < n; ++j) {
        MediaTrack* t = GetTrack(proj, j);
        depths.push_back(t ? static_cast<int>(GetMediaTrackInfo_Value(t, "I_FOLDERDEPTH")) : 0);
    }
    std::vector<int> kids;
    *beyond = FolderChildren(depths, fi, &kids, last);
    return kids;
}

MediaTrack* AddTrackAt(ReaProject* proj, int idx, const std::string& name)
{
    if (!f_insert_track) return nullptr;
    f_insert_track(idx, false);
    MediaTrack* tr = GetTrack(proj, idx);
    if (!tr) return nullptr;
    std::string n = name;
    std::vector<char> buf(n.begin(), n.end());
    buf.push_back('\0');
    GetSetMediaTrackInfo_String(tr, "P_NAME", buf.data(), true);
    return tr;
}

MediaTrack* AddChildTrack(ReaProject* proj, MediaTrack* parent, const std::string& name)
{
    if (!CanMakeTracks()) return nullptr;
    const int fi = TrackIndex(proj, parent);
    if (fi < 0) return nullptr;
    const double d_parent = DepthOf(parent);
    if (d_parent < 1.0) {
        // Not a folder yet: it becomes one, its only child closing what it closed.
        MediaTrack* t = AddTrackAt(proj, fi + 1, name);
        if (!t) return nullptr;
        f_set_track(parent, "I_FOLDERDEPTH", 1.0);
        f_set_track(t, "I_FOLDERDEPTH", d_parent - 1.0);
        return t;
    }
    // A new child after the last descendant, closing the folder and the folders the old last one
    // closed above it (a nested folder).
    int              last = fi, beyond = 0;
    std::vector<int> kids = DirectChildren(proj, fi, &last, &beyond);
    MediaTrack*      old_last = GetTrack(proj, last);
    const double     d_last = last == fi ? 0.0 : DepthOf(old_last);
    MediaTrack*      t = AddTrackAt(proj, last + 1, name);
    if (!t) return nullptr;
    if (last != fi) f_set_track(old_last, "I_FOLDERDEPTH", d_last + 1.0 + beyond);
    f_set_track(t, "I_FOLDERDEPTH", -1.0 - beyond);
    return t;
}

bool IsSwitchTreeTrack(MediaTrack* tr)
{
    char buf[16] = {};
    return tr && GetSetMediaTrackInfo_String(tr, kSwitchTreeExt, buf, false) && buf[0] == '1';
}

MediaTrack* EnsureTrackPath(ReaProject* proj, const std::vector<TrackStep>& path, std::string* failed)
{
    if (failed) failed->clear();
    if (path.empty()) return nullptr;
    auto fail = [&](const std::string& name) -> MediaTrack* {
        if (failed) *failed = name;
        return nullptr;
    };
    if (!CanMakeTracks()) return fail(path.front().name);
    // The top level: a folder track named so (FolderFor's step 2), else a new folder at the bottom.
    MediaTrack* cur = nullptr;
    const int   n = CountTracks(proj);
    for (int i = 0; i < n && !cur; ++i) {
        MediaTrack* t = GetTrack(proj, i);
        if (t && DepthOf(t) >= 1.0 && TrackName(t) == path.front().name) cur = t;
    }
    if (!cur) {
        cur = AddTrackAt(proj, n, path.front().name);
        if (!cur) return fail(path.front().name);
        f_set_track(cur, "I_FOLDERDEPTH", 1.0);
    }
    // Each next level among the parent's direct children, made last when missing.
    for (size_t k = 1; k < path.size(); ++k) {
        MediaTrack* next = nullptr;
        const int   fi = TrackIndex(proj, cur);
        if (fi < 0) return fail(path[k].name);
        if (DepthOf(cur) >= 1.0) {
            int                      last = fi, beyond = 0;
            const std::vector<int>   kids = DirectChildren(proj, fi, &last, &beyond);
            std::vector<std::string> names;
            std::vector<MediaTrack*> tracks;
            for (int j : kids) {
                MediaTrack* t = GetTrack(proj, j);
                // The library folder (k == 1) is a Switch-tree folder or a folder: a Lands-in
                // folder's plain child named so is a layer track, never taken.
                if (k == 1 && !IsSwitchTreeTrack(t) && DepthOf(t) < 1.0) continue;
                names.push_back(TrackName(t));
                tracks.push_back(t);
            }
            const int m = MatchChildTrack(names, path[k].name);
            if (m >= 0) next = tracks[static_cast<size_t>(m)];
        }
        if (!next) next = AddChildTrack(proj, cur, path[k].name);
        if (!next) return fail(path[k].name);
        if (k == 1) MarkSwitchTree(next);  // the library folder: never a layer track (PickLayerChild)
        cur = next;
    }
    return cur;
}

}  // namespace rav

#endif  // _WIN32
