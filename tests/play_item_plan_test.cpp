// SPDX-License-Identifier: MIT
//
// Host test of "which item REAPER plays at p, at which clip time" (src/play_item_plan.h,
// story 11-7). No REAPER, no Windows: any C++17 compiler.

#include "play_item_plan.h"

#include <climits>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include "tagging_signal.h"

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

bool Near(double a, double b, double tol = 1e-9)
{
    return std::fabs(a - b) < tol;
}

PlayCandidate Item(double pos, double len, double offs = 0.0, double rate = 1.0, double clip = 2.0, bool loop = false)
{
    PlayCandidate c;
    c.pos = pos;
    c.len = len;
    c.start_offs = offs;
    c.rate = rate;
    c.clip_len = clip;
    c.loop = loop;
    return c;
}

}  // namespace

int main()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();

    // ---- Clip time: offset, rate, guards -----------------------------------------------------
    {
        CHECK(Near(PlayClipTime(Item(10.0, 4.0), 11.5), 1.5));             // rate 1, no offset
        CHECK(Near(PlayClipTime(Item(10.0, 4.0, 0.5), 11.0), 1.5));        // left trim
        CHECK(Near(PlayClipTime(Item(10.0, 4.0, 1.0, 2.0), 11.0), 3.0));   // rate 2
        CHECK(Near(PlayClipTime(Item(10.0, 4.0, 0.0, 0.5), 12.0), 1.0));  // rate 0.5
        CHECK(Near(PlayClipTime(Item(10.0, 4.0, 0.0, 0.0), 11.0), 1.0));   // rate 0 reads as 1
        CHECK(Near(PlayClipTime(Item(10.0, 4.0, 0.0, nan), 11.0), 1.0));   // NaN rate reads as 1
        CHECK(Near(PlayClipTime(Item(10.0, 4.0, nan), 11.0), 1.0));        // NaN offset reads as 0
        CHECK(Near(PlayClipTime(Item(10.0, 4.0, -3.0), 11.0), 0.0));       // before the clip: 0
        CHECK(Near(PlayClipTime(Item(10.0, 4.0), nan), 0.0));
        // Not looped, past the clip's end: keeps growing (the renderer holds the last frame).
        CHECK(Near(PlayClipTime(Item(10.0, 5.0, 0.0, 1.0, 2.0), 13.5), 3.5));
    }

    // ---- Loop: the clip repeats, like the Tagging strip ----------------------------------------
    {
        PlayCandidate c = Item(10.0, 5.0, 0.0, 1.0, 2.0, true);
        CHECK(Near(PlayClipTime(c, 11.0), 1.0));
        CHECK(Near(PlayClipTime(c, 12.0), 0.0));
        CHECK(Near(PlayClipTime(c, 14.5), 0.5));
        c.start_offs = 1.5;  // starts late in the clip, wraps after 0.5 s
        CHECK(Near(PlayClipTime(c, 10.25), 1.75));
        CHECK(Near(PlayClipTime(c, 10.75), 0.25));
        c.start_offs = 0.0;
        c.rate = 2.0;  // twice as fast: one pass per second
        CHECK(Near(PlayClipTime(c, 11.25), 0.5));
        CHECK(Near(PlayClipTime(c, 13.9), 1.8, 1e-6));
        c.rate = 1.0;
        c.start_offs = -0.5;  // a negative offset wraps too
        CHECK(Near(PlayClipTime(c, 10.0), 1.5));
        // Agrees with the strip's mapping at every point of the item.
        ItemClipMap m;
        m.item_pos = 10.0;
        m.item_len = 5.0;
        m.start_offs = 0.3;
        m.rate = 1.7;
        m.clip_len = 2.0;
        m.loop = true;
        PlayCandidate d = Item(10.0, 5.0, 0.3, 1.7, 2.0, true);
        for (double p = 10.0; p < 15.0; p += 0.037) {
            double t = -1.0;
            CHECK(ProjectToClipTime(m, p, &t) && Near(PlayClipTime(d, p), t));
        }
        // A loop with no clip length falls back to straight time.
        CHECK(Near(PlayClipTime(Item(10.0, 5.0, 0.0, 1.0, 0.0, true), 13.0), 3.0));
    }

    // ---- Which item: span, mute, lanes -------------------------------------------------------
    {
        std::vector<PlayCandidate> v = {Item(0.0, 2.0), Item(5.0, 2.0)};
        CHECK(PickPlayingItem(v, 1.0) == 0);
        CHECK(PickPlayingItem(v, 6.0) == 1);
        CHECK(PickPlayingItem(v, 2.0) == -1);  // end is exclusive
        CHECK(PickPlayingItem(v, 4.0) == -1);
        CHECK(PickPlayingItem(v, nan) == -1);
        CHECK(PickPlayingItem({}, 1.0) == -1);
        v[0].muted = true;
        CHECK(PickPlayingItem(v, 1.0) == -1);
        v[0].muted = false;
        v[0].lane_plays = false;
        CHECK(PickPlayingItem(v, 1.0) == -1);
    }

    // ---- Overlap -------------------------------------------------------------------------------
    {
        // Same track: the later-starting item wins, whatever the walk order.
        std::vector<PlayCandidate> v = {Item(0.0, 10.0), Item(3.0, 10.0)};
        CHECK(PickPlayingItem(v, 4.0) == 1);
        CHECK(PickPlayingItem({v[1], v[0]}, 4.0) == 0);
        CHECK(PickPlayingItem(v, 2.0) == 0);
        // Muted front item: the one behind shows.
        v[1].muted = true;
        CHECK(PickPlayingItem(v, 4.0) == 0);
        v[1].muted = false;
        // Same start: the later in walk order.
        std::vector<PlayCandidate> same = {Item(1.0, 5.0), Item(1.0, 5.0)};
        CHECK(PickPlayingItem(same, 2.0) == 1);
        // Fixed lanes: the upper playing lane first, then the later start.
        std::vector<PlayCandidate> lanes = {Item(0.0, 10.0), Item(3.0, 10.0)};
        lanes[0].lane = 0;
        lanes[1].lane = 1;
        CHECK(PickPlayingItem(lanes, 4.0) == 0);
        lanes[0].lane_plays = false;  // lane 0 does not play: lane 1 shows
        CHECK(PickPlayingItem(lanes, 4.0) == 1);
        // Tracks: the topmost track wins over a later start.
        std::vector<PlayCandidate> tracks = {Item(3.0, 10.0), Item(0.0, 10.0)};
        tracks[0].track_rank = 3;
        tracks[1].track_rank = 1;
        CHECK(PickPlayingItem(tracks, 4.0) == 1);
        tracks[1].track_rank = INT_MAX;  // unknown track: lowest priority, still picked alone
        CHECK(PickPlayingItem(tracks, 4.0) == 0);
        CHECK(PickPlayingItem({tracks[1]}, 4.0) == 0);
    }

    if (g_fails) {
        std::printf("play_item_plan_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("play_item_plan_test: all passed\n");
    return 0;
}
