// SPDX-License-Identifier: MIT
//
// Story 10-9n: Arms > Strike, a fast arc blow that stops (a hit): a hammer fist, a hook, an
// uppercut, a weapon blow. Its events and the row's functions of auto_types.cpp (event source,
// lanes); its parts, match and roles are Swing's (the torso and the hand).
//
// Per hand, every peak of the hand's speed relative to the torso (the whoosh track,
// motion_whoosh.cpp) of strike_speed at least (the sensitivity divides it), the hand not touching
// at the peak, the peak being its run's own (the run bounded as a swing's: under
// whoosh_edge_frac of the peak, or a dip under half of it) and the run not under way on the clip's
// first sample. It is a strike when:
//   - it stops: the speed falls under strike_stop_frac of the peak within strike_stop_s after it
//     (a follow-through, an arm swinging on, takes longer);
//   - and it hits (the speed under strike_hit_frac of the peak within strike_hit_s: a blade into
//     the floor, a fist into a block, and staying under strike_rebound_frac for strike_rebound_s
//     after it: a hand coming straight back is a reversal, no hit) or travels an arc in front: at the peak the arm is raised
//     (the shoulder -> hand direction's vertical share, WhooshTrack::rise, at strike_min_rise at
//     least: not a pendulum through the bottom of the arm's arc, as running arms and jump swings
//     are) and the hand moves forward (its velocity relative to the torso, its share along the
//     body's facing at strike_min_fwd at least: a counter-arm pulled back is none).
// It is not a strike when the run overlaps a punch of that hand (a straight extension stays a
// Punch), a clap (either hand) or a hand on body of that hand lies in the run or within
// strike_touch_s of it (the hands meeting or parting after a clap, a hand on the body), or it is that hand's
// Arms Jump (the arm thrown up into a take-off, Legs Jump's take-offs at their default constants).
// Time = the peak, span = the run, strength = the peak speed (leg/s).
//
// The body's facing: horizontal, perpendicular to the hip line (left up leg -> right up leg),
// else to the shoulder line; without either, only a hit is a strike. The events are
// PhysicsKind::Swing (matched as Swing's, AutoMatchSwing), from this source only. The type row's
// `replaces_in_span` = Swing (own hand: that hand's swing over the run is the same motion) and
// `replaces` = Arms Land (own hand, at the same peak: a blow that lands with a jump hits; Jump
// Attack's sword).

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "auto_types.h"
#include "hand_touch.h"

