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

// ---- v5: one tick per side (Fork-style window) -------------------------------------------------------------------------

static void Chg(SyncDiff* d, const std::string& key, SyncSide side, const std::string& node, const std::string& label,
                const std::string& here, const std::string& file, std::vector<std::string> needs = {}, bool adds = false,
                const std::string& text = "")
{
    SyncChange c;
    c.key = key;
    c.side = side;
    c.node = node;
    c.subject = node;
    c.label = label;
    c.here_text = here;
    c.file_text = file;
    c.text = text.empty() ? label : text;
    c.needs = std::move(needs);
    c.adds_container = adds;
    d->changes.push_back(c);
}

// under R: a (Gain here), b (Gain in the file), c (Gain on both sides), rg (a Random added here, with a Sound), old (a Random
// removed here), nw (a Random added in the file).
static SyncDiff V5()
{
    SyncDiff d;
    d.root = "R";
    d.mine_nodes = {"R", "a", "b", "c", "rg", "old"};
    d.parent = {{"a", "R"}, {"b", "R"}, {"c", "R"}, {"rg", "R"}, {"old", "R"}, {"nw", "R"}};
    Chg(&d, "F|a|gain", SyncSide::Here, "a", "Gain", "-3 dB", "0 dB", {}, false, "Gain 0 -> -3 dB");
    Chg(&d, "F|b|gain", SyncSide::File, "b", "Gain", "0 dB", "-6 dB", {}, false, "Gain 0 -> -6 dB");
    Chg(&d, "F|c|gain", SyncSide::Both, "c", "Gain", "-3 dB", "-6 dB", {}, false, "Gain: here -3 - global -6");
    Chg(&d, "X|rg", SyncSide::Here, "rg", "Run_Gravel", "present", "absent", {}, true, "Random Run_Gravel added");
    Chg(&d, "X|rgs", SyncSide::Here, "rg", "FS_Run_Gravel_01", "present", "absent", {"X|rg"}, false, "Sound FS_Run_Gravel_01 added");
    Chg(&d, "X|old", SyncSide::Here, "old", "Walk_Old", "removed", "present", {}, false, "Random Walk_Old removed");
    Chg(&d, "X|nw", SyncSide::File, "nw", "Walk_New", "absent", "present", {}, true, "Random Walk_New added");
    d.BuildIndex();
    return d;
}

static void TestV5PreTick()
{
    const SyncDiff d = V5();
    SyncSelection  s;
    s.PreTick(d);
    CHECK(s.IsOn("F|a|gain") && s.Tick("F|a|gain")->dir == SyncDir::Up);   // changed here: keep mine
    CHECK(s.IsOn("F|b|gain") && s.Tick("F|b|gain")->dir == SyncDir::Down); // changed in the global: keep global
    CHECK(!s.IsOn("F|c|gain"));                                            // a conflict is never pre-ticked
    CHECK(s.IsOn("X|rg") && s.IsOn("X|rgs") && !s.Tick("X|rg")->auto_);    // each addition is ticked on its own
    CHECK(s.IsOn("X|old") && s.Tick("X|old")->dir == SyncDir::Up);
    CHECK(s.IsOn("X|nw") && s.Tick("X|nw")->dir == SyncDir::Down);
    CHECK(s.Counts(d).ticked == 6);
    // the engine's picks: keep mine = up, keep global = down; the engine's closure changes nothing
    const auto p = s.Picks(d);
    CHECK(p.at("F|a|gain") == SyncDir::Up && p.at("F|b|gain") == SyncDir::Down && p.at("X|nw") == SyncDir::Down);
    CHECK(CloseSelection(d, p) == p);
    // a user's untick survives a rebuilt diff; a new change is pre-ticked
    s.ToggleSide(d, "F|a|gain", SyncDir::Up);
    CHECK(!s.IsOn("F|a|gain"));
    SyncDiff d2 = V5();
    Chg(&d2, "F|e|gain", SyncSide::File, "b", "Gain", "0 dB", "-1 dB");
    d2.BuildIndex();
    s.Rekey(d2);
    s.PreTick(d2);
    CHECK(!s.IsOn("F|a|gain") && s.IsOn("F|e|gain") && s.Tick("F|e|gain")->dir == SyncDir::Down);
}

