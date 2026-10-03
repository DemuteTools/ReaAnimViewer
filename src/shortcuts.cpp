// SPDX-License-Identifier: MIT
//
// See shortcuts.h -- the live half: the current bindings, REAPER ExtState, the
// recording session and the key names of the user's keyboard layout.

#include "shortcuts.h"

#include <windows.h>

#include "console_log.h"
#include "reaper_api.h"

namespace rav {
namespace {

constexpr char kExtSection[] = "ReaAnimViewer";

ShortcutBindings g_bindings = DefaultShortcutBindings();
std::string      g_labels[kShortcutCount];
int              g_recording_id = -1;
int              g_conflict_id  = -1;

std::string ExtKey(int id)
{
    return std::string("shortcut.") + kShortcutTable[id].id;
}

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

void RefreshLabels()
{
    for (int i = 0; i < kShortcutCount; ++i)
        g_labels[i] = FormatShortcut(g_bindings[static_cast<size_t>(i)], &LayoutKeyName);
}

// Only custom keys are stored: a default deletes the entry.
void Save(int id)
{
    if (id < 0 || id >= kShortcutCount) return;
    const KeyBinding& b = g_bindings[static_cast<size_t>(id)];
    const std::string key = ExtKey(id);
    if (b == kShortcutTable[id].def) {
        if (DeleteExtState) DeleteExtState(kExtSection, key.c_str(), true);
    } else if (SetExtState) {
        SetExtState(kExtSection, key.c_str(), EncodeShortcut(b).c_str(), true);
    }
}

}  // namespace

void LoadShortcuts()
{
    g_bindings = DefaultShortcutBindings();
    bool bad[kShortcutCount] = {};
    for (int i = 0; i < kShortcutCount; ++i) {
        if (!GetExtState) break;
        const std::string key = ExtKey(i);
        const char* v = GetExtState(kExtSection, key.c_str());
        if (!v || !v[0]) continue;
        KeyBinding b;
        if (DecodeShortcut(v, &b)) {
            g_bindings[static_cast<size_t>(i)] = b;
        } else {
            LogWarn("shortcut \"%s\": stored key \"%s\" is not valid, using %s", kShortcutTable[i].label, v,
                    FormatShortcut(kShortcutTable[i].def, &LayoutKeyName).c_str());
            bad[i] = true;
        }
    }
    for (int i : SanitizeShortcuts(g_bindings)) {
        LogWarn("shortcut \"%s\": stored key clashes with another action, using %s", kShortcutTable[i].label,
                FormatShortcut(kShortcutTable[i].def, &LayoutKeyName).c_str());
        Save(i);
    }
    for (int i = 0; i < kShortcutCount; ++i)
        if (bad[i]) Save(i);  // the default now: the stale entry goes
    RefreshLabels();
}

const ShortcutBindings& CurrentShortcuts()
{
    return g_bindings;
}

int ShortcutActionForKey(unsigned key, bool ctrl, bool shift, bool alt, bool video_view)
{
    return ShortcutActionFor(g_bindings, key, ctrl, shift, alt, video_view);
}

void RefreshShortcutLabels()
{
    RefreshLabels();
}

const char* ShortcutKeyLabel(int id)
{
    if (id < 0 || id >= kShortcutCount) return "";
    if (g_labels[id].empty()) RefreshLabels();
    return g_labels[id].c_str();
}

void ResetShortcutToDefault(int id)
{
    if (id < 0 || id >= kShortcutCount) return;
    std::vector<int> changed = ResetShortcut(g_bindings, id);
    changed.push_back(id);  // its entry goes even if it already held the default
    for (int i : changed) Save(i);
    RefreshLabels();
}

void BeginShortcutRecording(int id)
{
    if (id < 0 || id >= kShortcutCount) return;
    g_recording_id = id;
    g_conflict_id = -1;
}

void CancelShortcutRecording()
{
    g_recording_id = -1;
    g_conflict_id = -1;
}

int ShortcutRecordingId()
{
    return g_recording_id;
}

const char* ShortcutRecordingConflict()
{
    return (g_recording_id >= 0 && g_conflict_id >= 0) ? kShortcutTable[g_conflict_id].label : nullptr;
}

void ShortcutRecordKey(unsigned key, bool ctrl, bool shift, bool alt)
{
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
    case RecordOutcome::Assigned:
        Save(g_recording_id);
        RefreshLabels();
        CancelShortcutRecording();
        break;
    }
}

}  // namespace rav
