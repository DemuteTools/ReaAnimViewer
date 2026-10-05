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
constexpr char kUndoApply[] = "RAV: Apply auto-tagging";
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

// Removes from `existing` the markers this Apply wrote (they are RAV's, not foreign).
void DropOwn(std::vector<ExistingMarker>& existing, const std::vector<ExistingMarker>& own)
{
    for (const ExistingMarker& o : own) {
        const int i = FindTwinMarker(existing, o.t, o.name, 1e-9);
        if (i >= 0) existing.erase(existing.begin() + i);
    }
}

}  // namespace

void InitTagMarkers(void* (*get_func)(const char* name))
{
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
    g_mode = mode == MarkerMode::Take ? 0 : mode == MarkerMode::Project ? 1 : 2;
    SavePrefFloat(kPrefMarkers, static_cast<float>(g_mode));
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

ApplyResult ApplyTaggingMarkers()
{
    ApplyResult res;
    try {
        const MarkerMode mode = GetTaggingMarkerMode();
        const bool       want_take = mode != MarkerMode::Project;
        const bool       want_project = mode != MarkerMode::Take;
        if ((want_take && !TakeApiReady()) || (want_project && !ProjectApiReady())) {
            res.error = want_project && !ProjectApiReady()
                            ? "Project markers need REAPER 7 or newer: nothing was written."
                            : "Take markers need REAPER 5.981 or newer: nothing was written.";
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
        // First every item's previous RAV markers go (both kinds: an option change removes the
        // other kind), so no item takes another's old RAV marker for a foreign one.
        for (Run& r : runs) {
            try {
                MediaItem_Take* take = RavTakeOf(r.item);
                if (!take) continue;
                if (TakeApiReady()) DeleteOwnTakeMarkers(take, r.det.rules.tmarkers);
                if (ProjectApiReady()) DeleteOwnProjectMarkers(r.det.rules.pmarkers);
            } catch (...) {
            }
        }
        std::vector<ExistingMarker> written_project;  // this Apply's project markers (RAV's, never foreign)
        for (Run& r : runs) {
            try {
                MediaItem_Take* take = RavTakeOf(r.item);
                if (!take) {
                    ++res.failed;
                    continue;
                }
                const ItemRules&                 rules = r.det.rules;
                const std::vector<ShownEvent>    list = BuildEventList(r.det.events, rules.events, rules.blocks.size());
                const std::vector<PlannedMarker> plan = PlanMarkers(list, rules.blocks, r.det.map);

                std::vector<TakeMarkerRef>    own_take;
                std::vector<ProjectMarkerRef> own_project;
                if (want_take) {
                    std::vector<ExistingMarker> foreign = TakeMarkers(take);
                    std::vector<ExistingMarker> mine;  // this Apply's take markers on this take
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
                if (want_project) {
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

                // The record: the markers RAV now owns, the snapshot and the applied state.
                const bool keep_project_refs = !ProjectApiReady();  // could not delete them: still RAV's
                std::string err;
                const bool ok = ModifyItemRulesNoUndo(
                    r.item,
                    [&](ItemRules& rec) {
                        rec.tmarkers = own_take;
                        if (keep_project_refs) {
                            for (const ProjectMarkerRef& p : own_project) rec.pmarkers.push_back(p);
                        } else {
                            rec.pmarkers = own_project;
                        }
                        RecordApplied(rec, plan, mode);
                        return true;
                    },
                    &err);
                if (ok) ++res.items;
                else ++res.failed;
            } catch (...) {
                ++res.failed;
            }
        }
        Undo_EndBlock2(nullptr, kUndoApply, UNDO_STATE_ALL);
        UpdateArrange();
        res.ran = res.items > 0;
        TaggingReread();  // the footer: "markers up to date"
    } catch (const std::exception& e) {
        res.error = std::string("Apply failed: ") + e.what();
    } catch (...) {
        res.error = "Apply failed.";
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
