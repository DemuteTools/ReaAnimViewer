// SPDX-License-Identifier: MIT
//
// Host tests of src/library_sync_select.h (story 13-77c): the Sync with file window's selection model, on a synthetic
// Footstep diff shaped like the mockup's "Les deux" state.

#include "library_sync_select.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) {                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);             \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static void Add(SyncDiff* d, const std::string& key, SyncSide side, const std::string& node, const std::string& text,
                std::vector<std::string> needs = {}, bool adds = false, const std::string& subject = "", const std::string& label = "")
{
    SyncChange c;
    c.key = key;
    c.side = side;
    c.node = node;
    c.text = text;
    c.needs = std::move(needs);
    c.adds_container = adds;
    c.subject = subject.empty() ? node : subject;
    c.label = label;
    c.here_text = "h";
    c.file_text = "f";
    d->changes.push_back(c);
}

// root R: Walk_Concrete (wc: here Sound, both Gain), Walk_Wood (ww: both Gain, 2 file), Run_Gravel (rg, new here, 2
// Sounds, its Switch case under swRun), Walk_Mud (wm, new in the file, under swWalk, with the Switch case and the value).
static SyncDiff Fixture()
{
    SyncDiff d;
    d.root = "R";
    d.mine_nodes = {"R", "wc", "ww", "swRun", "swWalk", "rg"};
    d.parent = {{"wc", "R"}, {"ww", "R"}, {"swRun", "R"}, {"swWalk", "R"}, {"rg", "R"}, {"wm", "swWalk"}};
    Add(&d, "X|wcs4", SyncSide::Here, "wc", "Sound FS_Walk_Concrete_04 added");
    Add(&d, "F|wc|gain", SyncSide::Both, "wc", "Gain: here -6 - file -8");
    Add(&d, "F|ww|gain", SyncSide::Both, "ww", "Gain of Walk_Wood", {}, false, "", "Gain");
    Add(&d, "F|ww|avoid", SyncSide::File, "ww", "Avoid repeat 1 -> 2");
    Add(&d, "F|ww|fades", SyncSide::File, "ww", "Fades changed");
    Add(&d, "X|rg", SyncSide::Here, "rg", "Random Run_Gravel added", {}, true, "rg", "Run_Gravel");
    Add(&d, "X|rgs1", SyncSide::Here, "rg", "Sound FS_Run_Gravel_01 added", {"X|rg"});
    Add(&d, "X|rgs2", SyncSide::Here, "rg", "Sound FS_Run_Gravel_02 added", {"X|rg"});
    Add(&d, "C|swRun|Gravel", SyncSide::Here, "swRun", "Switch value Gravel added", {"X|rg"});
    Add(&d, "GX|gS", SyncSide::File, "grp:gS", "Switch Group Surface: Mud added");
    Add(&d, "VX|gS|Mud", SyncSide::File, "grp:gS", "Value Mud added", {});
    Add(&d, "X|wm", SyncSide::File, "swWalk", "Random Walk_Mud added", {}, true, "wm", "Walk_Mud");
    Add(&d, "C|swWalk|Mud", SyncSide::File, "swWalk", "Switch value Mud added", {"VX|gS|Mud", "X|wm"});
    return d;
}

static void TestTick()
{
    const SyncDiff d = Fixture();
    SyncSelection  s;
    SyncEffect     fx = s.Toggle(d, "X|rgs1");
    CHECK(s.IsOn("X|rgs1") && s.IsOn("X|rg"));
    CHECK(s.Tick("X|rg")->auto_ && s.Tick("X|rg")->by == "X|rgs1" && s.Tick("X|rg")->needs_why);
    CHECK(s.Tick("X|rg")->dir == SyncDir::Up);
    CHECK(fx.ticked.size() == 1 && fx.ticked[0] == "X|rg");
    CHECK(SyncEffectToast(d, fx) == "Also ticked: Random Run_Gravel added");
    CHECK(!s.IsOn("X|rgs2"));
    // another ticked change needs the Random too
    s.Toggle(d, "X|rgs2");
    CHECK(s.NeededBy(d, "X|rg").size() == 2);
    // case with two needs, in the file's direction
    s.Toggle(d, "C|swWalk|Mud");
    CHECK(s.IsOn("VX|gS|Mud") && s.IsOn("X|wm"));
    CHECK(s.Tick("X|wm")->dir == SyncDir::Down);
    const auto p = s.Picks(d);
    CHECK(p.size() == 6);
    CHECK(CloseSelection(d, p) == p);  // the engine's closure changes nothing: directions are resolved
}

