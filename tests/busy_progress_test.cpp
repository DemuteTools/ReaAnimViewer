// SPDX-License-Identifier: MIT
//
// Host test of 13-59's shared busy card model (src/busy_progress.h): the bar's fill, the status
// text, the ~150 ms show threshold and ~100 ms repaint, and the memory of a long gesture.
//   c++ -std=c++17 -I src tests/busy_progress_test.cpp -o t && ./t

#include "busy_progress.h"

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

void TestFraction()
{
    BusyFrame f;
    CHECK(Near(BusyFraction(f), 0.0f));  // nothing counted
    f.items = 4;
    f.item = 1;
    CHECK(Near(BusyFraction(f), 0.25f));  // item 2 of 4, its steps not counted yet
    f.steps = 10;
    f.step = 5;
    CHECK(Near(BusyFraction(f), 0.375f));  // half of item 2
    f.item = 9;  // past the end: clamped
    CHECK(Near(BusyFraction(f), 1.0f));
    BusyFrame s;  // steps only
    s.items = 0;
    s.steps = 8;
    s.step = 2;
    CHECK(Near(BusyFraction(s), 0.25f));
    s.step = -3;
    CHECK(Near(BusyFraction(s), 0.0f));
}

void TestText()
{
    BusyFrame f;
    CHECK(BusyText(f) == "Working...");
    f.items = 5;
    f.item = 1;
    CHECK(BusyText(f) == "Item 2 of 5");
    f.steps = 120;
    f.step = 40;
    CHECK(BusyText(f) == "Item 2 of 5 \xC2\xB7 event 40 of 120");
    f.phase = "Writing";
    CHECK(BusyText(f) == "Writing \xC2\xB7 Item 2 of 5 \xC2\xB7 event 40 of 120");
    BusyFrame s;
    s.steps = 3;
    s.step = 1;
    s.step_unit = "variation";
    CHECK(BusyText(s) == "Variation 1 of 3");  // capitalised when it leads
    BusyFrame p;
    p.phase = "Searching";
    CHECK(BusyText(p) == "Searching");
}

void TestGate()
{
    BusyGate g;
    g.Start(10.0);
    CHECK(!g.Due(10.0));
    CHECK(!g.Due(10.1));  // a quick gesture shows nothing
    CHECK(!g.Shown());
    CHECK(g.Due(10.15));  // the first frame after ~150 ms
    CHECK(g.Shown());
    CHECK(!g.Due(10.2));
    CHECK(g.Due(10.25));  // then about every 100 ms
    CHECK(g.Due(10.0));   // a clock going back is due at once
    BusyGate a;
    a.Start(5.0, true);
    CHECK(a.Due(5.0));  // a gesture known to be long: at once
    CHECK(!a.Due(5.05));
    BusyGate r;  // Start again resets
    r.Start(1.0);
    CHECK(r.Due(2.0));
    r.Start(3.0);
    CHECK(!r.Shown());
    CHECK(!r.Due(3.01));
}

void TestMemory()
{
    BusyMemory m;
    CHECK(!m.WasLong("Reshuffling sounds"));
    m.Record("Reshuffling sounds", 0.1);
    CHECK(!m.WasLong("Reshuffling sounds"));
    m.Record("Reshuffling sounds", 0.3);
    CHECK(m.WasLong("Reshuffling sounds"));
    CHECK(!m.WasLong("Populating sounds"));
    m.Record("Reshuffling sounds", 0.05);  // the last run counts
    CHECK(!m.WasLong("Reshuffling sounds"));
}

}  // namespace

int main()
{
    TestFraction();
    TestText();
    TestGate();
    TestMemory();
    if (g_fails) {
        std::printf("%d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("busy_progress: all checks passed\n");
    return 0;
}
