// SPDX-License-Identifier: MIT
//
// Host test of src/shortcut_key_index.h (13-64): the reverse index gesture -> REAPER actions built from
// fake actions (REAPER's defaults as it lists them, the user's reaper-kb.ini keys, the wheels, MIDI / OSC
// skipped, one key on several actions), the Link modal's search (name, id, shortcut typed) and RAV's side
// (KEYS rows, view rows, the Sound Edit window's own keys, the fixed mouse wheel).

#include "shortcut_key_index.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++g_fail;                                                         \
        }                                                                     \
    } while (0)

static KbGesture Key(int code, int mods = 0)
{
    KbGesture g;
    g.input = KbInput::Key;
    g.code = code;
    g.mods = mods;
    return g;
}

static KbGesture Wheel(int mods = 0, bool horizontal = false)
{
    KbGesture g;
    g.input = horizontal ? KbInput::HWheel : KbInput::Wheel;
    g.mods = mods;
    return g;
}

static std::vector<KeyIndexSource> FakeActions()
{
    return {
        {"40757", "Item: Split items at edit cursor (no change selection)", {}},
        {"40759", "Item: Split items at edit cursor (select right)", {"S"}},  // a REAPER default
        {"40029", "Edit: Undo", {"Ctrl+Z"}},
        {"40030", "Edit: Redo", {"Ctrl+Shift+Z", "Ctrl+Y"}},
        {"40057", "Edit: Copy items/tracks/envelope points", {"Ctrl+C"}},
        {"40044", "Transport: Play/stop", {"Space", "MIDI Chan 1 Note 60", "OSC: /play"}},  // unparsable kept as text
        {"990", "View: Zoom horizontally (MIDI CC relative/mousewheel)", {"Mousewheel", "Alt+HorizWheel"}},
        {"988", "View: Scroll horizontally (MIDI CC relative/mousewheel)", {"Alt+Mousewheel", "HorizWheel"}},
        {"_WOL_ADJENVTRHMOUSE", "Adjust envelope or track height (mousewheel)", {"Shift+Mousewheel"}},
        {"_SWS_SAVESEL", "SWS: Save current track selection", {"S"}},  // one key on two actions
        {"40026", "File: Save project", {"Ctrl+S", "Win+S", "MultiZoom"}},
        {"41111", "Test: equals", {"="}},  // a character key
    };
}

static int Find(const ShortcutKeyIndex& ix, const char* id)
{
    for (size_t i = 0; i < ix.actions.size(); ++i)
        if (ix.actions[i].id == id) return static_cast<int>(i);
    return -1;
}

static void TestIndex()
{
    const ShortcutKeyIndex ix = BuildShortcutKeyIndex(FakeActions());
    CHECK(ix.actions.size() == FakeActions().size());
    // REAPER's defaults (as it lists them).
    std::vector<int> on = ix.ActionsOn(Key('Z', kViewModCtrl));
    CHECK(on.size() == 1 && on[0] == Find(ix, "40029"));
    on = ix.ActionsOn(Key('C', kViewModCtrl));
    CHECK(on.size() == 1 && on[0] == Find(ix, "40057"));
    // One key on several actions, in REAPER's order.
    on = ix.ActionsOn(Key('S'));
    CHECK(on.size() == 2 && on[0] == Find(ix, "40759") && on[1] == Find(ix, "_SWS_SAVESEL"));
    // Two keys on one action.
    CHECK(ix.ActionsOn(Key('Y', kViewModCtrl)).size() == 1);
    CHECK(ix.ActionsOn(Key('Z', kViewModCtrl | kViewModShift)).size() == 1);
    // The wheels.
    on = ix.ActionsOn(Wheel());
    CHECK(on.size() == 1 && on[0] == Find(ix, "990"));
    on = ix.ActionsOn(Wheel(kViewModAlt));
    CHECK(on.size() == 1 && on[0] == Find(ix, "988"));
    on = ix.ActionsOn(Wheel(kViewModShift));
    CHECK(on.size() == 1 && on[0] == Find(ix, "_WOL_ADJENVTRHMOUSE"));
    CHECK(ix.ActionsOn(Wheel(0, true)).size() == 1);
    CHECK(ix.ActionsOn(Wheel(kViewModAlt, true)).size() == 1);
    // MIDI / OSC / Win / trackpad skipped as gestures, kept as text.
    const KeyIndexAction& play = ix.actions[static_cast<size_t>(Find(ix, "40044"))];
    CHECK(play.gestures.size() == 1 && play.gestures[0] == Key(0x20));
    CHECK(play.descs.size() == 3);
    const KeyIndexAction& save = ix.actions[static_cast<size_t>(Find(ix, "40026"))];
    CHECK(save.gestures.size() == 1);
    // No key: none.
    CHECK(ix.actions[static_cast<size_t>(Find(ix, "40757"))].gestures.empty());
    CHECK(ix.ActionsOn(Key('Q')).empty());
    // A character key stays a character without a normalizer.
    on = ix.ActionsOn(KbGesture{KbInput::Key, '=', true, 0});
    CHECK(on.size() == 1 && on[0] == Find(ix, "41111"));
}

