// SPDX-License-Identifier: MIT
//
// Host test of story 13-19's pure pieces (src/populate_progress.h): the gesture's live-item set,
// the progress fraction and text, and the ~100 ms pacing of the progress frames.
//   c++ -std=c++17 -I src tests/populate_progress_test.cpp -o t && ./t

#include "populate_progress.h"

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

bool Near(float a, float b)
{
    return std::fabs(a - b) < 1e-5f;
}

void TestLiveSet()
{
    int      a = 0, b = 0;
    LiveSet  s;
    // Outside a gesture: nothing is known, nothing is learnt (every check goes to REAPER).
    s.Learn(&a);
    CHECK(!s.Active());
    CHECK(!s.Known(&a));
    s.Begin();
    CHECK(s.Active());
    CHECK(!s.Known(&a));
    s.Learn(&a);
    CHECK(s.Known(&a));
    CHECK(!s.Known(&b));
    CHECK(!s.Known(nullptr));
    s.Learn(nullptr);
    CHECK(!s.Known(nullptr));
    // A deleted item is forgotten: the next check goes to REAPER again.
    s.Forget(&a);
    CHECK(!s.Known(&a));
    s.Learn(&a);
    // Nested gestures share the outer set; only the outer end clears it.
    s.Begin();
    CHECK(s.Known(&a));
    s.End();
    CHECK(s.Active());
    CHECK(s.Known(&a));
    s.End();
    CHECK(!s.Active());
    CHECK(!s.Known(&a));
    // A new gesture starts empty (a pointer may be reused by another item).
    s.Begin();
    CHECK(!s.Known(&a));
    s.End();
    s.End();  // an extra end is harmless
    CHECK(!s.Active());
}

void TestFraction()
{
    PopulateProgress p;
    CHECK(Near(ProgressFraction(p), 0.0f));  // no items yet
    p.items = 4;
    CHECK(Near(ProgressFraction(p), 0.0f));
    p.item = 1;  // the second item, its events not counted yet
    CHECK(Near(ProgressFraction(p), 0.25f));
    p.events = 10;
    p.event = 5;
    CHECK(Near(ProgressFraction(p), 0.375f));
    p.item = 3;
    p.event = 10;
    CHECK(Near(ProgressFraction(p), 1.0f));
    p.event = 50;  // out of range: clamped
    CHECK(Near(ProgressFraction(p), 1.0f));
    p.item = 9;
    CHECK(Near(ProgressFraction(p), 1.0f));
    p.item = -3;
    p.event = -1;
    CHECK(Near(ProgressFraction(p), 0.0f));
}

void TestText()
{
    PopulateProgress p;
    CHECK(ProgressText(p) == "Item 1 of 1");
    p.items = 5;
    p.item = 1;
    CHECK(ProgressText(p) == "Item 2 of 5");
    p.events = 120;
    p.event = 40;
    CHECK(ProgressText(p) == "Item 2 of 5 \xC2\xB7 event 40 of 120");
    p.item = 7;  // clamped to the last item
    p.event = 500;
    CHECK(ProgressText(p) == "Item 5 of 5 \xC2\xB7 event 120 of 120");
}

void TestPacer()
{
    ProgressPacer pace(0.1);
    CHECK(pace.Due(10.0));    // the first frame at once (before any work)
    CHECK(!pace.Due(10.05));
    CHECK(!pace.Due(10.099));
    CHECK(pace.Due(10.1));
    CHECK(!pace.Due(10.15));
    CHECK(pace.Due(10.5));    // a slow event: due at once
    CHECK(pace.Due(3.0));     // the clock went back: due, restarts from there
    CHECK(!pace.Due(3.05));
    ProgressPacer every(0.0);
    CHECK(every.Due(1.0));
    CHECK(every.Due(1.0));
}

}  // namespace

int main()
{
    TestLiveSet();
    TestFraction();
    TestText();
    TestPacer();
    if (g_fails) {
        std::printf("populate_progress_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("populate_progress_test: all passed\n");
    return 0;
}
