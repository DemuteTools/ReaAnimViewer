// SPDX-License-Identifier: MIT
//
// See sound_tracks.h (13-11 decision 12, story 13-13).

#include "sound_tracks.h"

#ifdef _WIN32

#include <algorithm>
#include <map>

#include "ravlib.h"
#include "sound_project.h"

namespace rav {

namespace {

using InsertTrackAtIndexFn = void (*)(int idx, bool want_defaults);
using SetMediaTrackInfoValueFn = bool (*)(MediaTrack* tr, const char* parm, double v);
using MoveMediaItemToTrackFn = bool (*)(MediaItem* item, MediaTrack* desttr);

InsertTrackAtIndexFn     f_insert_track = nullptr;
SetMediaTrackInfoValueFn f_set_track = nullptr;
MoveMediaItemToTrackFn   f_move_item = nullptr;

// The mark of a Switch-tree folder (IsSwitchTreeTrack): saved with the track in the project.
constexpr const char* kSwitchTreeExt = "P_EXT:RAV_SWITCH_TREE";
// 13-16 (B7): the mark of a track RAV made for sounds, saved with the track.
constexpr const char* kTrackMarkExt = "P_EXT:RAV_TRACK";
// 13-17 (N2): the name RAV last gave that track; a current name that differs = the user renamed it.
constexpr const char* kGivenNameExt = "P_EXT:RAV_TRACK_NAME";

// ---- 13-16: the gesture's marks and lanes ----

int                                g_batch = 0;
bool                               g_marks_read = false;
std::map<std::string, MediaTrack*> g_marks;  // mark -> track, the batch's (read once)
struct LaneItem {
    double     start = 0.0, end = 0.0;
    MediaItem* item = nullptr;
};
std::map<MediaTrack*, std::vector<LaneItem>> g_lanes;  // track -> its items, the batch's

std::string ReadMark(MediaTrack* tr)
{
    char buf[4096] = {};  // P_EXT copies the whole stored value
    if (!tr || !GetSetMediaTrackInfo_String(tr, kTrackMarkExt, buf, false)) return {};
    return buf;
}

void SetTrackText(MediaTrack* tr, const char* key, const std::string& text)
{
    std::vector<char> buf(text.begin(), text.end());
    buf.push_back('\0');
    GetSetMediaTrackInfo_String(tr, key, buf.data(), true);
}

std::string ReadGivenName(MediaTrack* tr)
{
    char buf[4096] = {};
    if (!tr || !GetSetMediaTrackInfo_String(tr, kGivenNameExt, buf, false)) return {};
    return buf;
}

// Marks a track RAV just made (named so): its mark, and its name as the one RAV gave (13-17 N2).
void WriteMark(MediaTrack* tr, const std::string& mark)
{
    if (!tr || mark.empty()) return;
    SetTrackText(tr, kTrackMarkExt, mark);
    SetTrackText(tr, kGivenNameExt, TrackName(tr));
    g_marks[mark] = tr;
}

std::string TrackGuidText(MediaTrack* tr)
{
    char buf[64] = {};
    if (!tr || !GetSetMediaTrackInfo_String(tr, "GUID", buf, false)) return {};
    return buf;
}

// The project's marks, and every track's GuidTrackMark (a RAV track with no mark, 13-11's "layer
// N", is found by it): read once a batch, every call outside one.
std::map<std::string, MediaTrack*>& Marks(ReaProject* proj)
{
    if (g_batch > 0 && g_marks_read) return g_marks;
    g_marks.clear();
    const int n = CountTracks(proj);
    for (int i = 0; i < n; ++i) {
        MediaTrack*       t = GetTrack(proj, i);
        const std::string m = ReadMark(t);
        if (!m.empty() && !g_marks.count(m)) g_marks[m] = t;  // the first one when copied
        const std::string g = TrackGuidText(t);
        if (!g.empty()) g_marks[GuidTrackMark(g)] = t;
    }
    g_marks_read = g_batch > 0;
    return g_marks;
}

LaneItem SpanItem(MediaItem* it)
{
    LaneItem li;
    li.item = it;
    li.start = GetMediaItemInfo_Value(it, "D_POSITION");
    li.end = li.start + GetMediaItemInfo_Value(it, "D_LENGTH");
    return li;
}

// The items of a track (cached in a batch).
std::vector<LaneItem>& LaneItems(MediaTrack* tr)
{
    static std::vector<LaneItem> scratch;
    std::vector<LaneItem>*       out = &scratch;
    if (g_batch > 0) {
        auto hit = g_lanes.find(tr);
        if (hit != g_lanes.end()) return hit->second;
        out = &g_lanes[tr];
    }
    out->clear();
    const int n = tr ? CountTrackMediaItems(tr) : 0;
    for (int i = 0; i < n; ++i)
        if (MediaItem* it = GetTrackMediaItem(tr, i)) out->push_back(SpanItem(it));
    return *out;
}

void DropLaneItem(MediaItem* it)
{
    for (auto& kv : g_lanes)
        kv.second.erase(std::remove_if(kv.second.begin(), kv.second.end(), [it](const LaneItem& l) { return l.item == it; }),
                        kv.second.end());
}

// The spans of a track's items but `skip`.
std::vector<LaneSpan> SpansOn(MediaTrack* tr, MediaItem* skip)
{
    std::vector<LaneSpan> out;
    for (const LaneItem& l : LaneItems(tr))
        if (l.item != skip) out.push_back({l.start, l.end, -1});
    return out;
}

// Moves `it` to `tr` (the lane cache in step). False when REAPER refused.
bool MoveToTrack(MediaItem* it, MediaTrack* tr)
{
    if (GetMediaItem_Track(it) != tr && (!f_move_item || !f_move_item(it, tr))) return false;
    if (g_batch > 0) {
        DropLaneItem(it);
        auto hit = g_lanes.find(tr);
        if (hit != g_lanes.end()) hit->second.push_back(SpanItem(it));
    }
    return true;
}

// The spill children of the track marked `base`, by k.
std::vector<std::pair<int, MediaTrack*>> LanesOf(ReaProject* proj, const std::string& base)
{
    std::vector<std::pair<int, MediaTrack*>> out;
    const std::map<std::string, MediaTrack*>& marks = Marks(proj);
    const std::string                         prefix = LaneMark(base, 0).substr(0, base.size() + 3);  // "<base>|S|"
    for (auto it = marks.lower_bound(prefix); it != marks.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
        std::string b;
        const int   k = LaneOfMark(it->first, &b);
        if (k > 0 && b == base && it->second) out.push_back({k, it->second});
    }
    std::sort(out.begin(), out.end(), [](const std::pair<int, MediaTrack*>& a, const std::pair<int, MediaTrack*>& b) {
        return a.first < b.first;
    });
    return out;
}

// The current name of the track carrying `mark` (NameForMark's pile source / "layer N" lookup).
std::string NameOfMarkedTrack(ReaProject* proj, const std::string& mark)
{
    const std::map<std::string, MediaTrack*>& marks = Marks(proj);
    auto                                      hit = marks.find(mark);
    return hit != marks.end() ? TrackName(hit->second) : std::string();
}

// A new spill child k of `base`, named after the Library (13-17 N1: "Heel01"), else "<base name>01".
MediaTrack* MakeLane(ReaProject* proj, MediaTrack* base, const std::string& base_mark, int k)
{
    const std::string lane = LaneMark(base_mark, k);
    std::string       name = NameForMark(GetSoundStore(proj), lane,
                                         [proj](const std::string& m) { return NameOfMarkedTrack(proj, m); });
    if (name.empty()) name = NumberedName(TrackName(base), k);
    MediaTrack* t = AddChildTrack(proj, base, name);
    if (t) WriteMark(t, lane);
    return t;
}

// The tag's tk of an item RAV moved to another lane.
void Retag(MediaItem* it, const std::string& mark)
{
    PlacedTag tag;
    if (!ReadPlacedTag(it, &tag) || tag.tk == mark) return;
    tag.tk = mark;
    WritePlacedTag(it, tag);
}

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
    f_move_item = reinterpret_cast<MoveMediaItemToTrackFn>(get_func("MoveMediaItemToTrack"));
    SetSoundStoreEditHook(&RefreshRavTrackNames);  // 13-17 N3: a Library rename renames the tracks
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
    char buf[4096] = {};  // P_EXT copies the whole stored value (see video_timeline.cpp)
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
    // 13-16 (B7): the deepest level found by its mark, wherever the user put it.
    MediaTrack* cur = nullptr;
    size_t      from = 1;
    {
        const std::map<std::string, MediaTrack*>& marks = Marks(proj);
        for (size_t k = path.size(); k-- > 1 && !cur;) {
            if (path[k].mark.empty()) continue;
            auto hit = marks.find(path[k].mark);
            if (hit != marks.end() && hit->second && TrackIndex(proj, hit->second) >= 0) {
                cur = hit->second;
                from = k + 1;
            }
        }
    }
    if (!cur) {
        // The top level: a folder track named so (FolderFor's step 2), else a new folder at the bottom.
        const int n = CountTracks(proj);
        for (int i = 0; i < n && !cur; ++i) {
            MediaTrack* t = GetTrack(proj, i);
            if (t && DepthOf(t) >= 1.0 && TrackName(t) == path.front().name) cur = t;
        }
        if (!cur) {
            cur = AddTrackAt(proj, n, path.front().name);
            if (!cur) return fail(path.front().name);
            f_set_track(cur, "I_FOLDERDEPTH", 1.0);
        }
    }
    // Each next level: a marked one is made (none found above); an unmarked one among the
    // parent's direct children by name (13-13), made last when missing.
    for (size_t k = from; k < path.size(); ++k) {
        MediaTrack* next = nullptr;
        const int   fi = TrackIndex(proj, cur);
        if (fi < 0) return fail(path[k].name);
        if (path[k].mark.empty() && DepthOf(cur) >= 1.0) {
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
        if (!next) {
            next = AddChildTrack(proj, cur, path[k].name);
            if (next) WriteMark(next, path[k].mark);
        }
        if (!next) return fail(path[k].name);
        if (k == 1) MarkSwitchTree(next);  // the library folder: never a layer track (PickLayerChild)
        cur = next;
    }
    return cur;
}

// ---- Story 13-16 ---------------------------------------------------------------------------------

void TracksBatch(bool on)
{
    if (on) {
        ++g_batch;
        return;
    }
    if (g_batch <= 0) return;
    if (--g_batch == 0) {
        g_marks.clear();
        g_marks_read = false;
        g_lanes.clear();
    }
}

std::string TrackMarkOf(MediaTrack* tr)
{
    if (!tr) return {};
    const std::string m = ReadMark(tr);
    if (!m.empty()) return m;
    const std::string g = TrackGuidText(tr);
    return g.empty() ? std::string() : GuidTrackMark(g);
}

MediaTrack* FindTrackByMark(ReaProject* proj, const std::string& mark)
{
    if (mark.empty()) return nullptr;
    std::map<std::string, MediaTrack*>& marks = Marks(proj);
    auto                                hit = marks.find(mark);
    if (hit != marks.end()) return TrackIndex(proj, hit->second) >= 0 ? hit->second : nullptr;
    if (mark[0] == '#' && g_batch > 0) {  // a track made during the batch (a new "layer 1"): looked up once
        const int n = CountTracks(proj);
        for (int i = 0; i < n; ++i) {
            MediaTrack* t = GetTrack(proj, i);
            if (t && GuidTrackMark(TrackGuidText(t)) == mark) return marks[mark] = t;
        }
    }
    return nullptr;
}

MediaTrack* EnsureMarkedChild(ReaProject* proj, MediaTrack* parent, const std::string& name, const std::string& mark)
{
    if (MediaTrack* t = FindTrackByMark(proj, mark)) return t;
    if (!parent) return nullptr;
    MediaTrack* t = AddChildTrack(proj, parent, name);
    if (t) WriteMark(t, mark);
    return t;
}

std::string SettleOnLanes(ReaProject* proj, MediaItem* item, MediaTrack* base, const std::string& base_mark)
{
    if (!item || !base || base_mark.empty() || !f_move_item) return {};
    const LaneItem me = SpanItem(item);
    const LaneSpan span{me.start, me.end, -1};
    auto stay = [&]() -> std::string {  // on the track itself
        if (!MoveToTrack(item, base)) return {};
        RefreshLaneItem(item);
        return base_mark;
    };
    std::vector<std::pair<int, MediaTrack*>> lanes = LanesOf(proj, base_mark);
    if (lanes.empty()) {
        // A plain track: the items on it and their tags' tk (PlanSpill picks who moves).
        std::vector<MediaItem*>  on;
        std::vector<LaneSpan>    spans;
        std::vector<std::string> tks;
        for (const LaneItem& l : LaneItems(base)) {
            if (l.item == item) continue;
            on.push_back(l.item);
            spans.push_back({l.start, l.end, -1});
        }
        // 13-19: PlanSpill stays exactly when the item fits; the tags (a read per item on the track,
        // for every sound placed) are read only when it spills.
        if (FitsLane(spans, span)) return stay();
        for (MediaItem* it : on) {
            PlacedTag tag;
            tks.push_back(ReadPlacedTag(it, &tag) ? tag.tk : std::string());
        }
        SpillPlan plan;
        if (!PlanSpill(spans, tks, base_mark, span, &plan)) return stay();
        std::vector<MediaTrack*> made;
        for (int k = 1; k <= plan.lanes; ++k) {
            MediaTrack* t = MakeLane(proj, base, base_mark, k);
            if (!t) break;
            made.push_back(t);
        }
        if (static_cast<int>(made.size()) < plan.lanes) return stay();  // could not make them: left there
        for (size_t i = 0; i < on.size(); ++i) {
            const int l = plan.lane_of[i];
            if (l >= 0 && MoveToTrack(on[i], made[static_cast<size_t>(l)])) Retag(on[i], LaneMark(base_mark, l + 1));
        }
        if (!MoveToTrack(item, made[static_cast<size_t>(plan.item_lane)])) return stay();
        return LaneMark(base_mark, plan.item_lane + 1);
    }
    // Spilled already: the first child that fits, else a new one (PickLaneK).
    std::vector<int>                   ks;
    std::vector<std::vector<LaneSpan>> lane_spans;
    for (const auto& l : lanes) {
        ks.push_back(l.first);
        lane_spans.push_back(SpansOn(l.second, item));
    }
    bool        made = false;
    const int   k = PickLaneK(ks, lane_spans, span, &made);
    MediaTrack* to = nullptr;
    if (made) to = MakeLane(proj, base, base_mark, k);
    else
        for (const auto& l : lanes)
            if (l.first == k) to = l.second;
    if (!to || !MoveToTrack(item, to)) return stay();
    return LaneMark(base_mark, k);
}

bool FixLaneOverlap(ReaProject* proj, MediaItem* item)
{
    if (!item) return false;
    PlacedTag tag;
    if (!ReadPlacedTag(item, &tag) || tag.tk.empty()) return false;
    MediaTrack* cur = GetMediaItem_Track(item);
    std::string base_mark;
    // On the track RAV uses for its mark (a copy carrying the same mark is not it), else theirs (B7b).
    if (!cur || PlacedSpotOf(tag.tk, cur == FindTrackByMark(proj, tag.tk), &base_mark) != PlacedSpot::OnRavTrack)
        return false;
    const LaneItem me = SpanItem(item);
    if (FitsLane(SpansOn(cur, item), {me.start, me.end, -1})) return false;
    MediaTrack* base = base_mark == tag.tk ? cur : FindTrackByMark(proj, base_mark);
    if (!base) return false;
    const std::string landed = SettleOnLanes(proj, item, base, base_mark);
    if (landed.empty() || landed == tag.tk) return false;
    tag.tk = landed;
    WritePlacedTag(item, tag);
    return true;
}

void ForgetLaneItem(MediaItem* item)
{
    if (item && g_batch > 0) DropLaneItem(item);
}

void RefreshLaneItem(MediaItem* item)
{
    if (!item || g_batch <= 0) return;
    DropLaneItem(item);
    auto hit = g_lanes.find(GetMediaItem_Track(item));
    if (hit != g_lanes.end()) hit->second.push_back(SpanItem(item));
}

// ---- Story 13-17 ---------------------------------------------------------------------------------

bool RefreshRavTrackNames(ReaProject* proj)
{
    try {
        const int n = CountTracks(proj);
        if (n <= 0) return false;
        // One scan: each track's mark (its own, and its GUID's) -> its current name.
        std::vector<MediaTrack*>           tracks;
        std::vector<std::string>           marks;
        std::map<std::string, std::string> names;
        for (int i = 0; i < n; ++i) {
            MediaTrack* t = GetTrack(proj, i);
            if (!t) continue;
            const std::string m = ReadMark(t);
            const std::string name = TrackName(t);
            if (!m.empty()) {
                tracks.push_back(t);
                marks.push_back(m);
                names.emplace(m, name);  // the first one when copied (as Marks)
            }
            const std::string g = TrackGuidText(t);
            if (!g.empty()) names[GuidTrackMark(g)] = name;
        }
        if (tracks.empty()) return false;
        const SoundStore& store = GetSoundStore(proj);
        auto name_of = [&names](const std::string& m) {
            auto hit = names.find(m);
            return hit != names.end() ? hit->second : std::string();
        };
        bool changed = false;
        for (size_t i = 0; i < tracks.size(); ++i) {
            const std::string want = NameForMark(store, marks[i], name_of);
            const TrackRename r = PlanTrackRename(TrackName(tracks[i]), ReadGivenName(tracks[i]), want);
            if (r.rename) SetTrackText(tracks[i], "P_NAME", want);
            if (r.record) SetTrackText(tracks[i], kGivenNameExt, want);
            changed = changed || r.rename || r.record;
        }
        return changed;
    } catch (...) {
        return false;
    }
}

}  // namespace rav

#endif  // _WIN32
