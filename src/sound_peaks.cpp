// SPDX-License-Identifier: MIT
//
// See sound_peaks.h. Stub bodies (13-2): each returns a neutral value and is replaced by 13-7b.

#include "sound_peaks.h"

#ifdef _WIN32


namespace rav {

void InitSoundPeaks(void* (*)(const char*))
{
    // STUB (13-7b)
}

bool ReadPeaks(const std::string&, double, double, double, PeakData* out)
{
    if (out) *out = PeakData{};
    return false;  // STUB (13-7b)
}

}  // namespace rav

#endif  // _WIN32
