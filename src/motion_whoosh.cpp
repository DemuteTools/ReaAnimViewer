// SPDX-License-Identifier: MIT
//
// Story 10-9c: swings, kicks and punches (whooshes), from the motion physics' role tracks. See
// motion_physics.h (WhooshTrack, WhooshEvents) and spec-10-9c-swings-kicks-punches.md.
//
// A whoosh is a hand or a foot moving fast relative to the torso (the chest, else the spine,
// else the hips): its speed relative to the torso, in leg lengths per second, rises above the
// type's speed (kick_speed, swing_speed, punch_speed; the sensitivity divides it) and falls back.
// One run above that speed (two runs whose dip between them stays above 70 % of it are one) is
// one event: its peak is the run's fastest sample, its start and end where the speed, walking
// away from the peak, falls under whoosh_edge_frac of the peak, or turns back up from a dip
// under half the peak (the motion before it, or after it). Strength = the peak speed (leg/s).
//   kick    a foot: the fastest of its present parts (heel, ball, toe end). At the peak no part
//           of that foot touches (a planted foot "moves" relative to a torso that travels: a run's
//           stance), the fastest part is at least kick_min_height leg above the other foot's
//           lowest part and at least kick_min_reach leg from the up leg (the hips without one):
//           raised on an extended leg. A running stride's foot whips as fast, but folded under the
//           hips when it is up, and low when the leg is extended. Part = that fastest part.
//   swing   a hand, not in contact at the peak (a climbing hand pulls the torso past it).
//   punch   the same hand peak, whose speed along the shoulder -> hand line (extending) is at
//           least punch_radial_frac of its speed: a straight thrust; any other hand peak is a
//           swing. One peak is one or the other.
// Looping clips: read on the unrolled track as the other events, kept when the peak lies in the
// clip (its start wrapped into [0, T)). Not looping, a peak on the first or last sample, or a
// whoosh already under way on the clip's first sample (its speed there at whoosh_edge_frac of the
// peak or more: the motion started before the clip, its start is not in it), is no whoosh.
//
// Pure C++17, deterministic. Host-tested (tests/motion_physics_test.cpp).

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "motion_physics.h"

