// SPDX-License-Identifier: MIT
//
// THROWAWAY SPIKE (Spike 0 / Story 0-1) — NOT production code, do not merge to main.
//
// assimp boundary for the spike. This is the ONLY spike TU that includes
// <assimp/...> (mirrors the production boundary rule, cheaply). Loads a skinned
// glTF/GLB into spike::Model. FBX is a non-goal for the spike.

#pragma once

#include <string>

#include "spike_scene.h"

namespace spike {

// Returns true on success. On failure, fills outError and leaves model untouched.
// Throwing is contained here (assimp throws); callers get bool + message.
bool LoadModel(const std::string& path, Model& outModel, std::string& outError);

}  // namespace spike
