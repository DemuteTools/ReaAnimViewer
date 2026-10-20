// SPDX-License-Identifier: MIT
//
// Host tests of src/library_sync_select.h (story 13-77c): the Sync with file window's selection model, on a synthetic
// Footstep diff shaped like the mockup's "Les deux" state.

#include "library_sync_select.h"

#include <algorithm>
#include <chrono>
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
    CHECK(SyncEffectToast(d, fx) == "Changed on both sides: pick \xE2\x86\x91 Send to global or \xE2\x86\x93 Get from global");
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
    CHECK(SyncFooterText(c, 13) == "3 ticked: \xE2\x86\x91 1 to global, \xE2\x86\x93 2 taken here");
    s.Toggle(d, "X|rgs1");
    CHECK(SyncFooterText(s.Counts(d), 13).find("1 ticked for you because another needs it") != std::string::npos);
    CHECK(SyncFooterText(SyncSelection().Counts(d), 4) == "Nothing ticked. 4 changes waiting.");
    CHECK(SyncFooterText(SyncSelection().Counts(d), 1) == "Nothing ticked. 1 change waiting.");
    CHECK(SyncFooterText(SyncSelection().Counts(d), 0) == "Nothing left: this project and the global library agree.");
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

// ---- review 13-77c ---------------------------------------------------------------------------------------------

static bool Consistent(const SyncDiff& d, const SyncSelection& s)
{
    const auto p = s.Picks(d);
    if (CloseSelection(d, p) != p) return false;
    for (const SyncChange& c : d.changes) {  // no auto tick whose asker is off or goes the other way
        const SyncTick* t = s.Tick(c.key);
        if (!t || !t->on || !t->auto_) continue;
        const SyncTick* b = s.Tick(t->by);
        if (!b || !b->on || b->dir != t->dir) return false;
        // and it traces back to a tick the user made (same direction), never to a ring of auto ticks
        bool rooted = false;
        for (const SyncChange& u : d.changes) {
            const SyncTick* ut = s.Tick(u.key);
            if (!ut || !ut->on || ut->auto_ || ut->dir != t->dir || u.side != c.side) continue;
            const bool adopts = (u.side == SyncSide::Here && ut->dir == SyncDir::Up) || (u.side == SyncSide::File && ut->dir == SyncDir::Down);
            const auto cl = adopts ? NeedsClosure(d, u.key) : DependentsClosure(d, u.key);
            if (std::find(cl.begin(), cl.end(), c.key) != cl.end()) { rooted = true; break; }
        }
        if (!rooted) return false;
    }
    return true;
}

static void TestH2Flips()
{
    const SyncDiff d = Fixture();
    {  // the asker turns: what it ticked for you in the old direction goes
        SyncSelection s;
        s.Toggle(d, "X|rgs1");
        CHECK(s.IsOn("X|rg") && s.Tick("X|rg")->dir == SyncDir::Up);
        s.Choose(d, "X|rgs1", SyncDir::Down);
        CHECK(!s.IsOn("X|rg"));
        CHECK(s.IsOn("X|rgs1") && s.Tick("X|rgs1")->dir == SyncDir::Down);
        CHECK(Consistent(d, s));
        CHECK(s.Picks(d).size() == 1);
    }
    {  // a need turns: the ticks that asked for it turn with it
        SyncSelection s;
        s.Toggle(d, "X|rgs1");
        s.Toggle(d, "X|rgs2");
        s.Choose(d, "X|rg", SyncDir::Down);
        CHECK(s.Tick("X|rgs1")->dir == SyncDir::Down && s.Tick("X|rgs2")->dir == SyncDir::Down);
        CHECK(Consistent(d, s));
        s.Choose(d, "X|rgs1", SyncDir::Up);  // and back: rg and its other dependents follow, the chip says so
        CHECK(s.Tick("X|rg")->dir == SyncDir::Up);
        CHECK(Consistent(d, s));
        CHECK(s.NeededBy(d, "X|rg").size() == 1 && s.NeededBy(d, "X|rg")[0] == "X|rgs1");
    }
    {  // a group's direction on the whole library: the file's changes turn, mine stay (never reverted by the other button)
        SyncSelection s;
        s.GroupDir(d, SyncKeysUnder(d, "*"), SyncDir::Up);
        s.GroupDir(d, SyncKeysUnder(d, "*"), SyncDir::Down);
        CHECK(Consistent(d, s));
        CHECK(s.Tick("X|rgs1")->dir == SyncDir::Up && s.Tick("X|wm")->dir == SyncDir::Down);
    }
}

