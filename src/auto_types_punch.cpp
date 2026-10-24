// SPDX-License-Identifier: MIT
//
// Story 10-9o: Arms > Punch, any fist blow (straight, hook, uppercut, hammer fist; hitting or in
// the air), and Swing's counter-arm. The row functions of auto_types.cpp for the punch's event
// sources (marked at the stop, the default; the peak; the start), its lanes, and Swing's sources.
// Parts, match and roles are the whoosh hand's (auto_types_whoosh.cpp).
//
// A punch is either:
//   - straight: an extension run (WhooshEvents, motion_whoosh.cpp: the arm opening forward);
//   - an arc blow (10-9n's Strike, folded in): per hand, every peak of the hand's speed vs torso
//     (the whoosh track) of punch_arc_speed at least (the sensitivity divides it), the hand not
//     touching at the peak, the peak being its run's own (the run bounded as a swing's: under
//     whoosh_edge_frac of the peak, or a dip under half of it) and the run not under way on the
//     clip's first sample. It is a blow when:
//       - it stops: the speed falls under punch_arc_stop_frac of the peak within punch_arc_stop_s
//         after it (a follow-through, an arm swinging on, takes longer);
//       - and it hits (the speed under punch_hit_frac of the peak within punch_hit_s: a blade into
//         the floor, a fist into a block, then staying under punch_rebound_frac for
//         punch_rebound_s: a hand coming straight back is a reversal, no hit) or travels an arc in
//         front: at the peak the arm is raised (the shoulder -> hand direction's vertical share at
//         punch_arc_min_rise at least: not a pendulum through the bottom of the arm's arc, as
//         running arms and jump swings are) and the hand moves forward (its share along the body's
//         facing, body_facing.h, at punch_min_fwd at least: a counter-arm pulled back is none);
//       - not: an arc in the air ending with the hand above punch_arc_max_end_rise (a guard
//         raised), a run overlapping a straight punch of that hand (the same blow), that hand's
//         Arms Jump run (an arm thrown up into a take-off), a clap (either hand) or that hand's
//         hand on body within punch_touch_s of the run (the hands meeting or parting, a hand on
//         the body); a clap after which the hands stay together (HandsHeld: one hand holding the
//         other's fist, an elbow blow pushed into the palm) does not count.
//     Its stop: the first sample from the peak where the speed falls under punch_stop_frac of it;
//     a blow that hits (PunchContactStop: the elbow still bent as the hand brakes), where the
//     speed falls under punch_contact_frac of it.
// Time = the stop (default), the peak or the start; span = the run (to the stop at least);
// strength = the hand's peak speed relative to the torso (leg/s), straight or arc.
//
// Swing (its sources here): a swing of a hand moving backward at its peak (forward share under 0)
// within swing_counter_s of the other hand's punch peak is the counter-arm pulled back: dropped.

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "auto_types.h"
#include "body_facing.h"
#include "hand_touch.h"

