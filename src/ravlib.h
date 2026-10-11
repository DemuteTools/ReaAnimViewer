// SPDX-License-Identifier: MIT
//
// User Library files (13-11, pure): a .ravlib = the RAVSOUNDS grammar (sound_record.h) of one library
// and its Switch Groups under the magic "RAVLIB 1".
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-11
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#include <string>

#include "sound_model.h"

namespace rav {

constexpr const char kRavlibMagic[] = "RAVLIB";

// Reads a .ravlib into `out` (one library, its groups). False (and `out` untouched) when it is not one.
bool ParseRavlib(const std::string& text, SoundStore* out);
// Writes one library and the groups its Switches use.
std::string SerializeRavlib(const SoundStore& one);

}  // namespace rav
