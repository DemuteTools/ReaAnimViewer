// SPDX-License-Identifier: MIT
//
// The rule engine (see bone_events.h). Pure C++17, host-tested.

#include "bone_events.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace rav {
namespace {

bool TracksFit(const std::vector<BoneTrack>& tracks)
{
    if (tracks.empty()) return false;
    const size_t n = tracks[0].pos.size();
    if (n == 0 || !(tracks[0].rate_hz > 0.0) || !std::isfinite(tracks[0].rate_hz)) return false;
    for (const BoneTrack& t : tracks) {
        if (t.pos.size() != n || t.rate_hz != tracks[0].rate_hz) return false;
        for (const Vec3d& p : t.pos)
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
    }
    return true;
}

bool IndicesFit(const std::vector<int>& idx, size_t n_tracks)
{
    if (idx.empty()) return false;
    for (int i : idx)
        if (i < 0 || static_cast<size_t>(i) >= n_tracks) return false;
    return true;
}

// The combined point at sample i. `floors` (optional, same size as idx): each bone's own
// floor, taken off its height before the combine.
Vec3d CombineAt(const std::vector<BoneTrack>& tracks, const std::vector<int>& idx, Combine mode,
                const std::vector<double>* floors, size_t i)
{
    auto at = [&](size_t k) {
        Vec3d p = tracks[idx[k]].pos[i];
        if (floors) p.y -= (*floors)[k];
        return p;
    };
    switch (mode) {
    case Combine::Single:
        return at(0);
    case Combine::Average: {
        Vec3d s;
        for (size_t k = 0; k < idx.size(); ++k) {
            const Vec3d p = at(k);
            s.x += p.x;
            s.y += p.y;
            s.z += p.z;
        }
        const double inv = 1.0 / static_cast<double>(idx.size());
        s.x *= inv;
        s.y *= inv;
        s.z *= inv;
        return s;
    }
    case Combine::Lowest:
    case Combine::Highest: {
        Vec3d best = at(0);
        for (size_t k = 1; k < idx.size(); ++k) {
            const Vec3d p = at(k);
            if (mode == Combine::Lowest ? p.y < best.y : p.y > best.y) best = p;
        }
        return best;
    }
    }
    return at(0);
}

// Zero-phase Gaussian smoothing (truncated, renormalised kernel at the edges).
std::vector<Vec3d> Smooth(const std::vector<Vec3d>& in, double sigma_samples)
{
    if (!(sigma_samples >= 0.3) || in.size() < 3) return in;
    const int r = static_cast<int>(std::ceil(3.0 * sigma_samples));
    std::vector<double> w(static_cast<size_t>(r) + 1);
    for (int k = 0; k <= r; ++k) w[k] = std::exp(-0.5 * (k * k) / (sigma_samples * sigma_samples));
    const int n = static_cast<int>(in.size());
    std::vector<Vec3d> out(in.size());
    for (int i = 0; i < n; ++i) {
        double sx = 0, sy = 0, sz = 0, sw = 0;
        for (int j = std::max(0, i - r); j <= std::min(n - 1, i + r); ++j) {
            const double wk = w[static_cast<size_t>(std::abs(j - i))];
            sx += wk * in[j].x;
            sy += wk * in[j].y;
            sz += wk * in[j].z;
            sw += wk;
        }
        out[i] = Vec3d{sx / sw, sy / sw, sz / sw};
    }
    return out;
}

double AxisValue(const Vec3d& v, Axis axis, bool magnitude_components)
{
    double c = 0.0;
    switch (axis) {
    case Axis::Vertical:
    case Axis::Y: c = v.y; break;
    case Axis::X: c = v.x; break;
    case Axis::Z: c = v.z; break;
    case Axis::Horizontal: return std::sqrt(v.x * v.x + v.z * v.z);
    case Axis::Total: return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    }
    return magnitude_components ? std::fabs(c) : c;
}

double Percentile(std::vector<double> v, double pct)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double p = std::min(100.0, std::max(0.0, pct)) / 100.0 * static_cast<double>(v.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(p));
    const size_t hi = std::min(v.size() - 1, lo + 1);
    const double f = p - static_cast<double>(lo);
    return v[lo] + (v[hi] - v[lo]) * f;
}

// Otsu's split of `v`: the level that best separates a low and a high cluster.
double OtsuSplit(const std::vector<double>& v)
{
    const auto mm = std::minmax_element(v.begin(), v.end());
    const double lo = *mm.first, hi = *mm.second;
    if (!(hi - lo > 1e-12)) return hi;
    constexpr int kBins = 256;
    std::vector<double> hist(kBins, 0.0);
    for (double x : v) {
        int b = static_cast<int>((x - lo) / (hi - lo) * kBins);
        hist[std::min(kBins - 1, std::max(0, b))] += 1.0;
    }
    const double total = static_cast<double>(v.size());
    double sum_all = 0.0;
    for (int b = 0; b < kBins; ++b) sum_all += b * hist[b];
    double w0 = 0.0, sum0 = 0.0, best = -1.0;
    int best_b = 0;
    for (int b = 0; b < kBins - 1; ++b) {
        w0 += hist[b];
        sum0 += b * hist[b];
        const double w1 = total - w0;
        if (w0 <= 0.0 || w1 <= 0.0) continue;
        const double m0 = sum0 / w0, m1 = (sum_all - sum0) / w1;
        const double between = w0 * w1 * (m0 - m1) * (m0 - m1);
        if (between > best) {
            best = between;
            best_b = b;
        }
    }
    return lo + (hi - lo) * (best_b + 1) / kBins;  // the upper edge of the last low bin
}

