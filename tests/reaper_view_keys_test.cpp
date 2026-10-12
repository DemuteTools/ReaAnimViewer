// SPDX-License-Identifier: MIT
//
// Host test of src/reaper_view_keys.h (story 13-7b): reaper-kb.ini wheel and zoom-key lines,
// with modifiers, override REAPER's defaults; other sections, commands and lines are ignored.

#include "reaper_view_keys.h"

#include <cstdio>

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++g_fail;                                                         \
        }                                                                     \
    } while (0)

static bool Has(const std::vector<ViewKeyBinding>& v, bool is_char, int code, int mods)
{
    ViewKeyBinding b;
    b.is_char = is_char;
    b.code = code;
    b.mods = mods;
    for (const ViewKeyBinding& x : v)
        if (x == b) return true;
    return false;
}

static void TestDefaults()
{
    const ViewKeymap k = ParseViewKeymap("");
    CHECK(k.wheel[0] == ViewWheelAction::ZoomH);
    CHECK(k.wheel[kViewModAlt] == ViewWheelAction::ScrollH);
    CHECK(k.wheel[kViewModCtrl] == ViewWheelAction::ZoomV);
    CHECK(k.wheel[kViewModCtrl | kViewModAlt] == ViewWheelAction::ScrollV);
    CHECK(k.hwheel[0] == ViewWheelAction::ScrollH);
    CHECK(k.hwheel[kViewModAlt] == ViewWheelAction::ZoomH);
    CHECK(Has(k.zoom_in, false, 0x6B, 0) && Has(k.zoom_in, false, 0xBB, 0));
    CHECK(Has(k.zoom_in, false, 0xBB, kViewModShift) && Has(k.zoom_in, false, 0x26, 0));
    CHECK(Has(k.zoom_out, false, 0x6D, 0) && Has(k.zoom_out, false, 0xBD, 0) && Has(k.zoom_out, false, 0x28, 0));
}

static void TestWinKeySkipped()
{
    // Win+F (mod 1|32) bound to zoom in: skipped; Ctrl+F (mod 9) kept.
    const ViewKeymap k = ParseViewKeymap("KEY 33 70 1012 0\nKEY 9 70 1012 0\n");
    CHECK(!Has(k.zoom_in, false, 70, 0));
    CHECK(Has(k.zoom_in, false, 70, kViewModCtrl));
}

static void TestWheelOverrides()
{
    // A real user keymap: plain wheel scrolls, Ctrl+wheel zooms, Alt+wheel zooms vertically,
    // the horizontal wheel disabled.
    const char* ini =
        "SCR 4 0 RS1234 \"Custom: x\" foo.lua\n"
        "KEY 255 216 0 0 # Main : HorizWheel : DISABLED DEFAULT\n"
        "KEY 255 218 0 0 # Main : Alt+HorizWheel : DISABLED DEFAULT\n"
        "KEY 255 248 988 0 # Main : Mousewheel : OVERRIDE DEFAULT\n"
        "KEY 255 249 990 0 # Main : Ctrl+Mousewheel : OVERRIDE DEFAULT\n"
        "KEY 255 250 1000 0 # Main : Alt+Mousewheel : OVERRIDE DEFAULT\n"
        "KEY 255 252 990 32060 # MIDI editor: ignored\n"
        "KEY 255 125 990 0 # older code: Ctrl+Shift+Mousewheel\n"
        "KEY 255 222 988 0 # Alt+Shift+HorizWheel\n";
    const ViewKeymap k = ParseViewKeymap(ini);
    CHECK(k.wheel[0] == ViewWheelAction::ScrollH);
    CHECK(k.wheel[kViewModCtrl] == ViewWheelAction::ZoomH);
    CHECK(k.wheel[kViewModAlt] == ViewWheelAction::ZoomV);
    CHECK(k.wheel[kViewModShift] == ViewWheelAction::None);
    CHECK(k.wheel[kViewModCtrl | kViewModShift] == ViewWheelAction::ZoomH);
    CHECK(k.hwheel[0] == ViewWheelAction::None);
    CHECK(k.hwheel[kViewModAlt] == ViewWheelAction::None);
    CHECK(k.hwheel[kViewModAlt | kViewModShift] == ViewWheelAction::ScrollH);
}

static void TestKeys()
{
    const char* ini =
        "KEY 9 187 1012 0 # Ctrl+= zoom in\n"         // 9 = virtual key + Ctrl
        "KEY 0 93 1012 0 # char ']' zoom in\n"        // even flags: a character
        "KEY 29 189 1011 0 # Ctrl+Alt+Shift+-\n"      // 1 + 4 + 8 + 16
        "KEY 1 107 40001 0 # keypad + runs something else\n"
        "KEY 1 109 _RS1234abcd 0 # keypad - runs a script\n"
        "KEY 1 189 1011 32060 # MIDI editor: ignored\n"
        "KEY garbage\n";
    const ViewKeymap k = ParseViewKeymap(ini);
    CHECK(Has(k.zoom_in, false, 187, kViewModCtrl));
    CHECK(Has(k.zoom_in, true, ']', 0));
    CHECK(Has(k.zoom_in, false, 187, 0));   // the default = stays
    CHECK(!Has(k.zoom_in, false, 107, 0));  // taken by another action
    CHECK(Has(k.zoom_out, false, 189, kViewModCtrl | kViewModAlt | kViewModShift));
    CHECK(!Has(k.zoom_out, false, 109, 0));  // taken by a script
    CHECK(Has(k.zoom_out, false, 189, 0));
    CHECK(ViewWheelActionOf(990) == ViewWheelAction::ZoomH && ViewWheelActionOf(1) == ViewWheelAction::None);
}

int main()
{
    TestDefaults();
    TestWheelOverrides();
    TestKeys();
    TestWinKeySkipped();
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("reaper_view_keys: all passed\n");
    return 0;
}
