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

AutoSoundAssign* FindEntry(SoundStore& store, const std::string& key)
{
    for (AutoSoundAssign& a : store.auto_sounds)
        if (a.key == key) return &a;
    return nullptr;
}

// The entry of `key`, added when absent.
AutoSoundAssign& Entry(SoundStore& store, const std::string& key)
{
    if (AutoSoundAssign* a = FindEntry(store, key)) return *a;
    AutoSoundAssign fresh;
    fresh.key = key;
    store.auto_sounds.push_back(fresh);
    return store.auto_sounds.back();
}

// An entry left with nothing (no library, no value, nothing kept) is removed.
void DropEmpty(SoundStore& store)
{
    store.auto_sounds.erase(std::remove_if(store.auto_sounds.begin(), store.auto_sounds.end(),
                                           [](const AutoSoundAssign& a) {
                                               return a.library.empty() && a.values.empty() && a.kept.empty() &&
                                                      a.kept_lines.empty();
                                           }),
                            store.auto_sounds.end());
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

const AutoSoundAssign* FindAutoSound(const SoundStore& store, const std::string& key)
{
    if (key.empty()) return nullptr;
    for (const AutoSoundAssign& a : store.auto_sounds)
        if (a.key == key) return &a;
    return nullptr;
}

bool SetAutoSoundLibrary(SoundStore& store, const std::vector<std::string>& keys, const std::string& library_uid)
{
    bool changed = false;
    for (const std::string& key : keys) {
        if (key.empty()) continue;
        AutoSoundAssign* a = FindEntry(store, key);
        if (!a) {
            if (library_uid.empty()) continue;
            a = &Entry(store, key);
        }
        if (a->library == library_uid) continue;
        a->library = library_uid;
        changed = true;
    }
    if (changed) DropEmpty(store);
    return changed;
}

bool SetAutoSoundValue(SoundStore& store, const std::vector<std::string>& keys, const std::string& group_uid,
                       const std::string& value_uid)
{
    if (group_uid.empty()) return false;
    bool changed = false;
    for (const std::string& key : keys) {
        if (key.empty()) continue;
        AutoSoundAssign* a = FindEntry(store, key);
        if (!a) {
            if (value_uid.empty()) continue;
            a = &Entry(store, key);
        }
        if (SetValueIn(a->values, group_uid, value_uid)) changed = true;
    }
    if (changed) DropEmpty(store);
    return changed;
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

bool SetRulesLibrary(ItemSounds& item, const std::vector<std::string>& rids, const std::string& library_uid)
{
    bool changed = false;
    for (const std::string& rid : rids) {
        if (rid.empty()) continue;
        if (library_uid.empty() && !FindRuleSounds(item, rid)) continue;
        RuleSounds& r = RuleOf(item, rid);
        if (r.library == library_uid) continue;
        r.library = library_uid;
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

AutoSoundShown AutoSoundFor(const SoundStore& store, const ItemSounds& item, const std::vector<Block>& blocks,
                            const std::string& key)
{
    AutoSoundShown out;
    if (key.empty()) return out;
    for (const Block& b : blocks) {
        if (AutoSoundKeyOf(b) != key) continue;
        out.on_item = true;
        const RuleSounds* r = b.rid.empty() ? nullptr : FindRuleSounds(item, b.rid);
        if (r) {
            out.library = r->library;
            out.values = r->values;
        }
        return out;  // the first block of the key (a key has one block per item)
    }
    if (const AutoSoundAssign* a = FindAutoSound(store, key)) {
        out.library = a->library;
        out.values = a->values;
    }
    return out;
}

namespace {

// The store entry Detect / a row gives to this block: one with a library still in the store.
const AutoSoundAssign* GivenEntry(const SoundStore& store, const Block& b, const std::vector<std::string>& keys)
{
    const std::string key = AutoSoundKeyOf(b);
    if (key.empty() || (!keys.empty() && !Listed(keys, key))) return nullptr;
    const AutoSoundAssign* a = FindAutoSound(store, key);
    if (!a || a->library.empty() || !FindLibrary(store, a->library)) return nullptr;
    return a;
}

}  // namespace

std::vector<std::string> AutoFillRids(const ItemSounds& item, const std::vector<Block>& blocks, const SoundStore& store,
                                      const std::vector<std::string>& keys)
{
    std::vector<std::string> out;
    for (const Block& b : blocks) {
        if (b.rid.empty() || !GivenEntry(store, b, keys)) continue;
        const RuleSounds* cur = FindRuleSounds(item, b.rid);
        if (cur && !cur->library.empty()) continue;  // the rule has its own: kept
        if (std::find(out.begin(), out.end(), b.rid) == out.end()) out.push_back(b.rid);
    }
    return out;
}

bool FillAutoSounds(ItemSounds& item, const std::vector<Block>& blocks, const SoundStore& store,
                    const std::vector<std::string>& keys)
{
    bool changed = false;
    for (const std::string& rid : AutoFillRids(item, blocks, store, keys)) {
        const AutoSoundAssign* a = nullptr;
        for (const Block& b : blocks)
            if (b.rid == rid && (a = GivenEntry(store, b, keys)) != nullptr) break;
        if (!a) continue;
        RuleSounds& r = RuleOf(item, rid);
        r.library = a->library;
        for (const ValueSet& v : a->values) SetValueIn(r.values, v.group_uid, v.value_uid);
        changed = true;
    }
    return changed;
}

bool AutoFillNeedsRids(const std::vector<Block>& blocks, const SoundStore& store, const std::vector<std::string>& keys)
{
    for (const Block& b : blocks)
        if (b.rid.empty() && GivenEntry(store, b, keys)) return true;
    return false;
}

bool HasAutoSounds(const SoundStore& store)
{
    for (const AutoSoundAssign& a : store.auto_sounds)
        if (!a.library.empty() && FindLibrary(store, a.library)) return true;
    return false;
}

}  // namespace rav