double InterpAt(const std::vector<double>& s, double t, double rate)
{
    if (s.empty()) return 0.0;
    const double p = t * rate;
    if (p <= 0.0) return s.front();
    const size_t i = static_cast<size_t>(std::floor(p));
    if (i + 1 >= s.size()) return s.back();
    const double f = p - static_cast<double>(i);
    return s[i] + (s[i + 1] - s[i]) * f;
}

// Sets the floor of a floor-referenced spec from the clip (AnalyseOptions).
void EstimateFloor(SignalSpec& spec, const std::vector<BoneTrack>& tracks, const AnalyseOptions& o)
{
    if (spec.quantity != Quantity::Point || spec.reference != Reference::Floor ||
        !IndicesFit(spec.bones, tracks.size()))
        return;
    const size_t n = tracks[0].pos.size();
    spec.floor_y = 0.0;
    spec.bone_floors.clear();
    if (o.per_bone_floor) {
        for (int b : spec.bones) {
            std::vector<double> y(n);
            for (size_t i = 0; i < n; ++i) y[i] = tracks[b].pos[i].y;
            spec.bone_floors.push_back(Percentile(std::move(y), o.floor_percentile));
        }
    }
    std::vector<double> low(n);
    for (size_t i = 0; i < n; ++i)
        low[i] = CombineAt(tracks, spec.bones, spec.combine, spec.bone_floors.empty() ? nullptr : &spec.bone_floors,
                           i)
                     .y;
    spec.floor_y = Percentile(std::move(low), o.floor_percentile);
}

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = 180.0 / kPi;

// ---- Spike 10-7a: stillness and relative drop ------------------------------------------------

// A measure's window in samples (MeasureWindowMs), at most the clip and at least one sample
// (a window shorter than a sample would read no motion at all).
size_t WindowSamples(const SignalSpec& spec, double rate, size_t n)
{
    const double k = MeasureWindowMs(spec) / 1000.0 * rate;
    if (!(k > 0.0)) return 0;
    const size_t w = static_cast<size_t>(std::floor(std::min(k, static_cast<double>(n)) + 1e-9));
    return (w == 0 && n > 1) ? 1 : w;
}

// The components of v that an axis reads (the others zeroed): one for vertical / X / Y / Z,
// X and Z for horizontal, all three for total.
Vec3d OnAxis(const Vec3d& v, Axis axis)
{
    switch (axis) {
    case Axis::Vertical:
    case Axis::Y: return Vec3d{0.0, v.y, 0.0};
    case Axis::X: return Vec3d{v.x, 0.0, 0.0};
    case Axis::Z: return Vec3d{0.0, 0.0, v.z};
    case Axis::Horizontal: return Vec3d{v.x, 0.0, v.z};
    case Axis::Total: return v;
    }
    return v;
}

// Stillness: at each sample i, over samples i..i+w, the largest distance from the window's
// mid-range point (per component). On one component, half the range; for a slide along a
// line, half its length. The window is clamped to the clip at full length: the last w
// samples read the last full window, so a bone still moving at the clip end does not read
// still there.
std::vector<double> StillnessSeries(const std::vector<Vec3d>& p, size_t w)
{
    const size_t n = p.size();
    std::vector<double> out(n, 0.0);
    if (n == 0) return out;
    const size_t len = std::min(w, n - 1);
    for (size_t i = 0; i < n; ++i) {
        const size_t s = std::min(i, n - 1 - len);
        const size_t e = s + len;
        Vec3d lo = p[s], hi = p[s];
        for (size_t j = s + 1; j <= e; ++j) {
            lo.x = std::min(lo.x, p[j].x);
            lo.y = std::min(lo.y, p[j].y);
            lo.z = std::min(lo.z, p[j].z);
            hi.x = std::max(hi.x, p[j].x);
            hi.y = std::max(hi.y, p[j].y);
            hi.z = std::max(hi.z, p[j].z);
        }
        const Vec3d c{0.5 * (lo.x + hi.x), 0.5 * (lo.y + hi.y), 0.5 * (lo.z + hi.z)};
        double r2 = 0.0;
        for (size_t j = s; j <= e; ++j) {
            const double dx = p[j].x - c.x, dy = p[j].y - c.y, dz = p[j].z - c.z;
            r2 = std::max(r2, dx * dx + dy * dy + dz * dz);
        }
        out[i] = std::sqrt(r2);
    }
    return out;
}

// Relative drop: speed[i] over the peak of speed[i-w..i] (clamped to the clip), the peak
// floored at kRelativeDropPeakFloor of the clip's 95th-percentile speed, so the noise of a
// bone resting after a move reads near 0. A peak under 1e-9 per second is no motion (the
// rounding left by smoothing an exact hold): it reads 0. The floor follows the clip: a bone
// that only jitters for the whole clip still compares its noise with its noise. `speed`
// holds magnitudes, so the result is in [0, 1].
std::vector<double> RelativeDropSeries(const std::vector<double>& speed, size_t w)
{
    constexpr double kNoMotion = 1e-9;
    const size_t n = speed.size();
    std::vector<double> out(n, 0.0);
    if (n == 0) return out;
    const double floor = kRelativeDropPeakFloor * Percentile(speed, 95.0);
    for (size_t i = 0; i < n; ++i) {
        double peak = floor;
        for (size_t j = i > w ? i - w : 0; j <= i; ++j) peak = std::max(peak, speed[j]);
        out[i] = peak > kNoMotion ? speed[i] / peak : 0.0;
    }
    return out;
}

