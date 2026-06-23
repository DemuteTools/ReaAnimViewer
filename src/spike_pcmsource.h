// SPDX-License-Identifier: MIT
//
// THROWAWAY SPIKE (Spike 0 / Story 0-1) — NOT production code, do not merge to main.
//
// Transport-integration experiment: a minimal PCM_source so dropping a .fbx/.glb on
// a Reaper track creates a timeline item of the animation's length, plus a helper
// that maps the current playhead to an item-relative animation time. Proves Reaper
// can both HOLD the animation as an item and DRIVE which frame the viewer shows.

#pragma once

namespace spike {

// Register/unregister our PCM_source factory (pass rec->Register).
void RegisterPcmSrc(int (*reg)(const char*, void*));
void UnregisterPcmSrc(int (*reg)(const char*, void*));

// If one of our animation items spans the current play (or edit-cursor) position,
// returns true and sets outAnimTimeSec to the item-relative time (clamped >= 0).
bool CurrentAnimTime(double& outAnimTimeSec);

}  // namespace spike
