// SPDX-License-Identifier: MIT
//
// See item_rules.h.

#include "item_rules.h"

#ifdef _WIN32

#include <algorithm>
#include <cstring>
#include <exception>
#include <map>
#include <memory>

#include "asset_cache.h"
#include "console_log.h"
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

ItemRulesWrittenHook g_written_hook = nullptr;

constexpr char kUndoMakeUnique[] = "RAV: Make tagging unique";

// ---- 10-6: pools ---------------------------------------------------------------------------------

// A take's record text, without ReadItemRules' 1 MB zero-fill (a pool scan reads every item of
// the file). "" when there is none.
std::string ReadRecordText(MediaItem_Take* take)
{
    static std::vector<char> buf;
    if (buf.size() != kItemRulesMaxBytes + 1) buf.assign(kItemRulesMaxBytes + 1, '\0');
    buf[0] = '\0';
    if (!GetSetMediaItemTakeInfo_String(take, kItemRulesKey, buf.data(), false)) return "";
    buf[kItemRulesMaxBytes] = '\0';
    return buf.data();
}

// One member of a pool, its record as read.
struct PoolMember {
    MediaItem*      item = nullptr;
    MediaItem_Take* take = nullptr;
    bool            present = false;  // it has a record (it reads: an unreadable one is never a member)
    ItemRules       rules;            // the record (none: no rules)
    std::string     raw;
};

// The RAV items of `proj` that play `path` with pool id `id`, in project order, `skip` left out.
// Membership is rule_record.h's InPool (host-tested); only the reads are REAPER's.
std::vector<PoolMember> ScanPool(ReaProject* proj, const std::string& path, const std::string& id, MediaItem* skip)
{
    std::vector<PoolMember> out;
    if (path.empty()) return out;
    const int n = CountMediaItems(proj);
    for (int i = 0; i < n; ++i) {
        MediaItem* it = GetMediaItem(proj, i);
        if (!it || it == skip) continue;
        MediaItem_Take* take = RavTakeOf(it);
        if (!take) continue;
        PoolCandidate c;
        c.path = AnimPathOf(take);
        if (!SamePoolPath(c.path, path)) continue;  // another file: its record is not read
        c.record = ReadRecordText(take);
        PoolMember m;
        if (!InPool(c, path, id, &m.rules)) continue;  // another pool, or a record that does not read
        m.item = it;
        m.take = take;
        m.present = !c.record.empty();
        m.raw = std::move(c.record);
        out.push_back(std::move(m));
    }
    return out;
}

// One other member of a gesture's pool, as it is written.
struct MemberWrite {
    MediaItem*      item = nullptr;
    MediaItem_Take* take = nullptr;
    bool            before_valid = false;
    ItemRules       before;
    std::string     before_raw;
    ItemRules       after;
    std::string     text;
};

// The pool's records, for rule_record.h's PoolGestureStart / PlanPoolGesture.
std::vector<PoolRecord> RecordsOf(const std::vector<PoolMember>& members)
{
    std::vector<PoolRecord> out;
    out.reserve(members.size());
    for (const PoolMember& m : members) {
        PoolRecord r;
        r.present = m.present;
        r.rules = m.rules;
        out.push_back(std::move(r));
    }
    return out;
}

// The members the plan writes (`which`: indices into `members`): each with its own bookkeeping and
// `content`'s content.
std::vector<MemberWrite> MemberWrites(std::vector<PoolMember>& members, const std::vector<size_t>& which,
                                      const ItemRules& content)
{
    std::vector<MemberWrite> out;
    for (size_t i : which) {
        if (i >= members.size()) continue;
        PoolMember& m = members[i];
        MemberWrite w;
        w.after = m.rules;
        CopyPoolContent(content, w.after);
        w.text = SerializeItemRules(w.after);
        w.item = m.item;
        w.take = m.take;
        w.before_valid = m.present;
        w.before = m.rules;
        w.before_raw = m.raw;
        out.push_back(std::move(w));
    }
    return out;
}

// Writes the members (inside the gesture's undo block), each followed by its written-hook.
void WritePoolMembers(const std::vector<MemberWrite>& pool, const char* desc)
{
    for (const MemberWrite& w : pool) {
        try {
            std::string why;
            if (!WriteRaw(w.take, w.text, desc, false, &why)) {
                LogWarn("Auto-tagging: a linked item's rules could not be saved: %s", why.c_str());
                continue;
            }
            if (g_written_hook) g_written_hook(w.item, w.before_valid, w.before, w.before_raw, w.after);
        } catch (...) {
        }
    }
}