namespace rav {
namespace {

constexpr char kWhooshSide[2] = {'L', 'R'};
const Role     kWhooshHand[2] = {Role::LeftHand, Role::RightHand};
const Role     kWhooshShoulder[2] = {Role::LeftShoulder, Role::RightShoulder};
const Role     kWhooshUpLeg[2] = {Role::LeftUpLeg, Role::RightUpLeg};
// Two runs above the speed whose dip stays above this share of it are one whoosh.
constexpr double kWhooshMergeFrac = 0.7;
// A whoosh's start / end stops at a dip under this share of its peak.
constexpr double kWhooshDipFrac = 0.5;

const BoneTrack* RoleTrackOf(const std::vector<BoneTrack>& rt, Role r, size_t n)
{
    const size_t i = static_cast<size_t>(r);
    return i < rt.size() && rt[i].pos.size() == n && n ? &rt[i] : nullptr;
}

// Zero-phase Gaussian (sigma in s), the ends held (motion_physics.cpp's, kept local).
std::vector<Vec3d> WhooshSmooth(const std::vector<Vec3d>& p, double sigma_s, double rate)
{
    const double sig = sigma_s * rate;
    if (!(sig >= 0.5) || p.size() < 2) return p;
    const int           r = static_cast<int>(std::ceil(3.0 * sig));
    std::vector<double> k(static_cast<size_t>(2 * r + 1));
    double              sum = 0.0;
    for (int j = -r; j <= r; ++j) {
        k[static_cast<size_t>(j + r)] = std::exp(-0.5 * (j / sig) * (j / sig));
        sum += k[static_cast<size_t>(j + r)];
    }
    for (double& w : k) w /= sum;
    const int          n = static_cast<int>(p.size());
    std::vector<Vec3d> out(p.size());
    for (int i = 0; i < n; ++i) {
        Vec3d a{0.0, 0.0, 0.0};
        for (int j = -r; j <= r; ++j) {
            const Vec3d& q = p[static_cast<size_t>(std::clamp(i + j, 0, n - 1))];
            const double w = k[static_cast<size_t>(j + r)];
            a.x += w * q.x;
            a.y += w * q.y;
            a.z += w * q.z;
        }
        out[static_cast<size_t>(i)] = a;
    }
    return out;
}

// Central differences (one-sided at the ends), per second.
std::vector<Vec3d> WhooshVelocity(const std::vector<Vec3d>& p, double rate)
{
    const int          n = static_cast<int>(p.size());
    std::vector<Vec3d> v(p.size());
    if (n < 2) return v;
    for (int i = 0; i < n; ++i) {
        const int    lo = std::max(0, i - 1), hi = std::min(n - 1, i + 1);
        const double f = rate / (hi - lo);
        const Vec3d &a = p[static_cast<size_t>(lo)], &b = p[static_cast<size_t>(hi)];
        v[static_cast<size_t>(i)] = Vec3d{(b.x - a.x) * f, (b.y - a.y) * f, (b.z - a.z) * f};
    }
    return v;
}

double Len(const Vec3d& v)
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

Vec3d Sub(const Vec3d& a, const Vec3d& b)
{
    return Vec3d{a.x - b.x, a.y - b.y, a.z - b.z};
}

// One whoosh found on a speed curve: its peak, start and end samples.
struct WhooshRun {
    int    peak = 0;
    int    start = 0;
    int    end = 0;
    double speed = 0.0;
    bool   cut_start = false;  // the speed still at the edge share of the peak on the first sample
};

// The runs above `thr` (dips above kWhooshMergeFrac x thr joined), each with its peak and span.
std::vector<WhooshRun> FindRuns(const std::vector<double>& v, double thr, double edge_frac)
{
    std::vector<WhooshRun> out;
    const int              n = static_cast<int>(v.size());
    if (!(thr > 0.0) || n < 3) return out;
    std::vector<std::pair<int, int>> runs;  // [first, last]
    for (int i = 0; i < n;) {
        if (!(v[static_cast<size_t>(i)] >= thr)) {
            ++i;
            continue;
        }
        const int s = i;
        while (i < n && v[static_cast<size_t>(i)] >= thr) ++i;
        runs.push_back({s, i - 1});
    }
    std::vector<std::pair<int, int>> merged;
    for (const auto& r : runs) {
        if (!merged.empty()) {
            double lo = v[static_cast<size_t>(r.first)];
            for (int j = merged.back().second; j <= r.first; ++j) lo = std::min(lo, v[static_cast<size_t>(j)]);
            if (lo >= kWhooshMergeFrac * thr) {
                merged.back().second = r.second;
                continue;
            }
        }
        merged.push_back(r);
    }
    for (const auto& r : merged) {
        WhooshRun w;
        w.peak = r.first;
        for (int j = r.first; j <= r.second; ++j)
            if (v[static_cast<size_t>(j)] > v[static_cast<size_t>(w.peak)]) w.peak = j;
        w.speed = v[static_cast<size_t>(w.peak)];
        // The edges: walking away from the peak, the first sample under edge_frac x the peak, or a
        // dip under half the peak (a local minimum: the motion before / after it).
        const double edge = edge_frac * w.speed, dip = kWhooshDipFrac * w.speed;
        auto         at = [&](int j) { return v[static_cast<size_t>(j)]; };
        w.start = w.peak;
        while (w.start > 0 && at(w.start) >= edge && !(at(w.start) < dip && at(w.start - 1) > at(w.start))) --w.start;
        w.cut_start = w.start == 0 && at(0) >= edge;
        w.end = w.peak;
        while (w.end + 1 < n && at(w.end) >= edge && !(at(w.end) < dip && at(w.end + 1) > at(w.end))) ++w.end;
        out.push_back(w);
    }
    return out;
}

}  // namespace

void AnalyseWhooshTracks(const std::vector<BoneTrack>& rt, const PhysicsParams& p, PhysicsAnalysis& a)
{
    const size_t n = a.samples;
    const double rate = a.rate_hz, leg = a.leg_length;
    for (WhooshTrack* w : {&a.whoosh_hand[0], &a.whoosh_hand[1], &a.whoosh_foot[0], &a.whoosh_foot[1]}) *w = WhooshTrack{};
    a.torso_missing.clear();
    if (n < 3 || !(rate > 0.0) || !(leg > 0.0)) {
        a.torso_missing = "no analysis";
        return;
    }
    const BoneTrack* torso = RoleTrackOf(rt, Role::Chest, n);
    if (!torso) torso = RoleTrackOf(rt, Role::Spine, n);
    if (!torso) torso = RoleTrackOf(rt, Role::Hips, n);
    if (!torso) {
        a.torso_missing = "no bone for the chest, the spine or the hips";
        for (WhooshTrack* w : {&a.whoosh_hand[0], &a.whoosh_hand[1], &a.whoosh_foot[0], &a.whoosh_foot[1]})
            w->missing = "no torso (" + a.torso_missing + ")";
        return;
    }
    const double             sig = p.whoosh_smooth_s;
    const std::vector<Vec3d> tsm = WhooshSmooth(torso->pos, sig, rate);
    const std::vector<Vec3d> tv = WhooshVelocity(tsm, rate);
    // Hands: speed relative to the torso, radial speed from the shoulder (else the torso).
    for (int s = 0; s < 2; ++s) {
        WhooshTrack&     w = a.whoosh_hand[s];
        const BoneTrack* h = RoleTrackOf(rt, kWhooshHand[s], n);
        if (!h || !a.hand[s].part.present) {
            w.missing = std::string("no bone for ") + RoleName(kWhooshHand[s]);
            continue;
        }
        const BoneTrack*         sh = RoleTrackOf(rt, kWhooshShoulder[s], n);
        const std::vector<Vec3d> hsm = WhooshSmooth(h->pos, sig, rate);
        const std::vector<Vec3d> hv = WhooshVelocity(hsm, rate);
        const std::vector<Vec3d> asm_ = sh ? WhooshSmooth(sh->pos, sig, rate) : tsm;
        const std::vector<Vec3d> av = sh ? WhooshVelocity(asm_, rate) : tv;
        w.present = true;
        w.speed.resize(n);
        w.radial.resize(n);
        w.reach.resize(n);
        for (size_t i = 0; i < n; ++i) {
            w.speed[i] = Len(Sub(hv[i], tv[i])) / leg;
            const Vec3d  d = Sub(hsm[i], asm_[i]);
            const double dl = Len(d);
            w.reach[i] = dl / leg;
            const Vec3d  rv = Sub(hv[i], av[i]);
            w.radial[i] = dl > 1e-9 ? (rv.x * d.x + rv.y * d.y + rv.z * d.z) / dl / leg : 0.0;
        }
    }
    // Feet: the fastest present part relative to the torso, its height above the other foot.
    std::vector<Vec3d> psm[2][kFootPartCount];
    for (int s = 0; s < 2; ++s)
        for (int q = 0; q < kFootPartCount; ++q)
            if (a.foot[s].part[q].present)
                if (const BoneTrack* t = RoleTrackOf(rt, FootPartRole(kWhooshSide[s], q), n))
                    psm[s][q] = WhooshSmooth(t->pos, sig, rate);
    for (int s = 0; s < 2; ++s) {
        WhooshTrack& w = a.whoosh_foot[s];
        std::vector<Vec3d> pv[kFootPartCount];
        bool               any = false;
        for (int q = 0; q < kFootPartCount; ++q)
            if (!psm[s][q].empty()) {
                pv[q] = WhooshVelocity(psm[s][q], rate);
                any = true;
            }
        if (!any) {
            w.missing = std::string(1, kWhooshSide[s]) + " foot: no heel, toe or toe end";
            continue;
        }
        const BoneTrack* up = RoleTrackOf(rt, kWhooshUpLeg[s], n);
        if (!up) up = RoleTrackOf(rt, Role::Hips, n);
        const std::vector<Vec3d> usm = up ? WhooshSmooth(up->pos, sig, rate) : tsm;
        w.present = true;
        w.speed.assign(n, 0.0);
        w.reach.assign(n, 0.0);
        w.height.assign(n, 0.0);
        w.part.assign(n, -1);
        for (size_t i = 0; i < n; ++i) {
            for (int q = 0; q < kFootPartCount; ++q) {
                if (pv[q].empty()) continue;
                const double v = Len(Sub(pv[q][i], tv[i])) / leg;
                if (w.part[i] < 0 || v > w.speed[i]) {
                    w.speed[i] = v;
                    w.part[i] = q;
                }
            }
            bool   other = false;
            double low = 0.0;
            for (int q = 0; q < kFootPartCount; ++q)
                if (!psm[1 - s][q].empty() && (!other || psm[1 - s][q][i].y < low)) {
                    low = psm[1 - s][q][i].y;
                    other = true;
                }
            if (other) w.height[i] = (psm[s][w.part[i]][i].y - low) / leg;
            w.reach[i] = Len(Sub(psm[s][w.part[i]][i], usm[i])) / leg;
        }
    }
}

std::vector<PhysicsEvent> WhooshEvents(const PhysicsAnalysis& a, const PhysicsParams& p, bool at_start)
{
    std::vector<PhysicsEvent> ev;
    if (!a.ok) return ev;
    const int    n = static_cast<int>(a.samples);
    const double rate = a.rate_hz;
    const int    first = static_cast<int>(a.window_first);
    const double T = a.looping && a.window_samples > 1 ? static_cast<double>(a.window_samples - 1) / rate : 0.0;
    // The clip time of an analysed sample; false outside the clip (a looping peak outside the
    // middle cycle, or a peak on the clip's first / last sample).
    auto clip_peak = [&](int i, double* t) {
        if (!a.looping) {
            if (i <= 0 || i >= n - 1) return false;
            *t = i / rate;
            return true;
        }
        const double c = (i - first) / rate;
        if (c < 0.0 || c >= T) return false;
        *t = c;
        return true;
    };
    auto clip_of = [&](int i) {
        double c = (i - (a.looping ? first : 0)) / rate;
        if (a.looping && T > 0.0) c = std::fmod(std::fmod(c, T) + T, T);
        return c;
    };
    auto push = [&](PhysicsKind k, char side, int part, const WhooshRun& r) {
        PhysicsEvent e;
        double       tp;
        if (!clip_peak(r.peak, &tp)) return;
        if (!a.looping && r.cut_start) return;  // review 10-9c: started before the clip
        e.kind = k;
        e.side = side;
        e.part = part;
        e.start_s = clip_of(r.start);
        e.end_s = a.looping ? tp + (r.end - r.peak) / rate : r.end / rate;
        if (a.looping && e.start_s > tp) e.start_s -= T;  // the start before the seam
        e.time_s = at_start ? clip_of(r.start) : tp;
        e.strength = r.speed;
        ev.push_back(e);
    };
    for (int s = 0; s < 2; ++s) {
        const WhooshTrack& f = a.whoosh_foot[s];
        if (f.present)
            for (const WhooshRun& r : FindRuns(f.speed, p.kick_speed, p.whoosh_edge_frac)) {
                const size_t i = static_cast<size_t>(r.peak);
                bool         touch = false;
                for (const PartTrack& pt : a.foot[s].part)
                    if (pt.present && i < pt.contact.size() && pt.contact[i]) touch = true;
                if (touch || f.height[i] < p.kick_min_height || f.reach[i] < p.kick_min_reach) continue;
                push(PhysicsKind::Kick, kWhooshSide[s], f.part[i], r);
            }
        const WhooshTrack& h = a.whoosh_hand[s];
        if (!h.present) continue;
        const std::vector<char>& hc = a.hand[s].part.contact;
        for (int pass = 0; pass < 2; ++pass) {
            const bool punch = pass == 1;
            for (const WhooshRun& r : FindRuns(h.speed, punch ? p.punch_speed : p.swing_speed, p.whoosh_edge_frac)) {
                const size_t i = static_cast<size_t>(r.peak);
                if (i < hc.size() && hc[i]) continue;
                const bool thrust = h.radial[i] >= p.punch_radial_frac * h.speed[i];
                if (thrust != punch) continue;
                push(punch ? PhysicsKind::Punch : PhysicsKind::Swing, kWhooshSide[s], -1, r);
            }
        }
    }
    std::sort(ev.begin(), ev.end(), [](const PhysicsEvent& x, const PhysicsEvent& y) {
        if (x.time_s != y.time_s) return x.time_s < y.time_s;
        if (x.side != y.side) return x.side < y.side;
        return static_cast<int>(x.kind) < static_cast<int>(y.kind);
    });
    return ev;
}

std::string WhooshSummary(const PhysicsAnalysis& a, const std::vector<PhysicsEvent>& ev)
{
    if (!a.ok) return "whooshes: skipped: " + a.error;
    int kicks = 0, swings = 0, punches = 0;
    for (const PhysicsEvent& e : ev) {
        if (e.kind == PhysicsKind::Kick) ++kicks;
        if (e.kind == PhysicsKind::Swing) ++swings;
        if (e.kind == PhysicsKind::Punch) ++punches;
    }
    char b[96];
    std::snprintf(b, sizeof(b), "whooshes: kicks %d, swings %d, punches %d", kicks, swings, punches);
    std::string out = b, miss;
    auto        add = [&](const std::string& m) { miss += (miss.empty() ? "" : ", ") + m; };
    if (!a.torso_missing.empty()) add("torso: " + a.torso_missing);
    else
        for (int s = 0; s < 2; ++s) {
            if (!a.whoosh_hand[s].present) add(std::string(1, kWhooshSide[s]) + " hand: " + a.whoosh_hand[s].missing);
            if (!a.whoosh_foot[s].present) add(a.whoosh_foot[s].missing);
        }
    if (!miss.empty()) out += "; missing: " + miss;
    return out;
}

}  // namespace rav
