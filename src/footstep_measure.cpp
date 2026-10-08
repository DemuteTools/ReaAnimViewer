// SPDX-License-Identifier: MIT
//
// See footstep_measure.h.

#include "footstep_measure.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "bone_roles.h"

namespace rav {
namespace {

std::string Format(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

std::string JsonString(const std::string& s)
{
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            o += Format("\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
        } else {
            o += c;
        }
    }
    return o + "\"";
}

}  // namespace

ItemMeasure MeasureFootsteps(const std::vector<std::string>& bone_names, const TrackSampler& sample,
                             const std::vector<double>& refs, double lo, double hi, const FootstepsParams& prm)
{
    ItemMeasure r;
    const std::vector<int> role_bone = GuessRoleMapping(bone_names);
    const Preset preset = FootstepsPreset(prm);
    const std::vector<Role> used = RolesUsed(preset.blocks);
    std::vector<int> role_to_track(static_cast<size_t>(Role::Count), -1);
    std::vector<int> bone_indices;
    for (Role role : used) {
        const int b = role_bone[static_cast<size_t>(role)];
        if (b < 0) continue;
        role_to_track[static_cast<size_t>(role)] = static_cast<int>(bone_indices.size());
        bone_indices.push_back(b);
    }
    std::string missing;
    std::vector<Block> blocks = preset.blocks;
    if (!BindRoles(blocks, role_to_track, &missing)) {
        r.skipped = "roles not found: " + missing;
        return r;
    }
    const std::vector<BoneTrack> tracks = sample ? sample(bone_indices) : std::vector<BoneTrack>{};
    if (tracks.size() != bone_indices.size() || tracks.empty()) {
        r.skipped = "could not sample the animation";
        return r;
    }
    // 10-4 follow-up, defensive: a rotation condition needs the bones' orientations, which an
    // offline CSV dump (tests/detection_eval) does not hold. This path only runs the built-in
    // Footsteps preset, which has no rotation condition, so this never fires today; and any
    // rotation signal on position-only tracks evaluates empty and never runs anyway.
    for (const Block& b : blocks)
        for (const Condition& c : b.conditions)
            if (c.signal.quantity == Quantity::Rotation)
                for (int t : c.signal.bones) {
                    const size_t k = static_cast<size_t>(t);
                    if (t >= 0 && k < tracks.size() && tracks[k].rot_world.empty()) {
                        r.skipped = "rotation conditions are not supported here (the tracks hold positions only)";
                        return r;
                    }
                }
    r.blocks = Analyse(blocks, tracks, FootstepsAnalyseOptions(prm));
    r.events = Detect(r.blocks, tracks, FootstepsDetectOptions(prm));
    for (const Event& e : r.events) r.det.push_back(e.time_s);
    r.match = MatchEvents(refs, r.det, kMatchWindowS, lo, hi);
    return r;
}

std::string ParamsText(const FootstepsParams& p)
{
    return Format("height %g%% of gap, knee onset %g x p95, yaw limit %g deg/s, min hold %g ms, cooldown %g ms, "
                  "offset %+g ms, floor %s",
                  p.height_fraction * 100.0, p.knee_fraction, p.yaw_limit_dps, p.min_hold_ms, p.cooldown_ms,
                  p.offset_ms, p.per_bone_floor ? "per bone" : "per foot");
}

std::string MatchText(const MatchResult& m)
{
    return Format("REF %d  det %d  match %d  R %.1f%%  P %.1f%%  err mean %.1f ms max %.1f ms (lag %+.1f ms)", m.n_ref,
                  m.n_det, m.n_match, m.recall * 100.0, m.precision * 100.0, m.mean_abs_err_s * 1000.0,
                  m.max_abs_err_s * 1000.0, m.mean_signed_err_s * 1000.0);
}

std::string TimesText(const std::vector<double>& t)
{
    if (t.empty()) return "-";
    std::string s;
    const size_t cap = 24;
    for (size_t i = 0; i < t.size() && i < cap; ++i) s += Format(i ? " %.3f" : "%.3f", t[i]);
    if (t.size() > cap) s += Format(" ... (+%zu)", t.size() - cap);
    return s;
}

std::string ItemReport(const std::string& label, const ItemMeasure& m, const std::vector<double>& refs,
                       const std::string& note)
{
    std::string out = label + "  " + MatchText(m.match) + note + "\n";
    std::string th = " ";
    for (const Block& b : m.blocks) {
        if (b.conditions.size() < 3) continue;
        const Condition& h = b.conditions[0];
        std::string floor;
        if (h.signal.bone_floors.empty()) {
            floor = Format("%.3f", h.signal.floor_y);
        } else {  // heel / toe floors (+ the floor of their combined height, about 0)
            for (size_t f = 0; f < h.signal.bone_floors.size(); ++f)
                floor += Format(f ? "/%.3f" : "%.3f", h.signal.bone_floors[f]);
            floor += Format(" (+%.3f)", h.signal.floor_y);
        }
        th += Format(" %s: h=%.3f m (m=%.3f) floor=%s m  knee>%.0f deg/s  yaw<%.0f deg/s;", b.marker.c_str(),
                     h.threshold, h.margin, floor.c_str(), b.conditions[1].threshold, b.conditions[2].threshold);
    }
    out += th + "\n";
    out += "  unmatched REF: " + TimesText(m.match.unmatched_ref) + "  unmatched det: " +
           TimesText(m.match.unmatched_det) + "\n";
    static const char* const kCond[] = {"height", "knee", "yaw"};
    for (const Event& e : m.events) {
        out += Format("    det %.3f %s", e.time_s, e.marker.c_str());
        bool has = false;
        double best = 0.0;
        for (double t : refs)
            if (!has || std::fabs(t - e.time_s) < std::fabs(best - e.time_s)) {
                best = t;
                has = true;
            }
        if (!has) {
            out += "\n";
            continue;
        }
        out += Format("  nearest REF %.3f (%+.0f ms):", best, (e.time_s - best) * 1000.0);
        for (size_t c = 0; c < e.cond_entry_s.size(); ++c)
            out += Format(" %s %+.0f", c < 3 ? kCond[c] : "cond", (e.cond_entry_s[c] - best) * 1000.0);
        out += " ms\n";
    }
    return out;
}

std::string SkippedReport(const std::string& label, const std::string& reason)
{
    return label + "  skipped: " + reason + "\n";
}

Edits EditsOf(const MatchResult& m)
{
    Edits e;
    e.missed = static_cast<int>(m.unmatched_ref.size());
    e.extra = static_cast<int>(m.unmatched_det.size());
    for (double d : m.match_err_s)
        if (std::fabs(d) > kGateMaxErrS + 1e-9) ++e.off;
    return e;
}

std::string TotalReport(const std::vector<MatchResult>& measured, const FootstepsParams& prm)
{
    const MatchResult total = SumMatches(measured);
    const bool go = !measured.empty() && PassesAccuracyGate(total);
    std::string out = Format("TOTAL (%zu item%s)  ", measured.size(), measured.size() == 1 ? "" : "s");
    out += measured.empty() ? std::string("nothing measured") : MatchText(total);
    out += Format(" -> %s (NFR-AT1: R>=98%% P>=98%% max |err|<=16 ms)  [knobs used: %s]\n", go ? "GO" : "NO-GO",
                  ParamsText(prm).c_str());
    return out;
}

std::string ItemJson(const std::string& label, const ItemMeasure& m)
{
    if (!m.skipped.empty())
        return "{\"item\":" + JsonString(label) + ",\"skipped\":" + JsonString(m.skipped) + "}";
    const MatchResult& r = m.match;
    return "{\"item\":" + JsonString(label) +
           Format(",\"n_ref\":%d,\"n_det\":%d,\"n_match\":%d,\"recall\":%.6f,\"precision\":%.6f,"
                  "\"mean_err_ms\":%.3f,\"max_err_ms\":%.3f,\"lag_ms\":%.3f,\"go\":%s}",
                  r.n_ref, r.n_det, r.n_match, r.recall, r.precision, r.mean_abs_err_s * 1000.0,
                  r.max_abs_err_s * 1000.0, r.mean_signed_err_s * 1000.0, PassesAccuracyGate(r) ? "true" : "false");
}

}  // namespace rav
