// SPDX-License-Identifier: MIT
//
// Bone tracks for the event engine (Epic 10, spike 10-0): the Windows glue between a
// loaded animation and the pure rule engine (bone_events.h). ComputePose is sampled over
// the whole clip [0, duration] at a fixed rate; each bone's model-space position
// (modelRoot * its global matrix, column 3) is turned into metres (metersPerUnit), with
// vertical = model Y.
//
// Offline, any thread (no REAPER, no GL). No-throw beyond std::bad_alloc.

#pragma once

#ifdef _WIN32

#include <vector>

#include "asset_loader.h"
#include "bone_events.h"

namespace rav {

// One track per entry of bone_indices (indices into asset.skeleton.bones), in that order,
// from the first clip. Empty when the asset has no animation, a bone index is out of
// range or rate_hz <= 0.
std::vector<BoneTrack> SampleBoneTracks(const CpuAsset& asset, const std::vector<int>& bone_indices,
                                        double rate_hz = 240.0);

}  // namespace rav

#endif  // _WIN32