std::string FreshPoolId()
{
    GUID g{};
    genGuid(&g);
    char buf[64] = {};
    guidToString(&g, buf);
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

bool ModifyCore(MediaItem* item, const char* undo_desc, bool with_undo, const std::function<bool(ItemRules&)>& edit,
                std::string* err)
{
    try {
        MediaItem_Take* take = RavTakeOf(item);
        if (!take) return Fail(err, "Not a RAV animation item.");
        ItemRulesRead cur;
        if (!ReadItemRules(item, &cur)) return Fail(err, "The item's rules could not be read.");
        // 10-6: a gesture reaches the other copies of its pool (a member tagged differently takes
        // this item's tagging too); the bookkeeping writers (no undo) stay on their item. An item
        // with no record joins its pool: the edit starts from the pool's tagging, and the item is
        // written with it. The decisions are rule_record.h's PoolGestureStart / PlanPoolGesture.
        const std::string       path = AnimPathOf(take);
        const std::string       start_id = cur.valid ? cur.rules.pool_id : std::string();
        std::vector<PoolMember> members;
        if (with_undo) members = ScanPool(nullptr, path, start_id, item);
        bool      adopted = false;
        ItemRules rules = PoolGestureStart(cur.present, cur.valid, cur.rules, RecordsOf(members), &adopted);
        // What the record says before the edit: an absent or unreadable record = no rules.
        const std::string before = SerializeItemRules(rules);
        if (!edit || !edit(rules)) return true;  // cancelled: nothing written
        if (with_undo && rules.pool_id != start_id) members = ScanPool(nullptr, path, rules.pool_id, item);
        const PoolGesturePlan plan = PlanPoolGesture(cur.valid, cur.raw, before, adopted, rules, RecordsOf(members));
        const std::string     text = SerializeItemRules(rules);
        // No change, no undo point (and an absent or unreadable record stays as it is).
        const bool               self = plan.write_self;
        std::vector<MemberWrite> pool = MemberWrites(members, plan.members, rules);
        if (!self && pool.empty()) return true;
        if (!with_undo) return WriteRaw(take, text, undo_desc, false, err);
        if (!g_written_hook && pool.empty()) return WriteRaw(take, text, undo_desc, true, err);
        // 10-4 fb-4: the record, the first Cancel snapshot and the preview markers in ONE undo
        // point (10-6: the pool's too). What would refuse the write is checked first, so no empty
        // undo point is made.
        if (self) {
            if (text.size() >= kItemRulesMaxBytes) return Fail(err, "The item's rules are too large to save.");
            if (!ValidatePtr2(nullptr, take, "MediaItem_Take*")) return Fail(err, "The item's take is gone.");
        }
        const char* desc = undo_desc ? undo_desc : "RAV: Edit auto-tagging rules";
        Undo_BeginBlock2(nullptr);
        bool ok = !self;
        try {
            if (self) {
                ok = WriteRaw(take, text, desc, false, err);
                if (ok && g_written_hook) g_written_hook(item, cur.valid, cur.valid ? cur.rules : ItemRules{}, cur.raw, rules);
            }
            if (ok) WritePoolMembers(pool, desc);  // the item's own write failed: no member either
        } catch (...) {
            // The block is always closed; the record write's own result stands.
        }
        Undo_EndBlock2(nullptr, desc, UNDO_STATE_ALL);
        return ok;
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

bool ReadCommittedSnapshot(MediaItem* item, std::string* text)
{
    if (text) text->clear();
    try {
        MediaItem_Take* take = RavTakeOf(item);
        if (!take) return false;
        static std::vector<char> buf;
        buf.assign(kItemRulesMaxBytes + 1, '\0');
        if (!GetSetMediaItemTakeInfo_String(take, kItemRulesCommittedKey, buf.data(), false)) return false;
        buf[kItemRulesMaxBytes] = '\0';
        if (!buf[0]) return false;
        if (text) *text = buf.data();
        return true;
    } catch (...) {
        if (text) text->clear();
        return false;
    }
}

bool WriteCommittedSnapshotNoUndo(MediaItem* item, const std::string& text, std::string* err)
{
    try {
        if (text.size() >= kItemRulesMaxBytes) return Fail(err, "The item's rules are too large to save.");
        MediaItem_Take* take = RavTakeOf(item);
        if (!take || !ValidatePtr2(nullptr, take, "MediaItem_Take*")) return Fail(err, "The item's take is gone.");
        if (!GetSetMediaItemTakeInfo_String(take, kItemRulesCommittedKey, const_cast<char*>(text.c_str()), true))
            return Fail(err, "REAPER refused to save the item's committed rules.");
        return true;
    } catch (...) {
        return Fail(err, "The item's committed rules could not be saved.");
    }
}

void SetItemRulesWrittenHook(ItemRulesWrittenHook hook)
{
    g_written_hook = hook;
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

RoleMapping ItemRoleMapping(const std::vector<std::string>& bone_names, const std::vector<Block>& blocks)
{
    return RoleMappingForBlocks(RulesResourceRoot(), bone_names, blocks);
}

bool BindWithRoleMapping(std::vector<Block>& blocks, const std::vector<std::string>& bone_names,
                         const std::vector<int>& bone_parents, std::string* missing)
{
    return BindBlocksWithRoleMapping(RulesResourceRoot(), blocks, bone_names, bone_parents, missing);
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
        return BindWithRoleMapping(bound, names, sk.parents, missing);
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
    // 10-6: a linked item with no record holds its pool's tagging (ModifyItemRules writes it with it).
    if ((!rd.present || !rd.valid) && (rd.present || !ItemPoolContent(item, nullptr)))
        return Fail(err, "The item has no rules to save.");
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

// ---- 10-6: pooled copies --------------------------------------------------------------------------

bool ItemPoolContent(MediaItem* item, ItemRules* out)
{
    try {
        MediaItem_Take* take = RavTakeOf(item);
        if (!take || !ReadRecordText(take).empty()) return false;  // not RAV, or a record of its own
        bool            adopted = false;
        const ItemRules held =
            PoolGestureStart(false, false, ItemRules{}, RecordsOf(ScanPool(nullptr, AnimPathOf(take), "", item)), &adopted);
        if (!adopted) return false;
        if (out) *out = held;
        return true;
    } catch (...) {
        return false;
    }
}

bool ItemPoolKey(MediaItem* item, std::string* path, std::string* pool_id)
{
    try {
        MediaItem_Take* take = RavTakeOf(item);
        if (!take) return false;
        const std::string p = AnimPathOf(take);
        std::string       id;
        if (!PoolKeyOf(p, p.empty() ? std::string() : ReadRecordText(take), &id)) return false;  // in no pool
        if (path) *path = p;
        if (pool_id) *pool_id = id;
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<MediaItem*> PoolMembersOf(MediaItem* item, ReaProject* proj)
{
    std::vector<MediaItem*> out;
    try {
        std::string path, id;
        if (!ItemPoolKey(item, &path, &id)) return out;
        for (const PoolMember& m : ScanPool(proj, path, id, item)) out.push_back(m.item);
    } catch (...) {
        out.clear();
    }
    return out;
}

std::vector<MediaItem*> WithPoolMembers(const std::vector<MediaItem*>& items, ReaProject* proj, std::vector<int>* pool_of)
{
    std::vector<MediaItem*> out;
    std::vector<int>        group;
    try {
        // The project's items as candidates (paths only), then the items asked about that are not
        // among them. The grouping itself is rule_record.h's GroupPools (host-tested).
        std::vector<MediaItem*>       handles;
        std::vector<PoolCandidate>    cands;
        std::map<MediaItem*, size_t>  index;
        auto path_of = [](MediaItem* it) {
            MediaItem_Take* take = RavTakeOf(it);
            return take ? AnimPathOf(take) : std::string();
        };
        const int n = CountMediaItems(proj);
        for (int i = 0; i < n; ++i) {
            MediaItem* it = GetMediaItem(proj, i);
            if (!it || index.count(it)) continue;
            index[it] = handles.size();
            handles.push_back(it);
            PoolCandidate c;
            c.path = path_of(it);
            cands.push_back(std::move(c));
        }
        std::vector<size_t> sel;
        for (MediaItem* it : items) {
            if (!it) continue;
            const auto f = index.find(it);
            if (f != index.end()) {
                sel.push_back(f->second);
                continue;
            }
            index[it] = handles.size();
            sel.push_back(handles.size());
            handles.push_back(it);
            PoolCandidate c;
            c.path = path_of(it);
            c.in_project = false;
            cands.push_back(std::move(c));
        }
        // Records are read only where a pool asked about could be: the files of the items asked about.
        for (size_t j = 0; j < cands.size(); ++j)
            for (size_t s : sel)
                if (SamePoolPath(cands[j].path, cands[s].path)) {
                    if (MediaItem_Take* take = RavTakeOf(handles[j])) cands[j].record = ReadRecordText(take);
                    break;
                }
        const PoolGrouping g = GroupPools(cands, sel);
        for (size_t k = 0; k < g.order.size(); ++k) {
            out.push_back(handles[g.order[k]]);
            group.push_back(g.pool_of[k]);
        }
    } catch (...) {
        // The items asked about stand, each in a pool of its own.
        out.clear();
        group.clear();
        for (MediaItem* it : items)
            if (it && std::find(out.begin(), out.end(), it) == out.end()) {
                out.push_back(it);
                group.push_back(static_cast<int>(group.size()));
            }
    }
    if (pool_of) *pool_of = group;
    return out;
}

namespace {

// Gives each item that shares its pool a fresh pool id of its own, in one undo point. How many.
int MakeUniqueCore(const std::vector<MediaItem*>& items, std::string* err)
{
    struct Write {
        MediaItem_Take* take = nullptr;
        std::string     text;
    };
    std::vector<Write> writes;
    for (MediaItem* it : items) {
        try {
            MediaItem_Take* take = RavTakeOf(it);
            if (!take) continue;
            ItemRulesRead rd;
            if (!ReadItemRules(it, &rd) || (rd.present && !rd.valid)) continue;  // never overwritten
            std::string path, id;
            if (!ItemPoolKey(it, &path, &id)) continue;
            const std::vector<PoolMember> members = ScanPool(nullptr, path, id, it);
            if (members.empty()) continue;  // nothing shares its tagging: already unique
            // A linked item with no record holds its pool's tagging: it keeps that.
            bool      adopted = false;
            ItemRules r = PoolGestureStart(rd.present, rd.valid, rd.rules, RecordsOf(members), &adopted);
            r.has_pool = true;
            r.pool_id = FreshPoolId();
            Write w;
            w.take = take;
            w.text = SerializeItemRules(r);
            if (w.text.size() >= kItemRulesMaxBytes) continue;
            writes.push_back(std::move(w));
        } catch (...) {
        }
    }
    if (writes.empty()) return 0;
    int done = 0;
    Undo_BeginBlock2(nullptr);
    for (const Write& w : writes) {
        try {
            if (WriteRaw(w.take, w.text, kUndoMakeUnique, false, err)) ++done;
        } catch (...) {
        }
    }
    Undo_EndBlock2(nullptr, kUndoMakeUnique, UNDO_STATE_ITEMS);
    return done;
}

}  // namespace

bool MakeItemUnique(MediaItem* item, bool* done, std::string* err)
{
    if (done) *done = false;
    try {
        if (!item || !ValidatePtr2(nullptr, item, "MediaItem*")) return Fail(err, "The item is gone.");
        if (!RavTakeOf(item)) return Fail(err, "Not a RAV animation item.");
        std::string path, id;
        if (!ItemPoolKey(item, &path, &id)) return true;  // its record does not read: left as it is
        if (PoolMembersOf(item).empty()) return true;     // already unique
        std::string why;
        const int   n = MakeUniqueCore({item}, &why);
        if (n == 0) return Fail(err, why.empty() ? std::string("The item's rules could not be saved.") : why);
        if (done) *done = true;
        return true;
    } catch (...) {
        return Fail(err, "The item's rules could not be saved.");
    }
}

int MakeTaggingUniqueOnSelectedItems()
{
    try {
        std::vector<MediaItem*> items;
        const int               n = CountSelectedMediaItems(nullptr);
        for (int i = 0; i < n; ++i)
            if (MediaItem* it = GetSelectedMediaItem(nullptr, i)) items.push_back(it);
        std::string err;
        const int   done = MakeUniqueCore(items, &err);
        if (done > 0) LogInfo("Auto-tagging: %d item%s made unique", done, done == 1 ? "" : "s");
        else if (!err.empty()) LogWarn("Auto-tagging: Make unique: %s", err.c_str());
        return done;
    } catch (...) {
        return 0;
    }
}

}  // namespace rav

#endif  // _WIN32
