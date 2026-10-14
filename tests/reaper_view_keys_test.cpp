// SPDX-License-Identifier: MIT
//
// Host test of src/reaper_view_keys.h (story 13-7b, 13-45): reaper-kb.ini wheel and zoom-key lines,
// with modifiers, override REAPER's defaults; other sections, commands and lines are ignored. The
// command ids and the wheel codes are REAPER 7.81's own (read from its action table and decoder).

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

static ViewWheelAction A(const ViewWheelBinding& b) { return b.action; }

static void TestDefaults()
{
    const ViewKeymap k = ParseViewKeymap("");
    CHECK(A(k.wheel[0]) == ViewWheelAction::ZoomH);
    CHECK(A(k.wheel[kViewModAlt]) == ViewWheelAction::ScrollH);
    CHECK(A(k.wheel[kViewModCtrl]) == ViewWheelAction::ZoomV);
    CHECK(A(k.wheel[kViewModCtrl | kViewModAlt]) == ViewWheelAction::ScrollV);
    CHECK(A(k.wheel[kViewModAlt | kViewModShift]) == ViewWheelAction::ScrollV && k.wheel[kViewModAlt | kViewModShift].page);
    CHECK(A(k.wheel[kViewModShift]) == ViewWheelAction::None);
    CHECK(A(k.wheel[kViewModCtrl | kViewModShift]) == ViewWheelAction::None);  // 972: track heights
    CHECK(A(k.hwheel[0]) == ViewWheelAction::ScrollH);
    CHECK(A(k.hwheel[kViewModAlt]) == ViewWheelAction::ZoomH);
    CHECK(!k.wheel[0].reversed && !k.wheel[0].page);
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
        "KEY 255 222 988 0 # Alt+Shift+HorizWheel\n";
    const ViewKeymap k = ParseViewKeymap(ini);
    CHECK(A(k.wheel[0]) == ViewWheelAction::ScrollH);
    CHECK(A(k.wheel[kViewModCtrl]) == ViewWheelAction::ZoomH);
    CHECK(A(k.wheel[kViewModAlt]) == ViewWheelAction::ZoomV);
    CHECK(A(k.wheel[kViewModShift]) == ViewWheelAction::None);
    CHECK(A(k.hwheel[0]) == ViewWheelAction::None);
    CHECK(A(k.hwheel[kViewModAlt]) == ViewWheelAction::None);
    CHECK(A(k.hwheel[kViewModAlt | kViewModShift]) == ViewWheelAction::ScrollH);
}

// 13-45: the codes REAPER really writes. 120 / 88 + bits are no wheel (13-7b read them as an
// "older" wheel); bit 3 clear = the Win key held: never the plain wheel.
static void TestWheelCodes()
{
    const char* ini =
        "KEY 255 120 988 0 # not a wheel code\n"
        "KEY 255 88 990 0 # not a wheel code\n"
        "KEY 255 240 988 0 # Win+Mousewheel\n"
        "KEY 255 208 0 0 # Win+HorizWheel disabled\n"
        "KEY 255 252 988 0 # Shift+Mousewheel\n"
        "KEY 255 168 997 0 # MultiZoom: not ours\n";
    const ViewKeymap k = ParseViewKeymap(ini);
    CHECK(A(k.wheel[0]) == ViewWheelAction::ZoomH);  // the default stays
    CHECK(A(k.hwheel[0]) == ViewWheelAction::ScrollH);
    CHECK(A(k.wheel[kViewModShift]) == ViewWheelAction::ScrollH);
}

// 13-45: every view wheel command REAPER has, reversed and one-page ones included.
static void TestCommandIds()
{
    struct Row {
        int             cmd;
        ViewWheelAction a;
        bool            rev, page;
    };
    const Row rows[] = {
        {988, ViewWheelAction::ScrollH, false, false}, {977, ViewWheelAction::ScrollH, true, false},
        {981, ViewWheelAction::ScrollH, false, true},  {975, ViewWheelAction::ScrollH, true, true},
        {989, ViewWheelAction::ScrollV, false, false}, {978, ViewWheelAction::ScrollV, true, false},
        {976, ViewWheelAction::ScrollV, false, true},  {982, ViewWheelAction::ScrollV, true, true},
        {990, ViewWheelAction::ZoomH, false, false},   {979, ViewWheelAction::ZoomH, true, false},
        {1000, ViewWheelAction::ZoomV, false, false},  {1001, ViewWheelAction::ZoomV, true, false},
        {991, ViewWheelAction::ZoomV, false, false},   {980, ViewWheelAction::ZoomV, true, false},
        {974, ViewWheelAction::None, false, false},    {992, ViewWheelAction::None, false, false},
        {997, ViewWheelAction::None, false, false},    {972, ViewWheelAction::None, false, false},
        {40001, ViewWheelAction::None, false, false},  {0, ViewWheelAction::None, false, false},
    };
    for (const Row& r : rows) {
        const ViewWheelBinding b = ViewWheelBindingOf(r.cmd);
        CHECK(b.action == r.a);
        CHECK(ViewWheelActionOf(r.cmd) == r.a);
        if (r.a != ViewWheelAction::None) CHECK(b.reversed == r.rev && b.page == r.page);
    }
    // Antho's case: his wheel binding runs 990 = View: Zoom horizontally (Shift+wheel here).
    const ViewKeymap k = ParseViewKeymap("KEY 255 252 990 0\nKEY 255 250 977 0\n");
    CHECK(A(k.wheel[kViewModShift]) == ViewWheelAction::ZoomH);
    CHECK(A(k.wheel[kViewModAlt]) == ViewWheelAction::ScrollH && k.wheel[kViewModAlt].reversed);
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
}

int main()
{
    TestDefaults();
    TestWheelOverrides();
    TestKeys();
    TestWinKeySkipped();
    TestWheelCodes();
    TestCommandIds();
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("reaper_view_keys: all passed\n");
    return 0;
}
