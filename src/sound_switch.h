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
};

struct Resolution {
    std::vector<ResolvedSound> sounds;
    std::vector<std::string>   path;  // node uids walked, depth-first
    std::string                path_text;  // e.g. "Material Wood → Concrete (default) › Blend ×2"
    bool                       blocked = false;
    std::string                reason;
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
// A Switch's default value: default_value, else the group's first value, else (group not found)
// the first case's value; "" when none.
std::string DefaultValue(const SoundStore& store, const SoundNode& sw);

struct LibraryCheck {
    bool                     blocked = false;
    std::string              reason;
    std::vector<std::string> nodes;  // the nodes that block it
};
// Blocked: a Switch whose default value has no sound, or an empty Random / Blend.
// Also blocked: no root, a cycle, a Switch whose group is missing or has no value (the 2-argument
// form only). The 1-argument form checks against an empty store (default "" = the first case).
LibraryCheck CheckLibrary(const SoundStore& store, const SoundLibrary& lib);
LibraryCheck CheckLibrary(const SoundLibrary& lib);
// Changes a container's type, keeping its children (to a Switch: by name, the rest Unassigned).
bool ConvertNodeType(SoundLibrary& lib, const std::string& uid, SoundNodeType type, const std::string& group_uid,
                     const SoundStore& store);
// A name as Switch matching compares it (ASCII case, '_', '-' and spaces ignored; accented
// letters keep their case, so "Été" and "été" do not match).
std::string SwitchNameKey(const std::string& s);

}  // namespace rav
