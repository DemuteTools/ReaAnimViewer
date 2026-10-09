// SPDX-License-Identifier: MIT
//
// See detection_measure.h.

#include "detection_measure.h"

#ifdef _WIN32

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "asset_loader.h"
#include "bone_events.h"
#include "bone_presets.h"
#include "bone_roles.h"
#include "bone_sampling.h"
#include "footstep_measure.h"
#include "item_rules.h"
#include "motion_physics.h"
#include "pcm_source_anim.h"
#include "reaper_api.h"
#include "ref_labels.h"
#include "role_map_store.h"

namespace rav {
namespace {

constexpr const char kTitle[] = "RAV: Measure detection against reference markers";
constexpr const char kDetName[] = "RAV?";
constexpr double kRateHz = kDetectRateHz;

// Optional take-marker functions (REAPER 5.981+), resolved through GetFunc so a missing
// one never refuses the whole plugin.
using GetNumTakeMarkersFn = int (*)(MediaItem_Take* take);
using GetTakeMarkerFn = double (*)(MediaItem_Take* take, int idx, char* nameOut, int nameOut_sz, int* colorOut);
using SetTakeMarkerFn = int (*)(MediaItem_Take* take, int idx, const char* nameIn, double* srcposIn, int* colorIn);
using DeleteTakeMarkerFn = bool (*)(MediaItem_Take* take, int idx);
GetNumTakeMarkersFn g_num_take_markers = nullptr;
GetTakeMarkerFn     g_get_take_marker = nullptr;
SetTakeMarkerFn     g_set_take_marker = nullptr;
DeleteTakeMarkerFn  g_delete_take_marker = nullptr;

// The dialog's knobs (FootstepsParams defaults), remembered for the session.
FootstepsParams g_params;

std::string Format(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

std::string BaseName(const std::string& path)
{
    const size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

// The knobs dialog. False on Cancel or a bad value (the user was told).
bool AskKnobs(FootstepsParams& k)
{
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%g,%g,%g,%g,%g", k.height_fraction * 100.0, k.knee_fraction, k.yaw_limit_dps,
                  k.cooldown_ms, k.offset_ms);
    if (!GetUserInputs(kTitle, 5,
                       "Height threshold (% of gap),Knee onset (x p95 of flexion speed),Foot yaw limit (deg/s),"
                       "Cooldown (ms),Offset (ms),extrawidth=80",
                       buf, sizeof(buf)))
        return false;

    double v[5] = {};
    const char* p = buf;
    for (int i = 0; i < 5; ++i) {
        char* end = nullptr;
        v[i] = std::strtod(p, &end);
        while (end && *end == ' ') ++end;
        const bool last = (i == 4);
        if (end == p || !std::isfinite(v[i]) || (last ? (*end != '\0') : (*end != ','))) {
            ShowMessageBox("Each field must be a number.", kTitle, 0);
            return false;
        }
        p = end + (last ? 0 : 1);
    }
    if (!(v[0] > 0.0 && v[0] <= 100.0) || !(v[1] >= 0.0 && v[1] <= 1.0) || !(v[2] > 0.0) || v[3] < 0.0) {
        ShowMessageBox("Height threshold: above 0, up to 100. Knee onset: 0 to 1.\n"
                       "Foot yaw limit: above 0. Cooldown: 0 or more.",
                       kTitle, 0);
        return false;
    }
    k.height_fraction = v[0] / 100.0;
    k.knee_fraction = v[1];
    k.yaw_limit_dps = v[2];
    k.cooldown_ms = v[3];
    k.offset_ms = v[4];
    return true;
}

struct ItemRun {
    std::string         label;
    std::string         skipped;  // non-empty = left out of the total, with the reason
    MediaItem_Take*     take = nullptr;
    ItemMeasure         m;        // the shared measure (footstep_measure.h)
    bool                ref_from_project = false;  // REF read from project markers over the item
    RefTimes            refs;     // REF times (clip time), all and per label (ref_labels.h)
    std::string         dump;     // the bone-track dump written for offline tuning ("" = none)
    // Spike 10-8a: the physics foot events of every loaded item, REF or not (PHY take markers).
    bool                      phys_ran = false;
    std::string               phys_summary;
    std::vector<PhysicsEvent> phys_events;
    std::string               hand_summary;  // story 10-8c
    std::vector<PhysicsEvent> hand_events;
};


// Dev harness: writes every bone's track (240 Hz, metres, model Y up) and the REF times, all
// and per label with each label's bone, to <project folder>/RAV_detection_dump/<file>.csv, so
// detection can be tuned offline on the exact data REAPER measured. Positions only (no
// orientations): rotation conditions cannot be tuned offline. Returns the file path, "" on
// failure. No-throw.
std::string DumpTracks(const CpuAsset& asset, const std::string& label, const RefTimes& refs, double lo, double hi)
{
    try {
        std::vector<int> all;
        for (size_t b = 0; b < asset.skeleton.bones.size(); ++b) all.push_back(static_cast<int>(b));
        const std::vector<BoneTrack> tr = SampleBoneTracks(asset, all, kRateHz);
        if (tr.empty()) return "";
        char proj[4096] = {};
        EnumProjects(-1, proj, sizeof(proj));
        std::filesystem::path dir = proj[0] ? std::filesystem::u8path(proj).parent_path()
                                            : std::filesystem::u8path(GetResourcePath());
        dir /= "RAV_detection_dump";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path file = dir / std::filesystem::u8path(label + ".csv");
        std::ofstream out(file, std::ios::binary);
        if (!out) return "";
        out << "# item=" << label << "\n# rate_hz=" << kRateHz << "\n# visible=" << Format("%.6f,%.6f", lo, hi)
            << "\n# ref=";
        for (size_t i = 0; i < refs.all.size(); ++i) out << (i ? "," : "") << Format("%.6f", refs.all[i]);
        out << "\n" << RefLabelLines(refs) << "# parent=";
        for (size_t b = 0; b < asset.skeleton.bones.size(); ++b) out << (b ? "," : "") << asset.skeleton.bones[b].parentIdx;
        out << "\nt";
        for (const SceneBone& b : asset.skeleton.bones) out << "," << b.name << ".x," << b.name << ".y," << b.name << ".z";
        out << "\n";
        const size_t n = tr[0].pos.size();
        for (size_t i = 0; i < n; ++i) {
            out << Format("%.6f", static_cast<double>(i) / kRateHz);
            for (const BoneTrack& t : tr) out << Format(",%.6f,%.6f,%.6f", t.pos[i].x, t.pos[i].y, t.pos[i].z);
            out << "\n";
        }
        return out ? file.u8string() : "";
    } catch (...) {
        return "";
    }
}

// Spike 10-8a: the physics foot events (motion_physics.h, default constants and timing) on the
// user's role mapping (roles.txt, else the guess), as the Tagging view maps roles. Story 10-8c:
// the hand events too.
void MeasurePhysics(const CpuAsset& asset, const std::vector<std::string>& names, const RoleMapFile& roles,
                    ItemRun& run)
{
    const std::vector<int> role_bone = ResolveRoleMapping(roles, names);
    const PhysicsAnalysis  a = AnalyseMotion(PhysicsRoleTracks(
        role_bone, [&](const std::vector<int>& bones) { return SampleBoneTracks(asset, bones, kRateHz); }));
    run.phys_events = FootEvents(a);
    run.phys_summary = PhysicsSummary(a, run.phys_events);
    run.hand_events = HandEvents(a);
    run.hand_summary = HandSummary(a, run.hand_events);
    run.phys_ran = true;  // last: a failure above leaves no bare "physics:" line
}

void MeasureItem(MediaItem* item, const FootstepsParams& k, ItemRun& run)
{
    MediaItem_Take* take = item ? GetActiveTake(item) : nullptr;
    if (!take) {
        run.skipped = "no active take";
        return;
    }
    PCM_source* src = GetMediaItemTake_Source(take);
    const char* fn = (src && IsRavAnimSource(src)) ? src->GetFileName() : nullptr;
    if (!fn || !fn[0]) {
        run.skipped = "not a RAV animation item";
        return;
    }
    const std::string path = fn;
    run.label = BaseName(path);
    // A section / reversed take wraps our source: its take markers are in the wrapper's
    // time, not the clip's.
    const char* type = src->GetType();
    if (!type || std::strcmp(type, "RAV_ANIM") != 0) {
        run.skipped = "section or reversed take not supported";
        return;
    }
    run.take = take;  // a RAV take: its old RAV? markers are cleared even if it is skipped below

    // The visible part of the clip, in source time.
    const double lo = GetMediaItemTakeInfo_Value(take, "D_STARTOFFS");
    double rate = GetMediaItemTakeInfo_Value(take, "D_PLAYRATE");
    if (!(rate > 0.0) || !std::isfinite(rate)) rate = 1.0;
    const double item_pos = GetMediaItemInfo_Value(item, "D_POSITION");
    const double item_len = GetMediaItemInfo_Value(item, "D_LENGTH");
    const double hi = lo + item_len * rate;

    // REF and "REF <label>" take markers (source time). Without any, the REF project markers
    // over the item (not regions), mapped to source time. Never both: the same step would
    // count twice.
    RefTimes refs;
    std::string ref_label;
    const int nm = g_num_take_markers(take);
    for (int i = 0; i < nm; ++i) {
        char name[256] = {};
        const double pos = g_get_take_marker(take, i, name, sizeof(name), nullptr);
        if (pos >= 0.0 && ParseRefMarkerName(name, &ref_label)) refs.Add(ref_label, pos);
    }
    if (refs.all.empty()) {
        bool is_rgn = false;
        double pos = 0.0, rgn_end = 0.0;
        const char* name = nullptr;
        int number = 0;
        for (int i = 0; EnumProjectMarkers2(nullptr, i, &is_rgn, &pos, &rgn_end, &name, &number) > 0; ++i) {
            if (is_rgn || !ParseRefMarkerName(name, &ref_label)) continue;
            if (pos < item_pos - 1e-9 || pos > item_pos + item_len + 1e-9) continue;
            refs.Add(ref_label, lo + (pos - item_pos) * rate);
        }
        if (!refs.all.empty()) run.ref_from_project = true;
    }
    // Without a REF (inside the visible part), the item is still loaded, dumped and measured
    // by physics (spike 10-8a); only the Footsteps measure is skipped.
    std::string foot_skip;
    if (refs.all.empty()) foot_skip = "no REF: physics only";
    run.refs = refs;  // the report shows the labels even if the item is skipped below
    int refs_inside = 0;
    for (double r : refs.all)
        if (r >= lo && r <= hi) ++refs_inside;
    if (foot_skip.empty() && refs_inside == 0) foot_skip = "no REF marker inside the visible part of the item";

    const CpuLoadResult loaded = LoadCpuAsset(path);
    if (!loaded.asset) {
        run.skipped = "could not load: " + (loaded.hint.empty() ? loaded.detail : loaded.hint);
        return;
    }
    const CpuAsset& asset = *loaded.asset;
    if (asset.animations.empty() || asset.no_animation) {
        run.skipped = "the file has no animation";
        return;
    }
    std::vector<std::string> names;
    for (const SceneBone& b : asset.skeleton.bones) names.push_back(b.name);
    // Each label is a role key: its bone on this skeleton, from the user's roles.txt (stored,
    // else guessed), as the Tagging view maps it.
    const RoleMapFile roles = ReadRoleMapFile(RulesResourceRoot());
    if (!run.refs.by_label.empty()) ResolveRefBones(roles, names, &run.refs);
    run.dump = DumpTracks(asset, run.label, run.refs, lo, hi);
    MeasurePhysics(asset, names, roles, run);
    if (!foot_skip.empty()) {
        run.skipped = foot_skip;
        return;
    }

    // Roles -> bones -> tracks -> Analyse + Detect -> match (shared with tests/detection_eval).
    // Every REF counts, labelled or not.
    run.m = MeasureFootsteps(
        names, [&](const std::vector<int>& bones) { return SampleBoneTracks(asset, bones, kRateHz); }, run.refs.all,
        lo, hi, k);
    if (!run.m.skipped.empty()) run.skipped = run.m.skipped;
}

// Clears the RAV? and PHY take markers of every RAV take in the run (skipped ones too, so no
// stale detections stay behind) and writes the new ones: RAV? on the measured items, PHY on
// every item physics ran on. One undo point.
void WriteDetectionMarkers(const std::vector<ItemRun>& runs)
{
    bool any = false;
    for (const ItemRun& r : runs)
        if (r.take) any = true;
    if (!any) return;

    Undo_BeginBlock();
    for (const ItemRun& r : runs) {
        if (!r.take) continue;
        for (int i = g_num_take_markers(r.take) - 1; i >= 0; --i) {
            char name[256] = {};
            g_get_take_marker(r.take, i, name, sizeof(name), nullptr);
            if (std::strcmp(name, kDetName) == 0 || IsPhysicsMarkerName(name)) g_delete_take_marker(r.take, i);
        }
        for (const std::vector<PhysicsEvent>* evs : {&r.phys_events, &r.hand_events})
            for (const PhysicsEvent& e : *evs) {
                if (e.time_s < 0.0) continue;
                double pos = e.time_s;
                g_set_take_marker(r.take, -1, PhysicsMarkerName(e).c_str(), &pos, nullptr);
            }
        if (!r.skipped.empty()) continue;
        for (double t : r.m.det) {
            if (t < 0.0) continue;
            double pos = t;
            g_set_take_marker(r.take, -1, kDetName, &pos, nullptr);
        }
    }
    Undo_EndBlock("RAV: Measure detection (RAV? and PHY take markers)", UNDO_STATE_ITEMS);
    UpdateArrange();
}

void Report(const std::vector<ItemRun>& runs, const FootstepsParams& k)
{
    std::string out = Format("\n=== %s ===\nFootsteps preset + Analyse, %g Hz, match window +-%g ms, source time\n", kTitle,
                             kRateHz, kMatchWindowS * 1000.0);
    std::vector<MatchResult> measured;
    for (size_t i = 0; i < runs.size(); ++i) {
        const ItemRun& r = runs[i];
        const std::string label = Format("#%zu %s", i + 1, r.label.empty() ? "(item)" : r.label.c_str());
        // Spike 10-8a: one physics line per loaded item, then its events (its PHY markers); story
        // 10-8c: the hands' line after it, then the foot events and the hand events.
        const std::string phys = r.phys_ran ? "  physics: " + r.phys_summary + "\n  " + r.hand_summary + "\n" +
                                                  PhysicsEventLines(r.phys_events, "    ") +
                                                  PhysicsEventLines(r.hand_events, "    ")
                                            : "";
        if (!r.skipped.empty()) {
            out += SkippedReport(label, r.skipped);
            out += RefLabelsText(r.refs);
            if (!r.dump.empty()) out += "  bone tracks: " + r.dump + "\n";
            out += phys;
            continue;
        }
        measured.push_back(r.m.match);
        out += ItemReport(label, r.m, r.refs.all, r.ref_from_project ? "  (REF: project markers)" : "");
        out += RefLabelsText(r.refs);
        if (!r.dump.empty()) out += "  bone tracks: " + r.dump + "\n";
        out += phys;
    }
    out += TotalReport(measured, k);
    out += "Physics (spike 10-8a, PHY take markers): step timing " + StepTimingText(PhysicsParams{}) +
           "; strengths in leg lengths per second, pivots in degrees\n";
    ShowConsoleMsg(out.c_str());
}

}  // namespace

void InitDetectionMeasure(void* (*get_func)(const char* name))
{
    if (!get_func) return;
    g_num_take_markers = reinterpret_cast<GetNumTakeMarkersFn>(get_func("GetNumTakeMarkers"));
    g_get_take_marker = reinterpret_cast<GetTakeMarkerFn>(get_func("GetTakeMarker"));
    g_set_take_marker = reinterpret_cast<SetTakeMarkerFn>(get_func("SetTakeMarker"));
    g_delete_take_marker = reinterpret_cast<DeleteTakeMarkerFn>(get_func("DeleteTakeMarker"));
}

void MeasureDetectionOnSelectedItems()
{
    try {
        if (!g_num_take_markers || !g_get_take_marker || !g_set_take_marker || !g_delete_take_marker) {
            ShowMessageBox("This action needs REAPER 5.981 or newer (take markers).", kTitle, 0);
            return;
        }
        const int n = CountSelectedMediaItems(nullptr);
        if (n <= 0) {
            ShowMessageBox("Select animation items. REF markers (take markers, or project markers over the items) "
                           "measure the Footsteps preset; every item gets PHY markers.",
                           kTitle, 0);
            return;
        }
        FootstepsParams k = g_params;
        if (!AskKnobs(k)) return;
        g_params = k;

        std::vector<MediaItem*> items;
        for (int i = 0; i < n; ++i) items.push_back(GetSelectedMediaItem(nullptr, i));
        std::vector<ItemRun> runs(items.size());
        for (size_t i = 0; i < items.size(); ++i) {
            try {
                MeasureItem(items[i], k, runs[i]);
            } catch (const std::exception& e) {
                runs[i].skipped = std::string("failed: ") + e.what();
            } catch (...) {
                runs[i].skipped = "failed";
            }
        }
        WriteDetectionMarkers(runs);
        Report(runs, k);
    } catch (...) {
        ShowMessageBox("The measurement failed (out of memory?). Nothing more was done.", kTitle, 0);
    }
}

}  // namespace rav

#endif  // _WIN32