static void TestUntickPrunes()
{
    const SyncDiff d = Fixture();
    SyncSelection  s;
    s.Toggle(d, "X|rgs1");
    s.Toggle(d, "X|rgs2");
    SyncEffect fx = s.Toggle(d, "X|rg");  // untick the needed one
    CHECK(!s.IsOn("X|rg") && !s.IsOn("X|rgs1") && !s.IsOn("X|rgs2"));
    CHECK(fx.unticked.size() == 2);
    CHECK(SyncEffectToast(d, fx).find("Also unticked, they went together: ") == 0);
    // unticking a Sound that ticked the Random for you: the Random (auto, its asker gone) goes too
    s.Toggle(d, "X|rgs1");
    s.Toggle(d, "X|rgs1");
    CHECK(!s.IsOn("X|rg") && !s.IsOn("X|rgs1"));
    // ... but not while another still needs it
    s.Toggle(d, "X|rgs1");
    s.Toggle(d, "X|rgs2");
    s.Toggle(d, "X|rgs1");
    CHECK(s.IsOn("X|rg") && s.IsOn("X|rgs2"));
}

static void TestDropPulls()
{
    const SyncDiff d = Fixture();
    SyncSelection  s;
    s.Toggle(d, "X|rgs1");
    SyncEffect fx = s.Choose(d, "X|rg", SyncDir::Down);  // drop mine
    CHECK(s.Tick("X|rg")->dir == SyncDir::Down && !s.Tick("X|rg")->auto_);
    for (const char* k : {"X|rgs1", "X|rgs2", "C|swRun|Gravel"}) {
        CHECK(s.IsOn(k) && s.Tick(k)->dir == SyncDir::Down);
    }
    CHECK(!s.Tick("X|rgs2")->needs_why && s.Tick("X|rgs2")->by == "X|rg");
    CHECK(fx.ticked.size() == 2);  // rgs2 and the case (rgs1 was ticked, its direction turned)
    const auto p = s.Picks(d);
    CHECK(CloseSelection(d, p) == p);
}

static void TestBoth()
{
    const SyncDiff d = Fixture();
    SyncSelection  s;
    SyncEffect     fx = s.Toggle(d, "F|ww|gain");
    CHECK(fx.need_side && !s.IsOn("F|ww|gain"));
    CHECK(SyncEffectToast(d, fx) == "Changed on both sides: pick \xE2\x86\x91 Send mine or \xE2\x86\x93 Get file's");
    // a parent ticks the file ones and leaves the unsided both
    const auto ww = SyncKeysUnder(d, "ww");
    CHECK(ww.size() == 3);
    fx = s.ToggleGroup(d, ww);
    CHECK(s.IsOn("F|ww|avoid") && s.IsOn("F|ww|fades") && !s.IsOn("F|ww|gain"));
    CHECK(fx.skipped_both == 1 && SyncEffectToast(d, fx) == "1 changed on both sides: pick a side for each");
    CHECK(s.TriState(d, ww) == SyncTri::Some);
    // a side is picked: it ticks, the parent is all
    s.Choose(d, "F|ww|gain", SyncDir::Up);
    CHECK(s.IsOn("F|ww|gain") && s.TriState(d, ww) == SyncTri::All);
    // all on -> all off
    s.ToggleGroup(d, ww);
    CHECK(s.TriState(d, ww) == SyncTri::None);
    // the chosen side is remembered: ticking again uses it
    s.Toggle(d, "F|ww|gain");
    CHECK(s.IsOn("F|ww|gain") && s.Tick("F|ww|gain")->dir == SyncDir::Up);
}

