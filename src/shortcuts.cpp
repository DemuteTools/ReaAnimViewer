// SPDX-License-Identifier: MIT
//
// See shortcuts.h -- the live half: the current bindings, REAPER ExtState, the
// recording session and the key names of the user's keyboard layout. 13-60: the rows linked
// to a REAPER action, their keys read from reaper-kb.ini (through view_nav's watch).

#include "shortcuts.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include "action_link.h"
#include "console_log.h"
#include "reaper_api.h"
#include "reaper_view_keys.h"
#include "view_nav.h"  // 13-45: the view rows' recording; 13-60: reaper-kb.ini as read

namespace rav {
namespace {

constexpr char kExtSection[] = "ReaAnimViewer";

ShortcutBindings g_bindings = DefaultShortcutBindings();
std::string      g_labels[kShortcutCount];
int              g_recording_id = -1;
int              g_conflict_id  = -1;

// 13-60: the rows linked to a REAPER action ("" = own key), and the keys in force built from them
// (again when a binding, a link or reaper-kb.ini changes).
std::array<std::string, kShortcutCount> g_links;
std::array<bool, kShortcutCount>        g_follow_reaper{};  // 13-60b: the row is on REAPER (its own action's keys)
ShortcutKeymap                          g_keymap = ShortcutKeymapOf(DefaultShortcutBindings());
bool                                    g_keymap_dirty = true;
std::array<bool, kShortcutCount>        g_on_fallback{};  // 13-63: the row runs on its fallback key (REAPER has none)
int                                     g_keymap_kb_gen = -1;

std::string ExtKey(int id)
{
    return std::string("shortcut.") + kShortcutTable[id].id;
}

// 13-60b: a row on REAPER follows its own action: linked, as a row linked to an action the user picked.
std::string Followed(int i)
{
    if (g_follow_reaper[static_cast<size_t>(i)]) return ShortcutReaperActionId(i);
    return g_links[static_cast<size_t>(i)];
}

std::array<bool, kShortcutCount> LinkedFlags()
{
    std::array<bool, kShortcutCount> f{};
    for (int i = 0; i < kShortcutCount; ++i) f[static_cast<size_t>(i)] = !Followed(i).empty();
    return f;
}

}  // namespace

// The keyboard layout's own name for a key the table does not name (e.g. ";" or "ù").
std::string LayoutKeyName(unsigned key)
{
    UINT sc = MapVirtualKeyW(key, 4 /* MAPVK_VK_TO_VSC_EX */);
    if (sc == 0) return std::string();
    LONG lp = static_cast<LONG>((sc & 0xFF) << 16);
    if ((sc & 0xFF00) == 0xE000 || (sc & 0xFF00) == 0xE100) lp |= (1L << 24);  // extended key
    wchar_t wname[64] = {};
    const int len = GetKeyNameTextW(lp, wname, 64);
    if (len <= 0) return std::string();
    // A single lower-case letter ("ù", "ß") reads better as the key cap shows it.
    if (len == 1) CharUpperW(wname);
    char name[192] = {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, static_cast<int>(sizeof(name)), nullptr, nullptr);
    if (n <= 0) return std::string();
    return std::string(name);
}

bool KeyBindingOfGesture(const KbGesture& g, KeyBinding* out)
{
    if (g.input != KbInput::Key) return false;
    KeyBinding b;
    b.ctrl = (g.mods & kViewModCtrl) != 0;
    b.alt = (g.mods & kViewModAlt) != 0;
    b.shift = (g.mods & kViewModShift) != 0;
    if (g.is_char) {
        if (g.code <= 0 || g.code > 0xFFFF) return false;
        const SHORT scan = VkKeyScanExW(static_cast<WCHAR>(g.code), GetKeyboardLayout(0));
        if (scan == -1) return false;  // no key types it on this layout
        b.vk = static_cast<unsigned>(scan & 0xFF);
        b.shift = b.shift || (scan & 0x100);
        b.ctrl = b.ctrl || (scan & 0x200);
        b.alt = b.alt || (scan & 0x400);
    } else {
        b.vk = static_cast<unsigned>(g.code);
    }
    if (!IsBindableKey(b.vk)) return false;
    if (out) *out = b;
    return true;
}

namespace {

// 13-60: the keys REAPER gives a linked row's action (the wheel is not a key row's).
std::vector<KeyBinding> LinkedKeys(const std::string& cmd)
{
    std::vector<KeyBinding> out;
    for (const KbGesture& g : KbGesturesOf(CurrentReaperViewKeys(), cmd)) {
        KeyBinding b;
        if (KeyBindingOfGesture(g, &b) && std::find(out.begin(), out.end(), b) == out.end()) out.push_back(b);
    }
    return out;
}

void RefreshLabels();

void RebuildKeymap()
{
    ShortcutKeyLists linked_keys;
    const std::array<bool, kShortcutCount> linked = LinkedFlags();
    for (int i = 0; i < kShortcutCount; ++i)
        if (linked[static_cast<size_t>(i)]) linked_keys[static_cast<size_t>(i)] = LinkedKeys(Followed(i));
    ApplyReaperFallbacks(linked_keys, g_follow_reaper.data(), g_on_fallback.data());  // 13-63
    g_keymap = ResolveShortcutKeys(g_bindings, linked_keys, linked.data());
    g_keymap_dirty = false;
    g_keymap_kb_gen = ReaperKbGeneration();
    RefreshLabels();
}

const ShortcutKeymap& Keymap()
{
    if (g_keymap_dirty || g_keymap_kb_gen != ReaperKbGeneration()) RebuildKeymap();
    return g_keymap;
}

void RefreshLabels()
{
    // The first key in force (a linked row: its action's first key; none: "").
    for (int i = 0; i < kShortcutCount; ++i) {
        const std::vector<KeyBinding>& ks = g_keymap.keys[static_cast<size_t>(i)];
        g_labels[i] = ks.empty() ? std::string() : FormatShortcut(ks.front(), &LayoutKeyName);
    }
}

// Only custom keys are stored: a default deletes the entry (10-11: so does unbound, which
// gives the default back on the next load once its key is free). 13-60: a link is "link:<id>".
// 13-60b: a row on REAPER is "reaper". 13-61: a row whose default is REAPER (ShortcutDefaultFollowsReaper)
// stores nothing on REAPER, and "own" when Own with no key yet.
void Save(int id)
{
    if (id < 0 || id >= kShortcutCount) return;
    const KeyBinding& b = g_bindings[static_cast<size_t>(id)];
    const std::string key = ExtKey(id);
    const bool        reaper_default = ShortcutDefaultFollowsReaper(id);
    if (g_follow_reaper[static_cast<size_t>(id)]) {
        if (reaper_default) {
            if (DeleteExtState) DeleteExtState(kExtSection, key.c_str(), true);
        } else if (SetExtState) {
            SetExtState(kExtSection, key.c_str(), kShortcutFollowsReaper, true);
        }
    } else if (reaper_default && g_links[static_cast<size_t>(id)].empty() && b.vk == 0) {
        if (SetExtState) SetExtState(kExtSection, key.c_str(), kShortcutOwnNoKey, true);
    } else if (!g_links[static_cast<size_t>(id)].empty()) {
        if (SetExtState) SetExtState(kExtSection, key.c_str(), EncodeActionLink(g_links[static_cast<size_t>(id)]).c_str(), true);
    } else if (b == kShortcutTable[id].def || b.vk == 0) {
        if (DeleteExtState) DeleteExtState(kExtSection, key.c_str(), true);
    } else if (SetExtState) {
        SetExtState(kExtSection, key.c_str(), EncodeShortcut(b).c_str(), true);
    }
}

}  // namespace

void LoadShortcuts()
{
    g_bindings = DefaultShortcutBindings();
    g_links = {};
    g_follow_reaper = {};
    bool bad[kShortcutCount] = {};
    bool stored[kShortcutCount] = {};  // 10-11: the key comes from the user's settings
    for (int i = 0; i < kShortcutCount; ++i)  // 13-61: a row whose default is REAPER starts on it
        g_follow_reaper[static_cast<size_t>(i)] = ShortcutDefaultFollowsReaper(i);
    for (int i = 0; i < kShortcutCount; ++i) {
        if (!GetExtState) break;
        const std::string key = ExtKey(i);
        const char* v = GetExtState(kExtSection, key.c_str());
        if (!v || !v[0]) continue;
        KeyBinding b;
        std::string link;
        if (ShortcutDefaultFollowsReaper(i) && std::strcmp(v, kShortcutOwnNoKey) == 0) {  // 13-61: Own, no key yet
            g_follow_reaper[static_cast<size_t>(i)] = false;
            g_bindings[static_cast<size_t>(i)] = KeyBinding{};
        } else if (std::strcmp(v, kShortcutFollowsReaper) == 0) {  // 13-60b: on REAPER, no key of its own
            g_follow_reaper[static_cast<size_t>(i)] = true;
            g_bindings[static_cast<size_t>(i)] = KeyBinding{};
        } else if (DecodeActionLink(v, &link)) {  // 13-60: linked to a REAPER action, no key of its own
            g_links[static_cast<size_t>(i)] = link;
            g_follow_reaper[static_cast<size_t>(i)] = false;
            g_bindings[static_cast<size_t>(i)] = KeyBinding{};
        } else if (DecodeShortcut(v, &b)) {
            g_bindings[static_cast<size_t>(i)] = b;
            g_follow_reaper[static_cast<size_t>(i)] = false;
            stored[i] = true;
        } else {
            LogWarn("shortcut \"%s\": stored key \"%s\" is not valid, using %s", kShortcutTable[i].label, v,
                    FormatShortcut(kShortcutTable[i].def, &LayoutKeyName).c_str());
            bad[i] = true;
        }
    }
    const std::array<bool, kShortcutCount> linked = LinkedFlags();
    std::vector<int> unbound;
    for (int i : SanitizeShortcuts(g_bindings, stored, &unbound, linked.data())) {
        LogWarn("shortcut \"%s\": stored key clashes with another action, using %s", kShortcutTable[i].label,
                FormatShortcut(kShortcutTable[i].def, &LayoutKeyName).c_str());
        Save(i);
    }
    // 10-11: a default another action's stored key holds waits, unbound and unsaved.
    for (int i : unbound) {
        int owner = -1;
        for (int j = 0; j < kShortcutCount && owner < 0; ++j)
            if (j != i && g_bindings[static_cast<size_t>(j)] == kShortcutTable[i].def) owner = j;
        LogInfo("shortcut \"%s\": its key %s is yours for \"%s\", so it has no key (set one in Shortcuts)",
                kShortcutTable[i].label, FormatShortcut(kShortcutTable[i].def, &LayoutKeyName).c_str(),
                owner >= 0 ? kShortcutTable[owner].label : "another action");
    }
    for (int i = 0; i < kShortcutCount; ++i)
        if (bad[i]) Save(i);  // the default now: the stale entry goes
    RefreshReaperListedKeys();  // 13-60: the keys REAPER lists for the linked actions (once its lookups are ready)
    g_keymap_dirty = true;
    RebuildKeymap();
}

const ShortcutBindings& CurrentShortcuts()
{
    return g_bindings;
}

const ShortcutKeymap& CurrentShortcutKeymap()
{
    return Keymap();
}

const std::string& ShortcutLink(int id)
{
    static const std::string kNone;
    return (id < 0 || id >= kShortcutCount) ? kNone : g_links[static_cast<size_t>(id)];
}

void LinkShortcutToAction(int id, const std::string& cmd)
{
    if (id < 0 || id >= kShortcutCount) return;
    const std::string n = NormalizeCommandId(cmd);
    if (n.empty()) return;
    g_links[static_cast<size_t>(id)] = n;
    g_follow_reaper[static_cast<size_t>(id)] = false;  // 13-60b: an action of the user's choice is Own
    g_bindings[static_cast<size_t>(id)] = KeyBinding{};  // no key of its own while linked
    Save(id);
    // Its own key is free now: an action waiting for that default gets it back.
    const std::array<bool, kShortcutCount> linked = LinkedFlags();
    for (int i : RebindFreedDefaults(g_bindings, linked.data())) Save(i);
    RefreshReaperListedKeys();  // 13-60: its keys in REAPER's own list too
    g_keymap_dirty = true;
    RebuildKeymap();
}

bool ShortcutOnReaperFallback(int id)
{
    if (id < 0 || id >= kShortcutCount) return false;
    Keymap();  // follows reaper-kb.ini
    return g_on_fallback[static_cast<size_t>(id)];
}

bool ShortcutFollowsReaper(int id)
{
    return id >= 0 && id < kShortcutCount && g_follow_reaper[static_cast<size_t>(id)];
}

void SetShortcutFollowsReaper(int id)
{
    if (id < 0 || id >= kShortcutCount || g_follow_reaper[static_cast<size_t>(id)]) return;
    g_follow_reaper[static_cast<size_t>(id)] = true;
    g_links[static_cast<size_t>(id)].clear();
    g_bindings[static_cast<size_t>(id)] = KeyBinding{};  // its own key is free: REAPER's keys run it now
    Save(id);
    const std::array<bool, kShortcutCount> linked = LinkedFlags();
    for (int i : RebindFreedDefaults(g_bindings, linked.data())) Save(i);
    RefreshReaperListedKeys();  // its keys in REAPER's own list
    g_keymap_dirty = true;
    RebuildKeymap();
}

int SetShortcutOwn(int id)
{
    if (id < 0 || id >= kShortcutCount || !g_follow_reaper[static_cast<size_t>(id)]) return -1;
    if (ShortcutDefaultFollowsReaper(id)) {  // 13-61: its default is REAPER: Own with no key yet
        g_follow_reaper[static_cast<size_t>(id)] = false;
        g_links[static_cast<size_t>(id)].clear();
        // 13-63: ...or on its fallback key (the split's B) when no other row holds it.
        const KeyBinding fb = ShortcutReaperFallback(id);
        g_bindings[static_cast<size_t>(id)] = (fb.vk != 0 && ShortcutOwner(g_bindings, fb, id) < 0) ? fb : KeyBinding{};
        Save(id);
        g_keymap_dirty = true;
        RebuildKeymap();
        return -1;
    }
    return ResetShortcutToDefault(id);  // ends REAPER: its default key back
}

std::string ShortcutFollowedAction(int id)
{
    return (id < 0 || id >= kShortcutCount) ? std::string() : Followed(id);
}

std::string ShortcutBindingLabel(const KeyBinding& b)
{
    return FormatShortcut(b, &LayoutKeyName);
}

int ShortcutActionForKey(unsigned key, bool ctrl, bool shift, bool alt, bool video_view, bool tagging_view,
                         bool library_edit)
{
    return ShortcutActionFor(Keymap(), key, ctrl, shift, alt, video_view, tagging_view, library_edit);
}

void RefreshShortcutLabels()
{
    g_keymap_dirty = true;  // 13-60: a character key of a linked action may be another key now
    RebuildKeymap();
}

const char* ShortcutKeyLabel(int id)
{
    if (id < 0 || id >= kShortcutCount) return "";
    Keymap();  // 13-60: follows reaper-kb.ini (the labels with it)
    return g_labels[id].c_str();
}

int ResetShortcutToDefault(int id)
{
    if (id < 0 || id >= kShortcutCount) return -1;
    if (ShortcutDefaultFollowsReaper(id)) {  // 13-61: its default is REAPER
        g_links[static_cast<size_t>(id)].clear();
        g_follow_reaper[static_cast<size_t>(id)] = false;  // so the call below runs
        SetShortcutFollowsReaper(id);
        return -1;
    }
    std::vector<int> changed;
    if (!g_links[static_cast<size_t>(id)].empty() || g_follow_reaper[static_cast<size_t>(id)]) {  // 13-60: back to a key of its own
        g_links[static_cast<size_t>(id)].clear();
        g_follow_reaper[static_cast<size_t>(id)] = false;  // 13-60b: Own again
        changed.push_back(id);
    }
    const int blocker = ShortcutResetBlocker(g_bindings, id);
    if (blocker >= 0) {  // the user's own key stays
        for (int i : changed) Save(i);
        g_keymap_dirty = true;
        RebuildKeymap();
        return blocker;
    }
    for (int i : ResetShortcut(g_bindings, id)) changed.push_back(i);
    changed.push_back(id);  // its entry goes even if it already held the default
    const std::array<bool, kShortcutCount> linked = LinkedFlags();
    for (int i : RebindFreedDefaults(g_bindings, linked.data())) changed.push_back(i);
    for (int i : changed) Save(i);
    g_keymap_dirty = true;
    RebuildKeymap();
    return -1;
}

void BeginShortcutRecording(int id)
{
    if (id < 0 || id >= kShortcutCount) return;
    CancelViewNavRecording();  // 13-45: one recording at a time
    g_recording_id = id;
    g_conflict_id = -1;
}

void CancelShortcutRecording()
{
    g_recording_id = -1;
    g_conflict_id = -1;
    CancelViewNavRecording();  // 13-45: a view row's recording too
}

int ShortcutRecordingId()
{
    if (g_recording_id >= 0) return g_recording_id;
    const int nav = ViewNavRecordingRow();  // 13-45: kShortcutCount + its row
    return nav >= 0 ? kShortcutCount + nav : -1;
}

const char* ShortcutRecordingConflict()
{
    return (g_recording_id >= 0 && g_conflict_id >= 0) ? kShortcutTable[g_conflict_id].label : nullptr;
}

void ShortcutRecordKey(unsigned key, bool ctrl, bool shift, bool alt)
{
    if (g_recording_id < 0 && ViewNavRecordingRow() >= 0) {  // 13-45: a view row records
        ViewNavRecordKey(key, ctrl, shift, alt);
        return;
    }
    if (g_recording_id < 0) return;
    int owner = -1;
    switch (ShortcutRecordStep(g_bindings, g_recording_id, key, ctrl, shift, alt, &owner)) {
    case RecordOutcome::Ignored:
        break;
    case RecordOutcome::Conflict:
        g_conflict_id = owner;
        break;
    case RecordOutcome::Cancelled:
        CancelShortcutRecording();
        break;
    case RecordOutcome::Assigned: {
        g_links[static_cast<size_t>(g_recording_id)].clear();  // 13-60: a key of its own ends the link
        g_follow_reaper[static_cast<size_t>(g_recording_id)] = false;  // 13-60b: ...and REAPER (Own now)
        Save(g_recording_id);
        const std::array<bool, kShortcutCount> linked = LinkedFlags();
        for (int i : RebindFreedDefaults(g_bindings, linked.data())) Save(i);  // its old key may be another's default
        g_keymap_dirty = true;
        RebuildKeymap();
        CancelShortcutRecording();
        break;
    }
    }
}

// ---- small persisted settings (spec 11-fb-11) ------------------------------------------

bool LoadPrefBool(const char* key, bool def)
{
    if (!key || !GetExtState) return def;
    return ParsePrefBool(GetExtState(kExtSection, key), def);
}

void SavePrefBool(const char* key, bool value)
{
    if (key && SetExtState) SetExtState(kExtSection, key, value ? "1" : "0", true);
}

float LoadPrefFloat(const char* key, float def)
{
    if (!key || !GetExtState) return def;
    return ParsePrefFloat(GetExtState(kExtSection, key), def);
}

void SavePrefFloat(const char* key, float value)
{
    if (!key || !SetExtState) return;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(value));
    SetExtState(kExtSection, key, buf, true);
}

std::string LoadPrefString(const char* key)
{
    if (!key || !GetExtState) return std::string();
    const char* v = GetExtState(kExtSection, key);
    return v ? std::string(v) : std::string();
}

void SavePrefString(const char* key, const std::string& value)
{
    if (key && SetExtState) SetExtState(kExtSection, key, value.c_str(), true);
}

}  // namespace rav
