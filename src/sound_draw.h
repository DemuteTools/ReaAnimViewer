// SPDX-License-Identifier: MIT
//
// Sound draw (13-3): which variation an event plays (stable, seeded, avoid repeating).
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-3
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sound_model.h"

namespace rav {

// The pick among `active` (variation indices), avoiding the last `avoid_n` of `history`;
// deterministic for (seed, salt). -1 when `active` is empty.
int DrawPick(const std::vector<int>& active, int avoid_n, const std::vector<int>& history, uint64_t seed,
             uint64_t salt);
// The indices of the Sound's frames that are on.
std::vector<int> ActiveVariations(const SoundData& data);
// An event's salt (rule id + clip time, stable under the +-30 ms match).
uint64_t EventSalt(const std::string& rid, double t);
// The children a Blend node plays together, in line order.
std::vector<const SoundNode*> BlendParts(const SoundLibrary& lib, const SoundNode& node);

}  // namespace rav
