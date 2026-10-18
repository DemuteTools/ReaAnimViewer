// SPDX-License-Identifier: MIT
//
// Host test of src/asset_lru.h (freeze fix #1): the parse cache keeps, beside the video FX's files,
// the most recently used other parses within a count and a size bound, least recently used first out.
//   c++ -std=c++17 -I src tests/asset_lru_test.cpp -o t && ./t

#include "asset_lru.h"

#include <algorithm>
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

std::vector<size_t> Sorted(std::vector<size_t> v)
{
    std::sort(v.begin(), v.end());
    return v;
}

}  // namespace

int main()
{
    using rav::LruCandidate;
    using rav::LruEvictions;

    // Within both bounds: nothing goes.
    CHECK(LruEvictions({{1, 10, false}, {2, 10, false}}, 4, 100).empty());
    CHECK(LruEvictions({}, 4, 100).empty());
    // Over the count: the least recently used go.
    CHECK(Sorted(LruEvictions({{5, 1, false}, {1, 1, false}, {3, 1, false}, {4, 1, false}}, 2, 100)) ==
          (std::vector<size_t>{1, 2}));
    // Over the size: the oldest go until the rest fits.
    CHECK(Sorted(LruEvictions({{1, 40, false}, {2, 40, false}, {3, 40, false}}, 10, 100)) ==
          (std::vector<size_t>{0}));
    // A strict recency order: an older small one does not stay when a newer one did not fit.
    CHECK(Sorted(LruEvictions({{3, 60, false}, {2, 60, false}, {1, 1, false}}, 10, 100)) ==
          (std::vector<size_t>{1, 2}));
    // The most recent one stays even alone over the size bound.
    CHECK(Sorted(LruEvictions({{9, 500, false}, {1, 10, false}}, 10, 100)) == (std::vector<size_t>{1}));
    CHECK(LruEvictions({{9, 500, false}}, 10, 100).empty());
    CHECK(LruEvictions({{9, 1, false}}, 0, 100).empty());
    // The video FX's files (pinned) never go and do not count.
    {
        const std::vector<LruCandidate> c = {{1, 90, true}, {2, 90, true}, {3, 50, false}, {4, 40, false}, {0, 5, true}};
        CHECK(LruEvictions(c, 2, 100).empty());
        CHECK(Sorted(LruEvictions(c, 1, 100)) == (std::vector<size_t>{2}));
    }
    // The real bounds: 16 recent files.
    {
        std::vector<LruCandidate> c;
        for (uint64_t i = 0; i < 40; ++i) c.push_back({i + 1, 1000, false});
        const std::vector<size_t> d = LruEvictions(c, rav::kAssetLruMaxCount, rav::kAssetLruMaxBytes);
        CHECK(d.size() == 40 - rav::kAssetLruMaxCount);
        for (size_t i : d) CHECK(i < 40 - rav::kAssetLruMaxCount);  // the 24 oldest
    }
    // Huge sizes do not wrap the sum.
    {
        const size_t big = static_cast<size_t>(-1) / 2 + 10;
        CHECK(Sorted(LruEvictions({{3, big, false}, {2, big, false}}, 10, static_cast<size_t>(-1))) ==
              (std::vector<size_t>{1}));
    }

    if (g_fails == 0) std::printf("asset_lru: all checks passed\n");
    return g_fails == 0 ? 0 : 1;
}