// The measure of a scalar series (an angle): position = itself; speed / acceleration =
// centred differences on the smoothed series, magnitudes unless keep_sign; stillness = half
// the range over the window ahead; relative drop = of the speed magnitude.
std::vector<double> ScalarMeasure(const std::vector<double>& q, const SignalSpec& spec, double smooth_ms, double rate)
{
    const size_t n = q.size();
    switch (spec.measure) {
    case Measure::Position: return q;
    case Measure::Stillness: {
        std::vector<Vec3d> v(n);
        for (size_t i = 0; i < n; ++i) v[i].x = q[i];
        return StillnessSeries(v, WindowSamples(spec, rate, n));
    }
    case Measure::RelativeDrop: {
        SignalSpec speed = spec;
        speed.measure = Measure::Speed;
        speed.keep_sign = false;
        return RelativeDropSeries(ScalarMeasure(q, speed, smooth_ms, rate), WindowSamples(spec, rate, n));
    }
    case Measure::Speed:
    case Measure::Acceleration: break;
    }
    const Measure measure = spec.measure;
    const bool keep_sign = spec.keep_sign;
    std::vector<double> out(n, 0.0);
    if (n < 2) return out;
    std::vector<Vec3d> v(n);
    for (size_t i = 0; i < n; ++i) v[i].x = q[i];
    const std::vector<Vec3d> s = Smooth(v, smooth_ms / 1000.0 * rate);
    if (measure == Measure::Speed) {
        for (size_t i = 0; i < n; ++i) {
            const size_t a = (i == 0) ? 0 : i - 1;
            const size_t b = (i + 1 == n) ? i : i + 1;
            out[i] = (s[b].x - s[a].x) * rate / static_cast<double>(b - a);
        }
    } else {
        if (n < 3) return out;
        for (size_t i = 1; i + 1 < n; ++i) out[i] = (s[i + 1].x - 2 * s[i].x + s[i - 1].x) * rate * rate;
        out[0] = out[1];
        out[n - 1] = out[n - 2];
    }
    if (!keep_sign)
        for (double& x : out) x = std::fabs(x);
    return out;
}

// Joint flexion (180 - angle a-b-c), the interior angle a-b-c, or the unwrapped yaw of
// a -> b, in degrees, per sample. A degenerate sample (zero-length segment) keeps the
// previous value.
std::vector<double> AngleSeries(const SignalSpec& spec, const std::vector<BoneTrack>& tracks)
{
    const size_t n = tracks[0].pos.size();
    std::vector<double> q(n, 0.0);
    // A degenerate start reads as straight: 0 flexion, or an interior angle of 180.
    double prev = spec.quantity == Quantity::InteriorAngle ? 180.0 : 0.0;
    for (size_t i = 0; i < n; ++i) {
        double v = prev;
        if (spec.quantity == Quantity::JointAngle || spec.quantity == Quantity::InteriorAngle) {
            const Vec3d& a = tracks[spec.bones[0]].pos[i];
            const Vec3d& b = tracks[spec.bones[1]].pos[i];
            const Vec3d& c = tracks[spec.bones[2]].pos[i];
            const double ux = a.x - b.x, uy = a.y - b.y, uz = a.z - b.z;
            const double wx = c.x - b.x, wy = c.y - b.y, wz = c.z - b.z;
            const double lu = std::sqrt(ux * ux + uy * uy + uz * uz), lw = std::sqrt(wx * wx + wy * wy + wz * wz);
            if (lu > 1e-12 && lw > 1e-12) {
                const double cosv = std::min(1.0, std::max(-1.0, (ux * wx + uy * wy + uz * wz) / (lu * lw)));
                const double interior = std::acos(cosv) * kDeg;
                v = spec.quantity == Quantity::JointAngle ? 180.0 - interior : interior;
            }
        } else {
            const Vec3d& a = tracks[spec.bones[0]].pos[i];
            const Vec3d& b = tracks[spec.bones[1]].pos[i];
            const double dx = b.x - a.x, dz = b.z - a.z;
            if (std::sqrt(dx * dx + dz * dz) > 1e-12) {
                v = std::atan2(dx, dz) * kDeg;
                if (i > 0) {  // unwrap: the nearest turn to the previous value
                    while (v - prev > 180.0) v -= 360.0;
                    while (v - prev < -180.0) v += 360.0;
                }
            }
        }
        q[i] = v;
        prev = v;
    }
    return q;
}

// ---- Rotation (10-4 follow-up) ---------------------------------------------------------------

// The orientations a rotation signal reads: the parent-relative ones (Reference::Parent) or
// the world ones (Reference::Floor). Null when the reference does not apply or the track was
// not sampled with orientations (an offline dump).
const std::vector<Quatd>* RotationsOf(const SignalSpec& spec, const std::vector<BoneTrack>& tracks)
{
    if (spec.bones.size() != 1) return nullptr;
    const BoneTrack& t = tracks[static_cast<size_t>(spec.bones[0])];
    const std::vector<Quatd>* r = nullptr;
    if (spec.reference == Reference::Parent) r = &t.rot_parent;
    else if (spec.reference == Reference::Floor) r = &t.rot_world;
    if (!r || r->size() != t.pos.size()) return nullptr;
    for (const Quatd& q : *r)
        if (!std::isfinite(q.w) || !std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)) return nullptr;
    return r;
}

Quatd Normalized(const Quatd& q)
{
    const double l = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (!(l > 1e-12)) return Quatd{};
    return Quatd{q.w / l, q.x / l, q.y / l, q.z / l};
}

