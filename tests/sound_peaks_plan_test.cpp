// SPDX-License-Identifier: MIT
//
// Host test of 13-55's pure rules of the one peak reader: src/sound_peaks_plan.h (the rate a file is
// read at, its cache key, which waiting file is read next, REAPER's peak points folded into PeakData).

#include "sound_peaks_plan.h"

#include <cstdio>
#include <string>

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

int main()
{
    // ---- ChoosePeakRate: short files from their samples, long ones from the peak file ----
    CHECK(ChoosePeakRate(0.0) == 0.0);
    CHECK(ChoosePeakRate(-1.0) == 0.0);
    CHECK(ChoosePeakRate(0.25) == kPeakHiResRate);
    CHECK(ChoosePeakRate(kPeakHiResMaxSeconds) == kPeakHiResRate);
    CHECK(ChoosePeakRate(kPeakHiResMaxSeconds + 0.01) == kPeakFileRate);
    CHECK(ChoosePeakRate(3600.0) == kPeakFileRate);
    CHECK(kPeakFileRate < 400.0);  // under the peak file's finest mipmap (~400/s)
    CHECK(PeakReadsSamples(2.0) && !PeakReadsSamples(60.0) && !PeakReadsSamples(0.0));

    // ---- PeakBins: ceil + 1 (13-7b's sizing) ----
    CHECK(PeakBins(1.0, 1000.0) == 1001);
    CHECK(PeakBins(0.0015, 1000.0) == 3);
    CHECK(PeakBins(0.0, 1000.0) == 0 && PeakBins(1.0, 0.0) == 0);

    // ---- PeakCacheKey: one entry per Windows file ----
    CHECK(PeakCacheKey("C:/SFX/Foot_01.WAV") == "c:\\sfx\\foot_01.wav");
    CHECK(PeakCacheKey("c:\\sfx\\foot_01.wav") == PeakCacheKey("C:/SFX/Foot_01.WAV"));
    CHECK(PeakCacheKey("C:/SFX/Foot_01.wav") != PeakCacheKey("C:/SFX/Foot_02.wav"));
    CHECK(PeakCacheKey("") == "");
    CHECK(PeakCacheKey("\\\\nas\\Été\\x.wav") == "\\\\nas\\Été\\x.wav");  // non-ASCII bytes kept

    // ---- PickNextPeakJob ----
    {
        PeakPick p = PickNextPeakJob({}, 10.0);
        CHECK(p.next == -1 && p.drop.empty());
    }
    {  // the newest fresh one first (the file just clicked)
        PeakPick p = PickNextPeakJob({{1, 10.0}, {3, 10.0}, {2, 10.0}}, 10.1);
        CHECK(p.next == 1 && p.drop.empty());
    }
    {  // a fresh one before a newer stale-ish one
        PeakPick p = PickNextPeakJob({{1, 10.0}, {5, 9.0}}, 10.2);
        CHECK(p.next == 0);
    }
    {  // nothing fresh: the newest still wanted
        PeakPick p = PickNextPeakJob({{1, 9.0}, {2, 9.2}}, 10.0);
        CHECK(p.next == 1 && p.drop.empty());
    }
    {  // nobody asked for 2 s: dropped
        PeakPick p = PickNextPeakJob({{1, 7.0}, {2, 9.9}, {3, 7.5}}, 10.0);
        CHECK(p.next == 1 && p.drop.size() == 2 && p.drop[0] == 0 && p.drop[1] == 2);
    }
    {  // all dropped
        PeakPick p = PickNextPeakJob({{1, 1.0}}, 10.0);
        CHECK(p.next == -1 && p.drop.size() == 1);
    }
    {  // a clock that went back keeps the file
        PeakPick p = PickNextPeakJob({{1, 20.0}}, 10.0);
        CHECK(p.next == 0 && p.drop.empty());
    }

    // ---- FoldPeakPoints ----
    {  // mono, max and min
        PeakData p;
        PeakInit(&p, 1, 4);
        const double mx[] = {0.5, 0.1, -0.2};
        const double mn[] = {-0.25, -0.75, -0.3};
        FoldPeakPoints(&p, 1, mx, mn, 3, 1);
        CHECK(p.max_abs[0] == 0.0f);
        CHECK(p.max_abs[1] == 0.5f && p.max_abs[2] == 0.75f && p.max_abs[3] == 0.3f);
        CHECK(PeakChannels(p) == 1);
    }
    {  // stereo lanes, no minima
        PeakData p;
        PeakInit(&p, 2, 2);
        const float mx[] = {0.5f, 0.25f, 0.125f, 1.5f};
        FoldPeakPoints(&p, 0, mx, static_cast<const float*>(nullptr), 2, 2);
        CHECK(PeakChannels(p) == 2);
        CHECK(p.max_abs[0] == 0.5f && p.max_abs[1] == 1.5f);
        CHECK(p.lanes[0][0] == 0.5f && p.lanes[1][0] == 0.25f);
        CHECK(p.lanes[0][1] == 0.125f && p.lanes[1][1] == 1.5f);
    }
    {  // past the end: ignored; nothing given: nothing
        PeakData p;
        PeakInit(&p, 1, 2);
        const double mx[] = {0.1, 0.2, 0.3};
        FoldPeakPoints(&p, 1, mx, static_cast<const double*>(nullptr), 3, 1);
        CHECK(p.max_abs[1] == 0.1f && p.max_abs.size() == 2);
        FoldPeakPoints(&p, 0, static_cast<const double*>(nullptr), static_cast<const double*>(nullptr), 3, 1);
        CHECK(p.max_abs[0] == 0.0f);
    }

    // ---- PeakAbandonInFlight (13-55 review) ----
    CHECK(PeakAbandonInFlight(10.0, true, 10.0 + kPeakFreshSeconds + 0.01));   // left, another wanted
    CHECK(!PeakAbandonInFlight(10.0, true, 10.0 + kPeakFreshSeconds - 0.01));  // still shown
    CHECK(!PeakAbandonInFlight(10.0, false, 100.0));                            // nobody else waits
    CHECK(!PeakAbandonInFlight(10.0, true, 5.0));                               // clock went back

    // ---- PeakReadPathName ----
    CHECK(std::string(PeakReadPathName(PeakReadPath::PeakFile)) == "peak file");
    CHECK(std::string(PeakReadPathName(PeakReadPath::BuiltThenPeakFile)) == "built then peak file");

    if (g_fail) std::printf("%d failure(s)\n", g_fail);
    else std::printf("sound_peaks_plan_test: all passed\n");
    return g_fail ? 1 : 0;
}
