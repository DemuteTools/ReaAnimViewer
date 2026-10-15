// SPDX-License-Identifier: MIT
//
// 13-64 -- what already uses a key: the reverse index of the Shortcuts window. On REAPER's side, every
// action of the Main section with the keys and wheels REAPER gives it (its own list, its built-in
// defaults included, and the user's KEY lines of reaper-kb.ini); on RAV's side, the Shortcuts rows that
// run on a gesture now, the keys the Sound Edit window keeps for itself and the fixed mouse gestures.
// It feeds Find by key and the Link modal's search (by name, command ID or shortcut, typed or pressed).
// Pure (no REAPER, no Windows, no ImGui): host-tested by tests/shortcut_key_index_test.cpp. The window
// builds it once when it opens and again when ReaperKbGeneration() moves, never per frame.

#pragma once

#include <string>
#include <vector>

#include "reaper_view_keys.h"
#include "shortcuts.h"
#include "view_nav.h"

namespace rav {

// ---- REAPER's side ---------------------------------------------------------------------------

// One Main action as REAPER gives it: its id (reaper-kb.ini's form, "40757", "_SWS_ABOUT"), its name,
// and the shortcuts REAPER lists for it (GetActionShortcutDesc: "Ctrl+S", "Shift+Mousewheel", MIDI...).
struct KeyIndexSource {
    std::string              id;
    std::string              name;
    std::vector<std::string> shortcuts;
};

// One action in the index: its key and wheel gestures (each once), and REAPER's words for them.
struct KeyIndexAction {
    std::string              id;
    std::string              name;
    std::vector<KbGesture>   gestures;
    std::vector<std::string> descs;  // the listed shortcuts (any kind), for the text search
};

// A key gesture of a character ("+", "ù") as the key typing it on this keyboard layout (the window
// passes shortcuts.h KeyBindingOfGesture); false drops the gesture. nullptr: characters stay as they are.
using KbGestureNormalizer = bool (*)(KbGesture* g);

struct ShortcutKeyIndex {
    std::vector<KeyIndexAction> actions;  // REAPER's order
    // The actions REAPER runs on exactly this gesture (indices into `actions`, in order).
    std::vector<int> ActionsOn(const KbGesture& g) const;

    struct Entry {
        KbGesture g;
        int       action = -1;
    };
    std::vector<Entry> by_gesture;  // sorted by gesture (KbGestureLess), then by action
};

// A total order on gestures (the index's sort).
bool KbGestureLess(const KbGesture& a, const KbGesture& b);

// Builds the index. Each source's listed shortcuts are read with ParseReaperShortcutDesc (what it cannot
// read -- MIDI, OSC, a trackpad's gestures, the Win key -- is skipped as a gesture, kept as text). `kb`
// (optional): reaper-kb.ini as read; an action its KEY lines name gets the gestures KbGesturesOf gives
// it too (the user's keys, before REAPER lists them). `norm` (optional): see KbGestureNormalizer.
ShortcutKeyIndex BuildShortcutKeyIndex(const std::vector<KeyIndexSource>& src, const ViewKeymap* kb = nullptr,
                                       KbGestureNormalizer norm = nullptr);

// ---- the Link modal's search -----------------------------------------------------------------

// A typed text that reads as a shortcut: "ctrl+s", "Shift+Wheel", "F5", "space", "alt+h-wheel" (D7). A
// single character alone ("s") is a word of a name, not a shortcut.
bool ParseTypedShortcut(const std::string& text, KbGesture* out, KbGestureNormalizer norm = nullptr);

// The search: every word of `query` in the action's name or id (action_link.h ActionSearchMatch), or the
// text read as a shortcut (`typed`, from ParseTypedShortcut: nullptr when it does not read as one) is
// one of its gestures, or the text with a "+" starts one of REAPER's words for its shortcuts ("ctrl+sh"
// while typing "Ctrl+Shift+S"). An empty query matches every action.
bool LinkSearchMatch(const KeyIndexAction& a, const std::string& query, const KbGesture* typed);

// ---- RAV's side ----------------------------------------------------------------------------

// The keys the Sound Edit window keeps for itself while it has the focus (D6: shown "fixed").
struct EditReservedKey {
    unsigned    vk;
    int         mods;  // kViewMod* bits
    const char* label;
};
inline const EditReservedKey kEditReservedKeys[] = {
    {vk::kSpace, 0, "Play / stop"},
    {vk::kSpace, kViewModShift, "Play the selected part"},
    {vk::kEscape, 0, "Clear the selection, stop or close"},
    {'E', 0, "Close the Edit window"},
};
constexpr int kEditReservedCount = static_cast<int>(sizeof(kEditReservedKeys) / sizeof(kEditReservedKeys[0]));

// One RAV use of a gesture.
struct RavKeyUse {
    enum class Kind { KeyRow, ViewRow, EditReserved, Mouse };
    Kind kind = Kind::KeyRow;
    int  index = -1;        // kShortcutTable / kViewNavTable / kEditReservedKeys / kMouseGestureTable
    bool shadowed = false;  // KeyRow: listed for the row, but another row runs it (ShortcutKeymap::shadowed)
};

// The gesture as a KEYS binding (a key, not a character); false for the wheels.
bool KeyBindingOfKbGesture(const KbGesture& g, KeyBinding* out);
// A KEYS binding as a gesture.
KbGesture KbGestureOfBinding(const KeyBinding& b);

// Every RAV use of `g`: the KEYS rows whose keys in force hold it (then those that list it shadowed),
// the view rows it runs (ViewNavMap: the wheels by modifiers, the key gestures), the Sound Edit
// window's reserved keys and the fixed mouse gestures (the plain wheel's Zoom). `char_view_row` (>= 0):
// a view row the key runs through a character binding (the caller's layout: ViewNavRowForKeyDown).
std::vector<RavKeyUse> RavUsesOf(const ShortcutKeymap& keys, const ViewNavMap& nav, const KbGesture& g,
                                 int char_view_row = -1);

}  // namespace rav
