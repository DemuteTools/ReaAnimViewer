// SPDX-License-Identifier: MIT
//
// Host test of src/view_nav.h (story 13-45): the six view actions, linked to REAPER (reaper-kb.ini)
// or the user's own gesture; the gesture -> action map; a frame's input -> the view's moves; the
// recording rule; the ExtState encoding.

#include "view_nav.h"

#include <cmath>
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

static bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }

static void TestTable()
{
    // The linked REAPER actions (ids read from REAPER 7.81's action table).
    CHECK(kViewNavTable[kNavScrollH].reaper_cmd == 988);
    CHECK(kViewNavTable[kNavScrollV].reaper_cmd == 989);
    CHECK(kViewNavTable[kNavZoomH].reaper_cmd == 990);
    CHECK(kViewNavTable[kNavZoomV].reaper_cmd == 1000);
    CHECK(kViewNavTable[kNavZoomIn].reaper_cmd == 1012);
    CHECK(kViewNavTable[kNavZoomOut].reaper_cmd == 1011);
    for (int r = 0; r < 4; ++r) CHECK(ViewWheelActionOf(kViewNavTable[r].reaper_cmd) != ViewWheelAction::None);
    for (int r = 0; r < 4; ++r) CHECK(ViewNavRowOf(ViewWheelActionOf(kViewNavTable[r].reaper_cmd)) == r);
}

// Linked rows: REAPER's gestures, live from the file.
static void TestLinkedDefaults()
{
    const ViewNavMap m = ResolveViewNav(ParseViewKeymap(""), ViewNavChoices{});
    // Wheel = zoom H, Alt+wheel = scroll H, Ctrl+wheel = zoom V, Ctrl+Alt+wheel = scroll V.
    CHECK(ViewNavOwner(m, WheelGesture(false, 0)) == kNavZoomH);
    CHECK(ViewNavOwner(m, WheelGesture(false, kViewModAlt)) == kNavScrollH);
    CHECK(ViewNavOwner(m, WheelGesture(false, kViewModCtrl)) == kNavZoomV);
    CHECK(ViewNavOwner(m, WheelGesture(false, kViewModCtrl | kViewModAlt)) == kNavScrollV);
    CHECK(ViewNavOwner(m, WheelGesture(true, 0)) == kNavScrollH);
    CHECK(ViewNavOwner(m, WheelGesture(true, kViewModAlt)) == kNavZoomH);
    CHECK(ViewNavOwner(m, WheelGesture(false, kViewModShift)) == -1);
    CHECK(ViewNavKeyRow(m, 0x26, 0) == kNavZoomIn);    // Up
    CHECK(ViewNavKeyRow(m, 0x28, 0) == kNavZoomOut);   // Down
    CHECK(ViewNavKeyRow(m, 0x6B, 0) == kNavZoomIn);    // keypad +
    CHECK(ViewNavKeyRow(m, 0x26, kViewModCtrl) == -1);  // exact modifiers only
    // The rows' gestures, as the Shortcuts popup lists them.
    const std::vector<ViewGesture> zh = ViewNavGestures(m, kNavZoomH);
    CHECK(zh.size() == 2 && FormatViewGesture(zh[0]) == "Wheel" && FormatViewGesture(zh[1]) == "Alt+H-wheel");
    const std::vector<ViewGesture> sh = ViewNavGestures(m, kNavScrollH);
    CHECK(sh.size() == 2 && FormatViewGesture(sh[0]) == "Alt+Wheel" && FormatViewGesture(sh[1]) == "H-wheel");
    CHECK(FormatViewGesture(ViewNavGestures(m, kNavScrollV)[0]) == "Ctrl+Alt+Wheel");
    CHECK(ViewNavGestures(m, kNavScrollV).size() == 2);  // + Alt+Shift+Wheel (one page)
    CHECK(FormatViewGesture(ViewNavGestures(m, kNavZoomOut).back()) == "Down");
    CHECK(ViewNavHint(m) == "Zoom: Wheel  \xC2\xB7  Scroll: Alt+Wheel");
}

