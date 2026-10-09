// SPDX-License-Identifier: MIT
//
// Offline evaluator for footstep detection (Epic 10, spike 10-0). Runs the real engine
// (footstep_measure.h: the same code as "RAV: Measure detection against reference
// markers") on the bone-track dumps that action writes to RAV_detection_dump/*.csv, and
// prints the same report. For tuning FootstepsParams on tagged clips without REAPER.
//
//   detection_eval [--param=value ...] [--json] file.csv...
//   detection_eval --single [--axis=<axis>] [--window=<ms>] [--approach=<ms>] [--param=value ...] file.csv...
//   detection_eval --physics [--contact=<leg/s>] [--switch=<cost>] file.csv...
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
// --physics (spike 10-8a): the foot events of motion_physics.h, with no Analyse. Roles are
// guessed from the bone names (GuessRoleMapping). Per clip: the leg length and the ground
// velocity; per labelled foot part (a label whose bone plays the heel, toe or toe end of a
// side; any other label is listed as skipped), one line per step timing definition: the steps
// of that part, matched against the label's REFs, with the edits and the signed errors. Each
// definition gets one offset per part, chosen leave-one-clip-out: a clip is scored with minus
// the median signed error of that part on the OTHER clips (errors from a +-300 ms match). Per
// part, the definition with the fewest edits (then the lowest median |error|) is chosen. Then
// one combined-step line per foot (the foot's first part to touch, chosen timing), scored
// against that foot's visible REFs merged within 150 ms (the earliest kept). TOTAL lines per part x
// definition (with the offsets), the chosen definitions, the combined total and the GO line
// (missed + extra <= 3 and median |error| <= 16 ms over every labelled foot part: the timing
// chosen on all clips, the offsets leave-one-clip-out; a foot label whose part is missing on the
// rig, or whose clip's physics failed, counts its REFs as missed). An offset no error could
// estimate (a part labelled in one clip only) prints n/a and is 0. Last, every clip's events
// (untagged dumps too) with the module's default timing, as the measure action writes them (PHY
// take markers).
//   --contact=<leg/s>  the contact speed (default 0.4), --switch=<cost> the switch cost (10).
//   --json, --single and the Footsteps knobs do not apply.
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

#include "bone_roles.h"
#include "footstep_measure.h"
#include "motion_physics.h"
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


// ---- --physics (spike 10-8a) ---------------------------------------------------------------

// The +-window used to estimate a definition's steady lead or lag (wider than the match window,
// so a large lag is still measured).
constexpr double kOffsetWindowS = 0.300;
constexpr double kGateMedianErrS = 0.016;
constexpr int    kGateMissedExtra = 3;

// One labelled REF list of one clip, read as a foot part.
struct PhysLabel {
    std::string         label;
    std::string         bone;
    char                side = 'L';
    bool                foot = false;  // the bone plays a foot part of `side`
    int                 part = -1;     // -1 = skipped (not a foot part, or the part is missing)
    std::string         skip;
    std::vector<double> refs;
};

struct PhysClip {
    std::string            head;  // "#k <name>"
    bool                   read = false;
    std::string            err;
    double                 lo = 0.0, hi = 0.0;
    PhysicsAnalysis        a;
    std::vector<PhysLabel> labels;
    bool                   tagged = false;  // some label is a foot part
};

std::string ErrText(const std::vector<double>& e)
{
    std::string out;
    char        buf[32];
    for (double x : e) {
        std::snprintf(buf, sizeof(buf), "%s%+.0f", out.empty() ? "" : " ", x * 1000.0);
        out += buf;
    }
    return out.empty() ? "-" : out;
}

std::string EditsText(const Edits& e)
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "edits %d (missed %d, extra %d, off %d)", e.total(), e.missed, e.extra, e.off);
    return buf;
}

void AddEdits(Edits& to, const Edits& e)
{
    to.missed += e.missed;
    to.extra += e.extra;
    to.off += e.off;
}

// An offset in ms, or "n/a" when no error was there to estimate it (0 is used).
std::string OffsetText(double s, bool estimated)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%+.0f", s * 1000.0);
    return estimated ? buf : "n/a";
}

// The median, +infinity when empty (no matched step never wins a tie).
double MedianOrInf(const std::vector<double>& v)
{
    return v.empty() ? HUGE_VAL : Median(v);
}

std::vector<double> AbsMs(const std::vector<double>& e)
{
    std::vector<double> out;
    for (double x : e) out.push_back(std::fabs(x) * 1000.0);
    return out;
}

