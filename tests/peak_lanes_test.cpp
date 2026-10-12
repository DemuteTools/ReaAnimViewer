// SPDX-License-Identifier: MIT
//
// Host test of the peak lane helpers in src/sound_model.h (Epic 13 story 13-14): a mono source
// keeps one lane (max_abs, no extra series), a stereo one gets L / R lanes while max_abs stays the
// max over every channel (what detection reads), more channels fold into lane c % 2. Pure.

#include "sound_model.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace rav;

namespace {

int g_fails = 0;

#define CHECK(c)                                             \
    do {                                                     \
        if (!(c)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            ++g_fails;                                       \
        }                                                    \
    } while (0)

void TestMono()
{
    PeakData p;
    PeakInit(&p, 1, 3);
    CHECK(p.max_abs.size() == 3);
    CHECK(p.channels == 1);
    CHECK(p.lanes[0].empty() && p.lanes[1].empty());  // no extra memory for mono
    const double a[] = {-0.5}, b[] = {0.25}, c[] = {0.75};
    PeakAddFrame(&p, 0, a, 1);
    PeakAddFrame(&p, 0, b, 1);
    PeakAddFrame(&p, 2, c, 1);
    PeakAddFrame(&p, 9, c, 1);  // out of range: ignored
    CHECK(p.max_abs[0] == 0.5f && p.max_abs[1] == 0.0f && p.max_abs[2] == 0.75f);
    CHECK(PeakChannels(p) == 1);
    CHECK(&PeakLane(p, 0) == &p.max_abs);
    CHECK(&PeakLane(p, 1) == &p.max_abs);
    CHECK(PeakLaneMax(p, 0, 0, 3) == 0.75f);
    CHECK(PeakLaneMax(p, 0, 0, 2) == 0.5f);
    CHECK(PeakLaneMax(p, 0, 1, 100) == 0.75f);  // clamped
    CHECK(PeakLaneMax(p, 0, 5, 9) == 0.0f);
    PeakData empty;
    CHECK(PeakChannels(empty) == 1 && PeakLaneMax(empty, 0, 0, 10) == 0.0f);
}

void TestStereo()
{
    PeakData p;
    PeakInit(&p, 2, 2);
    CHECK(p.channels == 2 && PeakChannels(p) == 2);
    const float f0[] = {0.5f, -0.25f}, f1[] = {-0.125f, 0.9f}, f2[] = {0.0f, -0.0f};
    PeakAddFrame(&p, 0, f0, 2);
    PeakAddFrame(&p, 1, f1, 2);
    PeakAddFrame(&p, 1, f2, 2);
    CHECK(p.max_abs[0] == 0.5f && p.max_abs[1] == 0.9f);  // the max over both, as before
    CHECK(p.lanes[0][0] == 0.5f && p.lanes[1][0] == 0.25f);
    CHECK(p.lanes[0][1] == 0.125f && p.lanes[1][1] == 0.9f);
    CHECK(&PeakLane(p, 0) == &p.lanes[0] && &PeakLane(p, 1) == &p.lanes[1]);
    CHECK(&PeakLane(p, 2) == &p.max_abs && &PeakLane(p, -1) == &p.max_abs);
    CHECK(PeakLaneMax(p, 0, 0, 2) == 0.5f && PeakLaneMax(p, 1, 0, 2) == 0.9f);
    // Lanes of the wrong size (a reader that filled only max_abs): read as mono.
    PeakData q = p;
    q.max_abs.push_back(0.1f);
    CHECK(PeakChannels(q) == 1 && &PeakLane(q, 1) == &q.max_abs);
    // Re-initialized as mono: the lanes are released.
    PeakInit(&p, 1, 4);
    CHECK(p.channels == 1 && p.lanes[0].capacity() == 0 && p.lanes[1].capacity() == 0);
}

void TestManyChannels()
{
    PeakData p;
    PeakInit(&p, 6, 1);
    CHECK(PeakChannels(p) == 2);
    const double f[] = {0.1, 0.2, 0.7, 0.3, 0.4, 0.6};  // L R C LFE Ls Rs
    PeakAddFrame(&p, 0, f, 6);
    CHECK(p.max_abs[0] == 0.7f);
    CHECK(p.lanes[0][0] == 0.7f);  // channels 0, 2, 4
    CHECK(p.lanes[1][0] == 0.6f);  // channels 1, 3, 5
}

void TestNaN()
{
    PeakData p;
    PeakInit(&p, 2, 1);
    const double f[] = {std::numeric_limits<double>::quiet_NaN(), 0.5};
    PeakAddFrame(&p, 0, f, 2);
    CHECK(p.max_abs[0] == 0.5f);  // as std::max(m, |s|): NaN skipped
    CHECK(p.lanes[0][0] == 0.0f && p.lanes[1][0] == 0.5f);
}

}  // namespace

int main()
{
    TestMono();
    TestStereo();
    TestManyChannels();
    TestNaN();
    if (g_fails) {
        std::printf("peak_lanes: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("peak_lanes: all pass\n");
    return 0;
}
