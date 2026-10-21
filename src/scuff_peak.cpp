// SPDX-License-Identifier: MIT
//
// See scuff_peak.h. Story 10-9j: the Peak twins' timing, and the event sources their rows of
// auto_types.cpp read.

#include "scuff_peak.h"

#include <algorithm>
#include <cmath>
#include <tuple>

#include "auto_types.h"

namespace rav {
namespace {

// The part of the foot whose rub a slide / pivot event marks (scuff_peak.h).
const PartTrack* FootRubPart(const FootTrack& f, const PhysicsEvent& e, bool by_longest)
{
    int q = by_longest && e.longest >= 0 ? e.longest : e.part;
    if (q < 0 || q >= kFootPartCount || !f.part[q].present) {
        q = -1;
        for (FootPart fp : {FootPart::Ball, FootPart::Tip, FootPart::Heel})
            if (q < 0 && f.part[static_cast<int>(fp)].present) q = static_cast<int>(fp);
    }
    return q < 0 ? nullptr : &f.part[q];
}

}  // namespace

PhysicsEvent ScuffPeakEvent(const PhysicsAnalysis& a, const PhysicsEvent& e, bool by_longest)
{
    const bool foot = e.kind == PhysicsKind::Slide || e.kind == PhysicsKind::Pivot;
    if ((!foot && e.kind != PhysicsKind::HandPivot) || !a.ok || !(a.rate_hz > 0.0)) return e;
    const int        s = e.side == 'R' ? 1 : 0;
    const PartTrack* pt = foot ? FootRubPart(a.foot[s], e, by_longest) : &a.hand[s].part;
    if (!pt || !pt->present || pt->hspeed.empty()) return e;
    // The span in analysed samples (a loop's clip time is the analysed time less the window's
    // first sample: motion_physics.cpp ClipTime).
    const double shift = a.looping ? static_cast<double>(a.window_first) / a.rate_hz : 0.0;
    const long   n = static_cast<long>(pt->hspeed.size());
    const long   lo = std::clamp(std::lround((e.start_s + shift) * a.rate_hz), 0L, n - 1);
    const long   hi = std::clamp(std::lround((e.end_s + shift) * a.rate_hz), lo + 1, n);
    long         best = -1;
    for (int pass = 0; pass < 2 && best < 0; ++pass)
        for (long j = lo; j < hi; ++j) {
            const bool down = static_cast<size_t>(j) < pt->contact.size() && pt->contact[static_cast<size_t>(j)];
            if (pass == 0 && !down) continue;
            if (best < 0 || pt->hspeed[static_cast<size_t>(j)] > pt->hspeed[static_cast<size_t>(best)]) best = j;
        }
    PhysicsEvent out = e;
    out.time_s = e.start_s + static_cast<double>(best - lo) / a.rate_hz;
    return out;
}

std::vector<PhysicsEvent> ScuffPeakEvents(const PhysicsAnalysis& a, std::vector<PhysicsEvent> ev, bool by_longest)
{
    for (PhysicsEvent& e : ev) e = ScuffPeakEvent(a, e, by_longest);
    std::stable_sort(ev.begin(), ev.end(), [](const PhysicsEvent& x, const PhysicsEvent& y) {
        return std::make_tuple(std::llround(x.time_s * 1e6), x.side, static_cast<int>(x.kind), x.part) <
               std::make_tuple(std::llround(y.time_s * 1e6), y.side, static_cast<int>(y.kind), y.part);
    });
    return ev;
}

// ---- The rows' functions (auto_types.cpp) ------------------------------------------------------

// The Peak twins' sources: the same events as their Start kinds' (FootEvents, FootPartScuffEvents,
// HandEvents), their scuffs timed at the peak.
std::vector<PhysicsEvent> AutoSourceFootPeak(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return ScuffPeakEvents(a, FootEvents(a, p));
}

std::vector<PhysicsEvent> AutoSourceFootLongestPeak(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return ScuffPeakEvents(a, FootEvents(a, p), true);
}

std::vector<PhysicsEvent> AutoSourceFootPartsPeak(const PhysicsAnalysis& a, const PhysicsParams&)
{
    return ScuffPeakEvents(a, FootPartScuffEvents(a));
}

std::vector<PhysicsEvent> AutoSourceHandPeak(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return ScuffPeakEvents(a, HandEvents(a, p));
}

std::vector<PhysicsEvent> AutoSourceHandAnyPeak(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    PhysicsParams any = p;
    any.hand_pivot_after_grab = false;
    return ScuffPeakEvents(a, HandEvents(a, any));
}

}  // namespace rav
