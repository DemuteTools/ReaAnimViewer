// SPDX-License-Identifier: MIT
//
// See detection_measure.h.

#include "detection_measure.h"

#ifdef _WIN32

#include <cctype>
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
#include "pcm_source_anim.h"
#include "reaper_api.h"

namespace rav {
namespace {

constexpr const char kTitle[] = "RAV: Measure detection against reference markers";
constexpr const char kRefName[] = "REF";
constexpr const char kDetName[] = "RAV?";
constexpr double kRateHz = 240.0;
constexpr double kMatchWindowS = 0.150;

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

// The dialog's knobs, remembered for the session.
struct Knobs {
    double height_pct = 25.0;     // h = floor + this % of the floor-to-swing gap
    double stillness_pct = 30.0;  // v = this percentile of |vertical speed|
    double sensitivity = 0.0;     // 0..1
    double min_hold_ms = 30.0;
    double cooldown_ms = 250.0;
    double offset_ms = 0.0;
    int    per_bone_floor = 1;    // 1: heel and toe each above their own floor (see Implementation Notes)
    int    knee_bend = 1;         // 1: the Footsteps knee-bend condition is on
};
Knobs g_knobs;

std::string Format(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

std::string Trimmed(const char* s)
{
    std::string t = s ? s : "";
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    size_t i = 0;
    while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
    return t.substr(i);
}

bool SameName(const char* name, const char* want)
{
    const std::string t = Trimmed(name);
    if (t.size() != std::strlen(want)) return false;
    for (size_t i = 0; i < t.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(t[i])) != std::tolower(static_cast<unsigned char>(want[i])))
            return false;
    return true;
}

std::string BaseName(const std::string& path)
{
    const size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string KnobsText(const Knobs& k)
{
    return Format("height threshold %g%% of gap, stillness p%g, sensitivity %g, min hold %g ms, cooldown %g ms, offset %+g ms, "
                  "floor %s, knee bend %s",
                  k.height_pct, k.stillness_pct, k.sensitivity, k.min_hold_ms, k.cooldown_ms, k.offset_ms,
                  k.per_bone_floor ? "per bone" : "per foot", k.knee_bend ? "on" : "off");
}

// The knobs dialog. False on Cancel or a bad value (the user was told).
bool AskKnobs(Knobs& k)
{
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%g,%g,%g,%g,%g,%g,%d,%d", k.height_pct, k.stillness_pct, k.sensitivity,
                  k.min_hold_ms, k.cooldown_ms, k.offset_ms, k.per_bone_floor, k.knee_bend);
    if (!GetUserInputs(kTitle, 8,
                       "Height threshold (% of gap),Stillness (speed percentile),Sensitivity (0-1),Min hold (ms),"
                       "Cooldown (ms),Offset (ms),Floor per bone (1) / foot (0),Knee bend on (1) / off (0),"
                       "extrawidth=80",
                       buf, sizeof(buf)))
        return false;

    double v[8] = {};
    const char* p = buf;
    for (int i = 0; i < 8; ++i) {
        char* end = nullptr;
        v[i] = std::strtod(p, &end);
        while (end && *end == ' ') ++end;
        const bool last = (i == 7);
        if (end == p || !std::isfinite(v[i]) || (last ? (*end != '\0') : (*end != ','))) {
            ShowMessageBox("Each field must be a number.", kTitle, 0);
            return false;
        }
        p = end + (last ? 0 : 1);
    }
    if (!(v[0] > 0.0 && v[0] <= 100.0) || !(v[1] >= 0.0 && v[1] <= 100.0) || !(v[2] >= 0.0 && v[2] <= 1.0) ||
        v[3] < 0.0 || v[4] < 0.0 || !(v[6] == 0.0 || v[6] == 1.0) || !(v[7] == 0.0 || v[7] == 1.0)) {
        ShowMessageBox("Height threshold: above 0, up to 100. Stillness: 0 to 100. Sensitivity: 0 to 1.\n"
                       "Min hold and cooldown: 0 or more. Floor: 1 (per bone) or 0 (per foot).\n"
                       "Knee bend: 1 (on) or 0 (off).",
                       kTitle, 0);
        return false;
    }
    k.height_pct = v[0];
    k.stillness_pct = v[1];
    k.sensitivity = v[2];
    k.min_hold_ms = v[3];
    k.cooldown_ms = v[4];
    k.offset_ms = v[5];
    k.per_bone_floor = static_cast<int>(v[6]);
    k.knee_bend = static_cast<int>(v[7]);
    return true;
}

struct ItemRun {
    std::string         label;
    std::string         skipped;  // non-empty = left out of the total, with the reason
    MediaItem_Take*     take = nullptr;
    MatchResult         match;
    std::vector<Block>  blocks;   // as analysed
    std::vector<double> det;      // every detection (clip time)
    bool                ref_from_project = false;  // REF read from project markers over the item
    std::vector<double> refs;     // REF times (clip time), for the per-step diagnostic
    std::vector<Event>  events;   // every detection with its condition entry times
    std::string         dump;     // the bone-track dump written for offline tuning ("" = none)
};

std::string TimesText(const std::vector<double>& t)
{
    if (t.empty()) return "-";
    std::string s;
    const size_t cap = 24;
    for (size_t i = 0; i < t.size() && i < cap; ++i) s += Format(i ? " %.3f" : "%.3f", t[i]);
    if (t.size() > cap) s += Format(" ... (+%zu)", t.size() - cap);
    return s;
}

std::string MatchText(const MatchResult& m)
{
    return Format("REF %d  det %d  match %d  R %.1f%%  P %.1f%%  err mean %.1f ms max %.1f ms (lag %+.1f ms)", m.n_ref,
                  m.n_det, m.n_match, m.recall * 100.0, m.precision * 100.0, m.mean_abs_err_s * 1000.0,
                  m.max_abs_err_s * 1000.0, m.mean_signed_err_s * 1000.0);
}

// Dev harness: writes every bone's track (240 Hz, metres, model Y up) and the REF times to
// <project folder>/RAV_detection_dump/<file>.csv, so detection can be tuned offline on
// the exact data REAPER measured. Returns the file path, "" on failure. No-throw.
std::string DumpTracks(const CpuAsset& asset, const std::string& label, const std::vector<double>& refs,
                       double lo, double hi)
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
        for (size_t i = 0; i < refs.size(); ++i) out << (i ? "," : "") << Format("%.6f", refs[i]);
        out << "\n# parent=";
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

void MeasureItem(MediaItem* item, const Knobs& k, ItemRun& run)
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