namespace rav {

// Shared with auto_types_arm_jump.cpp / auto_types_whoosh.cpp (declared in auto_types.cpp).
std::vector<PhysicsEvent> AutoSourceArmJump(const PhysicsAnalysis& a, const PhysicsParams& p);
void AutoLanesPunch(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason);

namespace {

constexpr char kPunchSides[2] = {'L', 'R'};
// A run's start / end stops at a dip under this share of its peak (as a swing's).
constexpr double kArcDipFrac = 0.5;

double LoopPeriod(const PhysicsAnalysis& a)
{
    return a.looping && a.window_samples > 1 ? static_cast<double>(a.window_samples - 1) / a.rate_hz : 0.0;
}

// Whether [a0, a1] and [b0, b1] overlap (looping: also one cycle earlier or later).
bool Overlaps(double a0, double a1, double b0, double b1, double T)
{
    for (double sh : {0.0, -T, T}) {
        if (sh != 0.0 && !(T > 0.0)) continue;
        if (a0 <= b1 + sh && b0 + sh <= a1) return true;
    }
    return false;
}

// Whether the arm's state at sample k reads as an arc in front (raised, the hand moving forward).
bool ArcAt(const PhysicsAnalysis& a, const PhysicsParams& p, const WhooshTrack& h, const FacingFrame& f, long k)
{
    if (h.rise.size() != a.samples || !(h.rise[static_cast<size_t>(k)] >= p.punch_arc_min_rise)) return false;
    return ForwardShare(a, f, k) >= p.punch_min_fwd;
}

// The analysed sample of a clip time.
long SampleOf(const PhysicsAnalysis& a, double t)
{
    const long first = a.looping ? static_cast<long>(a.window_first) : 0L;
    return std::clamp(first + std::lround(t * a.rate_hz), 0L, static_cast<long>(a.samples) - 1);
}

// Story 10-9o: whether the hands stay together after a clap at clip time t (within
// punch_hold_gap leg of their distance at the clap for punch_hold_s at least): one hand holding
// the other's fist or palm (an elbow blow pushed by the other hand), no clap of a blow's end.
bool HandsHeld(const PhysicsAnalysis& a, const PhysicsParams& p, double t)
{
    const std::vector<Vec3d>* l = FacingRolePos(a, Role::LeftHand);
    const std::vector<Vec3d>* r = FacingRolePos(a, Role::RightHand);
    if (!l || !r || !(a.leg_length > 0.0)) return false;
    auto dist = [&](long i) {
        const Vec3d &x = (*l)[static_cast<size_t>(i)], &y = (*r)[static_cast<size_t>(i)];
        return std::sqrt((x.x - y.x) * (x.x - y.x) + (x.y - y.y) * (x.y - y.y) + (x.z - y.z) * (x.z - y.z)) / a.leg_length;
    };
    const long   i = SampleOf(a, t), n = static_cast<long>(a.samples);
    const long   need = std::lround(p.punch_hold_s * a.rate_hz);
    const double d0 = dist(i);
    // A hold lasting to the clip's last sample is a hold.
    for (long j = i; j <= std::min(n - 1, i + need); ++j)
        if (dist(j) > d0 + p.punch_hold_gap) return false;
    return true;
}

// The arc blows (see the file's header), PhysicsKind::Punch, timed by `mark`; `straight`: the
// straight punches (their spans).
std::vector<PhysicsEvent> ArcEvents(const PhysicsAnalysis& a, const PhysicsParams& p, WhooshMark mark,
                                    const std::vector<PhysicsEvent>& straight)
{
    std::vector<PhysicsEvent> ev;
    if (!a.ok || a.samples < 3 || !(a.rate_hz > 0.0)) return ev;
    const long   n = static_cast<long>(a.samples);
    const double rate = a.rate_hz, T = LoopPeriod(a);
    const long   first = a.looping ? static_cast<long>(a.window_first) : 0L;
    const long   lo = std::max(1L, first);
    const long   hi = a.looping ? std::min(n - 2, first + static_cast<long>(a.window_samples) - 2) : n - 2;
    const std::vector<PhysicsEvent> claps = ClapEvents(a, p);
    const std::vector<PhysicsEvent> bodies = HandOnBodyEvents(a, p);
    const std::vector<PhysicsEvent> throws = AutoSourceArmJump(a, p);
    for (int s = 0; s < 2; ++s) {
        const WhooshTrack& h = a.whoosh_hand[s];
        if (!h.present || h.speed.size() != a.samples) continue;
        const char               side = kPunchSides[s];
        const FacingFrame        f = FacingFrameOf(a, s);
        const std::vector<char>& hc = a.hand[s].part.contact;
        auto at = [&](long j) { return h.speed[static_cast<size_t>(j)]; };
        auto touching = [&](long j) { return static_cast<size_t>(j) < hc.size() && hc[static_cast<size_t>(j)]; };
        for (long k = lo; k <= hi; ++k) {
            const double v = at(k);
            if (!(v >= p.punch_arc_speed) || !(v >= at(k - 1) && v > at(k + 1)) || touching(k)) continue;
            // The run: as a swing's edges.
            const double edge = p.whoosh_edge_frac * v, dip = kArcDipFrac * v;
            long         st = k, en = k;
            while (st > 0 && at(st) >= edge && !(at(st) < dip && at(st - 1) > at(st))) --st;
            while (en + 1 < n && at(en) >= edge && !(at(en) < dip && at(en + 1) > at(en))) ++en;
            if (!a.looping && st == 0 && at(0) >= edge) continue;  // under way on the clip's first sample
            bool own = true;
            for (long j = st; j <= en && own; ++j) own = !(at(j) > v);
            if (!own) continue;
            // The stop test.
            long j = k;
            while (j + 1 < n && at(j) > p.punch_arc_stop_frac * v) ++j;
            if (at(j) > p.punch_arc_stop_frac * v || static_cast<double>(j - k) / rate > p.punch_arc_stop_s) continue;
            // A hit, or an arc in front. A hit leaves the hand stopped: after the hit window's
            // slowest point, the speed stays under punch_rebound_frac of the peak for
            // punch_rebound_s (a hand coming straight back, a feint or a dance pop, is a reversal).
            long       slow = k;
            const long hit_end = std::min(n - 1, k + std::lround(p.punch_hit_s * rate));
            for (long q = k; q <= hit_end; ++q)
                if (at(q) < at(slow)) slow = q;
            bool       hit = at(slow) <= p.punch_hit_frac * v;
            const long reb_end = std::min(n - 1, slow + std::lround(p.punch_rebound_s * rate));
            for (long q = slow; q <= reb_end && hit; ++q) hit = !(at(q) > p.punch_rebound_frac * v);
            if (!hit && !ArcAt(a, p, h, f, k)) continue;
            // The marker's stop: the speed under punch_stop_frac of the peak.
            long stop = k;
            while (stop + 1 < n && !(at(stop) < p.punch_stop_frac * v)) ++stop;
            // Story 10-9o: an arc in the air that ends with the hand above punch_arc_max_end_rise
            // (the arm swung up over the shoulder: a guard raised, a block) is no blow.
            if (!hit && h.rise.size() == a.samples && h.rise[static_cast<size_t>(stop)] > p.punch_arc_max_end_rise) continue;
            // A blow that hits: its marker at the contact (PunchContactStop; the span keeps `stop`).
            const int  contact = PunchContactStop(h, static_cast<int>(k), rate, p);
            const long mstop = contact >= 0 ? static_cast<long>(contact) : stop;
            PhysicsEvent e;
            e.kind = PhysicsKind::Punch;
            e.side = side;
            e.part = -1;
            const double peak_s = static_cast<double>(k - first) / rate;
            e.start_s = peak_s - static_cast<double>(k - st) / rate;
            e.end_s = peak_s + static_cast<double>(std::max(en, stop) - k) / rate;
            e.time_s = mark == WhooshMark::Start ? e.start_s
                       : mark == WhooshMark::Stop ? peak_s + static_cast<double>(mstop - k) / rate
                                                  : peak_s;
            // Past the seam (a stop) or before it (a start): the whole event shifted by one cycle
            // (start <= time <= end on one cycle).
            double shift = 0.0;
            if (T > 0.0 && e.time_s >= T) shift = -T;
            if (T > 0.0 && e.time_s < 0.0) shift = T;
            e.strength = v;
            bool other = false;
            for (const PhysicsEvent& q : straight)
                other = other || (q.side == side && Overlaps(e.start_s, peak_s + (en - k) / rate, q.start_s, q.end_s, T));
            for (const PhysicsEvent& q : throws)
                other = other || (q.side == side && Overlaps(e.start_s, peak_s + (en - k) / rate, q.start_s, q.end_s, T));
            const double t0 = e.start_s - p.punch_touch_s, t1 = peak_s + (en - k) / rate + p.punch_touch_s;
            for (const PhysicsEvent& q : claps)
                other = other || (Overlaps(t0, t1, q.time_s, q.time_s, T) && !HandsHeld(a, p, q.time_s));
            for (const PhysicsEvent& q : bodies) other = other || (q.side == side && Overlaps(t0, t1, q.time_s, q.time_s, T));
            if (!other) {
                e.time_s += shift;
                e.start_s += shift;
                e.end_s += shift;
                ev.push_back(e);
            }
        }
    }
    return ev;
}

void SortEvents(std::vector<PhysicsEvent>& ev)
{
    std::sort(ev.begin(), ev.end(), [](const PhysicsEvent& x, const PhysicsEvent& y) {
        if (x.time_s != y.time_s) return x.time_s < y.time_s;
        if (x.side != y.side) return x.side < y.side;
        return static_cast<int>(x.kind) < static_cast<int>(y.kind);
    });
}

// The punches, straight and arc, timed by `mark`.
std::vector<PhysicsEvent> PunchEvents(const PhysicsAnalysis& a, const PhysicsParams& p, WhooshMark mark)
{
    std::vector<PhysicsEvent> ev, spans;
    if (!a.ok) return ev;
    for (const PhysicsEvent& e : WhooshEvents(a, p, mark))
        if (e.kind == PhysicsKind::Punch) ev.push_back(e);
    for (const PhysicsEvent& e : WhooshEvents(a, p, WhooshMark::Peak))
        if (e.kind == PhysicsKind::Punch) spans.push_back(e);
    for (const PhysicsEvent& e : ArcEvents(a, p, mark, spans)) ev.push_back(e);
    SortEvents(ev);
    return ev;
}

// The swings (at the peak, or the start), the counter-arm's dropped (see the file's header).
std::vector<PhysicsEvent> SwingEvents(const PhysicsAnalysis& a, const PhysicsParams& p, bool at_start)
{
    std::vector<PhysicsEvent> out;
    if (!a.ok) return out;
    const std::vector<PhysicsEvent> punches = PunchEvents(a, p, WhooshMark::Peak);
    const std::vector<PhysicsEvent> peaks = WhooshEvents(a, p, WhooshMark::Peak);
    const double                    T = LoopPeriod(a);
    std::vector<PhysicsEvent>       dropped;  // the counter-arm swings (their spans)
    for (const PhysicsEvent& e : peaks) {
        if (e.kind != PhysicsKind::Swing) continue;
        const int    s = e.side == 'R' ? 1 : 0;
        const double fw = ForwardShare(a, FacingFrameOf(a, s), SampleOf(a, e.time_s));
        if (!(fw < 0.0)) continue;
        for (const PhysicsEvent& q : punches) {
            if (q.side == e.side) continue;
            double d = std::fabs(q.time_s - e.time_s);
            if (T > 0.0) d = std::min(d, T - std::fmod(d, T));
            if (d <= p.swing_counter_s + 1e-9) {
                dropped.push_back(e);
                break;
            }
        }
    }
    for (const PhysicsEvent& e : at_start ? WhooshEvents(a, p, WhooshMark::Start) : peaks) {
        if (e.kind != PhysicsKind::Swing) continue;
        bool drop = false;
        for (const PhysicsEvent& q : dropped)
            drop = drop || (q.side == e.side && q.start_s == e.start_s && q.end_s == e.end_s);
        if (!drop) out.push_back(e);
    }
    return out;
}

}  // namespace

// ---- The row's functions -----------------------------------------------------------------------

std::vector<PhysicsEvent> AutoSourcePunchStop(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return PunchEvents(a, p, WhooshMark::Stop);
}

std::vector<PhysicsEvent> AutoSourcePunchPeak(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return PunchEvents(a, p, WhooshMark::Peak);
}

std::vector<PhysicsEvent> AutoSourcePunchStart(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return PunchEvents(a, p, WhooshMark::Start);
}

std::vector<PhysicsEvent> AutoSourceSwing(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return SwingEvents(a, p, false);
}

std::vector<PhysicsEvent> AutoSourceSwingStart(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return SwingEvents(a, p, true);
}

// The lanes: the straight test's (the extending speed, AutoLanesPunch) and the arc test's (the
// hand's speed relative to the torso vs the arc speed, shaded where the arm is raised and the hand
// moves forward).
void AutoLanesPunchBlow(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    AutoLanesPunch(args, out, reason);
    const PhysicsAnalysis& a = *args.a;
    const int              s = args.side == 'R' ? 1 : 0;
    const WhooshTrack&     h = a.whoosh_hand[s];
    if (!h.present) return;
    const PhysicsParams& p = *args.p;
    const FacingFrame    f = FacingFrameOf(a, s);
    AutoLane             l;
    l.label = std::string("Hand ") + std::string(1, args.side) + "  speed vs torso (arc)";
    l.unit = "leg/s";
    l.curve = AutoLaneWindow(h.speed, args.first, args.n, false);
    l.contact.assign(args.n, 0);
    for (size_t i = 0; i < args.n; ++i) {
        const long k = static_cast<long>(args.first + i);
        if (k >= 0 && k < static_cast<long>(a.samples)) l.contact[i] = ArcAt(a, p, h, f, k) ? 1 : 0;
    }
    l.threshold = p.punch_arc_speed;
    l.above = true;
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "Shaded: the arm raised and the hand moving forward (an arc). A peak above the line\n"
                  "whose speed falls to %.0f %% within %.0f ms is a blow when it is in the shade, or when it\n"
                  "hits (to %.0f %% within %.0f ms, then under %.0f %% for %.0f ms).",
                  p.punch_arc_stop_frac * 100.0, p.punch_arc_stop_s * 1000.0, p.punch_hit_frac * 100.0,
                  p.punch_hit_s * 1000.0, p.punch_rebound_frac * 100.0, p.punch_rebound_s * 1000.0);
    l.shade_note = buf;
    out.push_back(std::move(l));
}

}  // namespace rav
