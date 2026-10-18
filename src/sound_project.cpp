// SPDX-License-Identifier: MIT
//
// See sound_project.h.

#include "sound_project.h"

#ifdef _WIN32

#include <cstring>
#include <exception>
#include <map>
#include <vector>

#include "console_log.h"
#include "item_rules.h"  // RavTakeOf
#include "populate_progress.h"  // 13-19: LiveSet
#include "sound_record.h"

namespace rav {
namespace {

using GetCurrentProjectInLoadSaveFn = ReaProject* (*)();
using UndoOnStateChangeEx2Fn = void (*)(ReaProject*, const char*, int, int);
using GetItemProjectContextFn = ReaProject* (*)(MediaItem*);

GetCurrentProjectInLoadSaveFn f_project_in_load_save = nullptr;
UndoOnStateChangeEx2Fn        f_undo_change_ex2 = nullptr;
GetItemProjectContextFn       f_item_project = nullptr;
int (*g_register)(const char*, void*) = nullptr;
bool (*g_store_edit_hook)(ReaProject*) = nullptr;  // 13-17 (SetSoundStoreEditHook)

std::map<ReaProject*, SoundStore> g_stores;
uint64_t                          g_store_gen = 0;  // SoundStoreGeneration
// Fix 2026-10-19 (Sound Edit writes slow): each project's store as written by its last ModifySoundStore (SerializeSoundStore
// of g_stores[proj], exactly); absent = not known (a load, a project closed). A write compares with it instead of
// serializing the store before, and REAPER's undo point / project save write it instead of serializing it again: one
// serialization per write instead of three.
std::map<ReaProject*, std::string> g_texts;
// A <RAVSOUNDS chunk that did not read: its text is written back unchanged while the project's
// store stays empty (a save never deletes a library it could not read).
std::map<ReaProject*, std::string> g_unreadable;

ReaProject* ProjectOrCurrent(ReaProject* proj)
{
    return proj ? proj : EnumProjects(-1, nullptr, 0);
}

ReaProject* LoadSaveProject()
{
    ReaProject* p = f_project_in_load_save ? f_project_in_load_save() : nullptr;
    return ProjectOrCurrent(p);
}

void UndoPoint(ReaProject* proj, const char* desc, int states)
{
    if (f_undo_change_ex2) f_undo_change_ex2(proj, desc, states, -1);
    else Undo_OnStateChangeEx(desc, states, -1);
}

// ---- projectconfig ----------------------------------------------------------------------------

constexpr char kChunkOpen[] = "<RAVSOUNDS";

bool ProcessExtensionLine(const char* line, ProjectStateContext* ctx, bool, project_config_extension_t*)
{
    try {
        if (!line || !ctx) return false;
        const size_t n = std::strlen(kChunkOpen);
        if (std::strncmp(line, kChunkOpen, n) != 0) return false;
        if (line[n] != '\0' && line[n] != ' ' && line[n] != '\t' && line[n] != '\r' && line[n] != '\n') return false;
        std::string       text;
        std::vector<char> buf(1u << 16);
        int               depth = 0;  // a nested chunk (not ours) is kept as text
        while (ctx->GetLine(buf.data(), static_cast<int>(buf.size())) == 0) {
            const char* p = buf.data();
            while (*p == ' ' || *p == '\t') ++p;
            if (*p == '>') {
                if (depth == 0) break;
                --depth;
            } else if (*p == '<') {
                ++depth;
            }
            text += p;
            text += '\n';
        }
        SoundStore  s;
        ReaProject* proj = LoadSaveProject();
        if (!ParseSoundStore(text, &s)) {
            LogInfo("RAV: the project's sound library could not be read; it is kept as written.");
            s = SoundStore{};
            g_unreadable[proj] = text;
        }
        g_stores[proj] = std::move(s);
        g_texts.erase(proj);
        ++g_store_gen;
        return true;
    } catch (...) {
        return true;
    }
}

void SaveExtensionConfig(ProjectStateContext* ctx, bool, project_config_extension_t*)
{
    try {
        if (!ctx) return;
        ReaProject*       proj = LoadSaveProject();
        const auto        it = g_stores.find(proj);
        const auto        known = g_texts.find(proj);
        std::string       text = it == g_stores.end() ? std::string()
                                 : known != g_texts.end() ? known->second
                                                          : SerializeSoundStore(it->second);
        const auto        raw = g_unreadable.find(proj);
        if (text.empty() && raw != g_unreadable.end()) text = raw->second;
        if (text.empty()) return;  // an empty store writes no chunk (D1)
        ctx->AddLine("%s", kChunkOpen);
        size_t pos = 0;
        while (pos < text.size()) {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();
            const std::string l = text.substr(pos, nl - pos);
            ctx->AddLine("%s", l.c_str());
            pos = nl + 1;
        }
        ctx->AddLine(">");
    } catch (...) {
    }
}

void BeginLoadProjectState(bool, project_config_extension_t*)
{
    try {
        ReaProject* proj = LoadSaveProject();
        g_stores.erase(proj);
        g_texts.erase(proj);
        ++g_store_gen;
        g_unreadable.erase(proj);
    } catch (...) {
    }
}

project_config_extension_t g_pcreg = {&ProcessExtensionLine, &SaveExtensionConfig, &BeginLoadProjectState, nullptr};
bool                       g_pcreg_on = false;

// ---- P_EXT text ---------------------------------------------------------------------------------

std::string ReadTakeText(MediaItem_Take* take, const char* key)
{
    static std::vector<char> buf;
    if (buf.size() != kItemSoundsMaxBytes + 1) buf.assign(kItemSoundsMaxBytes + 1, '\0');
    buf[0] = '\0';
    if (!GetSetMediaItemTakeInfo_String(take, key, buf.data(), false)) return "";
    buf[kItemSoundsMaxBytes] = '\0';
    return buf.data();
}

std::string ReadItemText(MediaItem* item, const char* key)
{
    std::vector<char> buf(8192, '\0');
    if (!GetSetMediaItemInfo_String(item, key, buf.data(), false)) return "";
    buf.back() = '\0';
    return buf.data();
}

// 13-19: a gesture's items already checked live (LiveItemsBatch): ValidatePtr2 walks the project's items.
LiveSet g_live;

bool ItemValid(MediaItem* item)
{
    return IsLiveItem(item);
}

// ---- The dev check (D10) ------------------------------------------------------------------------

bool AddTestLibrary(SoundStore& s)
{
    SwitchGroup g;
    g.uid = NewSoundUid();
    g.name = "Material";
    SwitchValue wood{NewSoundUid(), "Wood", {}};
    SwitchValue concrete{NewSoundUid(), "Concrete", {}};
    g.values = {wood, concrete};
    SoundLibrary lib;
    lib.uid = NewSoundUid();
    lib.name = "Test library";
    SoundNode root;
    root.uid = NewSoundUid();
    root.type = SoundNodeType::Switch;
    root.name = "Footsteps";
    root.group_uid = g.uid;
    root.default_value = wood.uid;
    SoundNode snd;
    snd.uid = NewSoundUid();
    snd.parent = root.uid;
    snd.type = SoundNodeType::Sound;
    snd.name = "Wood steps";
    snd.sound.source.kind = SoundSourceKind::File;
    snd.sound.source.asset.name = "wood_steps.wav";
    root.cases.push_back({wood.uid, snd.uid, {}});
    lib.root = root.uid;
    lib.nodes = {root, snd};
    s.groups.push_back(g);
    s.libraries.push_back(lib);
    return true;
}

}  // namespace

void InitSoundProject(reaper_plugin_info_t* rec)
{
    if (!rec) return;
    if (rec->GetFunc) {
        f_project_in_load_save =
            reinterpret_cast<GetCurrentProjectInLoadSaveFn>(rec->GetFunc("GetCurrentProjectInLoadSave"));
        f_undo_change_ex2 = reinterpret_cast<UndoOnStateChangeEx2Fn>(rec->GetFunc("Undo_OnStateChangeEx2"));
        f_item_project = reinterpret_cast<GetItemProjectContextFn>(rec->GetFunc("GetItemProjectContext"));
    }
    g_register = rec->Register;
    if (g_register) g_pcreg_on = g_register("projectconfig", &g_pcreg) != 0;
}

void QuitSoundProject()
{
    if (g_register && g_pcreg_on) g_register("-projectconfig", &g_pcreg);
    g_pcreg_on = false;
    g_stores.clear();
    g_texts.clear();
    ++g_store_gen;
    g_unreadable.clear();
}

void LiveItemsBatch(bool on)
{
    if (on) g_live.Begin();
    else g_live.End();
}

void ForgetLiveItem(MediaItem* item)
{
    g_live.Forget(item);
}

bool IsLiveItem(MediaItem* item)
{
    if (!item) return false;
    if (g_live.Known(item)) return true;
    const bool live = ValidatePtr2(nullptr, item, "MediaItem*");
    if (live) g_live.Learn(item);
    return live;
}

uint64_t SoundStoreGeneration() { return g_store_gen; }

const SoundStore& GetSoundStore(ReaProject* proj)
{
    static const SoundStore kEmpty;
    const auto it = g_stores.find(ProjectOrCurrent(proj));
    return it == g_stores.end() ? kEmpty : it->second;
}

void SetSoundStoreEditHook(bool (*hook)(ReaProject* proj))
{
    g_store_edit_hook = hook;
}

bool ModifySoundStore(ReaProject* proj, const char* undo_desc, const std::function<bool(SoundStore&)>& edit)
{
    try {
        proj = ProjectOrCurrent(proj);
        if (!proj || !edit) return false;
        SoundStore        copy = GetSoundStore(proj);
        const auto        known = g_texts.find(proj);
        const std::string before = known != g_texts.end() ? known->second : SerializeSoundStore(copy);
        if (!edit(copy)) return false;
        std::string after = SerializeSoundStore(copy);
        if (after == before) return false;
        g_stores[proj] = std::move(copy);
        g_texts[proj] = std::move(after);  // fix 2026-10-19: the undo point below writes it as is
        ++g_store_gen;
        MarkProjectDirty(proj);
        int states = UNDO_STATE_MISCCFG;
        if (g_store_edit_hook && g_store_edit_hook(proj)) states |= UNDO_STATE_TRACKCFG;  // 13-17: track names
        if (undo_desc) UndoPoint(proj, undo_desc, states);
        return true;
    } catch (...) {
        return false;
    }
}

ItemSounds ReadItemSounds(MediaItem* item)
{
    ItemSounds s;
    try {
        if (!ItemValid(item)) return s;
        MediaItem_Take* take = RavTakeOf(item);
        if (!take) return s;
        const std::string text = ReadTakeText(take, kItemSoundsKey);
        if (!text.empty() && !ParseItemSounds(text, &s)) s = ItemSounds{};
    } catch (...) {
        s = ItemSounds{};
    }
    return s;
}

bool ModifyItemSounds(MediaItem* item, const char* undo_desc, const std::function<bool(ItemSounds&)>& edit,
                      bool* too_large)
{
    if (too_large) *too_large = false;
    try {
        if (!ItemValid(item) || !edit) return false;
        MediaItem_Take* take = RavTakeOf(item);
        if (!take) return false;
        const std::string before = ReadTakeText(take, kItemSoundsKey);
        ItemSounds        s;
        if (!before.empty() && !ParseItemSounds(before, &s)) s = ItemSounds{};
        if (!edit(s)) return false;
        const std::string after = ItemSoundsEmpty(s) ? std::string() : SerializeItemSounds(s);
        if (after == before) return false;
        if (after.size() >= kItemSoundsMaxBytes) {
            LogInfo("RAV: the item's sound record is too large to save.");
            if (too_large) *too_large = true;
            return false;
        }
        // A removal (empty record) REAPER may answer false to though it made it: judged by reading back.
        if (!GetSetMediaItemTakeInfo_String(take, kItemSoundsKey, const_cast<char*>(after.c_str()), true) &&
            !(after.empty() && ReadTakeText(take, kItemSoundsKey).empty()))
            return false;
        // The item's own project (it may sit in another tab), as ModifySoundStore does.
        ReaProject* proj = ProjectOrCurrent(f_item_project ? f_item_project(item) : nullptr);
        MarkProjectDirty(proj);
        if (undo_desc) UndoPoint(proj, undo_desc, UNDO_STATE_ITEMS);
        return true;
    } catch (...) {
        return false;
    }
}

bool ReadPlacedTag(MediaItem* item, PlacedTag* out)
{
    try {
        if (!ItemValid(item)) return false;
        const std::string text = ReadItemText(item, kPlacedTagKey);
        return !text.empty() && ParsePlacedTag(text, out);
    } catch (...) {
        return false;
    }
}

bool WritePlacedTag(MediaItem* item, const PlacedTag& tag)
{
    try {
        if (!ItemValid(item)) return false;
        std::string text = SerializePlacedTag(tag);
        if (!text.empty() && text.back() == '\n') text.pop_back();
        return GetSetMediaItemInfo_String(item, kPlacedTagKey, const_cast<char*>(text.c_str()), true);
    } catch (...) {
        return false;
    }
}

bool ClearPlacedTag(MediaItem* item)
{
    try {
        if (!ItemValid(item)) return false;
        char empty[1] = {'\0'};
        // REAPER may answer false to a removal it made: judged by reading back.
        return GetSetMediaItemInfo_String(item, kPlacedTagKey, empty, true) || ReadItemText(item, kPlacedTagKey).empty();
    } catch (...) {
        return false;
    }
}

SoundUndoScope::SoundUndoScope(ReaProject* proj, const char* desc) : proj_(proj), desc_(desc ? desc : "RAV: Sounds")
{
    Undo_BeginBlock2(proj_);
}

SoundUndoScope::~SoundUndoScope()
{
    try {
        Undo_EndBlock2(proj_, desc_.c_str(), UNDO_STATE_ALL);
    } catch (...) {
    }
}

void RunSoundStoreCheck()
{
    try {
        ReaProject* proj = ProjectOrCurrent(nullptr);
        if (!proj) return;
        const SoundStore& s = GetSoundStore(proj);
        if (SoundStoreEmpty(s)) {
            bool added = false;
            {
                SoundUndoScope scope(proj, "RAV: Add test sound library");
                added = ModifySoundStore(proj, nullptr, &AddTestLibrary);
            }
            ShowMessageBox(added ? "A test sound library was added to this project:\n"
                                   "Switch Group \"Material\" (Wood, Concrete), library \"Test library\"\n"
                                   "(a Switch with one Sound).\n\nEdit > Undo removes it."
                                 : "The test sound library could not be added.",
                           "RAV: Dev - Sound library storage check", 0);
            return;
        }
        size_t nodes = 0;
        for (const SoundLibrary& l : s.libraries) nodes += l.nodes.size();
        const std::string msg = "This project's sound store:\n\n" + std::to_string(s.libraries.size()) +
                                (s.libraries.size() == 1 ? " library\n" : " libraries\n") +
                                std::to_string(s.groups.size()) +
                                (s.groups.size() == 1 ? " Switch Group\n" : " Switch Groups\n") +
                                std::to_string(nodes) + (nodes == 1 ? " node\n" : " nodes\n") + "version " +
                                std::to_string(s.version);
        ShowMessageBox(msg.c_str(), "RAV: Dev - Sound library storage check", 0);
    } catch (...) {
    }
}

}  // namespace rav

#endif  // _WIN32
