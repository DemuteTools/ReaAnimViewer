// SPDX-License-Identifier: MIT
//
// Placeholder host test of src/sound_entries.h (Epic 13, created by 13-2): it links the module and calls
// one stub. Story 13-3 replaces it with the real tests.

#include "sound_entries.h"

#include <cstdio>

using namespace rav;

int main()
{
    const SoundFrame f;
    (void)FramePeakDb(PeakData{}, f);  // STUB call
    std::printf("sound_entries_test: placeholder passed (13-2 stub)\n");
    return 0;
}
