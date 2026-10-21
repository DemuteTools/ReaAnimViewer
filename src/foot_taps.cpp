// SPDX-License-Identifier: MIT
//
// See foot_taps.h. Story 10-9e: the Tap and Drag detectors, and the functions their rows of
// auto_types.cpp read (event sources, matches, lanes).

#include "foot_taps.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <tuple>

#include "auto_types.h"

namespace rav {
namespace {

constexpr int kHeelPart = static_cast<int>(FootPart::Heel);

bool PartOn(const PartTrack& pt, size_t j)
{
    return pt.present && j < pt.contact.size() && pt.contact[j];
}

bool FootOn(const FootTrack& f, size_t j)
{
    for (const PartTrack& pt : f.part)
        if (PartOn(pt, j)) return true;
    return false;
}

bool HasForefoot(const FootTrack& f)
{
    return f.part[static_cast<int>(FootPart::Ball)].present || f.part[static_cast<int>(FootPart::Tip)].present;
}

// An analysed time as a clip time (motion_physics.cpp's rule): looping, the middle cycle mapped
// to [0, T); false outside it.
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

void PushClip(const PhysicsAnalysis& a, PhysicsEvent e, std::vector<PhysicsEvent>& out)
{
    double t;
    if (!ToClip(a, e.time_s, &t)) return;
    const double shift = e.time_s - t;
    e.time_s = t;
    e.start_s -= shift;
    e.end_s -= shift;
    out.push_back(e);
}

void SortEvents(std::vector<PhysicsEvent>& ev)
{
    std::stable_sort(ev.begin(), ev.end(), [](const PhysicsEvent& x, const PhysicsEvent& y) {
        return std::make_tuple(std::llround(x.time_s * 1e6), x.side, x.part) <
               std::make_tuple(std::llround(y.time_s * 1e6), y.side, y.part);
    });
}

// Per sample, one forefoot part's drag condition (the heel test apart).
std::vector<char> PartDragMask(const PhysicsAnalysis& a, const PhysicsParams& p, const PartTrack& pt)
{
    const size_t      n = a.samples;
    std::vector<char> m(n, 0);
    if (!pt.present || pt.contacts.empty() || a.leg_length <= 0.0) return m;
    size_t ci = 0;  // the latest contact started at or before j (the first one before any)
    for (size_t j = 0; j < n && j < pt.y.size() && j < pt.hspeed.size(); ++j) {
        while (ci + 1 < pt.contacts.size() && static_cast<size_t>(pt.contacts[ci + 1].start) <= j) ++ci;
        if (PartOn(pt, j)) continue;
        const double h = (pt.y[j] - pt.contacts[ci].support_y) / a.leg_length;
        const double v = pt.hspeed[j];
        m[j] = std::fabs(h) <= p.drag_height && v >= p.drag_speed && v <= p.drag_max_speed ? 1 : 0;
    }
    return m;
}

}  // namespace

std::vector<PhysicsEvent> FootTapEvents(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    std::vector<PhysicsEvent> ev;
    if (!a.ok) return ev;
    const size_t n = a.samples;
    const double rate = a.rate_hz;
    for (int s = 0; s < 2; ++s) {
        const FootTrack& f = a.foot[s];
        const FootTrack& o = a.foot[1 - s];
        const PartTrack& heel = f.part[kHeelPart];
        if (!heel.present || !HasForefoot(f) || a.leg_length <= 0.0) continue;
        // The foot's flat pose, per forefoot part: the heel-over-part height while both are in
        // contact, its 10th percentile (the flattest it stands: a foot held still in the air,
        // heel up, reads as a contact too). None: that part gives no tap.
        double flat[kFootPartCount] = {};
        bool   has_flat[kFootPartCount] = {};
        for (int q = 1; q < kFootPartCount; ++q) {
            const PartTrack& pt = f.part[q];
            if (!pt.present) continue;
            std::vector<double> d;
            for (size_t j = 0; j < n && j < pt.y.size() && j < heel.y.size(); ++j)
                if (PartOn(heel, j) && PartOn(pt, j)) d.push_back(heel.y[j] - pt.y[j]);
            if (d.empty()) continue;
            std::nth_element(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(d.size() / 10), d.end());
            flat[q] = d[d.size() / 10];
            has_flat[q] = true;
        }
        auto toe_on = [&](size_t j) { return PartOn(f.part[1], j) || PartOn(f.part[2], j); };
        for (size_t i = 0; i < n;) {
            if (!toe_on(i)) {
                ++i;
                continue;
            }
            const size_t us = i;
            while (i < n && toe_on(i)) ++i;
            const size_t ue = i;
            if (us == 0 || ue >= n) continue;  // cut by the clip start or end
            if (static_cast<double>(ue - us) / rate > p.tap_max_s + 1e-9) continue;
            // Named and timed as a step: the earliest timed forefoot part touching in the run.
            PhysicsEvent       best;
            const PartContact* bc = nullptr;
            for (int q = 1; q < kFootPartCount; ++q) {
                const PartTrack& pt = f.part[q];
                if (!pt.present || !has_flat[q]) continue;
                const int k = std::clamp(static_cast<int>(p.step_timing[q]), 0, kStepTimingCount - 1);
                for (const PartContact& c : pt.contacts) {
                    if (c.start < static_cast<int>(us) || c.start >= static_cast<int>(ue)) continue;
                    const double t = c.step_s[k] + p.step_offset_s[q];
                    if (bc && t >= best.time_s) continue;
                    best.kind = PhysicsKind::Tap;
                    best.side = f.side;
                    best.part = q;
                    best.time_s = best.start_s = best.end_s = t;
                    best.strength = c.approach;
                    bc = &c;
                }
            }
            if (!bc) continue;
            const int        q = best.part;
            const PartTrack& pt = f.part[q];
            // On the floor the body stands on: the other foot's same part down at the touch, at
            // the same support height.
            const PartTrack& opt = o.part[q];
            bool             floor = false;
            for (const PartContact& c : opt.contacts)
                if (opt.present && c.start <= bc->start && bc->start < c.end)
                    floor = std::fabs(c.support_y - bc->support_y) / a.leg_length <= p.tap_floor_height;
            if (!floor) continue;
            // Light: the heel stays up (higher above the toe than flat) and the other foot down.
            bool light = true;
            for (size_t j = us; j < ue && light; ++j)
                light = FootOn(o, j) && (heel.y[j] - pt.y[j] - flat[q]) / a.leg_length >= p.tap_heel_rise;
            if (light) PushClip(a, best, ev);
        }
    }
    SortEvents(ev);
    return ev;
}

std::vector<char> FootDragMask(const PhysicsAnalysis& a, const PhysicsParams& p, char side)
{
    if (!a.ok) return {};
    const FootTrack& f = a.foot[side == 'R' ? 1 : 0];
    const PartTrack& heel = f.part[kHeelPart];
    if (!heel.present || !HasForefoot(f)) return {};
    const std::vector<char> ball = PartDragMask(a, p, f.part[static_cast<int>(FootPart::Ball)]);
    const std::vector<char> tip = PartDragMask(a, p, f.part[static_cast<int>(FootPart::Tip)]);
    std::vector<char>       m(a.samples, 0);
    for (size_t j = 0; j < a.samples; ++j) m[j] = !PartOn(heel, j) && (ball[j] || tip[j]) ? 1 : 0;
    return m;
}

std::vector<PhysicsEvent> FootDragEvents(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    std::vector<PhysicsEvent> ev;
    if (!a.ok) return ev;
    const size_t n = a.samples;
    const double rate = a.rate_hz;
    const size_t gap = static_cast<size_t>(std::floor(p.drag_gap_s * rate + 1e-9));
    for (int s = 0; s < 2; ++s) {
        const FootTrack&        f = a.foot[s];
        const std::vector<char> m = FootDragMask(a, p, f.side);
        if (m.empty()) continue;
        const PartTrack&        heel = f.part[kHeelPart];
        const PartTrack&        ball = f.part[static_cast<int>(FootPart::Ball)];
        const PartTrack&        tip = f.part[static_cast<int>(FootPart::Tip)];
        const std::vector<char> mb = PartDragMask(a, p, ball);
        const std::vector<char> mt = PartDragMask(a, p, tip);
        for (size_t i = 0; i < n;) {
            if (!m[i]) {
                ++i;
                continue;
            }
            const size_t rs = i;
            size_t       re = i;  // one past the run's last dragging sample
            while (i < n) {
                if (m[i]) {
                    re = ++i;
                    continue;
                }
                // A short gap with the heel still off joins the next run.
                size_t j = i;
                while (j < n && !m[j] && j - re < gap && !PartOn(heel, j)) ++j;
                if (j < n && m[j] && j - re <= gap) {
                    i = j;
                    continue;
                }
                break;
            }
            i = re;
            if (rs == 0 && !a.looping) continue;  // under way at the clip start
            if (static_cast<double>(re - rs) / rate < p.drag_min_s - 1e-9) continue;
            double dist = 0.0, peak = 0.0;
            int    nb = 0, nt = 0;
            for (size_t j = rs; j < re; ++j) {
                if (!m[j]) continue;
                const bool   b = mb[j] != 0, t = mt[j] != 0;
                const double v = std::max(b ? ball.hspeed[j] : 0.0, t ? tip.hspeed[j] : 0.0);
                dist += v / rate;
                peak = std::max(peak, v);
                nb += b;
                nt += t;
            }
            if (dist < p.drag_min_dist - 1e-9) continue;
            PhysicsEvent e;
            e.kind = PhysicsKind::Drag;
            e.side = f.side;
            e.part = nb >= nt ? static_cast<int>(FootPart::Ball) : static_cast<int>(FootPart::Tip);
            e.time_s = e.start_s = static_cast<double>(rs) / rate;
            e.end_s = static_cast<double>(re) / rate;
            e.strength = peak;
            PushClip(a, e, ev);
        }
    }
    SortEvents(ev);
    return ev;
}

// ---- The rows' functions (auto_types.cpp) ------------------------------------------------------

std::vector<PhysicsEvent> AutoSourceTap(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return FootTapEvents(a, p);
}

std::vector<PhysicsEvent> AutoSourceDrag(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return FootDragEvents(a, p);
}

bool AutoMatchTap(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Tap;
}

bool AutoMatchDrag(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Drag;
}

// Drag: the ball's (else the tip's) horizontal speed against drag_speed, shaded where the foot
// drags (FootDragMask).
void AutoLanesDrag(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const FootTrack& f = args.Foot();
    const PartTrack& ball = f.part[static_cast<int>(FootPart::Ball)];
    const PartTrack& tip = f.part[static_cast<int>(FootPart::Tip)];
    if (!f.part[kHeelPart].present || (!ball.present && !tip.present)) {
        reason = "drag needs the heel and the toe";
        return;
    }
    const PartTrack&        pt = ball.present ? ball : tip;
    const std::vector<char> m = FootDragMask(*args.a, *args.p, args.side);
    AutoLane                l;
    l.label = std::string(ball.present ? "Ball " : "Tip ") + std::string(1, args.side) + "  hspeed";
    l.unit = "leg/s";
    l.curve = AutoLaneWindow(pt.hspeed, args.first, args.n, false);
    l.contact.assign(args.n, 0);
    for (size_t i = 0; i < args.n && args.first + i < m.size(); ++i) l.contact[i] = m[args.first + i];
    l.threshold = args.p->drag_speed;
    l.above = true;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "Shaded: the toe slides on the ground with the heel up. A drag is a shaded run\n"
                  "of %.0f ms at least, under %.1f leg/s (faster is a swing).",
                  args.p->drag_min_s * 1000.0, args.p->drag_max_speed);
    l.shade_note = buf;
    out.push_back(std::move(l));
}

}  // namespace rav
