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

// 13-60: the gestures of any action, named ones by their string id (any case), with REAPER's known
// view defaults until the file takes their input over.
static void TestAnyActionGestures()
{
    const std::string kb =
        "ACT 0 0 \"abc\" \"Custom: x\" 40001\n"
        "KEY 255 249 _WOL_ADJENVTRHMOUSE 0\n"    // Ctrl+wheel: no longer 1000's
        "KEY 9 70 _WOL_ADJENVTRHMOUSE 0\n"       // Ctrl+F (virtual key)
        "KEY 0 43 _wol_adjenvtrhmouse 0\n"       // the character "+", same id in another case
        "KEY 33 71 _WOL_ADJENVTRHMOUSE 0\n"      // Win+G: never matched
        "KEY 176 7 _WOL_ADJENVTRHMOUSE 0\n"      // a MIDI CC: not a gesture RAV reads
        "KEY 255 240 _WOL_ADJENVTRHMOUSE 0\n"    // Win+wheel
        "KEY 9 70 _WOL_ADJENVTRHMOUSE 1\n"       // another section
        "KEY 255 218 0 0\n"                      // Alt+H-wheel disabled
        "KEY 1 72 40044 0\n"
        "KEY 1 72 _RS1234 0\n";                  // the same key later: the script's now
    const ViewKeymap k = ParseViewKeymap(kb);
    const std::vector<KbGesture> g = KbGesturesOf(k, "_WOL_ADJENVTRHMOUSE");
    CHECK(g.size() == 3);
    if (g.size() == 3) {
        CHECK(g[0].input == KbInput::Wheel && g[0].mods == kViewModCtrl);
        CHECK(g[1].input == KbInput::Key && !g[1].is_char && g[1].code == 'F' && g[1].mods == kViewModCtrl);
        CHECK(g[2].input == KbInput::Key && g[2].is_char && g[2].code == '+' && g[2].mods == 0);
    }
    CHECK(KbGesturesOf(k, "WOL_ADJENVTRHMOUSE").size() == 3);  // typed without its underscore
    CHECK(KbGesturesOf(k, "1000").empty());                    // its wheel went to the script action
    CHECK(KbGesturesOf(k, "40044").empty());                   // its key went to the script
    CHECK(KbGesturesOf(k, "_RS1234").size() == 1);
    // REAPER's defaults that RAV knows: 990 on the wheel, Alt+H-wheel disabled here.
    const std::vector<KbGesture> z = KbGesturesOf(k, "990");
    CHECK(z.size() == 1 && z[0].input == KbInput::Wheel && z[0].mods == 0);
    const std::vector<KbGesture> s = KbGesturesOf(ParseViewKeymap(""), "988");
    CHECK(s.size() == 2 && s[0].input == KbInput::Wheel && s[0].mods == kViewModAlt && s[1].input == KbInput::HWheel);
    CHECK(KbGesturesOf(ParseViewKeymap(""), "1012").size() == 4);
    CHECK(KbGesturesOf(k, "").empty());
    CHECK(KbGesturesOf(k, "0").empty());
}

// 13-60: REAPER's own wording of a shortcut (its built-in defaults are not in reaper-kb.ini).
static void TestShortcutDesc()
{
    KbGesture g;
    CHECK(ParseReaperShortcutDesc("S", &g) && g.input == KbInput::Key && !g.is_char && g.code == 'S' && g.mods == 0);
    CHECK(ParseReaperShortcutDesc("Ctrl+Shift+s", &g) && g.code == 'S' && g.mods == (kViewModCtrl | kViewModShift));
    CHECK(ParseReaperShortcutDesc("Shift+Ctrl+Alt+F5", &g) && g.code == 0x74 && g.mods == 7);
    CHECK(ParseReaperShortcutDesc("F24", &g) && g.code == 0x87);
    CHECK(!ParseReaperShortcutDesc("F25", &g));
    CHECK(ParseReaperShortcutDesc("Alt+Num +", &g) && g.code == 0x6B && g.mods == kViewModAlt && !g.is_char);
    CHECK(ParseReaperShortcutDesc("Num 7", &g) && g.code == 0x67);
    CHECK(ParseReaperShortcutDesc("Ctrl++", &g) && g.is_char && g.code == '+' && g.mods == kViewModCtrl);
    CHECK(ParseReaperShortcutDesc("Shift+=", &g) && g.is_char && g.code == '=' && g.mods == kViewModShift);
    CHECK(ParseReaperShortcutDesc(" Space ", &g) && g.code == 0x20);
    CHECK(ParseReaperShortcutDesc("PageDown", &g) && g.code == 0x22);
    CHECK(ParseReaperShortcutDesc("Page Up", &g) && g.code == 0x21);
    CHECK(ParseReaperShortcutDesc("Ctrl+Delete", &g) && g.code == 0x2E && g.mods == kViewModCtrl);
    CHECK(ParseReaperShortcutDesc("7", &g) && g.code == '7' && !g.is_char);
    CHECK(!ParseReaperShortcutDesc("Mousewheel", &g));
    CHECK(!ParseReaperShortcutDesc("Ctrl+Mousewheel", &g));
    CHECK(!ParseReaperShortcutDesc("Win+S", &g));
    CHECK(!ParseReaperShortcutDesc("CC 7", &g));
    CHECK(!ParseReaperShortcutDesc("Ctrl+", &g));
    CHECK(!ParseReaperShortcutDesc("", &g));
    CHECK(!ParseReaperShortcutDesc("Ctrl+Ctrl+S", &g));
    // The keys REAPER lists for an action add to the file's (once each).
    ViewKeymap k = ParseViewKeymap("KEY 1 83 40759 0\n");  // S, in the file too
    KbGesture s;
    ParseReaperShortcutDesc("S", &s);
    KbGesture cs;
    ParseReaperShortcutDesc("Ctrl+S", &cs);
    k.listed = {{"40759", s}, {"40759", cs}, {"40044", s}};
    const std::vector<KbGesture> got = KbGesturesOf(k, "40759");
    CHECK(got.size() == 2 && got[0] == s && got[1] == cs);
}

int main()
{
    TestShortcutDesc();
    TestAnyActionGestures();
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
