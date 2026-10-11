// SPDX-License-Identifier: MIT
//
// See sound_entries.h. Stub bodies (13-2): each returns a neutral value and is replaced by 13-3.

#include "sound_entries.h"

#include <limits>

namespace rav {

std::vector<Slice> SlicesFromRppText(const std::string&)
{
    return {};  // STUB (13-3)
}

std::vector<Slice> SlicesFromTakeMarkers(const std::vector<TakeMarkerPos>&, double)
{
    return {};  // STUB (13-3)
}

std::vector<SoundFrame> SplitOnSilence(const PeakData&, const SilenceParams&, double)
{
    return {};  // STUB (13-3)
}

std::vector<SoundFrame> DetectFrames(const EntryFacts&, DetectMode, const SilenceParams&, const PeakData*)
{
    return {};  // STUB (13-3)
}

void ClampFrames(std::vector<SoundFrame>&, double)
{
    // STUB (13-3)
}

void ResetToDetected(SoundData&, const std::vector<SoundFrame>&)
{
    // STUB (13-3)
}

double FramePeakDb(const PeakData&, const SoundFrame&)
{
    return -std::numeric_limits<double>::infinity();  // STUB (13-3)
}

void NormalizeFrames(std::vector<SoundFrame>&, const PeakData&, double, bool, const std::vector<int>&)
{
    // STUB (13-3)
}

}  // namespace rav
