// SPDX-License-Identifier: MIT
//
// Story 13-7b (Edit overlay zoom), 13-45 (every timeline view): the user's own REAPER view
// bindings, read from <resource path>/reaper-kb.ini, so RAV's timelines zoom and scroll like his
// arrange view. Pure (no REAPER, no ImGui): host-tested by tests/reaper_view_keys_test.cpp.
//
// reaper-kb.ini lines used: "KEY <mod> <key> <command> <section>", Main section (0) only.
//  - <mod> 255 = a special input. <key>: the high nibble is the input (0xF0 the mouse wheel, 0xD0
//    the horizontal wheel; others -- MultiZoom, MultiRotate... -- ignored), the low bits the
//    modifiers: 1 Ctrl, 2 Alt, 4 Shift, and 8 CLEAR = the Win key held. So 248..255 = the wheel
//    (+ Ctrl / Alt / Shift), 216..223 = the horizontal wheel; 240..247 / 208..215 = with Win
//    (never matched here). Read from REAPER 7.81's own decoder and default table (13-45): its
//    defaults are 248 -> 990, 249 -> 1000, 250 -> 988, 251 -> 989, 254 -> 976, 216 -> 988,
//    218 -> 990. (13-7b also read 120 / 88 + bits as an "older" wheel: REAPER does not, removed.)
//    Command 0 = the default binding disabled.
//  - otherwise <mod> is the Win32 accelerator flags: 1 = virtual key (FVIRTKEY), 4 Shift,
//    8 Ctrl, 16 Alt, 32 Win; <key> is a virtual-key code (odd <mod>) or a character (even <mod>).
// Commands mapped (REAPER's Main action list, ids checked in REAPER 7.81's action table):
//   988 / 977  View: Scroll horizontally [reversed] (MIDI CC relative/mousewheel)
//   981 / 975  View: Scroll view horizontally one page [reversed] (MIDI CC relative/mousewheel)
//   989 / 978  View: Scroll vertically [reversed] (MIDI CC relative/mousewheel)
//   976 / 982  View: Scroll view vertically one page [reversed] (MIDI CC relative/mousewheel)
//   990 / 979  View: Zoom horizontally [reversed] (MIDI CC relative/mousewheel)
//   1000 / 1001, 991 / 980  View: Zoom vertically [reversed][, snap to theme-defined sizes] (...)
//   1012 View: Zoom in horizontal, 1011 View: Zoom out horizontal (keys)
// Defaults when the file binds nothing (REAPER 7's): wheel and Alt+horizontal wheel = zoom
// horizontally around the mouse, Alt+wheel and the horizontal wheel = scroll horizontally,
// Ctrl+wheel = zoom vertically, Ctrl+Alt+wheel = scroll vertically, Alt+Shift+wheel = scroll
// vertically one page; keys: + (keypad), =, + (Shift+=) and Up zoom in, - (keypad), - and Down
// zoom out. A key binding with the Win key (<mod> bit 32) is skipped: never matched here.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace rav {

enum class ViewWheelAction { None, ZoomH, ScrollH, ScrollV, ZoomV };

// What one wheel gesture runs: the action, turned the other way (a "reversed" command), and by
// a whole view per notch (a "one page" command).
struct ViewWheelBinding {
    ViewWheelAction action = ViewWheelAction::None;
    bool            reversed = false;
    bool            page = false;
    bool operator==(const ViewWheelBinding& o) const { return action == o.action && reversed == o.reversed && page == o.page; }
};

// Modifier bits as REAPER orders them for the wheel.
constexpr int kViewModCtrl = 1, kViewModAlt = 2, kViewModShift = 4;

struct ViewKeyBinding {
    bool is_char = false;  // `code` is a character (else a Win32 virtual-key code)
    int  code = 0;
    int  mods = 0;         // kViewMod* bits
    bool operator==(const ViewKeyBinding& o) const { return is_char == o.is_char && code == o.code && mods == o.mods; }
};

// Lot 13-60: one gesture REAPER runs an action on (KbGesturesOf below).
enum class KbInput { Wheel, HWheel, Key };

struct KbGesture {
    KbInput input = KbInput::Key;
    int     code = 0;         // Key: a virtual key, or a character when is_char
    bool    is_char = false;
    int     mods = 0;         // kViewMod* bits
    bool operator==(const KbGesture& o) const
    {
        return input == o.input && code == o.code && is_char == o.is_char && mods == o.mods;
    }
};

