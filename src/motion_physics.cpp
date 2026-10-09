// SPDX-License-Identifier: MIT
//
// See motion_physics.h.

#include "motion_physics.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include <utility>

namespace rav {
namespace {

constexpr double kPi = 3.14159265358979323846;
// The switch cost counts samples of clear evidence at this rate; another rate scales the
// evidence so the same cost means the same time.
constexpr double kEvidenceRateHz = 240.0;
// Parts that touch (or leave) within this of each other are simultaneous: the first in part
// order (heel, toe, tip) names the foot's step or lift-off, so rounding never decides.
constexpr double kTieS = 1e-9;

const Role kPartRole[2][kFootPartCount] = {{Role::LeftHeel, Role::LeftToe, Role::LeftToeEnd},
                                           {Role::RightHeel, Role::RightToe, Role::RightToeEnd}};
const Role kKneeRole[2] = {Role::LeftKnee, Role::RightKnee};
const Role kUpLegRole[2] = {Role::LeftUpLeg, Role::RightUpLeg};
const Role kHandRole[2] = {Role::LeftHand, Role::RightHand};
const char kSide[2] = {'L', 'R'};

std::string Format(const char* fmt, ...)
{
    char    buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

int SamplesOf(double s, double rate)
{
    const double k = s * rate;
    return (std::isfinite(k) && k > 0.0) ? static_cast<int>(std::lround(k)) : 0;
}

double Dist(const Vec3d& a, const Vec3d& b)
{
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool SamePos(const std::vector<Vec3d>& a, const std::vector<Vec3d>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
    return true;
}

// The median (the mean of the two middle values for an even count); 0 when empty.
double Median(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    const size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(m), v.end());
    const double hi = v[m];
    if (v.size() % 2) return hi;
    const double lo = *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(m));
    return 0.5 * (lo + hi);
}

// Zero-phase Gaussian (sigma in s), the ends held. Unchanged under half a sample.
std::vector<Vec3d> Smooth(const std::vector<Vec3d>& p, double sigma_s, double rate)
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

// v rotated by q (normalised first; a zero quaternion is the identity).
Vec3d Rotate(const Quatd& in, const Vec3d& v)
{
    const double l = std::sqrt(in.w * in.w + in.x * in.x + in.y * in.y + in.z * in.z);
    if (!(l > 1e-12) || !std::isfinite(l)) return v;
    const double w = in.w / l, x = in.x / l, y = in.y / l, z = in.z / l;
    // v + 2 w (u x v) + 2 u x (u x v), u = (x, y, z)
    const double cx = y * v.z - z * v.y, cy = z * v.x - x * v.z, cz = x * v.y - y * v.x;
    return Vec3d{v.x + 2.0 * (w * cx + y * cz - z * cy), v.y + 2.0 * (w * cy + z * cx - x * cz),
                 v.z + 2.0 * (w * cz + x * cy - y * cx)};
}

Quatd Conj(const Quatd& q)
{
    return Quatd{q.w, -q.x, -q.y, -q.z};
}

// The unwrapped yaw (deg) of a heading whose raw yaw (deg, in (-180, 180]) is given per sample,
// and its rate (deg/s, central) where the sample and its neighbours are valid (0 elsewhere).
void YawOf(const std::vector<double>& raw, const std::vector<char>& valid, double rate, std::vector<double>& deg,
           std::vector<double>& dps)
{
    const size_t n = raw.size();
    deg.resize(n);
    dps.assign(n, 0.0);
    if (!n) return;
    double acc = raw[0];
    for (size_t i = 0; i < n; ++i) {
        if (i) {
            double d = raw[i] - raw[i - 1];
            while (d > 180.0) d -= 360.0;
            while (d < -180.0) d += 360.0;
            acc += d;
        }
        deg[i] = acc;
    }
    for (size_t i = 0; i < n; ++i) {
        const size_t lo = i ? i - 1 : 0, hi = std::min(n - 1, i + 1);
        if (valid[lo] && valid[i] && valid[hi] && hi > lo) dps[i] = (deg[hi] - deg[lo]) * rate / static_cast<double>(hi - lo);
    }
}

// Central differences (one-sided at the ends), per second.
std::vector<Vec3d> Velocity(const std::vector<Vec3d>& p, double rate)
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

// The most probable free (0) / contact (1) sequence: contact earns e[i] at sample i, free 0,
// each switch costs `cost`. Ties keep the state (and end free). Deterministic.
std::vector<char> Viterbi(const std::vector<double>& e, double cost)
{
    const size_t      n = e.size();
    std::vector<char> st(n, 0);
    if (n == 0) return st;
    std::vector<uint8_t> back(2 * n, 0);
    double               c0 = 0.0, c1 = -e[0];
    for (size_t i = 1; i < n; ++i) {
        const double stay0 = c0, sw0 = c1 + cost;
        const double stay1 = c1, sw1 = c0 + cost;
        back[2 * i] = stay0 <= sw0 ? 0 : 1;
        back[2 * i + 1] = stay1 <= sw1 ? 1 : 0;
        c0 = std::min(stay0, sw0);
        c1 = std::min(stay1, sw1) - e[i];
    }
    int s = c1 < c0 ? 1 : 0;
    for (size_t i = n - 1; i > 0; --i) {
        st[i] = static_cast<char>(s);
        s = back[2 * i + static_cast<size_t>(s)];
    }
    st[0] = static_cast<char>(s);
    return st;
}

// The sub-sample time where v goes from a (sample j - 1) to b (sample j) through th.
double CrossTime(int j, double a, double b, double th, double rate)
{
    const double f = (a != b) ? (a - th) / (a - b) : 0.0;
    return (j - 1 + std::clamp(f, 0.0, 1.0)) / rate;
}

// Where a contact comes to rest: its first sample, within `limit` of its start, whose speed is
// below settle_speed (-1 = none).
int RestStart(const PartTrack& pt, const PartContact& c, int limit, const PhysicsParams& p)
{
    for (int i = c.start; i < c.end && i <= c.start + limit; ++i)
        if (pt.speed[static_cast<size_t>(i)] < p.settle_speed) return i;
    return -1;
}

// The support: the median height over `window` from where the contact comes to rest (from its
// start when it never does).
double SupportOf(const PartTrack& pt, const PartContact& c, int rest, int window)
{
    const int           from = rest >= 0 ? rest : c.start;
    const int           stop = std::max(from + 1, std::min(c.end, from + std::max(1, window)));
    std::vector<double> h(pt.y.begin() + from, pt.y.begin() + stop);
    return Median(h);
}

double PeakSpeed(const std::vector<double>& sp, int a, int b)
{
    const int n = static_cast<int>(sp.size());
    a = std::max(0, a);
    b = std::min(n - 1, b);
    double m = 0.0;
    for (int i = a; i <= b; ++i) m = std::max(m, sp[static_cast<size_t>(i)]);
    return m;
}

// Each timing definition's step time for the contact that starts at sample s (see
// StepTiming): after `lo` (the previous contact's end, or the search window), up to s +
// search_fwd_s (Speed) or up to where the contact comes to rest (`rest`, the others).
void StepCandidates(const PartTrack& pt, const PartContact& c, int lo, int rest, double leg, double rate,
                    const PhysicsParams& p, double* out)
{
    const int                  s = c.start;
    const std::vector<double>& sp = pt.speed;
    const std::vector<double>& y = pt.y;
    const std::vector<double>& vy = pt.vy;
    const std::vector<double>& hs = pt.hspeed;
    // A crossing where the part still moves horizontally faster than a swing (slide_max_speed)
    // is no landing: the foot glides into place, so that definition keeps the Speed time.
    auto landed = [&](int j) { return hs[static_cast<size_t>(j)] <= p.slide_max_speed; };
    lo = std::max(1, lo);
    const int hi_fwd = std::max(s, std::min(c.end - 1, s + SamplesOf(p.search_fwd_s, rate)));
    const int hi = std::max(hi_fwd, std::min(c.end - 1, rest));

    // Speed: the downward crossing of the contact speed nearest to the start.
    double speed_t = s / rate;
    {
        const double th = p.contact_speed;
        int          best = -1;
        for (int j = lo; j <= hi_fwd; ++j)
            if (sp[j - 1] >= th && sp[j] < th && (best < 0 || std::abs(j - s) < std::abs(best - s))) best = j;
        if (best > 0) speed_t = CrossTime(best, sp[best - 1], sp[best], th, rate);
    }
    for (int k = 0; k < kStepTimingCount; ++k) out[k] = speed_t;

    // Height: the support + eps, falling: the first crossing after the part was last well
    // above (height_drop higher), else the last crossing before it rests.
    {
        const double h = c.support_y + p.height_eps * leg;
        const double high = h + p.height_drop * leg;
        int          m = -1, found = -1;
        for (int j = hi; j >= lo - 1 && m < 0; --j)
            if (y[j] >= high) m = j;
        for (int j = m + 1; m >= 0 && j <= hi && found < 0; ++j)
            if (y[j - 1] >= h && y[j] < h) found = j;
        for (int j = hi; found < 0 && j >= lo; --j)
            if (y[j - 1] >= h && y[j] < h) found = j;
        if (found > 0 && landed(found))
            out[static_cast<int>(StepTiming::Height)] = CrossTime(found, y[found - 1], y[found], h, rate);
    }
    // Descent: after the fastest descent of the approach, the downward speed falls below
    // descent_speed.
    {
        int m = lo;
        for (int j = lo; j <= s; ++j)
            if (vy[j] < vy[m]) m = j;
        const double d = p.descent_speed;
        const int    hold = SamplesOf(p.descent_hold_s, rate);
        const int    n = static_cast<int>(vy.size());
        if (-vy[m] >= d)
            for (int j = m + 1; j <= hi; ++j) {
                if (!(-vy[j - 1] >= d && -vy[j] < d)) continue;
                bool held = true;  // the descent stays ended for descent_hold_s
                for (int i = j; held && i < std::min(n, j + hold); ++i) held = -vy[i] < d;
                if (!held) continue;
                if (landed(j)) out[static_cast<int>(StepTiming::Descent)] = CrossTime(j, -vy[j - 1], -vy[j], d, rate);
                break;
            }
    }
    // Settle: the speed falls below settle_speed (the first time from the start; the last
    // time before it when already slower).
    {
        const double th = p.settle_speed;
        if (sp[s] >= th) {
            for (int j = s + 1; j <= hi; ++j)
                if (sp[j - 1] >= th && sp[j] < th) {
                    out[static_cast<int>(StepTiming::Settle)] = CrossTime(j, sp[j - 1], sp[j], th, rate);
                    break;
                }
        } else {
            for (int j = s; j >= lo; --j)
                if (sp[j - 1] >= th && sp[j] < th) {
                    out[static_cast<int>(StepTiming::Settle)] = CrossTime(j, sp[j - 1], sp[j], th, rate);
                    break;
                }
        }
    }
}

// The contacts of a part: Viterbi, slid gaps joined, supports, step and lift-off values.
void SegmentPart(PartTrack& pt, double leg, double rate, const PhysicsParams& p)
{
    const int n = static_cast<int>(pt.speed.size());
    const double s0 = p.contact_speed > 0.0 ? p.contact_speed : 1e-9;
    const double scale = kEvidenceRateHz / rate;
    std::vector<double> e(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        e[static_cast<size_t>(i)] =
            scale * std::clamp((s0 - pt.speed[static_cast<size_t>(i)]) / s0, std::min(0.0, p.free_evidence_floor), 1.0);
    const std::vector<char> st = Viterbi(e, p.switch_cost);

    std::vector<PartContact> cs;
    for (int i = 0; i < n;) {
        if (!st[static_cast<size_t>(i)]) {
            ++i;
            continue;
        }
        PartContact c;
        c.start = i;
        while (i < n && st[static_cast<size_t>(i)]) ++i;
        c.end = i;
        cs.push_back(c);
    }

    // A gap that stays at the first contact's support height, no faster than a slide, is a
    // slide within one contact.
    const int    win = SamplesOf(p.support_window_s, rate);
    const int    rest_limit = SamplesOf(p.rest_search_s, rate);
    const double tol = p.slide_height * leg;
    for (size_t k = 0; k + 1 < cs.size();) {
        PartContact& a = cs[k];
        const PartContact& b = cs[k + 1];
        bool joined = b.start - a.end <= SamplesOf(p.slide_gap_max_s, rate);
        const double sup = SupportOf(pt, a, RestStart(pt, a, rest_limit, p), win);
        for (int i = a.end; joined && i < b.start; ++i)
            if (std::fabs(pt.y[static_cast<size_t>(i)] - sup) > tol || pt.hspeed[static_cast<size_t>(i)] > p.slide_max_speed)
                joined = false;
        if (!joined) {
            ++k;
            continue;
        }
        a.end = b.end;
        cs.erase(cs.begin() + static_cast<std::ptrdiff_t>(k + 1));
    }

    pt.contact.assign(static_cast<size_t>(n), 0);
    const int back = SamplesOf(p.search_back_s, rate), fwd = SamplesOf(p.search_fwd_s, rate);
    const int aw = SamplesOf(p.approach_window_s, rate);
    for (size_t k = 0; k < cs.size(); ++k) {
        PartContact& c = cs[k];
        for (int i = c.start; i < c.end; ++i) pt.contact[static_cast<size_t>(i)] = 1;
        const int rest = RestStart(pt, c, rest_limit, p);
        c.support_y = SupportOf(pt, c, rest, win);
        const int prev_end = k ? cs[k - 1].end : 0;
        if (c.start > 0) {
            StepCandidates(pt, c, std::max(prev_end, c.start - back), rest, leg, rate, p, c.step_s);
            c.approach = PeakSpeed(pt.speed, c.start - aw, c.start);
        }
        if (c.end < n) {
            const double th = s0;
            const int    lo = std::max(c.start + 1, c.end - fwd);
            const int    hi = std::min(n - 1, std::min(k + 1 < cs.size() ? cs[k + 1].start : n - 1, c.end + fwd));
            int          best = -1;
            for (int j = lo; j <= hi; ++j)
                if (pt.speed[static_cast<size_t>(j - 1)] < th && pt.speed[static_cast<size_t>(j)] >= th &&
                    (best < 0 || std::abs(j - c.end) < std::abs(best - c.end)))
                    best = j;
            c.lift_s = best > 0 ? CrossTime(best, pt.speed[static_cast<size_t>(best - 1)],
                                            pt.speed[static_cast<size_t>(best)], th, rate)
                                : c.end / rate;
            c.departure = PeakSpeed(pt.speed, c.end, c.end + aw);
        }
    }
    pt.contacts = cs;
}

// Slide scuffs of one foot: per part within its contacts, then overlapping parts merged.
std::vector<Scuff> FindSlides(const FootTrack& f, double leg, double rate, const PhysicsParams& p)
{
    std::vector<Scuff> raw;
    const double       tol = p.slide_height * leg;
    const int          min_len = std::max(1, SamplesOf(p.slide_min_s, rate));
    for (int q = 0; q < kFootPartCount; ++q) {
        const PartTrack& pt = f.part[q];
        if (!pt.present) continue;
        for (const PartContact& c : pt.contacts) {
            auto sliding = [&](int i) {
                const double h = pt.hspeed[static_cast<size_t>(i)];
                return h >= p.slide_speed && h <= p.slide_max_speed &&
                       std::fabs(pt.y[static_cast<size_t>(i)] - c.support_y) <= tol;
            };
            for (int i = c.start; i < c.end;) {
                if (!sliding(i)) {
                    ++i;
                    continue;
                }
                Scuff  s;
                double dist = 0.0;
                s.start = i;
                s.part = q;
                while (i < c.end && sliding(i)) {
                    s.strength = std::max(s.strength, pt.hspeed[static_cast<size_t>(i)]);
                    dist += pt.hspeed[static_cast<size_t>(i)] / rate;
                    ++i;
                }
                s.end = i;
                if (s.end - s.start >= min_len && dist >= p.slide_min_dist) raw.push_back(s);
            }
        }
    }
    std::sort(raw.begin(), raw.end(),
              [](const Scuff& a, const Scuff& b) { return std::tie(a.start, a.part) < std::tie(b.start, b.part); });
    std::vector<Scuff> out;
    for (const Scuff& s : raw) {
        if (!out.empty() && s.start <= out.back().end) {
            Scuff& m = out.back();
            m.end = std::max(m.end, s.end);
            if (s.strength > m.strength) {
                m.strength = s.strength;
                m.part = s.part;
            }
            continue;
        }
        out.push_back(s);
    }
    return out;
}

// Pivot scuffs of one foot (needs the yaw).
std::vector<Scuff> FindPivots(const FootTrack& f, int n, double rate, const PhysicsParams& p)
{
    std::vector<Scuff> out;
    if (!f.has_yaw) return out;
    std::vector<char> in(static_cast<size_t>(n), 0);
    for (int q = 0; q < kFootPartCount; ++q)
        if (f.part[q].present)
            for (int i = 0; i < n; ++i)
                if (f.part[q].contact[static_cast<size_t>(i)]) in[static_cast<size_t>(i)] = 1;
    // A steep foot (story 10-8d): where the heel -> toe yaw gives no rate (the sample or a
    // neighbour does not hold, so a turn that crosses into the steep part is not cut in two),
    // the fallback heading holds and the part it turns on is in contact, its rate stands in.
    std::vector<double> r = f.yaw_rate_dps;
    std::vector<char>   fb(static_cast<size_t>(n), 0);
    if (f.heading_source != HeadingSource::None)
        for (int i = 0; i < n; ++i) {
            const size_t u = static_cast<size_t>(i);
            const size_t lo = i ? u - 1 : 0, hi = std::min(static_cast<size_t>(n - 1), u + 1);
            const bool   yaw_rate = f.yaw_valid[lo] && f.yaw_valid[u] && f.yaw_valid[hi] && hi > lo;
            if (yaw_rate || !f.heading_valid[u]) continue;
            const bool on_ball = f.part[static_cast<int>(FootPart::Ball)].contact[u] != 0;
            const bool on_heel = f.part[static_cast<int>(FootPart::Heel)].contact[u] != 0;
            if (on_ball || (f.heading_source == HeadingSource::Rotation && on_heel)) {
                fb[u] = 1;
                r[u] = f.heading_rate_dps[u];
            }
        }
    const double thr = p.pivot_rate_dps, ext = p.pivot_rate_dps / 3.0;
    struct Run {
        int start, end, sign, core;
    };
    std::vector<Run> runs;
    for (int i = 0; i < n;) {
        if (!(in[static_cast<size_t>(i)] && std::fabs(r[static_cast<size_t>(i)]) >= thr)) {
            ++i;
            continue;
        }
        const int sign = r[static_cast<size_t>(i)] > 0.0 ? 1 : -1;
        const int a = i;
        while (i < n && in[static_cast<size_t>(i)] && r[static_cast<size_t>(i)] * sign >= thr) ++i;
        const int core = i - a;
        int       s = a, e = i;
        while (s > 0 && in[static_cast<size_t>(s - 1)] && r[static_cast<size_t>(s - 1)] * sign >= ext) --s;
        while (e < n && in[static_cast<size_t>(e)] && r[static_cast<size_t>(e)] * sign >= ext) ++e;
        if (!runs.empty() && runs.back().sign == sign && s <= runs.back().end) {
            runs.back().end = std::max(runs.back().end, e);
            runs.back().core += core;
        } else {
            runs.push_back(Run{s, e, sign, core});
        }
        i = std::max(i, e);
    }
    const int        min_core = std::max(1, SamplesOf(p.pivot_min_s, rate));
    const PartTrack& heel = f.part[static_cast<int>(FootPart::Heel)];
    const int        toe_q = f.part[static_cast<int>(FootPart::Ball)].present ? static_cast<int>(FootPart::Ball)
                                                                               : static_cast<int>(FootPart::Tip);
    const int margin = SamplesOf(p.pivot_planted_s, rate);
    // In contact at sample i: the heel, the toe side (ball or tip), or any part (q < 0).
    auto in_part = [&](int q, int i) {
        const size_t u = static_cast<size_t>(i);
        if (q == static_cast<int>(FootPart::Heel)) return heel.contact[u] != 0;
        if (q < 0) return in[u] != 0;
        for (int t = static_cast<int>(FootPart::Ball); t < kFootPartCount; ++t)
            if (f.part[t].present && f.part[t].contact[u]) return true;
        return false;
    };
    for (const Run& run : runs) {
        // A run with a fallback sample sweeps the integral of the rate used.
        bool   steep = false;
        double integral = 0.0;
        for (int i = run.start; i < run.end; ++i) {
            steep = steep || fb[static_cast<size_t>(i)];
            integral += r[static_cast<size_t>(i)] / rate;
        }
        const double swept =
            steep ? integral : f.yaw_deg[static_cast<size_t>(run.end - 1)] - f.yaw_deg[static_cast<size_t>(run.start)];
        if (run.core < min_core || std::fabs(swept) < p.pivot_min_deg) continue;
        bool flat = true;  // a turn that ends with the foot pitched up, with no fallback heading, is a roll-off
        for (int i = run.end; flat && i < std::min(n, run.end + margin); ++i)
            flat = f.yaw_valid[static_cast<size_t>(i)] != 0 || fb[static_cast<size_t>(i)] != 0;
        if (!flat) continue;
        double heel_in = 0.0, toe_in = 0.0, heel_v = 0.0, toe_v = 0.0;
        for (int i = run.start; i < run.end; ++i) {
            const size_t u = static_cast<size_t>(i);
            bool         t_in = false;
            for (int q = static_cast<int>(FootPart::Ball); q < kFootPartCount; ++q)
                if (f.part[q].present && f.part[q].contact[u]) t_in = true;
            heel_in += heel.contact[u] ? 1.0 : 0.0;
            toe_in += t_in ? 1.0 : 0.0;
            heel_v += heel.hspeed[u];
            toe_v += f.part[toe_q].hspeed[u];
        }
        const double len = run.end - run.start;
        const bool   h_in = heel_in / len >= 0.5, t_in = toe_in / len >= 0.5;
        Scuff        s;
        s.start = run.start;
        s.end = run.end;
        s.strength = std::fabs(swept);
        if (t_in && !h_in) s.part = toe_q;
        else if (h_in && !t_in) s.part = static_cast<int>(FootPart::Heel);
        else if (h_in && t_in && toe_v < 0.5 * heel_v) s.part = toe_q;
        else if (h_in && t_in && heel_v < 0.5 * toe_v) s.part = static_cast<int>(FootPart::Heel);
        else s.part = -1;
        bool planted = true;
        for (int i = std::max(0, run.start - margin); planted && i < std::min(n, run.end + margin); ++i)
            planted = in_part(s.part, i);
        if (planted) out.push_back(s);
    }
    return out;
}

}  // namespace

const char* FootPartWord(int part)
{
    switch (part) {
    case 0: return "heel";
    case 1: return "toe";
    case 2: return "tip";
    default: return "?";
    }
}

Role FootPartRole(char side, int part)
{
    return kPartRole[side == 'R' ? 1 : 0][std::clamp(part, 0, kFootPartCount - 1)];
}

const char* StepTimingName(StepTiming t)
{
    switch (t) {
    case StepTiming::Speed: return "speed";
    case StepTiming::Height: return "height";
    case StepTiming::Descent: return "descent";
    case StepTiming::Settle: return "settle";
    }
    return "?";
}

const char* StepTimingHint(StepTiming t)
{
    switch (t) {
    case StepTiming::Speed: return "the speed falls through the contact speed (the segmentation's edge)";
    case StepTiming::Height: return "the height falls through the support + eps";
    case StepTiming::Descent: return "the main descent ends (downward speed below descent_speed)";
    case StepTiming::Settle: return "the speed falls below settle_speed (at rest)";
    }
    return "?";
}

const std::vector<Role>& PhysicsRoles()
{
    static const std::vector<Role> kRoles = {Role::LeftHeel,  Role::LeftToe,    Role::RightHeel,  Role::RightToe,
                                             Role::LeftKnee,  Role::RightKnee,  Role::LeftUpLeg,  Role::RightUpLeg,
                                             Role::Hips,      Role::LeftToeEnd, Role::RightToeEnd, Role::LeftHand,
                                             Role::RightHand};
    return kRoles;
}

std::vector<BoneTrack> PhysicsRoleTracks(const std::vector<int>& bone_of_role,
                                         const std::function<std::vector<BoneTrack>(const std::vector<int>&)>& sample)
{
    std::vector<int>  bones;
    std::vector<Role> roles;
    for (Role r : PhysicsRoles()) {
        const size_t i = static_cast<size_t>(r);
        if (i < bone_of_role.size() && bone_of_role[i] >= 0) {
            bones.push_back(bone_of_role[i]);
            roles.push_back(r);
        }
    }
    std::vector<BoneTrack> out(static_cast<size_t>(Role::Count));
    if (bones.empty() || !sample) return out;
    const std::vector<BoneTrack> tr = sample(bones);
    if (tr.size() != bones.size()) return {};
    for (size_t k = 0; k < roles.size(); ++k) out[static_cast<size_t>(roles[k])] = tr[k];
    return out;
}

PhysicsAnalysis AnalyseMotion(const std::vector<BoneTrack>& rt, const PhysicsParams& p)
{
    PhysicsAnalysis a;
    auto track = [&](Role r) -> const BoneTrack* {
        const size_t i = static_cast<size_t>(r);
        return i < rt.size() && !rt[i].pos.empty() ? &rt[i] : nullptr;
    };
    size_t n = 0;
    double rate = 0.0;
    for (Role r : PhysicsRoles())
        if (const BoneTrack* t = track(r)) {
            n = t->pos.size();
            rate = t->rate_hz;
            break;
        }
    if (n == 0) {
        a.error = rt.empty() ? "the bones could not be sampled" : "no leg or foot bone";
        return a;
    }
    for (Role r : PhysicsRoles())
        if (const BoneTrack* t = track(r))
            if (t->pos.size() != n || t->rate_hz != rate) {
                a.error = "the bone tracks differ in rate or length";
                return a;
            }
    if (!(rate > 0.0) || !std::isfinite(rate)) {
        a.error = "bad sample rate";
        return a;
    }
    if (n < 3) {
        a.error = "the clip is too short";
        return a;
    }
    a.rate_hz = rate;
    a.samples = n;

    // Body scale: up leg -> knee -> heel, per side.
    double sum = 0.0;
    int    sides = 0;
    bool   hips = false;
    for (int s = 0; s < 2; ++s) {
        const BoneTrack* h = track(kPartRole[s][0]);
        const BoneTrack* k = track(kKneeRole[s]);
        const BoneTrack* u = track(kUpLegRole[s]);
        const BoneTrack* top = u ? u : track(Role::Hips);
        if (!h || !k || !top) continue;
        std::vector<double> len(n);
        for (size_t i = 0; i < n; ++i) len[i] = Dist(top->pos[i], k->pos[i]) + Dist(k->pos[i], h->pos[i]);
        const double m = Median(len);
        if (!(m > 1e-6) || !std::isfinite(m)) continue;
        sum += m;
        ++sides;
        if (!u) hips = true;
    }
    // No side gives it (no heel): twice the thigh, up leg -> knee.
    bool thigh = false;
    if (!sides)
        for (int s = 0; s < 2; ++s) {
            const BoneTrack* k = track(kKneeRole[s]);
            const BoneTrack* u = track(kUpLegRole[s]);
            if (!k || !u) continue;
            std::vector<double> len(n);
            for (size_t i = 0; i < n; ++i) len[i] = 2.0 * Dist(u->pos[i], k->pos[i]);
            const double m = Median(len);
            if (!(m > 1e-6) || !std::isfinite(m)) continue;
            sum += m;
            ++sides;
            thigh = true;
        }
    if (!sides) {
        a.error = "no body scale: needs the heel, the knee and the up leg (or the hips) of one side, or its knee and up leg";
        return a;
    }
    const double leg = sum / sides;
    a.leg_length = leg;
    if (sides == 1) a.scale_note = "one leg";
    if (hips) a.scale_note += std::string(a.scale_note.empty() ? "" : ", ") + "hips for the up leg";
    if (thigh) a.scale_note += std::string(a.scale_note.empty() ? "" : ", ") + "thigh x2";

    // Parts present, smoothed, their velocities.
    std::vector<Vec3d> sm[2][kFootPartCount], vel[2][kFootPartCount];
    bool               any = false;
    for (int s = 0; s < 2; ++s) {
        FootTrack& f = a.foot[s];
        f.side = kSide[s];
        for (int q = 0; q < kFootPartCount; ++q) {
            PartTrack&       pt = f.part[q];
            const BoneTrack* t = track(kPartRole[s][q]);
            if (!t) {
                pt.missing = std::string("no bone for ") + RoleName(kPartRole[s][q]);
            } else {
                for (int o = 0; o < q && pt.missing.empty(); ++o)
                    if (f.part[o].present && SamePos(t->pos, track(kPartRole[s][o])->pos))
                        pt.missing = std::string("same bone as the ") + FootPartWord(o);
            }
            if (!pt.missing.empty()) {
                a.missing.push_back(Format("%c %s: %s", f.side, FootPartWord(q), pt.missing.c_str()));
                continue;
            }
            pt.present = true;
            any = true;
            sm[s][q] = Smooth(t->pos, p.smooth_ms / 1000.0, rate);
            vel[s][q] = Velocity(sm[s][q], rate);
        }
    }
    // Hands present, smoothed, their velocities.
    std::vector<Vec3d> hsm[2], hvel[2];
    bool               hands = false;
    for (int s = 0; s < 2; ++s) {
        HandTrack&       h = a.hand[s];
        const BoneTrack* t = track(kHandRole[s]);
        h.side = kSide[s];
        if (!t) {
            h.part.missing = std::string("no bone for ") + RoleName(kHandRole[s]);
            a.hand_missing.push_back(Format("%c hand: %s", h.side, h.part.missing.c_str()));
            continue;
        }
        h.part.present = true;
        hands = true;
        hsm[s] = Smooth(t->pos, p.smooth_ms / 1000.0, rate);
        hvel[s] = Velocity(hsm[s], rate);
    }
    if (!any && !hands) {
        a.error = "no foot part (heel, toe or toe end) and no hand";
        return a;
    }

    // Ground frame: the median horizontal velocity of the lowest part (0 without a foot part).
    if (any) {
        std::vector<double> vx(n), vz(n);
        for (size_t i = 0; i < n; ++i) {
            int bs = -1, bq = -1;
            for (int s = 0; s < 2; ++s)
                for (int q = 0; q < kFootPartCount; ++q)
                    if (a.foot[s].part[q].present && (bs < 0 || sm[s][q][i].y < sm[bs][bq][i].y)) {
                        bs = s;
                        bq = q;
                    }
            vx[i] = vel[bs][bq][i].x;
            vz[i] = vel[bs][bq][i].z;
        }
        a.ground_velocity = Vec3d{Median(vx), 0.0, Median(vz)};
    }

    for (int s = 0; s < 2; ++s) {
        FootTrack& f = a.foot[s];
        for (int q = 0; q < kFootPartCount; ++q) {
            PartTrack& pt = f.part[q];
            if (!pt.present) continue;
            pt.y.resize(n);
            pt.speed.resize(n);
            pt.hspeed.resize(n);
            pt.vy.resize(n);
            for (size_t i = 0; i < n; ++i) {
                const double dx = vel[s][q][i].x - a.ground_velocity.x, dz = vel[s][q][i].z - a.ground_velocity.z;
                const double dy = vel[s][q][i].y;
                pt.y[i] = sm[s][q][i].y;
                pt.speed[i] = std::sqrt(dx * dx + dy * dy + dz * dz) / leg;
                pt.hspeed[i] = std::sqrt(dx * dx + dz * dz) / leg;
                pt.vy[i] = dy / leg;
            }
            SegmentPart(pt, leg, rate, p);
        }

        // Yaw of heel -> toe (ball, else tip) seen from above.
        const int heel = static_cast<int>(FootPart::Heel);
        const int toe = f.part[1].present ? 1 : (f.part[2].present ? 2 : -1);
        f.has_yaw = f.part[heel].present && toe > 0;
        if (!f.has_yaw) {
            a.missing.push_back(Format("%c pivot: needs the heel and the toe", f.side));
        } else {
            const std::vector<Vec3d> h = Smooth(track(kPartRole[s][heel])->pos, p.pivot_smooth_ms / 1000.0, rate);
            const std::vector<Vec3d> t = Smooth(track(kPartRole[s][toe])->pos, p.pivot_smooth_ms / 1000.0, rate);
            std::vector<double>      len(n), foot(n), raw(n);
            for (size_t i = 0; i < n; ++i) {
                const double dx = t[i].x - h[i].x, dz = t[i].z - h[i].z;
                len[i] = std::sqrt(dx * dx + dz * dz);
                foot[i] = Dist(t[i], h[i]);
                raw[i] = std::atan2(dx, dz) * 180.0 / kPi;
            }
            // The yaw holds while the vector seen from above is at least half the foot's length
            // (heel -> toe, a bone length: the median over the clip).
            const double med = Median(foot);
            std::vector<char>& valid = f.yaw_valid;
            valid.assign(n, 0);
            for (size_t i = 0; i < n; ++i) valid[i] = med > 0.0 && len[i] >= 0.5 * med;
            YawOf(raw, valid, rate, f.yaw_deg, f.yaw_rate_dps);

            // The heading of a steep foot (story 10-8d): the ball -> toe end vector, else the
            // toe bone's rotation.
            const int           ball = static_cast<int>(FootPart::Ball), tip = static_cast<int>(FootPart::Tip);
            std::vector<double> hraw(n, 0.0);
            std::vector<char>   hvalid(n, 0);
            if (f.part[ball].present && f.part[tip].present) {
                const std::vector<Vec3d> b = Smooth(track(kPartRole[s][ball])->pos, p.pivot_smooth_ms / 1000.0, rate);
                const std::vector<Vec3d> e = Smooth(track(kPartRole[s][tip])->pos, p.pivot_smooth_ms / 1000.0, rate);
                std::vector<double>      hlen(n), seg(n);
                for (size_t i = 0; i < n; ++i) {
                    const double dx = e[i].x - b[i].x, dz = e[i].z - b[i].z;
                    hlen[i] = std::sqrt(dx * dx + dz * dz);
                    seg[i] = Dist(e[i], b[i]);
                    hraw[i] = std::atan2(dx, dz) * 180.0 / kPi;
                }
                const double hmed = Median(seg);
                for (size_t i = 0; i < n; ++i) hvalid[i] = hmed > 0.0 && hlen[i] >= 0.5 * hmed;
                f.heading_source = HeadingSource::ToeEnd;
            } else if (f.part[ball].present && track(kPartRole[s][ball])->rot_world.size() == n) {
                // toe == ball here: t is the ball, h the heel. The across axis in bone space:
                // the mean of R^-1 . (up x heel -> ball from above), where the yaw holds and the
                // ball is in contact.
                const std::vector<Quatd>& rot = track(kPartRole[s][ball])->rot_world;
                const PartTrack&          bp = f.part[ball];
                Vec3d                     sum{0.0, 0.0, 0.0};
                for (size_t i = 0; i < n; ++i) {
                    if (!valid[i] || !bp.contact[i]) continue;
                    const double dx = t[i].x - h[i].x, dz = t[i].z - h[i].z, l = std::sqrt(dx * dx + dz * dz);
                    if (!(l > 0.0)) continue;
                    const Vec3d ax = Rotate(Conj(rot[i]), Vec3d{dz / l, 0.0, -dx / l});
                    sum.x += ax.x;
                    sum.y += ax.y;
                    sum.z += ax.z;
                }
                const double sl = std::sqrt(sum.x * sum.x + sum.y * sum.y + sum.z * sum.z);
                if (sl > 1e-9 && std::isfinite(sl)) {
                    const Vec3d        axis{sum.x / sl, sum.y / sl, sum.z / sl};
                    std::vector<Vec3d> across(n);
                    for (size_t i = 0; i < n; ++i) across[i] = Rotate(rot[i], axis);
                    across = Smooth(across, p.pivot_smooth_ms / 1000.0, rate);
                    const double level = std::cos(p.pivot_level_deg * kPi / 180.0);
                    for (size_t i = 0; i < n; ++i) {
                        const Vec3d& acr = across[i];
                        const double hl = std::sqrt(acr.x * acr.x + acr.z * acr.z), al = std::sqrt(hl * hl + acr.y * acr.y);
                        // forward = across x up: the same heading as heel -> ball.
                        hraw[i] = std::atan2(-acr.z, acr.x) * 180.0 / kPi;
                        hvalid[i] = al > 1e-9 && hl >= level * al;
                    }
                    f.heading_source = HeadingSource::Rotation;
                }
            }
            if (f.heading_source != HeadingSource::None) {
                std::vector<double> hdeg;
                YawOf(hraw, hvalid, rate, hdeg, f.heading_rate_dps);
                f.heading_valid = std::move(hvalid);
            }
        }
        f.pivots = FindPivots(f, static_cast<int>(n), rate, p);
        std::vector<Scuff> slides = FindSlides(f, leg, rate, p);
        for (const Scuff& sl : slides) {
            bool in_pivot = false;
            for (const Scuff& pv : f.pivots)
                if (sl.start < pv.end && pv.start < sl.end) in_pivot = true;
            if (!in_pivot) f.slides.push_back(sl);
        }
    }

    // Hands: segmented as a foot part, with the hand contact speed and switch cost.
    PhysicsParams hp = p;
    hp.contact_speed = p.hand_contact_speed;
    hp.switch_cost = p.hand_switch_cost;
    for (int s = 0; s < 2; ++s) {
        PartTrack& pt = a.hand[s].part;
        if (!pt.present) continue;
        pt.y.resize(n);
        pt.speed.resize(n);
        pt.hspeed.resize(n);
        pt.vy.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const double dx = hvel[s][i].x - a.ground_velocity.x, dz = hvel[s][i].z - a.ground_velocity.z;
            const double dy = hvel[s][i].y;
            pt.y[i] = hsm[s][i].y;
            pt.speed[i] = std::sqrt(dx * dx + dy * dy + dz * dz) / leg;
            pt.hspeed[i] = std::sqrt(dx * dx + dz * dz) / leg;
            pt.vy[i] = dy / leg;
        }
        SegmentPart(pt, leg, rate, hp);
    }
    a.ok = true;
    return a;
}

std::vector<PhysicsEvent> FootEvents(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    std::vector<PhysicsEvent> ev;
    if (!a.ok) return ev;
    const int    n = static_cast<int>(a.samples);
    const double rate = a.rate_hz;
    auto timed = [&](const PartContact& c, int q) {
        const int k = std::clamp(static_cast<int>(p.step_timing[q]), 0, kStepTimingCount - 1);
        return c.step_s[k] + p.step_offset_s[q];
    };
    for (int s = 0; s < 2; ++s) {
        const FootTrack& f = a.foot[s];
        std::vector<char> any(static_cast<size_t>(n), 0);
        for (int q = 0; q < kFootPartCount; ++q) {
            const PartTrack& pt = f.part[q];
            if (!pt.present) continue;
            for (int i = 0; i < n; ++i)
                if (pt.contact[static_cast<size_t>(i)]) any[static_cast<size_t>(i)] = 1;
            for (const PartContact& c : pt.contacts) {
                if (c.start <= 0) continue;
                PhysicsEvent e;
                e.kind = PhysicsKind::Step;
                e.side = f.side;
                e.part = q;
                e.time_s = e.start_s = e.end_s = timed(c, q);
                e.strength = c.approach;
                ev.push_back(e);
            }
        }
        for (int i = 0; i < n;) {
            if (!any[static_cast<size_t>(i)]) {
                ++i;
                continue;
            }
            const int us = i;
            while (i < n && any[static_cast<size_t>(i)]) ++i;
            const int ue = i;
            if (us > 0) {
                PhysicsEvent best;
                bool         found = false;
                // Every part that touches during this contact: the earliest step names it.
                for (int q = 0; q < kFootPartCount; ++q)
                    for (const PartContact& c : f.part[q].contacts)
                        if (f.part[q].present && c.start >= us && c.start < ue &&
                            (!found || timed(c, q) < best.time_s - kTieS)) {
                            best.kind = PhysicsKind::FootStep;
                            best.side = f.side;
                            best.part = q;
                            best.time_s = best.start_s = best.end_s = timed(c, q);
                            best.strength = c.approach;
                            found = true;
                        }
                if (found) ev.push_back(best);
            }
            if (ue < n) {
                PhysicsEvent best;
                bool         found = false;
                for (int q = 0; q < kFootPartCount; ++q)
                    for (const PartContact& c : f.part[q].contacts)
                        if (f.part[q].present && c.end == ue && (!found || c.lift_s > best.time_s + kTieS)) {
                            best.kind = PhysicsKind::LiftOff;
                            best.side = f.side;
                            best.part = q;
                            best.time_s = best.start_s = best.end_s = c.lift_s;
                            best.strength = c.departure;
                            found = true;
                        }
                if (found) ev.push_back(best);
            }
        }
        for (int kind = 0; kind < 2; ++kind)
            for (const Scuff& sc : kind ? f.pivots : f.slides) {
                PhysicsEvent e;
                e.kind = kind ? PhysicsKind::Pivot : PhysicsKind::Slide;
                e.side = f.side;
                e.part = sc.part;
                e.time_s = e.start_s = sc.start / rate;
                e.end_s = sc.end / rate;
                e.strength = sc.strength;
                ev.push_back(e);
            }
    }
    std::stable_sort(ev.begin(), ev.end(), [](const PhysicsEvent& x, const PhysicsEvent& y) {
        return std::make_tuple(std::llround(x.time_s * 1e6), x.side, static_cast<int>(x.kind), x.part) <
               std::make_tuple(std::llround(y.time_s * 1e6), y.side, static_cast<int>(y.kind), y.part);
    });
    return ev;
}

std::vector<PhysicsEvent> HandEvents(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    std::vector<PhysicsEvent> ev;
    if (!a.ok) return ev;
    const int    n = static_cast<int>(a.samples);
    const double rate = a.rate_hz;
    const int    min_len = std::max(1, SamplesOf(p.hand_min_contact_s, rate));
    const int    k = std::clamp(static_cast<int>(p.hand_timing), 0, kStepTimingCount - 1);
    for (int s = 0; s < 2; ++s) {
        const HandTrack& h = a.hand[s];
        if (!h.part.present) continue;
        for (const PartContact& c : h.part.contacts) {
            // A stop between two moves; a contact the clip start or end cuts short counts.
            if (c.end - c.start < min_len && c.start > 0 && c.end < n) continue;
            PhysicsEvent e;
            e.side = h.side;
            e.part = -1;
            if (c.start > 0 && c.approach >= p.hand_min_approach) {
                e.kind = PhysicsKind::Grab;
                e.time_s = e.start_s = e.end_s = c.step_s[k] + p.hand_offset_s;
                e.strength = c.approach;
                ev.push_back(e);
            }
            if (c.end < n && c.departure >= p.hand_min_approach) {
                e.kind = PhysicsKind::Release;
                e.time_s = e.start_s = e.end_s = c.lift_s;
                e.strength = c.departure;
                ev.push_back(e);
            }
        }
    }
    std::stable_sort(ev.begin(), ev.end(), [](const PhysicsEvent& x, const PhysicsEvent& y) {
        return std::make_tuple(std::llround(x.time_s * 1e6), x.side, static_cast<int>(x.kind)) <
               std::make_tuple(std::llround(y.time_s * 1e6), y.side, static_cast<int>(y.kind));
    });
    return ev;
}

std::vector<double> PartStepTimes(const PhysicsAnalysis& a, char side, int part, StepTiming timing, double offset_s)
{
    std::vector<double> t;
    if (!a.ok || part < 0 || part >= kFootPartCount) return t;
    const FootTrack& f = a.foot[side == 'R' ? 1 : 0];
    if (!f.part[part].present) return t;
    for (const PartContact& c : f.part[part].contacts)
        if (c.start > 0) t.push_back(c.step_s[static_cast<int>(timing)] + offset_s);
    std::sort(t.begin(), t.end());
    return t;
}

std::string PhysicsMarkerName(const PhysicsEvent& e)
{
    const int ms = static_cast<int>(std::lround((e.end_s - e.start_s) * 1000.0));
    switch (e.kind) {
    case PhysicsKind::Step: return Format("PHY %c %s %.1f", e.side, FootPartWord(e.part), e.strength);
    case PhysicsKind::FootStep: return Format("PHY %c step %s %.1f", e.side, FootPartWord(e.part), e.strength);
    case PhysicsKind::LiftOff: return Format("PHY %c lift %s %.1f", e.side, FootPartWord(e.part), e.strength);
    case PhysicsKind::Slide: return Format("PHY %c slide %s %.1f %dms", e.side, FootPartWord(e.part), e.strength, ms);
    case PhysicsKind::Pivot:
        return Format("PHY %c pivot %s %.0fdeg %dms", e.side,
                      e.part < 0                                     ? "neither"
                      : e.part == static_cast<int>(FootPart::Heel)  ? "heel"
                      : e.part == static_cast<int>(FootPart::Tip)   ? "tip"
                                                                     : "ball",
                      e.strength, ms);
    case PhysicsKind::Grab: return Format("PHY %c grab %.1f", e.side, e.strength);
    case PhysicsKind::Release: return Format("PHY %c release %.1f", e.side, e.strength);
    }
    return "PHY ?";
}

bool IsPhysicsMarkerName(const char* name)
{
    return name && std::strncmp(name, "PHY ", 4) == 0;
}

std::string PhysicsSummary(const PhysicsAnalysis& a, const std::vector<PhysicsEvent>& ev)
{
    if (!a.ok) return "skipped: " + a.error;
    int steps[kFootPartCount] = {}, foot = 0, lift = 0, slide = 0, pivot = 0;
    for (const PhysicsEvent& e : ev) {
        switch (e.kind) {
        case PhysicsKind::Step:
            if (e.part >= 0 && e.part < kFootPartCount) ++steps[e.part];
            break;
        case PhysicsKind::FootStep: ++foot; break;
        case PhysicsKind::LiftOff: ++lift; break;
        case PhysicsKind::Slide: ++slide; break;
        case PhysicsKind::Pivot: ++pivot; break;
        case PhysicsKind::Grab:
        case PhysicsKind::Release: break;  // HandSummary's
        }
    }
    std::string out = Format("leg %.2f m%s, ground (%+.2f, %+.2f) m/s; steps heel %d, toe %d, tip %d; foot steps %d, "
                             "lift-offs %d, slides %d, pivots %d",
                             a.leg_length, a.scale_note.empty() ? "" : (" (" + a.scale_note + ")").c_str(),
                             a.ground_velocity.x, a.ground_velocity.z, steps[0], steps[1], steps[2], foot, lift, slide,
                             pivot);
    if (!a.missing.empty()) {
        out += "; missing: ";
        for (size_t i = 0; i < a.missing.size(); ++i) out += (i ? ", " : "") + a.missing[i];
    }
    return out;
}

std::string HandSummary(const PhysicsAnalysis& a, const std::vector<PhysicsEvent>& ev)
{
    if (!a.ok) return "hands: skipped: " + a.error;
    int grabs = 0, releases = 0;
    for (const PhysicsEvent& e : ev) {
        if (e.kind == PhysicsKind::Grab) ++grabs;
        if (e.kind == PhysicsKind::Release) ++releases;
    }
    std::string out = Format("hands: grabs %d, releases %d", grabs, releases);
    if (!a.hand_missing.empty()) {
        out += "; missing: ";
        for (size_t i = 0; i < a.hand_missing.size(); ++i) out += (i ? ", " : "") + a.hand_missing[i];
    }
    return out;
}

std::string PhysicsEventLines(const std::vector<PhysicsEvent>& ev, const std::string& indent)
{
    std::string out;
    for (const PhysicsEvent& e : ev) {
        out += indent + Format("%8.3f s  %s", e.time_s, PhysicsMarkerName(e).c_str());
        if (e.kind == PhysicsKind::Slide || e.kind == PhysicsKind::Pivot) out += Format("  (to %.3f s)", e.end_s);
        out += "\n";
    }
    return out;
}

std::string StepTimingText(const PhysicsParams& p)
{
    std::string out;
    for (int q = 0; q < kFootPartCount; ++q)
        out += Format("%s%s %s %+.0f ms", q ? ", " : "", FootPartWord(q), StepTimingName(p.step_timing[q]),
                      p.step_offset_s[q] * 1000.0);
    return out;
}

}  // namespace rav
