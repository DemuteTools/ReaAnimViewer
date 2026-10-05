// SPDX-License-Identifier: MIT
//
// See tag_markers.h.

#include "tag_markers.h"

#ifdef _WIN32

#include <cmath>
#include <algorithm>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "event_list.h"
#include "item_rules.h"
#include "reaper_api.h"
#include "shortcuts.h"  // LoadPrefFloat / SavePrefFloat
#include "tagging_session.h"

namespace rav {
namespace {

constexpr char kPrefMarkers[] = "tagging.markers";  // 0 take, 1 project, 2 both
constexpr char kUndoCommit[] = "RAV: Commit auto-tagging";
constexpr char kUndoCancel[] = "RAV: Cancel auto-tagging changes";
constexpr char kUndoOption[] = "RAV: Auto-tagging marker option";
// A take marker RAV wrote is recognised at the time it recorded (REAPER keeps the double).
constexpr double kOwnTakeTolS = 1e-5;

// Optional marker functions, resolved through GetFunc (a missing one never refuses the plugin).
using GetNumTakeMarkersFn = int (*)(MediaItem_Take*);
using GetTakeMarkerFn = double (*)(MediaItem_Take*, int, char*, int, int*);
using SetTakeMarkerFn = int (*)(MediaItem_Take*, int, const char*, double*, int*);
using DeleteTakeMarkerFn = bool (*)(MediaItem_Take*, int);
using AddProjectMarker2Fn = int (*)(ReaProject*, bool, double, double, const char*, int, int);
using AddRegionOrMarkerFn = ProjectMarker* (*)(ReaProject*, bool, double, double, const char*, int, int);
using DeleteProjectMarkerByIndexFn = bool (*)(ReaProject*, int);
using EnumProjectMarkers3Fn = int (*)(ReaProject*, int, bool*, double*, double*, const char**, int*, int*);
using GetRegionOrMarkerFn = ProjectMarker* (*)(ReaProject*, int, const char*);
using GetRegionOrMarkerInfo_ValueFn = double (*)(ReaProject*, ProjectMarker*, const char*);
using GetSetRegionOrMarkerInfo_StringFn = bool (*)(ReaProject*, ProjectMarker*, const char*, char*, bool);
using GetNumRegionsOrMarkersFn = int (*)(ReaProject*);
using ColorToNativeFn = int (*)(int, int, int);
using SetRegionOrMarkerInfo_ValueFn = double (*)(ReaProject*, ProjectMarker*, const char*, double);
using SetProjectMarkerByIndex2Fn = bool (*)(ReaProject*, int, bool, double, double, int, const char*, int, int);

GetNumTakeMarkersFn               f_num_take_markers = nullptr;
GetTakeMarkerFn                   f_get_take_marker = nullptr;
SetTakeMarkerFn                   f_set_take_marker = nullptr;
DeleteTakeMarkerFn                f_delete_take_marker = nullptr;
AddProjectMarker2Fn               f_add_project_marker2 = nullptr;
AddRegionOrMarkerFn               f_add_region_or_marker = nullptr;
DeleteProjectMarkerByIndexFn      f_delete_project_marker = nullptr;
EnumProjectMarkers3Fn             f_enum_project_markers3 = nullptr;
GetRegionOrMarkerFn               f_get_region_or_marker = nullptr;
GetRegionOrMarkerInfo_ValueFn     f_marker_value = nullptr;
GetSetRegionOrMarkerInfo_StringFn f_marker_string = nullptr;
GetNumRegionsOrMarkersFn          f_num_regions_or_markers = nullptr;
ColorToNativeFn                   f_color_to_native = nullptr;
SetRegionOrMarkerInfo_ValueFn     f_set_marker_value = nullptr;  // 10-4b: the mirror's moves
SetProjectMarkerByIndex2Fn        f_set_marker_by_index2 = nullptr;  // ... fallback

int        g_mode = -1;  // -1 = not read yet
ApplyResult g_last;

bool TakeApiReady()
{
    return f_num_take_markers && f_get_take_marker && f_set_take_marker && f_delete_take_marker;
}

bool ProjectApiReady()
{
    return (f_add_region_or_marker || f_add_project_marker2) && f_delete_project_marker && f_enum_project_markers3 &&
           f_get_region_or_marker && f_marker_value && f_marker_string;
}

// Block::color (0 = none, else 0x1000000 | 0xRRGGBB) -> REAPER's native colour | 0x1000000, 0 = default.
int NativeColor(uint32_t c)
{
    if (!(c & 0x1000000u)) return 0;
    const int r = static_cast<int>((c >> 16) & 0xFF), g = static_cast<int>((c >> 8) & 0xFF),
              b = static_cast<int>(c & 0xFF);
    const int native = f_color_to_native ? f_color_to_native(r, g, b) : (r | (g << 8) | (b << 16));
    return native | 0x1000000;
}

std::string MarkerGuid(ProjectMarker* m, ReaProject* proj = nullptr)
{
    if (!m) return "";
    char buf[256] = {};
    if (!f_marker_string(proj, m, "GUID", buf, false)) return "";
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

// The enumeration index (EnumProjectMarkers3 / DeleteProjectMarkerByIndex) of a project marker
// (not a region), or -1.
int EnumIndexOf(ProjectMarker* m, ReaProject* proj = nullptr)
{
    if (!m || f_marker_value(proj, m, "B_ISREGION") != 0.0) return -1;
    const double pos = f_marker_value(proj, m, "D_STARTPOS");
    const int    num = static_cast<int>(f_marker_value(proj, m, "I_NUMBER"));
    auto is_it = [&](int idx) {
        bool        rgn = false;
        double      p = 0.0, e = 0.0;
        const char* nm = nullptr;
        int         n = -1, col = 0;
        if (!f_enum_project_markers3(proj, idx, &rgn, &p, &e, &nm, &n, &col)) return false;
        return !rgn && n == num && std::fabs(p - pos) < 1e-9;
    };
    const int hint = static_cast<int>(f_marker_value(proj, m, "I_INDEX"));
    if (hint >= 0 && is_it(hint)) return hint;
    for (int i = 0; i < 100000; ++i) {
        bool        rgn = false;
        double      p = 0.0, e = 0.0;
        const char* nm = nullptr;
        int         n = -1, col = 0;
        if (!f_enum_project_markers3(proj, i, &rgn, &p, &e, &nm, &n, &col)) break;
        if (!rgn && n == num && std::fabs(p - pos) < 1e-9) return i;
    }
    return -1;
}

// The project's markers (not regions), as ExistingMarker.
std::vector<ExistingMarker> ProjectMarkers()
{
    std::vector<ExistingMarker> out;
    for (int i = 0; i < 100000; ++i) {
        bool        rgn = false;
        double      p = 0.0, e = 0.0;
        const char* nm = nullptr;
        int         n = -1, col = 0;
        if (!f_enum_project_markers3(nullptr, i, &rgn, &p, &e, &nm, &n, &col)) break;
        if (!rgn) out.push_back({p, nm ? nm : ""});
    }
    return out;
}

std::vector<ExistingMarker> TakeMarkers(MediaItem_Take* take)
{
    std::vector<ExistingMarker> out;
    const int n = f_num_take_markers(take);
    for (int i = 0; i < n; ++i) {
        char         name[512] = {};
        const double pos = f_get_take_marker(take, i, name, sizeof(name), nullptr);
        name[sizeof(name) - 1] = '\0';
        out.push_back({pos, name});
    }
    return out;
}

// Deletes the take markers RAV recorded (exact name, time within kOwnTakeTolS), each once.
void DeleteOwnTakeMarkers(MediaItem_Take* take, const std::vector<TakeMarkerRef>& own)
{
    for (const TakeMarkerRef& ref : own) {
        const std::vector<ExistingMarker> now = TakeMarkers(take);
        const int i = FindTwinMarker(now, ref.t, ref.name, kOwnTakeTolS);
        if (i >= 0) f_delete_take_marker(take, i);
    }
}

// The project marker (not a region) with this GUID, or null.
ProjectMarker* FindProjectMarker(const std::string& guid, ReaProject* proj = nullptr)
{
    if (guid.empty()) return nullptr;
    ProjectMarker* m = f_get_region_or_marker(proj, -1, guid.c_str());
    if (!m || MarkerGuid(m, proj) != guid) return nullptr;
    return m;
}

// Deletes one project marker by GUID; true when it was there and went.
bool DeleteProjectMarkerByGuid(const std::string& guid, ReaProject* proj = nullptr)
{
    ProjectMarker* m = FindProjectMarker(guid, proj);
    if (!m) return false;
    const int idx = EnumIndexOf(m, proj);
    return idx >= 0 && f_delete_project_marker(proj, idx);
}

// Deletes the project markers RAV recorded, by GUID (a GUID no longer there is ignored).
void DeleteOwnProjectMarkers(const std::vector<ProjectMarkerRef>& own)
{
    for (const ProjectMarkerRef& ref : own) DeleteProjectMarkerByGuid(ref.guid);
}

// Adds a project marker; its GUID ("" when it could not be added or found).
std::string AddProjectMarker(double pos, const std::string& name, int color, ReaProject* proj = nullptr)
{
    if (f_add_region_or_marker)
        return MarkerGuid(f_add_region_or_marker(proj, false, pos, pos, name.c_str(), -1, color), proj);
    const int num = f_add_project_marker2(proj, false, pos, pos, name.c_str(), -1, color);
    if (num < 0) return "";
    const int count = f_num_regions_or_markers ? f_num_regions_or_markers(proj) : 100000;
    for (int i = 0; i < count; ++i) {
        ProjectMarker* m = f_get_region_or_marker(proj, i, nullptr);
        if (!m) break;
        if (f_marker_value(proj, m, "B_ISREGION") == 0.0 && static_cast<int>(f_marker_value(proj, m, "I_NUMBER")) == num &&
            std::fabs(f_marker_value(proj, m, "D_STARTPOS") - pos) < 1e-9)
            return MarkerGuid(m, proj);
    }
    return "";
}

// The item's GUID ("" when it cannot be read).
std::string ItemGuidOf(MediaItem* item)
{
    if (!item) return "";
    char buf[128] = {};
    if (!GetSetMediaItemInfo_String(item, "GUID", buf, false)) return "";
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

// Removes from `existing` the markers this run wrote (they are RAV's, not foreign).
void DropOwn(std::vector<ExistingMarker>& existing, const std::vector<ExistingMarker>& own)
{
    for (const ExistingMarker& o : own) {
        const int i = FindTwinMarker(existing, o.t, o.name, 1e-9);
        if (i >= 0) existing.erase(existing.begin() + i);
    }
}

// Why the marker functions the option needs are missing ("" = they are all there).
const char* MissingApiReason(MarkerMode mode)
{
    const bool want_take = mode != MarkerMode::Project;
    const bool want_project = mode != MarkerMode::Take;
    if (want_project && !ProjectApiReady()) return "Project markers need REAPER 7 or newer";
    if (want_take && !TakeApiReady()) return "Take markers need REAPER 5.981 or newer";
    return "";
}

// Deletes the markers RAV recorded as its own on this item, committed AND previews (both kinds:
// an option change removes the other kind).
void DeleteItemOwnMarkers(MediaItem* item, const ItemRules& rules)
{
    MediaItem_Take* take = RavTakeOf(item);
    if (!take) return;
    if (TakeApiReady()) {
        DeleteOwnTakeMarkers(take, rules.tmarkers);
        DeleteOwnTakeMarkers(take, rules.ptmarkers);
    }
    if (ProjectApiReady()) {
        DeleteOwnProjectMarkers(rules.pmarkers);
        DeleteOwnProjectMarkers(rules.ppmarkers);
    }
}

// Deletes only the item's preview markers.
void DeleteItemPreviewMarkers(MediaItem* item, const ItemRules& rules)
{
    MediaItem_Take* take = RavTakeOf(item);
    if (!take) return;
    if (TakeApiReady()) DeleteOwnTakeMarkers(take, rules.ptmarkers);
    if (ProjectApiReady()) DeleteOwnProjectMarkers(rules.ppmarkers);
}

// Writes `plan` on one item as the option says (the markers RAV owns under these names already
// deleted). No foreign twin duplicated, none written twice. `written_project`: the project
// markers this run already wrote (RAV's, never foreign), grown here. What was written goes to
// `own_take` / `own_project`; counts to `res`.
void WritePlan(MediaItem_Take* take, const std::vector<PlannedMarker>& plan, MarkerMode mode,
               std::vector<ExistingMarker>& written_project, std::vector<TakeMarkerRef>& own_take,
               std::vector<ProjectMarkerRef>& own_project, ApplyResult& res)
{
    const bool want_take = mode != MarkerMode::Project && TakeApiReady();
    const bool want_project = mode != MarkerMode::Take && ProjectApiReady();
    // What each planned marker writes or records (event_list.h, pure); here only REAPER's side.
    const std::vector<MarkerWrite> writes = PlanMarkerWrites(plan, want_take, want_project);
    if (want_take) {
        std::vector<ExistingMarker> foreign = TakeMarkers(take);
        std::vector<ExistingMarker> mine;  // this run's take markers on this take
        for (const MarkerWrite& w : writes) {
            if (!w.take) continue;
            const PlannedMarker& m = plan[w.planned];
            if (FindTwinMarker(mine, m.clip_t, m.name) >= 0) continue;  // already written now: no duplicate
            if (FindTwinMarker(foreign, m.clip_t, m.name) >= 0) {
                ++res.already_present;
                continue;
            }
            double pos = m.clip_t;
            int    col = NativeColor(m.color);
            if (f_set_take_marker(take, -1, m.name.c_str(), &pos, col ? &col : nullptr) < 0) continue;
            TakeMarkerRef ref;
            ref.t = m.clip_t;
            ref.name = m.name;
            own_take.push_back(ref);
            mine.push_back({m.clip_t, m.name});
            ++res.markers;
        }
    }
    if (want_project) {
        std::vector<ExistingMarker> foreign = ProjectMarkers();
        DropOwn(foreign, written_project);
        for (const MarkerWrite& w : writes) {
            if (w.hidden) {
                // 10-4b: outside the item: recorded hidden (no GUID; its `t` an extrapolation),
                // the mirror shows it when the item is extended over it.
                own_project.push_back(w.ref);
                continue;
            }
            if (!w.project) continue;
            const PlannedMarker& m = plan[w.planned];
            if (FindTwinMarker(written_project, m.project_t, m.name) >= 0) continue;  // already written now
            if (FindTwinMarker(foreign, m.project_t, m.name) >= 0) {
                ++res.already_present;
                continue;
            }
            const std::string guid = AddProjectMarker(m.project_t, m.name, NativeColor(m.color));
            if (guid.empty()) continue;
            ProjectMarkerRef ref = w.ref;
            ref.guid = guid;
            own_project.push_back(ref);
            written_project.push_back({m.project_t, m.name});
            ++res.markers;
        }
    }
}

// Commit on one item: writes its markers from its detection (its own RAV markers, committed
// and previews, already deleted), then records them as RAV's with the applied snapshot, no
// preview left, and stores the record as the Cancel snapshot (no undo point: the caller's
// block holds it). Counts go to `res`.
void CommitItemMarkers(MediaItem* item, const ItemDetection& det, MarkerMode mode,
                       std::vector<ExistingMarker>& written_project, ApplyResult& res)
{
    try {
        MediaItem_Take* take = RavTakeOf(item);
        if (!take) {
            ++res.failed;
            return;
        }
        const ItemRules&                 rules = det.rules;
        const std::vector<ShownEvent>    list = BuildEventList(det.events, rules.events, rules.blocks.size());
        const std::vector<PlannedMarker> plan = PlanMarkers(list, rules.blocks, det.map);

        std::vector<TakeMarkerRef>    own_take;
        std::vector<ProjectMarkerRef> own_project;
        WritePlan(take, plan, mode, written_project, own_take, own_project, res);

        // The record: the markers RAV now owns, the snapshot and the applied state, no preview.
        const bool  keep_project_refs = !ProjectApiReady();  // could not delete them: still RAV's
        const bool  keep_take_refs = !TakeApiReady();
        std::string err, snapshot;
        const bool  ok = ModifyItemRulesNoUndo(
            item,
            [&](ItemRules& rec) {
                ComposeCommittedRecord(rec, own_take, own_project, keep_take_refs, keep_project_refs, plan, mode);
                // 10-4b: the markers just written are this item's (a copy becomes their owner).
                const std::string owner = ItemGuidOf(item);
                if (!owner.empty()) SetRecordOwner(rec, owner);
                snapshot = SerializeItemRules(rec);
                return true;
            },
            &err);
        if (ok && !snapshot.empty()) WriteCommittedSnapshotNoUndo(item, snapshot, nullptr);
        if (ok) ++res.items;
        else ++res.failed;
    } catch (...) {
        ++res.failed;
    }
}

// Rewrites one item's preview markers from its current result (no undo point: the caller's
// block holds it). "" when it went through (or nothing needed writing), else why not.
std::string RewriteItemPreviewsNoUndo(MediaItem* item)
{
    try {
        ItemRulesRead rd;
        if (!ReadItemRules(item, &rd) || !rd.present || !rd.valid) return "";
        const ItemRules& cur = rd.rules;
        const MarkerMode mode = GetTaggingMarkerMode();

        // What the previews should show now (nothing when the result cannot be computed).
        const ItemDetection        det = DetectItem(item);
        std::vector<PlannedMarker> plan;
        bool                       want = false;
        if (det.status == ItemDetection::Status::Ok) {
            const std::vector<ShownEvent> list = BuildEventList(det.events, det.rules.events, det.rules.blocks.size());
            plan = PlanMarkers(list, det.rules.blocks, det.map);
            want = PreviewNeeded(det.rules, plan, mode);
        }
        if (!want && !HasPreviews(cur)) return "";  // nothing shown, nothing to show
        // The previews already show this result with this option: nothing to rewrite.
        if (want && cur.has_previewed && cur.previewed.mode == mode && cur.previewed.sig == MarkerSignature(plan, mode))
            return "";

        std::string why;
        if (want) why = MissingApiReason(mode);
        DeleteItemPreviewMarkers(item, cur);
        std::vector<TakeMarkerRef>    own_take;
        std::vector<ProjectMarkerRef> own_project;
        if (want && why.empty()) {
            MediaItem_Take* take = RavTakeOf(item);
            if (take) {
                std::vector<ExistingMarker> written_project;
                ApplyResult                 counts;
                WritePlan(take, PreviewPlan(plan), mode, written_project, own_take, own_project, counts);
            }
        }
        const bool  keep_project_refs = !ProjectApiReady();
        const bool  keep_take_refs = !TakeApiReady();
        std::string err;
        ModifyItemRulesNoUndo(
            item,
            [&](ItemRules& rec) {
                std::vector<ProjectMarkerRef> pkeep = keep_project_refs ? rec.ppmarkers : std::vector<ProjectMarkerRef>{};
                std::vector<TakeMarkerRef>    tkeep = keep_take_refs ? rec.ptmarkers : std::vector<TakeMarkerRef>{};
                // 10-4b: the previews keep the record's owner (a copy not mirrored yet stays a copy).
                const std::string owner = RecordOwner(rec);
                ClearPreviewed(rec);
                rec.ptmarkers = tkeep;
                rec.ppmarkers = pkeep;
                for (const TakeMarkerRef& t : own_take) rec.ptmarkers.push_back(t);
                for (const ProjectMarkerRef& p : own_project) rec.ppmarkers.push_back(p);
                if (want && why.empty()) {
                    RecordPreviewed(rec, plan, mode);
                    // No owner yet: the previews are this item's.
                    rec.previewed.item = owner.empty() ? ItemGuidOf(item) : owner;
                }
                return true;
            },
            &err);
        UpdateArrange();
        if (!why.empty()) return why;
        return err;
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "unexpected error";
    }
}

// ModifyItemRules' hook (item_rules.h), inside the gesture's undo block: the first Cancel
// snapshot, then the item's previews.
void OnItemRulesWritten(MediaItem* item, bool before_valid, const ItemRules& before, const std::string& before_raw,
                        const ItemRules& after)
{
    try {
        g_last = ApplyResult{};  // the footer's last Commit / Cancel line is stale now
        // No snapshot yet: the state when the item first got its rules.
        const std::string first =
            FirstCommittedSnapshot(ReadCommittedSnapshot(item, nullptr), before_valid, before, before_raw, after);
        if (!first.empty()) WriteCommittedSnapshotNoUndo(item, first, nullptr);
        const std::string why = RewriteItemPreviewsNoUndo(item);
        if (!why.empty()) g_last.error = "Saved, but the preview markers could not be written: " + why + ".";
    } catch (...) {
    }
}


// ---- 10-4b: the marker mirror ----------------------------------------------------------------------

// What the mirror saw of one item on its last scan.
struct MirrorItemSeen {
    size_t                   raw_hash = 0;
    ItemClipMap              map;
    bool                     map_ok = false;
    bool                     managed = false;
    std::string              owner;   // the record's owner
    std::vector<std::string> guids;   // the project-marker GUIDs its record lists
};

// One open project's mirror state.
struct MirrorProject {
    int                                      count = -1;  // its state-change count when last mirrored
    std::string                              file;        // the tab's project file (File > Open reuses the tab)
    std::map<std::string, MirrorItemSeen>    items;       // by item GUID
    std::map<std::string, std::string>       owners;      // marker GUID -> item GUID (last scan)
    std::set<std::string>                    deleted;     // marker GUIDs the mirror itself deleted
};

std::map<ReaProject*, MirrorProject> g_mirror;
int (*g_mirror_register)(const char*, void*) = nullptr;
bool g_mirror_on = false;

bool MirrorApiReady()
{
    return ProjectApiReady() && (f_set_marker_value || f_set_marker_by_index2);
}

bool SameMap(const ItemClipMap& a, const ItemClipMap& b)
{
    return a.item_pos == b.item_pos && a.item_len == b.item_len && a.start_offs == b.start_offs && a.rate == b.rate &&
           a.clip_len == b.clip_len && a.loop == b.loop;
}

// The take's record text, without the 1 MB zero-fill ReadItemRules does (the mirror reads every
// RAV item of a changed project). "" when there is none.
std::string ReadRecordRaw(MediaItem_Take* take)
{
    static std::vector<char> buf;
    if (buf.size() != kItemRulesMaxBytes + 1) buf.assign(kItemRulesMaxBytes + 1, '\0');
    buf[0] = '\0';
    if (!GetSetMediaItemTakeInfo_String(take, kItemRulesKey, buf.data(), false)) return "";
    buf[kItemRulesMaxBytes] = '\0';
    return buf.data();
}

// The item's clip map from REAPER alone (no asset loaded): the twin of tagging_session's
// ItemClipMapOf, with the clip length from the RAV source (GetMediaSourceLength).
bool MirrorClipMap(MediaItem* item, MediaItem_Take* take, ItemClipMap* out)
{
    PCM_source* src = GetMediaItemTake_Source(take);
    for (int depth = 0; src && depth < 4; ++depth) {
        const char* type = src->GetType();
        if (type && std::strcmp(type, "RAV_ANIM") == 0) break;
        src = src->GetSource();
    }
    if (!src) return false;
    bool         qn = false;
    const double len = GetMediaSourceLength(src, &qn);
    if (qn || !(len > 0.0) || !std::isfinite(len)) return false;
    ItemClipMap map;
    map.clip_len = len;
    map.item_pos = GetMediaItemInfo_Value(item, "D_POSITION");
    map.item_len = GetMediaItemInfo_Value(item, "D_LENGTH");
    map.loop = GetMediaItemInfo_Value(item, "B_LOOPSRC") != 0.0;
    map.start_offs = GetMediaItemTakeInfo_Value(take, "D_STARTOFFS");
    const double rate = GetMediaItemTakeInfo_Value(take, "D_PLAYRATE");
    map.rate = (rate > 0.0 && std::isfinite(rate)) ? rate : 1.0;
    *out = map;
    return true;
}

// Moves the project marker with this GUID; true when it moved (read back by GUID: REAPER may
// re-sort its markers on a move).
bool MoveProjectMarker(const std::string& guid, double t, ReaProject* proj)
{
    ProjectMarker* m = FindProjectMarker(guid, proj);
    if (!m) return false;
    if (f_set_marker_value) {
        f_set_marker_value(proj, m, "D_STARTPOS", t);
    } else {
        const int idx = EnumIndexOf(m, proj);
        if (idx < 0) return false;
        bool        rgn = false;
        double      p = 0.0, e = 0.0;
        const char* nm = nullptr;
        int         n = -1, col = 0;
        if (!f_enum_project_markers3(proj, idx, &rgn, &p, &e, &nm, &n, &col)) return false;
        // An empty name leaves the name as it is.
        f_set_marker_by_index2(proj, idx, false, t, t, n, "", col, 0);
    }
    ProjectMarker* after = FindProjectMarker(guid, proj);
    return after && std::fabs(f_marker_value(proj, after, "D_STARTPOS") - t) < 1e-6;
}

std::vector<std::string> RefGuids(const ItemRules& r)
{
    std::vector<std::string> out;
    for (const ProjectMarkerRef& m : r.pmarkers)
        if (!m.guid.empty()) out.push_back(m.guid);
    for (const ProjectMarkerRef& m : r.ppmarkers)
        if (!m.guid.empty()) out.push_back(m.guid);
    return out;
}

// One RAV item read on this scan.
struct MirrorItem {
    MediaItem*      item = nullptr;
    std::string     guid;
    bool            fresh = false;  // read now (else unchanged since the last scan: `seen` holds it)
    bool            first = false;  // not seen on the last scan
    bool            map_changed = false;
    ItemRules       rules;
    MirrorItemSeen  seen;
    MirrorOwnership own = MirrorOwnership::Own;
    bool            holds = true;  // holds its markers' GUIDs after this scan (a copy: once refreshed)
};

// Applies the plan to one list of refs; true when the record changed.
bool MirrorRefs(std::vector<ProjectMarkerRef>& refs, const ItemClipMap& map, bool map_changed, bool copy,
                MirrorProject& mp, ReaProject* proj, bool* timeline_changed, std::vector<std::string>& created)
{
    std::vector<MirrorRefState> state(refs.size());
    for (size_t i = 0; i < refs.size(); ++i) {
        if (refs[i].guid.empty()) continue;
        ProjectMarker* m = FindProjectMarker(refs[i].guid, proj);
        state[i].exists = m != nullptr;
        if (m) state[i].now_t = f_marker_value(proj, m, "D_STARTPOS");
        state[i].restore = mp.deleted.count(refs[i].guid) > 0;
    }
    bool changed = false;
    for (const MirrorStep& st : MirrorPlan(refs, state, map, map_changed, copy)) {
        ProjectMarkerRef& r = refs[st.ref];
        switch (st.action) {
        case MirrorAction::Keep: break;
        case MirrorAction::Move:
            if (MoveProjectMarker(r.guid, st.new_t, proj)) {
                r.t = st.new_t;
                changed = *timeline_changed = true;
            }
            break;
        case MirrorAction::Hide:
            if (st.delete_old && DeleteProjectMarkerByGuid(r.guid, proj)) {
                mp.deleted.insert(r.guid);
                *timeline_changed = true;
            }
            r.guid.clear();
            changed = true;
            break;
        case MirrorAction::Show: {
            const std::string g = AddProjectMarker(st.new_t, r.has_name ? r.name : "", NativeColor(r.color), proj);
            if (copy && g.empty()) {
                r.guid.clear();  // never keep the original's GUID on a copy
                changed = true;
                break;
            }
            if (g.empty()) break;
            created.push_back(g);
            r.guid = g;
            r.t = st.new_t;
            changed = *timeline_changed = true;
            break;
        }
        }
    }
    return changed;
}

// Mirrors one project (its state-change count moved).
void MirrorOneProject(ReaProject* proj, MirrorProject& mp, bool* timeline_changed)
{
    std::vector<MirrorItem>  items;
    std::vector<std::string> present;  // every item GUID in the project
    const int                n = CountMediaItems(proj);
    for (int i = 0; i < n; ++i) {
        MediaItem* it = GetMediaItem(proj, i);
        if (!it) continue;
        const std::string guid = ItemGuidOf(it);
        if (guid.empty()) continue;
        present.push_back(guid);
        MediaItem_Take* take = RavTakeOf(it);
        if (!take) continue;
        const std::string raw = ReadRecordRaw(take);
        if (raw.empty()) continue;
        MirrorItem mi;
        mi.item = it;
        mi.guid = guid;
        mi.seen.raw_hash = std::hash<std::string>{}(raw);
        mi.seen.map_ok = MirrorClipMap(it, take, &mi.seen.map);
        const auto prev = mp.items.find(guid);
        mi.first = prev == mp.items.end();
        mi.map_changed = !mi.first && mi.seen.map_ok &&
                         (!prev->second.map_ok || !SameMap(prev->second.map, mi.seen.map));
        if (!mi.first && !mi.map_changed && prev->second.raw_hash == mi.seen.raw_hash &&
            prev->second.map_ok == mi.seen.map_ok) {
            mi.seen = prev->second;  // nothing changed on this item: nothing to parse
        } else {
            mi.fresh = true;
            if (!ParseItemRules(raw, &mi.rules)) continue;
            mi.seen.managed = MirrorManaged(mi.rules);
            mi.seen.owner = RecordOwner(mi.rules);
            mi.seen.guids = RefGuids(mi.rules);
        }
        items.push_back(std::move(mi));
    }

    // Ownership (event_list.h ArbitrateMirrorOwnership): an item that owns its record keeps its
    // markers; a copy gets fresh ones; a record without owner adopts its item unless another
    // item holds its markers.
    {
        std::vector<MirrorOwnerInput> in;
        in.reserve(items.size());
        for (const MirrorItem& mi : items) in.push_back({mi.guid, mi.seen.owner, mi.seen.managed, mi.seen.guids});
        const MirrorOwnerResult res = ArbitrateMirrorOwnership(in, mp.owners, present);
        for (size_t i = 0; i < items.size(); ++i) items[i].own = res.own[i];
    }

    // Each item: its markers placed, hidden, shown; a copy's made fresh; the record written
    // (no undo point) when it changed.
    for (MirrorItem& mi : items) {
        if (!mi.seen.managed) continue;
        const bool copy = mi.own == MirrorOwnership::Copy;
        mi.holds = !copy;  // a copy holds markers only once its fresh ones are recorded
        if (!mi.fresh && !copy) continue;  // unchanged since the last scan
        MediaItem_Take* take = RavTakeOf(mi.item);
        if (!take) continue;
        if (!mi.fresh && !ParseItemRules(ReadRecordRaw(take), &mi.rules)) continue;  // a copy decided from the cache
        if (!mi.seen.map_ok) continue;
        bool                     changed = false;
        std::vector<std::string> created;
        // First sight (project opened, item restored by an undo): nothing is moved or hidden; only
        // the markers the mirror itself deleted come back, and hidden refs whose event lies
        // inside the item are shown.
        const bool moved = mi.map_changed;
        changed |= MirrorRefs(mi.rules.pmarkers, mi.seen.map, moved, copy, mp, proj, timeline_changed, created);
        changed |= MirrorRefs(mi.rules.ppmarkers, mi.seen.map, moved, copy, mp, proj, timeline_changed, created);
        if (mi.own != MirrorOwnership::Own && RecordOwner(mi.rules) != mi.guid) changed |= SetRecordOwner(mi.rules, mi.guid);
        if (changed) {
            // Written straight to the take, no undo point (ModifyItemRulesNoUndo checks the take
            // against the CURRENT project only, and the mirror works on every tab).
            const std::string text = SerializeItemRules(mi.rules);
            const bool ok = text.size() < kItemRulesMaxBytes &&
                            GetSetMediaItemTakeInfo_String(take, kItemRulesKey, const_cast<char*>(text.c_str()), true);
            if (!ok) {
                // Not recorded: the markers made now would be made again on every scan.
                for (const std::string& g : created) DeleteProjectMarkerByGuid(g, proj);
                mi.seen.raw_hash = 0;  // tried again on the next scan
                if (copy) mi.seen.guids.clear();  // the original's markers stay the original's
                continue;
            }
            mi.seen.raw_hash = std::hash<std::string>{}(ReadRecordRaw(take));
        }
        if (copy) mi.holds = changed;  // recorded with its own markers and owner
        mi.seen.owner = RecordOwner(mi.rules);
        mi.seen.guids = RefGuids(mi.rules);
    }

    // The markers of a deleted item (or that its record no longer lists) go. Only items holding
    // their markers claim them (a copy that could not refresh still lists the original's).
    std::vector<MirrorHeld>  held;
    std::vector<std::string> read_items;
    for (const MirrorItem& mi : items) {
        read_items.push_back(mi.guid);
        held.push_back({mi.guid, mi.seen.managed, mi.holds, mi.seen.guids});
    }
    std::vector<std::pair<std::string, std::string>> prev(mp.owners.begin(), mp.owners.end());
    for (const std::string& g : MirrorOrphans(prev, MirrorClaimedNow(held), present, read_items)) {
        if (DeleteProjectMarkerByGuid(g, proj)) {
            mp.deleted.insert(g);
            *timeline_changed = true;
        }
    }

    // What the next scan compares with.
    std::map<std::string, MirrorItemSeen> seen;
    for (const MirrorItem& mi : items) seen[mi.guid] = mi.seen;
    mp.owners = NextMirrorOwners(held, mp.owners, present, read_items);
    mp.items = std::move(seen);
    if (mp.deleted.size() > 100000) mp.deleted.clear();  // bounded
}

}  // namespace

void InitTagMarkers(void* (*get_func)(const char* name))
{
    SetItemRulesWrittenHook(&OnItemRulesWritten);  // 10-4 fb-4: every gesture rewrites the previews
    if (!get_func) return;
    f_num_take_markers = reinterpret_cast<GetNumTakeMarkersFn>(get_func("GetNumTakeMarkers"));
    f_get_take_marker = reinterpret_cast<GetTakeMarkerFn>(get_func("GetTakeMarker"));
    f_set_take_marker = reinterpret_cast<SetTakeMarkerFn>(get_func("SetTakeMarker"));
    f_delete_take_marker = reinterpret_cast<DeleteTakeMarkerFn>(get_func("DeleteTakeMarker"));
    f_add_project_marker2 = reinterpret_cast<AddProjectMarker2Fn>(get_func("AddProjectMarker2"));
    f_add_region_or_marker = reinterpret_cast<AddRegionOrMarkerFn>(get_func("AddRegionOrMarker"));
    f_delete_project_marker = reinterpret_cast<DeleteProjectMarkerByIndexFn>(get_func("DeleteProjectMarkerByIndex"));
    f_enum_project_markers3 = reinterpret_cast<EnumProjectMarkers3Fn>(get_func("EnumProjectMarkers3"));
    f_get_region_or_marker = reinterpret_cast<GetRegionOrMarkerFn>(get_func("GetRegionOrMarker"));
    f_marker_value = reinterpret_cast<GetRegionOrMarkerInfo_ValueFn>(get_func("GetRegionOrMarkerInfo_Value"));
    f_marker_string = reinterpret_cast<GetSetRegionOrMarkerInfo_StringFn>(get_func("GetSetRegionOrMarkerInfo_String"));
    f_num_regions_or_markers = reinterpret_cast<GetNumRegionsOrMarkersFn>(get_func("GetNumRegionsOrMarkers"));
    f_color_to_native = reinterpret_cast<ColorToNativeFn>(get_func("ColorToNative"));
    f_set_marker_value = reinterpret_cast<SetRegionOrMarkerInfo_ValueFn>(get_func("SetRegionOrMarkerInfo_Value"));
    f_set_marker_by_index2 = reinterpret_cast<SetProjectMarkerByIndex2Fn>(get_func("SetProjectMarkerByIndex2"));
}

MarkerMode GetTaggingMarkerMode()
{
    if (g_mode < 0) {
        const float v = LoadPrefFloat(kPrefMarkers, 2.0f);
        const int   m = static_cast<int>(std::lround(v));
        g_mode = (m >= 0 && m <= 2) ? m : 2;
    }
    return g_mode == 0 ? MarkerMode::Take : g_mode == 1 ? MarkerMode::Project : MarkerMode::Both;
}

void SetTaggingMarkerMode(MarkerMode mode)
{
    const MarkerMode before = GetTaggingMarkerMode();
    g_mode = mode == MarkerMode::Take ? 0 : mode == MarkerMode::Project ? 1 : 2;
    SavePrefFloat(kPrefMarkers, static_cast<float>(g_mode));
    if (mode == before) return;
    // 10-4 fb-4: the previews follow the option. Every project item with previews or a commit
    // gets its previews rewritten for it, in one undo point (only when there is such an item).
    try {
        std::vector<MediaItem*> items;
        const int               n = CountMediaItems(nullptr);
        for (int i = 0; i < n; ++i) {
            MediaItem*    it = GetMediaItem(nullptr, i);
            ItemRulesRead rd;
            if (!it || !ReadItemRules(it, &rd) || !rd.present || !rd.valid) continue;
            if (HasPreviews(rd.rules) || rd.rules.has_applied) items.push_back(it);
        }
        if (items.empty()) return;
        Undo_BeginBlock2(nullptr);
        for (MediaItem* it : items) {
            try {
                RewriteItemPreviewsNoUndo(it);
            } catch (...) {
            }
        }
        Undo_EndBlock2(nullptr, kUndoOption, UNDO_STATE_ALL);
        UpdateArrange();
        TaggingReread();
    } catch (...) {
    }
}

const char* MarkerModeLine(MarkerMode mode)
{
    switch (mode) {
    case MarkerMode::Take: return "Writes take markers";
    case MarkerMode::Project: return "Writes project markers";
    case MarkerMode::Both: return "Writes take + project markers";
    }
    return "";
}

ApplyResult CommitTaggingMarkers()
{
    ApplyResult res;
    try {
        const MarkerMode mode = GetTaggingMarkerMode();
        if (const char* why = MissingApiReason(mode); why[0]) {
            res.error = std::string(why) + ": nothing was written.";
            g_last = res;
            return res;
        }

        // Each selected item: its detection, or why it is skipped.
        struct Run {
            MediaItem*    item = nullptr;
            ItemDetection det;
        };
        std::vector<Run> runs;
        const int        n = CountSelectedMediaItems(nullptr);
        for (int i = 0; i < n; ++i) {
            MediaItem* it = GetSelectedMediaItem(nullptr, i);
            Run        r;
            r.item = it;
            r.det = DetectItem(it);
            switch (r.det.status) {
            case ItemDetection::Status::Ok: runs.push_back(std::move(r)); break;
            case ItemDetection::Status::NotRav:
            case ItemDetection::Status::NoRules: ++res.without_rules; break;
            case ItemDetection::Status::NoFile:
            case ItemDetection::Status::RolesMissing: ++res.roles_skipped; break;
            case ItemDetection::Status::Failed: ++res.failed; break;
            }
        }
        if (runs.empty()) {
            g_last = res;
            return res;
        }

        Undo_BeginBlock2(nullptr);
        try {
            // First every item's previous RAV markers (committed and previews) go, so no item
            // takes another's old RAV marker for a foreign one.
            for (Run& r : runs) {
                try {
                    DeleteItemOwnMarkers(r.item, r.det.rules);
                } catch (...) {
                }
            }
            std::vector<ExistingMarker> written_project;  // this Commit's project markers (RAV's, never foreign)
            for (Run& r : runs) CommitItemMarkers(r.item, r.det, mode, written_project, res);
        } catch (...) {
            Undo_EndBlock2(nullptr, kUndoCommit, UNDO_STATE_ALL);
            throw;
        }
        Undo_EndBlock2(nullptr, kUndoCommit, UNDO_STATE_ALL);
        UpdateArrange();
        res.ran = res.items > 0;
        TaggingReread();  // the footer: "markers up to date"
    } catch (const std::exception& e) {
        res.error = std::string("Commit failed: ") + e.what();
    } catch (...) {
        res.error = "Commit failed.";
    }
    g_last = res;
    return res;
}

ApplyResult CancelTaggingChanges()
{
    ApplyResult res;
    res.cancel = true;
    try {
        // Each selected item with rules: what Cancel writes back, when anything changes.
        struct Run {
            MediaItem* item = nullptr;
            ItemRules  cur;
            ItemRules  next;
        };
        std::vector<Run> runs;
        const int        n = CountSelectedMediaItems(nullptr);
        for (int i = 0; i < n; ++i) {
            MediaItem*    it = GetSelectedMediaItem(nullptr, i);
            ItemRulesRead rd;
            if (!it || !ReadItemRules(it, &rd) || !rd.present || !rd.valid) {
                ++res.without_rules;
                continue;
            }
            // Cancellable: rules, a snapshot or previews (a commit then every rule deleted too).
            std::string snap_text;
            const bool  has_snap = ReadCommittedSnapshot(it, &snap_text);
            if (rd.rules.blocks.empty() && !has_snap && !HasPreviews(rd.rules)) {
                ++res.without_rules;
                continue;
            }
            Run r;
            r.item = it;
            r.cur = rd.rules;
            if (!CancelTarget(rd.rules, rd.raw, has_snap ? &snap_text : nullptr, &r.next)) continue;  // nothing to cancel
            runs.push_back(std::move(r));
        }
        if (runs.empty()) {
            g_last = res;
            return res;
        }

        Undo_BeginBlock2(nullptr);
        try {
            for (Run& r : runs) {
                try {
                    DeleteItemPreviewMarkers(r.item, r.cur);
                    // Previews that could not be deleted (marker API missing) stay RAV's.
                    if (!TakeApiReady()) r.next.ptmarkers = r.cur.ptmarkers;
                    if (!ProjectApiReady()) r.next.ppmarkers = r.cur.ppmarkers;
                    std::string err;
                    const bool  ok = ModifyItemRulesNoUndo(
                        r.item,
                        [&](ItemRules& rec) {
                            rec = r.next;
                            return true;
                        },
                        &err);
                    if (ok) ++res.items;
                    else ++res.failed;
                } catch (...) {
                    ++res.failed;
                }
            }
        } catch (...) {
            Undo_EndBlock2(nullptr, kUndoCancel, UNDO_STATE_ALL);
            throw;
        }
        Undo_EndBlock2(nullptr, kUndoCancel, UNDO_STATE_ALL);
        UpdateArrange();
        res.ran = res.items > 0;
        TaggingReread();  // the inspector: the committed thresholds and events
    } catch (const std::exception& e) {
        res.error = std::string("Cancel failed: ") + e.what();
    } catch (...) {
        res.error = "Cancel failed.";
    }
    g_last = res;
    return res;
}

void MarkerMirrorTick()
{
    try {
        if (!MirrorApiReady()) return;
        // Which projects are open, and which of them changed since the mirror last ran.
        std::vector<std::pair<ReaProject*, int>> counts;
        for (int p = 0;; ++p) {
            char        file[4096] = {};
            ReaProject* proj = EnumProjects(p, file, sizeof(file));
            if (!proj) break;
            file[sizeof(file) - 1] = '\0';
            counts.emplace_back(proj, GetProjectStateChangeCount(proj));
            // File > Open in the same tab reuses the ReaProject*: what the mirror knew is stale.
            const auto it = g_mirror.find(proj);
            if (it != g_mirror.end() && it->second.file != file) g_mirror.erase(it);
            g_mirror[proj].file = file;
        }
        // Closed tabs: forgotten.
        for (auto it = g_mirror.begin(); it != g_mirror.end();) {
            bool open = false;
            for (const auto& c : counts)
                if (c.first == it->first) open = true;
            it = open ? std::next(it) : g_mirror.erase(it);
        }
        bool timeline_changed = false;
        for (const auto& c : counts) {
            MirrorProject& mp = g_mirror[c.first];
            if (mp.count == c.second) continue;  // nothing moved in this project: no work
            try {
                MirrorOneProject(c.first, mp, &timeline_changed);
            } catch (...) {
            }
            // The mirror's own writes moved the count: the next tick has nothing to do.
            mp.count = GetProjectStateChangeCount(c.first);
        }
        if (timeline_changed) UpdateTimeline();
    } catch (...) {
    }
}

bool StartMarkerMirror(int (*register_fn)(const char*, void*))
{
    if (g_mirror_on || !register_fn) return g_mirror_on;
    g_mirror_register = register_fn;
    g_mirror_on = register_fn("timer", reinterpret_cast<void*>(&MarkerMirrorTick)) != 0;
    return g_mirror_on;
}

void StopMarkerMirror()
{
    if (g_mirror_on && g_mirror_register) g_mirror_register("-timer", reinterpret_cast<void*>(&MarkerMirrorTick));
    g_mirror_on = false;
    g_mirror.clear();
}

const ApplyResult& LastApplyResult()
{
    return g_last;
}

void ClearLastApplyResult()
{
    g_last = ApplyResult{};
}

}  // namespace rav

#endif  // _WIN32
