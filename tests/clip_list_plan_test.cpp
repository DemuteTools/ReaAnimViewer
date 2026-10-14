// SPDX-License-Identifier: MIT
//
// Host test of the animation clip list's plan (src/clip_list_plan.h, story 6-5-10): copies, the
// search, the three groupings, the row texts, the arrows' dashed row and the narrow layouts. The
// rows are the validated mockup's (maquette-liste-clips-animation.html).
//   c++ -std=c++17 -I src tests/clip_list_plan_test.cpp src/clip_list_plan.cpp -o t && ./t

#include "clip_list_plan.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

using namespace rav::cliplist;

namespace {

int g_fails = 0;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);        \
            ++g_fails;                                              \
        }                                                           \
    } while (0)

ClipEntry E(const char* clip, const char* file, const char* track, int num, double pos, double len)
{
    ClipEntry e;
    e.clip = clip;
    e.file = file;
    e.path = std::string("C:\\anims\\") + file;
    e.track = track;
    e.track_num = num;
    e.pos = pos;
    e.len = len;
    return e;
}

// The mockup's project: three tracks, Hero_Run three times.
std::vector<ClipEntry> Mockup()
{
    std::vector<ClipEntry> v;
    v.push_back(E("Golem_Walk", "golem.fbx", "Ennemi", 2, 1.0, 2.0));        // 0
    v.push_back(E("Hero_Idle", "hero_anims.fbx", "Heros", 1, 0.0, 2.4));     // 1
    v.push_back(E("Hero_Run", "hero_anims.fbx", "Heros", 1, 2.4, 1.1));      // 2
    v.push_back(E("Hero_Run", "hero_anims.fbx", "Heros", 1, 3.5, 1.1));      // 3
    v.push_back(E("Hero_Attack_01", "hero_combat.glb", "Heros", 1, 4.6, 0.9));  // 4
    v.push_back(E("Hero_Run", "hero_anims.fbx", "Heros", 1, 5.5, 1.1));      // 5
    v.push_back(E("Hero_Death_Fall_Backwards_Long", "hero_combat.glb", "Heros", 1, 8.0, 3.2));  // 6
    v.push_back(E("Golem_Slam", "golem.fbx", "Ennemi", 2, 4.8, 1.6));        // 7
    v.push_back(E("Sword_Swing", "sword.glb", "Armes", 3, 4.6, 0.9));        // 8
    v[0].track_rgb = 0xF06C6C;
    v[0].has_color = true;
    v[7].track_rgb = 0xF06C6C;
    v[7].has_color = true;
    return v;
}

}  // namespace

