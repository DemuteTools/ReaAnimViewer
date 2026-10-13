// SPDX-License-Identifier: MIT
//
// Which animation item REAPER plays at a project position, and which clip time it shows
// there (Story 11-7). One rule for the RAV viewport, the Video view, the Tagging 3D view
// (all through GetCurrentAnimItemOn) and the video FX (FindVideoItemAt), so they show what
// REAPER plays, like native media:
//   - clip time = D_STARTOFFS + (p - item start) * D_PLAYRATE, in source time;
//   - an item that loops its source (B_LOOPSRC) repeats the clip (the Tagging strip's
//     passes, tagging_signal.h ClipPasses / ProjectToClipTime, so pose and markers agree;
//     the host test checks the two against each other);
//   - a muted item (B_MUTE) and an item on a fixed lane that does not play are skipped;
//   - on overlap: the topmost track, then the upper playing lane, then the item that
//     starts later (REAPER draws it over the earlier one), then the later in walk order.
//
// Pure C++17: no REAPER, no Windows. Host-tested (tests/play_item_plan_test.cpp).

#pragma once

#include <vector>

namespace rav {

struct PlayCandidate {
    double pos = 0.0;         // D_POSITION
    double len = 0.0;         // D_LENGTH
    double start_offs = 0.0;  // take D_STARTOFFS (source s)
    double rate = 1.0;        // take D_PLAYRATE (<= 0 or not finite reads as 1)
    double clip_len = 0.0;    // the source's length (source s); the loop period
    bool   loop = false;      // B_LOOPSRC
    bool   muted = false;     // B_MUTE
    bool   lane_plays = true; // false: on a fixed lane that does not play
    int    track_rank = 0;    // smaller wins (the track's 1-based number; unknown = INT_MAX)
    int    lane = 0;          // fixed lane index (0 off fixed lanes); smaller wins
};

// True when `c` is under project position p (pos <= p < pos + len; a non-finite p
// matches nothing).
bool PlaySpans(const PlayCandidate& c, double p);

// The clip time `c` shows at project position p (see above). Never below 0 (a NaN offset
// gives 0); past the clip end of an item that does not loop it keeps growing, the renderer
// holds the last frame.
double PlayClipTime(const PlayCandidate& c, double p);

// The index of the item REAPER plays at p among `items` (walk order), or -1.
int PickPlayingItem(const std::vector<PlayCandidate>& items, double p);

}  // namespace rav