// a * conj(b): the rotation that takes b to a (in the reference frame).
Quatd MulConj(const Quatd& a, const Quatd& b)
{
    const double bw = b.w, bx = -b.x, by = -b.y, bz = -b.z;
    return Quatd{a.w * bw - a.x * bx - a.y * by - a.z * bz, a.w * bx + a.x * bw + a.y * bz - a.z * by,
                 a.w * by - a.x * bz + a.y * bw + a.z * bx, a.w * bz + a.x * by - a.y * bx + a.z * bw};
}

// The XYZ Euler angles (rotate order XYZ: R = Rz(z) * Ry(y) * Rx(x)), in degrees.
Vec3d EulerXYZ(const Quatd& in)
{
    const Quatd q = Normalized(in);
    const double r00 = 1.0 - 2.0 * (q.y * q.y + q.z * q.z);
    const double r10 = 2.0 * (q.x * q.y + q.w * q.z);
    const double r20 = 2.0 * (q.x * q.z - q.w * q.y);
    const double r21 = 2.0 * (q.y * q.z + q.w * q.x);
    const double r22 = 1.0 - 2.0 * (q.x * q.x + q.y * q.y);
    const double r11 = 1.0 - 2.0 * (q.x * q.x + q.z * q.z);
    const double r12 = 2.0 * (q.y * q.z - q.w * q.x);
    const double sy = std::min(1.0, std::max(-1.0, -r20));
    Vec3d e;
    e.y = std::asin(sy);
    if (std::sqrt(r21 * r21 + r22 * r22) > 1e-9) {
        e.x = std::atan2(r21, r22);
        e.z = std::atan2(r10, r00);
    } else {  // gimbal lock: Y at +-90, the X / Z split is free (all on X)
        e.x = std::atan2(-r12, r11);
        e.z = 0.0;
    }
    return Vec3d{e.x * kDeg, e.y * kDeg, e.z * kDeg};
}

// Which Euler axis a rotation signal reads: 0 = X, 1 = Y, 2 = Z, -1 = total (the turning rate).
int RotationAxis(const SignalSpec& spec)
{
    switch (spec.axis) {
    case Axis::X: return 0;
    case Axis::Y:
    case Axis::Vertical: return 1;
    case Axis::Z: return 2;
    case Axis::Total:
    case Axis::Horizontal:
        switch (spec.measure) {
        case Measure::Position: return 0;  // an angle has no total: X
        case Measure::Speed:
        case Measure::Acceleration: return -1;  // the turning rate
        case Measure::Stillness:
        case Measure::RelativeDrop: return -1;  // total too, which they do not fit (RotationSignal)
        }
        return -1;
    }
    return 0;
}

