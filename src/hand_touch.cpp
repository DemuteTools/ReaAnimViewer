// SPDX-License-Identifier: MIT
//
// See hand_touch.h. Story 10-9d: the touches, and the rows' functions of the Arms types Clap and
// Hand on body (auto_types.cpp declares them): event sources, matches, parts, lanes and roles.

#include "hand_touch.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "auto_types.h"

namespace rav {
namespace {

constexpr double kRearm = 1.3;     // a touch re-arms once the hand is this x reach away again
constexpr double kPeakFrac = 0.9;  // the approach's peak holds while the closing speed stays this x the peak

const PartTrack* HandPart(const PhysicsAnalysis& a, int s)
{
    const PartTrack& pt = a.hand[s].part;
    return pt.present ? &pt : nullptr;
}

const std::vector<Vec3d>* Pos(const PhysicsAnalysis& a, Role r)
{
    const size_t i = static_cast<size_t>(r);
    if (i >= a.role_pos.size() || a.role_pos[i].size() != a.samples || a.samples == 0) return nullptr;
    return &a.role_pos[i];
}

// A hand's track: present in the analysis (its own bone) and positioned.
const std::vector<Vec3d>* HandPos(const PhysicsAnalysis& a, int s)
{
    return HandPart(a, s) ? Pos(a, s ? Role::RightHand : Role::LeftHand) : nullptr;
}

struct Seg {
    const std::vector<Vec3d>* a;
    const std::vector<Vec3d>* b;
};

// The body's segments (see hand_touch.h), from the roles a function says are present.
template <typename Has>
std::vector<std::pair<Role, Role>> SegmentRoles(Has has)
{
    std::vector<std::pair<Role, Role>> out;
    Role prev = Role::Count;
    for (Role r : {Role::Hips, Role::Spine, Role::Chest, Role::Neck, Role::Head}) {
        if (!has(r)) continue;
        if (prev != Role::Count) out.emplace_back(prev, r);
        prev = r;
    }
    for (Role u : {Role::LeftUpLeg, Role::RightUpLeg}) {
        if (has(Role::Hips) && has(u)) out.emplace_back(Role::Hips, u);
        const Role k = u == Role::LeftUpLeg ? Role::LeftKnee : Role::RightKnee;
        if (has(u) && has(k)) out.emplace_back(u, k);
    }
    return out;
}

std::vector<Seg> BodySegments(const PhysicsAnalysis& a)
{
    std::vector<Seg> out;
    for (const auto& rr : SegmentRoles([&](Role r) { return Pos(a, r) != nullptr; }))
        out.push_back(Seg{Pos(a, rr.first), Pos(a, rr.second)});
    return out;
}

Vec3d Sub(const Vec3d& u, const Vec3d& v)
{
    return Vec3d{u.x - v.x, u.y - v.y, u.z - v.z};
}

double Dot(const Vec3d& u, const Vec3d& v)
{
    return u.x * v.x + u.y * v.y + u.z * v.z;
}

double Len(const Vec3d& u)
{
    return std::sqrt(Dot(u, u));
}

// Central-difference velocity of a position at sample i (one-sided at the ends), m/s.
Vec3d VelAt(const std::vector<Vec3d>& p, size_t i, double rate)
{
    const size_t n = p.size();
    const size_t lo = i > 0 ? i - 1 : i, hi = i + 1 < n ? i + 1 : i;
    if (hi == lo) return Vec3d{};
    const double k = rate / static_cast<double>(hi - lo);
    const Vec3d  d = Sub(p[hi], p[lo]);
    return Vec3d{d.x * k, d.y * k, d.z * k};
}

// The nearest point of the segment [A, B] to P: its parameter in [0, 1].
double SegParam(const Vec3d& P, const Vec3d& A, const Vec3d& B)
{
    const Vec3d  ab = Sub(B, A);
    const double l2 = Dot(ab, ab);
    if (!(l2 > 1e-12)) return 0.0;
    return std::clamp(Dot(Sub(P, A), ab) / l2, 0.0, 1.0);
}

Vec3d Lerp(const Vec3d& A, const Vec3d& B, double t)
{
    return Vec3d{A.x + (B.x - A.x) * t, A.y + (B.y - A.y) * t, A.z + (B.z - A.z) * t};
}

// Per sample: distance (leg), relative speed (leg/s) of hand h to its target.
struct TouchCurves {
    std::vector<double> d;
    std::vector<double> s;
};

TouchCurves ClapCurves(const PhysicsAnalysis& a, const std::vector<Vec3d>& l, const std::vector<Vec3d>& r)
{
    TouchCurves  c;
    const size_t n = a.samples;
    const double leg = a.leg_length;
    c.d.resize(n);
    c.s.resize(n);
    for (size_t i = 0; i < n; ++i) {
        c.d[i] = Len(Sub(l[i], r[i])) / leg;
        c.s[i] = Len(Sub(VelAt(l, i, a.rate_hz), VelAt(r, i, a.rate_hz))) / leg;
    }
    return c;
}

TouchCurves BodyCurves(const PhysicsAnalysis& a, const std::vector<Vec3d>& h, const std::vector<Seg>& segs)
{
    TouchCurves  c;
    const size_t n = a.samples;
    const double leg = a.leg_length;
    c.d.assign(n, 0.0);
    c.s.assign(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        double best = -1.0;
        Vec3d  vq;
        for (const Seg& g : segs) {
            const Vec3d& A = (*g.a)[i];
            const Vec3d& B = (*g.b)[i];
            const double t = SegParam(h[i], A, B);
            const double dd = Len(Sub(h[i], Lerp(A, B, t)));
            if (best < 0.0 || dd < best) {
                best = dd;
                vq = Lerp(VelAt(*g.a, i, a.rate_hz), VelAt(*g.b, i, a.rate_hz), t);
            }
        }
        c.d[i] = best / leg;
        c.s[i] = Len(Sub(VelAt(h, i, a.rate_hz), vq)) / leg;
    }
    return c;
}

// The touches of one curve pair: (stop sample, approach peak sample, peak closing speed).
struct Touch {
    int    at = 0;
    int    peak = 0;
    double speed = 0.0;
};

std::vector<Touch> FindTouches(const TouchCurves& c, double rate, double reach, double min_speed, double stop_s,
                               const PhysicsParams& p)
{
    std::vector<Touch> out;
    const int          n = static_cast<int>(c.d.size());
    if (n < 3) return out;
    std::vector<double> close(static_cast<size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) {
        const int lo = std::max(0, i - 1), hi = std::min(n - 1, i + 1);
        if (hi > lo) close[static_cast<size_t>(i)] = -(c.d[static_cast<size_t>(hi)] - c.d[static_cast<size_t>(lo)]) * rate / (hi - lo);
    }
    const int w = std::max(1, static_cast<int>(std::lround(p.touch_window_s * rate)));
    const int stop = static_cast<int>(std::lround(stop_s * rate));
    bool      armed = true;
    for (int i = 1; i < n; ++i) {
        const double d = c.d[static_cast<size_t>(i)];
        if (d > kRearm * reach) armed = true;
        if (!armed || d > reach) continue;
        int j = std::max(0, i - w);
        for (int k = j + 1; k <= i; ++k)
            if (close[static_cast<size_t>(k)] > close[static_cast<size_t>(j)]) j = k;
        const double cp = close[static_cast<size_t>(j)];
        // The braking starts at the last sample still at kPeakFrac of the peak (a held approach
        // speed brakes from its end).
        int brake = j;
        for (int k = i; k > j; --k)
            if (close[static_cast<size_t>(k)] >= kPeakFrac * cp) {
                brake = k;
                break;
            }
        if (!(cp >= min_speed) || i - brake > stop || !(c.s[static_cast<size_t>(i)] <= p.touch_stop_frac * cp)) continue;
        out.push_back(Touch{i, j, cp});
        armed = false;
    }
    return out;
}

// An analysed time as a clip time (motion_physics.cpp's ClipTime): false outside the clip window.
bool ToClip(const PhysicsAnalysis& a, double t, double* out)
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

bool MakeEvent(const PhysicsAnalysis& a, PhysicsKind kind, char side, const Touch& t, PhysicsEvent* out)
{
    double clip;
    if (!ToClip(a, static_cast<double>(t.at) / a.rate_hz, &clip)) return false;
    out->kind = kind;
    out->side = side;
    out->part = -1;
    out->time_s = out->start_s = out->end_s = clip;
    out->strength = t.speed;
    return true;
}

double PeakSpeed(const PartTrack& pt, int from, int to)
{
    double m = 0.0;
    for (int i = std::max(0, from); i <= to && static_cast<size_t>(i) < pt.speed.size(); ++i)
        m = std::max(m, pt.speed[static_cast<size_t>(i)]);
    return m;
}

void SortTouches(std::vector<PhysicsEvent>& ev)
{
    std::stable_sort(ev.begin(), ev.end(), [](const PhysicsEvent& x, const PhysicsEvent& y) {
        return x.time_s < y.time_s || (x.time_s == y.time_s && x.side < y.side);
    });
}

bool Usable(const PhysicsAnalysis& a)
{
    return a.ok && a.samples >= 3 && a.rate_hz > 0.0 && a.leg_length > 0.0;
}

}  // namespace

std::vector<PhysicsEvent> ClapEvents(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    std::vector<PhysicsEvent> ev;
    if (!Usable(a)) return ev;
    const std::vector<Vec3d>* l = HandPos(a, 0);
    const std::vector<Vec3d>* r = HandPos(a, 1);
    if (!l || !r) return ev;
    const TouchCurves c = ClapCurves(a, *l, *r);
    for (const Touch& t : FindTouches(c, a.rate_hz, p.clap_reach, p.clap_min_speed, p.clap_stop_s, p)) {
        const double vl = PeakSpeed(*HandPart(a, 0), t.peak, t.at);
        const double vr = PeakSpeed(*HandPart(a, 1), t.peak, t.at);
        PhysicsEvent e;
        if (MakeEvent(a, PhysicsKind::Clap, vr > vl + 1e-9 ? 'R' : 'L', t, &e)) ev.push_back(e);
    }
    SortTouches(ev);
    return ev;
}

std::vector<PhysicsEvent> HandOnBodyEvents(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    std::vector<PhysicsEvent> ev;
    if (!Usable(a)) return ev;
    const std::vector<Seg> segs = BodySegments(a);
    if (segs.empty()) return ev;
    for (int s = 0; s < 2; ++s) {
        const std::vector<Vec3d>* h = HandPos(a, s);
        if (!h) continue;
        const TouchCurves c = BodyCurves(a, *h, segs);
        for (const Touch& t : FindTouches(c, a.rate_hz, p.body_reach, p.body_min_speed, p.body_stop_s, p)) {
            // The hand comes to the body, not the body to a still hand.
            if (PeakSpeed(*HandPart(a, s), t.peak, t.at) < p.body_hand_frac * t.speed) continue;
            PhysicsEvent e;
            if (MakeEvent(a, PhysicsKind::HandOnBody, s ? 'R' : 'L', t, &e)) ev.push_back(e);
        }
    }
    SortTouches(ev);
    return ev;
}

std::vector<double> HandsDistance(const PhysicsAnalysis& a)
{
    if (!Usable(a)) return {};
    const std::vector<Vec3d>* l = HandPos(a, 0);
    const std::vector<Vec3d>* r = HandPos(a, 1);
    if (!l || !r) return {};
    return ClapCurves(a, *l, *r).d;
}

std::vector<double> HandBodyDistance(const PhysicsAnalysis& a, char side)
{
    if (!Usable(a)) return {};
    const std::vector<Vec3d>* h = HandPos(a, side == 'R' ? 1 : 0);
    const std::vector<Seg>    segs = BodySegments(a);
    if (!h || segs.empty()) return {};
    return BodyCurves(a, *h, segs).d;
}

bool HasBodySegment(const std::vector<int>& role_to_bone)
{
    return !SegmentRoles([&](Role r) { return AutoHasRole(role_to_bone, r); }).empty();
}

std::string TouchSummary(const PhysicsAnalysis& a, const std::vector<PhysicsEvent>& ev)
{
    if (!a.ok) return "touches: skipped: " + a.error;
    int claps = 0, body = 0;
    for (const PhysicsEvent& e : ev) {
        if (e.kind == PhysicsKind::Clap) ++claps;
        if (e.kind == PhysicsKind::HandOnBody) ++body;
    }
    char b[96];
    std::snprintf(b, sizeof(b), "touches: claps %d, hand on body %d", claps, body);
    return b;
}

// ---- The rows' functions (auto_types.cpp) ------------------------------------------------------

std::vector<PhysicsEvent> AutoSourceClap(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return ClapEvents(a, p);
}

std::vector<PhysicsEvent> AutoSourceHandOnBody(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return HandOnBodyEvents(a, p);
}

// Clap: both hands. Hand on body: the hands, and a body segment (named by the hips when none).
void AutoPartsClap(bool, const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    for (Role hand : {Role::LeftHand, Role::RightHand})
        if (!AutoHasRole(role_to_bone, hand)) AutoPushOnce(out, RoleName(hand));
}

void AutoPartsHandOnBody(bool separate, const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    AutoPartsClap(separate, role_to_bone, out);
    if (!HasBodySegment(role_to_bone)) AutoPushOnce(out, RoleName(Role::Hips));
}

bool AutoMatchClap(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Clap;
}

bool AutoMatchHandOnBody(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::HandOnBody;
}

bool AutoMissingClap(char, const std::vector<int>& role_to_bone)
{
    return !AutoHasRole(role_to_bone, Role::LeftHand) || !AutoHasRole(role_to_bone, Role::RightHand);
}

bool AutoMissingHandOnBody(char side, const std::vector<int>& role_to_bone)
{
    return !AutoHasRole(role_to_bone, side == 'R' ? Role::RightHand : Role::LeftHand) || !HasBodySegment(role_to_bone);
}

namespace {

void DistanceLane(const AutoLaneArgs& args, std::vector<AutoLane>& out, const std::vector<double>& d,
                  const std::string& label, double reach)
{
    AutoLane l;
    l.label = label;
    l.unit = "leg";
    l.curve = AutoLaneWindow(d, args.first, args.n, false);
    l.threshold = reach;
    l.above = false;  // a touch fires within reach
    l.shade_note = "";
    out.push_back(std::move(l));
}

}  // namespace

// Clap: the distance between the hands vs clap_reach.
void AutoLanesClap(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const std::vector<double> d = HandsDistance(*args.a);
    if (d.empty()) {
        reason = !args.a->hand[0].part.present ? args.a->hand[0].part.missing : args.a->hand[1].part.missing;
        if (reason.empty()) reason = "no hand positions";
        return;
    }
    DistanceLane(args, out, d, "Hands  distance", args.p->clap_reach);
}

// Hand on body: the hand's distance to the nearest body segment vs body_reach.
void AutoLanesHandOnBody(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const std::vector<double> d = HandBodyDistance(*args.a, args.side);
    if (d.empty()) {
        reason = !args.Hand().part.present ? args.Hand().part.missing : "no body bone (hips, spine, chest, neck, head, up legs)";
        return;
    }
    DistanceLane(args, out, d, "Hand " + std::string(1, args.side) + "  to body", args.p->body_reach);
}

// Clap: its hand, then the other. Hand on body: its hand.
std::vector<Role> AutoRolesClap(char side)
{
    return side == 'R' ? std::vector<Role>{Role::RightHand, Role::LeftHand} : std::vector<Role>{Role::LeftHand, Role::RightHand};
}

std::vector<Role> AutoRolesHandOnBody(char side)
{
    return {side == 'R' ? Role::RightHand : Role::LeftHand};
}

}  // namespace rav
