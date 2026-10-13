// SPDX-License-Identifier: MIT
//
// See play_item_plan.h.

#include "play_item_plan.h"

#include <cmath>

namespace rav {

bool PlaySpans(const PlayCandidate& c, double p)
{
    return p >= c.pos && p < c.pos + c.len;
}

double PlayClipTime(const PlayCandidate& c, double p)
{
    const double rate = (c.rate > 0.0 && std::isfinite(c.rate)) ? c.rate : 1.0;
    const double offs = std::isfinite(c.start_offs) ? c.start_offs : 0.0;
    double at = offs + (p - c.pos) * rate;  // source time
    // Looped: the pass of the clip p falls in, as tagging_signal.cpp ClipPasses cuts them
    // (pass k covers source [k L, (k+1) L)), without building the pass list: this runs per
    // video frame. tests/play_item_plan_test.cpp checks it against ProjectToClipTime.
    if (c.loop && c.clip_len > 0.0 && std::isfinite(c.clip_len) && std::isfinite(at)) {
        at -= std::floor(at / c.clip_len) * c.clip_len;
        if (at >= c.clip_len) at = 0.0;  // rounding at a pass edge
    }
    // Low guard; the negated form also catches a non-finite input. Not looped, the renderer
    // clamps the high end to the clip (holds the last frame).
    return (at >= 0.0) ? at : 0.0;
}

int PickPlayingItem(const std::vector<PlayCandidate>& items, double p)
{
    int best = -1;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const PlayCandidate& c = items[i];
        if (c.muted || !c.lane_plays || !PlaySpans(c, p)) continue;
        if (best < 0) {
            best = i;
            continue;
        }
        const PlayCandidate& b = items[best];
        if (c.track_rank != b.track_rank) {
            if (c.track_rank < b.track_rank) best = i;
            continue;
        }
        if (c.lane != b.lane) {
            if (c.lane < b.lane) best = i;
            continue;
        }
        if (c.pos >= b.pos) best = i;  // later start wins; a tie goes to the later in walk order
    }
    return best;
}

}  // namespace rav
