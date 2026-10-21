// SPDX-License-Identifier: MIT
//
// Story 10-9b: body contacts (knee, elbow, sit, fall) from the analysis' BodyTrack (see
// BodyContactEvents in motion_physics.h for the rules, PhysicsParams for the constants). Pure
// C++17, no REAPER. Host-tested (tests/motion_physics_test.cpp).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

#include "motion_physics.h"

namespace rav {
namespace {

constexpr char kBodySides[2] = {'L', 'R'};

// The peak of `v` (its absolute value when `down`: the downward speed) over [i - w, i].
double PeakBefore(const std::vector<double>& v, int i, int w, bool down)
{
    double peak = 0.0;
    for (int j = std::max(0, i - w); j <= i && j < static_cast<int>(v.size()); ++j)
        peak = std::max(peak, down ? -v[static_cast<size_t>(j)] : v[static_cast<size_t>(j)]);
    return peak;
}

double Median(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    const size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(m), v.end());
    return v[m];
}

PhysicsEvent BodyEvent(PhysicsKind kind, char side, int sample, double rate, double strength)
{
    PhysicsEvent e;
    e.kind = kind;
    e.side = side;
    e.part = -1;
    e.time_s = e.start_s = e.end_s = sample / rate;
    e.strength = strength;
    return e;
}

// A contact counts: not at the clip start, long enough unless the clip end cuts it short.
bool Counted(const PartContact& c, int n, double rate, double min_s)
{
    return c.start > 0 && (c.end >= n || (c.end - c.start) / rate >= min_s - 1e-9);
}

bool InContact(const PartTrack& pt, int from, int to)
{
    for (int i = std::max(0, from); i < to && i < static_cast<int>(pt.contact.size()); ++i)
        if (pt.contact[static_cast<size_t>(i)]) return true;
    return false;
}

}  // namespace