// The normalizer: "=" is the key 0xBB on this (fake) layout.
static bool FakeLayout(KbGesture* g)
{
    if (g->input == KbInput::Key && g->is_char) {
        if (g->code != '=') return false;
        g->is_char = false;
        g->code = 0xBB;
    }
    return true;
}

static void TestNormalizer()
{
    const ShortcutKeyIndex ix = BuildShortcutKeyIndex(FakeActions(), nullptr, &FakeLayout);
    std::vector<int> on = ix.ActionsOn(Key(0xBB));
    CHECK(on.size() == 1 && on[0] == Find(ix, "41111"));
    CHECK(ix.ActionsOn(KbGesture{KbInput::Key, '=', true, 0}).empty());
}

// The user's keys in reaper-kb.ini (before REAPER lists them) join REAPER's list.
static void TestUserKeys()
{
    const ViewKeymap kb = ParseViewKeymap(
        "KEY 9 75 _SWS_SAVESEL 0\n"      // Ctrl+K
        "KEY 255 249 40757 0\n"          // Ctrl+wheel on the split
        "KEY 176 7 40757 0\n"            // a MIDI CC: not a gesture
        "KEY 1 83 40759 1\n");           // another section: ignored
    const ShortcutKeyIndex ix = BuildShortcutKeyIndex(FakeActions(), &kb);
    std::vector<int> on = ix.ActionsOn(Key('K', kViewModCtrl));
    CHECK(on.size() == 1 && on[0] == Find(ix, "_SWS_SAVESEL"));
    on = ix.ActionsOn(Wheel(kViewModCtrl));
    CHECK(on.size() == 1 && on[0] == Find(ix, "40757"));
    // A key both listed and in the file counts once.
    const ViewKeymap kb2 = ParseViewKeymap("KEY 9 90 40029 0\n");  // Ctrl+Z, as REAPER lists it
    const ShortcutKeyIndex ix2 = BuildShortcutKeyIndex(FakeActions(), &kb2);
    CHECK(ix2.actions[static_cast<size_t>(Find(ix2, "40029"))].gestures.size() == 1);
    CHECK(ix2.ActionsOn(Key('Z', kViewModCtrl)).size() == 1);
}

static void TestTyped()
{
    KbGesture g;
    CHECK(ParseTypedShortcut("ctrl+s", &g) && g == Key('S', kViewModCtrl));
    CHECK(ParseTypedShortcut(" Shift+Wheel ", &g) && g == Wheel(kViewModShift));
    CHECK(ParseTypedShortcut("alt+h-wheel", &g) && g == Wheel(kViewModAlt, true));
    CHECK(ParseTypedShortcut("space", &g) && g == Key(0x20));
    CHECK(ParseTypedShortcut("F5", &g) && g == Key(0x74));
    CHECK(!ParseTypedShortcut("s", &g));       // one letter: a word of a name
    CHECK(!ParseTypedShortcut("\xC3\xB9", &g));  // one character, UTF-8
    CHECK(!ParseTypedShortcut("split", &g));
    CHECK(!ParseTypedShortcut("40757", &g));
    CHECK(!ParseTypedShortcut("", &g));
}

static std::vector<std::string> Search(const ShortcutKeyIndex& ix, const std::string& q)
{
    KbGesture g;
    const bool typed = ParseTypedShortcut(q, &g);
    std::vector<std::string> out;
    for (const KeyIndexAction& a : ix.actions)
        if (LinkSearchMatch(a, q, typed ? &g : nullptr)) out.push_back(a.id);
    return out;
}

static void TestSearch()
{
    const ShortcutKeyIndex ix = BuildShortcutKeyIndex(FakeActions());
    // By name.
    std::vector<std::string> r = Search(ix, "split");
    CHECK(r.size() == 2 && r[0] == "40757" && r[1] == "40759");
    // By id.
    r = Search(ix, "40757");
    CHECK(r.size() == 1 && r[0] == "40757");
    r = Search(ix, "wol_adj");
    CHECK(r.size() == 1 && r[0] == "_WOL_ADJENVTRHMOUSE");
    // By shortcut text.
    r = Search(ix, "ctrl+z");
    CHECK(r.size() == 1 && r[0] == "40029");
    r = Search(ix, "Shift+Wheel");
    CHECK(r.size() == 1 && r[0] == "_WOL_ADJENVTRHMOUSE");
    // A complete shortcut matches its gesture only (not every Ctrl+Shift+... action).
    r = Search(ix, "ctrl+s");
    CHECK(r.size() == 1 && r[0] == "40026");
    // Typing: "ctrl+sh" starts REAPER's "Ctrl+Shift+Z".
    r = Search(ix, "ctrl+sh");
    CHECK(r.size() == 1 && r[0] == "40030");
    // Empty: all.
    CHECK(Search(ix, "").size() == ix.actions.size());
}

