// SPDX-License-Identifier: MIT
//
// Host test of src/watchdog.cpp's platform-neutral parts (freeze audit): the breadcrumb ring (order, wrap, cut
// arguments, several threads writing at once), the main thread's scope stack, the worker slots and the report's
// text (no allocation, cut at its buffer).
//   c++ -std=c++17 -DRAV_HOST_TEST -DRAV_WATCHDOG_HOST -I src tests/watchdog_test.cpp src/watchdog.cpp -pthread -o t && ./t

#include "watchdog.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

bool Has(const char* text, const char* part) { return std::strstr(text, part) != nullptr; }

}  // namespace

int main()
{
    wd::ResetForTest();
    wd::SetMainThread();
    static wd::CrumbView cr[wd::kCrumbs];
    // The ring: the last 256, oldest first; an argument cut to 55 bytes; a number as text.
    {
        for (int i = 0; i < 300; ++i) wd::CrumbNum("n", i);
        const int n = wd::SnapshotCrumbs(cr, wd::kCrumbs);
        CHECK(n == wd::kCrumbs);
        CHECK(std::strcmp(cr[0].arg, "44") == 0 && std::strcmp(cr[n - 1].arg, "299") == 0);
        bool ordered = true;
        for (int i = 1; i < n; ++i) ordered = ordered && cr[i].seq == cr[i - 1].seq + 1;
        CHECK(ordered);
        const std::string longer(200, 'x');
        wd::Crumb("long", longer);
        const int m = wd::SnapshotCrumbs(cr, wd::kCrumbs);
        CHECK(std::strlen(cr[m - 1].arg) == static_cast<size_t>(wd::kArgLen - 1) && std::strcmp(cr[m - 1].what, "long") == 0);
        wd::CrumbNum("neg", -9223372036854775807LL - 1);  // the integer text is made by hand: its extremes
        wd::CrumbNum("zero", 0);
        const int m2 = wd::SnapshotCrumbs(cr, wd::kCrumbs);
        CHECK(std::strcmp(cr[m2 - 2].arg, "-9223372036854775808") == 0 && std::strcmp(cr[m2 - 1].arg, "0") == 0);
        wd::Crumb("none");
        CHECK(wd::SnapshotCrumbs(cr, wd::kCrumbs) == wd::kCrumbs && cr[wd::kCrumbs - 1].arg[0] == '\0');
        CHECK(wd::SnapshotCrumbs(cr, 10) == 10);  // a short buffer: the oldest ones
    }
    // Several threads at once: every crumb read back is whole (its what matches its argument).
    {
        wd::ResetForTest();
        static const char* kWhat[4] = {"t0", "t1", "t2", "t3"};
        std::vector<std::thread> th;
        for (int t = 0; t < 4; ++t)
            th.emplace_back([t] {
                for (int i = 0; i < 20000; ++i) wd::Crumb(kWhat[t], kWhat[t]);
            });
        int  reads = 0;
        bool whole = true;
        for (int k = 0; k < 200; ++k) {
            const int n = wd::SnapshotCrumbs(cr, wd::kCrumbs);
            reads += n;
            for (int i = 0; i < n; ++i) whole = whole && cr[i].what && std::strcmp(cr[i].what, cr[i].arg) == 0;
        }
        for (std::thread& x : th) x.join();
        CHECK(whole);
        CHECK(wd::SnapshotCrumbs(cr, wd::kCrumbs) == wd::kCrumbs);
        (void)reads;
    }
    // The scope stack: the main thread's only, nested, popped at its end; deeper than 16 counted, not kept.
    {
        wd::ResetForTest();
        wd::ScopeView sc[wd::kScopeDepth];
        int           depth = -1;
        {
            const wd::Scope a("RenderTick");
            {
                const wd::Scope b("Sound Edit", std::string("Footsteps"));
                const wd::Scope c("Zones", 12);
                CHECK(wd::SnapshotScopes(sc, wd::kScopeDepth, &depth) == 3 && depth == 3);
                CHECK(std::strcmp(sc[0].what, "RenderTick") == 0 && std::strcmp(sc[1].arg, "Footsteps") == 0 &&
                      std::strcmp(sc[2].arg, "12") == 0);
                std::thread other([] { const wd::Scope o("Worker scope"); });  // another thread: a crumb only
                other.join();
                CHECK(wd::SnapshotScopes(sc, wd::kScopeDepth, &depth) == 3 && depth == 3);
            }
            CHECK(wd::SnapshotScopes(sc, wd::kScopeDepth, &depth) == 1 && depth == 1);
        }
        CHECK(wd::SnapshotScopes(sc, wd::kScopeDepth, &depth) == 0 && depth == 0);
        {
            std::vector<std::unique_ptr<wd::Scope>> deep;
            for (int i = 0; i < 20; ++i) deep.emplace_back(new wd::Scope("deep", i));
            CHECK(wd::SnapshotScopes(sc, wd::kScopeDepth, &depth) == wd::kScopeDepth && depth == 20);
            CHECK(std::strcmp(sc[15].arg, "15") == 0);
        }
        CHECK(wd::SnapshotScopes(sc, wd::kScopeDepth, &depth) == 0 && depth == 0);
    }
    // Worker slots: one per name + index, at most 8.
    {
        wd::ResetForTest();
        const int a = wd::WorkerSlot("Peak reader", 0), b = wd::WorkerSlot("Peak reader", 1);
        CHECK(a >= 0 && b >= 0 && a != b && wd::WorkerSlot("Peak reader", 0) == a);
        for (int i = 2; i < wd::kWorkers; ++i) CHECK(wd::WorkerSlot("Peak reader", i) >= 0);
        CHECK(wd::WorkerSlot("one too many", 0) == -1);
    }
    // The report's text.
    {
        wd::ResetForTest();
        static char buf[64 * 1024];
        wd::ReportHead h;
        h.version = "0.2.2-test";
        h.when = "2026-10-18 14:03:22";
        h.now_ms = wd::NowMs();
        h.stall_ms = 12345;
        size_t n = wd::FormatReport(buf, sizeof(buf), h);
        CHECK(n == std::strlen(buf));
        CHECK(Has(buf, "Version: 0.2.2-test") && Has(buf, "no heartbeat for 12.3 s") && Has(buf, "outside RAV"));
        CHECK(Has(buf, "Audio thread: never ran") && Has(buf, "Workers: none started") && Has(buf, "Last 0 crumbs"));
        const int slot = wd::WorkerSlot("Peak reader", 0);
        wd::WorkerBusy(slot, "peaks", "C:\\sounds\\steps.wav");
        wd::AudioEnter("AuditionMix");
        {
            const wd::Scope s("Sound Edit", "Footsteps");
            wd::Crumb("ModifySoundStore", "RAV: Zone edge");
            n = wd::FormatReport(buf, sizeof(buf), h);
        }
        CHECK(Has(buf, "Main thread inside RAV, outermost first (1 open)") && Has(buf, "Sound Edit  \"Footsteps\""));
        CHECK(Has(buf, "Audio thread: INSIDE AuditionMix"));
        CHECK(Has(buf, "Peak reader 0: peaks  \"C:\\sounds\\steps.wav\""));
        CHECK(Has(buf, "Last 2 crumbs") && Has(buf, "main   ModifySoundStore  \"RAV: Zone edge\""));
        wd::AudioExit();
        wd::WorkerIdle(slot);
        n = wd::FormatReport(buf, sizeof(buf), h);
        CHECK(Has(buf, "Audio thread: idle, last left AuditionMix") && Has(buf, "Peak reader 0: idle"));
        // Cut at its buffer, always terminated.
        char small[64];
        std::memset(small, 'z', sizeof(small));
        const size_t k = wd::FormatReport(small, sizeof(small), h);
        CHECK(k <= sizeof(small) - 1 && small[k] == '\0' && std::strlen(small) == k);
        CHECK(wd::FormatReport(nullptr, 0, h) == 0);
    }

    // Freeze fix #1: a long path's crumb keeps its end (the file name), without a copy.
    {
        const std::string shortp = "C:\\a\\walk.fbx";
        CHECK(wd::PathTail(shortp) == shortp.c_str());
        const std::string longp = std::string(200, 'x') + "\\character_walk_cycle_final.fbx";
        const char* t = wd::PathTail(longp);
        CHECK(std::strlen(t) == static_cast<size_t>(wd::kArgLen) - 1 && t + std::strlen(t) == longp.c_str() + longp.size());
        CHECK(std::strstr(t, "character_walk_cycle_final.fbx") != nullptr);
        CHECK(*wd::PathTail(std::string()) == '\0');
    }

    if (g_fails == 0) std::printf("watchdog: all checks passed\n");
    return g_fails == 0 ? 0 : 1;
}
