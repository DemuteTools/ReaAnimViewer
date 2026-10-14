// SPDX-License-Identifier: MIT
//
// Host test of 13-51's pure rules: src/file_change_check.h (when the peak cache reads a file
// again) and src/evict_generation.h (a parse started before an eviction is not stored).

#include "evict_generation.h"
#include "file_change_check.h"

#include <cstdio>

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

int main()
{
    // ---- FileRecheckDue: at most once per interval ----
    CHECK(!FileRecheckDue(10.0, 10.0));
    CHECK(!FileRecheckDue(10.0, 10.99));
    CHECK(FileRecheckDue(10.0, 11.0));
    CHECK(FileRecheckDue(10.0, 9.0));  // clock went back
    CHECK(FileRecheckDue(0.0, 5.0, 2.0) && !FileRecheckDue(4.0, 5.0, 2.0));

    // ---- FileReadStale ----
    const FileVersion a{100, 2000};
    CHECK(!FileReadStale(a, a));
    CHECK(FileReadStale(a, FileVersion{101, 2000}));      // re-rendered, same size
    CHECK(FileReadStale(a, FileVersion{100, 2048}));      // same date, other size
    CHECK(FileReadStale(a, FileVersion{}));               // deleted
    CHECK(FileReadStale(FileVersion{}, a));               // failed (missing), now there
    CHECK(!FileReadStale(FileVersion{}, FileVersion{}));  // still missing: no retry

    // ---- EvictGenerations ----
    EvictGenerations g;
    CHECK(g.Of("a.fbx") == 0);
    const uint64_t start = g.Of("a.fbx");  // a parse starts
    CHECK(ParseStillCurrent(start, g.Of("a.fbx")));
    g.Bump("b.fbx");  // another file evicted: a.fbx's parse is still stored
    CHECK(ParseStillCurrent(start, g.Of("a.fbx")));
    g.Bump("a.fbx");  // "Locate textures..." during the parse
    CHECK(!ParseStillCurrent(start, g.Of("a.fbx")));
    const uint64_t again = g.Of("a.fbx");  // the next parse starts after it: stored
    CHECK(again == 1 && ParseStillCurrent(again, g.Of("a.fbx")));
    g.Bump("a.fbx");
    CHECK(g.Of("a.fbx") == 2 && g.Of("b.fbx") == 1);

    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("read_robustness: all checks passed\n");
    return 0;
}