// Antho's REAPER: Shift+wheel runs 990 (Zoom horizontally), the plain wheel 988. A linked row follows.
static void TestLinkedFollowsFile()
{
    const ViewKeymap k = ParseViewKeymap("KEY 255 248 988 0\nKEY 255 252 990 0\nKEY 255 250 0 0\n");
    const ViewNavMap m = ResolveViewNav(k, ViewNavChoices{});
    CHECK(ViewNavOwner(m, WheelGesture(false, 0)) == kNavScrollH);
    CHECK(ViewNavOwner(m, WheelGesture(false, kViewModShift)) == kNavZoomH);
    CHECK(ViewNavOwner(m, WheelGesture(false, kViewModAlt)) == -1);  // disabled in REAPER
    // The wheel up scrolls left; Shift+wheel up zooms in.
    ViewNavStep s = ViewNavStepFor(m, 0, 1.0f, 0.0f, {});
    CHECK(Near(s.scroll_h, -1.0) && Near(s.zoom_h, 0.0));
    s = ViewNavStepFor(m, kViewModShift, 1.0f, 0.0f, {});
    CHECK(Near(s.zoom_h, 1.0) && Near(s.scroll_h, 0.0));
    CHECK(ViewNavWheelBound(m, 0, false) && !ViewNavWheelBound(m, kViewModAlt, false));
}

// Own rows: the user's gesture replaces REAPER's for that row and takes it from a linked row.
static void TestOwn()
{
    const ViewKeymap k = ParseViewKeymap("");
    ViewNavChoices c{};
    ViewNavAssign(c, kNavScrollH, WheelGesture(false, kViewModShift));
    ViewNavMap m = ResolveViewNav(k, c);
    CHECK(ViewNavOwner(m, WheelGesture(false, kViewModShift)) == kNavScrollH);
    CHECK(ViewNavOwner(m, WheelGesture(false, kViewModAlt)) == -1);  // REAPER's Alt+wheel: not this row any more
    CHECK(ViewNavOwner(m, WheelGesture(true, 0)) == -1);
    CHECK(ViewNavGestures(m, kNavScrollH).size() == 1);
    CHECK(ViewNavOwner(m, WheelGesture(false, 0)) == kNavZoomH);  // the others still follow REAPER
    // An own gesture held by a linked row: taken from it.
    ViewNavAssign(c, kNavScrollH, WheelGesture(false, 0));
    m = ResolveViewNav(k, c);
    CHECK(ViewNavOwner(m, WheelGesture(false, 0)) == kNavScrollH);
    CHECK(ViewNavGestures(m, kNavZoomH).size() == 1);  // Alt+H-wheel left
    // Two own rows: the newer takes it, the other is left without one.
    ViewNavAssign(c, kNavZoomH, WheelGesture(false, 0));
    CHECK(c[kNavZoomH].own && c[kNavScrollH].own && c[kNavScrollH].g.kind == GestureKind::None);
    m = ResolveViewNav(k, c);
    CHECK(ViewNavOwner(m, WheelGesture(false, 0)) == kNavZoomH);
    CHECK(ViewNavGestures(m, kNavScrollH).empty());
    // A key on a scroll row: one notch forward per press.
    ViewNavAssign(c, kNavScrollH, KeyGesture(0x27, kViewModCtrl));  // Ctrl+Right
    m = ResolveViewNav(k, c);
    CHECK(ViewNavKeyRow(m, 0x27, kViewModCtrl) == kNavScrollH);
    ViewNavStep s = ViewNavStepFor(m, kViewModCtrl, 0.0f, 0.0f, {kNavScrollH});
    CHECK(Near(s.scroll_h, 1.0));
    // A key on zoom in: taken from zoom out's REAPER keys.
    ViewNavAssign(c, kNavZoomIn, KeyGesture(0x28, 0));  // Down
    m = ResolveViewNav(k, c);
    CHECK(ViewNavKeyRow(m, 0x28, 0) == kNavZoomIn);
    CHECK(ViewNavKeyRow(m, 0x26, 0) == -1);  // Up was zoom in's REAPER key: zoom in is Own now
    // Back to linked.
    c[kNavZoomIn] = ViewNavChoice{};
    m = ResolveViewNav(k, c);
    CHECK(ViewNavKeyRow(m, 0x26, 0) == kNavZoomIn && ViewNavKeyRow(m, 0x28, 0) == kNavZoomOut);
    // Own, not set: the row runs nothing.
    ViewNavChoices d{};
    d[kNavZoomH].own = true;
    m = ResolveViewNav(k, d);
    CHECK(ViewNavGestures(m, kNavZoomH).empty() && ViewNavOwner(m, WheelGesture(false, 0)) == -1);
}

