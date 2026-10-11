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
    std::vector<std::string>   path;  // node uids walked
    bool                       blocked = false;
    std::string                reason;
};

// Where a value came from (EffectiveValue's `from`).
enum class ValueFrom { Default, Item, Rule, Event };

Resolution ResolveEvent(const SoundStore& store, const SoundLibrary& lib, const ValueLayers& layers,
                        const ResolveHooks& hooks);
// The value a Switch node plays for these layers ("" = none); `from` (optional) says which layer.
std::string EffectiveValue(const SoundNode& sw, const ValueLayers& layers, ValueFrom* from);

struct LibraryCheck {
    bool                     blocked = false;
    std::string              reason;
    std::vector<std::string> nodes;  // the nodes that block it
};
// Blocked: a Switch whose default value has no sound, or an empty Random / Blend.
LibraryCheck CheckLibrary(const SoundLibrary& lib);
// Changes a container's type, keeping its children (to a Switch: by name, the rest Unassigned).
bool ConvertNodeType(SoundLibrary& lib, const std::string& uid, SoundNodeType type, const std::string& group_uid,
                     const SoundStore& store);
// A name as Switch matching compares it (case, '_', '-' and spaces ignored).
std::string SwitchNameKey(const std::string& s);

}  // namespace rav