// Lot 13-60: one KEY line of the Main section, as the file writes it (the command as text: "990",
// "_WOL_ADJENVTRHMOUSE"). A later line for the same <mod> <key> replaces an earlier one.
struct KbLine {
    int         mod = 0;
    int         key = 0;
    std::string cmd;
};

struct ViewKeymap {
    ViewWheelBinding wheel[8] = {};   // by modifier bits
    ViewWheelBinding hwheel[8] = {};
    std::vector<ViewKeyBinding> zoom_in, zoom_out;
    std::vector<KbLine> lines;  // 13-60: every Main-section KEY line of the file (any command)
    // 13-60: keys REAPER itself lists for a linked action (its live list, ParseReaperShortcutDesc), by
    // command: they add REAPER's built-in default keys, which the file does not hold. 13-60b: its wheels
    // too, and REAPER's view actions' always (ApplyListedViewGestures).
    std::vector<std::pair<std::string, KbGesture>> listed;
};

// REAPER's defaults (above).
ViewKeymap DefaultViewKeymap();
// The defaults overridden by the KEY lines of a reaper-kb.ini text (other lines ignored).
ViewKeymap ParseViewKeymap(const std::string& kb_ini_text);
// What a REAPER command does on the wheel (action None = not a view wheel command).
ViewWheelBinding ViewWheelBindingOf(int command);
ViewWheelAction  ViewWheelActionOf(int command);

// ---- lot 13-60: the gestures of any REAPER action (a Shortcuts row linked to it) -----------------

// REAPER's own default bindings that RAV knows (the view table above), as KEY lines: they hold until
// the file binds that <mod> <key> to something else (or to 0, disabled). REAPER's other built-in
// defaults are not in reaper-kb.ini (it holds the user's changes only): REAPER's own list of an
// action's shortcuts gives their keys (ViewKeymap::listed, ParseReaperShortcutDesc).
std::vector<KbLine> DefaultKbLines();

// Every key and wheel gesture REAPER runs `command` on ("990", "_WOL_ADJENVTRHMOUSE": action_link.h
// NormalizeCommandId's form, any case): the defaults above the file has not taken over, then the file's
// KEY lines for it, then the keys REAPER lists for it (`listed`) not found so. The wheel and the
// horizontal wheel by modifiers, the keys (none with Win); MIDI / OSC bindings and the special inputs
// other than the two wheels are not gestures RAV reads.
std::vector<KbGesture> KbGesturesOf(const ViewKeymap& k, const std::string& command);

// One shortcut as REAPER words it in its Actions list (GetActionShortcutDesc): "S", "Ctrl+Shift+F5",
// "Alt+Num +", "Shift+=", "Space", "PageDown", and (13-60b) the wheels: "Mousewheel",
// "Shift+Mousewheel", "Ctrl+Alt+Mousewheel", "HorizWheel". A key with Ctrl / Alt / Shift (also "Maj",
// "Strg", "Umschalt"): a letter, digit, F-key, arrow or named key as a virtual key, another single
// character (",", "=", "ù") as a character. Key names as REAPER, Windows' layouts or a LangPack write
// them, English or French ("GAUCHE", "PAGE SUIVANTE", "SUPPR", "+ (PAVÉ NUM.)", "PAVÉ NUM. 0"), any
// case, accents or not. False for anything else: MIDI, OSC, a trackpad's own gestures (MultiZoom,
// MultiRotate, MultiHorz, MultiVert), the Win key, the keypad's Enter, a name RAV does not know.
bool ParseReaperShortcutDesc(const std::string& desc, KbGesture* out);

// 13-60b: REAPER's view actions (the wheel ones of ViewWheelBindingOf, Zoom in / out 1012 / 1011): the
// shortcuts REAPER lists for them are asked for always (ViewKeymap::listed).
const std::vector<int>& ViewCommandIds();
// 13-60b: what REAPER lists for its view actions (`listed`) over the file and the default table: a
// listed wheel runs that view action (whatever the table says), a listed key zooms in / out. REAPER's
// own list is what is in force; the table is RAV's guess of its defaults.
void ApplyListedViewGestures(ViewKeymap& k);

}  // namespace rav
