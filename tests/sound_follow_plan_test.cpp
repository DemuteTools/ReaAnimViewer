// SPDX-License-Identifier: MIT
//
// Placeholder host test of src/sound_follow_plan.h (Epic 13, created by 13-2): it links the module and calls
// one stub. Story 13-9 replaces it with the real tests.

#include "sound_follow_plan.h"

#include <cstdio>

using namespace rav;

int main()
{
    (void)PlanFollow(ItemSounds{}, {}, {});  // STUB call
    std::printf("sound_follow_plan_test: placeholder passed (13-2 stub)\n");
    return 0;
}