std::vector<PhysicsEvent> BodyContactEvents(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    std::vector<PhysicsEvent> ev;
    const BodyTrack&          b = a.body;
    if (!a.ok || !b.has_floor || !(a.rate_hz > 0.0) || !(a.leg_length > 0.0)) return ev;
    const int    n = static_cast<int>(a.samples);
    const double rate = a.rate_hz, leg = a.leg_length;
    const int    w = static_cast<int>(std::lround(p.fall_window_s * rate));

    // Knees and elbows: each counted contact's start.
    for (int s = 0; s < 2; ++s)
        for (int part = 0; part < 2; ++part) {
            const PartTrack& pt = part ? b.elbow[s] : b.knee[s];
            if (!pt.present) continue;
            for (const PartContact& c : pt.contacts) {
                const double descent = PeakBefore(pt.vy, c.start, w, true);
                if (!Counted(c, n, rate, p.body_min_contact_s) || descent < p.body_min_descent) continue;
                // Timed where the descent has stopped: the first contact sample whose downward
                // speed is at most body_stop_frac of the descent's peak (else the contact start).
                int at = c.start;
                for (int j = c.start; j < std::min(c.end, n); ++j)
                    if (-pt.vy[static_cast<size_t>(j)] <= p.body_stop_frac * descent) {
                        at = j;
                        break;
                    }
                ev.push_back(BodyEvent(part ? PhysicsKind::Elbow : PhysicsKind::Knee, kBodySides[s], at, rate, descent));
            }
        }

    // Sit: a hips contact, upright, away from the feet, no knee down.
    const PartTrack& hips = b.hips;
    const PartTrack& chest = b.chest;
    if (hips.present && chest.present && b.hips_feet.size() == static_cast<size_t>(n))
        for (const PartContact& c : hips.contacts) {
            if (!Counted(c, n, rate, std::max(p.body_min_contact_s, p.sit_min_s))) continue;
            const int           end = std::min(c.end, n);
            std::vector<double> up, off;
            for (int i = c.start; i < end; ++i) {
                up.push_back((chest.y[static_cast<size_t>(i)] - hips.y[static_cast<size_t>(i)]) / leg);
                off.push_back(b.hips_feet[static_cast<size_t>(i)]);
            }
            if (Median(up) < p.sit_min_upright || Median(off) < p.sit_min_offset) continue;
            if (InContact(b.knee[0], c.start, end) || InContact(b.knee[1], c.start, end)) continue;
            ev.push_back(BodyEvent(PhysicsKind::Sit, 'B', c.start, rate, PeakBefore(hips.speed, c.start, w, false)));
        }

    // Fall: the trunk (the lower of hips and chest) lands near the floor after a fast descent.
    if (hips.present || chest.present) {
        auto low = [&](int i, double* vy) {
            double h = 1e9;
            for (const PartTrack* pt : {&hips, &chest})
                if (pt->present) {
                    const double y = (pt->y[static_cast<size_t>(i)] - b.floor_y) / leg;
                    if (y < h) {
                        h = y;
                        if (vy) *vy = pt->vy[static_cast<size_t>(i)];
                    }
                }
            return h;
        };
        const double fh = p.fall_height;
        bool         armed = low(0, nullptr) > fh;
        for (int i = 0; i < n; ++i) {
            const double h = low(i, nullptr);
            if (!armed) {
                armed = h > 1.5 * fh;
                continue;
            }
            if (h > fh) continue;
            armed = false;
            // The first sample, from here, where the descent has stopped (still low).
            for (int j = i; j < n; ++j) {
                double       vy = 0.0;
                const double hj = low(j, &vy);
                if (hj > 1.5 * fh) break;
                if (vy < -p.fall_stop_speed) continue;
                double peak = 0.0;
                for (int q = std::max(0, j - w); q <= j; ++q) {
                    double vq = 0.0;
                    low(q, &vq);
                    peak = std::max(peak, -vq);
                }
                if (peak >= p.fall_min_speed) ev.push_back(BodyEvent(PhysicsKind::Fall, 'B', j, rate, peak));
                i = j;
                break;
            }
        }
    }

    PhysicsEventsToClipTime(a, ev);
    std::stable_sort(ev.begin(), ev.end(), [](const PhysicsEvent& x, const PhysicsEvent& y) {
        return std::make_tuple(std::llround(x.time_s * 1e6), x.side, static_cast<int>(x.kind)) <
               std::make_tuple(std::llround(y.time_s * 1e6), y.side, static_cast<int>(y.kind));
    });
    return ev;
}

std::string BodyContactSummary(const PhysicsAnalysis& a, const std::vector<PhysicsEvent>& ev)
{
    if (!a.ok) return "body: skipped: " + a.error;
    int knees = 0, elbows = 0, sits = 0, falls = 0;
    for (const PhysicsEvent& e : ev) {
        if (e.kind == PhysicsKind::Knee) ++knees;
        if (e.kind == PhysicsKind::Elbow) ++elbows;
        if (e.kind == PhysicsKind::Sit) ++sits;
        if (e.kind == PhysicsKind::Fall) ++falls;
    }
    char buf[96];
    std::snprintf(buf, sizeof buf, "body: knees %d, elbows %d, sits %d, falls %d", knees, elbows, sits, falls);
    std::string              out = buf;
    std::vector<std::string> miss;
    const BodyTrack&         b = a.body;
    if (!b.has_floor) miss.push_back("floor: " + b.floor_missing);
    for (int s = 0; s < 2 && b.has_floor; ++s) {
        if (!b.knee[s].present) miss.push_back(std::string(1, kBodySides[s]) + " knee: " + b.knee[s].missing);
        if (!b.elbow[s].present) miss.push_back(std::string(1, kBodySides[s]) + " elbow: " + b.elbow[s].missing);
    }
    if (b.has_floor && !b.hips.present) miss.push_back("hips: " + b.hips.missing);
    if (b.has_floor && !b.chest.present) miss.push_back("chest: " + b.chest.missing);
    for (size_t i = 0; i < miss.size(); ++i) out += (i ? ", " : "; missing: ") + miss[i];
    return out;
}

}  // namespace rav
