// SPDX-License-Identifier: MIT
//
// Story 10-8n: the BIAS block's pieces for detection_eval --physics, kept apart so a host test
// pins them (tests/event_bias_test.cpp): each event REF kind's detector, PhysicsKind and match
// rule, the scuff (slide, pivot) span match, and the row text.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "bone_events.h"
#include "motion_physics.h"
#include "ref_labels.h"

namespace rav {

constexpr int kRefEventKinds = 7;
static_assert(static_cast<int>(RefEventKind::Land) + 1 == kRefEventKinds, "kRefEventKinds = RefEventKind's count");

inline const char* RefEventName(RefEventKind k)
{
    switch (k) {
    case RefEventKind::Lift: return "lift";
    case RefEventKind::Slide: return "slide";
    case RefEventKind::Pivot: return "pivot";
    case RefEventKind::Grab: return "grab";
    case RefEventKind::Release: return "release";
    case RefEventKind::Jump: return "jump";
    case RefEventKind::Land: return "land";
    }
    return "?";
}

// The detected kind a REF kind matches.
inline PhysicsKind RefEventPhysicsKind(RefEventKind k)
{
    switch (k) {
    case RefEventKind::Lift: return PhysicsKind::LiftOff;
    case RefEventKind::Slide: return PhysicsKind::Slide;
    case RefEventKind::Pivot: return PhysicsKind::Pivot;
    case RefEventKind::Grab: return PhysicsKind::Grab;
    case RefEventKind::Release: return PhysicsKind::Release;
    case RefEventKind::Jump: return PhysicsKind::Jump;
    case RefEventKind::Land: return PhysicsKind::Land;
    }
    return PhysicsKind::Step;
}

// The event function that detects a REF kind: FootEvents, HandEvents or (story 10-9a) JumpEvents.
enum class RefEventSource { Foot, Hand, Jump };
inline RefEventSource RefEventSourceOf(RefEventKind k)
{
    switch (k) {
    case RefEventKind::Lift:
    case RefEventKind::Slide:
    case RefEventKind::Pivot: return RefEventSource::Foot;
    case RefEventKind::Grab:
    case RefEventKind::Release: return RefEventSource::Hand;
    case RefEventKind::Jump:
    case RefEventKind::Land: return RefEventSource::Jump;
    }
    return RefEventSource::Foot;
}

// True: FootEvents detects it.
inline bool RefEventIsFoot(RefEventKind k)
{
    return RefEventSourceOf(k) == RefEventSource::Foot;
}

// True: a sideless REF kind (story 10-9a: jump, land), its REFs of side 'B'; story 10-9a-3: its
// events carry their foot (L / R) and a REF matches either.
inline bool RefEventIsSideless(RefEventKind k)
{
    return RefEventSourceOf(k) == RefEventSource::Jump;
}

// True: a scuff, matched on its span (MatchSpans); false: on its time (MatchEvents).
inline bool RefEventIsSpan(RefEventKind k)
{
    return k == RefEventKind::Slide || k == RefEventKind::Pivot;
}

// One event type's BIAS totals.
struct Bias {
    int                 ref = 0, matched = 0, missed = 0, extra = 0;
    std::vector<double> err;  // det - ref of each match (s)
    void                Add(const MatchResult& m)
    {
        ref += m.n_ref;
        matched += m.n_match;
        missed += static_cast<int>(m.unmatched_ref.size());
        extra += static_cast<int>(m.unmatched_det.size());
        err.insert(err.end(), m.match_err_s.begin(), m.match_err_s.end());
    }
};

// "  heel                 REF 40  matched 38  missed 2  extra 1  precision 97%  recall 95%  signed
// mean +4.1 ms  SD 9.3 ms  median |err| 6.0 ms\n" (sample SD; '-' for an undefined value).
inline std::string BiasRow(const std::string& name, const Bias& b)
{
    const auto pct = [](int a, int n) {
        char t[16] = "-";
        if (n > 0) std::snprintf(t, sizeof t, "%.0f%%", 100.0 * a / n);
        return std::string(t);
    };
    const size_t n = b.err.size();
    double       mean = 0.0, ss = 0.0;
    for (double e : b.err) mean += e;
    if (n) mean /= static_cast<double>(n);
    for (double e : b.err) ss += (e - mean) * (e - mean);
    char mean_t[32] = "-", sd_t[32] = "-", med_t[32] = "-";
    if (n) {
        std::vector<double> a;
        for (double e : b.err) a.push_back(std::fabs(e) * 1000.0);
        std::sort(a.begin(), a.end());
        const double med = (n % 2) ? a[n / 2] : 0.5 * (a[n / 2 - 1] + a[n / 2]);
        std::snprintf(mean_t, sizeof mean_t, "%+.1f ms", mean * 1000.0);
        std::snprintf(med_t, sizeof med_t, "%.1f ms", med);
    }
    if (n >= 2) std::snprintf(sd_t, sizeof sd_t, "%.1f ms", std::sqrt(ss / static_cast<double>(n - 1)) * 1000.0);
    char buf[320];
    std::snprintf(buf, sizeof buf,
                  "  %-20s REF %d  matched %d  missed %d  extra %d  precision %s  recall %s  signed mean %s  SD %s  "
                  "median |err| %s\n",
                  name.c_str(), b.ref, b.matched, b.missed, b.extra, pct(b.matched, b.matched + b.extra).c_str(),
                  pct(b.matched, b.ref).c_str(), mean_t, sd_t, med_t);
    return buf;
}

// A scuff (slide, pivot) REF matches a detected span of its side when it lies in [start - window,
// end]; err = start - REF. One-to-one, smallest |err| first. Only REFs and detections (by time_s)
// inside [lo, hi] count.
inline MatchResult MatchSpans(const std::vector<double>& ref, const std::vector<PhysicsEvent>& det, double window_s,
                              double lo, double hi)
{
    MatchResult               r;
    std::vector<double>       rf;
    std::vector<PhysicsEvent> dt;
    for (double t : ref)
        if (t >= lo && t <= hi) rf.push_back(t);
    for (const PhysicsEvent& e : det)
        if (e.time_s >= lo && e.time_s <= hi) dt.push_back(e);
    std::sort(rf.begin(), rf.end());
    r.n_ref = static_cast<int>(rf.size());
    r.n_det = static_cast<int>(dt.size());
    struct Pair {
        double d;
        size_t a, b;
    };
    std::vector<Pair> pairs;
    for (size_t a = 0; a < rf.size(); ++a)
        for (size_t b = 0; b < dt.size(); ++b)
            if (rf[a] >= dt[b].start_s - window_s - 1e-12 && rf[a] <= dt[b].end_s + 1e-12)
                pairs.push_back({std::fabs(dt[b].start_s - rf[a]), a, b});
    std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.d < y.d; });
    std::vector<char>   ra(rf.size(), 0), db(dt.size(), 0);
    std::vector<size_t> det_of(rf.size(), 0);
    for (const Pair& p : pairs) {
        if (ra[p.a] || db[p.b]) continue;
        ra[p.a] = db[p.b] = 1;
        det_of[p.a] = p.b;
        ++r.n_match;
    }
    for (size_t a = 0; a < rf.size(); ++a) {
        if (ra[a]) r.match_err_s.push_back(dt[det_of[a]].start_s - rf[a]);
        else r.unmatched_ref.push_back(rf[a]);
    }
    for (size_t b = 0; b < dt.size(); ++b)
        if (!db[b]) r.unmatched_det.push_back(dt[b].time_s);
    return r;
}

}  // namespace rav