static void TestGroupDir()
{
    const SyncDiff d = Fixture();
    SyncSelection  s;
    int            up = 0, down = 0;
    SyncDirCounts(d, SyncKeysUnder(d, "wc"), &up, &down);
    CHECK(up == 2 && down == 1);  // here + both ; both
    s.GroupDir(d, SyncKeysUnder(d, "wc"), SyncDir::Up);
    CHECK(s.IsOn("X|wcs4") && s.IsOn("F|wc|gain"));
    CHECK(s.Tick("F|wc|gain")->dir == SyncDir::Up);
    // ↓ file's on the same parent: the file ones (none here) and the both one, turned
    s.GroupDir(d, SyncKeysUnder(d, "wc"), SyncDir::Down);
    CHECK(s.Tick("F|wc|gain")->dir == SyncDir::Down && s.Tick("X|wcs4")->dir == SyncDir::Up);
    // the whole library
    s.GroupDir(d, SyncKeysUnder(d, "*"), SyncDir::Down);
    CHECK(s.IsOn("X|wm") && s.IsOn("F|ww|avoid") && !s.IsOn("X|rgs1"));
}

static void TestDirOnlyItsSide()
{
    SyncDiff d;
    d.root = "R";
    d.parent = {{"c", "R"}};
    Add(&d, "A", SyncSide::Here, "c", "a");
    Add(&d, "B", SyncSide::File, "c", "b");
    Add(&d, "C", SyncSide::Both, "c", "c");
    const auto keys = SyncKeysAt(d, "c");
    CHECK((SyncKeysForDir(d, keys, SyncDir::Up) == std::vector<std::string>{"A", "C"}));
    CHECK((SyncKeysForDir(d, keys, SyncDir::Down) == std::vector<std::string>{"B", "C"}));
    SyncSelection s;
    s.GroupDir(d, keys, SyncDir::Up);
    CHECK(s.IsOn("A") && s.IsOn("C") && !s.IsOn("B"));
    CHECK(s.Picks(d).count("B") == 0 && s.Picks(d).at("C") == SyncDir::Up);
    SyncSelection t;
    t.GroupDir(d, keys, SyncDir::Down);
    CHECK(t.IsOn("B") && t.IsOn("C") && !t.IsOn("A") && t.Tick("C")->dir == SyncDir::Down);
}

static void TestSyncAllTriState()
{
    const SyncDiff d = Fixture();
    const auto     all = SyncKeysUnder(d, "*");
    CHECK(all.size() == d.changes.size());
    SyncSelection s;
    CHECK(s.TriState(d, all) == SyncTri::None);
    s.Toggle(d, "F|ww|avoid");
    s.Toggle(d, "F|ww|fades");
    s.Toggle(d, "X|wcs4");
    CHECK(s.TriState(d, all) == SyncTri::Some);
    CHECK(s.TriState(d, {}) == SyncTri::Disabled);
    const SyncCountsTicked c = s.Counts(d);
    CHECK(c.ticked == 3 && c.up == 1 && c.down == 2 && c.auto_ == 0);
    CHECK(SyncFooterText(c, 13) == "3 ticked: \xE2\x86\x91 1 to the file, \xE2\x86\x93 2 taken here");
    s.Toggle(d, "X|rgs1");
    CHECK(SyncFooterText(s.Counts(d), 13).find("1 ticked for you because another needs it") != std::string::npos);
    CHECK(SyncFooterText(SyncSelection().Counts(d), 4) == "Nothing ticked. 4 changes waiting.");
    CHECK(SyncFooterText(SyncSelection().Counts(d), 1) == "Nothing ticked. 1 change waiting.");
    CHECK(SyncFooterText(SyncSelection().Counts(d), 0) == "Nothing left: this project and the file agree.");
}

