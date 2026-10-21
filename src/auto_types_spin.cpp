// SPDX-License-Identifier: MIT
//
// Story 10-9f: the Body category's Spin type as the rows of auto_types.cpp read it: the spins
// (SpinEvents: a start and an end event each), their event source, required parts, lanes and roles.
//
// A spin is a turn of the whole body about the vertical, read from the hip line (left up leg ->
// right up leg seen from above, PhysicsAnalysis::hips_yaw_deg): the turn segmentation of gait
// studies (R[1][12]). The yaw is smoothed (spin_smooth_s); its rate above spin_start_dps starts
// a spin, which runs back and forward from there while the rate stays at spin_end_dps or more in
// the same direction (a stop or a turn back ends it); a spin is kept when its yaw changes by
// spin_min_deg at least. A sample where the hip line is too steep to give a yaw counts as no
// turn. On a loop, the middle cycle's yaw changes are repeated over the unrolled track (a
// turning loop's heading jumps back at the seams, and its other cycles are shifted, not
// turned), and only the starts (and ends) in the middle cycle are the clip's.
// A spin cut by the clip's start or end is still kept: its start is the clip's first sample, its
// end the last.
//
// Sideless (one block per kind, side B): two kinds, "Spin" at the start and "Spin End" at the
// end. Strength = the sweep (degrees, whatever the direction).

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "auto_types.h"

