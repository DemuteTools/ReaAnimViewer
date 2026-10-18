// SPDX-License-Identifier: MIT
//
// Host test of src/dash_plan.h (freeze audit #1): a dashed line or a hatch walks only its marks inside the clip
// rect, on its own phase, capped -- a shape 2e9 px wide (deep zoom on a long file) finishes at once.
//   c++ -std=c++17 -I src tests/dash_plan_test.cpp -o t && ./t

#include "dash_plan.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int g_fails = 0;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);        \
            ++g_fails;                                              \
        }                                                           \
    } while (0)

// The marks the old loop drew that touch [lo, hi] (small spans only).
std::vector<double> Naive(double start, double end, double step, double extent, double lo, double hi)
{
    std::vector<double> v;
    for (double p = start; p < end; p += step)
        if (p + extent >= lo && p <= hi) v.push_back(p);
    return v;
}

std::vector<double> Planned(double start, double end, double step, double extent, double lo, double hi)
{
    std::vector<double> v;
    const dash::Run r = dash::Visible(start, end, step, extent, lo, hi);
    for (int i = 0; i < r.count; ++i) v.push_back(r.first + i * r.step);
    return v;
}

bool Same(const std::vector<double>& a, const std::vector<double>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::fabs(a[i] - b[i]) > 1e-6) return false;
    return true;
}

}  // namespace

int main()
{
    // DashedH(-1e9, 1e9) on a 1920 px clip: at most ~width / period lines, each one touching the clip.
    {
        const dash::Run r = dash::Visible(-1e9, 1e9, 6.0, 3.0, 0.0, 1920.0);
        CHECK(r.count > 0 && r.count <= 1920 / 6 + 2);
        CHECK(r.first + 3.0 >= 0.0 && r.first <= 0.0 + 6.0);
        const double last = r.first + (r.count - 1) * r.step;
        CHECK(last <= 1920.0 && last + 6.0 > 1920.0);
        // On the line's own phase: every mark is start + k * step.
        const double k = (r.first - -1e9) / 6.0;
        CHECK(std::fabs(k - std::round(k)) < 1e-6);
    }
    // Where float steps stop moving (1.3e8 px and beyond): still a handful of lines, no endless loop.
    {
        const dash::Run r = dash::Visible(-3e8, 5e12, 6.0, 6.0, 100.0, 900.0);
        CHECK(r.count > 0 && r.count <= 800 / 6 + 3);
    }
    // The same marks as the old loop wherever that one was sane: shape inside, across, outside the clip.
    CHECK(Same(Planned(10.0, 500.0, 7.0, 4.0, 0.0, 300.0), Naive(10.0, 500.0, 7.0, 4.0, 0.0, 300.0)));
    CHECK(Same(Planned(-250.0, 120.0, 6.0, 30.0, 0.0, 300.0), Naive(-250.0, 120.0, 6.0, 30.0, 0.0, 300.0)));
    CHECK(Same(Planned(-250.5, 1000.25, 6.0, 0.0, 17.0, 301.0), Naive(-250.5, 1000.25, 6.0, 0.0, 17.0, 301.0)));
    CHECK(Same(Planned(400.0, 900.0, 6.0, 3.0, 0.0, 300.0), Naive(400.0, 900.0, 6.0, 3.0, 0.0, 300.0)));  // none
    CHECK(Planned(400.0, 900.0, 6.0, 3.0, 0.0, 300.0).empty());
    // A hatch: strokes from x - H, each H wide; the ones reaching into the clip from its left are kept.
    CHECK(Same(Planned(-40.0, 200.0, 6.0, 40.0, 0.0, 120.0), Naive(-40.0, 200.0, 6.0, 40.0, 0.0, 120.0)));
    // Nothing for a bad input; never more than the cap.
    CHECK(dash::Visible(NAN, 10.0, 6.0, 3.0, 0.0, 100.0).count == 0);
    CHECK(dash::Visible(0.0, INFINITY, 6.0, 3.0, 0.0, 100.0).count == 0);
    CHECK(dash::Visible(0.0, 10.0, 0.0, 3.0, 0.0, 100.0).count == 0);
    CHECK(dash::Visible(0.0, 10.0, -6.0, 3.0, 0.0, 100.0).count == 0);
    CHECK(dash::Visible(10.0, 0.0, 6.0, 3.0, 0.0, 100.0).count == 0);
    CHECK(dash::Visible(0.0, 1e9, 0.001, 0.0, 0.0, 1e6).count == dash::kMaxMarks);
    CHECK(dash::Visible(0.0, 1e9, 1.0, 0.0, 0.0, 1e6, 50).count == 50);
    // A coordinate kept near the canvas; NaN to its far left.
    CHECK(dash::ClampCoord(5e12, 0.0, 800.0) == 800.0f + 4096.0f);
    CHECK(dash::ClampCoord(-5e12, 0.0, 800.0) == -4096.0f);
    CHECK(dash::ClampCoord(123.5, 0.0, 800.0) == 123.5f);
    CHECK(dash::ClampCoord(NAN, 0.0, 800.0) == -4096.0f);

    if (g_fails == 0) std::printf("dash_plan: all checks passed\n");
    return g_fails == 0 ? 0 : 1;
}
