// SPDX-License-Identifier: MIT
//
// Story 13-19: the pure pieces of a fast Populate with a progress bar (host-tested,
// tests/populate_progress_test.cpp). No REAPER, no ImGui.

#pragma once

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_set>

namespace rav {

// A gesture's known-live pointers. REAPER's pointer check (ValidatePtr2 on an item) walks the
// project's items; inside one gesture an item seen live stays live until the gesture deletes it,
// so it is checked once. Begin / End nest (the outer gesture's set); outside a gesture nothing is
// known and nothing is learnt. The gesture must Forget every item it deletes.
class LiveSet {
public:
    void Begin() { ++depth_; }
    void End()
    {
        if (depth_ > 0 && --depth_ == 0) known_.clear();
    }
    bool Active() const { return depth_ > 0; }
    bool Known(const void* p) const { return depth_ > 0 && p && known_.count(p) != 0; }
    void Learn(const void* p)
    {
        if (depth_ > 0 && p) known_.insert(p);
    }
    void Forget(const void* p) { known_.erase(p); }

private:
    int                             depth_ = 0;
    std::unordered_set<const void*> known_;
};

// Where a Populate is: `item` = the index of the item being worked on (0-based) of `items`;
// `event` = the events of that item handled so far, of `events` (0 = not counted yet).
struct PopulateProgress {
    int item = 0, items = 0;
    int event = 0, events = 0;
};

// The bar's fill, 0..1: the items done plus the share of the current one.
inline float ProgressFraction(const PopulateProgress& p)
{
    if (p.items <= 0) return 0.0f;
    const int   item = std::clamp(p.item, 0, p.items);
    const float within =
        p.events > 0 ? static_cast<float>(std::clamp(p.event, 0, p.events)) / static_cast<float>(p.events) : 0.0f;
    return std::clamp((static_cast<float>(item) + within) / static_cast<float>(p.items), 0.0f, 1.0f);
}

// "Item 2 of 5 · event 40 of 120" ("Item 1 of 1" while the item's events are not counted yet).
inline std::string ProgressText(const PopulateProgress& p)
{
    char      buf[96];
    const int items = std::max(p.items, 1);
    const int item = std::clamp(p.item + 1, 1, items);
    if (p.events > 0)
        std::snprintf(buf, sizeof(buf), "Item %d of %d \xC2\xB7 event %d of %d", item, items, std::clamp(p.event, 0, p.events),
                      p.events);
    else
        std::snprintf(buf, sizeof(buf), "Item %d of %d", item, items);
    return buf;
}

// Paces the progress frames: the first call is due, then one at least `interval_s` after the last
// due one (a clock going back is due at once).
class ProgressPacer {
public:
    explicit ProgressPacer(double interval_s = 0.1) : interval_(interval_s) {}
    bool Due(double now_s)
    {
        if (started_ && now_s >= last_ && now_s - last_ < interval_ - 1e-9) return false;
        started_ = true;
        last_ = now_s;
        return true;
    }

private:
    double interval_;
    double last_ = 0.0;
    bool   started_ = false;
};

}  // namespace rav
