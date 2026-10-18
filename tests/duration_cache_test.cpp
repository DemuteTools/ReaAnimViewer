// SPDX-License-Identifier: MIT
//
// Host test of src/duration_cache.h (freeze fix #2): an animation source's length is read once per
// file version (path, date, size), and a re-export is re-read only once the file stopped changing.
//   c++ -std=c++17 -I src tests/duration_cache_test.cpp -o t -pthread && ./t

#include "duration_cache.h"

#include <cstdio>
#include <string>
#include <thread>
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

struct St {
    long long          time = 0;
    unsigned long long size = 0;
    bool operator==(const St& o) const { return time == o.time && size == o.size; }
};

}  // namespace

int main()
{
    // Same path and version: kept; another version or path: not.
    {
        rav::DurationCache<St> c;
        double                 len = -1.0;
        CHECK(!c.Find("a.fbx", St{1, 10}, &len));
        c.Store("a.fbx", St{1, 10}, 2.5);
        CHECK(c.Find("a.fbx", St{1, 10}, &len) && len == 2.5);
        CHECK(!c.Find("a.fbx", St{2, 10}, &len));  // re-exported: its date
        CHECK(!c.Find("a.fbx", St{1, 11}, &len));  // its size
        CHECK(!c.Find("b.fbx", St{1, 10}, &len));
        // The new version replaces the old one (one entry per file).
        c.Store("a.fbx", St{2, 10}, 3.0);
        CHECK(c.Find("a.fbx", St{2, 10}, &len) && len == 3.0 && !c.Find("a.fbx", St{1, 10}, &len));
        CHECK(c.Size() == 1);
        // A zero length (no clip) is a length too.
        c.Store("none.fbx", St{5, 5}, 0.0);
        CHECK(c.Find("none.fbx", St{5, 5}, &len) && len == 0.0);
    }
    // Bounded.
    {
        rav::DurationCache<St> c(3);
        for (int i = 0; i < 10; ++i) c.Store("f" + std::to_string(i), St{i, 1}, i);
        CHECK(c.Size() == 3);
        double len = 0.0;
        CHECK(c.Find("f9", St{9, 1}, &len) && len == 9.0);  // the last one stored is there
    }
    // Several threads at once.
    {
        rav::DurationCache<St>   c(64);
        std::vector<std::thread> th;
        for (int t = 0; t < 4; ++t)
            th.emplace_back([&c, t]() {
                for (int i = 0; i < 2000; ++i) {
                    const std::string p = "p" + std::to_string((i + t) % 50);
                    c.Store(p, St{i % 7, 1}, 1.0);
                    double l = 0.0;
                    c.Find(p, St{i % 7, 1}, &l);
                }
            });
        for (auto& x : th) x.join();
        CHECK(c.Size() <= 64);
    }
    // StableStamp: unchanged -> no read; a new version read again only at the second check that sees it.
    {
        rav::StableStamp<St> g;
        const St             a{1, 10}, b{2, 10}, c{3, 12};
        CHECK(!g.Check(a, a));
        CHECK(!g.Check(a, b));  // first seen: wait
        CHECK(g.Check(a, b));   // the same a second later: read it
        // Still being written: a different version at each check, never read meanwhile.
        rav::StableStamp<St> w;
        CHECK(!w.Check(a, b) && !w.Check(a, c) && !w.Check(a, St{4, 13}) && !w.Check(a, St{5, 14}));
        CHECK(w.Check(a, St{5, 14}));  // it stopped changing
        // Back to the probed version before it settled: nothing to read, and the wait starts over.
        rav::StableStamp<St> r;
        CHECK(!r.Check(a, b) && !r.Check(a, a) && !r.Check(a, b) && r.Check(a, b));
    }

    if (g_fails == 0) std::printf("duration_cache: all checks passed\n");
    return g_fails == 0 ? 0 : 1;
}