// The rotation signal: the unwrapped Euler angle on one axis (then ScalarMeasure), or the
// magnitude of the angular velocity / acceleration for total. Empty when it does not fit.
std::vector<double> RotationSignal(const SignalSpec& spec, const std::vector<BoneTrack>& tracks, double smooth_ms)
{
    const std::vector<Quatd>* rots = RotationsOf(spec, tracks);
    if (!rots) return {};
    const std::vector<Quatd>& r = *rots;
    const size_t n = r.size();
    const double rate = tracks[0].rate_hz;
    const int axis = RotationAxis(spec);
    if (axis >= 0) {
        // XYZ Euler angles have two equivalent triples, (x, y, z) and (x+180, 180-y, z+180):
        // y alone stays within +-90. Per sample, each component of both is unwrapped to the
        // nearest turn of the previous sample's, and the triple closest to the previous one
        // is kept, so a turn past 90 about Y stays continuous instead of folding back.
        auto near_turn = [](double v, double ref) {
            while (v - ref > 180.0) v -= 360.0;
            while (v - ref < -180.0) v += 360.0;
            return v;
        };
        std::vector<double> q(n, 0.0);
        Vec3d prev;
        for (size_t i = 0; i < n; ++i) {
            Vec3d e = EulerXYZ(r[i]);
            if (i > 0) {
                const Vec3d a{near_turn(e.x, prev.x), near_turn(e.y, prev.y), near_turn(e.z, prev.z)};
                const Vec3d b{near_turn(e.x + 180.0, prev.x), near_turn(180.0 - e.y, prev.y),
                              near_turn(e.z + 180.0, prev.z)};
                auto dist = [&](const Vec3d& t) {
                    return (t.x - prev.x) * (t.x - prev.x) + (t.y - prev.y) * (t.y - prev.y) +
                           (t.z - prev.z) * (t.z - prev.z);
                };
                e = dist(b) < dist(a) ? b : a;
            }
            q[i] = axis == 0 ? e.x : axis == 1 ? e.y : e.z;
            prev = e;
        }
        return ScalarMeasure(q, spec, smooth_ms, rate);
    }
    // Total: the angular velocity (deg/s, a vector in the reference frame) by centred
    // differences of the orientation, smoothed; speed = its length, acceleration = the
    // length of its derivative. Stillness and relative drop have no total: they do not fit.
    switch (spec.measure) {
    case Measure::Speed:
    case Measure::Acceleration: break;
    case Measure::Position:  // reads as X (RotationAxis): never here
    case Measure::Stillness:
    case Measure::RelativeDrop: return {};
    }
    std::vector<double> out(n, 0.0);
    if (n < 2) return out;
    std::vector<Vec3d> w(n);
    for (size_t i = 0; i < n; ++i) {
        const size_t a = (i == 0) ? 0 : i - 1;
        const size_t b = (i + 1 == n) ? i : i + 1;
        Quatd d = MulConj(Normalized(r[b]), Normalized(r[a]));
        if (d.w < 0.0) d = Quatd{-d.w, -d.x, -d.y, -d.z};  // the short way round
        const double s = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (s > 1e-15) {
            const double ang = 2.0 * std::atan2(s, d.w) * kDeg * rate / static_cast<double>(b - a);
            w[i] = Vec3d{d.x / s * ang, d.y / s * ang, d.z / s * ang};
        }
    }
    const std::vector<Vec3d> ws = Smooth(w, smooth_ms / 1000.0 * rate);
    if (spec.measure == Measure::Speed) {
        for (size_t i = 0; i < n; ++i) out[i] = std::sqrt(ws[i].x * ws[i].x + ws[i].y * ws[i].y + ws[i].z * ws[i].z);
        return out;
    }
    for (size_t i = 0; i < n; ++i) {
        const size_t a = (i == 0) ? 0 : i - 1;
        const size_t b = (i + 1 == n) ? i : i + 1;
        const double inv = rate / static_cast<double>(b - a);
        const double dx = (ws[b].x - ws[a].x) * inv, dy = (ws[b].y - ws[a].y) * inv, dz = (ws[b].z - ws[a].z) * inv;
        out[i] = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    return out;
}

// Speed or acceleration (`measure`, only these two: EvaluateSignal's switch) of the relative
// point on the axis: centred differences on the smoothed positions, magnitudes unless keep_sign.
std::vector<double> PointDerivative(const std::vector<Vec3d>& rel, Measure measure, Axis axis, bool keep_sign,
                                    double smooth_ms, double rate)
{
    const size_t n = rel.size();
    std::vector<double> out(n, 0.0);
    const std::vector<Vec3d> s = Smooth(rel, smooth_ms / 1000.0 * rate);
    const bool mag = !keep_sign;
    if (n < 2) return out;
    std::vector<Vec3d> d(n);
    if (measure == Measure::Speed) {
        for (size_t i = 0; i < n; ++i) {
            const size_t a = (i == 0) ? 0 : i - 1;
            const size_t b = (i + 1 == n) ? i : i + 1;
            const double inv = rate / static_cast<double>(b - a);
            d[i] = Vec3d{(s[b].x - s[a].x) * inv, (s[b].y - s[a].y) * inv, (s[b].z - s[a].z) * inv};
        }
    } else {
        if (n < 3) return out;
        const double r2 = rate * rate;
        for (size_t i = 1; i + 1 < n; ++i)
            d[i] = Vec3d{(s[i + 1].x - 2 * s[i].x + s[i - 1].x) * r2, (s[i + 1].y - 2 * s[i].y + s[i - 1].y) * r2,
                         (s[i + 1].z - 2 * s[i].z + s[i - 1].z) * r2};
        d[0] = d[1];
        d[n - 1] = d[n - 2];
    }
    for (size_t i = 0; i < n; ++i) out[i] = AxisValue(d[i], axis, mag);
    return out;
}

// The series an event's values are read from (DetectTrace and EventValuesAt share them).
struct ValueSeries {
    std::vector<double> speed;     // total speed of the first condition's signal
    std::vector<double> strength;  // the strength signal (the speed when it has no bones)
    double              sign = 1.0;
};

ValueSeries MakeValueSeries(const Block& blk, const std::vector<BoneTrack>& tracks, double smooth_ms, size_t n)
{
    ValueSeries vs;
    if (!blk.conditions.empty()) {
        SignalSpec speed_spec = blk.conditions[0].signal;
        speed_spec.measure = Measure::Speed;
        speed_spec.axis = Axis::Total;
        speed_spec.keep_sign = false;
        vs.speed = EvaluateSignal(speed_spec, tracks, smooth_ms);
    }
    vs.sign = blk.strength_sign;
    if (blk.strength_signal.bones.empty()) {
        vs.strength = vs.speed;
        vs.sign = 1.0;
    } else {
        vs.strength = EvaluateSignal(blk.strength_signal, tracks, smooth_ms);
    }
    if (vs.strength.size() != n) vs.strength.assign(n, 0.0);
    return vs;
}

// Strength (the peak of sign * strength over the window before tl) and speed at landing tl.
void ValuesAt(const ValueSeries& vs, const Block& blk, double rate, size_t n, double tl, double* strength,
              double* speed)
{
    const double dt = 1.0 / rate;
    const double w = std::max(0.0, blk.strength_window_ms) / 1000.0;
    double peak = vs.sign * InterpAt(vs.strength, tl, rate);
    for (size_t k = 0; k < n; ++k) {
        const double tk = static_cast<double>(k) * dt;
        if (tk < tl - w - 1e-12) continue;
        if (tk > tl) break;
        peak = std::max(peak, vs.sign * vs.strength[k]);
    }
    *strength = peak;
    *speed = InterpAt(vs.speed, tl, rate);
}

}  // namespace

double MeasureWindowMs(const SignalSpec& spec)
{
    const bool own = spec.window_ms > 0.0 && std::isfinite(spec.window_ms);
    switch (spec.measure) {
    case Measure::Stillness: return own ? spec.window_ms : kStillnessWindowMs;
    case Measure::RelativeDrop: return own ? spec.window_ms : kRelativeDropWindowMs;
    case Measure::Position:
    case Measure::Speed:
    case Measure::Acceleration: return 0.0;
    }
    return 0.0;
}

bool EventValuesAt(const Block& blk, const std::vector<BoneTrack>& tracks, const DetectOptions& opts, double t,
                   double* strength, double* speed)
{
    double s = 0.0, v = 0.0;
    if (strength) *strength = 0.0;
    if (speed) *speed = 0.0;
    if (!TracksFit(tracks) || blk.conditions.empty() || !std::isfinite(t)) return false;
    const size_t n = tracks[0].pos.size();
    const double rate = tracks[0].rate_hz;
    const ValueSeries vs = MakeValueSeries(blk, tracks, opts.smooth_ms, n);
    if (vs.speed.size() != n) return false;
    ValuesAt(vs, blk, rate, n, t - blk.offset_ms / 1000.0, &s, &v);
    if (strength) *strength = s;
    if (speed) *speed = v;
    return true;
}

