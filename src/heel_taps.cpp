// SPDX-License-Identifier: MIT
//
// See heel_taps.h. Story 10-9i: the Heel Tap detector, and the functions its row of
// auto_types.cpp reads (event source, match, lanes).

#include "heel_taps.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <tuple>

#include "auto_types.h"

namespace rav {
namespace {

constexpr int kHeel = static_cast<int>(FootPart::Heel);

// The forefoot part a heel tap reads: the ball, else the tip; -1 = none.
int ForefootOf(const FootTrack& f)
{
    if (f.part[static_cast<int>(FootPart::Ball)].present) return static_cast<int>(FootPart::Ball);
    if (f.part[static_cast<int>(FootPart::Tip)].present) return static_cast<int>(FootPart::Tip);
    return -1;
}

// Per sample, the part's floor (its contact's support, m), NaN where it is not in contact.
std::vector<double> FloorOf(const PartTrack& pt, size_t n)
{
    std::vector<double> fl(n, std::nan(""));
    for (const PartContact& c : pt.contacts)
        for (int j = std::max(0, c.start); j < c.end && static_cast<size_t>(j) < n; ++j)
            if (static_cast<size_t>(j) < pt.contact.size() && pt.contact[static_cast<size_t>(j)])
                fl[static_cast<size_t>(j)] = c.support_y;
    return fl;
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

// Per sample of foot f: the heel's and the forefoot's heights over their floors (leg), NaN where
// that part is not in contact.
struct FootHeights {
    std::vector<double> heel, fore;
};

bool HeightsOf(const PhysicsAnalysis& a, const FootTrack& f, FootHeights* out)
{
    const PartTrack& heel = f.part[kHeel];
    const int        q = ForefootOf(f);
    if (!a.ok || !heel.present || q < 0 || a.leg_length <= 0.0) return false;
    const PartTrack&          fore = f.part[q];
    const size_t              n = a.samples;
    const std::vector<double> hf = FloorOf(heel, n), ff = FloorOf(fore, n);
    out->heel.assign(n, std::nan(""));
    out->fore.assign(n, std::nan(""));
    for (size_t j = 0; j < n; ++j) {
        if (j < heel.y.size() && !std::isnan(hf[j])) out->heel[j] = (heel.y[j] - hf[j]) / a.leg_length;
        if (j < fore.y.size() && !std::isnan(ff[j])) out->fore[j] = (fore.y[j] - ff[j]) / a.leg_length;
    }
    return true;
}

}  // namespace

std::vector<double> HeelTapRise(const PhysicsAnalysis& a, const PhysicsParams& p, char side, std::vector<char>* down)
{
    FootHeights h;
    if (!HeightsOf(a, a.foot[side == 'R' ? 1 : 0], &h)) {
        if (down) down->clear();
        return {};
    }
    const size_t        n = a.samples;
    std::vector<double> rise(n, 0.0);
    if (down) down->assign(n, 0);
    for (size_t j = 0; j < n; ++j) {
        if (std::isnan(h.heel[j])) continue;
        rise[j] = h.heel[j];
        if (down) (*down)[j] = !std::isnan(h.fore[j]) && h.fore[j] <= p.heel_tap_ball_height ? 1 : 0;
    }
    return rise;
}

std::vector<PhysicsEvent> HeelTapEvents(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    std::vector<PhysicsEvent> ev;
    if (!a.ok) return ev;
    const size_t n = a.samples;
    const double rate = a.rate_hz;
    for (int s = 0; s < 2; ++s) {
        const FootTrack& f = a.foot[s];
        FootHeights      h;
        if (!HeightsOf(a, f, &h)) continue;
        const PartTrack& heel = f.part[kHeel];
        // The heel is up where it is over its floor by more than heel_tap_land, or off the ground.
        auto up = [&](size_t j) { return std::isnan(h.heel[j]) || h.heel[j] > p.heel_tap_land; };
        for (size_t i = 0; i < n;) {
            if (!up(i)) {
                ++i;
                continue;
            }
            const size_t us = i;
            while (i < n && up(i)) ++i;
            const size_t ue = i;  // the first sample back down
            if (us == 0 || ue >= n) continue;  // cut by the clip start or end
            // On the floor the body stands on: the forefoot's support at the landing within
            // heel_tap_floor of the floor (a foot on a climbing hold, a crawl's toes on their
            // tips, are not on it).
            const PartTrack& fore = f.part[ForefootOf(f)];
            bool             on_floor = false;
            for (const PartContact& c : fore.contacts)
                if (c.start <= static_cast<int>(ue) && static_cast<int>(ue) < c.end)
                    on_floor = a.body.has_floor && std::fabs(c.support_y - a.body.floor_y) / a.leg_length <= p.heel_tap_floor;
            if (!on_floor) continue;
            // The heel stays on the ground (the segmentation's contact) and the ball down.
            bool   held = true;
            double tilt = 0.0;
            size_t peak = us;
            for (size_t j = us; j <= ue && held; ++j) {
                held = !std::isnan(h.heel[j]) && !std::isnan(h.fore[j]) && h.fore[j] <= p.heel_tap_ball_height;
                if (!held) break;
                const double t = h.heel[j] - h.fore[j];
                if (t > tilt) {
                    tilt = t;
                    peak = j;
                }
            }
            if (!held || tilt < p.heel_tap_min_rise - 1e-12) continue;
            // It comes down: the fastest descent after the tilt's peak.
            double fall = 0.0;
            for (size_t j = peak; j <= ue && j < heel.vy.size(); ++j) fall = std::max(fall, -heel.vy[j]);
            if (fall < p.heel_tap_min_speed - 1e-12) continue;
            // It rests there: within heel_tap_land of its floor (above or below) for
            // heel_tap_hold_s (a heel still sinking past its floor was not on it: the floor read
            // from a contact that started with the foot tilted).
            const size_t hold = static_cast<size_t>(std::ceil(p.heel_tap_hold_s * rate - 1e-9));
            bool         rests = ue + hold < n;
            for (size_t j = ue; j <= ue + hold && rests; ++j) rests = !std::isnan(h.heel[j]) && std::fabs(h.heel[j]) <= p.heel_tap_land;
            if (!rests) continue;
            const double a0 = h.heel[ue - 1], a1 = h.heel[ue];
            const double fr = a0 != a1 ? std::clamp((a0 - p.heel_tap_land) / (a0 - a1), 0.0, 1.0) : 1.0;
            PhysicsEvent e;
            e.kind = PhysicsKind::Tap;
            e.side = f.side;
            e.part = kHeel;
            e.time_s = e.start_s = e.end_s = (static_cast<double>(ue - 1) + fr) / rate;
            e.strength = fall;
            double t;
            if (!ToClip(a, e.time_s, &t)) continue;
            e.time_s = e.start_s = e.end_s = t;
            ev.push_back(e);
        }
    }
    std::stable_sort(ev.begin(), ev.end(), [](const PhysicsEvent& x, const PhysicsEvent& y) {
        return std::make_tuple(std::llround(x.time_s * 1e6), x.side) < std::make_tuple(std::llround(y.time_s * 1e6), y.side);
    });
    return ev;
}

// ---- The row's functions (auto_types.cpp) ------------------------------------------------------

std::vector<PhysicsEvent> AutoSourceHeelTap(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return HeelTapEvents(a, p);
}

bool AutoMatchHeelTap(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Tap && e.part == kHeel;
}

// Heel Tap: the heel's height over its floor against heel_tap_min_rise, shaded where the ball is
// down with the heel on the ground (HeelTapRise).
void AutoLanesHeelTap(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const FootTrack& f = args.Foot();
    if (!f.part[kHeel].present || ForefootOf(f) < 0) {
        reason = "heel tap needs the heel and the toe";
        return;
    }
    std::vector<char>         down;
    const std::vector<double> rise = HeelTapRise(*args.a, *args.p, args.side, &down);
    AutoLane                  l;
    l.label = std::string("Heel ") + std::string(1, args.side) + "  height";
    l.unit = "leg";
    l.curve = AutoLaneWindow(rise, args.first, args.n, false);
    l.contact.assign(args.n, 0);
    for (size_t i = 0; i < args.n && args.first + i < down.size(); ++i) l.contact[i] = down[args.first + i];
    l.threshold = args.p->heel_tap_min_rise;
    l.above = true;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "Shaded: the ball is down and the heel on the ground. A heel tap is the heel\n"
                  "rising %.3f leg or more over the ball (shaded throughout), then coming back down.",
                  args.p->heel_tap_min_rise);
    l.shade_note = buf;
    out.push_back(std::move(l));
}

}  // namespace rav
