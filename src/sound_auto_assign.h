// SPDX-License-Identifier: MIT
//
// Each auto marker type's sound (13-8, Antho 2026-10-12, pure, no REAPER): the Tagging view's Auto
// detection section picks, per marker type, the library and the rule-level Switch values its
// markers get, before or after Detect. Stored in the project's sound store
// (SoundStore::auto_sounds, sound_record.h's `auto` / `avalue` lines).
//
// One source of truth per item: the rule. Populate reads only the rules' assignments (RuleSounds);
// the store's entry is the type's default, which Detect gives to the rule of each auto block of
// that key that has no library yet. Both editors write both, in one undo point:
// - the section's row (one key, or every key of a type's folded row): the store's entries, and the
//   rule of every block of those keys on the item (and its pool), and, on the project's other
//   items, the rule of each block of those keys that has no library yet (never one set);
// - an auto block's Sound card (the rule): the rule, and its key's store entry.
// The row shows the item's rule when the item has a block of that key (what Populate uses, the
// same as its Sound card), else the store's entry (what Detect will give).
//
// The key is the block's identity as auto_detect.h defines it: its kind and side, the kinds that
// give the same marker (AutoMarkerName) sharing one key, so switching a hand pivot between after
// a Grab and any, or a slide / pivot part between Each part and Longest part, keeps the sound.
// Combined and separate are different markers (FS L vs Heel L / Toe L): different keys.

#pragma once

#include <string>
#include <vector>

#include "auto_detect.h"
#include "sound_model.h"

namespace rav {

// "<word>:<side>": "step:L", "heel:R", "hand_pivot:L" (both hand pivot kinds), "slide_heel:R"
// (Each part and Longest part)...; "" for a side other than L / R.
std::string AutoSoundKey(AutoKind k, char side);
// The key of a known auto block, "" for a rule or an unknown auto kind.
std::string AutoSoundKeyOf(const Block& b);

// The store's entry of that key (nullptr when none).
const AutoSoundAssign* FindAutoSound(const SoundStore& store, const std::string& key);

// The store edits of a row gesture, on every key listed (an entry added when needed; an entry left
// with no library and no value is removed). True when the store changed.
bool SetAutoSoundLibrary(SoundStore& store, const std::vector<std::string>& keys, const std::string& library_uid);
bool SetAutoSoundValue(SoundStore& store, const std::vector<std::string>& keys, const std::string& group_uid,
                       const std::string& value_uid);

// The rid of each block of these keys (block order, each once; a block with no rid is skipped).
std::vector<std::string> AutoKeyRids(const std::vector<Block>& blocks, const std::vector<std::string>& keys);
// True when a block of these keys has no rid yet.
bool AutoKeysNeedRids(const std::vector<Block>& blocks, const std::vector<std::string>& keys);

// The rule edits of a row gesture: each listed rule gets the library / value (its RuleSounds added
// when needed; none added to clear). True when the record changed.
bool SetRulesLibrary(ItemSounds& item, const std::vector<std::string>& rids, const std::string& library_uid);
bool SetRulesValue(ItemSounds& item, const std::vector<std::string>& rids, const std::string& group_uid,
                   const std::string& value_uid);

// What a key's row shows (see the header).
struct AutoSoundShown {
    std::string           library;   // "" = none
    std::vector<ValueSet> values;
    bool                  on_item = false;  // read from the item's rule (else the store's entry)
};
AutoSoundShown AutoSoundFor(const SoundStore& store, const ItemSounds& item, const std::vector<Block>& blocks,
                            const std::string& key);

// Detect's step, and a row edit's on the project's other items (Antho 2026-10-12): the rids of the
// auto blocks of `keys` ({} = every key) whose rule has no library yet, and whose key's store entry
// has a library still in the store (each once, block order). A rule with a library of its own is
// never in it.
std::vector<std::string> AutoFillRids(const ItemSounds& item, const std::vector<Block>& blocks, const SoundStore& store,
                                      const std::vector<std::string>& keys = {});
// Gives each of those rules its key's store entry (its library and values). True when changed.
bool FillAutoSounds(ItemSounds& item, const std::vector<Block>& blocks, const SoundStore& store,
                    const std::vector<std::string>& keys = {});
// True when such a block has no rid yet (the caller gives rids first).
bool AutoFillNeedsRids(const std::vector<Block>& blocks, const SoundStore& store,
                       const std::vector<std::string>& keys = {});
// True when the store has an entry with a library still in the store (Detect then has something
// to give).
bool HasAutoSounds(const SoundStore& store);

}  // namespace rav
