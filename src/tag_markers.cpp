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
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "console_log.h"
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
        // 10-6: a pooled gesture reaches every copy, maybe a copy the mirror has not seen yet: its
        // record lists the original's project previews (another owner). Those are never deleted
        // here (the ref is dropped); its take previews are on its own take.
        const std::string rec_owner = RecordOwner(cur);
        if (!rec_owner.empty() && rec_owner != ItemGuidOf(item)) {
            ItemRules own = cur;
            own.ppmarkers.clear();
            DeleteItemPreviewMarkers(item, own);
        } else {
            DeleteItemPreviewMarkers(item, cur);
        }
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
    size_t                           raw_hash = 0;
    std::shared_ptr<const ItemRules> rules;  // the record parsed (reused while its text is the same)
    ItemClipMap                      map;
    bool                             map_ok = false;
    bool                             managed = false;
    bool                             take_refs = false;  // 10-4c: take refs whose edits are read
    std::string                      owner;   // the record's owner
    std::vector<std::string>         guids;   // the project-marker GUIDs its record lists
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
std::string ReadTakeKeyRaw(MediaItem_Take* take, const char* key)
{
    static std::vector<char> buf;
    if (buf.size() != kItemRulesMaxBytes + 1) buf.assign(kItemRulesMaxBytes + 1, '\0');
    buf[0] = '\0';
    if (!GetSetMediaItemTakeInfo_String(take, key, buf.data(), false)) return "";
    buf[kItemRulesMaxBytes] = '\0';
    return buf.data();
}

std::string ReadRecordRaw(MediaItem_Take* take)
{
    return ReadTakeKeyRaw(take, kItemRulesKey);
}

