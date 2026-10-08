// SPDX-License-Identifier: MIT
//
// Offline evaluator for footstep detection (Epic 10, spike 10-0). Runs the real engine
// (footstep_measure.h: the same code as "RAV: Measure detection against reference
// markers") on the bone-track dumps that action writes to RAV_detection_dump/*.csv, and
// prints the same report. For tuning FootstepsParams on tagged clips without REAPER.
//
//   detection_eval [--param=value ...] [--json] file.csv...
//   detection_eval --single [--axis=<axis>] [--window=<ms>] [--approach=<ms>] [--param=value ...] file.csv...
//
// Params (FootstepsParams): --height=<% of gap> --knee=<x p95> --yaw=<deg/s>
//   --yaw-margin=<deg/s> --hold=<ms> --cooldown=<ms> --offset=<ms> --floor=bone|foot
//   --floor-pct=<pct> --margin-ratio=<r> --smooth=<ms> --sensitivity=<0-1> --edge=<ms>
// --json: one JSON line per item, then one for the TOTAL (for scripted parameter search).
//
// --single (spike 10-7a, step 1): each measure alone, per labelled bone. For every
// '# ref.<label>=' with a bone, one block per measure on that bone: Speed (today's), Stillness
// and Relative drop, floor reference, "below", on --axis (vertical, horizontal, total, x, y,
// z; default total). Each block is Analysed, Detected and matched against that label's REF
// times only. One line per bone x measure: R, P, error and the edits (missed + extra +
// matched beyond 16 ms), with the threshold Analyse proposed. A label with no bone (or a bone
// the dump lacks) is listed as skipped. The TOTAL lines give, per measure, the share of
// bone x clip with at most 3 edits (the spike's gate counts edits per bone per clip, EditsOf
// in footstep_measure.h), then the same once more with each measure's offset moved by minus
// its median signed error (a steady lead or lag that one offset fixes is not counted).
//   --window=<ms>    Stillness's window (default 150), --approach=<ms> Relative drop's (300).
//   The blocks use the knobs they share with Footsteps: --height (the level rule's share of
//   the gap, which Stillness and Relative drop take), --margin-ratio, --floor-pct, --smooth,
//   --cooldown, --hold, --offset, --sensitivity, --edge. Speed's "below" threshold is
//   Analyse's speed percentile (30). --json does not apply.
//
// The CSV: '# item=<label>', '# rate_hz=<hz>', '# visible=<lo>,<hi>', '# ref=<t>,<t>...',
// from spike 10-7a '# ref.<label>=<t>,<t>...' and '# ref_bone.<label>=<bone>' per labelled
// bone (ref_labels.h; absent from older dumps), '# parent=...' (ignored), then the header
// 't,<bone>.x,<bone>.y,<bone>.z,...' and one row per sample (metres, model Y up).
// The Footsteps measure uses every REF time; each item's report adds its "REF by bone" line.
// Positions only: a rotation condition (q=rot, 10-4 follow-up) cannot be evaluated from a
// dump. This tool only runs the built-in Footsteps preset, which has no rotation condition;
// MeasureFootsteps keeps a defensive skip for one, and any rotation signal on position-only
// tracks evaluates empty and never runs.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "footstep_measure.h"
#include "ref_labels.h"
#include "tagging_signal.h"

using namespace rav;

