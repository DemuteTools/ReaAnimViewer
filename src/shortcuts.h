// SPDX-License-Identifier: MIT
//
// The viewer's keyboard shortcuts (spec 11-fb-3): one table (id, label, default key,
// context) drives the key handling, the accelerator hook, the Shortcuts popup and the
// key caps drawn in the Video view. A binding is one virtual key plus optional
// Ctrl / Shift / Alt, and it matches only with exactly those modifiers held, so every
// other key (Space, ...) stays REAPER's.
//
// The top half is pure (no REAPER, no Windows): the table, the matching rule, the
// record / conflict / reset rules, the ExtState encoding and the key names. The host
// tests (tests/shortcuts_test.cpp) include it as is. The bottom half is the live state
// (src/shortcuts.cpp): the current bindings, REAPER ExtState, the recording session.

#pragma once

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace rav {

// ---- virtual keys (the Windows VK_* values, spelled here so this half stays Windows-free)

namespace vk {
constexpr unsigned kBack = 0x08, kTab = 0x09, kReturn = 0x0D, kShift = 0x10, kControl = 0x11,
                   kMenu = 0x12, kPause = 0x13, kCapital = 0x14, kEscape = 0x1B, kSpace = 0x20,
                   kPrior = 0x21, kNext = 0x22, kEnd = 0x23, kHome = 0x24, kLeft = 0x25, kUp = 0x26,
                   kRight = 0x27, kDown = 0x28, kInsert = 0x2D, kDelete = 0x2E, kLWin = 0x5B,
                   kRWin = 0x5C, kApps = 0x5D, kNumpad0 = 0x60, kMultiply = 0x6A, kAdd = 0x6B,
                   kSeparator = 0x6C, kSubtract = 0x6D, kDecimal = 0x6E, kDivide = 0x6F, kF1 = 0x70,
                   kF24 = 0x87, kNumLock = 0x90, kScroll = 0x91, kLShift = 0xA0, kRMenu = 0xA5,
                   kProcessKey = 0xE5, kPacket = 0xE7;
}  // namespace vk

// ---- the table -----------------------------------------------------------------------

enum class ShortcutContext {
    Anywhere,   // the viewer has the focus
    VideoView,  // ...and Video view is on (in RAV view the key stays REAPER's)
};

struct KeyBinding {
    unsigned vk = 0;  // Windows virtual-key code (0 = none)
    bool ctrl  = false;
    bool shift = false;
    bool alt   = false;
};

inline bool operator==(const KeyBinding& a, const KeyBinding& b)
{
    return a.vk == b.vk && a.ctrl == b.ctrl && a.shift == b.shift && a.alt == b.alt;
}
inline bool operator!=(const KeyBinding& a, const KeyBinding& b) { return !(a == b); }

struct ShortcutDef {
    const char*     id;     // ExtState key suffix: shortcut.<id>
    const char*     label;  // shown in the popup and in "Already used by <label>"
    KeyBinding      def;    // the default key
    ShortcutContext context;
};

enum ShortcutId : int {
    kShortcutToggleView  = 0,  // RAV view / Video view
    kShortcutTogglePanel = 1,  // the Video panel (from RAV view it opens Video view with it)
    kShortcutCut         = 2,  // cut at the playhead (Video view only)
    kShortcutDeleteShot  = 3,  // delete the current shot, after a confirmation (Video view only, spec 11-fb-11)
    kShortcutCount       = 4,
};

inline const ShortcutDef kShortcutTable[kShortcutCount] = {
    {"toggle_view",  "Toggle RAV view / Video view", {'V', false, false, false}, ShortcutContext::Anywhere},
    {"toggle_panel", "Show / hide the Video panel",  {'P', false, false, false}, ShortcutContext::Anywhere},
    {"cut",          "Cut at playhead",              {'C', false, false, false}, ShortcutContext::VideoView},
    {"delete_shot",  "Delete current shot",          {vk::kDelete, false, false, false}, ShortcutContext::VideoView},
};

// The mouse gestures the popup lists (fixed, never rebindable), by group.
struct MouseGestureGroup {
    const char* title;   // "MOUSE"
    const char* note;    // faint, next to the title (or nullptr)
    const char* footer;  // faint, under the group's rows (or nullptr)
};

inline const MouseGestureGroup kMouseGestureGroups[] = {
    {"MOUSE", nullptr, "In Video view these gestures edit the shot under the playhead."},
    {"SHOT STRIP", "Video view", nullptr},
};

struct MouseGestureDef {
    int         group;    // index in kMouseGestureGroups
    const char* action;
    const char* gesture;
};

inline const MouseGestureDef kMouseGestureTable[] = {
    {0, "Orbit", "Right-drag"},
    {0, "Pan", "Middle-drag"},
    {0, "Zoom", "Wheel"},
    {0, "Menu, buttons, nav cube", "Left-click"},
    {1, "Move the playhead", "Click / drag"},
    {1, "Retime a cut", "Drag a line between shots"},
    {1, "Switch Cut / Move", "Middle-click a shot"},
    {1, "Resize the strip", "Drag its top edge"},
};

using ShortcutBindings = std::array<KeyBinding, kShortcutCount>;

inline ShortcutBindings DefaultShortcutBindings()
{
    ShortcutBindings b;
    for (int i = 0; i < kShortcutCount; ++i) b[static_cast<size_t>(i)] = kShortcutTable[i].def;
    return b;
}


// A key that only modifies others (or toggles a lock): never recorded on its own.
inline bool IsModifierKey(unsigned k)
{
    return k == vk::kShift || k == vk::kControl || k == vk::kMenu || (k >= vk::kLShift && k <= vk::kRMenu) ||
           k == vk::kLWin || k == vk::kRWin || k == vk::kCapital || k == vk::kNumLock || k == vk::kScroll;
}

// Can this key be a binding? Not a mouse button, not a modifier, not Esc (it cancels).
inline bool IsBindableKey(unsigned k)
{
    if (k == 0 || k > 0xFE) return false;  // 0xFF: no key (a layout's "none")
    if (k == 0x01 || k == 0x02 || k == 0x04 || k == 0x05 || k == 0x06) return false;  // mouse buttons
    if (k == 0x03) return false;  // VK_CANCEL (Ctrl+Break)
    if (k == vk::kProcessKey || k == vk::kPacket) return false;  // IME / injected Unicode, not a key
    if (k == vk::kEscape) return false;
    return !IsModifierKey(k);
}

// ---- matching ------------------------------------------------------------------------

// True when vk, with exactly these modifiers held, is the binding.
inline bool ShortcutMatches(const KeyBinding& b, unsigned key, bool ctrl, bool shift, bool alt)
{
    return b.vk != 0 && b.vk == key && b.ctrl == ctrl && b.shift == shift && b.alt == alt;
}

// The action this key press runs, or -1. An action whose context is Video view only
// matches in Video view.
inline int ShortcutActionFor(const ShortcutBindings& b, unsigned key, bool ctrl, bool shift, bool alt,
                             bool video_view)
{
    for (int i = 0; i < kShortcutCount; ++i) {
        if (kShortcutTable[i].context == ShortcutContext::VideoView && !video_view) continue;
        if (ShortcutMatches(b[static_cast<size_t>(i)], key, ctrl, shift, alt)) return i;
    }
    return -1;
}

// The action other than `except` bound to this key (whatever its context), or -1.
inline int ShortcutOwner(const ShortcutBindings& b, const KeyBinding& key, int except)
{
    for (int i = 0; i < kShortcutCount; ++i)
        if (i != except && b[static_cast<size_t>(i)] == key) return i;
    return -1;
}

// ---- key routing (the accelerator hook's decision) -------------------------------------

// What the hook says to REAPER: keep the key (0), or hand it to the viewer window (-1;
// -20 for WM_SYSKEY* / WM_SYSCHAR, which REAPER would otherwise drop: the Alt keys).
constexpr int kRouteReaper = 0, kRouteViewer = -1, kRouteViewerSys = -20;

enum class KeyMsg { KeyDown, SysKeyDown, KeyUp, SysKeyUp, Char, SysChar, DeadChar, SysDeadChar, Wheel, Other };

inline bool IsSysKeyMsg(KeyMsg m)
{
    return m == KeyMsg::SysKeyDown || m == KeyMsg::SysKeyUp || m == KeyMsg::SysChar || m == KeyMsg::SysDeadChar;
}

struct KeyRouteInput {
    KeyMsg   msg = KeyMsg::Other;
    unsigned key = 0;         // wParam (the virtual key for key down / up)
    bool     repeat = false;  // key down auto-repeat (lParam bit 30)
    bool     ctrl = false, shift = false, alt = false;
    bool     video_view = false;
    bool     recording = false;   // a new key is being recorded: the viewer takes every key
    bool     text_input = false;  // an ImGui text field is active: the viewer takes every key
    bool     popup_open = false;  // the Shortcuts popup (or the delete confirmation) is open: Esc closes it
    bool     confirm_open = false;  // the delete confirmation is open: Enter confirms it (spec 11-fb-11)
    bool     imgui_mouse = false;   // the viewer's UI wants the mouse (spec 11-fb-12: Alt + wheel zooms the strip)
};

struct KeyRouteState {
    unsigned claimed_vk = 0;   // the key the viewer took: its characters and release follow it
    unsigned released_vk = 0;  // its WM_SYSKEYUP was sent to the viewer: the window swallows it
    bool     alt_wheel_taken = false;  // spec 11-fb-12: Alt + wheel went to the UI since Alt went down
};

// Which keys reach the viewer, and how the claim follows a taken key to its release.
inline int RouteViewerKey(const ShortcutBindings& b, const KeyRouteInput& in, KeyRouteState& st)
{
    const int to_viewer = IsSysKeyMsg(in.msg) ? kRouteViewerSys : kRouteViewer;
    const bool down = in.msg == KeyMsg::KeyDown || in.msg == KeyMsg::SysKeyDown;
    const bool up = in.msg == KeyMsg::KeyUp || in.msg == KeyMsg::SysKeyUp;
    // Spec 11-fb-12 -- Alt + wheel over the viewer's UI (the shot strip's zoom) is the viewer's,
    // never a REAPER mouse-wheel action, and Alt's release after it is the viewer's too (the
    // window swallows it: REAPER's menu bar does not open). Alt's next press starts afresh.
    // Spec 11-fb-14: Shift + wheel over the UI (the strip's scroll) is the viewer's too (Shift's
    // release opens no menu: nothing to swallow).
    if (in.msg == KeyMsg::Wheel) {
        if (in.alt && in.imgui_mouse) {
            st.alt_wheel_taken = true;
            return kRouteViewer;
        }
        if (in.shift && in.imgui_mouse) return kRouteViewer;
        return (in.recording || in.text_input) ? kRouteViewer : kRouteReaper;
    }
    if (down && in.key == vk::kMenu && !in.repeat) st.alt_wheel_taken = false;
    if (up && in.key == vk::kMenu && st.alt_wheel_taken && !in.recording) {
        st.alt_wheel_taken = false;
        if (in.msg == KeyMsg::SysKeyUp) {
            st.released_vk = vk::kMenu;
            return kRouteViewerSys;
        }
        return kRouteViewer;
    }
    if (in.recording) {
        // The record modal takes every key; the one it records keeps its character and its
        // release once recording ends.
        if (down) st.claimed_vk = in.key;
        if (in.msg == KeyMsg::SysKeyUp) st.released_vk = in.key;
        return to_viewer;
    }
    // Spec 11-fb-17 -- an active text field takes every key, with its characters and release:
    // Ctrl (and Ctrl+A / C / V / X / Z), Home / End, Shift + arrows and the rest of the editing
    // keys, which ImGui's InputText handles itself; Space types a space instead of playing.
    if (in.text_input) return to_viewer;
    if (down) {
        st.released_vk = 0;
        // An auto-repeat of the key we took stays ours, even if it no longer maps to an
        // action (e.g. the key just recorded for a Video view only action, in RAV view).
        if (in.repeat && st.claimed_vk != 0 && in.key == st.claimed_vk) return to_viewer;
        const bool ours = (in.key == vk::kEscape && in.popup_open) || (in.key == vk::kReturn && in.confirm_open) ||
                          ShortcutActionFor(b, in.key, in.ctrl, in.shift, in.alt, in.video_view) >= 0;
        if (!ours) {
            st.claimed_vk = 0;  // a lost release (focus moved) must not keep claiming characters
            return kRouteReaper;
        }
        st.claimed_vk = in.key;
        return to_viewer;
    }
    if (up) {
        // The release of the key we took (its modifiers may already be up) follows it.
        if (st.claimed_vk == 0 || in.key != st.claimed_vk) return kRouteReaper;
        st.claimed_vk = 0;
        if (in.msg == KeyMsg::SysKeyUp) st.released_vk = in.key;
        return to_viewer;
    }
    if (in.msg == KeyMsg::Char || in.msg == KeyMsg::SysChar || in.msg == KeyMsg::DeadChar ||
        in.msg == KeyMsg::SysDeadChar)
        return st.claimed_vk != 0 ? to_viewer : kRouteReaper;  // the character of the key we took
    return kRouteReaper;
}

// Spec 11-fb-17 -- whether a WM_CHAR / WM_SYSCHAR / WM_DEADCHAR reaching the viewer window is
// the viewer's: the character of a key it took (claimed_vk, still set until that key's
// release), any character while a new key is recorded, or while an ImGui text field is
// active. The window then answers it itself (ImGui already has the character) and never lets
// it reach DefWindowProc; other characters keep their old path.
inline bool ViewerTakesChar(const KeyRouteState& st, bool recording, bool text_input)
{
    return recording || text_input || st.claimed_vk != 0;
}

// ---- recording -----------------------------------------------------------------------

enum class RecordOutcome {
    Ignored,    // a modifier on its own (or an unbindable key): still waiting
    Cancelled,  // Esc: the binding is unchanged
    Conflict,   // the key belongs to another action: refused, still waiting
    Assigned,   // the key is now the action's binding
};

// One key down while recording action `id`. On Conflict, *owner is the other action.
inline RecordOutcome ShortcutRecordStep(ShortcutBindings& b, int id, unsigned key, bool ctrl, bool shift,
                                        bool alt, int* owner = nullptr)
{
    if (owner) *owner = -1;
    if (key == vk::kEscape) return RecordOutcome::Cancelled;
    if (id < 0 || id >= kShortcutCount || !IsBindableKey(key)) return RecordOutcome::Ignored;
    const KeyBinding nb{key, ctrl, shift, alt};
    const int other = ShortcutOwner(b, nb, id);
    if (other >= 0) {
        if (owner) *owner = other;
        return RecordOutcome::Conflict;
    }
    b[static_cast<size_t>(id)] = nb;
    return RecordOutcome::Assigned;
}

// Restores action `id` to its default. An action that holds that default now is reset
// too (and so on), so two actions never share a key. Returns every action that changed.
inline std::vector<int> ResetShortcut(ShortcutBindings& b, int id)
{
    std::vector<int> changed;
    if (id < 0 || id >= kShortcutCount) return changed;
    std::vector<int> todo{id};
    for (int guard = 0; !todo.empty() && guard < 4 * kShortcutCount; ++guard) {
        const int k = todo.back();
        todo.pop_back();
        const KeyBinding& d = kShortcutTable[k].def;
        if (b[static_cast<size_t>(k)] != d) {
            b[static_cast<size_t>(k)] = d;
            changed.push_back(k);
        }
        for (int j = 0; j < kShortcutCount; ++j)
            if (j != k && b[static_cast<size_t>(j)] == d) todo.push_back(j);
    }
    return changed;
}

// Makes a set of bindings valid (each bindable, no key shared), resetting what is not.
// Returns the actions it reset. Used on load, where the stored values may be stale.
inline std::vector<int> SanitizeShortcuts(ShortcutBindings& b)
{
    std::vector<int> reset;
    auto note = [&reset](const std::vector<int>& v) {
        for (int x : v)
            if (std::find(reset.begin(), reset.end(), x) == reset.end()) reset.push_back(x);
    };
    for (int i = 0; i < kShortcutCount; ++i)
        if (!IsBindableKey(b[static_cast<size_t>(i)].vk)) note(ResetShortcut(b, i));
    for (int pass = 0; pass < kShortcutCount; ++pass) {
        bool clash = false;
        for (int i = 0; i < kShortcutCount && !clash; ++i) {
            const int j = ShortcutOwner(b, b[static_cast<size_t>(i)], i);
            if (j < 0) continue;
            // Reset the custom one (the later one when both are custom).
            const int victim = (b[static_cast<size_t>(j)] == kShortcutTable[j].def) ? i : std::max(i, j);
            note(ResetShortcut(b, victim));
            clash = true;
        }
        if (!clash) break;
    }
    return reset;
}

// ---- ExtState encoding: "Ctrl+Shift+Alt+<decimal vk>", modifiers optional ---------------

inline std::string EncodeShortcut(const KeyBinding& b)
{
    std::string s;
    if (b.ctrl) s += "Ctrl+";
    if (b.shift) s += "Shift+";
    if (b.alt) s += "Alt+";
    s += std::to_string(b.vk);
    return s;
}

// False when the text is not a valid binding (then *out is untouched).
inline bool DecodeShortcut(const char* text, KeyBinding* out)
{
    if (!text || !out) return false;
    KeyBinding b;
    const char* p = text;
    for (;;) {
        if (std::strncmp(p, "Ctrl+", 5) == 0 && !b.ctrl) { b.ctrl = true; p += 5; }
        else if (std::strncmp(p, "Shift+", 6) == 0 && !b.shift) { b.shift = true; p += 6; }
        else if (std::strncmp(p, "Alt+", 4) == 0 && !b.alt) { b.alt = true; p += 4; }
        else break;
    }
    if (*p < '0' || *p > '9') return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(p, &end, 10);
    if (!end || *end != '\0' || v > 0xFE) return false;
    b.vk = static_cast<unsigned>(v);
    if (!IsBindableKey(b.vk)) return false;
    *out = b;
    return true;
}

// ---- persisted settings: the ExtState text of a bool / a float (spec 11-fb-11) -----------

// "1" / "0"; anything else (missing, empty, "true", "10") gives `def`.
inline bool ParsePrefBool(const char* text, bool def)
{
    if (!text) return def;
    if (text[0] == '1' && text[1] == '\0') return true;
    if (text[0] == '0' && text[1] == '\0') return false;
    return def;
}

// A plain decimal number ("123.0", as SavePrefFloat writes it), within +-1e6; anything else
// (missing, empty, trailing text, nan / inf, out of range) gives `def`.
inline float ParsePrefFloat(const char* text, float def)
{
    if (!text || !text[0]) return def;
    char* end = nullptr;
    const double d = std::strtod(text, &end);
    if (!end || end == text || *end != '\0') return def;
    if (!(d == d) || d > 1.0e6 || d < -1.0e6) return def;  // NaN, inf, absurd values
    return static_cast<float>(d);
}

// ---- key names -----------------------------------------------------------------------

// The name of a key: letters, digits, F1-F24, arrows, Space, Enter, Tab, Backspace,
// Delete, Insert, Home, End, PageUp/PageDown, numpad. Any other key asks `fallback`
// (the keyboard layout's own name, GetKeyNameTextW); without one, "Key 0xNN".
inline std::string ShortcutKeyName(unsigned k, std::string (*fallback)(unsigned) = nullptr)
{
    char buf[32];
    if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9')) return std::string(1, static_cast<char>(k));
    if (k >= vk::kF1 && k <= vk::kF24) {
        std::snprintf(buf, sizeof(buf), "F%u", k - vk::kF1 + 1);
        return buf;
    }
    if (k >= vk::kNumpad0 && k <= vk::kNumpad0 + 9) {
        std::snprintf(buf, sizeof(buf), "Num %u", k - vk::kNumpad0);
        return buf;
    }
    switch (k) {
    case vk::kLeft:     return "Left";
    case vk::kUp:       return "Up";
    case vk::kRight:    return "Right";
    case vk::kDown:     return "Down";
    case vk::kSpace:    return "Space";
    case vk::kReturn:   return "Enter";
    case vk::kTab:      return "Tab";
    case vk::kBack:     return "Backspace";
    case vk::kDelete:   return "Delete";
    case vk::kInsert:   return "Insert";
    case vk::kHome:     return "Home";
    case vk::kEnd:      return "End";
    case vk::kPrior:    return "PageUp";
    case vk::kNext:     return "PageDown";
    case vk::kMultiply: return "Num *";
    case vk::kAdd:      return "Num +";
    case vk::kSubtract: return "Num -";
    case vk::kDecimal:  return "Num .";
    case vk::kDivide:   return "Num /";
    default: break;
    }
    if (fallback) {
        std::string s = fallback(k);
        if (!s.empty()) return s;
    }
    std::snprintf(buf, sizeof(buf), "Key 0x%02X", k);
    return buf;
}

// "Ctrl+Shift+Alt+K" (modifiers only when held).
inline std::string FormatShortcut(const KeyBinding& b, std::string (*fallback)(unsigned) = nullptr)
{
    std::string s;
    if (b.ctrl) s += "Ctrl+";
    if (b.shift) s += "Shift+";
    if (b.alt) s += "Alt+";
    return s + ShortcutKeyName(b.vk, fallback);
}

// ---- live state (src/shortcuts.cpp, REAPER + Windows) ----------------------------------

// Reads the custom keys from REAPER ExtState (section ReaAnimViewer, key shortcut.<id>).
// A bad stored value falls back to the default with a warning. Call once at load.
void LoadShortcuts();

const ShortcutBindings& CurrentShortcuts();

// The action a key press runs in the viewer (Ctrl / Shift / Alt held as given), or -1.
int ShortcutActionForKey(unsigned key, bool ctrl, bool shift, bool alt, bool video_view);

// The current key of an action as text ("C", "Shift+V"), for key caps and the popup.
const char* ShortcutKeyLabel(int id);

// Re-reads the key names from the keyboard layout (after a layout switch).
void RefreshShortcutLabels();

// Right-click in the popup: back to the default (and whatever held it), ExtState cleared.
void ResetShortcutToDefault(int id);

// Recording (double-click in the popup): the next non-modifier key down becomes the
// binding. While it runs, the accelerator hook passes every key to the viewer.
void BeginShortcutRecording(int id);
void CancelShortcutRecording();  // also on focus loss / parking
int  ShortcutRecordingId();                // -1 when not recording
const char* ShortcutRecordingConflict();   // the label of the action that holds the last key, or nullptr
// A key down while recording (always consumed). Esc cancels.
void ShortcutRecordKey(unsigned key, bool ctrl, bool shift, bool alt);

// ---- small persisted settings (spec 11-fb-11) ------------------------------------------
// One persistence path for the viewer's own settings: REAPER ExtState, the same section as
// the keys (ReaAnimViewer), kept across sessions. A missing or unreadable value gives `def`.
bool  LoadPrefBool(const char* key, bool def);
void  SavePrefBool(const char* key, bool value);
float LoadPrefFloat(const char* key, float def);
void  SavePrefFloat(const char* key, float value);

}  // namespace rav