// That foot's REFs inside the visible part (every label read as a part of this side), merged
// within the match window: a REF within 150 ms of the last one kept is dropped (the earliest stays).
std::vector<double> MergedFootRefs(const PhysClip& c, char side)
{
    std::vector<double> all;
    for (const PhysLabel& l : c.labels)
        if (l.foot && l.side == side)
            for (double t : l.refs)
                if (t >= c.lo && t <= c.hi) all.push_back(t);  // a hidden REF never merges a visible one away
    std::sort(all.begin(), all.end());
    std::vector<double> out;
    for (double t : all)
        if (out.empty() || t - out.back() > kMatchWindowS) out.push_back(t);
    return out;
}

int RunPhysics(const std::vector<std::string>& files, const PhysicsParams& base)
{
    std::printf("=== detection_eval --physics ===\nFoot events from body physics (motion_physics.h): contact %g leg/s, "
                "switch cost %g, smoothing %g ms, no Analyse; match window +-%g ms, an edit = missed, extra or off by "
                "> %g ms\n",
                base.contact_speed, base.switch_cost, base.smooth_ms, kMatchWindowS * 1000.0, kGateMaxErrS * 1000.0);
    std::printf("Step timing definitions (one offset per part, leave-one-clip-out):\n");
    for (int d = 0; d < kStepTimingCount; ++d)
        std::printf("  %-8s %s\n", StepTimingName(static_cast<StepTiming>(d)), StepTimingHint(static_cast<StepTiming>(d)));

    // Read and analyse every clip.
    std::vector<PhysClip> clips(files.size());
    for (size_t f = 0; f < files.size(); ++f) {
        PhysClip& pc = clips[f];
        Clip      c;
        pc.read = ReadClip(files[f], c, &pc.err);
        pc.head = "#" + std::to_string(f + 1) + " " + (c.label.empty() ? files[f] : c.label);
        if (!pc.read) continue;
        pc.lo = c.lo;
        pc.hi = c.hi;
        const std::vector<int> role_bone = GuessRoleMapping(c.names);
        pc.a = AnalyseMotion(PhysicsRoleTracks(role_bone,
                                               [&](const std::vector<int>& idx) {
                                                   std::vector<BoneTrack> t;
                                                   for (int i : idx) t.push_back(c.tracks.at(static_cast<size_t>(i)));
                                                   return t;
                                               }),
                             base);
        for (const auto& kv : c.refs.by_label) {
            PhysLabel l;
            l.label = kv.first;
            l.refs = kv.second;
            const auto bi = c.refs.bone.find(kv.first);
            l.bone = bi == c.refs.bone.end() ? std::string() : bi->second;
            int bone = -1;
            for (size_t i = 0; i < c.names.size() && bone < 0; ++i)
                if (!l.bone.empty() && c.names[i] == l.bone) bone = static_cast<int>(i);
            for (int s = 0; s < 2 && l.part < 0; ++s)
                for (int q = 0; q < kFootPartCount && l.part < 0; ++q) {
                    const Role r = FootPartRole(s ? 'R' : 'L', q);
                    if (bone >= 0 && role_bone[static_cast<size_t>(r)] == bone) {
                        l.side = s ? 'R' : 'L';
                        l.part = q;
                    }
                }
            Role r;
            if (l.part < 0 && l.bone.empty() && RoleFromKey(kv.first, &r))
                for (int s = 0; s < 2 && l.part < 0; ++s)
                    for (int q = 0; q < kFootPartCount && l.part < 0; ++q)
                        if (FootPartRole(s ? 'R' : 'L', q) == r) {
                            l.side = s ? 'R' : 'L';
                            l.part = q;
                        }
            l.foot = l.part >= 0;
            if (l.part < 0) {
                l.skip = "not a foot part (heel, toe or toe end)";
            } else if (!pc.a.ok) {
                l.skip = "physics skipped: " + pc.a.error;
                l.part = -1;
            } else if (!pc.a.foot[l.side == 'R' ? 1 : 0].part[l.part].present) {
                l.skip = std::string(1, l.side) + " " + FootPartWord(l.part) + ": " +
                         pc.a.foot[l.side == 'R' ? 1 : 0].part[l.part].missing;
                l.part = -1;
            }
            if (l.part >= 0) pc.tagged = true;
            pc.labels.push_back(l);
        }
    }

    // Per part x definition: the signed errors per clip (offset 0, +-300 ms), then each clip's
    // leave-one-clip-out offset, and the all-clip offset.
    const size_t        nc = clips.size();
    std::vector<double> loo[kFootPartCount][kStepTimingCount];      // per clip (s)
    std::vector<char>   loo_est[kFootPartCount][kStepTimingCount];  // per clip: estimated from an error
    double              all_off[kFootPartCount][kStepTimingCount] = {};
    bool                all_est[kFootPartCount][kStepTimingCount] = {};
    bool                has_part[kFootPartCount] = {};
    for (int q = 0; q < kFootPartCount; ++q)
        for (int d = 0; d < kStepTimingCount; ++d) {
            std::vector<std::vector<double>> errs(nc);
            std::vector<double>              pooled;
            for (size_t ci = 0; ci < nc; ++ci)
                for (const PhysLabel& l : clips[ci].labels) {
                    if (l.part != q) continue;
                    has_part[q] = true;
                    const MatchResult m = MatchEvents(
                        l.refs, PartStepTimes(clips[ci].a, l.side, q, static_cast<StepTiming>(d), 0.0), kOffsetWindowS,
                        clips[ci].lo, clips[ci].hi);
                    errs[ci].insert(errs[ci].end(), m.match_err_s.begin(), m.match_err_s.end());
                    pooled.insert(pooled.end(), m.match_err_s.begin(), m.match_err_s.end());
                }
            all_off[q][d] = 0.0 - Median(pooled);  // (never -0: it prints)
            all_est[q][d] = !pooled.empty();
            loo[q][d].assign(nc, 0.0);
            loo_est[q][d].assign(nc, 0);
            for (size_t ci = 0; ci < nc; ++ci) {
                std::vector<double> others;
                for (size_t cj = 0; cj < nc; ++cj)
                    if (cj != ci) others.insert(others.end(), errs[cj].begin(), errs[cj].end());
                loo[q][d][ci] = 0.0 - Median(others);  // 0 when no other clip matched
                loo_est[q][d][ci] = !others.empty();
            }
        }

    // Each label scored by each definition with its clip's offset; the totals; the choice.
    auto score = [&](size_t ci, const PhysLabel& l, int d) {
        return MatchEvents(l.refs,
                           PartStepTimes(clips[ci].a, l.side, l.part, static_cast<StepTiming>(d), loo[l.part][d][ci]),
                           kMatchWindowS, clips[ci].lo, clips[ci].hi);
    };
    Edits               tot[kFootPartCount][kStepTimingCount];
    std::vector<double> abs_err[kFootPartCount][kStepTimingCount];
    for (size_t ci = 0; ci < nc; ++ci)
        for (const PhysLabel& l : clips[ci].labels) {
            if (l.part < 0) continue;
            for (int d = 0; d < kStepTimingCount; ++d) {
                const MatchResult m = score(ci, l, d);
                AddEdits(tot[l.part][d], EditsOf(m));
                const std::vector<double> ae = AbsMs(m.match_err_s);
                abs_err[l.part][d].insert(abs_err[l.part][d].end(), ae.begin(), ae.end());
            }
        }
    int chosen[kFootPartCount];
    for (int q = 0; q < kFootPartCount; ++q) {
        chosen[q] = static_cast<int>(base.step_timing[q]);
        if (!has_part[q]) continue;
        chosen[q] = 0;
        for (int d = 1; d < kStepTimingCount; ++d) {
            const int a = tot[q][d].total(), b = tot[q][chosen[q]].total();
            if (a < b || (a == b && MedianOrInf(abs_err[q][d]) < MedianOrInf(abs_err[q][chosen[q]]))) chosen[q] = d;
        }
    }
    // The params a clip is scored with: the chosen definitions and its leave-one-clip-out offsets.
    auto clip_params = [&](size_t ci) {
        PhysicsParams pp = base;
        for (int q = 0; q < kFootPartCount; ++q) {
            pp.step_timing[q] = static_cast<StepTiming>(chosen[q]);
            pp.step_offset_s[q] = has_part[q] ? loo[q][chosen[q]][ci] : base.step_offset_s[q];
        }
        return pp;
    };

    // Per clip.
    Edits               gate, foot_tot;
    std::vector<double> gate_err, foot_err;
    int                 gate_refs = 0, foot_refs = 0, skipped = 0;
    // A skipped label is listed; a foot part's REFs (inside the visible part) count as missed in
    // the gate: its part is missing on the rig, or the clip's physics failed.
    auto skip_label = [&](const PhysClip& pc, const PhysLabel& l) {
        ++skipped;
        std::printf("  %s  REF %zu  skipped: %s\n", l.label.c_str(), l.refs.size(), l.skip.c_str());
        if (!l.foot) return;
        for (double t : l.refs)
            if (t >= pc.lo && t <= pc.hi) {
                ++gate.missed;
                ++gate_refs;
            }
    };
    for (size_t ci = 0; ci < nc; ++ci) {
        const PhysClip& pc = clips[ci];
        if (!pc.read) {
            std::printf("%s  skipped: could not read %s: %s\n", pc.head.c_str(), files[ci].c_str(), pc.err.c_str());
            continue;
        }
        if (!pc.a.ok) {
            std::printf("%s  physics skipped: %s\n", pc.head.c_str(), pc.a.error.c_str());
            for (const PhysLabel& l : pc.labels) skip_label(pc, l);
            continue;
        }
        std::printf("%s  leg %.3f m%s, ground (%+.2f, %+.2f) m/s%s\n", pc.head.c_str(), pc.a.leg_length,
                    pc.a.scale_note.empty() ? "" : (" (" + pc.a.scale_note + ")").c_str(), pc.a.ground_velocity.x,
                    pc.a.ground_velocity.z, pc.tagged ? "" : "  (no labelled foot REF: events only)");
        for (const std::string& m : pc.a.missing) std::printf("  missing: %s\n", m.c_str());
        for (const PhysLabel& l : pc.labels) {
            if (l.part < 0) {
                skip_label(pc, l);
                continue;
            }
            std::printf("  %s (%c %s, %s)  REF %zu: %s\n", l.label.c_str(), l.side, FootPartWord(l.part), l.bone.c_str(),
                        l.refs.size(), TimesText(l.refs).c_str());
            for (int d = 0; d < kStepTimingCount; ++d) {
                const MatchResult m = score(ci, l, d);
                const Edits       e = EditsOf(m);
                std::printf("    %s%-8s offset %4s ms  steps %d  %s  err %s ms\n", d == chosen[l.part] ? "*" : " ",
                            StepTimingName(static_cast<StepTiming>(d)),
                            OffsetText(loo[l.part][d][ci], loo_est[l.part][d][ci] != 0).c_str(), m.n_det,
                            EditsText(e).c_str(), ErrText(m.match_err_s).c_str());
                if (d == chosen[l.part]) {
                    AddEdits(gate, e);
                    gate_refs += m.n_ref;
                    gate_err.insert(gate_err.end(), m.match_err_s.begin(), m.match_err_s.end());
                }
            }
        }
        if (!pc.tagged) continue;
        const std::vector<PhysicsEvent> ev = FootEvents(pc.a, clip_params(ci));
        for (char side : {'L', 'R'}) {
            const std::vector<double> refs = MergedFootRefs(pc, side);
            if (refs.empty()) continue;
            std::vector<double> det;
            for (const PhysicsEvent& e : ev)
                if (e.kind == PhysicsKind::FootStep && e.side == side) det.push_back(e.time_s);
            const MatchResult m = MatchEvents(refs, det, kMatchWindowS, pc.lo, pc.hi);
            const Edits       e = EditsOf(m);
            AddEdits(foot_tot, e);
            foot_refs += m.n_ref;
            foot_err.insert(foot_err.end(), m.match_err_s.begin(), m.match_err_s.end());
            std::printf("  %c foot step (combined)  REF %d (merged): %s  steps %d  %s  err %s ms\n", side, m.n_ref,
                        TimesText(refs).c_str(), m.n_det, EditsText(e).c_str(), ErrText(m.match_err_s).c_str());
        }
    }

    // Totals.
    for (int q = 0; q < kFootPartCount; ++q) {
        if (!has_part[q]) {
            std::printf("TOTAL %-4s no labelled REF\n", FootPartWord(q));
            continue;
        }
        for (int d = 0; d < kStepTimingCount; ++d) {
            std::string offs;
            for (size_t ci = 0; ci < nc; ++ci) {
                bool here = false;
                for (const PhysLabel& l : clips[ci].labels)
                    if (l.part == q) here = true;
                if (!here) continue;
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%s#%zu %s", offs.empty() ? "" : ", ", ci + 1,
                              OffsetText(loo[q][d][ci], loo_est[q][d][ci] != 0).c_str());
                offs += buf;
            }
            std::printf("TOTAL %-4s %s%-8s %s  |err| median %.1f ms  leave-one-clip-out offsets (ms): %s; all clips %s ms\n",
                        FootPartWord(q), d == chosen[q] ? "*" : " ", StepTimingName(static_cast<StepTiming>(d)),
                        EditsText(tot[q][d]).c_str(), Median(abs_err[q][d]), offs.c_str(),
                        OffsetText(all_off[q][d], all_est[q][d]).c_str());
        }
    }
    std::string ch;
    for (int q = 0; q < kFootPartCount; ++q) {
        char buf[96];
        if (has_part[q])
            std::snprintf(buf, sizeof(buf), "%s%s %s (all clips %s ms)", ch.empty() ? "" : ", ", FootPartWord(q),
                          StepTimingName(static_cast<StepTiming>(chosen[q])),
                          OffsetText(all_off[q][chosen[q]], all_est[q][chosen[q]]).c_str());
        else
            std::snprintf(buf, sizeof(buf), "%s%s no REF", ch.empty() ? "" : ", ", FootPartWord(q));
        ch += buf;
    }
    std::printf("CHOSEN %s\n", ch.c_str());
    std::printf("TOTAL foot step (combined)  REF %d  %s  |err| median %.1f ms\n", foot_refs, EditsText(foot_tot).c_str(),
                Median(AbsMs(foot_err)));
    const double med = Median(AbsMs(gate_err));
    const bool   go = gate_refs > 0 && !gate_err.empty() && gate.missed + gate.extra <= kGateMissedExtra &&
                    med <= kGateMedianErrS * 1000.0;
    char med_text[32];
    std::snprintf(med_text, sizeof(med_text), gate_err.empty() ? "n/a" : "%.1f ms", med);
    std::printf("GO gate (separate steps, timing chosen on all clips, offsets leave-one-clip-out): REF %d, missed + "
                "extra %d (<= %d), off %d, median |err| %s (<= %.0f ms): %s%s\n",
                gate_refs, gate.missed + gate.extra, kGateMissedExtra, gate.off, med_text, kGateMedianErrS * 1000.0,
                go ? "GO" : "NO-GO", go ? " (the dance clip still has to be judged)" : "");
    if (skipped) std::printf("(%d label%s skipped)\n", skipped, skipped == 1 ? "" : "s");

    // Every clip's events, as the measure action writes them.
    std::printf("EVENTS (default timing: %s; strengths in leg/s, pivots in degrees)\n", StepTimingText(base).c_str());
    for (const PhysClip& pc : clips) {
        if (!pc.read || !pc.a.ok) continue;
        const std::vector<PhysicsEvent> ev = FootEvents(pc.a, base);
        std::printf("%s  %s\n%s", pc.head.c_str(), PhysicsSummary(pc.a, ev).c_str(), PhysicsEventLines(ev, "  ").c_str());
    }
    return 0;
}
}  // namespace

