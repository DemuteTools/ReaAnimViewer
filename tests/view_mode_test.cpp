// SPDX-License-Identifier: MIT
//
// Host test of src/view_mode.h (release 0.4.0): the V cycle and a Library request, with and without the
// sound library (built twice: view_mode, and view_mode_off with RAV_WITH_SOUNDS=0).

#include "view_mode.h"

#include <cstdio>
#include <initializer_list>

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

int main()
{
    // With the sounds: RAV -> Tagging -> Video -> Library -> RAV; Library is asked for and given.
    CHECK(NextViewMode(ViewMode::Rav, true) == ViewMode::Tagging);
    CHECK(NextViewMode(ViewMode::Tagging, true) == ViewMode::Video);
    CHECK(NextViewMode(ViewMode::Video, true) == ViewMode::Library);
    CHECK(NextViewMode(ViewMode::Library, true) == ViewMode::Rav);
    CHECK(ViewModeBuilt(ViewMode::Library, true) == ViewMode::Library);
    // Without: RAV -> Tagging -> Video -> RAV, Library never reached; a Library request gives RAV view.
    CHECK(NextViewMode(ViewMode::Rav, false) == ViewMode::Tagging);
    CHECK(NextViewMode(ViewMode::Tagging, false) == ViewMode::Video);
    CHECK(NextViewMode(ViewMode::Video, false) == ViewMode::Rav);
    CHECK(ViewModeBuilt(ViewMode::Library, false) == ViewMode::Rav);
    for (ViewMode m : {ViewMode::Rav, ViewMode::Tagging, ViewMode::Video}) {
        CHECK(ViewModeBuilt(m, false) == m);
        CHECK(ViewModeBuilt(m, true) == m);
    }
    {
        ViewMode m = ViewMode::Rav;
        for (int i = 0; i < 9; ++i) {
            m = NextViewMode(m, false);
            CHECK(m != ViewMode::Library);
        }
    }
    // This build's default.
    CHECK(NextViewMode(ViewMode::Video) == (kWithSounds ? ViewMode::Library : ViewMode::Rav));
    CHECK(ViewModeBuilt(ViewMode::Library) == (kWithSounds ? ViewMode::Library : ViewMode::Rav));
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("view_mode: all passed\n");
    return 0;
}