namespace rav {
namespace {

// A Gaussian smoothing of a per-sample curve (sigma in samples; edges clamped).
std::vector<double> SmoothCurve(const std::vector<double>& v, double sigma)
{
    if (!(sigma > 0.0) || v.size() < 2) return v;
    const int           r = static_cast<int>(std::ceil(3.0 * sigma));
    std::vector<double> k(static_cast<size_t>(2 * r + 1));
    double              sum = 0.0;
    for (int i = -r; i <= r; ++i) sum += (k[static_cast<size_t>(i + r)] = std::exp(-0.5 * (i / sigma) * (i / sigma)));
    for (double& w : k) w /= sum;
    const int           n = static_cast<int>(v.size());
    std::vector<double> out(v.size(), 0.0);
    for (int i = 0; i < n; ++i) {
        double acc = 0.0;
        for (int j = -r; j <= r; ++j) acc += k[static_cast<size_t>(j + r)] * v[static_cast<size_t>(std::clamp(i + j, 0, n - 1))];
        out[static_cast<size_t>(i)] = acc;
    }
    return out;
}

// The smoothed hip-line yaw (degrees) and its rate (deg/s) per analysed sample. Each sample's
// change is kept only between two valid samples. On a loop the changes are the middle cycle's,
// repeated over the unrolled track (its other cycles are shifted, not turned: a turning loop's
// hip line is bent there), and the seam's change is the one before it (a turning loop's heading
// jumps back there). Empty without the yaw.
struct SpinYaw {
    std::vector<double> yaw;
    std::vector<double> rate;
};

SpinYaw SpinYawOf(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    SpinYaw out;
    const size_t n = a.hips_yaw_deg.size();
    if (!a.ok || n < 2 || n != a.samples || a.hips_yaw_valid.size() != n || !(a.rate_hz > 0.0)) return out;
    // The cycle (0: not a loop); the middle cycle's samples are [m, 2 m) of the 3 m + 1.
    const size_t m = a.looping && a.window_samples > 2 && a.window_first + 1 == a.window_samples &&
                             n == 3 * (a.window_samples - 1) + 1
                         ? a.window_samples - 1
                         : 0;
    auto delta = [&](size_t i) {  // the change from sample i - 1 to sample i (i >= 1)
        if (m > 0) {
            size_t j = i % m;
            if (j == 0) j = m - 1;  // the seam: as the sample before
            i = m + j;
        }
        return a.hips_yaw_valid[i] && a.hips_yaw_valid[i - 1] ? a.hips_yaw_deg[i] - a.hips_yaw_deg[i - 1] : 0.0;
    };
    std::vector<double> joined(n, 0.0);
    for (size_t i = 1; i < n; ++i) joined[i] = joined[i - 1] + delta(i);
    out.yaw = SmoothCurve(joined, p.spin_smooth_s * a.rate_hz);
    out.rate.assign(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        const size_t lo = i > 0 ? i - 1 : 0, hi = i + 1 < n ? i + 1 : n - 1;
        out.rate[i] = (out.yaw[hi] - out.yaw[lo]) * a.rate_hz / static_cast<double>(hi - lo);
    }
    return out;
}

// The spins as analysed-sample spans [start, end] (inclusive), by time.
struct SpinSpan {
    size_t start = 0;
    size_t end = 0;
    double sweep = 0.0;  // degrees, signed
};

std::vector<SpinSpan> SpinSpans(const SpinYaw& y, const PhysicsParams& p)
{
    std::vector<SpinSpan> out;
    const size_t          n = y.rate.size();
    auto                  dir = [&](size_t i) { return y.rate[i] > 0.0 ? 1 : (y.rate[i] < 0.0 ? -1 : 0); };
    auto                  holds = [&](size_t i, int d) { return dir(i) == d && std::fabs(y.rate[i]) >= p.spin_end_dps; };
    for (size_t i = 0; i < n; ++i) {
        if (!(std::fabs(y.rate[i]) > p.spin_start_dps)) continue;
        const int d = dir(i);
        size_t    s = i, e = i;
        while (s > 0 && holds(s - 1, d)) --s;
        while (e + 1 < n && holds(e + 1, d)) ++e;
        SpinSpan sp;
        sp.start = s;
        sp.end = e;
        sp.sweep = y.yaw[e] - y.yaw[s];
        if (std::fabs(sp.sweep) >= p.spin_min_deg) out.push_back(sp);
        i = e;
    }
    return out;
}

// An analysed time as a clip time (false outside the clip window; a loop: the middle cycle).
bool SpinClipTime(const PhysicsAnalysis& a, double t, double* out)
{
    if (!a.looping) {
        *out = t;
        return true;
    }
    constexpr double kEps = 1e-9;
    const double     c = t - static_cast<double>(a.window_first) / a.rate_hz;
    const double     T = static_cast<double>(a.window_samples - 1) / a.rate_hz;
    if (c < -kEps || c >= T - kEps) return false;
    *out = std::max(0.0, c);
    return true;
}

// The spins in clip time, by time: each a Spin event at its start and a SpinEnd event at its end
// (side B, start_s / end_s the span in clip time, shifted with time_s, strength |sweep|). On a
// loop, an event is the clip's when its own time falls in the middle cycle.
std::vector<PhysicsEvent> SpinEvents(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    std::vector<PhysicsEvent> ev;
    const SpinYaw             y = SpinYawOf(a, p);
    if (y.rate.empty()) return ev;
    for (const SpinSpan& sp : SpinSpans(y, p)) {
        const double s = static_cast<double>(sp.start) / a.rate_hz, e = static_cast<double>(sp.end) / a.rate_hz;
        for (const bool at_end : {false, true}) {
            double t;
            if (!SpinClipTime(a, at_end ? e : s, &t)) continue;
            const double shift = (at_end ? e : s) - t;
            PhysicsEvent pe;
            pe.kind = at_end ? PhysicsKind::SpinEnd : PhysicsKind::Spin;
            pe.side = 'B';
            pe.part = -1;
            pe.time_s = t;
            pe.start_s = s - shift;
            pe.end_s = e - shift;
            pe.strength = std::fabs(sp.sweep);
            ev.push_back(pe);
        }
    }
    std::stable_sort(ev.begin(), ev.end(), [](const PhysicsEvent& x, const PhysicsEvent& z) { return x.time_s < z.time_s; });
    return ev;
}

}  // namespace

// ---- Event sources -----------------------------------------------------------------------------

// Every spin's start (Spin) and end (SpinEnd) events.
std::vector<PhysicsEvent> AutoSourceSpin(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return SpinEvents(a, p);
}

// ---- Type parts (AutoMissingParts) -------------------------------------------------------------

// Spin: both up legs (the hip line).
void AutoPartsSpin(bool, const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    for (Role r : {Role::LeftUpLeg, Role::RightUpLeg})
        if (!AutoHasRole(role_to_bone, r)) AutoPushOnce(out, RoleName(r));
}

// ---- Matches -----------------------------------------------------------------------------------

bool AutoMatchSpin(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Spin;
}

bool AutoMatchSpinEnd(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::SpinEnd;
}

// ---- Part missing (AutoBlockPartMissing) -------------------------------------------------------

bool AutoMissingSpin(char, const std::vector<int>& role_to_bone)
{
    return !AutoHasRole(role_to_bone, Role::LeftUpLeg) || !AutoHasRole(role_to_bone, Role::RightUpLeg);
}

// ---- Lanes -------------------------------------------------------------------------------------

// Spin (both kinds): the hip line's |yaw rate| vs spin_start_dps, shaded over the spins kept.
void AutoLanesSpin(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const PhysicsAnalysis& a = *args.a;
    const SpinYaw          y = SpinYawOf(a, *args.p);
    if (y.rate.empty()) {
        reason = a.spin_missing.empty() ? "no usable hip line" : a.spin_missing;
        return;
    }
    AutoLane l;
    l.label = "Hips  yaw rate";
    l.unit = "deg/s";
    l.curve = AutoLaneWindow(y.rate, args.first, args.n, true);
    l.contact.assign(args.n, 0);
    for (const SpinSpan& sp : SpinSpans(y, *args.p))
        for (size_t j = sp.start; j <= sp.end; ++j)
            if (j >= args.first && j - args.first < args.n) l.contact[j - args.first] = 1;
    l.threshold = args.p->spin_start_dps;
    l.above = true;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "Shaded: a spin. It starts where the rate passes the line, runs while it stays\n"
                  "above %.0f deg/s the same way, and counts when it turns %.0f deg or more.",
                  args.p->spin_end_dps, args.p->spin_min_deg);
    l.shade_note = buf;
    out.push_back(std::move(l));
}

// ---- Roles -------------------------------------------------------------------------------------

// A spin block shows the hips and the up legs (the hip line).
std::vector<Role> AutoRolesSpin(char)
{
    return {Role::Hips, Role::LeftUpLeg, Role::RightUpLeg};
}

}  // namespace rav