static void TestRekeyAfterApply()
{
    SyncDiff      d = Fixture();
    SyncSelection s;
    s.Toggle(d, "X|rgs1");
    s.Toggle(d, "F|ww|avoid");
    s.Toggle(d, "X|wcs4");
    // Apply the Run_Gravel family: those changes are gone; the other tick stays only by key, "applied" ones are gone
    SyncDiff rest = d;
    rest.changes.erase(std::remove_if(rest.changes.begin(), rest.changes.end(),
                                      [](const SyncChange& c) { return c.key.find("rg") != std::string::npos; }),
                       rest.changes.end());
    SyncSelection after = s;
    after.Rekey(rest);
    CHECK(!after.IsOn("X|rgs1") && !after.IsOn("X|rg"));
    CHECK(after.IsOn("F|ww|avoid") && after.IsOn("X|wcs4"));
    // a change whose side moved drops its tick
    SyncDiff moved = d;
    for (SyncChange& c : moved.changes)
        if (c.key == "F|ww|avoid") c.side = SyncSide::Both;
    SyncSelection m = s;
    m.Rekey(moved);
    CHECK(!m.IsOn("F|ww|avoid") && m.IsOn("X|wcs4"));
    // a tick whose need vanished is pruned
    SyncDiff noneed = d;
    noneed.changes.erase(std::remove_if(noneed.changes.begin(), noneed.changes.end(), [](const SyncChange& c) { return c.key == "X|rg"; }),
                         noneed.changes.end());
    SyncSelection n = s;
    n.Rekey(noneed);
    CHECK(n.IsOn("X|rgs1"));  // its need is gone from the diff (applied elsewhere): nothing left to need
    // Applying everything ticked leaves nothing and nothing ticked
    SyncDiff empty;
    SyncSelection e = s;
    e.Rekey(empty);
    CHECK(e.Counts(empty).ticked == 0 && e.Picks(empty).empty());
}

static void TestTree()
{
    const SyncDiff d = Fixture();
    const auto     nodes = SyncChangedNodes(d);
    CHECK(std::find(nodes.begin(), nodes.end(), "R") != nodes.end());
    CHECK(std::find(nodes.begin(), nodes.end(), "wm") == nodes.end());  // wm holds no change by node: its X sits under swWalk
    CHECK(std::find(nodes.begin(), nodes.end(), "grp") != nodes.end() && std::find(nodes.begin(), nodes.end(), "grp:gS") != nodes.end());
    const auto top = SyncTopNodes(d, nodes);
    CHECK(top.size() == 2 && top[0] == "R" && top[1] == "grp");
    CHECK(SyncKeysUnder(d, "swWalk").size() == 2);
    CHECK(SyncKeysUnder(d, "grp").size() == 2 && SyncKeysAt(d, "grp:gS").size() == 2);
    CHECK(SyncKeysUnder(d, "R").size() == 11);
    CHECK(SyncGhostName(d, "wm") == "Walk_Mud" && SyncGhostType(d, "wm") == "Random");
    CHECK(SyncGhostName(d, "grp:gS") == "Surface" && SyncGhostName(d, "grp") == "Switch Groups");
    CHECK(SyncParentOf(d, "grp:gS") == "grp" && SyncParentOf(d, "wm") == "swWalk" && SyncParentOf(d, "R").empty());
    CHECK(SyncChangesWord(1) == "1 change" && SyncChangesWord(3) == "3 changes");
}

int main()
{
    TestTick();
    TestUntickPrunes();
    TestDropPulls();
    TestBoth();
    TestGroupDir();
    TestDirOnlyItsSide();
    TestSyncAllTriState();
    TestRekeyAfterApply();
    TestTree();
    if (g_fail == 0) std::printf("library_sync_select_test: all passed\n");
    return g_fail == 0 ? 0 : 1;
}
