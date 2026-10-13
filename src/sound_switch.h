// SPDX-License-Identifier: MIT
//
// Switch resolution (13-4): what an event plays through its library's tree.
// Epic 13 (D20). Created by 13-2 with declared interfaces and neutral stub bodies; story 13-4
// owns this file: it fills the stubs and may ADD declarations. Shared types: sound_model.h.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "sound_model.h"

namespace rav {

// The Switch values set at each level (the event's win over the rule's, over the item's).
struct ValueLayers {
    std::vector<ValueSet> item;
    std::vector<ValueSet> rule;
    std::vector<ValueSet> event;
    std::string           role;  // 13-23 (D1): the event's side-free role key ("" = none: the whole variation)
};

// What resolution asks its caller (so it calls no other module): the child index a Random node
// plays (-1 = none), the variation a Sound plays (-1 = none), whether a Sound's media is missing.
struct ResolveHooks {
    // Random: an index into ChildrenOf(lib, node.uid) (line order); null hook = 0; -1 or out of
    // range = that branch plays nothing (not blocked).
    std::function<int(const SoundNode&)>  random_child;
    std::function<int(const SoundNode&)>  variation;
    std::function<bool(const SoundNode&)> is_missing;
};

struct ResolvedSound {
    std::string node;             // the Sound node
    double      offset_ms = 0.0;  // Blend offsets summed down the path
    double      gain_db = 0.0;    // volumes summed down the path
    int         variation = -1;
    bool        missing = false;
    std::string part_value;       // 13-23 (D3): the part it plays (value uid, "" = the whole variation)
};

struct Resolution {
    std::vector<ResolvedSound> sounds;
    std::vector<std::string>   path;  // node uids walked, depth-first
    std::string                path_text;  // e.g. "Material Wood → Concrete (default) › Blend ×2"
    bool                       blocked = false;
    std::string                reason;
    // 13-21: no sound at all because a branch ended on a library gap (a Switch value without
    // sound, an empty Random / Blend): e.g. "Steps › Material Concrete (default) has no sound".
    // Empty when a sound resolved or nothing played for another reason.
    std::string                gap;
    std::string                gap_node;  // the node of that gap
};

// Where a value came from (EffectiveValue's `from`).
enum class ValueFrom { Default, Item, Rule, Event };

Resolution ResolveEvent(const SoundStore& store, const SoundLibrary& lib, const ValueLayers& layers,
                        const ResolveHooks& hooks);
// The value a Switch node plays for these layers ("" = none); `from` (optional) says which layer.
// Layers: item < rule < event; a ValueSet with an empty value_uid is unset. When no layer sets it,
// returns sw.default_value (may be "") with `from` = Default. What a Switch then plays (and what a
// UI shows as its effective value) is DefaultValue(store, sw), not this "".
std::string EffectiveValue(const SoundNode& sw, const ValueLayers& layers, ValueFrom* from);
// A Switch's default value (13-18 D1): the node's own default_value (when still a value of the
// group), else the group's default_value (same), else the group's first value, else (group not
// found) the node's own, else the first case's value; "" when none.
std::string DefaultValue(const SoundStore& store, const SoundNode& sw);
// 13-18: whether `value_uid` is one of the group's values ("" = no).
bool GroupHasValue(const SwitchGroup& g, const std::string& value_uid);
// 13-18: what a Switch with no default of its own plays: the group's default_value (when still a
// value), else its first value; "" when it has none.
std::string GroupDefaultValue(const SwitchGroup& g);

// 13-21: a gap that warns, not blocks: the node and why ("Mat: default Concrete has no sound").
struct LibraryGap {
    std::string node;
    std::string reason;
};
struct LibraryCheck {
    bool                     blocked = false;
    std::string              reason;   // the first blocking reason
    std::vector<std::string> nodes;    // the nodes that block it
    std::vector<LibraryGap>  warnings; // 13-21: every gap, in tree order (the library stays usable)
};
// Blocked (W1): no root, a cycle, a Switch whose group is missing or has no value (the 2-argument
// form only). Warnings (13-21 W2, not blocking): a Switch whose default value has no sound, an
// empty Random / Blend; an event that lands on one places nothing. The 1-argument form checks
// against an empty store (default "" = the first case).
LibraryCheck CheckLibrary(const SoundStore& store, const SoundLibrary& lib);
LibraryCheck CheckLibrary(const SoundLibrary& lib);
// Changes a container's type, keeping its children (to a Switch: by name, the rest Unassigned).
bool ConvertNodeType(SoundLibrary& lib, const std::string& uid, SoundNodeType type, const std::string& group_uid,
                     const SoundStore& store);
// A name as Switch matching compares it (ASCII case, '_', '-' and spaces ignored; accented
// letters keep their case, so "Été" and "été" do not match).
std::string SwitchNameKey(const std::string& s);

}  // namespace rav