namespace {

std::vector<std::string> Split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        const size_t b = s.find(sep, a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

bool ToDouble(const std::string& s, double* out)
{
    if (s.empty()) return false;
    char* end = nullptr;
    *out = std::strtod(s.c_str(), &end);
    return end && *end == '\0';
}

struct Clip {
    std::string            label;
    double                 rate = 240.0;
    bool                   has_visible = false;
    double                 lo = 0.0, hi = 0.0;
    RefTimes               refs;
    std::vector<std::string> names;
    std::vector<BoneTrack> tracks;
};

bool ReadClip(const std::string& path, Clip& c, std::string* err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *err = "cannot open";
        return false;
    }
    const size_t slash = path.find_last_of("/\\");
    c.label = slash == std::string::npos ? path : path.substr(slash + 1);
    std::string line;
    bool header = false;
    size_t row = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            std::string kv = line.substr(1);
            while (!kv.empty() && kv[0] == ' ') kv.erase(0, 1);
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (k == "item") {
                c.label = v;
            } else if (k == "rate_hz") {
                if (!ToDouble(v, &c.rate) || !(c.rate > 0.0)) {
                    *err = "bad rate_hz";
                    return false;
                }
            } else if (k == "visible") {
                const auto p = Split(v, ',');
                c.has_visible = p.size() == 2 && ToDouble(p[0], &c.lo) && ToDouble(p[1], &c.hi);
            } else {
                ReadRefHeader(k, v, &c.refs);  // ref, ref.<label>, ref_bone.<label>; others ignored
            }
            continue;
        }
        const auto cols = Split(line, ',');
        if (!header) {
            if (cols.empty() || cols[0] != "t" || (cols.size() - 1) % 3 != 0) {
                *err = "bad header (expected t,<bone>.x,<bone>.y,<bone>.z,...)";
                return false;
            }
            for (size_t i = 1; i < cols.size(); i += 3) {
                const std::string& n = cols[i];
                c.names.push_back(n.size() > 2 && n.compare(n.size() - 2, 2, ".x") == 0 ? n.substr(0, n.size() - 2) : n);
            }
            c.tracks.assign(c.names.size(), BoneTrack{});
            for (BoneTrack& t : c.tracks) t.rate_hz = c.rate;
            header = true;
            continue;
        }
        ++row;
        if (cols.size() != 1 + 3 * c.names.size()) {
            *err = "row " + std::to_string(row) + ": wrong column count";
            return false;
        }
        for (size_t b = 0; b < c.names.size(); ++b) {
            Vec3d p;
            if (!ToDouble(cols[1 + 3 * b], &p.x) || !ToDouble(cols[2 + 3 * b], &p.y) ||
                !ToDouble(cols[3 + 3 * b], &p.z)) {
                *err = "row " + std::to_string(row) + ": not a number";
                return false;
            }
            c.tracks[b].pos.push_back(p);
        }
    }
    if (!header || c.tracks.empty() || c.tracks[0].pos.empty()) {
        *err = "no samples";
        return false;
    }
    if (!c.has_visible) {
        c.lo = 0.0;
        c.hi = static_cast<double>(c.tracks[0].pos.size() - 1) / c.rate;
    }
    return true;
}

// --name=value into the params. False on an unknown name or a bad value.
bool SetParam(const std::string& name, const std::string& value, FootstepsParams& p)
{
    if (name == "floor") {
        if (value == "bone") p.per_bone_floor = true;
        else if (value == "foot") p.per_bone_floor = false;
        else return false;
        return true;
    }
    double v;
    if (!ToDouble(value, &v)) return false;
    if (name == "height") p.height_fraction = v / 100.0;
    else if (name == "knee") p.knee_fraction = v;
    else if (name == "yaw") p.yaw_limit_dps = v;
    else if (name == "yaw-margin") p.yaw_margin_dps = v;
    else if (name == "hold") p.min_hold_ms = v;
    else if (name == "cooldown") p.cooldown_ms = v;
    else if (name == "offset") p.offset_ms = v;
    else if (name == "floor-pct") p.floor_percentile = v;
    else if (name == "margin-ratio") p.margin_ratio = v;
    else if (name == "smooth") p.smooth_ms = v;
    else if (name == "sensitivity") p.sensitivity = v;
    else if (name == "edge") p.edge_margin_ms = v;
    else return false;
    return true;
}

// ---- --single (spike 10-7a, step 1) ----------------------------------------------------------

struct SingleOptions {
    Axis   axis = Axis::Total;
    double window_ms = 0.0;    // Stillness (0 = its default)
    double approach_ms = 0.0;  // Relative drop (0 = its default)
};

constexpr Measure kSingleMeasures[] = {Measure::Speed, Measure::Stillness, Measure::RelativeDrop};
constexpr size_t  kSingleCount = sizeof(kSingleMeasures) / sizeof(kSingleMeasures[0]);

