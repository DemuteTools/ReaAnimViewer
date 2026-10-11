// SPDX-License-Identifier: MIT
//
// Placeholder host test of src/sound_switch.h (Epic 13, created by 13-2): it links the module and calls
// one stub. Story 13-4 replaces it with the real tests.

#include "sound_switch.h"

#include <cstdio>

using namespace rav;

int main()
{
    (void)SwitchNameKey("Wet_Concrete");  // STUB call
    std::printf("sound_switch_test: placeholder passed (13-2 stub)\n");
    return 0;
}
