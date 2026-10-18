// SPDX-License-Identifier: MIT
//
// The parse cache's recent files (freeze fix #1, 2026-10-19; asset_cache.cpp). The cache keeps the
// files the video FX tracks use (pinned) and, beside them, the most recently used other parses (the
// viewer's, the Tagging view's, the follow's and Populate's), so switching between animation items
// or a follow pass does not parse the same file again each time. Those are bounded by count and by
// an approximate size in memory: the least recently used go first. The most recently used one is
// always kept, even alone over the size bound (the file just parsed is the one being used).
//
// Pure, header-only, host-tested (tests/asset_lru_test.cpp); the caller holds its own lock.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rav {

struct LruCandidate {
    uint64_t    last_use = 0;  // a counter: higher = used more recently
    std::size_t bytes = 0;     // approximate size in memory
    bool        pinned = false;  // a video FX track uses it: never dropped, not counted
};

constexpr std::size_t kAssetLruMaxCount = 16;
constexpr std::size_t kAssetLruMaxBytes = std::size_t(1) << 30;  // 1 GiB

// The indices (into `c`) of the entries to drop so that the unpinned ones fit max_count entries and
// max_bytes, least recently used first.
inline std::vector<std::size_t> LruEvictions(const std::vector<LruCandidate>& c, std::size_t max_count,
                                             std::size_t max_bytes)
{
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < c.size(); ++i)
        if (!c[i].pinned) order.push_back(i);
    // Most recent first (ties: the later index first, a stable answer).
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return c[a].last_use != c[b].last_use ? c[a].last_use > c[b].last_use : a > b;
    });
    // Kept: the most recent ones while they fit (the first always); from the first that does not,
    // that one and every older one go.
    std::vector<std::size_t> drop;
    std::size_t              bytes = 0;
    bool                     full = false;
    for (std::size_t k = 0; k < order.size(); ++k) {
        const LruCandidate& e = c[order[k]];
        if (k > 0 && !full) full = k >= max_count || e.bytes > max_bytes - bytes;
        if (full) {
            drop.push_back(order[k]);
        } else {
            bytes = e.bytes > max_bytes - bytes ? max_bytes : bytes + e.bytes;
        }
    }
    return drop;
}

}  // namespace rav
