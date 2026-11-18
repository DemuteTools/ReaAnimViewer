// SPDX-License-Identifier: MIT
//
// Story 10-12e: the Body category's whole-body moves as the rows of auto_types.cpp read them:
// Crouch (the hips go down), Stand Up (the hips come up), Bend (the trunk folds forward, the hips
// staying up) and Rise (the trunk straightens up). Their physics (BodyMoves), event sources,
// required parts, lanes and roles.
//
// Signals (motion_physics.cpp AnalyseBodyMoveSignals): the hips' height above the lowest foot part
// at each sample (PhysicsAnalysis::hips_feet, leg: never one floor for the clip, a climb's feet
// climb too) and the trunk's forward fold (trunk_fold_deg, degrees). Both are read only while a
// foot is down (feet_down > 0: a flight is skipped, its tucked legs are no crouch), smoothed per
// stretch between flights (body_move_smooth_s sigma, edges clamped), and differentiated there.
// A move starts where the rate passes its start speed and runs back and forward while the rate
// stays at its end speed or more the same way (spin's segmentation, auto_types_spin.cpp); moves
// the same way with a pause of body_move_merge_s at most between them are one (a fighter's crouch
// in two steps); a move counts when its curve changes by the type's minimum (the sensitivity
// divides it):
//   Crouch / Stand Up  the hips down / up by crouch_min_drop / stand_min_rise, with both feet down
//                      over body_move_min_support of the move at least (a climb's push is on one
//                      foot while the other steps up). The crouch before a jump and the absorbing
//                      on landing count (the flight between is skipped).
//   Bend / Rise        the trunk folds / straightens by bend_min_deg / rise_min_deg, whatever the
//                      hips do: a crouch that folds the trunk gives Crouch and Bend, a stand-up
//                      that straightens it Stand Up and Rise (Antho 2026-11-18: each type fires on
//                      its own signal, the user unticks the one he does not want).
// Constants in body units (PhysicsParams), never per-clip statistics.
//
// Sideless (one block per kind, side B). Each type has three kinds: its start (the default, "Crouch"),
// its peak (the rate's fastest sample, Peak) and its stop (Start & Stop, "Crouch Stop": the move's
// end, a sound of its own). Strength = the peak rate: leg/s for Crouch / Stand Up (the hips'
// vertical speed), deg/s for Bend / Rise (the trunk's fold rate). A move cut by the clip's end is
// kept (its end the last sample); one already under way at the clip's first sample (its rate past
// the start there: Lvl_TopDown's run starts mid-lean) is none, one starting there from rest is
// kept; on a loop the middle cycle's.
//
// Story 10-12e-2: Dash, a burst of travel speed (a dash from standstill, a walk or mid-run), on
// the hips' horizontal speed in the ground frame (hips_vh: an in-place and a root-motion clip read
// alike), smoothed over dash_smooth_s on every sample and differentiated (the acceleration). A
// burst starts where the acceleration passes dash_start_acc and runs back and forward while it
// stays at dash_end_acc or more (CurveMoves, merged across dash_merge_s); it counts when the speed
// gains dash_min_gain (the sensitivity divides it) within dash_gain_window_s and its peak
// acceleration falls with a foot down (a flight swings the hips: Jump_WhileRunning). A clip that
// does not loop is read dash_edge_sigmas sigmas away from its edges (its first and last raw
// samples are wrong), a burst cut there starting at that edge (no under-way rule: a dash from the
// first frames is real, Jump Attack). Its stop ("Dash Stop", Start & Stop) is the halt when the
// character brakes hard after it (a braking starting within dash_stop_window_s that loses the
// minimum gain, before the next dash: where it settles), else where the burst's speed plateaus
// (its end). A hard stop with no dash before it gives nothing. Strength = the speed's largest gain
// within dash_gain_window_s (leg/s, the gain that made it a dash). Parts, missing parts and roles:
// Crouch's (the hips and a foot part per side). Sideless,
// the kinds as the body moves': dash (Start, the default), dash_peak, dash_stop.

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "auto_types.h"