static void TestV5ConflictPick()
{
    const SyncDiff d = V5();
    SyncSelection  s;
    s.PreTick(d);
    s.ToggleSide(d, "F|c|gain", SyncDir::Up);  // keep mine
    CHECK(s.IsOn("F|c|gain") && s.Tick("F|c|gain")->dir == SyncDir::Up);
    s.ToggleSide(d, "F|c|gain", SyncDir::Down);  // the other side: switches
    CHECK(s.IsOn("F|c|gain") && s.Tick("F|c|gain")->dir == SyncDir::Down);
    s.ToggleSide(d, "F|c|gain", SyncDir::Down);  // the same box again: off, the row is left aside
    CHECK(!s.IsOn("F|c|gain"));
    CHECK(s.Picks(d).count("F|c|gain") == 0);
}

static void TestV5RevertByTickingTheUnchangedSide()
{
    const SyncDiff d = V5();
    SyncSelection  s;
    s.PreTick(d);
    s.ToggleSide(d, "F|a|gain", SyncDir::Down);  // a modification made here: tick the global's box = revert it
    CHECK(s.IsOn("F|a|gain") && s.Tick("F|a|gain")->dir == SyncDir::Down);
    CHECK(s.Picks(d).at("F|a|gain") == SyncDir::Down);
    const SyncFooter f = s.Footer(d);
    bool             back = false;
    for (const std::string& g : f.project.gets) back |= g == "Gain back to 0 dB";
    CHECK(back);  // this project takes the global's value back
    s.ToggleSide(d, "F|b|gain", SyncDir::Up);  // and the reverse: refuse the global's change
    CHECK(s.Tick("F|b|gain")->dir == SyncDir::Up);
}

static void TestV5AdditionSingleBox()
{
    const SyncDiff d = V5();
    SyncSelection  s;
    s.PreTick(d);
    CHECK(SyncSideAvailable(*d.Find("X|rg"), SyncDir::Up) && !SyncSideAvailable(*d.Find("X|rg"), SyncDir::Down));
    CHECK(!SyncSideAvailable(*d.Find("X|nw"), SyncDir::Up) && SyncSideAvailable(*d.Find("X|nw"), SyncDir::Down));
    CHECK(SyncSideAvailable(*d.Find("X|old"), SyncDir::Up) && SyncSideAvailable(*d.Find("X|old"), SyncDir::Down));  // removals: two boxes
    CHECK(SyncSideAvailable(*d.Find("F|c|gain"), SyncDir::Up) && SyncSideAvailable(*d.Find("F|c|gain"), SyncDir::Down));
    const uint64_t v = s.Version();
    s.ToggleSide(d, "X|rg", SyncDir::Down);  // no box there: nothing happens, never a deletion of the addition
    CHECK(s.IsOn("X|rg") && s.Tick("X|rg")->dir == SyncDir::Up && s.Version() == v);
    // unticking the Random: its Sound goes with it (it needs it); both stay only here
    s.ToggleSide(d, "X|rg", SyncDir::Up);
    CHECK(!s.IsOn("X|rg") && !s.IsOn("X|rgs"));
    CHECK(s.Picks(d).count("X|rg") == 0);
    // ticking the Sound ticks the Random for you
    s.ToggleSide(d, "X|rgs", SyncDir::Up);
    CHECK(s.IsOn("X|rg") && s.Tick("X|rg")->auto_ && s.Tick("X|rg")->by == "X|rgs");
}

static void TestV5RemovalKeep()
{
    const SyncDiff d = V5();
    SyncSelection  s;
    s.PreTick(d);
    CHECK(s.Footer(d).global.drops.size() == 1);  // removed here, pre-ticked: the global drops it
    s.ToggleSide(d, "X|old", SyncDir::Down);      // the global's box keeps the element
    CHECK(s.Picks(d).at("X|old") == SyncDir::Down);
    const SyncFooter f = s.Footer(d);
    CHECK(f.global.drops.empty());
    bool restored = false;
    for (const std::string& g : f.project.gets) restored |= g == "Random Walk_Old removed restored";
    CHECK(restored);
}

