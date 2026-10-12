// SPDX-License-Identifier: MIT
//
// Host tests of src/sound_tracks_plan.h (story 13-13): every row of the spec's I/O matrix (the NVK
// pile row is REAPER-side: sound_populate.cpp) and its test plan.

#include "sound_tracks_plan.h"

#include <cstdio>
#include <string>
#include <vector>

#include "ravlib.h"
#include "sound_record.h"

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) {                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);             \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static SoundNode Node(const std::string& uid, const std::string& parent, SoundNodeType t)
{
    SoundNode n;
    n.uid = uid;
    n.parent = parent;
    n.type = t;
    n.name = uid;
    return n;
}

static SoundNode Switch(const std::string& uid, const std::string& parent, const std::string& group,
                        std::vector<SwitchCase> cases)
{
    SoundNode n = Node(uid, parent, SoundNodeType::Switch);
    n.group_uid = group;
    n.cases = std::move(cases);
    return n;
}

static SwitchGroup Group(const std::string& uid, const std::string& name,
                         std::vector<std::pair<std::string, std::string>> values)
{
    SwitchGroup g;
    g.uid = uid;
    g.name = name;
    for (const auto& v : values) g.values.push_back({v.first, v.second, {}});
    return g;
}

// Loco (Run, Walk) > Feet (Shoes, Bare) > Material (Concrete, Wood).
static SoundStore NestedStore()
{
    SoundStore s;
    s.groups.push_back(Group("gLoco", "LocomotionType", {{"vRun", "Run"}, {"vWalk", "Walk"}}));
    s.groups.push_back(Group("gFeet", "FeetType", {{"vShoes", "Shoes"}, {"vBare", "Bare"}}));
    s.groups.push_back(Group("gMat", "Material", {{"vConcrete", "Concrete"}, {"vWood", "Wood"}, {"vEmpty", ""}}));
    SoundLibrary lib;
    lib.uid = "lib";
    lib.name = "Feet";
    lib.root = "loco";
    lib.nodes.push_back(Switch("loco", "", "gLoco", {{"vRun", "feet", {}}}));
    lib.nodes.push_back(Switch("feet", "loco", "gFeet", {{"vShoes", "mat", {}}}));
    // Concrete has no case of its own: the default (Wood) plays; Wood's sound is named "Wood".
    lib.nodes.push_back(Switch("mat", "feet", "gMat",
                               {{"vConcrete", "conc", {}}, {"vWood", "wood", {}}, {"vEmpty", "empty", {}}}));
    lib.nodes.push_back(Node("conc", "mat", SoundNodeType::Sound));
    lib.nodes.push_back(Node("wood", "mat", SoundNodeType::Sound));
    lib.nodes.push_back(Node("empty", "mat", SoundNodeType::Sound));
    s.libraries.push_back(lib);
    return s;
}

static std::vector<std::string> Names(const std::vector<SwitchStep>& p)
{
    std::vector<std::string> out;
    for (const SwitchStep& s : p) out.push_back(s.value_name);
    return out;
}

static bool SameSteps(const std::vector<TrackStep>& got, const std::vector<std::pair<std::string, bool>>& want)
{
    if (got.size() != want.size()) return false;
    for (size_t i = 0; i < got.size(); ++i)
        if (got[i].name != want[i].first || got[i].folder != want[i].second) return false;
    return true;
}

static void TestSwitchPath()
{
    SoundStore          s = NestedStore();
    const SoundLibrary& lib = s.libraries.front();
    // 3 nested Switches.
    const std::vector<SwitchStep> p = SwitchPathOf(s, lib, "conc");
    CHECK((Names(p) == std::vector<std::string>{"Run", "Shoes", "Concrete"}));
    CHECK(p.size() == 3 && p[0].group_name == "LocomotionType" && p[2].group_uid == "gMat" && p[2].value_uid == "vConcrete");
    // Default fallback: the tree's value (the sound under Wood lands on Wood's track).
    CHECK((Names(SwitchPathOf(s, lib, "wood")) == std::vector<std::string>{"Run", "Shoes", "Wood"}));
    // A Switch node itself: its ancestors only.
    CHECK((Names(SwitchPathOf(s, lib, "mat")) == std::vector<std::string>{"Run", "Shoes"}));
    // An unknown node: empty.
    CHECK(SwitchPathOf(s, lib, "nope").empty());
}