// Written straight to the take, no undo point (ModifyItemRulesNoUndo checks the take against the
// CURRENT project only, and the mirror works on every tab).
bool WriteTakeKeyRaw(MediaItem_Take* take, const char* key, const std::string& text)
{
    return text.size() < kItemRulesMaxBytes && GetSetMediaItemTakeInfo_String(take, key, const_cast<char*>(text.c_str()), true);
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

// 10-4c: a project marker as the timeline holds it now.
struct MarkerNow {
    double      t = 0.0;
    std::string name;
    bool        has_name = false;
};

// One enumeration of the project's markers (not regions), by GUID: one pass per scan instead of
// a lookup per ref.
std::map<std::string, MarkerNow> ProjectMarkersByGuid(ReaProject* proj)
{
    std::map<std::string, MarkerNow> out;
    std::vector<char>                name(4096, '\0');
    for (int i = 0; i < 1000000; ++i) {
        ProjectMarker* m = f_get_region_or_marker(proj, i, nullptr);
        if (!m) break;
        if (f_marker_value(proj, m, "B_ISREGION") != 0.0) continue;
        const std::string g = MarkerGuid(m, proj);
        if (g.empty()) continue;
        MarkerNow mn;
        mn.t = f_marker_value(proj, m, "D_STARTPOS");
        name[0] = '\0';
        if (f_marker_string(proj, m, "NAME", name.data(), false)) {
            name.back() = '\0';
            mn.name = name.data();
            mn.has_name = true;
        } else {
            // Fallback: the enumeration's name (unreadable: no rename is ever seen).
            const int idx = EnumIndexOf(m, proj);
            bool        rgn = false;
            double      p = 0.0, e = 0.0;
            const char* nm = nullptr;
            int         num = -1, col = 0;
            if (idx >= 0 && f_enum_project_markers3(proj, idx, &rgn, &p, &e, &nm, &num, &col) && nm) {
                mn.name = nm;
                mn.has_name = true;
            }
        }
        out[g] = std::move(mn);
    }
    return out;
}

std::vector<MirrorRefState> RefStates(const std::vector<ProjectMarkerRef>& refs,
                                      const std::map<std::string, MarkerNow>& now, const MirrorProject& mp)
{
    std::vector<MirrorRefState> state(refs.size());
    for (size_t i = 0; i < refs.size(); ++i) {
        if (refs[i].guid.empty()) continue;
        const auto f = now.find(refs[i].guid);
        state[i].exists = f != now.end();
        if (state[i].exists) {
            state[i].now_t = f->second.t;
            state[i].name = f->second.name;
            state[i].has_name = f->second.has_name;
        }
        state[i].restore = mp.deleted.count(refs[i].guid) > 0;
    }
    return state;
}

// One RAV item read on this scan.
struct MirrorItem {
    MediaItem*      item = nullptr;
    std::string     guid;
    MirrorItemSeen  seen;
    MirrorOwnership own = MirrorOwnership::Own;
    bool            holds = true;  // holds its markers' GUIDs after this scan (a copy: once refreshed)
    bool            map_changed = false;  // its clip map differs from the last scan's (false on first sight)
};

// One of the mirror's own steps (Move / Hide / Show / Retime) on its ref; true when the record
// changed.
bool ApplyMirrorStep(ProjectMarkerRef& r, const MirrorStep& st, bool copy, MirrorProject& mp, ReaProject* proj,
                     bool* timeline_changed, std::vector<std::string>& created)
{
    switch (st.action) {
    case MirrorAction::Move:
        if (MoveProjectMarker(r.guid, st.new_t, proj)) {
            r.t = st.new_t;
            *timeline_changed = true;
            return true;
        }
        return false;
    case MirrorAction::Hide:
        if (st.delete_old && DeleteProjectMarkerByGuid(r.guid, proj)) {
            mp.deleted.insert(r.guid);
            *timeline_changed = true;
        }
        r.guid.clear();
        return true;
    case MirrorAction::Show: {
        const std::string g = AddProjectMarker(st.new_t, r.has_name ? r.name : "", NativeColor(r.color), proj);
        if (copy && g.empty()) {
            r.guid.clear();  // never keep the original's GUID on a copy
            return true;
        }
        if (g.empty()) return false;
        created.push_back(g);
        r.guid = g;
        r.t = st.new_t;
        *timeline_changed = true;
        return true;
    }
    case MirrorAction::Retime:
        r.t = st.new_t;
        return true;
    default: return false;
    }
}

// The record written back after a scan changed it (no undo point); false (and the markers just
// created deleted again) when REAPER refused it.
bool StoreMirroredRecord(MirrorItem& mi, MediaItem_Take* take, const ItemRules& rules,
                         const std::vector<std::string>& created, ReaProject* proj)
{
    if (!WriteTakeKeyRaw(take, kItemRulesKey, SerializeItemRules(rules))) {
        // Not recorded: the markers made now would be made again on every scan.
        for (const std::string& g : created) DeleteProjectMarkerByGuid(g, proj);
        mi.seen.raw_hash = 0;  // tried again on the next scan
        return false;
    }
    return true;
}

// The cache after a write: the record as REAPER now holds it.
void RefreshSeen(MirrorItem& mi, MediaItem_Take* take)
{
    const std::string raw = ReadRecordRaw(take);
    auto              r = std::make_shared<ItemRules>();
    if (!raw.empty() && ParseItemRules(raw, r.get())) {
        mi.seen.raw_hash = std::hash<std::string>{}(raw);
        mi.seen.rules = r;
        mi.seen.managed = MirrorManaged(*r);
        mi.seen.take_refs = !RecordOwner(*r).empty() && (!r->tmarkers.empty() || !r->ptmarkers.empty());
    } else {
        mi.seen.raw_hash = 0;
    }
}

const char* EditWord(MarkerEditKind k)
{
    switch (k) {
    case MarkerEditKind::Drag: return "moved";
    case MarkerEditKind::Delete: return "deleted";
    case MarkerEditKind::Disown: return "given to the user";
    }
    return "";
}

// A copy (duplicate, paste, right part of a split): fresh markers of its own. True when its
// record now holds them.
bool MirrorCopyItem(MirrorItem& mi, MediaItem_Take* take, const std::map<std::string, MarkerNow>& now, MirrorProject& mp,
                    ReaProject* proj, bool* timeline_changed)
{
    ItemRules                rules = *mi.seen.rules;
    bool                     changed = false;
    std::vector<std::string> created;
    for (std::vector<ProjectMarkerRef>* refs : {&rules.pmarkers, &rules.ppmarkers})
        for (const MirrorStep& st : MirrorPlan(*refs, RefStates(*refs, now, mp), mi.seen.map, false, true))
            changed |= ApplyMirrorStep((*refs)[st.ref], st, true, mp, proj, timeline_changed, created);
    if (RecordOwner(rules) != mi.guid) changed |= SetRecordOwner(rules, mi.guid);
    if (!changed) return false;
    if (!StoreMirroredRecord(mi, take, rules, created, proj)) {
        mi.seen.guids.clear();  // the original's markers stay the original's
        return false;
    }
    RefreshSeen(mi, take);
    mi.seen.owner = RecordOwner(rules);
    mi.seen.guids = RefGuids(rules);
    return true;
}

// 10-6: the user's REAPER-side edits read on one item this scan (its event list changed), for the
// other copies of its pool.
struct MirrorEdit {
    MediaItem*                       item = nullptr;
    std::shared_ptr<const ItemRules> rules;  // its record as written with the edits
    CopyEdits                        edits;  // the edits read (event_list.h), its snapshot already took them
};

// An item that is not a copy: its markers placed, hidden, shown (10-4b), and the user's edits of
// them read into its event list (10-4c): the record, the last Commit (signature, Cancel snapshot)
// and the previews updated, no undo point (it rides the edit's own). 10-6: an event-list change is
// reported in `edit_out` (its pool follows, PropagateMirrorEdits).
void MirrorOwnItem(MirrorItem& mi, MediaItem_Take* take, const std::map<std::string, MarkerNow>& now, MirrorProject& mp,
                   ReaProject* proj, bool* timeline_changed, MirrorEdit* edit_out)
{
    const ItemRules&   base = *mi.seen.rules;
    const ItemClipMap& map = mi.seen.map;
    const std::vector<MirrorStep> ps = MirrorPlan(base.pmarkers, RefStates(base.pmarkers, now, mp), map, mi.map_changed, false);
    const std::vector<MirrorStep> pps = MirrorPlan(base.ppmarkers, RefStates(base.ppmarkers, now, mp), map, mi.map_changed, false);
    // Take refs: no GUID, matched by time and name, then by elimination (MatchTakeRefs).
    const size_t              nt = base.tmarkers.size();
    std::vector<TakeRefMatch> tms;
    std::vector<ExistingMarker> tnow;
    if (mi.seen.take_refs && TakeApiReady()) {
        tnow = TakeMarkers(take);
        std::vector<TakeMarkerRef> all = base.tmarkers;
        all.insert(all.end(), base.ptmarkers.begin(), base.ptmarkers.end());
        tms = MatchTakeRefs(all, tnow, kOwnTakeTolS);
    }
    const bool adopt = mi.own == MirrorOwnership::Adopt && RecordOwner(base) != mi.guid;
    bool       any = adopt;
    for (const MirrorStep& st : ps) any |= st.action != MirrorAction::Keep;
    for (const MirrorStep& st : pps) any |= st.action != MirrorAction::Keep;
    for (const TakeRefMatch& m : tms) any |= m.fate != TakeRefFate::InPlace;
    if (!any) return;  // the usual case: everything where it belongs

    ItemRules                rules = base;
    bool                     changed = false;
    std::vector<std::string> created;
    std::vector<MarkerEdit>  committed, previews;
    std::vector<std::string> what;  // per edit (committed then previews): the log's "project" / "take"
    std::vector<std::string> what_prev;
    std::vector<char>        erase_p(rules.pmarkers.size(), 0), erase_pp(rules.ppmarkers.size(), 0);
    std::vector<char>        erase_t(rules.tmarkers.size(), 0), erase_pt(rules.ptmarkers.size(), 0);
    std::vector<char>        twin_done_t(rules.tmarkers.size(), 0), twin_done_p(rules.pmarkers.size(), 0);

    auto edit_of = [](MarkerEditKind k, double c, double new_c, const std::string& name, uint32_t color, bool has_color,
                      bool preview) {
        MarkerEdit e;
        e.kind = k;
        e.c = c;
        e.new_c = new_c;
        e.name = name;
        e.color = color;
        e.has_color = has_color;
        e.preview = preview;
        return e;
    };
    auto kind_of = [](MirrorAction a) {
        return a == MirrorAction::UserDrag ? MarkerEditKind::Drag
             : a == MirrorAction::UserDelete ? MarkerEditKind::Delete : MarkerEditKind::Disown;
    };
    // Both: the committed take marker of an event, and its project marker.
    // Skipped: a ref dropped or already handled this scan.
    auto either = [](const std::vector<char>& a, const std::vector<char>& b) {
        std::vector<char> out(a.size(), 0);
        for (size_t i = 0; i < a.size(); ++i) out[i] = (a[i] || (i < b.size() && b[i])) ? 1 : 0;
        return out;
    };
    auto take_twin = [&](double c, const std::string& name) {
        return FindTakeTwin(rules.tmarkers, c, name, kOwnTakeTolS, either(erase_t, twin_done_t));
    };
    auto project_twin = [&](double c, const std::string& name) {
        return FindProjectTwin(rules.pmarkers, c, name, kOwnTakeTolS, either(erase_p, twin_done_p));
    };
    auto set_take_marker_time = [&](double from, const std::string& name, double to) {
        const std::vector<ExistingMarker> cur = TakeMarkers(take);
        const int                         i = FindTwinMarker(cur, from, name, kOwnTakeTolS);
        if (i < 0) return;
        double pos = to;
        f_set_take_marker(take, i, name.c_str(), &pos, nullptr);
        *timeline_changed = true;
    };
    auto delete_take_marker = [&](double at, const std::string& name) {
        const std::vector<ExistingMarker> cur = TakeMarkers(take);
        const int                         i = FindTwinMarker(cur, at, name, kOwnTakeTolS);
        if (i >= 0 && f_delete_take_marker(take, i)) *timeline_changed = true;
    };
    std::vector<std::pair<std::string, double>> edited_events;  // (name, clip time) edited through a project marker

    // An edit whose rule is not found (the rule renamed or deleted since): its marker is only given
    // to the user (its ref dropped), no twin and no event changes.
    auto no_rule = [&](const MarkerEdit& e, const char* kind) {
        if (MarkerEditBlock(base, e) >= 0) return false;
        LogInfo("Auto-tagging: %s marker \"%s\" at %.3f s (clip time) edited in REAPER: no rule matches, "
                "the marker is the user's now, event list unchanged",
                kind, e.name.c_str(), e.c);
        changed = true;
        return true;
    };

    // Committed project markers.
    for (const MirrorStep& st : ps) {
        ProjectMarkerRef& r = rules.pmarkers[st.ref];
        switch (st.action) {
        case MirrorAction::Keep: break;
        case MirrorAction::UserDrag:
        case MirrorAction::UserDelete:
        case MirrorAction::UserDisown: {
            const MarkerEditKind k = kind_of(st.action);
            const MarkerEdit     e = edit_of(k, r.c, st.new_c, r.name, r.color, r.has_color, false);
            if (no_rule(e, "project")) {
                erase_p[st.ref] = twin_done_p[st.ref] = 1;
                break;
            }
            committed.push_back(e);
            what.push_back("project");
            edited_events.push_back({r.name, r.c});
            const int tw = TakeApiReady() ? take_twin(r.c, r.name) : -1;  // Both: the take marker follows
            if (k == MarkerEditKind::Drag) {
                if (tw >= 0) {
                    set_take_marker_time(rules.tmarkers[static_cast<size_t>(tw)].t, r.name, st.new_c);
                    rules.tmarkers[static_cast<size_t>(tw)].t = st.new_c;
                    twin_done_t[static_cast<size_t>(tw)] = 1;
                }
                r.c = st.new_c;
                r.t = st.new_t;
            } else {
                if (tw >= 0) {
                    delete_take_marker(rules.tmarkers[static_cast<size_t>(tw)].t, r.name);
                    erase_t[static_cast<size_t>(tw)] = 1;
                }
                erase_p[st.ref] = 1;  // deleted, or the user's now
            }
            twin_done_p[st.ref] = 1;
            changed = true;
            break;
        }
        default: changed |= ApplyMirrorStep(r, st, false, mp, proj, timeline_changed, created); break;
        }
    }
    // Preview project markers: their edits change the events only (the previews are rewritten).
    for (const MirrorStep& st : pps) {
        ProjectMarkerRef& r = rules.ppmarkers[st.ref];
        switch (st.action) {
        case MirrorAction::Keep: break;
        case MirrorAction::UserDrag:
        case MirrorAction::UserDelete:
        case MirrorAction::UserDisown: {
            const MarkerEditKind k = kind_of(st.action);
            const MarkerEdit     e = edit_of(k, r.c, st.new_c, r.name, r.color, r.has_color, true);
            if (no_rule(e, "project preview")) {
                erase_pp[st.ref] = 1;
                break;
            }
            previews.push_back(e);
            what_prev.push_back("project preview");
            if (k == MarkerEditKind::Drag) {
                r.c = st.new_c;  // still RAV's: the rewrite replaces it
                r.t = st.new_t;
            } else {
                erase_pp[st.ref] = 1;
            }
            changed = true;
            break;
        }
        default: changed |= ApplyMirrorStep(r, st, false, mp, proj, timeline_changed, created); break;
        }
    }
    // Take markers (committed, then previews).
    for (size_t k = 0; k < tms.size(); ++k) {
        const TakeRefMatch& m = tms[k];
        if (m.fate == TakeRefFate::InPlace) continue;
        const bool prev = k >= nt;
        const size_t i = prev ? k - nt : k;
        if (!prev && (erase_t[i] || twin_done_t[i])) continue;  // its project marker's edit handled it
        TakeMarkerRef& r = prev ? rules.ptmarkers[i] : rules.tmarkers[i];
        bool skip = false;
        if (!prev)
            for (const auto& ev : edited_events)
                if (ev.first == r.name && std::fabs(ev.second - r.t) <= kOwnTakeTolS) skip = true;
        if (skip) continue;
        MarkerEditKind kind = MarkerEditKind::Delete;
        double         to = 0.0, pt = 0.0;
        if (m.fate == TakeRefFate::Drag && m.marker >= 0) {
            to = tnow[static_cast<size_t>(m.marker)].t;
            // Dragged outside the item: the user's, like a rename.
            kind = FirstPassProjectTime(map, to, &pt) ? MarkerEditKind::Drag : MarkerEditKind::Disown;
        } else if (m.fate == TakeRefFate::Rename) {
            kind = MarkerEditKind::Disown;
        }
        const MarkerEdit e = edit_of(kind, r.t, to, r.name, 0, false, prev);
        if (no_rule(e, prev ? "take preview" : "take")) {
            (prev ? erase_pt : erase_t)[i] = 1;
            continue;
        }
        if (prev) {
            previews.push_back(e);
            what_prev.push_back("take preview");
            if (kind == MarkerEditKind::Drag) r.t = to;  // the rewrite deletes it there
            else erase_pt[i] = 1;
            changed = true;
            continue;
        }
        committed.push_back(e);
        what.push_back("take");
        const int pw = project_twin(r.t, r.name);  // Both: the project marker follows
        if (kind == MarkerEditKind::Drag) {
            if (pw >= 0) {
                ProjectMarkerRef& p = rules.pmarkers[static_cast<size_t>(pw)];
                if (!p.guid.empty() && MoveProjectMarker(p.guid, pt, proj)) *timeline_changed = true;
                p.c = to;
                p.t = pt;
                twin_done_p[static_cast<size_t>(pw)] = 1;
            }
            r.t = to;
        } else {
            if (pw >= 0) {
                ProjectMarkerRef& p = rules.pmarkers[static_cast<size_t>(pw)];
                if (!p.guid.empty() && DeleteProjectMarkerByGuid(p.guid, proj)) *timeline_changed = true;
                erase_p[static_cast<size_t>(pw)] = 1;
            }
            erase_t[i] = 1;
        }
        changed = true;
    }
    if (adopt) changed |= SetRecordOwner(rules, mi.guid);
    if (!changed) return;

    // Refs of markers deleted by the user or given to them: no longer RAV's.
    auto compact = [](auto& v, const std::vector<char>& erase) {
        size_t w = 0;
        for (size_t r = 0; r < v.size(); ++r)
            if (!erase[r]) v[w++] = std::move(v[r]);
        v.resize(w);
    };
    compact(rules.pmarkers, erase_p);
    compact(rules.ppmarkers, erase_pp);
    compact(rules.tmarkers, erase_t);
    compact(rules.ptmarkers, erase_pt);

    // The event list (and, when the item was up to date, the commit's signature).
    std::string new_sig;
    ItemDetection det;
    const bool edits = !committed.empty() || !previews.empty();
    std::vector<int> blocks;
    if (edits) {
        det = DetectItem(mi.item);  // the record not written yet: the rules as they were
        const bool det_ok = det.status == ItemDetection::Status::Ok;
        const MarkerMode mode = GetTaggingMarkerMode();
        bool up_to_date = false;
        if (det_ok && !committed.empty()) {
            const std::vector<ShownEvent> before = BuildEventList(det.events, det.rules.events, det.rules.blocks.size());
            up_to_date = MarkersUpToDate(det.rules, PlanMarkers(before, det.rules.blocks, det.map), mode);
        }
        std::vector<MarkerEdit> all = committed;
        all.insert(all.end(), previews.begin(), previews.end());
        rules.events = ApplyMarkerEdits(rules, all, det_ok ? &det.events : nullptr, &blocks).events;
        if (up_to_date) {
            const std::vector<ShownEvent> after = BuildEventList(det.events, rules.events, rules.blocks.size());
            new_sig = MarkerSignature(PlanMarkers(after, rules.blocks, det.map), mode);
            rules.applied.sig = new_sig;
        }
    }
    if (!StoreMirroredRecord(mi, take, rules, created, proj)) {
        LogWarn("Auto-tagging: a marker edit could not be saved to the item's rules");
        return;
    }
    if (!committed.empty()) {
        // The last Commit takes the edit too, so Cancel does not undo it.
        ItemRules         snap;
        const std::string snap_raw = ReadTakeKeyRaw(take, kItemRulesCommittedKey);
        if (!snap_raw.empty() && !ParseItemRules(snap_raw, &snap)) {
            LogWarn("Auto-tagging: a marker edit could not be applied to the last Commit (unreadable); Cancel would undo it");
        } else if (!snap_raw.empty()) {
            const bool det_ok = det.status == ItemDetection::Status::Ok;
            snap.events = ApplyMarkerEdits(snap, committed, det_ok ? &det.events : nullptr).events;
            snap.pmarkers = rules.pmarkers;
            snap.tmarkers = rules.tmarkers;
            if (!new_sig.empty() && snap.has_applied) snap.applied.sig = new_sig;
            if (!WriteTakeKeyRaw(take, kItemRulesCommittedKey, SerializeItemRules(snap)))
                LogWarn("Auto-tagging: a marker edit could not be saved to the last Commit; Cancel would undo it");
        }
    }
    // The previews show the event list as it is now (on the current tab: the preview writer works
    // there only).
    if (edits && EnumProjects(-1, nullptr, 0) == proj) RewriteItemPreviewsNoUndo(mi.item);
    // 10-6: the event list changed: the other copies of its pool follow (after the scan).
    if (edits && edit_out) {
        edit_out->item = mi.item;
        edit_out->rules = std::make_shared<const ItemRules>(rules);
        edit_out->edits.all = committed;
        edit_out->edits.all.insert(edit_out->edits.all.end(), previews.begin(), previews.end());
        edit_out->edits.committed = committed;
        edit_out->edits.det_ok = det.status == ItemDetection::Status::Ok;
        if (edit_out->edits.det_ok) edit_out->edits.detections = det.events;
    }
    for (size_t k = 0; k < committed.size() + previews.size(); ++k) {
        const bool        pv = k >= committed.size();
        const MarkerEdit& e = pv ? previews[k - committed.size()] : committed[k];
        const std::string& w = pv ? what_prev[k - committed.size()] : what[k];
        const int          b = k < blocks.size() ? blocks[k] : -1;
        if (e.kind == MarkerEditKind::Drag)
            LogInfo("Auto-tagging: %s marker \"%s\" at %.3f s %s in REAPER to %.3f s (clip time)%s", w.c_str(),
                    e.name.c_str(), e.c, EditWord(e.kind), e.new_c, b < 0 ? ": event list unchanged (no matching rule, or the same event already edited)" : "");
        else
            LogInfo("Auto-tagging: %s marker \"%s\" at %.3f s %s in REAPER (clip time)%s", w.c_str(), e.name.c_str(), e.c,
                    EditWord(e.kind), b < 0 ? ": event list unchanged (no matching rule, or the same event already edited)" : "");
    }
    RefreshSeen(mi, take);
    mi.seen.owner = RecordOwner(rules);
    mi.seen.guids = RefGuids(rules);
}

// ---- 10-6: the copies of a pool follow a REAPER-side edit -----------------------------------------

// One copy of an edited item's pool takes the pool's content now (`content`: the edited record's),
// written without an undo point (it rides the edit's own, as the edit itself). A copy whose
// markers were up to date has its committed markers re-placed from the new events (like a Commit,
// on its own position, trim and rate) and stays up to date; any other has its previews rewritten.
// Its Cancel snapshot takes the committed edits (Cancel does not undo a marker edit). Only on the
// current tab the markers are touched (the marker writers work there); another tab gets the
// content only. `snap_edits`: the committed edits its Cancel snapshot has not taken yet (never
// the ones read on itself: MirrorOwnItem applied those). The record and the snapshot are
// event_list.h's FollowerRecord / FollowerSnapshot (host-tested). False when nothing was written.
bool FollowPoolEdit(MediaItem* item, const ItemRules& content, const std::vector<MarkerEdit>& snap_edits,
                    const std::vector<Event>* detections, ReaProject* proj, MirrorProject& mp, bool* timeline_changed)
{
    MediaItem_Take* take = RavTakeOf(item);
    if (!take) return false;
    const std::string raw = ReadRecordRaw(take);
    ItemRules         cur;
    if (!raw.empty() && !ParseItemRules(raw, &cur)) return false;  // a record that does not read: left as written
    if (PoolContentEqual(cur, content)) return false;              // already the pool's (the edited item itself)
    const bool       current = EnumProjects(-1, nullptr, 0) == proj;
    const MarkerMode mode = GetTaggingMarkerMode();
    // Whose markers its record lists: its own (re-placed when up to date), none yet (an older
    // record: previews only), or another item's project markers (a copy the mirror has not given
    // markers of its own yet: none of them is touched, the next scan does that). A copy listing no
    // project-marker GUID (Take mode: its take markers are on its own take) is its own.
    const std::string owner = RecordOwner(cur);
    const std::string self_guid = ItemGuidOf(item);
    const bool        lists_guids = !RefGuids(cur).empty();
    const bool        mine = !owner.empty() && (owner == self_guid || !lists_guids);
    const bool        foreign = !owner.empty() && owner != self_guid && lists_guids;

    // Up to date before the edit: its own result is what its last Commit wrote.
    ItemDetection det;
    bool          up_to_date = false;
    if (current && mine && cur.has_applied) {
        det = DetectItem(item);
        if (det.status == ItemDetection::Status::Ok) {
            const std::vector<ShownEvent> list = BuildEventList(det.events, det.rules.events, det.rules.blocks.size());
            up_to_date = MarkersUpToDate(det.rules, PlanMarkers(list, det.rules.blocks, det.map), mode);
        }
    }
    const bool replace = up_to_date && detections && !MissingApiReason(mode)[0];
    std::vector<TakeMarkerRef>    own_take;
    std::vector<ProjectMarkerRef> own_project;
    std::vector<PlannedMarker>    plan;
    std::string                   new_sig;
    if (replace) {
        // Its committed markers (and any preview) go, the new events' are written: the events stay
        // the pool's as they are (a Commit's record of the detections is not redone).
        for (const std::vector<ProjectMarkerRef>* refs : {&cur.pmarkers, &cur.ppmarkers})
            for (const ProjectMarkerRef& r : *refs)
                if (!r.guid.empty()) mp.deleted.insert(r.guid);  // the mirror's own deletes, never the user's
        DeleteItemOwnMarkers(item, cur);
        const std::vector<ShownEvent> list = BuildEventList(*detections, content.events, content.blocks.size());
        plan = PlanMarkers(list, content.blocks, det.map);
        std::vector<ExistingMarker> written;
        ApplyResult                 counts;
        WritePlan(take, plan, mode, written, own_take, own_project, counts);
        *timeline_changed = true;
    }
    const ItemRules next = FollowerRecord(cur, content, replace, own_take, own_project, !TakeApiReady(),
                                          !ProjectApiReady(), plan, mode, mine ? self_guid : std::string());
    if (replace) new_sig = next.applied.sig;
    if (!WriteTakeKeyRaw(take, kItemRulesKey, SerializeItemRules(next))) {
        // Not recorded: the markers just made are nobody's; they go again.
        if (replace) {
            if (TakeApiReady()) DeleteOwnTakeMarkers(take, own_take);
            for (const ProjectMarkerRef& p : own_project)
                if (!p.guid.empty()) DeleteProjectMarkerByGuid(p.guid, proj);
        }
        LogWarn("Auto-tagging: a linked item could not take a marker edit (its rules could not be saved)");
        return false;
    }
    // The last Commit takes the committed edits it has not taken yet, so Cancel does not undo them.
    if (!snap_edits.empty()) {
        ItemRules         snap;
        const std::string snap_raw = ReadTakeKeyRaw(take, kItemRulesCommittedKey);
        if (!snap_raw.empty() && ParseItemRules(snap_raw, &snap)) {
            const ItemRules out = FollowerSnapshot(snap, snap_edits, detections, next, new_sig);
            if (!WriteTakeKeyRaw(take, kItemRulesCommittedKey, SerializeItemRules(out)))
                LogWarn("Auto-tagging: a marker edit could not be saved to a linked item's last Commit");
        }
    }
    const bool previews = !replace && current && !foreign;
    if (previews) RewriteItemPreviewsNoUndo(item);
    const char* nm = GetTakeName(take);
    LogInfo("Auto-tagging: linked item \"%s\" follows the marker edit (%s)", nm ? nm : "",
            replace ? "markers re-placed, still up to date" : previews ? "previews rewritten" : "event list only");
    return true;
}

// After a scan: each pool with an item whose event list a REAPER-side edit changed shares the new
// list (and the rest of the content) with its other copies. Edits read on several copies of one
// pool in the same scan are all kept (applied in turn). The mirror's view of each copy written is
// refreshed, so this scan's bookkeeping and the next scan see its new markers as in place.
void PropagateMirrorEdits(ReaProject* proj, MirrorProject& mp, std::vector<MirrorItem>& items,
                          const std::vector<MirrorEdit>& edited, bool* timeline_changed)
{
    std::set<MediaItem*> done;
    for (size_t e = 0; e < edited.size(); ++e) {
        const MirrorEdit& src = edited[e];
        if (!src.rules || done.count(src.item)) continue;
        done.insert(src.item);
        std::vector<MediaItem*> pool = PoolMembersOf(src.item, proj);
        if (pool.empty()) continue;
        // The copies of this pool edited in this scan (src first), and the pool's content then.
        std::vector<MediaItem*> edited_items = {src.item};
        std::vector<CopyEdits>  pool_edits = {src.edits};
        for (size_t o = e + 1; o < edited.size(); ++o) {
            const MirrorEdit& other = edited[o];
            if (std::find(pool.begin(), pool.end(), other.item) == pool.end()) continue;
            edited_items.push_back(other.item);
            pool_edits.push_back(other.edits);
            done.insert(other.item);
        }
        const ItemRules content =
            MergePoolEdits(*src.rules, std::vector<CopyEdits>(pool_edits.begin() + 1, pool_edits.end()));
        pool.insert(pool.begin(), src.item);  // itself too, when another copy's edit was merged in
        const std::vector<Event>* det = src.edits.det_ok ? &src.edits.detections : nullptr;
        for (MediaItem* m : pool) {
            // Its snapshot takes the edits read on the other copies only (its own are in it already).
            const auto own = std::find(edited_items.begin(), edited_items.end(), m);
            const int  own_index = own == edited_items.end() ? -1 : static_cast<int>(own - edited_items.begin());
            try {
                if (!FollowPoolEdit(m, content, SnapshotEditsFor(pool_edits, own_index), det, proj, mp, timeline_changed))
                    continue;
            } catch (...) {
                continue;
            }
            for (MirrorItem& mi : items) {
                if (mi.item != m) continue;
                MediaItem_Take* take = RavTakeOf(m);
                if (!take) break;
                RefreshSeen(mi, take);
                if (mi.seen.rules) {
                    mi.seen.owner = RecordOwner(*mi.seen.rules);
                    mi.seen.guids = RefGuids(*mi.seen.rules);
                }
                // It holds the markers its record lists only when that record is its own.
                if (mi.seen.owner == mi.guid) mi.holds = true;
                break;
            }
        }
    }
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
        mi.map_changed = prev != mp.items.end() && prev->second.map_ok && mi.seen.map_ok &&
                         !SameMap(prev->second.map, mi.seen.map);
        if (prev != mp.items.end() && prev->second.rules && prev->second.raw_hash == mi.seen.raw_hash) {
            // The same record as on the last scan: nothing to parse.
            const ItemClipMap map = mi.seen.map;
            const bool        map_ok = mi.seen.map_ok;
            mi.seen = prev->second;
            mi.seen.map = map;
            mi.seen.map_ok = map_ok;
        } else {
            auto r = std::make_shared<ItemRules>();
            if (!ParseItemRules(raw, r.get())) continue;
            mi.seen.rules = r;
            mi.seen.managed = MirrorManaged(*r);
            mi.seen.owner = RecordOwner(*r);
            mi.seen.guids = RefGuids(*r);
            // 10-4c: take refs are read once the record names its owner (written by 10-4b or later).
            mi.seen.take_refs = !mi.seen.owner.empty() && (!r->tmarkers.empty() || !r->ptmarkers.empty());
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

    // Every managed item, every moved tick (10-4c: a marker edit leaves the item unchanged): its
    // markers checked against where they belong; a copy's made fresh. One marker enumeration.
    std::map<std::string, MarkerNow> now;
    bool                             enumerated = false;
    std::vector<MirrorEdit>          edited;  // 10-6: items whose event list an edit changed
    for (MirrorItem& mi : items) {
        const bool copy = mi.seen.managed && mi.own == MirrorOwnership::Copy;
        mi.holds = !copy;  // a copy holds markers only once its fresh ones are recorded
        if (!mi.seen.managed && !mi.seen.take_refs) continue;
        if (!mi.seen.map_ok || !mi.seen.rules) continue;
        MediaItem_Take* take = RavTakeOf(mi.item);
        if (!take) continue;
        if (!enumerated) {
            now = ProjectMarkersByGuid(proj);
            enumerated = true;
        }
        if (copy) {
            mi.holds = MirrorCopyItem(mi, take, now, mp, proj, timeline_changed);
        } else {
            MirrorEdit edit;
            MirrorOwnItem(mi, take, now, mp, proj, timeline_changed, &edit);
            if (edit.rules) edited.push_back(std::move(edit));
        }
    }
    // 10-6: the other copies of an edited item's pool follow, once every item read its own edits.
    if (!edited.empty()) PropagateMirrorEdits(proj, mp, items, edited, timeline_changed);

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

        // Each selected item and (10-6) every other copy of its pool, each once: its detection, or
        // why it is skipped.
        struct Run {
            MediaItem*    item = nullptr;
            ItemDetection det;
        };
        std::vector<Run>        runs;
        std::vector<MediaItem*> selected;
        const int               n = CountSelectedMediaItems(nullptr);
        for (int i = 0; i < n; ++i) selected.push_back(GetSelectedMediaItem(nullptr, i));
        const std::vector<MediaItem*> targets = WithPoolMembers(selected);
        // 10-6: a linked item with no record holds its pool's tagging: it gets it first, in this
        // Commit's undo point (opened here only when there is such an item), then is committed.
        std::vector<std::pair<MediaItem*, ItemRules>> adopt;
        for (MediaItem* it : targets) {
            ItemRules held;
            if (ItemPoolContent(it, &held)) adopt.push_back({it, std::move(held)});
        }
        bool in_block = false;
        if (!adopt.empty()) {
            Undo_BeginBlock2(nullptr);
            in_block = true;
            for (const auto& a : adopt) {
                try {
                    ModifyItemRulesNoUndo(
                        a.first,
                        [&](ItemRules& rec) {
                            CopyPoolContent(a.second, rec);
                            return true;
                        },
                        nullptr);
                } catch (...) {
                }
            }
        }
        try {
            for (MediaItem* it : targets) {
                Run r;
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
        } catch (...) {
            if (in_block) Undo_EndBlock2(nullptr, kUndoCommit, UNDO_STATE_ALL);  // the block is always closed
            throw;
        }
        if (runs.empty()) {
            if (in_block) {
                Undo_EndBlock2(nullptr, kUndoCommit, UNDO_STATE_ALL);
                TaggingReread();
            }
            g_last = res;
            return res;
        }

        if (!in_block) Undo_BeginBlock2(nullptr);
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
        // Each selected item with rules: what Cancel writes back, when anything changes. 10-6: per
        // pool, one copy runs it and its result's content goes to every other copy of the pool
        // (event_list.h PlanPoolCancel, host-tested).
        struct Run {
            MediaItem* item = nullptr;
            ItemRules  cur;
            ItemRules  next;
        };
        std::vector<Run>        runs;
        std::vector<MediaItem*> selected;
        const int               n = CountSelectedMediaItems(nullptr);
        for (int i = 0; i < n; ++i) selected.push_back(GetSelectedMediaItem(nullptr, i));
        std::vector<int>              pool_of;
        const std::vector<MediaItem*> all = WithPoolMembers(selected, nullptr, &pool_of);
        MediaItem* const              shown = GetTaggingModel().item;
        int                           pools = 0;
        for (int p : pool_of) pools = std::max(pools, p + 1);
        for (int p = 0; p < pools; ++p) {
            std::vector<MediaItem*> items;  // this pool's items: the selected ones first
            std::vector<CancelCopy> copies;
            for (size_t i = 0; i < all.size(); ++i) {
                if (pool_of[i] != p) continue;
                CancelCopy    c;
                ItemRulesRead rd;
                c.shown = all[i] == shown;
                c.selected = std::find(selected.begin(), selected.end(), all[i]) != selected.end();
                c.readable = ReadItemRules(all[i], &rd) && rd.present && rd.valid;
                if (c.readable) {
                    c.rules = std::move(rd.rules);
                    c.raw = std::move(rd.raw);
                    c.has_snap = ReadCommittedSnapshot(all[i], &c.snap_text);
                }
                items.push_back(all[i]);
                copies.push_back(std::move(c));
            }
            PoolCancelPlan plan = PlanPoolCancel(copies);
            res.without_rules += plan.without_rules;
            for (CancelWrite& w : plan.writes) runs.push_back({items[w.copy], copies[w.copy].rules, std::move(w.next)});
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
