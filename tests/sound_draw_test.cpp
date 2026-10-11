// SPDX-License-Identifier: MIT
//
// Placeholder host test of src/sound_draw.h (Epic 13, created by 13-2): it links the module and calls
// one stub. Story 13-3 replaces it with the real tests.

#include "sound_draw.h"

#include <cstdio>

using namespace rav;

int main()
{
    (void)DrawPick({}, 2, {}, 1, 2);  // STUB call
    std::printf("sound_draw_test: placeholder passed (13-2 stub)\n");
    return 0;
}