static void TestRandomBlend()
{
    SoundStore s;
    s.groups.push_back(Group("gLoco", "Loco", {{"vRun", "Run"}}));
    s.groups.push_back(Group("gMat", "Mat", {{"vWood", "Wood"}}));
    SoundLibrary lib;
    lib.uid = "lib";
    lib.name = "L";
    // Random between Switches: Loco > Run > Random > Mat > Wood > Sound.
    lib.nodes.push_back(Switch("loco", "", "gLoco", {{"vRun", "rnd", {}}}));
    lib.nodes.push_back(Node("rnd", "loco", SoundNodeType::Random));
    lib.nodes.push_back(Switch("mat", "rnd", "gMat", {{"vWood", "s1", {}}}));
    lib.nodes.push_back(Node("s1", "mat", SoundNodeType::Sound));
    // No Switch: Random > Sound.
    lib.nodes.push_back(Node("r2", "", SoundNodeType::Random));
    lib.nodes.push_back(Node("s2", "r2", SoundNodeType::Sound));
    // Switch under Blend, and a Blend sibling with no Switch.
    lib.nodes.push_back(Node("bl", "", SoundNodeType::Blend));
    lib.nodes.push_back(Switch("mat2", "bl", "gMat", {{"vWood", "s3", {}}}));
    lib.nodes.push_back(Node("s3", "mat2", SoundNodeType::Sound));
    lib.nodes.push_back(Node("s4", "bl", SoundNodeType::Sound));
    s.libraries.push_back(lib);
    CHECK((Names(SwitchPathOf(s, lib, "s1")) == std::vector<std::string>{"Run", "Wood"}));
    CHECK(SwitchPathOf(s, lib, "s2").empty());
    CHECK((Names(SwitchPathOf(s, lib, "s3")) == std::vector<std::string>{"Wood"}));
    CHECK(SwitchPathOf(s, lib, "s4").empty());
    CHECK(TrackPathFor(SwitchPathOf(s, lib, "s4"), TrackLayout::Flat, "RAV Library", "L").empty());
    CHECK(SameSteps(TrackPathFor(SwitchPathOf(s, lib, "s3"), TrackLayout::Flat, "RAV Library", "L"),
                    {{"RAV Library", true}, {"L", true}, {"Wood", false}}));
}

static void TestOddTrees()
{
    SoundStore s;
    s.groups.push_back(Group("gMat", "Mat", {{"vWood", "Wood"}}));
    SoundLibrary lib;
    lib.uid = "lib";
    // Unassigned: the child is on no value.
    SoundNode sw = Switch("sw", "", "gMat", {{"vWood", "a", {}}});
    sw.unassigned = {"b"};
    lib.nodes.push_back(sw);
    lib.nodes.push_back(Node("a", "sw", SoundNodeType::Sound));
    lib.nodes.push_back(Node("b", "sw", SoundNodeType::Sound));
    // A missing group, a missing value.
    lib.nodes.push_back(Switch("g1", "", "gGone", {{"vX", "c", {}}}));
    lib.nodes.push_back(Node("c", "g1", SoundNodeType::Sound));
    lib.nodes.push_back(Switch("g2", "", "gMat", {{"vGone", "d", {}}}));
    lib.nodes.push_back(Node("d", "g2", SoundNodeType::Sound));
    // A cycle: x <-> y.
    lib.nodes.push_back(Switch("x", "y", "gMat", {{"vWood", "e", {}}, {"vWood", "y", {}}}));
    lib.nodes.push_back(Switch("y", "x", "gMat", {{"vWood", "x", {}}}));
    lib.nodes.push_back(Node("e", "x", SoundNodeType::Sound));
    CHECK((Names(SwitchPathOf(s, lib, "a")) == std::vector<std::string>{"Wood"}));
    CHECK(SwitchPathOf(s, lib, "b").empty());
    const std::vector<SwitchStep> c = SwitchPathOf(s, lib, "c");
    CHECK(c.size() == 1 && c[0].group_name == "?" && c[0].value_name == "?");
    const std::vector<SwitchStep> d = SwitchPathOf(s, lib, "d");
    CHECK(d.size() == 1 && d[0].group_name == "Mat" && d[0].value_name == "?");
    CHECK(SwitchPathOf(s, lib, "e").empty());
    // Cased but also Unassigned (review F4): CaseChild reads no content, so no path.
    SoundNode sw2 = Switch("sw2", "", "gMat", {{"vWood", "f", {}}});
    sw2.unassigned = {"f"};
    lib.nodes.push_back(sw2);
    lib.nodes.push_back(Node("f", "sw2", SoundNodeType::Sound));
    CHECK(SwitchPathOf(s, lib, "f").empty());
}