std::vector<double> EvaluateSignal(const SignalSpec& spec, const std::vector<BoneTrack>& tracks, double smooth_ms)
{
    if (!TracksFit(tracks) || !IndicesFit(spec.bones, tracks.size())) return {};
    if (spec.quantity == Quantity::Rotation) return RotationSignal(spec, tracks, smooth_ms);
    if (spec.quantity != Quantity::Point) {
        const size_t need = (spec.quantity == Quantity::Yaw) ? 2 : 3;
        if (spec.bones.size() != need) return {};
        return ScalarMeasure(AngleSeries(spec, tracks), spec, smooth_ms, tracks[0].rate_hz);
    }
    if (spec.reference == Reference::Parent) return {};  // a point has no parent reference
    if (spec.reference == Reference::Bones && !IndicesFit(spec.ref_bones, tracks.size())) return {};
    const bool own_floors = spec.reference == Reference::Floor && !spec.bone_floors.empty();
    if (own_floors && spec.bone_floors.size() != spec.bones.size()) return {};

    const size_t n = tracks[0].pos.size();
    const double rate = tracks[0].rate_hz;
    std::vector<Vec3d> rel(n);
    for (size_t i = 0; i < n; ++i) {
        Vec3d p = CombineAt(tracks, spec.bones, spec.combine, own_floors ? &spec.bone_floors : nullptr, i);
        if (spec.reference == Reference::Floor) {
            p.y -= spec.floor_y;
        } else {
            const Vec3d r = CombineAt(tracks, spec.ref_bones, spec.ref_combine, nullptr, i);
            p.x -= r.x;
            p.y -= r.y;
            p.z -= r.z;
        }
        rel[i] = p;
    }

    switch (spec.measure) {
    case Measure::Position: {
        std::vector<double> out(n, 0.0);
        for (size_t i = 0; i < n; ++i) out[i] = AxisValue(rel[i], spec.axis, false);
        return out;
    }
    case Measure::Stillness:
        for (Vec3d& p : rel) p = OnAxis(p, spec.axis);
        return StillnessSeries(rel, WindowSamples(spec, rate, n));
    case Measure::RelativeDrop:
        return RelativeDropSeries(PointDerivative(rel, Measure::Speed, spec.axis, false, smooth_ms, rate),
                                  WindowSamples(spec, rate, n));
    case Measure::Speed:
    case Measure::Acceleration: break;
    }
    return PointDerivative(rel, spec.measure, spec.axis, spec.keep_sign, smooth_ms, rate);
}