static void TestRavSide()
{
    ShortcutBindings b = DefaultShortcutBindings();
    const ShortcutKeymap km = ShortcutKeymapOf(b);
    ViewNavChoices ch{};
    const ViewNavMap nav = ResolveViewNav(DefaultViewKeymap(), ch);
    // C: Video's cut. Ctrl+C: copy camera.
    std::vector<RavKeyUse> u = RavUsesOf(km, nav, Key('C'));
    CHECK(u.size() == 1 && u[0].kind == RavKeyUse::Kind::KeyRow && u[0].index == kShortcutCut);
    u = RavUsesOf(km, nav, Key('C', kViewModCtrl));
    CHECK(u.size() == 1 && u[0].index == kShortcutCopyCamera);
    // Delete: two rows (Video and Tagging).
    u = RavUsesOf(km, nav, Key(vk::kDelete));
    CHECK(u.size() == 2);
    // Space: the Sound Edit window's own key, fixed. Shift+Space too. E: Tagging's row and Edit's own.
    u = RavUsesOf(km, nav, Key(vk::kSpace));
    CHECK(u.size() == 1 && u[0].kind == RavKeyUse::Kind::EditReserved && u[0].index == 0);
    u = RavUsesOf(km, nav, Key(vk::kSpace, kViewModShift));
    CHECK(u.size() == 1 && u[0].kind == RavKeyUse::Kind::EditReserved && u[0].index == 1);
    u = RavUsesOf(km, nav, Key('E'));
    CHECK(u.size() == 2 && u[0].kind == RavKeyUse::Kind::KeyRow && u[0].index == kShortcutTagAddEvent &&
          u[1].kind == RavKeyUse::Kind::EditReserved);
    // The plain wheel: Zoom horizontally (REAPER's default) and the 3D view's fixed Zoom.
    u = RavUsesOf(km, nav, Wheel());
    CHECK(u.size() == 2 && u[0].kind == RavKeyUse::Kind::ViewRow && u[0].index == kNavZoomH &&
          u[1].kind == RavKeyUse::Kind::Mouse);
    // Alt+wheel: Scroll horizontally.
    u = RavUsesOf(km, nav, Wheel(kViewModAlt));
    CHECK(u.size() == 1 && u[0].kind == RavKeyUse::Kind::ViewRow && u[0].index == kNavScrollH);
    // 13-74: Ctrl+Shift+wheel: the zones' gain rows (up and down), whatever else the view table puts there.
    u = RavUsesOf(km, nav, Wheel(kViewModCtrl | kViewModShift));
    {
        int gain_rows = 0;
        for (const RavKeyUse& x : u)
            if (x.kind == RavKeyUse::Kind::KeyRow && (x.index == kShortcutEditGainUp || x.index == kShortcutEditGainDown) && !x.shadowed)
                ++gain_rows;
        CHECK(gain_rows == 2);
    }
    CHECK(KbGestureOfBinding(KeyBinding{vk::kWheelUp, true, true, false}) == Wheel(kViewModCtrl | kViewModShift));
    CHECK(KbGestureOfBinding(KeyBinding{vk::kWheelDown, true, true, false}) == Wheel(kViewModCtrl | kViewModShift));
    // Up: Zoom in (a key of the view table). A character row given by the caller counts once.
    u = RavUsesOf(km, nav, Key(vk::kUp));
    CHECK(u.size() == 1 && u[0].index == kNavZoomIn);
    u = RavUsesOf(km, nav, Key(vk::kUp), kNavZoomIn);
    CHECK(u.size() == 1);
    // Nothing on Q.
    CHECK(RavUsesOf(km, nav, Key('Q')).empty());
    // A shadowed key: listed, marked.
    ShortcutKeymap sh = km;
    sh.shadowed[kShortcutClipList].push_back(KeyBinding{'C', false, false, false});
    u = RavUsesOf(sh, nav, Key('C'));
    CHECK(u.size() == 2 && !u[0].shadowed && u[1].shadowed && u[1].index == kShortcutClipList);
    // Gesture <-> binding.
    KeyBinding kb;
    CHECK(KeyBindingOfKbGesture(Key('S', kViewModCtrl | kViewModAlt), &kb) && kb.vk == 'S' && kb.ctrl && kb.alt && !kb.shift);
    CHECK(!KeyBindingOfKbGesture(Wheel(), &kb));
    CHECK(KbGestureOfBinding(KeyBinding{'S', true, true, false}) == Key('S', kViewModCtrl | kViewModShift));
}

int main()
{
    TestIndex();
    TestNormalizer();
    TestUserKeys();
    TestTyped();
    TestSearch();
    TestRavSide();
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("shortcut_key_index: all checks passed\n");
    return 0;
}
