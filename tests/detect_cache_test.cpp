// SPDX-License-Identifier: MIT
//
// Host test of src/detect_cache.h (freeze fix #1): the follow's detection is kept for exactly what it
// reads (the rules record, the file's version, the role mapping file), with no time limit, and the
// detections a tick runs are spread under a time budget (at least one a tick).
//   c++ -std=c++17 -I src tests/detect_cache_test.cpp -o t && ./t

#include "detect_cache.h"

#include <cstdio>
#include <string>
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

rav::DetectInputs In(const char* rules, const char* path, long long t, unsigned long long sz)
{
    return rav::DetectInputs{rules, path, t, sz};
}

}  // namespace

int main()
{
    using rav::DetectCache;
    using rav::DetectInputs;

    // Same inputs: a hit, as many passes later as wanted (no time limit).
    {
        DetectCache<int> c;
        c.BeginPass("roles A");
        CHECK(c.Find(In("r", "a.fbx", 10, 100)) == nullptr);
        c.Store(In("r", "a.fbx", 10, 100), 7);
        for (int pass = 0; pass < 50; ++pass) {
            c.BeginPass("roles A");
            const int* v = c.Find(In("r", "a.fbx", 10, 100));
            CHECK(v && *v == 7);
            c.DropUnused();
        }
        CHECK(c.Size() == 1);
    }
    // Any input changed: a miss (the detection runs again).
    {
        DetectCache<int> c;
        c.BeginPass("roles A");
        c.Store(In("r", "a.fbx", 10, 100), 7);
        CHECK(c.Find(In("r2", "a.fbx", 10, 100)) == nullptr);   // the rules record
        CHECK(c.Find(In("r", "b.fbx", 10, 100)) == nullptr);    // the file
        CHECK(c.Find(In("r", "a.fbx", 11, 100)) == nullptr);    // re-exported: its date
        CHECK(c.Find(In("r", "a.fbx", 10, 101)) == nullptr);    // its size
        CHECK(c.Find(In("r", "a.fbx", 10, 100)) != nullptr);
        CHECK(c.Has(In("r", "a.fbx", 10, 100)) && !c.Has(In("r", "a.fbx", 10, 99)));
        // The role mapping file changed: everything runs again.
        c.BeginPass("roles B");
        CHECK(c.Find(In("r", "a.fbx", 10, 100)) == nullptr && c.Size() == 0);
        c.Store(In("r", "a.fbx", 10, 100), 8);
        c.BeginPass("roles B");
        CHECK(c.Find(In("r", "a.fbx", 10, 100)) && *c.Find(In("r", "a.fbx", 10, 100)) == 8);
    }
    // Two items reading the same inputs (copies) share one detection.
    {
        DetectCache<int> c;
        c.BeginPass("");
        c.Store(In("same", "a.fbx", 1, 2), 3);
        CHECK(c.Find(In("same", "a.fbx", 1, 2)) && c.Find(In("same", "a.fbx", 1, 2)) && c.Size() == 1);
    }
    // A full pass drops what it did not look up; a stored entry counts as looked up in its pass.
    {
        DetectCache<int> c;
        c.BeginPass("");
        c.Store(In("a", "a", 0, 0), 1);
        c.Store(In("b", "b", 0, 0), 2);
        c.DropUnused();
        CHECK(c.Size() == 2);
        c.BeginPass("");  // a waiting pass (detections spread): stores only, drops nothing
        c.Store(In("c", "c", 0, 0), 3);
        c.BeginPass("");  // the full pass: reads a and c
        CHECK(c.Find(In("a", "a", 0, 0)) && c.Find(In("c", "c", 0, 0)));
        c.DropUnused();
        CHECK(c.Size() == 2 && !c.Has(In("b", "b", 0, 0)));
    }
    // Keys order strictly (std::map key).
    {
        const DetectInputs a = In("r", "p", 1, 1), b = In("r", "p", 1, 2);
        CHECK((a < b) != (b < a) && !(a < a) && a == a && !(a == b));
    }

    // RunUnderBudget: at least one, then while the budget lasts, in order; the rest wait.
    {
        double              clock = 0.0;
        std::vector<size_t> ran;
        auto                run = [&](size_t i) {
            ran.push_back(i);
            clock += 12.0;  // each detection: 12 ms
        };
        auto elapsed = [&]() { return clock; };
        CHECK(rav::RunUnderBudget(10, 30.0, run, elapsed) == 3);  // 0, 12, 24 < 30: three
        CHECK(ran == (std::vector<size_t>{0, 1, 2}));
        // One detection over the budget on its own: still one per tick.
        ran.clear();
        clock = 0.0;
        auto slow = [&](size_t i) {
            ran.push_back(i);
            clock += 2000.0;
        };
        CHECK(rav::RunUnderBudget(5, 30.0, slow, elapsed) == 1 && ran.size() == 1);
        // Ticks one after the other run them all, each once.
        std::vector<size_t> all;
        size_t              left = 7;
        int                 ticks = 0;
        while (left > 0 && ticks < 100) {
            clock = 0.0;
            const size_t base = 7 - left;
            const size_t n = rav::RunUnderBudget(
                left, 30.0,
                [&](size_t i) {
                    all.push_back(base + i);
                    clock += 20.0;
                },
                elapsed);
            CHECK(n >= 1);
            left -= n;
            ++ticks;
        }
        CHECK(left == 0 && all == (std::vector<size_t>{0, 1, 2, 3, 4, 5, 6}) && ticks == 4);
        // Nothing to run.
        CHECK(rav::RunUnderBudget(0, 30.0, run, elapsed) == 0);
    }

    if (g_fails == 0) std::printf("detect_cache: all checks passed\n");
    return g_fails == 0 ? 0 : 1;
}