int main(int argc, char** argv)
{
    FootstepsParams prm;
    bool json = false;
    bool single = false;
    bool single_only = false;  // --axis / --window / --approach given
    bool physics = false;
    bool physics_only = false;  // --contact / --switch given
    bool footsteps_knob = false;
    PhysicsParams pp;
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
        } else if (a == "--physics") {
            physics = true;
        } else if (key == "--contact" || key == "--switch") {
            if (!ToDouble(val, &ms) || !std::isfinite(ms) || !(ms > 0.0)) {
                std::fprintf(stderr, "detection_eval: bad option %s\n", a.c_str());
                return 2;
            }
            if (key == "--contact") pp.contact_speed = ms;
            else pp.switch_cost = ms;
            physics_only = true;
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
            footsteps_knob = true;
        } else {
            files.push_back(a);
        }
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: detection_eval [--param=value ...] [--json] file.csv...\n"
                             "       detection_eval --single [--axis=<axis>] [--window=<ms>] [--approach=<ms>] "
                             "[--param=value ...] file.csv...\n"
                             "       detection_eval --physics [--contact=<leg/s>] [--switch=<cost>] file.csv...\n");
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
    if (physics_only && !physics) {
        std::fprintf(stderr, "detection_eval: --contact and --switch need --physics\n");
        return 2;
    }
    if (physics && (json || single || footsteps_knob)) {
        std::fprintf(stderr, "detection_eval: --json, --single and the Footsteps knobs do not apply to --physics\n");
        return 2;
    }
    if (physics) return RunPhysics(files, pp);
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