    // REF take markers (source time). Without any, the REF project markers over the item
    // (not regions), mapped to source time. Never both: the same step would count twice.
    std::vector<double> refs;
    const int nm = g_num_take_markers(take);
    for (int i = 0; i < nm; ++i) {
        char name[256] = {};
        const double pos = g_get_take_marker(take, i, name, sizeof(name), nullptr);
        if (pos >= 0.0 && SameName(name, kRefName)) refs.push_back(pos);
    }
    if (refs.empty()) {
        bool is_rgn = false;
        double pos = 0.0, rgn_end = 0.0;
        const char* name = nullptr;
        int number = 0;
        for (int i = 0; EnumProjectMarkers2(nullptr, i, &is_rgn, &pos, &rgn_end, &name, &number) > 0; ++i) {
            if (is_rgn || !SameName(name, kRefName)) continue;
            if (pos < item_pos - 1e-9 || pos > item_pos + item_len + 1e-9) continue;
            refs.push_back(lo + (pos - item_pos) * rate);
        }
        if (!refs.empty()) run.ref_from_project = true;
    }
    if (refs.empty()) {
        run.skipped = "no REF marker (take marker, or project marker over the item)";
        return;
    }
    int refs_inside = 0;
    for (double r : refs)
        if (r >= lo && r <= hi) ++refs_inside;
    if (refs_inside == 0) {
        run.skipped = "no REF marker inside the visible part of the item";
        return;
    }

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
    run.dump = DumpTracks(asset, run.label, refs, lo, hi);

    // Roles -> bones -> tracks.
    std::vector<std::string> names;
    for (const SceneBone& b : asset.skeleton.bones) names.push_back(b.name);
    const std::vector<int> role_bone = GuessRoleMapping(names);
    Preset preset = FootstepsPreset(k.knee_bend != 0);
    const std::vector<Role> used = RolesUsed(preset.blocks);
    std::vector<int> role_to_track(static_cast<size_t>(Role::Count), -1);
    std::vector<int> bone_indices;
    for (Role r : used) {
        const int b = role_bone[static_cast<size_t>(r)];
        if (b < 0) continue;
        role_to_track[static_cast<size_t>(r)] = static_cast<int>(bone_indices.size());
        bone_indices.push_back(b);
    }
    std::string missing;
    std::vector<Block> blocks = preset.blocks;
    if (!BindRoles(blocks, role_to_track, &missing)) {
        run.skipped = "roles not found: " + missing;
        return;
    }
    for (Block& b : blocks) {
        b.min_hold_ms = k.min_hold_ms;
        b.cooldown_ms = k.cooldown_ms;
        b.offset_ms = k.offset_ms;
    }

    const std::vector<BoneTrack> tracks = SampleBoneTracks(asset, bone_indices, kRateHz);
    if (tracks.empty()) {
        run.skipped = "could not sample the animation";
        return;
    }