DetectionTrace DetectTrace(const std::vector<Block>& blocks, const std::vector<BoneTrack>& tracks,
                           const DetectOptions& opts)
{
    DetectionTrace trace;
    trace.blocks.resize(blocks.size());
    std::vector<Event>& all = trace.events;
    if (!TracksFit(tracks)) return trace;
    const size_t n = tracks[0].pos.size();
    const double rate = tracks[0].rate_hz;
    const double dt = 1.0 / rate;
    const double duration = static_cast<double>(n - 1) * dt;
    trace.rate_hz = rate;
    trace.samples = n;

    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        const Block& blk = blocks[bi];
        BlockTrace&  bt = trace.blocks[bi];
        if (!blk.enabled || blk.conditions.empty()) continue;
        const size_t nc = blk.conditions.size();

        // Each condition's in/out state (with hysteresis) and its latest entry time.
        std::vector<std::vector<double>> vals(nc);
        bool bad = false;
        for (size_t c = 0; c < nc; ++c) {
            vals[c] = EvaluateSignal(blk.conditions[c].signal, tracks, opts.smooth_ms);
            if (vals[c].size() != n) bad = true;
        }
        if (bad) continue;

        std::vector<char> active(n, 0);
        std::vector<double> entry_at(n, 0.0);  // while in: the crossing that completed the AND
        std::vector<double> trig_at(n, 0.0);   // while in: the trigger's (condition 0's) crossing
        std::vector<std::vector<double>> cond_at(n);  // while in: every condition's entry
        std::vector<char> inside(nc, 0);
        std::vector<double> last_entry(nc, 0.0);
        std::vector<std::vector<char>> holds(nc, std::vector<char>(n, 0));
        for (size_t i = 0; i < n; ++i) {
            bool all_in = true;
            for (size_t c = 0; c < nc; ++c) {
                const Condition& cd = blk.conditions[c];
                const double th = cd.threshold;
                const double m = std::max(0.0, cd.margin);
                const double v = vals[c][i];
                const bool below = cd.dir == Direction::Below;
                bool now;
                if (inside[c])
                    now = below ? !(v > th + m) : !(v < th - m);
                else
                    now = below ? (v < th) : (v > th);
                if (now && !inside[c]) {
                    double t = static_cast<double>(i) * dt;
                    if (i > 0) {
                        const double v0 = vals[c][i - 1];
                        const double den = v - v0;
                        double f = (std::fabs(den) > 1e-300) ? (th - v0) / den : 1.0;
                        f = std::min(1.0, std::max(0.0, f));
                        t = (static_cast<double>(i - 1) + f) * dt;
                    }
                    last_entry[c] = t;
                }
                inside[c] = now ? 1 : 0;
                holds[c][i] = inside[c];
                if (!now) all_in = false;
            }
            active[i] = all_in ? 1 : 0;
            if (all_in) {
                double t = 0.0;
                for (size_t c = 0; c < nc; ++c) t = std::max(t, last_entry[c]);
                entry_at[i] = t;
                trig_at[i] = last_entry[0];
                cond_at[i] = last_entry;
            }
        }

        // Strength and speed series (shared with EventValuesAt).
        const ValueSeries series = MakeValueSeries(blk, tracks, opts.smooth_ms, n);

        std::vector<Event> evs;
        bool has_last = false;
        double last_t = 0.0;     // the last event's crossing
        bool has_used = false;
        double used_trig = 0.0;  // the last trigger entry spent (fired, or dropped by cooldown / edge margin)
        const double cooldown = std::max(0.0, blk.cooldown_ms) / 1000.0;
        const double hold = std::max(0.0, blk.min_hold_ms) / 1000.0;
        for (size_t i = 1; i < n; ++i) {
            if (!active[i] || active[i - 1]) continue;  // fire only on the way in
            const double tc = entry_at[i];
            // Where the event lands: the crossing, or the peak of a condition's signal over
            // the span where this AND holds (sub-sample, parabolic).
            double tl = tc;
            if (blk.landing == Landing::PeakOf && blk.peak_condition >= 0 &&
                static_cast<size_t>(blk.peak_condition) < nc) {
                const std::vector<double>& y = vals[static_cast<size_t>(blk.peak_condition)];
                size_t end = i;
                while (end + 1 < n && active[end + 1]) ++end;
                size_t k = i;
                for (size_t j = i; j <= end; ++j)
                    if (blk.peak_max ? y[j] > y[k] : y[j] < y[k]) k = j;
                double delta = 0.0;
                if (k > 0 && k + 1 < n) {
                    const double den = y[k - 1] - 2.0 * y[k] + y[k + 1];
                    if (std::fabs(den) > 1e-300) delta = 0.5 * (y[k - 1] - y[k + 1]) / den;
                    delta = std::min(0.5, std::max(-0.5, delta));
                }
                tl = (static_cast<double>(k) + delta) * dt;
            }
            // One chance per entry of the trigger: a qualifier flickering while the trigger
            // stays in never re-fires, nor fires after an entry dropped by the cooldown or
            // the edge margin (a min-hold failure may retry).
            if (has_used && trig_at[i] <= used_trig + 1e-12) continue;
            if (has_last && tl - last_t < cooldown - 1e-12) {  // in cooldown: wait for the next way in
                has_used = true;
                used_trig = trig_at[i];
                continue;
            }
            bool held = true;
            for (size_t j = i + 1; j < n && static_cast<double>(j) * dt <= tc + hold + 1e-12; ++j)
                if (!active[j]) {
                    held = false;
                    break;
                }
            if (!held) continue;
            // Edge margin on the crossing (T-pose artefacts at the clip ends).
            const double edge = std::max(0.0, opts.edge_margin_ms) / 1000.0;
            has_used = true;
            used_trig = trig_at[i];
            if (edge > 0.0 && (tl < edge - 1e-12 || tl > duration - edge + 1e-12)) continue;
            has_last = true;
            last_t = tl;

            Event e;
            e.time_s = tl + blk.offset_ms / 1000.0;
            e.block = static_cast<int>(bi);
            e.marker = blk.marker;
            e.cond_entry_s = cond_at[i];
            ValuesAt(series, blk, rate, n, tl, &e.strength, &e.speed);
            evs.push_back(std::move(e));
        }

        if (opts.sensitivity > 0.0 && !evs.empty()) {
            double strongest = evs[0].strength;
            for (const Event& e : evs) strongest = std::max(strongest, e.strength);
            if (strongest > 0.0) {
                const double floor = opts.sensitivity * strongest;
                evs.erase(std::remove_if(evs.begin(), evs.end(),
                                         [&](const Event& e) { return e.strength < floor - 1e-12; }),
                          evs.end());
            }
        }
        for (const Event& e : evs) all.push_back(e);
        bt.ran = true;
        bt.rearm.resize(nc);
        for (size_t c = 0; c < nc; ++c) {
            const Condition& cd = blk.conditions[c];
            const double     m = std::max(0.0, cd.margin);
            bt.rearm[c] = cd.dir == Direction::Below ? cd.threshold + m : cd.threshold - m;
        }
        bt.curves = std::move(vals);
        bt.holds = std::move(holds);
        bt.active = std::move(active);
        bt.events = std::move(evs);
    }
    std::stable_sort(all.begin(), all.end(), [](const Event& a, const Event& b) { return a.time_s < b.time_s; });
    return trace;
}

std::vector<Event> Detect(const std::vector<Block>& blocks, const std::vector<BoneTrack>& tracks,
                          const DetectOptions& opts)
{
    return DetectTrace(blocks, tracks, opts).events;
}