static void TestCrossSideIgnored()
{
    SyncDiff d;
    d.root = "R";
    d.parent = {{"c", "R"}};
    Add(&d, "H", SyncSide::Here, "c", "h", {"F"});  // a here change that (wrongly) needs a file one
    Add(&d, "F", SyncSide::File, "c", "f");
    Add(&d, "B", SyncSide::Both, "c", "b", {"H"});
    d.BuildIndex();
    SyncSelection s;
    s.Toggle(d, "H");
    CHECK(s.IsOn("H") && !s.IsOn("F"));  // Pull never crosses sides, Prune does not ask for it either
    s.Choose(d, "H", SyncDir::Up);
    CHECK(s.IsOn("H"));
    SyncSelection t;
    t.Choose(d, "B", SyncDir::Up);  // a both change pulls nothing
    CHECK(t.IsOn("B") && !t.IsOn("H") && t.Picks(d).size() == 1);
}

static void TestGroupUntickPrunes()
{
    const SyncDiff d = Fixture();
    SyncSelection  s;
    s.Toggle(d, "X|rgs1");
    CHECK(s.IsOn("X|rg"));
    s.ToggleGroup(d, {"X|rg"});  // all ticked: untick the Random from its group row
    CHECK(!s.IsOn("X|rg") && !s.IsOn("X|rgs1"));
}

static void TestMutualPairNeedsUserTick()
{
    SyncDiff d;
    d.root = "R";
    d.parent = {{"N", "R"}, {"SN", "N"}};
    Add(&d, "X|N", SyncSide::Here, "N", "Random N added", {"C|V3"});
    Add(&d, "X|SN", SyncSide::Here, "N", "Sound SN added", {"X|N"});
    Add(&d, "C|V3", SyncSide::Here, "N", "V3 now plays N (new)", {"X|N"});
    d.BuildIndex();
    {  // tick SN, then untick it: the pair does not keep itself
        SyncSelection s;
        s.Toggle(d, "X|SN");
        CHECK(s.IsOn("X|N") && s.IsOn("C|V3") && Consistent(d, s));
        s.Toggle(d, "X|SN");
        CHECK(!s.IsOn("X|SN") && !s.IsOn("X|N") && !s.IsOn("C|V3"));
        CHECK(s.Picks(d).empty());
    }
    {  // tick SN, then send it Down: only SN stays
        SyncSelection s;
        s.Toggle(d, "X|SN");
        s.Choose(d, "X|SN", SyncDir::Down);
        CHECK(s.IsOn("X|SN") && !s.IsOn("X|N") && !s.IsOn("C|V3"));
        CHECK(Consistent(d, s));
        CHECK(s.Picks(d).size() == 1);
    }
}

static void TestRekeyPrunesOrphanAuto()
{
    const SyncDiff d = Fixture();
    SyncSelection  s;
    s.Toggle(d, "X|rgs1");
    SyncDiff now = d;  // the Sound is applied elsewhere, its Random is still listed
    now.changes.erase(std::remove_if(now.changes.begin(), now.changes.end(), [](const SyncChange& c) { return c.key == "X|rgs1"; }),
                      now.changes.end());
    s.Rekey(now);
    CHECK(!s.IsOn("X|rg"));  // ticked for a change that is gone: nobody asks for it any more
}

static void TestUntickOnUnsidedBoth()
{
    const SyncDiff d = Fixture();
    const auto     ww = SyncKeysUnder(d, "ww");
    SyncSelection  s;
    s.ToggleGroup(d, ww);
    CHECK(s.IsOn("F|ww|avoid") && s.IsOn("F|ww|fades") && !s.IsOn("F|ww|gain"));
    s.ToggleGroup(d, ww);  // M1: the second click unticks, the unsided both does not hold the box
    CHECK(!s.IsOn("F|ww|avoid") && !s.IsOn("F|ww|fades"));
    const auto all = SyncKeysUnder(d, "*");
    s.ToggleGroup(d, all);
    CHECK(s.Counts(d).ticked == 11);  // 13 minus the 2 unsided both
    s.ToggleGroup(d, all);
    CHECK(s.Counts(d).ticked == 0);
    // only unsided both under it: the click ticks nothing and says why
    SyncEffect fx = s.ToggleGroup(d, {"F|wc|gain", "F|ww|gain"});
    CHECK(fx.skipped_both == 2 && s.Counts(d).ticked == 0);
}

