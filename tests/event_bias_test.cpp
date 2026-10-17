// SPDX-License-Identifier: MIT
//
// Host test of detection_eval's BIAS pieces (tests/event_bias.h, story 10-8n): each event REF
// kind's detector, kind and match rule, the scuff span match and the row text. No REAPER.

#include "event_bias.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace rav;

namespace {

int g_fails = 0;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);        \
            ++g_fails;                                              \
        }                                                           \
    } while (0)

bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }

PhysicsEvent Span(double start, double end)
{
    PhysicsEvent e;
    e.kind = PhysicsKind::Slide;
    e.time_s = e.start_s = start;
    e.end_s = end;
    return e;
}

}  // namespace

int main()
{
    // Routing: foot kinds from FootEvents, hand kinds from HandEvents; scuffs on their span.
    CHECK(RefEventPhysicsKind(RefEventKind::Lift) == PhysicsKind::LiftOff);
    CHECK(RefEventPhysicsKind(RefEventKind::Slide) == PhysicsKind::Slide);
    CHECK(RefEventPhysicsKind(RefEventKind::Pivot) == PhysicsKind::Pivot);
    CHECK(RefEventPhysicsKind(RefEventKind::Grab) == PhysicsKind::Grab);
    CHECK(RefEventPhysicsKind(RefEventKind::Release) == PhysicsKind::Release);
    CHECK(RefEventIsFoot(RefEventKind::Lift) && RefEventIsFoot(RefEventKind::Slide) && RefEventIsFoot(RefEventKind::Pivot));
    CHECK(!RefEventIsFoot(RefEventKind::Grab) && !RefEventIsFoot(RefEventKind::Release));
    CHECK(RefEventIsSpan(RefEventKind::Slide) && RefEventIsSpan(RefEventKind::Pivot));
    CHECK(!RefEventIsSpan(RefEventKind::Lift) && !RefEventIsSpan(RefEventKind::Grab) &&
          !RefEventIsSpan(RefEventKind::Release));
    CHECK(std::string(RefEventName(RefEventKind::Lift)) == "lift");
    // Story 10-9a: jump and land, from JumpEvents, sideless, matched on their time.
    CHECK(RefEventPhysicsKind(RefEventKind::Jump) == PhysicsKind::Jump);
    CHECK(RefEventPhysicsKind(RefEventKind::Land) == PhysicsKind::Land);
    CHECK(RefEventSourceOf(RefEventKind::Jump) == RefEventSource::Jump &&
          RefEventSourceOf(RefEventKind::Land) == RefEventSource::Jump);
    CHECK(RefEventSourceOf(RefEventKind::Grab) == RefEventSource::Hand &&
          RefEventSourceOf(RefEventKind::Lift) == RefEventSource::Foot);
    CHECK(RefEventIsSideless(RefEventKind::Jump) && RefEventIsSideless(RefEventKind::Land));
    CHECK(!RefEventIsSideless(RefEventKind::Lift) && !RefEventIsSideless(RefEventKind::Release));
    CHECK(!RefEventIsFoot(RefEventKind::Jump) && !RefEventIsSpan(RefEventKind::Jump) && !RefEventIsSpan(RefEventKind::Land));
    CHECK(std::string(RefEventName(RefEventKind::Jump)) == "jump" && std::string(RefEventName(RefEventKind::Land)) == "land");
    for (int k = 0; k < kRefEventKinds; ++k) CHECK(std::string(RefEventName(static_cast<RefEventKind>(k))) != "?");

    // A REF inside the span: matched, err = start - REF (the spec's scuff row: -70 ms).
    {
        const MatchResult m = MatchSpans({1.50}, {Span(1.43, 1.68)}, 0.150, 0.0, 10.0);
        CHECK(m.n_ref == 1 && m.n_match == 1 && m.unmatched_ref.empty() && m.unmatched_det.empty());
        CHECK(m.match_err_s.size() == 1 && Near(m.match_err_s[0], -0.07));
    }
    // Before the start, within the window: matched, err > 0. Too early, or after the end: not.
    {
        const MatchResult m = MatchSpans({1.33}, {Span(1.43, 1.68)}, 0.150, 0.0, 10.0);
        CHECK(m.n_match == 1 && Near(m.match_err_s[0], 0.10));
        const MatchResult early = MatchSpans({1.25}, {Span(1.43, 1.68)}, 0.150, 0.0, 10.0);
        CHECK(early.n_match == 0 && early.unmatched_ref.size() == 1 && early.unmatched_det.size() == 1);
        const MatchResult late = MatchSpans({1.70}, {Span(1.43, 1.68)}, 0.150, 0.0, 10.0);
        CHECK(late.n_match == 0 && late.unmatched_ref.size() == 1 && late.unmatched_det.size() == 1);
    }
    // One-to-one: two REFs in one span, the nearest to its start wins; the other is missed.
    {
        const MatchResult m = MatchSpans({1.60, 1.45}, {Span(1.43, 1.68)}, 0.150, 0.0, 10.0);
        CHECK(m.n_match == 1 && m.unmatched_ref.size() == 1 && Near(m.unmatched_ref[0], 1.60));
        CHECK(Near(m.match_err_s[0], -0.02));
    }
    // Outside [lo, hi] (by time_s): neither counts.
    {
        const MatchResult m = MatchSpans({0.5, 1.5}, {Span(0.45, 0.6), Span(1.45, 1.6)}, 0.150, 1.0, 2.0);
        CHECK(m.n_ref == 1 && m.n_det == 1 && m.n_match == 1);
    }

    // Rows: '-' when undefined; sample SD.
    {
        Bias none;
        none.ref = 2;
        none.missed = 2;
        const std::string r0 = BiasRow("pivot", none);
        CHECK(r0.find("precision -  recall 0%  signed mean -  SD -  median |err| -") != std::string::npos);

        Bias one;
        MatchResult m = MatchSpans({1.50}, {Span(1.43, 1.68), Span(3.0, 3.2)}, 0.150, 0.0, 10.0);
        one.Add(m);
        const std::string r1 = BiasRow("slide", one);
        CHECK(r1.find("REF 1  matched 1  missed 0  extra 1  precision 50%  recall 100%  signed mean -70.0 ms  SD -  "
                      "median |err| 70.0 ms") != std::string::npos);

        Bias two;
        two.ref = two.matched = 2;
        two.err = {0.010, 0.030};
        const std::string r2 = BiasRow("lift", two);
        CHECK(r2.find("signed mean +20.0 ms  SD 14.1 ms  median |err| 20.0 ms") != std::string::npos);
    }

    if (g_fails) {
        std::printf("%d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("event_bias_test: all checks passed\n");
    return 0;
}
