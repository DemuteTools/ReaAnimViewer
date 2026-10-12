// SPDX-License-Identifier: MIT
//
// See sound_auto_assign.h.

#include "sound_auto_assign.h"

#include <algorithm>

#include "sound_populate_plan.h"  // SetValueIn
#include "sound_record.h"         // FindRuleSounds

namespace rav {
namespace {

// The kind whose word names the key: the kinds of one marker share it.
AutoKind KeyKind(AutoKind k)
{
    switch (k) {
    case AutoKind::HandPivotAny: return AutoKind::HandPivot;
    case AutoKind::SlideHeelLongest: return AutoKind::SlideHeel;
    case AutoKind::SlideToeLongest: return AutoKind::SlideToe;
    case AutoKind::PivotHeelLongest: return AutoKind::PivotHeel;
    case AutoKind::PivotToeLongest: return AutoKind::PivotToe;
    default: break;
    }
    return k;
}

bool Listed(const std::vector<std::string>& keys, const std::string& k)
{
    return !k.empty() && std::find(keys.begin(), keys.end(), k) != keys.end();
}

RuleSounds& RuleOf(ItemSounds& item, const std::string& rid)
{
    if (RuleSounds* r = FindRuleSounds(item, rid)) return *r;
    RuleSounds fresh;
    fresh.rid = rid;
    item.rules.push_back(fresh);
    return item.rules.back();
}

}  // namespace

std::string AutoSoundKey(AutoKind k, char side)
{
    if (side != 'L' && side != 'R') return std::string();
    return std::string(AutoKindWord(KeyKind(k))) + ":" + std::string(1, side);
}

std::string AutoSoundKeyOf(const Block& b)
{
    AutoKind k;
    char     side = 0;
    if (!AutoBlockKind(b, &k, &side)) return std::string();
    return AutoSoundKey(k, side);
}

std::vector<std::string> AutoKeyRids(const std::vector<Block>& blocks, const std::vector<std::string>& keys)
{
    std::vector<std::string> out;
    for (const Block& b : blocks) {
        if (b.rid.empty() || !Listed(keys, AutoSoundKeyOf(b))) continue;
        if (std::find(out.begin(), out.end(), b.rid) == out.end()) out.push_back(b.rid);
    }
    return out;
}

bool AutoKeysNeedRids(const std::vector<Block>& blocks, const std::vector<std::string>& keys)
{
    for (const Block& b : blocks)
        if (b.rid.empty() && Listed(keys, AutoSoundKeyOf(b))) return true;
    return false;
}

bool SetRulesLibrary(ItemSounds& item, const std::vector<std::string>& rids, LibState state,
                     const std::string& library_uid)
{
    const bool        none = state == LibState::None;
    const std::string lib = state == LibState::Library ? library_uid : std::string();
    bool              changed = false;
    for (const std::string& rid : rids) {
        if (rid.empty()) continue;
        if (state == LibState::Inherit && !FindRuleSounds(item, rid)) continue;
        RuleSounds& r = RuleOf(item, rid);
        if (r.library == lib && r.none == none) continue;
        r.library = lib;
        r.none = none;
        changed = true;
    }
    return changed;
}

bool SetRulesValue(ItemSounds& item, const std::vector<std::string>& rids, const std::string& group_uid,
                   const std::string& value_uid)
{
    if (group_uid.empty()) return false;
    bool changed = false;
    for (const std::string& rid : rids) {
        if (rid.empty()) continue;
        if (value_uid.empty() && !FindRuleSounds(item, rid)) continue;
        if (SetValueIn(RuleOf(item, rid).values, group_uid, value_uid)) changed = true;
    }
    return changed;
}

AutoSoundShown AutoSoundFor(const SoundStore& store, const ItemSounds& item, const SoundCharacter* ch,
                            const std::vector<Block>& blocks, const std::string& key)
{
    AutoSoundShown out;
    if (key.empty()) return out;
    for (const Block& b : blocks) {
        if (AutoSoundKeyOf(b) != key) continue;
        out.on_item = true;
        if (const RuleSounds* r = b.rid.empty() ? nullptr : FindRuleSounds(item, b.rid)) {
            out.own_state = r->none ? LibState::None : r->library.empty() ? LibState::Inherit : LibState::Library;
            out.own_library = r->library;
            out.values = r->values;
        }
        out.effective = ResolveBlock(store, item, ch, b);
        return out;  // the first block of the key (a key has one block per item)
    }
    // No block of the key yet: what a new one would get (its part and type from the key's kind).
    AutoKind     k;
    const size_t colon = key.find(':');
    if (colon != std::string::npos && AutoKindFromWord(key.substr(0, colon), &k))
        out.effective = ResolveBlock(store, item, ch, MakeAutoBlock(k, 'L', kAutoDefaultSens, 0.0));
    return out;
}

}  // namespace rav
