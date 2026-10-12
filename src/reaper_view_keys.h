// SPDX-License-Identifier: MIT
//
// Story 13-7b (Edit overlay zoom): the user's own REAPER view bindings, read from
// <resource path>/reaper-kb.ini, so the Edit overlay zooms and scrolls like his arrange view.
// Pure (no REAPER, no ImGui): host-tested by tests/reaper_view_keys_test.cpp.
//
// reaper-kb.ini lines used: "KEY <mod> <key> <command> <section>", Main section (0) only.
//  - <mod> 255 = a special input. Mouse wheel: <key> 248 + bits (Ctrl 1, Alt 2, Shift 4), or
//    the older 120 + bits; horizontal wheel: 216 + bits, or the older 88 + bits.
//    Command 0 = the default binding disabled.
//  - otherwise <mod> is the Win32 accelerator flags: 1 = virtual key (FVIRTKEY), 4 Shift,
//    8 Ctrl, 16 Alt; <key> is a virtual-key code (odd <mod>) or a character (even <mod>).
// Commands mapped (REAPER's Main action list):
//   988  View: Scroll horizontally (MIDI CC relative/mousewheel)
//   989  View: Scroll vertically (MIDI CC relative/mousewheel)      (no vertical view here: ignored)
//   990  View: Zoom horizontally (MIDI CC relative/mousewheel)
//   1000 View: Zoom vertically (MIDI CC relative/mousewheel)        (ignored)
//   1011 View: Zoom out horizontal, 1012 View: Zoom in horizontal (keys)
// Defaults when the file binds nothing (REAPER 7's): wheel and Alt+horizontal wheel = zoom
// horizontally around the mouse, Alt+wheel and the horizontal wheel = scroll horizontally,
// Ctrl+wheel = zoom vertically and Ctrl+Alt+wheel = scroll vertically (both ignored);
// keys: + (keypad), =, + (Shift+=) and Up zoom in, - (keypad), - and Down zoom out.
// A binding with the Win key (<mod> bit 32) is skipped: never matched here.

#pragma once

#include <string>
#include <vector>

namespace rav {

enum class ViewWheelAction { None, ZoomH, ScrollH, ScrollV, ZoomV };

// Modifier bits as REAPER orders them for the wheel.
constexpr int kViewModCtrl = 1, kViewModAlt = 2, kViewModShift = 4;

struct ViewKeyBinding {
    bool is_char = false;  // `code` is a character (else a Win32 virtual-key code)
    int  code = 0;
    int  mods = 0;         // kViewMod* bits
    bool operator==(const ViewKeyBinding& o) const { return is_char == o.is_char && code == o.code && mods == o.mods; }
};

struct ViewKeymap {
    ViewWheelAction wheel[8] = {};   // by modifier bits
    ViewWheelAction hwheel[8] = {};
    std::vector<ViewKeyBinding> zoom_in, zoom_out;
};

// REAPER's defaults (above).
ViewKeymap DefaultViewKeymap();
// The defaults overridden by the KEY lines of a reaper-kb.ini text (other lines ignored).
ViewKeymap ParseViewKeymap(const std::string& kb_ini_text);
// The command a wheel action runs (0 = none), for messages / tests.
ViewWheelAction ViewWheelActionOf(int command);

}  // namespace rav