int main()
{
    // Copies: the same clip of the same file, numbered in time order.
    {
        std::vector<ClipEntry> v = Mockup();
        NumberCopies(v);
        CHECK(v[2].copy_n == 1 && v[2].copy_of == 3);
        CHECK(v[3].copy_n == 2 && v[3].copy_of == 3);
        CHECK(v[5].copy_n == 3 && v[5].copy_of == 3);
        CHECK(v[1].copy_n == 0 && v[1].copy_of == 0);
        CHECK(CopyBadge(v[3]) == "copy 2/3");
        CHECK(CopyBadge(v[1]).empty());
        // The path's case does not matter (Windows); another clip of the same file is no copy.
        std::vector<ClipEntry> w;
        w.push_back(E("A", "x.fbx", "T", 1, 0.0, 1.0));
        w.push_back(E("A", "X.FBX", "T", 1, 2.0, 1.0));
        w.push_back(E("B", "x.fbx", "T", 1, 4.0, 1.0));
        NumberCopies(w);
        CHECK(w[0].copy_of == 2 && w[1].copy_of == 2 && w[2].copy_of == 0);
        // Numbered again from scratch (a copy deleted).
        w.pop_back();
        w.pop_back();
        NumberCopies(w);
        CHECK(w[0].copy_of == 0 && w[0].copy_n == 0);
    }

    // Grouping by track (the default): the arrange's order, rows in time order, the colour kept.
    {
        std::vector<ClipEntry> v = Mockup();
        const ListView         lv = BuildListView(v, Grouping::Track, "");
        CHECK(lv.total == 9 && lv.shown == 9);
        CHECK(lv.groups.size() == 3);
        CHECK(lv.groups[0].label == "Heros" && lv.groups[1].label == "Ennemi" && lv.groups[2].label == "Armes");
        CHECK(lv.groups[1].has_color && lv.groups[1].rgb == 0xF06C6C && !lv.groups[0].has_color);
        CHECK((lv.groups[0].rows == std::vector<int>{1, 2, 3, 4, 5, 6}));
        CHECK((lv.groups[1].rows == std::vector<int>{0, 7}));
        CHECK((lv.order == std::vector<int>{1, 2, 3, 4, 5, 6, 0, 7, 8}));
        CHECK(GroupingFromInt(0) == Grouping::Track && GroupingFromInt(7) == Grouping::Track);
    }

    // Grouping by file: files by name, the copies under each other.
    {
        std::vector<ClipEntry> v = Mockup();
        const ListView         lv = BuildListView(v, Grouping::File, "");
        CHECK(lv.groups.size() == 4);
        CHECK(lv.groups[0].label == "golem.fbx" && lv.groups[1].label == "hero_anims.fbx" &&
              lv.groups[2].label == "hero_combat.glb" && lv.groups[3].label == "sword.glb");
        CHECK((lv.groups[1].rows == std::vector<int>{1, 2, 3, 5}));
        // Same time: the upper track first.
        CHECK((lv.groups[2].rows == std::vector<int>{4, 6}));
        CHECK(SubLine(v[0], Grouping::File) == "Ennemi \xC2\xB7 2.0 s");
        CHECK(SubLine(v[0], Grouping::Track) == "golem.fbx \xC2\xB7 2.0 s");
    }

    // No grouping: one headerless group in time order (a tie: the upper track first).
    {
        std::vector<ClipEntry> v = Mockup();
        const ListView         lv = BuildListView(v, Grouping::None, "");
        CHECK(lv.groups.size() == 1 && lv.groups[0].label.empty());
        CHECK((lv.order == std::vector<int>{1, 0, 2, 3, 4, 8, 7, 5, 6}));
    }

    // The search: clip, file or track, any case, from the first letter; empty groups go.
    {
        std::vector<ClipEntry> v = Mockup();
        ListView lv = BuildListView(v, Grouping::Track, "run");
        CHECK(lv.shown == 3 && lv.total == 9 && lv.groups.size() == 1);
        lv = BuildListView(v, Grouping::Track, "  GOLEM ");
        CHECK(lv.shown == 2 && lv.groups.size() == 1 && lv.groups[0].label == "Ennemi");
        lv = BuildListView(v, Grouping::Track, "armes");  // a track's name
        CHECK(lv.shown == 1 && lv.order.front() == 8);
        lv = BuildListView(v, Grouping::Track, ".glb");  // a file's
        CHECK(lv.shown == 3);
        lv = BuildListView(v, Grouping::File, "zzz");
        CHECK(lv.shown == 0 && lv.groups.empty() && lv.order.empty());
        lv = BuildListView(v, Grouping::Track, "h");
        CHECK(lv.shown == 6);  // the six Hero_* rows; no other clip, file or track has an h
    }

    // Texts.
    {
        CHECK(FormatPosition(0.0) == "0:00.0");
        CHECK(FormatPosition(2.4) == "0:02.4");
        CHECK(FormatPosition(65.25) == "1:05.3");
        CHECK(FormatPosition(59.96) == "1:00.0");
        CHECK(FormatPosition(-3.0) == "0:00.0");
        CHECK(FormatPosition(3725.0) == "1:02:05.0");
        CHECK(FormatLength(1.1) == "1.1 s");
        CHECK(FormatLength(-1.0) == "0.0 s");
    }

    // The arrows: from the current row, then the dashed one; both ends hold.
    {
        const std::vector<int> o{1, 2, 3, 0};
        CHECK(StepRow(o, -1, 2, +1) == 3);
        CHECK(StepRow(o, -1, 2, -1) == 1);
        CHECK(StepRow(o, 3, 2, +1) == 0);
        CHECK(StepRow(o, 0, 2, +1) == 0);   // the bottom holds
        CHECK(StepRow(o, 1, -1, -1) == 1);  // the top holds
        CHECK(StepRow(o, -1, 9, +1) == 1);  // current row filtered out: the first row
        CHECK(StepRow(o, -1, -1, -1) == 1);
        CHECK(StepRow({}, -1, -1, +1) == -1);
    }

    // The panel's width: 270 px, at most 70 % of the view, nearly the whole view under 300 px.
    {
        CHECK(PanelWidth(1200.0f, 1.0f) == 270.0f);
        CHECK(PanelWidth(420.0f, 1.0f) == 270.0f);         // 70 % would be 294
        CHECK(PanelWidth(360.0f, 1.0f) == 0.7f * 360.0f);  // 252
        CHECK(PanelWidth(260.0f, 1.0f) == 200.0f);   // at least 200 while the view has room
        CHECK(PanelWidth(200.0f, 1.0f) == 172.0f);   // nearly the whole view (the button's 28 px)
        CHECK(PanelWidth(1200.0f, 2.0f) == 540.0f);  // a 200 % screen
        CHECK(PanelWidth(500.0f, 2.0f) == 400.0f);   // under 600 physical px at 200 %
        CHECK(PanelWidth(0.0f, 1.0f) == 0.0f);
        CHECK(PanelWidth(10.0f, 1.0f) == 0.0f);
        // Continuous and never shrinking while the view grows (no jump at 300 px or anywhere).
        float prev = 0.0f;
        bool  smooth = true;
        for (float v = 0.0f; v <= 1200.0f; v += 0.25f) {
            const float pw = PanelWidth(v, 1.0f);
            if (pw < prev - 1e-3f || pw - prev > 0.25f + 1e-3f || (pw > 0.0f && pw > v - 28.0f + 1e-3f)) smooth = false;
            prev = pw;
        }
        CHECK(smooth);
    }

    // The flat lines and the ones on screen.
    {
        ListView lv;
        lv.groups.resize(2);
        lv.groups[0].label = "A";
        lv.groups[0].rows = {3, 1};
        lv.groups[1].label = "B";
        lv.groups[1].rows = {0, 2, 4};
        lv.order = {3, 1, 0, 2, 4};
        const std::vector<ListLine> lines = FlattenListView(lv);
        CHECK(lines.size() == 7u);
        CHECK(lines[0].row == -1 && lines[0].group == 0);
        CHECK(lines[1].row == 3 && lines[3].row == -1 && lines[3].group == 1 && lines[6].row == 4);
        float total = 0.0f;
        const std::vector<float> tops = LineTops(lines, 10.0f, 20.0f, &total);
        CHECK(total == 2 * 10.0f + 5 * 20.0f);
        CHECK(tops[1] == 10.0f && tops[3] == 50.0f && tops[4] == 60.0f);
        int f = -1, l = -1;
        VisibleLines(tops, total, 0.0f, 15.0f, &f, &l);
        CHECK(f == 0 && l == 2);
        VisibleLines(tops, total, 55.0f, 85.0f, &f, &l);  // the end of B's header to row 0's middle
        CHECK(f == 3 && l == 6);
        VisibleLines(tops, total, 500.0f, 600.0f, &f, &l);
        CHECK(f == l);
        VisibleLines(std::vector<float>(), 0.0f, 0.0f, 100.0f, &f, &l);
        CHECK(f == 0 && l == 0);
        ListView none;
        none.groups.resize(1);
        none.groups[0].rows = {0, 1};
        CHECK(FlattenListView(none).size() == 2u);  // no header without a label
    }

    // The button: centred between the menu and the switch; just under the menu when they are close.
    {
        CHECK(ButtonCentreY(46.0f, 346.0f, 22.0f, 1.0f) == 196.0f);
        CHECK(ButtonCentreY(46.0f, 150.0f, 22.0f, 1.0f) == 46.0f + 6.0f + 11.0f);
        CHECK(ButtonFits(46.0f, 150.0f, 22.0f, 1.0f));
        CHECK(ButtonFits(46.0f, 46.0f + 32.0f, 22.0f, 1.0f));
        CHECK(!ButtonFits(46.0f, 46.0f + 31.0f, 22.0f, 1.0f));  // it would touch the switch: not drawn
        CHECK(!ButtonFits(46.0f, 20.0f, 22.0f, 1.0f));
        // 6-5-10b review: open, the button is wholly outside the panel (its left edge 2 px right of
        // the panel's right edge, never over the rows or the scrollbar), and still in the view.
        for (float v = 0.0f; v <= 1200.0f; v += 0.5f) {
            for (float dp : {1.0f, 1.5f, 2.0f}) {
                const float pw = PanelWidth(v, dp);
                if (pw <= 0.0f) continue;
                const float cx = OpenButtonCentreX(pw, 22.0f * dp, dp);
                CHECK(cx - 11.0f * dp >= pw + 2.0f * dp - 1e-3f);
                CHECK(cx + 11.0f * dp <= v + 1e-3f);
            }
        }
    }

    // 6-5-10b review: the keys a list with the keyboard types into its search (not shortcuts).
    {
        CHECK(ListTypesKey('L', false, false));
        CHECK(ListTypesKey('L', false, false) && ListTypesKey('W', false, false) && ListTypesKey('0', false, false));
        CHECK(ListTypesKey(0xBD, false, false));   // '-'
        CHECK(ListTypesKey(0xDE, false, false));   // quote
        CHECK(ListTypesKey(0xE2, false, false));   // < > (ISO keyboards)
        CHECK(!ListTypesKey('L', true, false));    // Ctrl+L: a shortcut
        CHECK(!ListTypesKey('L', false, true));    // Alt+L: a shortcut
        CHECK(!ListTypesKey(0x20, false, false));  // Space plays
        CHECK(!ListTypesKey(0x1B, false, false));  // Esc: the list's own
        CHECK(!ListTypesKey(0x0D, false, false));  // Enter: the list's own
        CHECK(!ListTypesKey(0x26, false, false));  // Up
        CHECK(!ListTypesKey(0x60, false, false));  // numpad 0
        CHECK(!ListTypesKey(0x6A, false, false));  // numpad *
        CHECK(!ListTypesKey(0x7A, false, false));  // F11 (full screen)
        CHECK(!ListTypesKey(0x2E, false, false));  // Delete
        CHECK(!ListTypesKey(0x08, false, false));  // Backspace
    }

    // 6-5-10b: folded groups. A fold keeps only the header, the arrows skip its rows, a search opens
    // every group it kept, Expand / Collapse all, one header's click.
    {
        std::vector<ClipEntry> v = Mockup();
        v[1].track_key = v[2].track_key = v[3].track_key = v[4].track_key = v[5].track_key = v[6].track_key = "{H}";
        CHECK(GroupKey(v[1], Grouping::Track) == "t:{H}");
        CHECK(GroupKey(v[0], Grouping::Track) == "t:#2");  // no GUID: its number
        CHECK(GroupKey(v[4], Grouping::File) == "f:hero_combat.glb");
        CHECK(GroupKey(v[4], Grouping::None).empty());
        ListView lv = BuildListView(v, Grouping::Track, "");
        CHECK(lv.groups.size() == 3u && lv.groups[0].key == "t:{H}" && lv.groups[1].key == "t:#2");
        FoldSet folded;
        CHECK(!CanExpandAll(lv, folded, false) && CanCollapseAll(lv, folded, false));
        ToggleFold("t:{H}", &folded);
        ApplyFolds(lv, folded, false);
        CHECK(lv.groups[0].folded && !lv.groups[1].folded);
        CHECK(lv.order.size() == 3u && lv.order[0] == 0);  // Heros' six rows are out of the arrows' walk
        std::vector<ListLine> lines = FlattenListView(lv);
        CHECK(lines.size() == 3u + 3u);                    // three headers, Ennemi's two rows, Armes' one
        CHECK(lines[0].row == -1 && lines[1].row == -1 && lines[1].group == 1);
        CHECK(CanExpandAll(lv, folded, false) && CanCollapseAll(lv, folded, false));
        CHECK(!CanExpandAll(lv, folded, true) && !CanCollapseAll(lv, folded, true));  // off while searching
        // A search opens the folded group that holds a match; the fold is kept for later.
        ListView found = BuildListView(v, Grouping::Track, "run");
        ApplyFolds(found, folded, /*searching=*/true);
        CHECK(found.groups.size() == 1u && !found.groups[0].folded && found.order.size() == 3u);
        CHECK(folded.count("t:{H}") == 1u);
        ApplyFolds(lv, folded, false);
        CHECK(lv.groups[0].folded);
        SetAllFolds(lv, true, &folded);
        ApplyFolds(lv, folded, false);
        CHECK(lv.order.empty() && FlattenListView(lv).size() == 3u && !CanCollapseAll(lv, folded, false));
        SetAllFolds(lv, false, &folded);
        ApplyFolds(lv, folded, false);
        CHECK(folded.empty() && lv.order.size() == 9u && FlattenListView(lv).size() == 12u);
        ToggleFold("t:{H}", &folded);
        ToggleFold("t:{H}", &folded);
        CHECK(folded.empty());
        // A file group's fold is its own (Track's folds stay when File is shown).
        folded.insert("t:{H}");
        ListView by_file = BuildListView(v, Grouping::File, "");
        ApplyFolds(by_file, folded, false);
        CHECK(by_file.order.size() == 9u);
        SetAllFolds(by_file, true, &folded);
        CHECK(folded.count("t:{H}") == 1u && folded.count("f:golem.fbx") == 1u && folded.size() == 1u + 4u);
        // No grouping: nothing folds, no Expand / Collapse all.
        ListView flat = BuildListView(v, Grouping::None, "");
        ApplyFolds(flat, folded, false);
        CHECK(flat.order.size() == 9u && !CanExpandAll(flat, folded, false) && !CanCollapseAll(flat, folded, false));
    }

    // 6-5-10b: REAPER's selection focused in the list: the first row newly selected.
    {
        CHECK(SelectionFocus({}, {4}) == 4);
        CHECK(SelectionFocus({4}, {4}) == -1);          // same selection: nothing moves
        CHECK(SelectionFocus({4, 7}, {4}) == -1);       // a deselection: nothing moves
        CHECK(SelectionFocus({4}, {4, 7, 2}) == 7);     // the first one added (REAPER's order)
        CHECK(SelectionFocus({4}, {}) == -1);
        CHECK(SelectionFocus({}, {-1, 3}) == 3);        // a selected item that is not in the list
    }

    // 6-5-10b: the panel's height: between the menu button and the bottom-left controls, never over
    // the strip; a short window reaches over the controls, then over the menu.
    {
        PanelSpan s = PanelVerticalSpan(44.0f, 600.0f, 700.0f, 1.0f);
        CHECK(s.top == 52.0f && s.bottom == 592.0f);
        s = PanelVerticalSpan(88.0f, 1200.0f, 1400.0f, 2.0f);  // 200 % UI scale
        CHECK(s.top == 104.0f && s.bottom == 1184.0f);
        // Tagging: the switch sits above the strip (view_bottom = the strip's top); a taller strip
        // moves the switch up and the panel's bottom with it.
        const PanelSpan a = PanelVerticalSpan(44.0f, 400.0f, 450.0f, 1.0f);
        const PanelSpan b = PanelVerticalSpan(44.0f, 300.0f, 350.0f, 1.0f);
        CHECK(a.bottom == 392.0f && b.bottom == 292.0f && a.top == b.top);
        // Short: over the controls (down to the strip's top), never past it.
        s = PanelVerticalSpan(44.0f, 180.0f, 230.0f, 1.0f);
        CHECK(s.top == 52.0f && s.bottom == 212.0f);
        s = PanelVerticalSpan(44.0f, 150.0f, 180.0f, 1.0f);
        CHECK(s.bottom == 180.0f && s.top == 20.0f);  // then over the menu
        s = PanelVerticalSpan(44.0f, 60.0f, 100.0f, 1.0f);
        CHECK(s.bottom == 100.0f && s.top == 0.0f);
        s = PanelVerticalSpan(44.0f, 0.0f, 0.0f, 1.0f);  // no room at all: still a valid 1 px span
        CHECK(s.bottom == 1.0f && s.top == 0.0f);
        // Continuous while the window grows (no jump at the 160 px threshold).
        float prev_b = -1.0f;
        bool  smooth = true;
        for (float h = 50.0f; h < 900.0f; h += 0.5f) {
            const PanelSpan t = PanelVerticalSpan(44.0f, h - 60.0f, h - 20.0f, 1.0f);
            if (t.bottom > h - 20.0f + 1e-3f || t.top < 0.0f || t.bottom <= t.top) smooth = false;
            if (prev_b >= 0.0f && (t.bottom < prev_b - 1e-3f || t.bottom - prev_b > 0.5f + 1e-3f)) smooth = false;
            prev_b = t.bottom;
        }
        CHECK(smooth);
    }

    // 6-5-10c: the spans with no Model / Skeleton switch (Video, Library: the status icon's top is the
    // bottom-left controls' top, 12 + 22 px over the view's bottom), and with an edge not known yet.
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        // Video view, 600 px viewer, a 64 + 16 px strip band: the view's bottom at 520, the icon at 486.
        PanelSpan s = PanelVerticalSpan(44.0f, 520.0f - 34.0f, 520.0f, 1.0f);
        CHECK(s.top == 52.0f && s.bottom == 478.0f);
        // Library view: no strip, the icon over the window's bottom.
        s = PanelVerticalSpan(44.0f, 600.0f - 34.0f, 600.0f, 1.0f);
        CHECK(s.top == 52.0f && s.bottom == 558.0f);
        // The switch's place not known (not a number): the view's bottom stands in (never a 1 px panel).
        s = PanelVerticalSpan(44.0f, nan, 600.0f, 1.0f);
        CHECK(s.top == 52.0f && s.bottom == 592.0f);
        s = PanelVerticalSpan(44.0f, 560.0f, nan, 1.0f);
        CHECK(s.top == 52.0f && s.bottom == 552.0f);
        s = PanelVerticalSpan(nan, nan, nan, 1.0f);
        CHECK(std::isfinite(s.top) && std::isfinite(s.bottom) && s.bottom - s.top >= 160.0f);
        // The switch's top 0 (laid out nowhere): the panel still gets its usable height in the view.
        s = PanelVerticalSpan(44.0f, 0.0f, 600.0f, 1.0f);
        CHECK(s.top == 52.0f && s.bottom == 212.0f);
    }

    // 6-5-10c: the menu button: the corner icons' 24 px button, centred on the top row, 18 px in; the
    // open menu's panel 8 px around it; the list 8 px under it, at every scale.
    {
        MenuButtonBox b = MenuButtonAt(32.0f, 1.0f);  // 100 %: 14 + 10 + 16 / 2
        CHECK(b.x == 18.0f && b.y == 20.0f && b.size == 24.0f && b.bottom == 44.0f);
        CHECK(b.panel_x == 10.0f && b.panel_y == 12.0f && b.trail == 50.0f);
        b = MenuButtonAt(48.0f, 1.5f);  // 150 %
        CHECK(b.x == 27.0f && b.y == 30.0f && b.size == 36.0f && b.bottom == 66.0f);
        CHECK(b.panel_x == 15.0f && b.panel_y == 18.0f && b.trail == 75.0f);
        b = MenuButtonAt(30.5f, 1.0f);  // a smaller font: whole pixels, still centred
        CHECK(b.y == 19.0f || b.y == 18.0f);
        b = MenuButtonAt(4.0f, 1.0f);   // never closer than 10 px to the window's top
        CHECK(b.y == 10.0f && b.panel_y == 2.0f);
        for (float dp : {1.0f, 1.25f, 1.5f, 2.0f}) {
            const MenuButtonBox m = MenuButtonAt(32.0f * dp, dp);
            const PanelSpan     s = PanelVerticalSpan(m.bottom, 600.0f * dp, 700.0f * dp, dp);
            CHECK(s.top - m.bottom == 8.0f * dp);       // the gap Antho asked for (~8 px)
            CHECK(m.y + m.size * 0.5f == 32.0f * dp);   // centred on the top row
            CHECK(m.trail - (m.x + m.size) == 8.0f * dp);
        }
    }

    // 6-5-10c: a project load REAPER reports closes the list only when another project shows.
    {
        int        a = 0, b = 0;
        ProjectLook before;
        before.proj = &a;
        before.path = "C:\\proj\\walk.rpp";
        before.items = 12;
        ProjectLook now = before;
        CHECK(ProjectLoadVerdict(before, now, 9, 9) == LoadVerdict::Stays);   // same file, same items
        CHECK(ProjectLoadVerdict(before, now, 9, 5) == LoadVerdict::Stays);   // most still there
        CHECK(ProjectLoadVerdict(before, now, 9, 4) == LoadVerdict::Replaced);  // the same file read again
        CHECK(ProjectLoadVerdict(before, now, 9, 0) == LoadVerdict::Replaced);
        CHECK(ProjectLoadVerdict(before, now, 0, 0) == LoadVerdict::Stays);   // the list was closed: no rows
        now.path = "C:\\proj\\run.rpp";
        CHECK(ProjectLoadVerdict(before, now, 9, 9) == LoadVerdict::OtherFile);
        now = before;
        now.path = "";                                                       // File > New after a saved one
        now.items = 0;
        CHECK(ProjectLoadVerdict(before, now, 9, 0) == LoadVerdict::OtherFile);
        before.path = "";                                                    // File > New, never saved
        CHECK(ProjectLoadVerdict(before, now, 0, 0) == LoadVerdict::Emptied);
        now = before;
        now.proj = &b;                                                       // a project opened in a new tab
        CHECK(ProjectLoadVerdict(before, now, 9, 9) == LoadVerdict::OtherTab);
        ProjectLook none;                                                    // nothing seen before (viewer never drawn)
        CHECK(ProjectLoadVerdict(none, before, 0, 0) == LoadVerdict::OtherTab);
        CHECK(std::string(LoadVerdictText(LoadVerdict::OtherFile)) == "another file");
        CHECK(std::string(LoadVerdictText(LoadVerdict::Stays)) == "nothing opened");
    }

    // 6-5-10c: the log's note on a close.
    {
        CHECK(CloseNote("Main", -1, -1.0) == "Main view");
        CHECK(CloseNote("Tagging", 3, 0.214) == "Tagging view, 3 frames (0.21 s) after a row click");
        CHECK(CloseNote("Video", 1, 0.0) == "Video view, 1 frame (0.00 s) after a row click");
        CHECK(CloseNote("Main", 900, 12.0) == "Main view");  // long after: not the click's
        CHECK(CloseNote("", -1, -1.0) == "viewer");
    }

    if (g_fails == 0) std::printf("clip_list_plan: all checks passed\n");
    return g_fails == 0 ? 0 : 1;
}
