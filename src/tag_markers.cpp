// SPDX-License-Identifier: MIT
//
// See tag_markers.h.

#include "tag_markers.h"

#ifdef _WIN32

#include <cmath>
#include <exception>
#include <string>
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

std::string MarkerGuid(ProjectMarker* m)
{
    if (!m) return "";
    char buf[256] = {};
    if (!f_marker_string(nullptr, m, "GUID", buf, false)) return "";
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

// The enumeration index (EnumProjectMarkers3 / DeleteProjectMarkerByIndex) of a project marker
// (not a region), or -1.
int EnumIndexOf(ProjectMarker* m)
{
    if (!m || f_marker_value(nullptr, m, "B_ISREGION") != 0.0) return -1;
    const double pos = f_marker_value(nullptr, m, "D_STARTPOS");
    const int    num = static_cast<int>(f_marker_value(nullptr, m, "I_NUMBER"));
    auto is_it = [&](int idx) {
        bool        rgn = false;
        double      p = 0.0, e = 0.0;
        const char* nm = nullptr;
        int         n = -1, col = 0;
        if (!f_enum_project_markers3(nullptr, idx, &rgn, &p, &e, &nm, &n, &col)) return false;
        return !rgn && n == num && std::fabs(p - pos) < 1e-9;
    };
    const int hint = static_cast<int>(f_marker_value(nullptr, m, "I_INDEX"));
    if (hint >= 0 && is_it(hint)) return hint;
    for (int i = 0; i < 100000; ++i) {
        bool        rgn = false;
        double      p = 0.0, e = 0.0;
        const char* nm = nullptr;
        int         n = -1, col = 0;
        if (!f_enum_project_markers3(nullptr, i, &rgn, &p, &e, &nm, &n, &col)) break;
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

// Deletes the project markers RAV recorded, by GUID (a GUID no longer there is ignored).
void DeleteOwnProjectMarkers(const std::vector<ProjectMarkerRef>& own)
{
    for (const ProjectMarkerRef& ref : own) {
        if (ref.guid.empty()) continue;
        ProjectMarker* m = f_get_region_or_marker(nullptr, -1, ref.guid.c_str());
        if (!m || MarkerGuid(m) != ref.guid) continue;
        const int idx = EnumIndexOf(m);
        if (idx >= 0) f_delete_project_marker(nullptr, idx);
    }
}

// Adds a project marker; its GUID ("" when it could not be added or found).
std::string AddProjectMarker(double pos, const std::string& name, int color)
{
    if (f_add_region_or_marker) return MarkerGuid(f_add_region_or_marker(nullptr, false, pos, pos, name.c_str(), -1, color));
    const int num = f_add_project_marker2(nullptr, false, pos, pos, name.c_str(), -1, color);
    if (num < 0) return "";
    const int count = f_num_regions_or_markers ? f_num_regions_or_markers(nullptr) : 100000;
    for (int i = 0; i < count; ++i) {
        ProjectMarker* m = f_get_region_or_marker(nullptr, i, nullptr);
        if (!m) break;
        if (f_marker_value(nullptr, m, "B_ISREGION") == 0.0 && static_cast<int>(f_marker_value(nullptr, m, "I_NUMBER")) == num &&
            std::fabs(f_marker_value(nullptr, m, "D_STARTPOS") - pos) < 1e-9)
            return MarkerGuid(m);
    }
    return "";
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
    const bool want_take = mode != MarkerMode::Project;
    const bool want_project = mode != MarkerMode::Take;
    if (want_take && TakeApiReady()) {
        std::vector<ExistingMarker> foreign = TakeMarkers(take);
        std::vector<ExistingMarker> mine;  // this run's take markers on this take
        for (const PlannedMarker& m : plan) {
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
    if (want_project && ProjectApiReady()) {
        std::vector<ExistingMarker> foreign = ProjectMarkers();
        DropOwn(foreign, written_project);
        for (const PlannedMarker& m : plan) {
            if (FindTwinMarker(written_project, m.project_t, m.name) >= 0) continue;  // already written now
            if (FindTwinMarker(foreign, m.project_t, m.name) >= 0) {
                ++res.already_present;
                continue;
            }
            const std::string guid = AddProjectMarker(m.project_t, m.name, NativeColor(m.color));
            if (guid.empty()) continue;
            ProjectMarkerRef ref;
            ref.guid = guid;
            ref.t = m.project_t;
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
                ClearPreviewed(rec);
                rec.ptmarkers = tkeep;
                rec.ppmarkers = pkeep;
                for (const TakeMarkerRef& t : own_take) rec.ptmarkers.push_back(t);
                for (const ProjectMarkerRef& p : own_project) rec.ppmarkers.push_back(p);
                if (want && why.empty()) RecordPreviewed(rec, plan, mode);
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