// A frame's input: signs, reversed / page commands, the horizontal wheel, keys.
static void TestSteps()
{
    const ViewKeymap k = ParseViewKeymap("KEY 255 249 979 0\nKEY 255 252 981 0\nKEY 255 253 978 0\n");
    const ViewNavMap m = ResolveViewNav(k, ViewNavChoices{});
    ViewNavStep s = ViewNavStepFor(m, 0, 1.0f, 0.0f, {});  // wheel up: zoom in
    CHECK(Near(s.zoom_h, 1.0) && !s.any_v());
    s = ViewNavStepFor(m, kViewModCtrl, 1.0f, 0.0f, {});  // 979 reversed: zoom out
    CHECK(Near(s.zoom_h, -1.0));
    s = ViewNavStepFor(m, kViewModShift, -2.0f, 0.0f, {});  // 981 one page: wheel down = two pages right
    CHECK(Near(s.scroll_h_pages, 2.0) && Near(s.scroll_h, 0.0));
    s = ViewNavStepFor(m, kViewModCtrl | kViewModShift, 1.0f, 0.0f, {});  // 978 reversed: up = down
    CHECK(Near(s.scroll_v, 1.0) && !s.any_h());
    s = ViewNavStepFor(m, kViewModAlt, -1.0f, 0.0f, {});  // Alt+wheel down: right
    CHECK(Near(s.scroll_h, 1.0));
    s = ViewNavStepFor(m, 0, 0.0f, -0.5f, {});  // a trackpad swipe right (ImGui H < 0): right
    CHECK(Near(s.scroll_h, 0.5));
    s = ViewNavStepFor(m, kViewModAlt, 0.0f, 1.0f, {});  // Alt+H-wheel left: zoom in
    CHECK(Near(s.zoom_h, 1.0));
    s = ViewNavStepFor(m, 0, 0.0f, 0.0f, {kNavZoomIn, kNavZoomOut, kNavZoomOut});
    CHECK(Near(s.zoom_h, -2.0));
    s = ViewNavStepFor(m, kViewModShift | kViewModAlt | kViewModCtrl, 3.0f, 0.0f, {});  // unbound
    CHECK(!s.any_h() && !s.any_v());
}

static void TestRecord()
{
    const ViewNavMap m = ResolveViewNav(ParseViewKeymap(""), ViewNavChoices{});
    int owner = -2;
    CHECK(ViewNavRecordStep(m, kNavScrollH, WheelGesture(false, kViewModShift), &owner) == NavRecordOutcome::Assigned);
    CHECK(owner == -1);
    CHECK(ViewNavRecordStep(m, kNavScrollH, WheelGesture(false, 0), &owner) == NavRecordOutcome::Conflict);
    CHECK(owner == kNavZoomH);
    CHECK(ViewNavRecordStep(m, kNavScrollH, WheelGesture(true, 0), &owner) == NavRecordOutcome::Assigned);  // its own
    CHECK(ViewNavRecordStep(m, kNavZoomIn, WheelGesture(false, kViewModShift), &owner) == NavRecordOutcome::Ignored);
    CHECK(ViewNavRecordStep(m, kNavZoomIn, KeyGesture(0x10, 0), &owner) == NavRecordOutcome::Ignored);  // Shift alone
    CHECK(ViewNavRecordStep(m, kNavZoomIn, KeyGesture(0x28, 0), &owner) == NavRecordOutcome::Conflict && owner == kNavZoomOut);
    CHECK(ViewNavRecordStep(m, kNavZoomIn, KeyGesture('Z', kViewModCtrl), &owner) == NavRecordOutcome::Assigned);
}