bool ReadAxis(const std::string& v, Axis* out)
{
    static const struct {
        const char* word;
        Axis        axis;
    } kAxes[] = {{"vertical", Axis::Vertical}, {"horizontal", Axis::Horizontal}, {"total", Axis::Total},
                 {"x", Axis::X},               {"y", Axis::Y},                   {"z", Axis::Z}};
    for (const auto& a : kAxes)
        if (v == a.word) {
            *out = a.axis;
            return true;
        }
    return false;
}

// One block: `measure` of track 0 below a threshold Analyse proposes.
Block SingleBlock(Measure measure, const std::string& marker, const SingleOptions& so, const FootstepsParams& prm)
{
    Condition c;
    c.signal.quantity = Quantity::Point;
    c.signal.bones = {0};
    c.signal.reference = Reference::Floor;
    c.signal.measure = measure;
    c.signal.axis = so.axis;
    c.signal.window_ms = measure == Measure::Stillness ? so.window_ms
                         : measure == Measure::RelativeDrop ? so.approach_ms
                                                            : 0.0;
    c.dir = Direction::Below;
    Block b;
    b.marker = marker;
    b.conditions = {c};
    b.min_hold_ms = prm.min_hold_ms;
    b.cooldown_ms = prm.cooldown_ms;
    b.offset_ms = prm.offset_ms;
    return b;
}

std::string Pad(std::string s, size_t w)
{
    if (s.size() < w) s.append(w - s.size(), ' ');
    return s;
}

// One labelled bone of one clip: its track, its REF times and the visible part.
struct SingleJob {
    std::string            label;
    std::vector<BoneTrack> tracks;
    std::vector<double>    refs;
    double                 lo = 0.0, hi = 0.0;
};

// `measure` alone on a job, its block offset moved by shift_ms: Analyse, Detect, match against
// the job's REFs. `analysed` (optional) gets the condition as Analysed.
MatchResult RunJob(const SingleJob& j, Measure measure, double shift_ms, const SingleOptions& so,
                   const FootstepsParams& prm, const AnalyseOptions& ao, const DetectOptions& dopt,
                   Condition* analysed = nullptr)
{
    Block b = SingleBlock(measure, j.label, so, prm);
    b.offset_ms += shift_ms;
    const std::vector<Block> blocks = Analyse({b}, j.tracks, ao);
    if (analysed) *analysed = blocks[0].conditions[0];
    std::vector<double> det;
    for (const Event& e : Detect(blocks, j.tracks, dopt)) det.push_back(e.time_s);
    return MatchEvents(j.refs, det, kMatchWindowS, j.lo, j.hi);
}

double Median(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t m = v.size() / 2;
    return (v.size() % 2) ? v[m] : 0.5 * (v[m - 1] + v[m]);
}

