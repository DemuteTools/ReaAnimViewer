// SPDX-License-Identifier: MIT
//
// Story 10-9k: the arms of a jump, two Arms types: Jump (the arm thrown up into a take-off) and
// Land (the arm coming down at a landing, reaching for the floor or the knees). Their events and
// the rows' functions of auto_types.cpp (event sources, required parts, part missing, lanes).
//
// The jumps they follow are Legs Jump / Land's own events (all of Jump's signatures: flight,
// push-off, free fall; Land's flight), at their default constants: only the arm constants of `p`
// differ from them. Per take-off and hand: the hand's speed relative to the torso (the whoosh
// track, motion_whoosh.cpp) is searched within [take-off - arm_jump_before_s, take-off +
// arm_jump_after_s] for its local peaks at arm_jump_speed at least (the sensitivity divides it),
// the hand not touching at the peak; the run around a peak bounded as a swing's (the first sample
// under whoosh_edge_frac of the peak, or a dip under half the peak); the arm going up over the run
// (the shoulder -> hand direction's vertical share, WhooshTrack::rise, higher at the run's end than
// at its start); a run that overlaps a punch's is none (a punch stays a Punch). The fastest such
// peak is the event: time = the peak, span = the run, strength = the peak speed (leg/s). Per
// landing the same with the arm_land_* constants and the arm going down. One event per hand and
// peak (two take-offs that find the same peak make one).
//
// The events are PhysicsKind::Swing (the hand whipping; matched as Swing's, AutoMatchSwing): only
// these sources make them for these types. The type rows' `replaces_in_span` = Swing, own side: a
// Swing event of that hand within the run (start to end, +-kAutoReplaceS) is the same motion and
// gives way when both are ticked (one motion, one sound).

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "auto_types.h"

