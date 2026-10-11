// SPDX-License-Identifier: MIT
//
// Placeholder host test of src/ravlib.h (Epic 13, created by 13-2): it links the module and calls
// one stub. Story 13-11 replaces it with the real tests.

#include "ravlib.h"

#include <cstdio>

using namespace rav;

int main()
{
    SoundStore s;
    (void)ParseRavlib("", &s);  // STUB call
    std::printf("ravlib_test: placeholder passed (13-2 stub)\n");
    return 0;
}