static void TestEncoding()
{
    ViewNavChoice c, d;
    CHECK(EncodeViewNavChoice(c).empty());
    c.own = true;
    CHECK(EncodeViewNavChoice(c) == "own");
    CHECK(DecodeViewNavChoice("own", &d) && d.own && d.g.kind == GestureKind::None);
    c.g = WheelGesture(false, kViewModCtrl | kViewModShift);
    CHECK(EncodeViewNavChoice(c) == "own:Ctrl+Shift+Wheel");
    CHECK(DecodeViewNavChoice("own:Ctrl+Shift+Wheel", &d) && d == c);
    c.g = WheelGesture(true, kViewModAlt);
    CHECK(DecodeViewNavChoice(EncodeViewNavChoice(c).c_str(), &d) && d == c && FormatViewGesture(d.g) == "Alt+H-wheel");
    c.g = KeyGesture(0x26, kViewModCtrl | kViewModAlt);
    CHECK(EncodeViewNavChoice(c) == "own:Ctrl+Alt+38");
    CHECK(DecodeViewNavChoice("own:Ctrl+Alt+38", &d) && d == c && FormatViewGesture(d.g) == "Ctrl+Alt+Up");
    c.g = ViewGesture{};
    c.g.kind = GestureKind::Key;
    c.g.is_char = true;
    c.g.code = ']';
    CHECK(DecodeViewNavChoice(EncodeViewNavChoice(c).c_str(), &d) && d == c && FormatViewGesture(d.g) == "]");
    CHECK(DecodeViewNavChoice("", &d) && !d.own);
    ViewNavChoice keep = d;
    CHECK(!DecodeViewNavChoice("own:Wheels", &d) && d == keep);
    CHECK(!DecodeViewNavChoice("own:16", &d));      // Shift alone
    CHECK(!DecodeViewNavChoice("own:Ctrl+", &d));
    CHECK(!DecodeViewNavChoice("reaper", &d));
    CHECK(!DecodeViewNavChoice("own:999", &d));
}

// 13-45 review: a character binding is claimed by its character (Ctrl / Alt must match, Shift is in
// the character); the Blend box leaves plain / Shift wheels to its panel; reaper-kb.ini is read again
// only once its stamp held still for a second.
static void TestReviewRules()
{
    ViewKeymap k = DefaultViewKeymap();
    ViewKeyBinding plus;
    plus.is_char = true;
    plus.code = '+';
    k.zoom_in.push_back(plus);
    const ViewNavMap m = ResolveViewNav(k, ViewNavChoices{});
    CHECK(ViewNavCharRow(m, '+', 0) == kNavZoomIn);
    CHECK(ViewNavCharRow(m, '+', kViewModShift) == kNavZoomIn);  // Shift typed it
    CHECK(ViewNavCharRow(m, '+', kViewModCtrl) == -1);
    CHECK(ViewNavCharRow(m, '*', 0) == -1);
    CHECK(ViewNavCharRow(m, 0, 0) == -1);

    CHECK(ViewNavWheelYields(0));
    CHECK(ViewNavWheelYields(kViewModShift));
    CHECK(!ViewNavWheelYields(kViewModCtrl));
    CHECK(!ViewNavWheelYields(kViewModAlt));
    CHECK(!ViewNavWheelYields(kViewModCtrl | kViewModShift));

    KbWatch w;
    KbStamp a;
    a.exists = true;
    a.time = 100;
    a.size = 50;
    CHECK(KbShouldRead(w, a, 0.0));  // the first time: at once
    KbApplied(w, a);
    CHECK(!KbShouldRead(w, a, 5.0));  // unchanged
    KbStamp b = a;
    b.time = 200;
    b.size = 10;  // REAPER starts writing
    CHECK(!KbShouldRead(w, b, 10.0));
    KbStamp c = b;
    c.size = 80;  // still writing
    CHECK(!KbShouldRead(w, c, 10.6));
    CHECK(!KbShouldRead(w, c, 11.2));  // 0.6 s still
    CHECK(KbShouldRead(w, c, 11.7));   // a second unchanged: read
    KbApplied(w, c);
    CHECK(!KbShouldRead(w, c, 20.0));
    KbStamp gone;  // deleted: a change like any other, settled first
    CHECK(!KbShouldRead(w, gone, 30.0));
    CHECK(KbShouldRead(w, gone, 31.0));
    KbApplied(w, gone);
    CHECK(!KbShouldRead(w, c, 40.0));
    CHECK(!KbShouldRead(w, gone, 40.5));  // back as applied: the wait is dropped
    CHECK(!w.waiting);
}

int main()
{
    TestTable();
    TestLinkedDefaults();
    TestLinkedFollowsFile();
    TestOwn();
    TestSteps();
    TestRecord();
    TestEncoding();
    TestReviewRules();
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("view_nav: all passed\n");
    return 0;
}