namespace rav {

// auto_types_legs.cpp: the hips and a foot part per side (the jumps' parts, reused).
void AutoPartsFootSplit(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
bool AutoMissingJump(char side, const std::vector<int>& role_to_bone);

namespace {

// A Gaussian smoothing of `v` (sigma in samples) over each stretch of valid samples on its own
// (edges clamped within the stretch); invalid samples keep their value.
std::vector<double> SmoothValid(const std::vector<double>& v, const std::vector<char>& valid, double sigma)
{
    std::vector<double> out = v;
    const int           n = static_cast<int>(v.size());
    if (!(sigma > 0.0)) return out;
    const int           r = static_cast<int>(std::ceil(3.0 * sigma));
    std::vector<double> k(static_cast<size_t>(2 * r + 1));
    double              sum = 0.0;
    for (int i = -r; i <= r; ++i) sum += (k[static_cast<size_t>(i + r)] = std::exp(-0.5 * (i / sigma) * (i / sigma)));
    for (double& w : k) w /= sum;
    for (int a = 0; a < n;) {
        if (!valid[static_cast<size_t>(a)]) {
            ++a;
            continue;
        }
        int b = a;
        while (b + 1 < n && valid[static_cast<size_t>(b + 1)]) ++b;
        for (int i = a; i <= b; ++i) {
            double acc = 0.0;
            for (int j = -r; j <= r; ++j) acc += k[static_cast<size_t>(j + r)] * v[static_cast<size_t>(std::clamp(i + j, a, b))];
            out[static_cast<size_t>(i)] = acc;
        }
        a = b + 1;
    }
    return out;
}

// The rate of `v` per second, a central difference within each valid stretch (0 where invalid or
// alone).
std::vector<double> RateValid(const std::vector<double>& v, const std::vector<char>& valid, double rate_hz)
{
    const size_t        n = v.size();
    std::vector<double> out(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        if (!valid[i]) continue;
        const size_t lo = i > 0 && valid[i - 1] ? i - 1 : i, hi = i + 1 < n && valid[i + 1] ? i + 1 : i;
        if (hi > lo) out[i] = (v[hi] - v[lo]) * rate_hz / static_cast<double>(hi - lo);
    }
    return out;
}

// One move: its kind, its analysed samples [start, end] (inclusive), its fastest sample, its peak
// rate (absolute) and its change (signed).
struct BodyMove {
    PhysicsKind kind = PhysicsKind::Crouch;
    size_t      start = 0;
    size_t      end = 0;
    size_t      peak = 0;
    double      strength = 0.0;
    double      change = 0.0;
};

// The curves of one signal: smoothed, its rate, the samples read (a foot down).
struct MoveCurve {
    std::vector<double> curve;
    std::vector<double> rate;
    std::vector<char>   valid;
};

MoveCurve CurveOf(const PhysicsAnalysis& a, const PhysicsParams& p, const std::vector<double>& v)
{
    MoveCurve c;
    const size_t n = a.samples;
    if (!a.ok || v.size() != n || a.feet_down.size() != n || n < 2 || !(a.rate_hz > 0.0)) return c;
    c.valid.assign(n, 0);
    for (size_t i = 0; i < n; ++i) c.valid[i] = a.feet_down[i] > 0;
    c.curve = SmoothValid(v, c.valid, p.body_move_smooth_s * a.rate_hz);
    c.rate = RateValid(c.curve, c.valid, a.rate_hz);
    return c;
}

// The moves of one curve the way `sign` says (+1 up, -1 down): start / end rates, merged across a
// pause of `merge` samples at most whose samples are all read (a flight between keeps them
// apart), kept when the curve changes by `min_change` at least. `drop_under_way`: the under-way
// rule below (the body moves on a clip that does not loop).
std::vector<BodyMove> CurveMoves(const MoveCurve& c, int sign, double start_rate, double end_rate, size_t merge,
                                 double min_change, PhysicsKind kind, bool drop_under_way)
{
    std::vector<BodyMove> spans;
    const size_t          n = c.rate.size();
    auto                  holds = [&](size_t i) { return c.valid[i] && sign * c.rate[i] >= end_rate; };
    for (size_t i = 0; i < n; ++i) {
        if (!c.valid[i] || !(sign * c.rate[i] > start_rate)) continue;
        size_t s = i, e = i;
        while (s > 0 && holds(s - 1)) --s;
        while (e + 1 < n && holds(e + 1)) ++e;
        bool read = !spans.empty() && s <= spans.back().end + merge;
        for (size_t j = read ? spans.back().end + 1 : s; j < s; ++j)
            if (!c.valid[j]) read = false;
        if (read) {
            spans.back().end = e;  // a short pause: the same move
        } else {
            BodyMove m;
            m.kind = kind;
            m.start = s;
            m.end = e;
            spans.push_back(m);
        }
        i = e;
    }
    // The clip's first read sample (a clip may open in a flight).
    size_t first = 0;
    while (first < n && !c.valid[first]) ++first;
    std::vector<BodyMove> out;
    for (BodyMove m : spans) {
        // A move already under way at the clip's first read sample (its rate there at the end
        // rate or more its way) began before the clip: none (a loop has no first sample: its
        // track is unrolled).
        if (drop_under_way && m.start == first && first < n && sign * c.rate[first] >= end_rate) continue;
        m.change = c.curve[m.end] - c.curve[m.start];
        if (!(sign * m.change >= min_change)) continue;
        m.peak = m.start;
        for (size_t j = m.start; j <= m.end; ++j)
            if (sign * c.rate[j] > sign * c.rate[m.peak]) m.peak = j;
        m.strength = std::fabs(c.rate[m.peak]);
        out.push_back(m);
    }
    return out;
}

// The hips moves (Crouch, StandUp) kept: both feet down over body_move_min_support of the move.
std::vector<BodyMove> HipsMoves(const PhysicsAnalysis& a, const PhysicsParams& p, const MoveCurve& c)
{
    std::vector<BodyMove> out;
    if (c.curve.empty()) return out;
    const size_t merge = static_cast<size_t>(std::lround(p.body_move_merge_s * a.rate_hz));
    for (const int sign : {-1, 1})
        for (const BodyMove& m : CurveMoves(c, sign, p.body_move_start_speed, p.body_move_end_speed, merge,
                                            sign < 0 ? p.crouch_min_drop : p.stand_min_rise,
                                            sign < 0 ? PhysicsKind::Crouch : PhysicsKind::StandUp, !a.looping)) {
            size_t both = 0;
            for (size_t j = m.start; j <= m.end; ++j) both += a.feet_down[j] >= 2 ? 1 : 0;
            if (static_cast<double>(both) >= p.body_move_min_support * static_cast<double>(m.end - m.start + 1) - 1e-9)
                out.push_back(m);
        }
    return out;
}

// Every move, by start: the hips moves and the trunk's folds, each on its own signal (a stand-up
// that straightens the trunk is a Stand Up and a Rise).
std::vector<BodyMove> BodyMoves(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    std::vector<BodyMove> out = HipsMoves(a, p, CurveOf(a, p, a.hips_feet));
    const MoveCurve       t = CurveOf(a, p, a.trunk_fold_deg);
    if (!t.curve.empty()) {
        const size_t merge = static_cast<size_t>(std::lround(p.body_move_merge_s * a.rate_hz));
        for (const int sign : {1, -1})
            for (const BodyMove& m : CurveMoves(t, sign, p.trunk_start_dps, p.trunk_end_dps, merge,
                                                sign > 0 ? p.bend_min_deg : p.rise_min_deg,
                                                sign > 0 ? PhysicsKind::Bend : PhysicsKind::Rise, !a.looping))
                out.push_back(m);
    }
    std::stable_sort(out.begin(), out.end(), [](const BodyMove& x, const BodyMove& y) { return x.start < y.start; });
    return out;
}

enum class MoveMark { Start, Peak, Stop };

// The moves as events in clip time, timed at `mark` (side B, start_s / end_s the span).
std::vector<PhysicsEvent> BodyMoveEvents(const PhysicsAnalysis& a, const PhysicsParams& p, MoveMark mark)
{
    std::vector<PhysicsEvent> ev;
    if (!a.ok || !(a.rate_hz > 0.0)) return ev;
    for (const BodyMove& m : BodyMoves(a, p)) {
        PhysicsEvent e;
        e.kind = m.kind;
        e.side = 'B';
        e.part = -1;
        const size_t at = mark == MoveMark::Start ? m.start : mark == MoveMark::Peak ? m.peak : m.end;
        e.time_s = static_cast<double>(at) / a.rate_hz;
        e.start_s = static_cast<double>(m.start) / a.rate_hz;
        e.end_s = static_cast<double>(m.end) / a.rate_hz;
        e.strength = m.strength;
        ev.push_back(e);
    }
    PhysicsEventsToClipTime(a, ev);
    std::stable_sort(ev.begin(), ev.end(), [](const PhysicsEvent& x, const PhysicsEvent& y) { return x.time_s < y.time_s; });
    return ev;
}

// ---- Dash (story 10-12e-2) ----------------------------------------------------------------------

// The hips' travel speed (hips_vh smoothed over dash_smooth_s, every sample: a flight's are read
// too, the flight gate is on the burst's peak) and its acceleration, read away from the edges of a
// clip that does not loop (dash_edge_sigmas x the sigma at each end: the first and last raw
// samples are wrong). Empty without the hips or the feet.
MoveCurve DashCurve(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    MoveCurve    c;
    const size_t n = a.samples;
    if (!a.ok || a.hips_vh.size() != n || a.feet_down.size() != n || n < 3 || !(a.rate_hz > 0.0)) return c;
    const size_t edge =
        a.looping ? 0 : static_cast<size_t>(std::max(0L, std::lround(p.dash_edge_sigmas * p.dash_smooth_s * a.rate_hz)));
    // The outer half of the edge (a sigma at the defaults) is not smoothed at all (edges clamped
    // inside it): a wrong edge sample never leaks into a read one through the kernel's 3 sigma.
    // Clamping at the read edge itself would read a stride's swing there as a burst (Two Cycle
    // Sprint): the read samples keep a sigma of real motion on each side.
    const size_t      keep = edge / 2;
    std::vector<char> smooth(n, 0);
    for (size_t i = keep; i + keep < n; ++i) smooth[i] = 1;
    c.curve = SmoothValid(a.hips_vh, smooth, p.dash_smooth_s * a.rate_hz);
    c.rate = RateValid(c.curve, smooth, a.rate_hz);
    c.valid.assign(n, 0);
    for (size_t i = edge; i + edge < n; ++i) c.valid[i] = 1;
    return c;
}

// One dash: its burst [start, end] (analysed samples), its fastest gain (the peak acceleration),
// its stop (the halt or the plateau) and its speed gain (leg/s).
struct DashMove {
    size_t start = 0;
    size_t end = 0;
    size_t peak = 0;
    size_t stop = 0;
    double gain = 0.0;
};

// Every dash, by start: a burst (CurveMoves on the acceleration: no under-way rule, a burst cut by
// the read edge starts there) whose speed gains dash_min_gain within dash_gain_window_s (its
// strength), a foot down within dash_flight_tol_s of its peak; its stop at the end of a braking
// that starts within dash_stop_window_s after it, before the next dash, and loses dash_min_gain,
// else at its end.
std::vector<DashMove> Dashes(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    std::vector<DashMove> out;
    const MoveCurve       c = DashCurve(a, p);
    if (c.curve.empty()) return out;
    const double rate = a.rate_hz;
    const size_t merge = static_cast<size_t>(std::lround(p.dash_merge_s * rate));
    const size_t window = static_cast<size_t>(std::max(1L, std::lround(p.dash_gain_window_s * rate)));
    const size_t stop_window = static_cast<size_t>(std::lround(p.dash_stop_window_s * rate));
    const size_t tol = static_cast<size_t>(std::max(0L, std::lround(p.dash_flight_tol_s * rate)));
    const double any = -1e300;  // every span: the gain is read within its window below
    const std::vector<BodyMove> bursts =
        CurveMoves(c, 1, p.dash_start_acc, p.dash_end_acc, merge, any, PhysicsKind::Dash, false);
    const std::vector<BodyMove> brakes =
        CurveMoves(c, -1, p.dash_start_acc, p.dash_settle_acc, merge, any, PhysicsKind::Dash, false);
    for (const BodyMove& b : bursts) {
        // The speed's largest gain within the window inside the burst.
        double gain = 0.0;
        for (size_t i = b.start; i < b.end; ++i)
            for (size_t j = i + 1; j <= b.end && j - i <= window; ++j) gain = std::max(gain, c.curve[j] - c.curve[i]);
        if (!(gain >= p.dash_min_gain - 1e-12)) continue;
        // A flight's hips swing: no foot down within the tolerance of the peak (a running
        // stride's own flight, ~0.1 s, has one close by).
        bool down = false;
        for (size_t j = b.peak > tol ? b.peak - tol : 0; j <= b.peak + tol && j < a.feet_down.size(); ++j)
            down = down || a.feet_down[j] > 0;
        if (!down) continue;
        DashMove d;
        d.start = b.start;
        d.end = b.end;
        d.peak = b.peak;
        d.stop = b.end;  // the plateau
        d.gain = gain;
        out.push_back(d);
    }
    for (size_t q = 0; q < out.size(); ++q) {
        DashMove&    d = out[q];
        const size_t next = q + 1 < out.size() ? out[q + 1].start : c.curve.size();  // a braking is the next dash's no more
        for (const BodyMove& k : brakes) {
            if (k.start < d.end) continue;
            if (k.start - d.end > stop_window || k.start >= next) break;
            double top = c.curve[d.end];
            for (size_t j = d.end; j <= k.end; ++j) top = std::max(top, c.curve[j]);
            if (top - c.curve[k.end] >= p.dash_min_gain - 1e-12) {
                d.stop = k.end;  // the halt: the speed settled
                break;
            }
        }
    }
    return out;
}

// The dashes as events in clip time, timed at `mark` (side B, start_s / end_s the burst).
std::vector<PhysicsEvent> DashEvents(const PhysicsAnalysis& a, const PhysicsParams& p, MoveMark mark)
{
    std::vector<PhysicsEvent> ev;
    if (!a.ok || !(a.rate_hz > 0.0)) return ev;
    for (const DashMove& d : Dashes(a, p)) {
        PhysicsEvent e;
        e.kind = PhysicsKind::Dash;
        e.side = 'B';
        e.part = -1;
        const size_t at = mark == MoveMark::Start ? d.start : mark == MoveMark::Peak ? d.peak : d.stop;
        e.time_s = static_cast<double>(at) / a.rate_hz;
        e.start_s = static_cast<double>(d.start) / a.rate_hz;
        e.end_s = static_cast<double>(d.end) / a.rate_hz;
        e.strength = d.gain;
        ev.push_back(e);
    }
    PhysicsEventsToClipTime(a, ev);
    std::stable_sort(ev.begin(), ev.end(),
                     [](const PhysicsEvent& x, const PhysicsEvent& y) { return x.time_s < y.time_s; });
    return ev;
}

bool HasTop(const std::vector<int>& role_to_bone)
{
    return AutoHasRole(role_to_bone, Role::Neck) || AutoHasRole(role_to_bone, Role::Chest) ||
           AutoHasRole(role_to_bone, Role::Spine);
}

bool HasFacing(const std::vector<int>& role_to_bone)
{
    return (AutoHasRole(role_to_bone, Role::LeftUpLeg) && AutoHasRole(role_to_bone, Role::RightUpLeg)) ||
           (AutoHasRole(role_to_bone, Role::LeftShoulder) && AutoHasRole(role_to_bone, Role::RightShoulder));
}

}  // namespace

// ---- Event sources -----------------------------------------------------------------------------

// Every move (Crouch, StandUp, Bend, Rise) at its start, its fastest sample, its end.
std::vector<PhysicsEvent> AutoSourceBodyMoveStart(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return BodyMoveEvents(a, p, MoveMark::Start);
}

std::vector<PhysicsEvent> AutoSourceBodyMovePeak(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return BodyMoveEvents(a, p, MoveMark::Peak);
}

std::vector<PhysicsEvent> AutoSourceBodyMoveStop(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return BodyMoveEvents(a, p, MoveMark::Stop);
}

// Story 10-12e-2: every dash at its start, its fastest gain, its stop.
std::vector<PhysicsEvent> AutoSourceDashStart(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return DashEvents(a, p, MoveMark::Start);
}

std::vector<PhysicsEvent> AutoSourceDashPeak(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return DashEvents(a, p, MoveMark::Peak);
}

std::vector<PhysicsEvent> AutoSourceDashStop(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return DashEvents(a, p, MoveMark::Stop);
}

// ---- Type parts (AutoMissingParts) -------------------------------------------------------------

// Crouch, Stand Up, Dash: the hips and a foot part per side (the jumps' parts).
void AutoPartsBodyMoveHips(bool, const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    if (!AutoHasRole(role_to_bone, Role::Hips)) AutoPushOnce(out, RoleName(Role::Hips));
    AutoPartsFootSplit(false, role_to_bone, out);
}

// Bend, Rise: the hips, the neck (else the chest, else the spine), the up legs (else the
// shoulders), and a foot part per side as Crouch (the flights are read from the feet).
void AutoPartsBodyMoveTrunk(bool, const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    if (!AutoHasRole(role_to_bone, Role::Hips)) AutoPushOnce(out, RoleName(Role::Hips));
    AutoPartsFootSplit(false, role_to_bone, out);
    if (!HasTop(role_to_bone)) {
        AutoPushOnce(out, RoleName(Role::Spine));
        AutoPushOnce(out, RoleName(Role::Neck));
    }
    if (!HasFacing(role_to_bone))
        for (Role r : {Role::LeftUpLeg, Role::RightUpLeg})
            if (!AutoHasRole(role_to_bone, r)) AutoPushOnce(out, RoleName(r));
}

// ---- Matches -----------------------------------------------------------------------------------

bool AutoMatchCrouch(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Crouch;
}

bool AutoMatchStandUp(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::StandUp;
}

bool AutoMatchBend(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Bend;
}

bool AutoMatchRise(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Rise;
}

bool AutoMatchDash(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Dash;
}

// ---- Part missing (AutoBlockPartMissing) -------------------------------------------------------

bool AutoMissingBodyMoveHips(char side, const std::vector<int>& role_to_bone)
{
    return AutoMissingJump(side, role_to_bone);
}

bool AutoMissingBodyMoveTrunk(char side, const std::vector<int>& role_to_bone)
{
    return AutoMissingJump(side, role_to_bone) || !HasTop(role_to_bone) || !HasFacing(role_to_bone);
}

// ---- Lanes -------------------------------------------------------------------------------------

namespace {

// One move lane: the curve's rate the type's way (`sign`), vs its start rate, shaded over the
// type's moves kept.
void MoveLane(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason, bool trunk, int sign,
              PhysicsKind kind)
{
    const PhysicsAnalysis& a = *args.a;
    const PhysicsParams&   p = *args.p;
    const MoveCurve        c = CurveOf(a, p, trunk ? a.trunk_fold_deg : a.hips_feet);
    if (c.curve.empty()) {
        reason = trunk ? (a.trunk_missing.empty() ? "no trunk" : a.trunk_missing)
                       : (a.hips_feet_missing.empty() ? "no hips height" : a.hips_feet_missing);
        return;
    }
    std::vector<double> v(c.rate.size(), 0.0);
    for (size_t i = 0; i < v.size(); ++i) v[i] = std::max(0.0, sign * c.rate[i]);
    AutoLane l;
    l.label = trunk ? (sign > 0 ? "Trunk  fold rate" : "Trunk  straighten rate")
                    : (sign < 0 ? "Hips  descent (over the feet)" : "Hips  rise (over the feet)");
    l.unit = trunk ? "deg/s" : "leg/s";
    l.curve = AutoLaneWindow(v, args.first, args.n, false);
    l.contact.assign(args.n, 0);
    for (const BodyMove& m : BodyMoves(a, p))
        if (m.kind == kind)
            for (size_t j = m.start; j <= m.end; ++j)
                if (j >= args.first && j - args.first < args.n) l.contact[j - args.first] = 1;
    l.threshold = trunk ? p.trunk_start_dps : p.body_move_start_speed;
    l.above = true;
    char buf[320];
    if (trunk)
        std::snprintf(buf, sizeof(buf),
                      "Shaded: a move. It starts where the rate passes the line, runs while it stays\n"
                      "above %.0f deg/s, and counts when the trunk turns %.0f deg or more\n"
                      "(with or without a Crouch / Stand Up at the same time).",
                      p.trunk_end_dps, sign > 0 ? p.bend_min_deg : p.rise_min_deg);
    else
        std::snprintf(buf, sizeof(buf),
                      "Shaded: a move. It starts where the rate passes the line, runs while it stays\n"
                      "above %.2f leg/s, and counts when the hips move %.2f leg or more with both feet down.",
                      p.body_move_end_speed, sign < 0 ? p.crouch_min_drop : p.stand_min_rise);
    l.shade_note = buf;
    out.push_back(std::move(l));
}

}  // namespace

void AutoLanesCrouch(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    MoveLane(args, out, reason, false, -1, PhysicsKind::Crouch);
}

void AutoLanesStandUp(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    MoveLane(args, out, reason, false, 1, PhysicsKind::StandUp);
}

void AutoLanesBend(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    MoveLane(args, out, reason, true, 1, PhysicsKind::Bend);
}

void AutoLanesRise(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    MoveLane(args, out, reason, true, -1, PhysicsKind::Rise);
}

// Story 10-12e-2: the hips' travel acceleration vs the burst's start, shaded over the dashes kept.
void AutoLanesDash(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const PhysicsAnalysis& a = *args.a;
    const PhysicsParams&   p = *args.p;
    const MoveCurve        c = DashCurve(a, p);
    if (c.curve.empty()) {
        reason = !a.hips_missing.empty() ? a.hips_missing
                 : !a.hips_feet_missing.empty() ? a.hips_feet_missing
                                                : "no hips speed";
        return;
    }
    std::vector<double> v(c.rate.size(), 0.0);
    for (size_t i = 0; i < v.size(); ++i) v[i] = c.valid[i] ? std::max(0.0, c.rate[i]) : 0.0;
    AutoLane l;
    l.label = "Hips  travel acceleration";
    l.unit = "leg/s2";
    l.curve = AutoLaneWindow(v, args.first, args.n, false);
    l.contact.assign(args.n, 0);
    for (const DashMove& d : Dashes(a, p))
        for (size_t j = d.start; j <= d.end; ++j)
            if (j >= args.first && j - args.first < args.n) l.contact[j - args.first] = 1;
    l.threshold = p.dash_start_acc;
    l.above = true;
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "Shaded: a dash. It starts where the acceleration passes the line, runs while it\n"
                  "stays above %.1f leg/s2, and counts when the travel speed gains %.2f leg/s or more\n"
                  "within %.1f s, its fastest gain with a foot down (not a jump's flight).",
                  p.dash_end_acc, p.dash_min_gain, p.dash_gain_window_s);
    l.shade_note = buf;
    out.push_back(std::move(l));
}

// ---- Roles -------------------------------------------------------------------------------------

// Crouch / Stand Up / Dash show the hips and both feet's parts (what they read); Bend / Rise the
// hips, the trunk and the up legs (the facing).
std::vector<Role> AutoRolesBodyMoveHips(char)
{
    std::vector<Role> out = {Role::Hips};
    for (char fs : {'L', 'R'})
        for (int q = 0; q < kFootPartCount; ++q) out.push_back(FootPartRole(fs, q));
    return out;
}

std::vector<Role> AutoRolesBodyMoveTrunk(char)
{
    return {Role::Hips, Role::Spine, Role::Chest, Role::Neck, Role::LeftUpLeg, Role::RightUpLeg};
}

}  // namespace rav