static void TestValueMovedDropsTick()
{
    SyncDiff      d = Fixture();
    SyncSelection s;
    s.Toggle(d, "X|wcs4");
    s.Toggle(d, "F|ww|avoid");
    const uint64_t sig = SyncDiffSignature(d);
    for (SyncChange& c : d.changes)
        if (c.key == "F|ww|avoid") c.file_text = "another value";
    d.BuildIndex();
    CHECK(SyncDiffSignature(d) != sig);  // L3: the window sees the list moved
    s.Rekey(d);
    CHECK(s.IsOn("X|wcs4") && !s.IsOn("F|ww|avoid"));
    SyncDiff e = Fixture();
    SyncDiff f = Fixture();
    f.changes[0].node = "elsewhere";
    CHECK(SyncDiffSignature(e) == SyncDiffSignature(Fixture()) && SyncDiffSignature(e) != SyncDiffSignature(f));
}

static void TestIndexMatchesScan()
{
    SyncDiff d = Fixture();
    const SyncDiff plain = d;  // no index: the scan
    d.BuildIndex();
    CHECK(d.IndexValid() && !plain.IndexValid());
    for (const SyncChange& c : plain.changes) {
        CHECK(d.Find(c.key) == &d.changes[static_cast<size_t>(d.Find(c.key) - d.changes.data())] && d.Find(c.key)->key == c.key);
        CHECK(NeedsClosure(d, c.key) == NeedsClosure(plain, c.key));
        CHECK(DependentsClosure(d, c.key) == DependentsClosure(plain, c.key));
    }
    CHECK(d.Find("nope") == nullptr && plain.Find("nope") == nullptr);
    d.changes.pop_back();  // edited by hand after the index: back to the scan, still right
    CHECK(!d.IndexValid() && d.Find("C|swWalk|Mud") == nullptr && d.Find("X|wm") != nullptr);
}

static double Ms(std::chrono::steady_clock::time_point a)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
}

static void TestBigDiffIsFast()
{
    SyncDiff d;
    d.root = "R";
    d.mine_nodes = {"R"};
    const int packs = 250;  // 250 Randoms x 3 Sounds = 1000 changes, all from the file
    for (int i = 0; i < packs; ++i) {
        const std::string r = "r" + std::to_string(i);
        d.parent[r] = "R";
        Add(&d, "X|" + r, SyncSide::File, r, "Random " + r + " added", {}, true, r, r);
        for (int k = 0; k < 3; ++k) Add(&d, "X|" + r + "s" + std::to_string(k), SyncSide::File, r, "Sound added", {"X|" + r});
    }
    d.BuildIndex();
    const auto t0 = std::chrono::steady_clock::now();
    const auto all = SyncKeysUnder(d, "*");
    SyncSelection s;
    s.ToggleGroup(d, all);  // Sync all
    CHECK(s.TriState(d, all) == SyncTri::All && s.Counts(d).ticked == 1000);
    const auto nb = s.NeededByAll(d);  // what the window computes once per selection change
    CHECK(nb.size() >= static_cast<size_t>(packs) && nb.at("X|r0").size() == 3);
    for (int i = 0; i < 20; ++i) CHECK(s.TriState(d, all) == SyncTri::All);  // frames
    s.Toggle(d, "X|r7");  // untick one Random: its 3 Sounds go with it
    CHECK(s.Counts(d).ticked == 996 && !s.IsOn("X|r7s2"));
    s.Toggle(d, "X|r7s1");  // tick a Sound: the Random is ticked for you again
    CHECK(s.IsOn("X|r7") && s.Counts(d).ticked == 998);
    s.ToggleGroup(d, all);  // some ticked: ticks the rest
    CHECK(s.Counts(d).ticked == 1000);
    s.ToggleGroup(d, all);
    CHECK(s.Counts(d).ticked == 0);
    const double ms = Ms(t0);
    std::printf("big diff (1000 changes): %.0f ms\n", ms);
    CHECK(ms < 1500.0);  // was ~8 s for the chips of one frame alone (review H1); a loose bound for a slow -O0 host
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
    TestH2Flips();
    TestCrossSideIgnored();
    TestGroupUntickPrunes();
    TestMutualPairNeedsUserTick();
    TestRekeyPrunesOrphanAuto();
    TestUntickOnUnsidedBoth();
    TestValueMovedDropsTick();
    TestIndexMatchesScan();
    TestBigDiffIsFast();
    if (g_fail == 0) std::printf("library_sync_select_test: all passed\n");
    return g_fail == 0 ? 0 : 1;
}
