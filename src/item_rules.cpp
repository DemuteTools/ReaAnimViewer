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

// A name as SavePresetAs stores it (trimmed), for an undo point's name.
std::string TrimPresetNameForUndo(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
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

SkeletonBones SkeletonBonesOf(const SceneSkeleton& skel)
{
    SkeletonBones sk;
    sk.names.reserve(skel.bones.size());
    sk.parents.reserve(skel.bones.size());
    for (const SceneBone& b : skel.bones) {
        sk.names.push_back(b.name);
        sk.parents.push_back(b.parentIdx);
    }
    return sk;
}

SkeletonBones ItemSkeletonBones(MediaItem* item)
{
    SkeletonBones sk;
    try {
        const std::string path = AnimPathOf(RavTakeOf(item));
        if (path.empty()) return sk;
        const std::shared_ptr<const CpuAsset> asset = AcquireCpuAsset(path);
        if (!asset) return sk;
        sk = SkeletonBonesOf(asset->skeleton);
    } catch (...) {
        sk = SkeletonBones{};
    }
    return sk;
}

bool CheckItemBinding(MediaItem* item, const std::vector<Block>& blocks, std::string* missing)
{
    if (missing) missing->clear();
    try {
        const SkeletonBones sk = ItemSkeletonBones(item);
        const std::vector<std::string>& names = sk.names;
        if (names.empty()) {
            if (missing) *missing = "the animation file did not load";
            return false;
        }
        std::vector<Block> bound = blocks;
        return BindBoneRefs(bound, GetRoleMapping(RulesResourceRoot(), names), names, sk.parents, missing);
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

bool SaveItemAsPreset(MediaItem* item, const std::string& preset_id, const std::string& name, std::string* saved_id,
                      std::string* saved_name, std::string* err)
{
    ItemRulesRead rd;
    if (!ReadItemRules(item, &rd)) return Fail(err, "The item is not a RAV animation.");
    if (!rd.present || !rd.valid) return Fail(err, "The item has no rules to save.");
    // The file is written (no undo point: a file is not project state) inside the edit, before
    // the item is; the item then adopts it in one undo point. The undo name needs the preset's
    // name, known only once written: Save keeps the preset's name, Save as uses `name`.
    std::string desc_name = TrimPresetNameForUndo(name);
    if (!preset_id.empty()) {
        PresetInfo info;
        if (FindPreset(RulesResourceRoot(), preset_id, &info)) desc_name = info.name;
    }
    const std::string desc = "RAV: Save preset " + desc_name;
    std::string why, id, nm;
    bool        file_written = false;
    const bool  ok = ModifyItemRules(
        item, desc.c_str(),
        [&](ItemRules& r) {
            file_written = SaveRulesAsPreset(RulesResourceRoot(), r, preset_id, name, &id, &nm, &why);
            return file_written;
        },
        err);
    if (!file_written) {
        if (why.empty() && err && !err->empty()) return false;  // the item could not be read
        return Fail(err, why.empty() ? "The preset could not be saved." : why);
    }
    if (saved_id) *saved_id = id;
    if (saved_name) *saved_name = nm;
    if (!ok) {
        const std::string reason = err ? *err : std::string();
        return Fail(err, "The preset " + nm + " was written, but the item could not take it: " + reason);
    }
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