    AnalyseOptions ao;
    ao.position_fraction = k.height_pct / 100.0;
    ao.speed_percentile = k.stillness_pct;
    ao.per_bone_floor = k.per_bone_floor != 0;
    run.blocks = Analyse(blocks, tracks, ao);
    DetectOptions dop;
    dop.sensitivity = k.sensitivity;
    run.events = Detect(run.blocks, tracks, dop);
    for (const Event& e : run.events) run.det.push_back(e.time_s);
    run.refs = refs;
    run.match = MatchEvents(refs, run.det, kMatchWindowS, lo, hi);
}

// Clears the RAV? take markers of every RAV take in the run (skipped ones too, so no
// stale detections stay behind) and writes the new ones on the measured items, in one
// undo point.
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
            if (std::strcmp(name, kDetName) == 0) g_delete_take_marker(r.take, i);
        }
        if (!r.skipped.empty()) continue;
        for (double t : r.det) {
            if (t < 0.0) continue;
            double pos = t;
            g_set_take_marker(r.take, -1, kDetName, &pos, nullptr);
        }
    }
    Undo_EndBlock("RAV: Measure detection (RAV? take markers)", UNDO_STATE_ITEMS);
    UpdateArrange();
}

// Per detection: the nearest REF and when each Footsteps condition came true, relative to
// that REF (which condition makes the event late or early).
std::string StepsText(const ItemRun& r)
{
    static const char* const kCond[] = {"height", "still", "knee"};
    std::string s;
    for (const Event& e : r.events) {
        s += Format("    det %.3f %s", e.time_s, e.marker.c_str());
        double best = -1.0;
        for (double t : r.refs)
            if (best < 0.0 || std::fabs(t - e.time_s) < std::fabs(best - e.time_s)) best = t;
        if (best < 0.0) {
            s += "\n";
            continue;
        }
        s += Format("  nearest REF %.3f (%+.0f ms):", best, (e.time_s - best) * 1000.0);
        for (size_t c = 0; c < e.cond_entry_s.size(); ++c)
            s += Format(" %s %+.0f", c < 3 ? kCond[c] : "cond", (e.cond_entry_s[c] - best) * 1000.0);
        s += " ms\n";
    }
    return s;
}

void Report(const std::vector<ItemRun>& runs, const Knobs& k)
{
    std::string out = Format("\n=== %s ===\nFootsteps preset + Analyse, %g Hz, match window +-%g ms, source time\n", kTitle,
                             kRateHz, kMatchWindowS * 1000.0);
    std::vector<MatchResult> measured;
    for (size_t i = 0; i < runs.size(); ++i) {
        const ItemRun& r = runs[i];
        const std::string label = Format("#%zu %s", i + 1, r.label.empty() ? "(item)" : r.label.c_str());
        if (!r.skipped.empty()) {
            out += label + "  skipped: " + r.skipped + "\n";
            continue;
        }
        measured.push_back(r.match);
        out += label + "  " + MatchText(r.match) + (r.ref_from_project ? "  (REF: project markers)" : "") + "\n";
        std::string th = " ";
        for (const Block& b : r.blocks) {
            if (b.conditions.size() < 2) continue;
            const Condition& h = b.conditions[0];
            const Condition& v = b.conditions[1];
            std::string floor;
            if (h.signal.bone_floors.empty()) {
                floor = Format("%.3f", h.signal.floor_y);
            } else {  // heel / toe floors (+ the floor of their combined height, about 0)
                for (size_t f = 0; f < h.signal.bone_floors.size(); ++f)
                    floor += Format(f ? "/%.3f" : "%.3f", h.signal.bone_floors[f]);
                floor += Format(" (+%.3f)", h.signal.floor_y);
            }
            th += Format(" %s: h=%.3f m (m=%.3f)  v=%.3f m/s  floor=%s m", b.marker.c_str(), h.threshold, h.margin,
                         v.threshold, floor.c_str());
            if (b.conditions.size() >= 3) th += Format("  knee<%.3f m/s", b.conditions[2].threshold);
            th += ";";
        }
        out += th + "\n";
        out += "  unmatched REF: " + TimesText(r.match.unmatched_ref) +
               "  unmatched det: " + TimesText(r.match.unmatched_det) + "\n";
        out += StepsText(r);
        if (!r.dump.empty()) out += "  bone tracks: " + r.dump + "\n";
    }
    const MatchResult total = SumMatches(measured);
    const bool go = !measured.empty() && PassesAccuracyGate(total);
    out += Format("TOTAL (%zu item%s)  ", measured.size(), measured.size() == 1 ? "" : "s");
    out += measured.empty() ? std::string("nothing measured") : MatchText(total);
    out += Format(" -> %s (NFR-AT1: R>=98%% P>=98%% max |err|<=16 ms)  [knobs used: %s]\n", go ? "GO" : "NO-GO",
                  KnobsText(k).c_str());
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
            ShowMessageBox("Select animation items with REF markers (take markers, or project markers over the items)", kTitle, 0);
            return;
        }
        Knobs k = g_knobs;
        if (!AskKnobs(k)) return;
        g_knobs = k;

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
