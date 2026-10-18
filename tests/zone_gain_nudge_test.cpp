// SPDX-License-Identifier: MIT
//
// Host test of src/zone_gain_nudge.h (13-74): the selected zones' gain a notch at a time -- 1 dB per notch, each
// zone clamped as the Gain row at each notch, notches in a row merged into one undo point.
//   c++ -std=c++17 -I src tests/zone_gain_nudge_test.cpp -o t && ./t

#include "zone_gain_nudge.h"

#include <cmath>
#include <cstdio>

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

}  // namespace

int main()
{
    // The step: 1 dB a notch, signed, on the 0.1 dB grid.
    CHECK(kGainNudgeStepDb == 1.0);
    CHECK(Near(GainNudged(0.0, 1), 1.0) && Near(GainNudged(0.0, -1), -1.0) && Near(GainNudged(0.0, 3), 3.0));
    CHECK(Near(GainNudged(-2.34, 1), -1.3));  // a typed -2.34 lands on the grid
    CHECK(Near(GainNudged(5.0, 0), 5.0));
    // Clamped as the Gain row (-60..+24), notch by notch: no overshoot to wind back.
    CHECK(Near(GainNudged(23.5, 1), 24.0) && Near(GainNudged(24.0, 5), 24.0));
    CHECK(Near(GainNudged(GainNudged(24.0, 5), -1), 23.0));  // the first notch down lowers it at once
    CHECK(Near(GainNudged(-59.6, -1), -60.0) && Near(GainNudged(-60.0, -3), -60.0));
    // Several zones: each its own value, each clamped on its own.
    const std::vector<double> v = GainsNudged({0.0, 23.0, -6.5}, 2);
    CHECK(v.size() == 3 && Near(v[0], 2.0) && Near(v[1], 24.0) && Near(v[2], -4.5));
    const std::vector<double> w = GainsNudged(v, -1);
    CHECK(Near(w[0], 1.0) && Near(w[1], 23.0) && Near(w[2], -5.5));

    // Merging: same Sound, timeline and selection, within 0.5 s of the last notch.
    GainNudgeRun r;
    r.on = true;
    r.uid = "s1";
    r.tl = -1;
    r.sel = {ZoneRef{0, 0}, ZoneRef{1, 0}};
    r.last_t = 10.0;
    CHECK(kGainNudgeMergeS == 0.5);
    CHECK(GainNudgeContinues(r, "s1", -1, r.sel, 10.2));
    CHECK(GainNudgeContinues(r, "s1", -1, r.sel, 10.5));
    CHECK(!GainNudgeContinues(r, "s1", -1, r.sel, 10.51));             // a pause: a new undo point
    CHECK(!GainNudgeContinues(r, "s2", -1, r.sel, 10.1));              // another Sound
    CHECK(!GainNudgeContinues(r, "s1", 0, r.sel, 10.1));               // another take
    CHECK(!GainNudgeContinues(r, "s1", -1, {ZoneRef{0, 0}}, 10.1));    // another selection
    CHECK(!GainNudgeContinues(r, "s1", -1, r.sel, 9.9));               // time went back (a new session)
    GainNudgeRun off = r;
    off.on = false;
    CHECK(!GainNudgeContinues(off, "s1", -1, r.sel, 10.1));
    // Due: entered once the wheel rests longer than the merge window; never when nothing runs.
    CHECK(!GainNudgeDue(r, 10.4) && !GainNudgeDue(r, 10.5) && GainNudgeDue(r, 10.6));
    CHECK(!GainNudgeDue(off, 99.0));

    if (g_fails == 0) std::printf("zone_gain_nudge: all checks passed\n");
    return g_fails == 0 ? 0 : 1;
}