static void TestTrackPath()
{
    SoundStore                    s = NestedStore();
    const std::vector<SwitchStep> p = SwitchPathOf(s, s.libraries.front(), "conc");
    CHECK(SameSteps(TrackPathFor(p, TrackLayout::Flat, "RAV Library", "Feet"),
                    {{"RAV Library", true}, {"Feet", true}, {"Run-Shoes-Concrete", false}}));
    CHECK(SameSteps(TrackPathFor(p, TrackLayout::Tree, "RAV Library", "Feet"),
                    {{"RAV Library", true}, {"Feet", true}, {"Run", true}, {"Shoes", true}, {"Concrete", false}}));
    CHECK(TrackPathFor(p, TrackLayout::Off, "RAV Library", "Feet").empty());
    CHECK(TrackPathFor({}, TrackLayout::Tree, "RAV Library", "Feet").empty());
    // A user library's Lands-in folder.
    CHECK(SameSteps(TrackPathFor(p, TrackLayout::Flat, "Foley", "Steps"),
                    {{"Foley", true}, {"Steps", true}, {"Run-Shoes-Concrete", false}}));
    // Unnamed value.
    const std::vector<SwitchStep> e = SwitchPathOf(s, s.libraries.front(), "empty");
    CHECK(SameSteps(TrackPathFor(e, TrackLayout::Flat, "RAV Library", "Feet"),
                    {{"RAV Library", true}, {"Feet", true}, {"Run-Shoes-(unnamed)", false}}));
    CHECK(SameSteps(TrackPathFor(e, TrackLayout::Tree, "RAV Library", "Feet"),
                    {{"RAV Library", true}, {"Feet", true}, {"Run", true}, {"Shoes", true}, {"(unnamed)", false}}));
}

static void TestMatch()
{
    CHECK(MatchChildTrack({"concrete ", "Wood"}, "Concrete") == 0);
    CHECK(MatchChildTrack({"Wood", "  CONCRETE"}, "concrete") == 1);
    CHECK(MatchChildTrack({"Wood", "Stone"}, "Concrete") == -1);
    CHECK(MatchChildTrack({}, "Concrete") == -1);
    CHECK(MatchChildTrack({"Run", "run"}, "RUN") == 0);  // first of two
    CHECK(MatchChildTrack({"Run-Shoes"}, "Run") == -1);
}

