// SPDX-License-Identifier: MIT
//
// See item_rules.h.

#include "item_rules.h"

#ifdef _WIN32

#include <cstring>
#include <exception>
#include <memory>

#include "asset_cache.h"
#include "event_list.h"
#include "pcm_source_anim.h"
#include "preset_store.h"
#include "role_map_store.h"

namespace rav {
namespace {

bool Fail(std::string* err, const std::string& why)
{
    if (err) *err = why;
    return false;
}

// Writes the record text, then records one undo point (UNDO_STATE_ITEMS) only when REAPER
// accepted it: a refused write leaves no empty undo point. `with_undo` false: no undo point
// (the caller's undo block records it).
bool WriteRaw(MediaItem_Take* take, const std::string& text, const char* undo_desc, bool with_undo, std::string* err)
{
    if (text.size() >= kItemRulesMaxBytes) return Fail(err, "The item's rules are too large to save.");
    if (!take || !ValidatePtr2(nullptr, take, "MediaItem_Take*")) return Fail(err, "The item's take is gone.");
    if (!GetSetMediaItemTakeInfo_String(take, kItemRulesKey, const_cast<char*>(text.c_str()), true))
        return Fail(err, "REAPER refused to save the item's rules.");
    if (with_undo) Undo_OnStateChangeEx(undo_desc ? undo_desc : "RAV: Edit auto-tagging rules", UNDO_STATE_ITEMS, -1);
    return true;
}

bool ModifyCore(MediaItem* item, const char* undo_desc, bool with_undo, const std::function<bool(ItemRules&)>& edit,
                std::string* err)
{
    try {
        MediaItem_Take* take = RavTakeOf(item);
        if (!take) return Fail(err, "Not a RAV animation item.");
        ItemRulesRead cur;
        if (!ReadItemRules(item, &cur)) return Fail(err, "The item's rules could not be read.");
        ItemRules rules = cur.valid ? cur.rules : ItemRules{};
        // What the record says before the edit: an absent or unreadable record = no rules.
        const std::string before = SerializeItemRules(rules);
        if (!edit || !edit(rules)) return true;  // cancelled: nothing written
        const std::string text = SerializeItemRules(rules);
        // No change, no undo point (and an absent or unreadable record stays as it is).
        if (text == before || (cur.valid && text == cur.raw)) return true;
        return WriteRaw(take, text, undo_desc, with_undo, err);
    } catch (const std::exception& e) {
        return Fail(err, std::string("The item's rules could not be saved: ") + e.what());
    } catch (...) {
        return Fail(err, "The item's rules could not be saved.");
    }
}

}  // namespace

std::string AnimPathOf(MediaItem_Take* take)
{
    PCM_source* src = take ? GetMediaItemTake_Source(take) : nullptr;
    if (!src || !IsRavAnimSource(src)) return "";
    for (int depth = 0; src && depth < 4; ++depth) {
        const char* type = src->GetType();
        if (type && std::strcmp(type, "RAV_ANIM") == 0) break;
        src = src->GetSource();
    }
    const char* fn = src ? src->GetFileName() : nullptr;
    return fn ? fn : "";
}

MediaItem_Take* RavTakeOf(MediaItem* item)
{
    MediaItem_Take* take = item ? GetActiveTake(item) : nullptr;
    PCM_source*     src = take ? GetMediaItemTake_Source(take) : nullptr;
    return (src && IsRavAnimSource(src)) ? take : nullptr;
}

bool ReadItemRules(MediaItem* item, ItemRulesRead* out)
{
    if (!out) return false;
    *out = ItemRulesRead{};
    try {
        MediaItem_Take* take = RavTakeOf(item);
        if (!take) return false;
        // The _String getter copies into the caller's buffer: give it the largest record.
        static std::vector<char> buf;
        buf.assign(kItemRulesMaxBytes + 1, '\0');
        if (!GetSetMediaItemTakeInfo_String(take, kItemRulesKey, buf.data(), false)) return true;
        buf[kItemRulesMaxBytes] = '\0';
        out->raw = buf.data();
        out->present = !out->raw.empty();
        if (out->present) out->valid = ParseItemRules(out->raw, &out->rules);
        return true;
    } catch (...) {
        *out = ItemRulesRead{};
        return false;
    }
}

bool ModifyItemRules(MediaItem* item, const char* undo_desc, const std::function<bool(ItemRules&)>& edit,
                     std::string* err)
{
    return ModifyCore(item, undo_desc, true, edit, err);
}

bool ModifyItemRulesNoUndo(MediaItem* item, const std::function<bool(ItemRules&)>& edit, std::string* err)
{
    return ModifyCore(item, nullptr, false, edit, err);
}

bool WriteItemRules(MediaItem* item, const ItemRules& rules, const char* undo_desc, std::string* err)
{
    return ModifyItemRules(
        item, undo_desc,
        [&](ItemRules& r) {
            r = rules;
            return true;
        },
        err);
}

std::string RulesResourceRoot()
{
    const char* p = GetResourcePath();
    return p ? p : "";
}

std::vector<std::string> ItemBoneNames(MediaItem* item)
{
    std::vector<std::string> names;
    try {
        const std::string path = AnimPathOf(RavTakeOf(item));
        if (path.empty()) return names;
        const std::shared_ptr<const CpuAsset> asset = AcquireCpuAsset(path);
        if (!asset) return names;
        for (const SceneBone& b : asset->skeleton.bones) names.push_back(b.name);
    } catch (...) {
        names.clear();
    }
    return names;
}

bool CheckItemBinding(MediaItem* item, const std::vector<Block>& blocks, std::string* missing)
{
    if (missing) missing->clear();
    try {
        const std::vector<std::string> names = ItemBoneNames(item);
        if (names.empty()) {
            if (missing) *missing = "the animation file did not load";
            return false;
        }
        std::vector<Block> bound = blocks;
        return BindBoneRefs(bound, GetRoleMapping(RulesResourceRoot(), names), names, missing);
    } catch (...) {
        if (missing) *missing = "the item's bones could not be read";
        return false;
    }
}

bool SetUpPresetOnItem(MediaItem* item, const std::string& preset_id, std::string* missing, std::string* err)
{
    if (missing) missing->clear();
    PresetData preset;
    if (!LoadPreset(RulesResourceRoot(), preset_id, &preset, err)) return false;
    const std::string desc = "RAV: Load preset " + preset.name;
    if (!ModifyItemRules(
            item, desc.c_str(),
            [&](ItemRules& r) {
                ApplyPreset(r, preset);
                ClearCorrections(r);  // Story 10-4: a preset starts the item's event list afresh
                return true;
            },
            err))
        return false;
    CheckItemBinding(item, preset.blocks, missing);
    return true;
}

bool UpdateItemFromPreset(MediaItem* item, std::string* err)
{
    std::string why;
    const bool  ok = ModifyItemRules(
        item, "RAV: Update rules from preset",
        [&](ItemRules& r) { return UpdateFromPreset(RulesResourceRoot(), r, &why); }, err);
    if (ok && !why.empty()) return Fail(err, why);
    return ok;
}

bool KeepItemCurrent(MediaItem* item, std::string* err)
{
    std::string why;
    const bool  ok = ModifyItemRules(
        item, "RAV: Keep current rules",
        [&](ItemRules& r) { return KeepCurrent(RulesResourceRoot(), r, &why); }, err);
    if (ok && !why.empty()) return Fail(err, why);
    return ok;
}

}  // namespace rav

#endif  // _WIN32
