// SPDX-License-Identifier: MIT
//
// Host test of the animation clip list's plan (src/clip_list_plan.h, story 6-5-10): copies, the
// search, the three groupings, the row texts, the arrows' dashed row and the narrow layouts. The
// rows are the validated mockup's (maquette-liste-clips-animation.html).
//   c++ -std=c++17 -I src tests/clip_list_plan_test.cpp src/clip_list_plan.cpp -o t && ./t

#include "clip_list_plan.h"

#include <cstdio>
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
        CHECK(PanelWidth(260.0f, 1.0f) == 244.0f);
        CHECK(PanelWidth(1200.0f, 2.0f) == 540.0f);  // a 200 % screen
        CHECK(PanelWidth(500.0f, 2.0f) == 468.0f);   // under 600 physical px at 200 %
        CHECK(PanelWidth(0.0f, 1.0f) == 0.0f);
        CHECK(PanelWidth(10.0f, 1.0f) == 0.0f);
    }

    // The button: centred between the menu and the switch; just under the menu when they are close.
    {
        CHECK(ButtonCentreY(46.0f, 346.0f, 22.0f, 1.0f) == 196.0f);
        CHECK(ButtonCentreY(46.0f, 150.0f, 22.0f, 1.0f) == 46.0f + 6.0f + 11.0f);
    }

    if (g_fails == 0) std::printf("clip_list_plan: all checks passed\n");
    return g_fails == 0 ? 0 : 1;
}