namespace rav {

// Shared with the other category files (declared in auto_types.cpp).
std::vector<PhysicsEvent> AutoSourceJump(const PhysicsAnalysis& a, const PhysicsParams& p);
std::vector<PhysicsEvent> AutoSourceJumpPushOff(const PhysicsAnalysis& a, const PhysicsParams& p);
std::vector<PhysicsEvent> AutoSourceJumpFreeFall(const PhysicsAnalysis& a, const PhysicsParams& p);
void AutoPartsJump(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
void AutoPartsWhooshHand(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out);
bool AutoMissingJump(char side, const std::vector<int>& role_to_bone);
bool AutoMissingWhooshHand(char side, const std::vector<int>& role_to_bone);

namespace {

constexpr char kArmSides[2] = {'L', 'R'};
// A run's start / end stops at a dip under this share of its peak (as a swing's).
constexpr double kArmDipFrac = 0.5;

// The constants of one of the two types.
struct ArmWindow {
    double speed;
    double before_s;
    double after_s;
    bool   up;  // the arm goes up over the run (Jump), else down (Land)
};

ArmWindow JumpWindow(const PhysicsParams& p)
{
    return {p.arm_jump_speed, p.arm_jump_before_s, p.arm_jump_after_s, true};
}

ArmWindow LandWindow(const PhysicsParams& p)
{
    return {p.arm_land_speed, p.arm_land_before_s, p.arm_land_after_s, false};
}

// The take-offs (every Jump signature's) or the landings, clip times, sorted.
std::vector<double> JumpTimes(const PhysicsAnalysis& a, const PhysicsParams& p, bool land)
{
    std::vector<double> out;
    if (land) {
        for (const PhysicsEvent& e : AutoSourceJump(a, p))
            if (e.kind == PhysicsKind::Land) out.push_back(e.time_s);
    } else {
        for (auto src : {&AutoSourceJump, &AutoSourceJumpPushOff, &AutoSourceJumpFreeFall})
            for (const PhysicsEvent& e : src(a, p))
                if (e.kind == PhysicsKind::Jump) out.push_back(e.time_s);
    }
    std::sort(out.begin(), out.end());
    return out;
}

double LoopPeriod(const PhysicsAnalysis& a)
{
    return a.looping && a.window_samples > 1 ? static_cast<double>(a.window_samples - 1) / a.rate_hz : 0.0;
}

// A clip time's analysed sample (looping: the middle cycle), not clamped.
long SampleAt(const PhysicsAnalysis& a, double clip_t)
{
    const double base = a.looping ? static_cast<double>(a.window_first) : 0.0;
    return std::lround(base + clip_t * a.rate_hz);
}

// An analysed sample's clip time (looping: wrapped into [0, T)).
double ClipTimeOf(const PhysicsAnalysis& a, long i)
{
    double c = static_cast<double>(i - (a.looping ? static_cast<long>(a.window_first) : 0L)) / a.rate_hz;
    const double T = LoopPeriod(a);
    if (T > 0.0) c = std::fmod(std::fmod(c, T) + T, T);
    return c;
}

// Whether the analysed sample lies within `w` of one of the times (each time's window).
bool InAnyWindow(const PhysicsAnalysis& a, long i, const std::vector<double>& times, const ArmWindow& w)
{
    for (double t : times) {
        const long c = SampleAt(a, t);
        if (i >= c - std::lround(w.before_s * a.rate_hz) && i <= c + std::lround(w.after_s * a.rate_hz)) return true;
    }
    return false;
}

// The arm events of one type (see the file's header).
std::vector<PhysicsEvent> ArmEvents(const PhysicsAnalysis& a, const PhysicsParams& p, bool land)
{
    std::vector<PhysicsEvent> ev;
    if (!a.ok || a.samples < 3) return ev;
    const ArmWindow           w = land ? LandWindow(p) : JumpWindow(p);
    const std::vector<double> times = JumpTimes(a, p, land);
    if (times.empty()) return ev;
    const long   n = static_cast<long>(a.samples);
    const double T = LoopPeriod(a);
    // The punches (a punch stays a Punch): their spans, clip times.
    std::vector<PhysicsEvent> punches;
    for (const PhysicsEvent& e : WhooshEvents(a, p, false))
        if (e.kind == PhysicsKind::Punch) punches.push_back(e);
    for (int s = 0; s < 2; ++s) {
        const WhooshTrack& h = a.whoosh_hand[s];
        if (!h.present || h.speed.size() != a.samples) continue;
        const std::vector<char>& hc = a.hand[s].part.contact;
        auto at = [&](long j) { return h.speed[static_cast<size_t>(j)]; };
        auto touching = [&](long j) { return static_cast<size_t>(j) < hc.size() && hc[static_cast<size_t>(j)]; };
        for (double t : times) {
            const long c = SampleAt(a, t);
            const long lo = std::max(1L, c - std::lround(w.before_s * a.rate_hz));
            const long hi = std::min(n - 2, c + std::lround(w.after_s * a.rate_hz));
            long       best = -1, best_start = 0, best_end = 0;
            for (long k = lo; k <= hi; ++k) {
                const double v = at(k);
                if (!(v >= w.speed) || !(v >= at(k - 1) && v > at(k + 1)) || touching(k)) continue;
                if (best >= 0 && !(v > at(best))) continue;
                // The run: as a swing's edges.
                const double edge = p.whoosh_edge_frac * v, dip = kArmDipFrac * v;
                long         st = k, en = k;
                while (st > 0 && at(st) >= edge && !(at(st) < dip && at(st - 1) > at(st))) --st;
                while (en + 1 < n && at(en) >= edge && !(at(en) < dip && at(en + 1) > at(en))) ++en;
                if (!a.looping && st == 0 && at(0) >= edge) continue;  // under way on the clip's first sample
                // The run's own peak only (a bump on the shoulder of a faster peak is that motion).
                bool own = true;
                for (long j = st; j <= en && own; ++j) own = !(at(j) > v);
                if (!own) continue;
                // The arm's way over the run: up for a take-off, down for a landing.
                // Take-off: at its peak or its end the arm is higher than at its start (a swing
                // through the bottom of the arm's arc goes down to its peak, then up). Landing:
                // the arm ends lower than it started (review 10-9k: a pendulum through the bottom
                // that comes back up is a running arm's swing, Running Jump Over Hole's right arm
                // -0.65 -> -0.79 -> -0.33; the arms coming down at a landing end 0.2-1.8 lower).
                if (h.rise.size() == a.samples) {
                    const double r0 = h.rise[static_cast<size_t>(st)], rp = h.rise[static_cast<size_t>(k)],
                                 re = h.rise[static_cast<size_t>(en)];
                    if (w.up ? !(std::max(rp, re) > r0) : !(re < r0)) continue;
                }
                best = k, best_start = st, best_end = en;
            }
            if (best < 0) continue;
            // Looping: a peak outside the middle cycle wraps into it (the cycles are copies), so
            // two take-offs finding the same peak through the seam make one event too.
            const double tp = ClipTimeOf(a, best);
            bool         seen = false;
            for (const PhysicsEvent& q : ev) {
                double d = std::fabs(q.time_s - tp);
                if (T > 0.0) d = std::min(d, T - d);
                seen = seen || (q.side == kArmSides[s] && d < 0.5 / a.rate_hz);
            }
            if (seen) continue;
            PhysicsEvent e;
            e.kind = PhysicsKind::Swing;
            e.side = kArmSides[s];
            e.part = -1;
            e.time_s = tp;
            e.start_s = e.time_s - static_cast<double>(best - best_start) / a.rate_hz;
            e.end_s = e.time_s + static_cast<double>(best_end - best) / a.rate_hz;
            bool in_punch = false;
            for (const PhysicsEvent& q : punches)
                if (q.side == e.side && e.start_s <= q.end_s && q.start_s <= e.end_s) in_punch = true;
            if (in_punch) continue;
            e.strength = at(best);
            ev.push_back(e);
        }
    }
    std::sort(ev.begin(), ev.end(), [](const PhysicsEvent& x, const PhysicsEvent& y) {
        if (x.time_s != y.time_s) return x.time_s < y.time_s;
        return x.side < y.side;
    });
    return ev;
}

// The lane: the hand's speed relative to the torso vs the type's speed, shaded within the
// windows of the take-offs (landings).
void ArmLane(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason, bool land)
{
    const PhysicsAnalysis& a = *args.a;
    const WhooshTrack&     h = a.whoosh_hand[args.side == 'R' ? 1 : 0];
    if (!h.present) {
        reason = h.missing;
        return;
    }
    const ArmWindow           w = land ? LandWindow(*args.p) : JumpWindow(*args.p);
    const std::vector<double> times = JumpTimes(a, *args.p, land);
    AutoLane                  l;
    l.label = std::string("Hand ") + std::string(1, args.side) + "  speed vs torso";
    l.unit = "leg/s";
    l.curve = AutoLaneWindow(h.speed, args.first, args.n, false);
    l.contact.assign(args.n, 0);
    for (size_t i = 0; i < args.n; ++i)
        l.contact[i] = InAnyWindow(a, static_cast<long>(args.first + i), times, w) ? 1 : 0;
    l.threshold = w.speed;
    l.above = true;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "Shaded: from %.0f ms before to %.0f ms after a %s. The arm's fastest peak above\n"
                  "the line there, the arm going %s, is the event.",
                  w.before_s * 1000.0, w.after_s * 1000.0, land ? "landing" : "take-off", w.up ? "up" : "down");
    l.shade_note = buf;
    out.push_back(std::move(l));
}

}  // namespace

// ---- The rows' functions -----------------------------------------------------------------------

std::vector<PhysicsEvent> AutoSourceArmJump(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return ArmEvents(a, p, false);
}

std::vector<PhysicsEvent> AutoSourceArmLand(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return ArmEvents(a, p, true);
}

// The jumps' parts (the hips and the feet) and the hands' (the torso and the hands).
void AutoPartsArmJump(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    AutoPartsJump(false, role_to_bone, out);
    AutoPartsWhooshHand(separate, role_to_bone, out);
}

bool AutoMissingArmJump(char side, const std::vector<int>& role_to_bone)
{
    return AutoMissingJump(side, role_to_bone) || AutoMissingWhooshHand(side, role_to_bone);
}

void AutoLanesArmJump(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    ArmLane(args, out, reason, false);
}

void AutoLanesArmLand(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    ArmLane(args, out, reason, true);
}

}  // namespace rav
