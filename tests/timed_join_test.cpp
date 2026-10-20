// SPDX-License-Identifier: MIT
// Freeze audit #8: src/timed_join.h.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "timed_join.h"

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);             \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

int main()
{
    using namespace std::chrono;
    {  // a worker that ends in time is joined
        std::atomic<bool> done{false};
        std::thread       t([&] {
            std::this_thread::sleep_for(milliseconds(30));
            done = true;
        });
        CHECK(rav::JoinWithin(t, [&] { return done.load(); }, 2.0));
        CHECK(!t.joinable());
    }
    {  // no thread: nothing to wait for
        std::thread t;
        CHECK(rav::JoinWithin(t, [] { return false; }, 0.01));
    }
    {  // a stuck worker is given up on after the wait (detached), without hanging the caller
        static std::atomic<bool> release{false};
        static std::atomic<bool> finished{false};
        std::thread              t([] {
            while (!release) std::this_thread::sleep_for(milliseconds(5));
            finished = true;
        });
        const auto t0 = steady_clock::now();
        CHECK(!rav::JoinWithin(t, [] { return finished.load(); }, 0.1));
        CHECK(duration<double>(steady_clock::now() - t0).count() < 1.5);
        CHECK(!t.joinable());
        release = true;  // let the detached thread end before the process does
        std::this_thread::sleep_for(milliseconds(50));
    }
    {  // several threads
        std::atomic<int>         live{3};
        std::vector<std::thread> ts;
        for (int i = 0; i < 3; ++i)
            ts.emplace_back([&] {
                std::this_thread::sleep_for(milliseconds(20));
                --live;
            });
        CHECK(rav::JoinAllWithin(ts, [&] { return live.load() == 0; }, 2.0));
        CHECK(!ts[0].joinable() && !ts[2].joinable());
    }
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("timed_join: all passed\n");
    return 0;
}
