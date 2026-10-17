// SPDX-License-Identifier: MIT
//
// Each auto marker type's sound on the item (13-8, reworked by 13-15a, pure, no REAPER): the
// Tagging view's Auto detection section edits, per marker type, the library and the rule-level
// Switch values of the item's auto blocks (and its pool's). The rule is the item's layer of the
// assignment (sound_assign.h); the defaults of a marker type now live at Project / Character level
// (body part + type), set in the assignment window, never copied into the rules by Detect.
//
// The key is the block's identity as auto_detect.h defines it: its kind and side, the kinds that
// give the same marker (AutoMarkerName) sharing one key, so switching a hand pivot between after
// a Grab and any, or a slide / pivot part between Each part and Longest part, keeps the sound.
// Combined and separate are different markers (FS L vs Heel L / Toe L): different keys.

#pragma once

#include <string>
#include <vector>

#include "auto_detect.h"
#include "sound_assign.h"
#include "sound_model.h"

namespace rav {

// "<word>:<side>": "step:L", "heel:R", "hand_pivot:L" (both hand pivot kinds), "slide_heel:R"
// (Each part and Longest part), "jump:L", "land:R", "lift_heel:L", "jump_toe:R" (story 10-9a-3)...;
// "" for a side other than L / R. A sideless kind's key (story 10-9a; no kind since 10-9a-3) is
// "<word>:B" for L, R or B; B with any other kind gives "".
std::string AutoSoundKey(AutoKind k, char side);
// The key of a known auto block, "" for a rule or an unknown auto kind.
std::string AutoSoundKeyOf(const Block& b);

// The rid of each block of these keys (block order, each once; a block with no rid is skipped).
std::vector<std::string> AutoKeyRids(const std::vector<Block>& blocks, const std::vector<std::string>& keys);
// True when a block of these keys has no rid yet.
bool AutoKeysNeedRids(const std::vector<Block>& blocks, const std::vector<std::string>& keys);

// The rule edits of a row gesture: each listed rule gets the library state (Inherit clears the
// rule's own: library "" and not None) / value (its RuleSounds added when needed; none added to
// clear). True when the record changed.
bool SetRulesLibrary(ItemSounds& item, const std::vector<std::string>& rids, LibState state,
                     const std::string& library_uid);
bool SetRulesValue(ItemSounds& item, const std::vector<std::string>& rids, const std::string& group_uid,
                   const std::string& value_uid);

// What a key's row shows: the item's rule when it has a block of that key (what Populate uses, the
// same as its Sound card), else what a new block of that key would get from the Character /
// Project.
struct AutoSoundShown {
    LibState              own_state = LibState::Inherit;  // the rule's own library state
    std::string           own_library;                    // ... and its library
    std::vector<ValueSet> values;                         // the rule's own Switch values
    EffectiveAssign       effective;                      // what Populate places (library, layers, sources)
    bool                  on_item = false;                // read from the item's rule
};
AutoSoundShown AutoSoundFor(const SoundStore& store, const ItemSounds& item, const SoundCharacter* ch,
                            const std::vector<Block>& blocks, const std::string& key);

}  // namespace rav