int RunSingle(const std::vector<std::string>& files, const SingleOptions& so, const FootstepsParams& prm)
{
    const AnalyseOptions ao = FootstepsAnalyseOptions(prm);
    const DetectOptions  dopt = FootstepsDetectOptions(prm);
    {
        SignalSpec st, dr;
        st.measure = Measure::Stillness;
        st.window_ms = so.window_ms;
        dr.measure = Measure::RelativeDrop;
        dr.window_ms = so.approach_ms;
        std::printf("=== detection_eval --single ===\nEach measure alone per labelled bone: floor reference, below, axis "
                    "%s, Analyse (level share %g%%, speed percentile %g), stillness window %g ms, approach %g ms, "
                    "cooldown %g ms; match window +-%g ms, an edit = missed, extra or off by > %g ms\n",
                    AxisLabel(so.axis), ao.position_fraction * 100.0, ao.speed_percentile, MeasureWindowMs(st),
                    MeasureWindowMs(dr), prm.cooldown_ms, kMatchWindowS * 1000.0, kGateMaxErrS * 1000.0);
    }
    std::vector<SingleJob>   jobs;
    std::vector<MatchResult> per[kSingleCount];
    int                      good[kSingleCount] = {};
    int                      edits_sum[kSingleCount] = {};
    int                      skipped_labels = 0;
    for (size_t f = 0; f < files.size(); ++f) {
        Clip        c;
        std::string err;
        const bool  ok = ReadClip(files[f], c, &err);
        const std::string name = c.label.empty() ? files[f] : c.label;
        const std::string head = "#" + std::to_string(f + 1) + " " + name;
        if (!ok) {
            std::printf("%s  skipped: could not read %s: %s\n", head.c_str(), files[f].c_str(), err.c_str());
            continue;
        }
        if (c.refs.by_label.empty()) {
            std::printf("%s  skipped: no labelled REF ('# ref.<label>=') in the file\n", head.c_str());
            continue;
        }
        std::printf("%s\n", head.c_str());
        for (const auto& kv : c.refs.by_label) {
            const std::string& label = kv.first;
            const auto         bi = c.refs.bone.find(label);
            const std::string  bone = bi == c.refs.bone.end() ? std::string() : bi->second;
            size_t             track = c.names.size();
            for (size_t i = 0; i < c.names.size() && track == c.names.size(); ++i)
                if (c.names[i] == bone) track = i;
            if (bone.empty() || track == c.names.size()) {
                ++skipped_labels;
                std::printf("  %s  REF %zu  skipped: %s\n", label.c_str(), kv.second.size(),
                            bone.empty() ? "no bone for this label on this skeleton"
                                         : ("bone " + bone + " is not in the dump").c_str());
                continue;
            }
            std::printf("  %s (%s)\n", label.c_str(), bone.c_str());
            SingleJob job;
            job.label = label;
            job.tracks = {c.tracks[track]};
            job.refs = kv.second;
            job.lo = c.lo;
            job.hi = c.hi;
            jobs.push_back(job);
            for (size_t k = 0; k < kSingleCount; ++k) {
                Condition         cd;
                const MatchResult m = RunJob(job, kSingleMeasures[k], 0.0, so, prm, ao, dopt, &cd);
                const Edits       ed = EditsOf(m);
                per[k].push_back(m);
                edits_sum[k] += ed.total();
                if (WithinEditGate(ed)) ++good[k];
                std::printf("    %s %s  edits %d (missed %d, extra %d, off %d)  [below %s, margin %s]\n",
                            Pad(MeasureLabel(kSingleMeasures[k]), 13).c_str(), MatchText(m).c_str(), ed.total(),
                            ed.missed, ed.extra, ed.off, FormatDisplay(cd.signal, cd.threshold).c_str(),
                            FormatDisplay(cd.signal, cd.margin).c_str());
            }
        }
    }
    for (size_t k = 0; k < kSingleCount; ++k) {
        const size_t n = per[k].size();
        std::printf("TOTAL %s bone x clip %zu: %d with <= %d edits (%.1f%%), edits %d  %s\n",
                    Pad(MeasureLabel(kSingleMeasures[k]), 13).c_str(), n, good[k], kGateEdits,
                    n ? 100.0 * good[k] / static_cast<double>(n) : 0.0, edits_sum[k],
                    n ? MatchText(SumMatches(per[k])).c_str() : "nothing measured");
    }
    // A steady lead or lag is what one offset fixes (Stillness comes ahead of the contact by
    // design): each measure again, its offset moved by minus its median signed error.
    for (size_t k = 0; k < kSingleCount && !jobs.empty(); ++k) {
        const double             shift_ms = -1000.0 * Median(SumMatches(per[k]).match_err_s);
        std::vector<MatchResult> shifted;
        int                      good2 = 0, edits2 = 0;
        for (const SingleJob& j : jobs) {
            const MatchResult m = RunJob(j, kSingleMeasures[k], shift_ms, so, prm, ao, dopt);
            const Edits       ed = EditsOf(m);
            shifted.push_back(m);
            edits2 += ed.total();
            if (WithinEditGate(ed)) ++good2;
        }
        std::printf("TOTAL %s offset %+.0f ms: %d with <= %d edits (%.1f%%), edits %d  %s\n",
                    Pad(MeasureLabel(kSingleMeasures[k]), 13).c_str(), prm.offset_ms + shift_ms, good2, kGateEdits,
                    100.0 * good2 / static_cast<double>(jobs.size()), edits2, MatchText(SumMatches(shifted)).c_str());
    }
    if (skipped_labels) std::printf("(%d label%s skipped)\n", skipped_labels, skipped_labels == 1 ? "" : "s");
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    FootstepsParams prm;
    bool json = false;
    bool single = false;
    bool single_only = false;  // --axis / --window / --approach given
    SingleOptions so;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const size_t      eq = a.find('=');
        const std::string key = eq == std::string::npos ? a : a.substr(0, eq);
        const std::string val = eq == std::string::npos ? std::string() : a.substr(eq + 1);
        double            ms = 0.0;
        if (a == "--json") {
            json = true;
        } else if (a == "--single") {
            single = true;
        } else if (key == "--axis" || key == "--window" || key == "--approach") {
            const bool good = key == "--axis" ? ReadAxis(val, &so.axis)
                                              : (ToDouble(val, &ms) && std::isfinite(ms) && ms > 0.0);
            if (!good) {
                std::fprintf(stderr, "detection_eval: bad option %s\n", a.c_str());
                return 2;
            }
            if (key == "--window") so.window_ms = ms;
            if (key == "--approach") so.approach_ms = ms;
            single_only = true;
        } else if (a.compare(0, 2, "--") == 0) {
            if (eq == std::string::npos || !SetParam(key.substr(2), val, prm)) {
                std::fprintf(stderr, "detection_eval: bad option %s\n", a.c_str());
                return 2;
            }
        } else {
            files.push_back(a);
        }
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: detection_eval [--param=value ...] [--json] file.csv...\n"
                             "       detection_eval --single [--axis=<axis>] [--window=<ms>] [--approach=<ms>] "
                             "[--param=value ...] file.csv...\n");
        return 2;
    }
    if (single_only && !single) {
        std::fprintf(stderr, "detection_eval: --axis, --window and --approach need --single\n");
        return 2;
    }
    if (single && json) {
        std::fprintf(stderr, "detection_eval: --json does not apply to --single\n");
        return 2;
    }
    if (single) return RunSingle(files, so, prm);

    if (!json)
        std::printf("=== detection_eval ===\nFootsteps preset + Analyse, match window +-%g ms, source time\n",
                    kMatchWindowS * 1000.0);
    std::vector<MatchResult> measured;
    for (size_t f = 0; f < files.size(); ++f) {
        Clip c;
        std::string err;
        const std::string label = "#" + std::to_string(f + 1) + " ";
        ItemMeasure m;
        if (!ReadClip(files[f], c, &err)) {
            m.skipped = "could not read " + files[f] + ": " + err;
        } else if (c.refs.all.empty()) {
            m.skipped = "no REF times in the file";
        } else {
            m = MeasureFootsteps(
                c.names,
                [&](const std::vector<int>& idx) {
                    std::vector<BoneTrack> t;
                    for (int i : idx) t.push_back(c.tracks.at(static_cast<size_t>(i)));
                    return t;
                },
                c.refs.all, c.lo, c.hi, prm);
        }
        const std::string name = c.label.empty() ? files[f] : c.label;
        if (m.skipped.empty()) measured.push_back(m.match);
        if (json) {
            std::printf("%s\n", ItemJson(name, m).c_str());
        } else if (!m.skipped.empty()) {
            std::printf("%s%s", SkippedReport(label + name, m.skipped).c_str(), RefLabelsText(c.refs).c_str());
        } else {
            std::printf("%s%s", ItemReport(label + name, m, c.refs.all, "").c_str(), RefLabelsText(c.refs).c_str());
        }
    }
    if (json) {
        ItemMeasure total;
        total.match = SumMatches(measured);
        if (measured.empty()) total.skipped = "nothing measured";
        std::printf("%s\n", ItemJson("TOTAL", total).c_str());
    } else {
        std::printf("%s", TotalReport(measured, prm).c_str());
    }
    return 0;
}