std::vector<Block> Analyse(const std::vector<Block>& blocks, const std::vector<BoneTrack>& tracks,
                           const AnalyseOptions& opts)
{
    std::vector<Block> out = blocks;
    if (!TracksFit(tracks)) return out;
    for (Block& blk : out) {
        for (Condition& cd : blk.conditions) {
            if (!cd.auto_threshold) continue;
            EstimateFloor(cd.signal, tracks, opts);
            const std::vector<double> v = EvaluateSignal(cd.signal, tracks, opts.smooth_ms);
            if (v.empty()) continue;
            // Position, stillness and relative drop are levels (spike 10-7a: the new
            // measures take the position rule); speed and acceleration take percentiles.
            bool level = true;
            switch (cd.signal.measure) {
            case Measure::Position:
            case Measure::Stillness:
            case Measure::RelativeDrop: level = true; break;
            case Measure::Speed:
            case Measure::Acceleration: level = false; break;
            }
            if (level) {
                const double low = Percentile(v, opts.floor_percentile);
                const double split = OtsuSplit(v);
                std::vector<double> upper;
                for (double x : v)
                    if (x >= split) upper.push_back(x);
                const double high = upper.empty() ? low : Percentile(std::move(upper), 50.0);
                const double gap = std::max(0.0, high - low);
                cd.threshold = (cd.dir == Direction::Below) ? low + opts.position_fraction * gap
                                                            : high - opts.position_fraction * gap;
                cd.margin = opts.margin_ratio * opts.position_fraction * gap;
            } else if (cd.signal.keep_sign) {
                std::vector<double> mag(v.size());
                for (size_t i = 0; i < v.size(); ++i) mag[i] = std::fabs(v[i]);
                const double scale = opts.onset_fraction * Percentile(std::move(mag), 95.0);
                cd.threshold = (cd.dir == Direction::Below) ? -scale : scale;
                cd.margin = opts.margin_ratio * scale;
            } else {
                std::vector<double> mag(v.size());
                for (size_t i = 0; i < v.size(); ++i) mag[i] = std::fabs(v[i]);
                // A held pose (keyed holds) is exactly still: its percentile can be 0, and
                // "below 0" would never hold. The threshold keeps a floor of 1 % of the
                // 95th percentile (the signal's own scale).
                const double scale_floor = 0.01 * Percentile(mag, 95.0);
                cd.threshold = Percentile(std::move(mag), cd.dir == Direction::Below ? opts.speed_percentile
                                                                                      : 100.0 - opts.speed_percentile);
                if (cd.dir == Direction::Below) cd.threshold = std::max(cd.threshold, scale_floor);
                cd.margin = opts.margin_ratio * cd.threshold;
            }
        }
        if (!blk.strength_signal.bones.empty()) EstimateFloor(blk.strength_signal, tracks, opts);
    }
    return out;
}

MatchResult MatchEvents(const std::vector<double>& ref, const std::vector<double>& det, double window_s,
                        double lo_s, double hi_s)
{
    MatchResult r;
    std::vector<double> rf, dt;
    for (double t : ref)
        if (t >= lo_s && t <= hi_s) rf.push_back(t);
    for (double t : det)
        if (t >= lo_s && t <= hi_s) dt.push_back(t);
    std::sort(rf.begin(), rf.end());
    std::sort(dt.begin(), dt.end());
    r.n_ref = static_cast<int>(rf.size());
    r.n_det = static_cast<int>(dt.size());

    struct Pair {
        double d;
        size_t a, b;
    };
    std::vector<Pair> pairs;
    for (size_t a = 0; a < rf.size(); ++a)
        for (size_t b = 0; b < dt.size(); ++b) {
            const double d = std::fabs(dt[b] - rf[a]);
            if (d <= window_s + 1e-12) pairs.push_back({d, a, b});
        }
    std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.d < y.d; });
    std::vector<char> ra(rf.size(), 0), db(dt.size(), 0);
    std::vector<size_t> det_of(rf.size(), 0);  // a matched reference's detection
    double sum_abs = 0.0, sum_signed = 0.0;
    for (const Pair& p : pairs) {
        if (ra[p.a] || db[p.b]) continue;
        ra[p.a] = db[p.b] = 1;
        det_of[p.a] = p.b;
        ++r.n_match;
        sum_abs += p.d;
        sum_signed += dt[p.b] - rf[p.a];
        r.max_abs_err_s = std::max(r.max_abs_err_s, p.d);
    }
    for (size_t a = 0; a < rf.size(); ++a) {
        if (ra[a]) r.match_err_s.push_back(dt[det_of[a]] - rf[a]);
        else r.unmatched_ref.push_back(rf[a]);
    }
    for (size_t b = 0; b < dt.size(); ++b)
        if (!db[b]) r.unmatched_det.push_back(dt[b]);
    if (r.n_match > 0) {
        r.mean_abs_err_s = sum_abs / r.n_match;
        r.mean_signed_err_s = sum_signed / r.n_match;
    }
    r.recall = r.n_ref ? static_cast<double>(r.n_match) / r.n_ref : 1.0;
    r.precision = r.n_det ? static_cast<double>(r.n_match) / r.n_det : 1.0;
    return r;
}

MatchResult SumMatches(const std::vector<MatchResult>& items)
{
    MatchResult t;
    double sum_abs = 0.0, sum_signed = 0.0;
    for (const MatchResult& m : items) {
        t.n_ref += m.n_ref;
        t.n_det += m.n_det;
        t.n_match += m.n_match;
        sum_abs += m.mean_abs_err_s * m.n_match;
        sum_signed += m.mean_signed_err_s * m.n_match;
        t.max_abs_err_s = std::max(t.max_abs_err_s, m.max_abs_err_s);
        t.match_err_s.insert(t.match_err_s.end(), m.match_err_s.begin(), m.match_err_s.end());
    }
    if (t.n_match > 0) {
        t.mean_abs_err_s = sum_abs / t.n_match;
        t.mean_signed_err_s = sum_signed / t.n_match;
    }
    t.recall = t.n_ref ? static_cast<double>(t.n_match) / t.n_ref : 1.0;
    t.precision = t.n_det ? static_cast<double>(t.n_match) / t.n_det : 1.0;
    return t;
}

bool PassesAccuracyGate(const MatchResult& m)
{
    return m.n_ref > 0 && m.recall >= kGateRecall - 1e-12 && m.precision >= kGatePrecision - 1e-12 &&
           m.max_abs_err_s <= kGateMaxErrS + 1e-9;
}

}  // namespace rav