// Review F1: the Lands-in folder's layer children with a Switch tree among them.
static void TestLayerChild()
{
    int next = 0;
    // No Switch tree: 13-11's position (byte-identical to 1fe0956).
    CHECK(PickLayerChild({"layer 1", "layer 2"}, {false, false}, 1, &next) == 0 && next == 3);
    CHECK(PickLayerChild({"layer 1", "layer 2"}, {false, false}, 2) == 1);
    CHECK(PickLayerChild({"Kick", "Snap"}, {false, false}, 2) == 1);  // renamed: by position
    CHECK(PickLayerChild({"layer 1"}, {false}, 2, &next) == -1 && next == 2);
    CHECK(PickLayerChild({}, {}, 1, &next) == -1 && next == 1);
    // The Switch tree ("Feet", made first in an empty Lands-in folder) is never a layer, even
    // after the option is back to Off.
    CHECK(PickLayerChild({"Feet"}, {true}, 1, &next) == -1 && next == 1);
    CHECK(PickLayerChild({"Feet", "layer 1"}, {true, false}, 1) == 1);
    CHECK(PickLayerChild({"Feet", "layer 1"}, {true, false}, 2, &next) == -1 && next == 2);
    CHECK(PickLayerChild({"layer 1", "Feet", "layer 2"}, {false, true, false}, 2) == 2);
    CHECK(PickLayerChild({"Kick", "Feet", "Snap"}, {false, true, false}, 2) == 2);  // position skips it
    // A Switch-tree folder named "layer 1" (a library so named) is not taken either.
    CHECK(PickLayerChild({"layer 1", "x"}, {true, false}, 1) == 1);
    // By name first: "layer 2" wherever it sits.
    CHECK(PickLayerChild({"Kick", "Layer 2 "}, {false, false}, 2) == 1);
    CHECK(PickLayerChild({"layer 2", "layer 1"}, {false, false}, 1) == 1);
    // A flag list shorter than the names: the rest are layer candidates.
    CHECK(PickLayerChild({"a", "b"}, {}, 2) == 1);
    CHECK(PickLayerChild({"a"}, {false}, 0) == -1);
}

static void TestLayoutToken()
{
    SoundStore s;
    CHECK(TrackLayoutOf(s) == TrackLayout::Off);
    s.kept = {"zz=1", "trk=tree", "q"};
    CHECK(TrackLayoutOf(s) == TrackLayout::Tree);
    s.kept = {"trk=flat"};
    CHECK(TrackLayoutOf(s) == TrackLayout::Flat);
    s.kept = {"trk=spiral"};
    CHECK(TrackLayoutOf(s) == TrackLayout::Off);
    CHECK(!SetTrackLayout(s, TrackLayout::Off));  // reads Off already: unchanged
    // Off removes the token, the other kept tokens untouched.
    s.kept = {"zz=1", "trk=tree", "q"};
    CHECK(SetTrackLayout(s, TrackLayout::Off));
    CHECK((s.kept == std::vector<std::string>{"zz=1", "q"}));
    CHECK(TrackLayoutOf(s) == TrackLayout::Off);
    // The same value: false.
    CHECK(!SetTrackLayout(s, TrackLayout::Off));
    CHECK(SetTrackLayout(s, TrackLayout::Flat));
    CHECK((s.kept == std::vector<std::string>{"zz=1", "q", "trk=flat"}));
    CHECK(!SetTrackLayout(s, TrackLayout::Flat));
    CHECK(SetTrackLayout(s, TrackLayout::Tree));  // replaced in place
    CHECK((s.kept == std::vector<std::string>{"zz=1", "q", "trk=tree"}));

    // Serialize -> parse keeps it; a .ravlib has no token.
    SoundStore st = NestedStore();
    CHECK(SetTrackLayout(st, TrackLayout::Tree));
    SoundStore back;
    CHECK(ParseSoundStore(SerializeSoundStore(st), &back));
    CHECK(TrackLayoutOf(back) == TrackLayout::Tree);
    CHECK(back.libraries.size() == 1);
    const SoundStore one = RavlibOf(back, "lib", nullptr);
    CHECK(one.libraries.size() == 1);
    CHECK(TrackLayoutOf(one) == TrackLayout::Off);
    CHECK(one.kept.empty());
    SoundStore again;
    CHECK(ParseRavlib(SerializeRavlib(one), &again));
    CHECK(TrackLayoutOf(again) == TrackLayout::Off);
}

int main()
{
    TestSwitchPath();
    TestRandomBlend();
    TestOddTrees();
    TestTrackPath();
    TestMatch();
    TestLayerChild();
    TestLayoutToken();
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("sound_tracks_plan: all passed\n");
    return 0;
}