static void TestV5KeepAllSettlesConflicts()
{
    const SyncDiff d = V5();
    SyncSelection  s;
    s.PreTick(d);
    std::vector<std::string> all;
    for (const SyncChange& c : d.changes) all.push_back(c.key);
    CHECK(s.KeepTri(d, all, SyncDir::Up) == SyncTri::Some && s.KeepTri(d, all, SyncDir::Down) == SyncTri::Some);
    s.KeepAll(d, all, SyncDir::Down);  // keep all global
    CHECK(s.Tick("F|c|gain")->on && s.Tick("F|c|gain")->dir == SyncDir::Down);  // the conflict is settled
    CHECK(s.Tick("F|a|gain")->dir == SyncDir::Down);                           // mine reverted
    CHECK(!s.IsOn("X|rg") && !s.IsOn("X|rgs"));                                // an addition made here stays only here
    CHECK(s.IsOn("X|nw") && s.Tick("X|nw")->dir == SyncDir::Down);
    CHECK(s.IsOn("X|old") && s.Tick("X|old")->dir == SyncDir::Down);           // removal refused
    CHECK(s.KeepTri(d, all, SyncDir::Down) == SyncTri::All);
    CHECK(s.KeepTri(d, all, SyncDir::Up) == SyncTri::None);
    // clicking the full box clears it (an addition of the other side goes back to ticked)
    s.KeepClear(d, all, SyncDir::Down);
    CHECK(!s.IsOn("F|c|gain") && !s.IsOn("F|a|gain") && s.IsOn("X|rg"));
    s.KeepAll(d, all, SyncDir::Up);  // keep all mine
    CHECK(s.Tick("F|c|gain")->dir == SyncDir::Up && s.Tick("F|b|gain")->dir == SyncDir::Up);
    CHECK(s.IsOn("X|rg") && !s.IsOn("X|nw"));
    CHECK(s.KeepTri(d, all, SyncDir::Up) == SyncTri::All);
    CHECK(s.KeepTri(d, {}, SyncDir::Up) == SyncTri::Disabled);
    const auto p = s.Picks(d);
    CHECK(CloseSelection(d, p) == p);
}

static void TestV5Dependencies()
{
    SyncDiff d;
    d.root = "R";
    d.parent = {{"sw", "R"}, {"wm", "sw"}};
    Chg(&d, "X|wm", SyncSide::File, "sw", "Walk_Mud", "absent", "present", {}, true, "Random Walk_Mud added");
    Chg(&d, "C|sw|Mud", SyncSide::File, "sw", "Switch value Mud", "absent", "plays Walk_Mud", {"X|wm"}, false, "Mud now plays Walk_Mud");
    d.BuildIndex();
    SyncSelection s;
    s.PreTick(d);
    CHECK(s.IsOn("X|wm") && s.IsOn("C|sw|Mud"));
    CHECK(s.NeededBy(d, "X|wm").size() == 1 && s.NeededBy(d, "X|wm")[0] == "C|sw|Mud");
    s.ToggleSide(d, "X|wm", SyncDir::Down);  // untick the needed Random: the case goes with it
    CHECK(!s.IsOn("X|wm") && !s.IsOn("C|sw|Mud"));
    s.ToggleSide(d, "C|sw|Mud", SyncDir::Down);  // tick the case: the Random is ticked for you
    CHECK(s.IsOn("X|wm") && s.Tick("X|wm")->auto_ && s.Tick("X|wm")->by == "C|sw|Mud");
    // an auto tick needs a user tick behind it: unticking the case lets the Random go again
    s.ToggleSide(d, "C|sw|Mud", SyncDir::Down);
    CHECK(!s.IsOn("X|wm") && !s.IsOn("C|sw|Mud"));
}

