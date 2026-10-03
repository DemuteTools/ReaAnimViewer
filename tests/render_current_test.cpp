// SPDX-License-Identifier: MIT
//
// Host test of Render Current's source switch (src/render_current.h, spec 11-fb-16): each
// source bit is cleared, the other bits are kept, 0 stays 0. No REAPER, no Windows.
//   c++ -std=c++17 -I src tests/render_current_test.cpp -o t && ./t

#include "render_current.h"

#include <cstdio>

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

}  // namespace

int main()
{
    // 0 (master mix, no option) stays 0.
    CHECK(RenderSettingsAsMasterMix(0) == 0);

    // Each source bit alone is cleared.
    const int sources[] = {1, 2, 3, 8, 32, 64, 128, 8192, 16384};
    for (int s : sources) CHECK(RenderSettingsAsMasterMix(s) == 0);
    CHECK(RenderSettingsAsMasterMix(kRenderSourceMask) == 0);

    // The other bits are kept: multichannel (4), mono (16), 2nd pass (2048), embed (512 / 1024),
    // and anything above.
    const int others[] = {4, 16, 256, 512, 1024, 2048, 4096, 32768, 65536, 1 << 20};
    for (int o : others) {
        CHECK(RenderSettingsAsMasterMix(o) == o);
        CHECK(RenderSettingsAsMasterMix(o | 8) == o);           // region matrix + an option
        CHECK(RenderSettingsAsMasterMix(o | 128 | 2) == o);     // selected tracks + stems
    }
    CHECK((kRenderSourceMask & 4) == 0 && (kRenderSourceMask & 16) == 0);

    // Restore: source bits from saved, every other bit from now.
    CHECK(RenderSettingsRestored(0, 8) == 8);                    // region matrix comes back
    CHECK(RenderSettingsRestored(2048 | 16, 8) == (8 | 2048 | 16));  // options set in the dialog stay
    CHECK(RenderSettingsRestored(4, 8 | 2048) == (8 | 4));       // option cleared in the dialog stays cleared
    CHECK(RenderSettingsRestored(32 | 4, 128 | 2) == (128 | 2 | 4));  // a source picked in the dialog is replaced
    CHECK(RenderSettingsRestored(0, 0) == 0);

    // Tail flag: bit 1 (custom bounds) cleared, the others kept.
    CHECK(RenderTailFlagNoCustomTail(1) == 0);
    CHECK(RenderTailFlagNoCustomTail(0) == 0);
    CHECK(RenderTailFlagNoCustomTail(1 | 2 | 4) == (2 | 4));

    if (g_fails == 0) std::printf("render_current: all passed\n");
    return g_fails == 0 ? 0 : 1;
}
