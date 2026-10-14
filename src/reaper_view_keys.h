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

struct ViewKeymap {
    ViewWheelBinding wheel[8] = {};   // by modifier bits
    ViewWheelBinding hwheel[8] = {};
    std::vector<ViewKeyBinding> zoom_in, zoom_out;
};

// REAPER's defaults (above).
ViewKeymap DefaultViewKeymap();
// The defaults overridden by the KEY lines of a reaper-kb.ini text (other lines ignored).
ViewKeymap ParseViewKeymap(const std::string& kb_ini_text);
// What a REAPER command does on the wheel (action None = not a view wheel command).
ViewWheelBinding ViewWheelBindingOf(int command);
ViewWheelAction  ViewWheelActionOf(int command);

}  // namespace rav
