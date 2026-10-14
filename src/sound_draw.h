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
// 13-23 (A5): the frames that can play part `part_value` (a value uid; "" = the whole variation): on,
// with at least one span of it (PlacedSpans). A variation whose part is empty or excluded is skipped.
// 13-62h: a Sound edited as zones (PlacedFromZones): on, with at least one zone of that part ("": any part zone).
std::vector<int> ActiveVariationsFor(const SoundData& data, const std::string& part_value);
// An event's salt: rule id + clip time in 100 ms bins (llround(t * 10)). It only seeds a first
// draw; an event keeps its pick across re-detection through its stored draw (+-30 ms match).
uint64_t EventSalt(const std::string& rid, double t);
// The children a Blend node plays together, in line order.
std::vector<const SoundNode*> BlendParts(const SoundLibrary& lib, const SoundNode& node);
// Keeps prev->pick when it is still in `active` (hand-picked or stored), else draws (DrawPick).
int KeepOrDraw(const SoundDraw* prev, const std::vector<int>& active, int avoid_n, const std::vector<int>& history,
               uint64_t seed, uint64_t salt);
// A new seed: every draw not picked by hand is erased (drawn again on next use).
void Reshuffle(ItemSounds& item, uint64_t new_seed);
// The nodes whose parent is `node`, in line order.
std::vector<const SoundNode*> ChildNodes(const SoundLibrary& lib, const SoundNode& node);

}  // namespace rav
