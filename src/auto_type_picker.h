// SPDX-License-Identifier: MIT
//
// Story 10-10e: what the Smart Detection type picker shows, apart from the drawing (tagging_view_ui.cpp):
// the common types drawn first (in bold), the search, and the user's presets (a named set of ticks,
// kept per user across projects in REAPER ExtState, applied as exactly its ticks).
//
// Pure C++17, no ImGui, no REAPER. Host-tested (tests/auto_type_picker_test.cpp).

#pragma once

#include <string>
#include <vector>

#include "auto_detect.h"

namespace rav {

// The common types (Step, Lift-off, Jump, Land, Grab, Release): first in their category, in bold.
bool AutoTypeCommon(AutoType t);
// The types of a category in the picker's order: the common ones first, then the others, each in
// enum order (a type appended to the enum joins its category's chips).
std::vector<AutoType> AutoPickerOrder(AutoCategory c);
// Whether a type matches the search (case-insensitive, in its label, marker word, category and
// hint); an empty or blank query matches every type.
bool AutoTypeMatches(AutoType t, const std::string& query);

// One preset: its name and its ticked types (enum order, each once).
struct AutoPreset {
    std::string           name;
    std::vector<AutoType> types;
};
constexpr int    kAutoPresetMax = 64;       // presets kept (the rest of a stored value is dropped)
constexpr size_t kAutoPresetNameMax = 48;   // bytes of a name

// A name as stored: control characters removed, spaces trimmed, at most kAutoPresetNameMax bytes
// (never cut inside a UTF-8 character). "" when nothing is left.
std::string CleanAutoPresetName(const std::string& name);
// The stored value: "name;word,word|name;word" (the types by their stable sound_word; '%', '|',
// ';', ',' and control bytes of a name escaped as %XX). One line, safe in an ini file.
std::string EncodeAutoPresets(const std::vector<AutoPreset>& presets);
// The presets of a stored value. Never fails: an unknown word, a bad escape, an empty name or a
// preset past kAutoPresetMax is skipped; a type listed twice counts once.
std::vector<AutoPreset> DecodeAutoPresets(const std::string& value);

// The preset of the current ticks.
AutoPreset AutoPresetFromTicks(const std::string& name, const AutoSettings& s);
// Whether the ticks are exactly the preset's (its chip shows a tick).
bool AutoPresetMatches(const AutoPreset& p, const AutoSettings& s);
// Applying a preset: exactly its types ticked, every other type unticked (values and modes kept).
// Returns whether a tick changed.
bool ApplyAutoPreset(const AutoPreset& p, AutoSettings& s);

}  // namespace rav