static void TestV5FooterAndNames()
{
    const SyncDiff d = V5();
    SyncSelection  s;
    s.PreTick(d);
    const SyncFooter f = s.Footer(d);
    CHECK(f.changes == 6);
    CHECK(f.global.gets.size() == 3 && f.global.drops.size() == 1);  // Gain a, Random Run_Gravel, its Sound; Walk_Old dropped
    CHECK(f.project.gets.size() == 2);                                // Gain b, Walk_New
    std::string       full;
    const std::string sum = SyncFooterSummary(f, &full);
    CHECK(sum.find("Apply: 6 changes") == 0);
    CHECK(sum.find("the global gets ") != std::string::npos && sum.find(" more") == std::string::npos);
    CHECK(sum.find("drops Random Walk_Old removed") != std::string::npos);
    CHECK(sum.find("this project gets ") != std::string::npos);
    CHECK(full.find("+1 more") == std::string::npos && full.find("Sound FS_Run_Gravel_01 added") != std::string::npos);
    CHECK(SyncFooterSummary(SyncFooter(), nullptr).empty());
    SyncFooter many;
    many.changes = 5;
    many.global.gets = {"a", "b", "c", "d", "e"};
    std::string many_full;
    const std::string many_sum = SyncFooterSummary(many, &many_full);
    CHECK(many_sum == "Apply: 5 changes \xC2\xB7 the global gets a, b, c +2 more" && many_full.find("d, e") != std::string::npos);

    // names: common start (up to a space or '_') and common end (from a '_') hidden, the differing middle kept
    const std::vector<std::string> n = {"FEETHmn_Boots Concrete Walk 01_B00M_ONE.wav", "FEETHmn_Boots Wood Walk 02_B00M_ONE.wav",
                                        "FEETHmn_Boots Metal Plate 01_B00M_ONE.wav"};
    const SyncNameTrim t = SyncCommonTrim(n);
    CHECK(t.pfx == "FEETHmn_Boots " && t.sfx == "_B00M_ONE.wav");
    CHECK(SyncTrimName(t, n[1]) == "Wood Walk 02");
    CHECK(SyncCommonTrim({"only one"}).pfx.empty());
    size_t a0, a1, b0, b1;
    SyncDiffRange("Gravel Light Walk 01", "Gravel Light Jog 01", &a0, &a1, &b0, &b1);
    CHECK(std::string("Gravel Light Walk 01").substr(a0, a1 - a0) == "Walk" && std::string("Gravel Light Jog 01").substr(b0, b1 - b0) == "Jog");
    SyncDiffRange("same", "same", &a0, &a1, &b0, &b1);
    CHECK(a0 == a1 && b0 == b1);
}

static void TestV5BigDiffIsFast()
{
    SyncDiff d;
    d.root = "R";
    const int packs = 250;
    for (int i = 0; i < packs; ++i) {
        const std::string r = "r" + std::to_string(i);
        d.parent[r] = "R";
        Chg(&d, "X|" + r, SyncSide::File, r, "R" + std::to_string(i), "absent", "present", {}, true, "Random " + r + " added");
        for (int k = 0; k < 3; ++k)
            Chg(&d, "X|" + r + "s" + std::to_string(k), SyncSide::File, r, "S", "absent", "present", {"X|" + r}, false, "Sound added");
    }
    d.BuildIndex();
    const auto    t0 = std::chrono::steady_clock::now();
    SyncSelection s;
    s.PreTick(d);
    std::vector<std::string> all;
    for (const SyncChange& c : d.changes) all.push_back(c.key);
    s.KeepAll(d, all, SyncDir::Up);
    s.KeepAll(d, all, SyncDir::Down);
    const SyncFooter f = s.Footer(d);
    (void)SyncFooterSummary(f, nullptr);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(s.Counts(d).ticked == 1000 && f.changes == 1000);
    CHECK(ms < 3000.0);
    std::printf("v5 big diff (1000 changes): %.0f ms\n", ms);
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
    TestV5PreTick();
    TestV5ConflictPick();
    TestV5RevertByTickingTheUnchangedSide();
    TestV5AdditionSingleBox();
    TestV5RemovalKeep();
    TestV5KeepAllSettlesConflicts();
    TestV5Dependencies();
    TestV5FooterAndNames();
    TestV5BigDiffIsFast();
    if (g_fail == 0) std::printf("library_sync_select_test: all passed\n");
    return g_fail == 0 ? 0 : 1;
}