namespace rav {

// Shared with auto_types_arm_jump.cpp (declared in auto_types.cpp).
std::vector<PhysicsEvent> AutoSourceArmJump(const PhysicsAnalysis& a, const PhysicsParams& p);

namespace {

constexpr char kStrikeSides[2] = {'L', 'R'};
const Role     kStrikeHand[2] = {Role::LeftHand, Role::RightHand};
// A run's start / end stops at a dip under this share of its peak (as a swing's).
constexpr double kStrikeDipFrac = 0.5;

const std::vector<Vec3d>* RolePos(const PhysicsAnalysis& a, Role r)
{
    const size_t i = static_cast<size_t>(r);
    if (i >= a.role_pos.size() || a.role_pos[i].size() != a.samples || a.samples == 0) return nullptr;
    return &a.role_pos[i];
}

// The positions the arc reads: the hand, the torso (the chest, else the spine, else the hips) and
// the line the facing is perpendicular to (the up legs, else the shoulders).
struct StrikeFrame {
    const std::vector<Vec3d>* hand = nullptr;
    const std::vector<Vec3d>* torso = nullptr;
    const std::vector<Vec3d>* left = nullptr;
    const std::vector<Vec3d>* right = nullptr;
};

StrikeFrame FrameOf(const PhysicsAnalysis& a, int s)
{
    StrikeFrame f;
    f.hand = RolePos(a, kStrikeHand[s]);
    for (Role r : {Role::Chest, Role::Spine, Role::Hips})
        if (!f.torso) f.torso = RolePos(a, r);
    f.left = RolePos(a, Role::LeftUpLeg);
    f.right = RolePos(a, Role::RightUpLeg);
    if (!f.left || !f.right) {
        f.left = RolePos(a, Role::LeftShoulder);
        f.right = RolePos(a, Role::RightShoulder);
    }
    if (!f.left || !f.right) f.left = f.right = nullptr;
    return f;
}

// The share of the hand's velocity relative to the torso along the body's facing at sample k
// (-1 backward .. +1 forward); NaN when it cannot be read.
double ForwardShare(const PhysicsAnalysis& a, const StrikeFrame& f, long k)
{
    const long n = static_cast<long>(a.samples);
    if (!f.hand || !f.torso || !f.left || n < 3) return std::nan("");
    const long   k0 = std::max(0L, k - 1), k1 = std::min(n - 1, k + 1);
    const size_t i0 = static_cast<size_t>(k0), i1 = static_cast<size_t>(k1), i = static_cast<size_t>(k);
    const Vec3d& h0 = (*f.hand)[i0];
    const Vec3d& h1 = (*f.hand)[i1];
    const Vec3d& t0 = (*f.torso)[i0];
    const Vec3d& t1 = (*f.torso)[i1];
    const double vx = (h1.x - t1.x) - (h0.x - t0.x), vy = (h1.y - t1.y) - (h0.y - t0.y), vz = (h1.z - t1.z) - (h0.z - t0.z);
    const double vl = std::sqrt(vx * vx + vy * vy + vz * vz);
    double       lx = (*f.right)[i].x - (*f.left)[i].x, lz = (*f.right)[i].z - (*f.left)[i].z;
    const double ll = std::sqrt(lx * lx + lz * lz);
    if (!(vl > 1e-12) || !(ll > 1e-9)) return std::nan("");
    lx /= ll;
    lz /= ll;
    // Facing = up x (left -> right), horizontal.
    return (vx * lz - vz * lx) / vl;
}

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
bool ArcAt(const PhysicsAnalysis& a, const PhysicsParams& p, const WhooshTrack& h, const StrikeFrame& f, long k)
{
    if (h.rise.size() != a.samples || !(h.rise[static_cast<size_t>(k)] >= p.strike_min_rise)) return false;
    const double fw = ForwardShare(a, f, k);
    return fw >= p.strike_min_fwd;
}

// The strikes (see the file's header).
std::vector<PhysicsEvent> StrikeEvents(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    std::vector<PhysicsEvent> ev;
    if (!a.ok || a.samples < 3 || !(a.rate_hz > 0.0)) return ev;
    const long   n = static_cast<long>(a.samples);
    const double rate = a.rate_hz, T = LoopPeriod(a);
    const long   first = a.looping ? static_cast<long>(a.window_first) : 0L;
    const long   lo = std::max(1L, first);
    const long   hi = a.looping ? std::min(n - 2, first + static_cast<long>(a.window_samples) - 2) : n - 2;
    std::vector<PhysicsEvent> punches, touches;
    for (const PhysicsEvent& e : WhooshEvents(a, p, false))
        if (e.kind == PhysicsKind::Punch) punches.push_back(e);
    const std::vector<PhysicsEvent> claps = ClapEvents(a, p);
    const std::vector<PhysicsEvent> bodies = HandOnBodyEvents(a, p);
    const std::vector<PhysicsEvent> throws = AutoSourceArmJump(a, p);
    for (int s = 0; s < 2; ++s) {
        const WhooshTrack& h = a.whoosh_hand[s];
        if (!h.present || h.speed.size() != a.samples) continue;
        const char               side = kStrikeSides[s];
        const StrikeFrame        f = FrameOf(a, s);
        const std::vector<char>& hc = a.hand[s].part.contact;
        auto at = [&](long j) { return h.speed[static_cast<size_t>(j)]; };
        auto touching = [&](long j) { return static_cast<size_t>(j) < hc.size() && hc[static_cast<size_t>(j)]; };
        for (long k = lo; k <= hi; ++k) {
            const double v = at(k);
            if (!(v >= p.strike_speed) || !(v >= at(k - 1) && v > at(k + 1)) || touching(k)) continue;
            // The run: as a swing's edges.
            const double edge = p.whoosh_edge_frac * v, dip = kStrikeDipFrac * v;
            long         st = k, en = k;
            while (st > 0 && at(st) >= edge && !(at(st) < dip && at(st - 1) > at(st))) --st;
            while (en + 1 < n && at(en) >= edge && !(at(en) < dip && at(en + 1) > at(en))) ++en;
            if (!a.looping && st == 0 && at(0) >= edge) continue;  // under way on the clip's first sample
            bool own = true;
            for (long j = st; j <= en && own; ++j) own = !(at(j) > v);
            if (!own) continue;
            // The stop.
            long j = k;
            while (j + 1 < n && at(j) > p.strike_stop_frac * v) ++j;
            if (at(j) > p.strike_stop_frac * v || static_cast<double>(j - k) / rate > p.strike_stop_s) continue;
            // A hit, or an arc in front.
            // A hit leaves the hand stopped: after the hit window's slowest point, the speed stays
            // under strike_rebound_frac of the peak for strike_rebound_s (a hand coming straight
            // back, a feint or a dance pop, is a reversal, no hit).
            long       slow = k;
            const long hit_end = std::min(n - 1, k + std::lround(p.strike_hit_s * rate));
            for (long q = k; q <= hit_end; ++q)
                if (at(q) < at(slow)) slow = q;
            bool       hit = at(slow) <= p.strike_hit_frac * v;
            const long reb_end = std::min(n - 1, slow + std::lround(p.strike_rebound_s * rate));
            for (long q = slow; q <= reb_end && hit; ++q) hit = !(at(q) > p.strike_rebound_frac * v);
            if (!hit && !ArcAt(a, p, h, f, k)) continue;
            PhysicsEvent e;
            e.kind = PhysicsKind::Swing;
            e.side = side;
            e.part = -1;
            e.time_s = static_cast<double>(k - first) / rate;
            e.start_s = e.time_s - static_cast<double>(k - st) / rate;
            e.end_s = e.time_s + static_cast<double>(en - k) / rate;
            e.strength = v;
            bool other = false;
            for (const PhysicsEvent& q : punches)
                other = other || (q.side == side && Overlaps(e.start_s, e.end_s, q.start_s, q.end_s, T));
            for (const PhysicsEvent& q : throws)
                other = other || (q.side == side && Overlaps(e.start_s, e.end_s, q.start_s, q.end_s, T));
            const double t0 = e.start_s - p.strike_touch_s, t1 = e.end_s + p.strike_touch_s;
            for (const PhysicsEvent& q : claps) other = other || Overlaps(t0, t1, q.time_s, q.time_s, T);
            for (const PhysicsEvent& q : bodies) other = other || (q.side == side && Overlaps(t0, t1, q.time_s, q.time_s, T));
            if (!other) ev.push_back(e);
        }
    }
    std::sort(ev.begin(), ev.end(), [](const PhysicsEvent& x, const PhysicsEvent& y) {
        if (x.time_s != y.time_s) return x.time_s < y.time_s;
        return x.side < y.side;
    });
    return ev;
}

}  // namespace

// ---- The row's functions -----------------------------------------------------------------------

std::vector<PhysicsEvent> AutoSourceStrike(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return StrikeEvents(a, p);
}

// The lane: the hand's speed relative to the torso vs the type's speed, shaded where the arm is
// raised and the hand moves forward (an arc in front).
void AutoLanesStrike(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const PhysicsAnalysis& a = *args.a;
    const int              s = args.side == 'R' ? 1 : 0;
    const WhooshTrack&     h = a.whoosh_hand[s];
    if (!h.present) {
        reason = h.missing;
        return;
    }
    const PhysicsParams& p = *args.p;
    const StrikeFrame    f = FrameOf(a, s);
    AutoLane             l;
    l.label = std::string("Hand ") + std::string(1, args.side) + "  speed vs torso";
    l.unit = "leg/s";
    l.curve = AutoLaneWindow(h.speed, args.first, args.n, false);
    l.contact.assign(args.n, 0);
    for (size_t i = 0; i < args.n; ++i) {
        const long k = static_cast<long>(args.first + i);
        if (k >= 0 && k < static_cast<long>(a.samples)) l.contact[i] = ArcAt(a, p, h, f, k) ? 1 : 0;
    }
    l.threshold = p.strike_speed;
    l.above = true;
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "Shaded: the arm raised and the hand moving forward (an arc). A peak above the line\n"
                  "whose speed falls to %.0f %% within %.0f ms is a strike when it is in the shade, or when it\n"
                  "hits (to %.0f %% within %.0f ms, then under %.0f %% for %.0f ms).",
                  p.strike_stop_frac * 100.0, p.strike_stop_s * 1000.0, p.strike_hit_frac * 100.0, p.strike_hit_s * 1000.0,
                  p.strike_rebound_frac * 100.0, p.strike_rebound_s * 1000.0);
    l.shade_note = buf;
    out.push_back(std::move(l));
}

}  // namespace rav
