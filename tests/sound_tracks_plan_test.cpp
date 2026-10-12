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

// A Switch path with no Blend layer (13-13's TrackPathFor cases, now on SoundTrackPath).
static std::vector<TrackStep> PathOnly(const std::vector<SwitchStep>& path, TrackLayout layout, const std::string& lands,
                                       const std::string& lib_name)
{
    return SoundTrackPath(path, BlendLayer(), layout, lands, "lib", lib_name);
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
    CHECK(PathOnly(SwitchPathOf(s, lib, "s4"), TrackLayout::Flat, "RAV Library", "L").empty());
    CHECK(SameSteps(PathOnly(SwitchPathOf(s, lib, "s3"), TrackLayout::Flat, "RAV Library", "L"),
                    {{"RAV Library", true}, {"L", true}, {"Wood", false}}));
    // 13-16: with its Blend layer, Wood becomes the folder of the layer (the Switch "mat2").
    CHECK(SameSteps(SoundTrackPath(SwitchPathOf(s, lib, "s3"), LayerOf(lib, "s3"), TrackLayout::Flat, "RAV Library",
                                   "lib", "L"),
                    {{"RAV Library", true}, {"L", true}, {"Wood", true}, {"mat2", false}}));
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
    CHECK(SameSteps(PathOnly(p, TrackLayout::Flat, "RAV Library", "Feet"),
                    {{"RAV Library", true}, {"Feet", true}, {"Run_Shoes_Concrete", false}}));
    CHECK(SameSteps(PathOnly(p, TrackLayout::Tree, "RAV Library", "Feet"),
                    {{"RAV Library", true}, {"Feet", true}, {"Run", true}, {"Shoes", true}, {"Concrete", false}}));
    CHECK(PathOnly(p, TrackLayout::Off, "RAV Library", "Feet").empty());
    CHECK(PathOnly({}, TrackLayout::Tree, "RAV Library", "Feet").empty());
    // A user library's Lands-in folder.
    CHECK(SameSteps(PathOnly(p, TrackLayout::Flat, "Foley", "Steps"),
                    {{"Foley", true}, {"Steps", true}, {"Run_Shoes_Concrete", false}}));
    // Unnamed value.
    const std::vector<SwitchStep> e = SwitchPathOf(s, s.libraries.front(), "empty");
    CHECK(SameSteps(PathOnly(e, TrackLayout::Flat, "RAV Library", "Feet"),
                    {{"RAV Library", true}, {"Feet", true}, {"Run_Shoes_(unnamed)", false}}));
    CHECK(SameSteps(PathOnly(e, TrackLayout::Tree, "RAV Library", "Feet"),
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
    // By position only, never by name.
    CHECK(PickLayerChild({"Kick", "Layer 2 "}, {false, false}, 2) == 1);
    CHECK(PickLayerChild({"layer 2", "layer 1"}, {false, false}, 1) == 0);  // by position, never by name
    CHECK(PickLayerChild({"layer 2"}, {false}, 2, &next) == -1 && next == 2);  // a deleted layer 1: no shared track
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

// ---- Story 13-16: Blend layer tracks, no overlapping placed items -------------------------------

static SoundNode Named(const std::string& uid, const std::string& parent, SoundNodeType t, const std::string& name)
{
    SoundNode n = Node(uid, parent, t);
    n.name = name;
    return n;
}

static bool SameNames(const std::vector<TrackStep>& got, const std::vector<std::pair<std::string, bool>>& want)
{
    return SameSteps(got, want);
}

// Every level but the Lands-in folder carries a mark, none twice.
static bool MarkedOnce(const std::vector<TrackStep>& p)
{
    if (p.empty() || !p[0].mark.empty()) return false;
    for (size_t i = 1; i < p.size(); ++i) {
        if (p[i].mark.empty()) return false;
        for (size_t j = 1; j < i; ++j)
            if (p[j].mark == p[i].mark) return false;
    }
    return true;
}

static std::vector<TrackStep> PathOf(const SoundStore& s, const std::string& sound, TrackLayout layout,
                                     const std::string& lands = "RAV Library")
{
    const SoundLibrary& lib = s.libraries.front();
    return SoundTrackPath(SwitchPathOf(s, lib, sound), LayerOf(lib, sound), layout, lands, lib.uid, lib.name);
}

// Run > Shoes > Concrete > Blend [Heel, Scuff].
static SoundStore BlendUnderPath()
{
    SoundStore s;
    s.groups.push_back(Group("gLoco", "Loco", {{"vRun", "Run"}}));
    s.groups.push_back(Group("gFeet", "Feet", {{"vShoes", "Shoes"}}));
    s.groups.push_back(Group("gMat", "Material", {{"vConcrete", "Concrete"}, {"vWood", "Wood"}}));
    SoundLibrary lib;
    lib.uid = "lib";
    lib.name = "Steps";
    lib.nodes.push_back(Switch("loco", "", "gLoco", {{"vRun", "feet", {}}}));
    lib.nodes.push_back(Switch("feet", "loco", "gFeet", {{"vShoes", "mat", {}}}));
    lib.nodes.push_back(Switch("mat", "feet", "gMat", {{"vConcrete", "bl", {}}, {"vWood", "wood", {}}}));
    lib.nodes.push_back(Named("bl", "mat", SoundNodeType::Blend, "Blend"));
    lib.nodes.push_back(Named("heel", "bl", SoundNodeType::Sound, "Heel"));
    lib.nodes.push_back(Named("scuff", "bl", SoundNodeType::Sound, "Scuff"));
    lib.nodes.push_back(Named("wood", "mat", SoundNodeType::Sound, "WoodStep"));
    s.libraries.push_back(lib);
    return s;
}

static void TestLayerUnderPath()
{
    const SoundStore s = BlendUnderPath();
    const BlendLayer heel = LayerOf(s.libraries.front(), "heel");
    CHECK(heel.in_blend && heel.layer_uid == "heel" && heel.layer_name == "Heel" && heel.blend_uid == "bl");
    CHECK(!LayerOf(s.libraries.front(), "wood").in_blend);
    // Folders: Concrete becomes a folder, Heel and Scuff inside it.
    const std::vector<TrackStep> th = PathOf(s, "heel", TrackLayout::Tree);
    const std::vector<TrackStep> ts = PathOf(s, "scuff", TrackLayout::Tree);
    CHECK(SameNames(th, {{"RAV Library", true}, {"Steps", true}, {"Run", true}, {"Shoes", true}, {"Concrete", true}, {"Heel", false}}));
    CHECK(SameNames(ts, {{"RAV Library", true}, {"Steps", true}, {"Run", true}, {"Shoes", true}, {"Concrete", true}, {"Scuff", false}}));
    CHECK(MarkedOnce(th) && MarkedOnce(ts));
    for (size_t i = 0; i + 1 < th.size(); ++i) CHECK(th[i].mark == ts[i].mark);  // one Concrete
    CHECK(th.back().mark != ts.back().mark);
    // One per path: the path track is the layers' folder.
    CHECK(SameNames(PathOf(s, "heel", TrackLayout::Flat),
                    {{"RAV Library", true}, {"Steps", true}, {"Run_Shoes_Concrete", true}, {"Heel", false}}));
    // A sound outside any Blend stays on the path track itself (13-13).
    const std::vector<TrackStep> tw = PathOf(s, "wood", TrackLayout::Tree);
    CHECK(SameNames(tw, {{"RAV Library", true}, {"Steps", true}, {"Run", true}, {"Shoes", true}, {"Wood", false}}));
    CHECK(tw[3].mark == th[3].mark && tw[4].mark != th[4].mark);  // the same Shoes, another value
    // Off: the Blend folder in the library folder, every mode (B4).
    CHECK(SameNames(PathOf(s, "heel", TrackLayout::Off),
                    {{"RAV Library", true}, {"Steps", true}, {"Blend", true}, {"Heel", false}}));
    CHECK(PathOf(s, "wood", TrackLayout::Off).empty());
    // Marks are uids: the same path twice gives the same marks; another Lands-in, another library folder.
    const std::vector<TrackStep> again = PathOf(s, "heel", TrackLayout::Tree);
    for (size_t i = 0; i < th.size(); ++i) CHECK(again[i].mark == th[i].mark);
    CHECK(PathOf(s, "heel", TrackLayout::Tree, "Foley")[1].mark != th[1].mark);
    // A renamed value or layer: the same marks (found by mark, the name used only when made).
    SoundStore r = s;
    r.groups[2].values[0].name = "Stone";
    r.libraries.front().nodes[4].name = "Heel2";
    const std::vector<TrackStep> tr = PathOf(r, "heel", TrackLayout::Tree);
    CHECK(tr[4].name == "Stone" && tr[5].name == "Heel2");
    for (size_t i = 0; i < th.size(); ++i) CHECK(tr[i].mark == th[i].mark);
    // Flat and Tree never share a track.
    CHECK(PathOf(s, "heel", TrackLayout::Flat)[2].mark != th[4].mark);
}

static void TestLayerKinds()
{
    SoundStore   s;
    SoundLibrary lib;
    lib.uid = "lib";
    lib.name = "Foley";
    // Blend in Blend: Blend [Body, Impact = Blend [Click, Thud]].
    lib.nodes.push_back(Named("outer", "", SoundNodeType::Blend, "Hit"));
    lib.nodes.push_back(Named("body", "outer", SoundNodeType::Sound, "Body"));
    lib.nodes.push_back(Named("impact", "outer", SoundNodeType::Blend, "Impact"));
    lib.nodes.push_back(Named("click", "impact", SoundNodeType::Sound, "Click"));
    lib.nodes.push_back(Named("thud", "impact", SoundNodeType::Sound, "Thud"));
    s.libraries.push_back(lib);
    const BlendLayer body = LayerOf(s.libraries.front(), "body");
    const BlendLayer click = LayerOf(s.libraries.front(), "click");
    const BlendLayer thud = LayerOf(s.libraries.front(), "thud");
    CHECK(body.blend_uid == "outer" && click.blend_uid == "outer" && thud.blend_uid == "outer");
    CHECK(body.layer_name == "Body" && click.layer_name == "Click" && thud.layer_name == "Thud");
    const std::vector<TrackStep> pb = PathOf(s, "body", TrackLayout::Off);
    const std::vector<TrackStep> pc = PathOf(s, "click", TrackLayout::Tree);  // no Switch: as Off
    CHECK(SameNames(pb, {{"RAV Library", true}, {"Foley", true}, {"Hit", true}, {"Body", false}}));
    CHECK(SameNames(pc, {{"RAV Library", true}, {"Foley", true}, {"Hit", true}, {"Click", false}}));
    CHECK(pb[2].mark == pc[2].mark && pb[3].mark != pc[3].mark);  // side by side, no folder per inner Blend

    // Random layer: Blend [Cloth = Random (3 files), Heel]; same names: Blend [Thud, Thud].
    SoundStore   r;
    SoundLibrary l2;
    l2.uid = "l2";
    l2.name = "Steps";
    l2.nodes.push_back(Named("bl", "", SoundNodeType::Blend, "Footstep"));
    l2.nodes.push_back(Named("cloth", "bl", SoundNodeType::Random, "Cloth"));
    l2.nodes.push_back(Named("c1", "cloth", SoundNodeType::Sound, "a"));
    l2.nodes.push_back(Named("c2", "cloth", SoundNodeType::Sound, "b"));
    l2.nodes.push_back(Named("c3", "cloth", SoundNodeType::Sound, "c"));
    l2.nodes.push_back(Named("heel", "bl", SoundNodeType::Sound, "Heel"));
    l2.nodes.push_back(Named("t1", "bl", SoundNodeType::Sound, "Thud"));
    l2.nodes.push_back(Named("t2", "bl", SoundNodeType::Sound, "thud "));
    l2.nodes.push_back(Named("u", "bl", SoundNodeType::Sound, ""));
    r.libraries.push_back(l2);
    const SoundLibrary& L = r.libraries.front();
    CHECK(LayerOf(L, "c1").layer_uid == "cloth" && LayerOf(L, "c3").layer_name == "Cloth");
    CHECK(PathOf(r, "c1", TrackLayout::Off).back().mark == PathOf(r, "c2", TrackLayout::Off).back().mark);
    CHECK(PathOf(r, "c1", TrackLayout::Off).back().mark != PathOf(r, "heel", TrackLayout::Off).back().mark);
    // Off: RAV Library > library > Footstep > Heel.
    CHECK(SameNames(PathOf(r, "heel", TrackLayout::Off), {{"RAV Library", true}, {"Steps", true}, {"Footstep", true}, {"Heel", false}}));
    CHECK(LayerOf(L, "t1").layer_name == "Thud" && LayerOf(L, "t2").layer_name == "thud02");
    CHECK(LayerOf(L, "u").layer_name == "(unnamed)");
    // The Blend itself, and a node with no Blend above: no layer.
    CHECK(!LayerOf(L, "bl").in_blend);
    CHECK(!LayerOf(L, "nope").in_blend);
}

static void TestBlendAboveSwitch()
{
    // Blend [Heel > Switch Material > Wood, Scuff], One per path.
    SoundStore s;
    s.groups.push_back(Group("gMat", "Material", {{"vWood", "Wood"}}));
    SoundLibrary lib;
    lib.uid = "lib";
    lib.name = "Steps";
    lib.nodes.push_back(Named("bl", "", SoundNodeType::Blend, "Footstep"));
    SoundNode heel = Switch("heelsw", "bl", "gMat", {{"vWood", "w", {}}});
    heel.name = "Heel";
    lib.nodes.push_back(heel);
    lib.nodes.push_back(Named("w", "heelsw", SoundNodeType::Sound, "wood file"));
    lib.nodes.push_back(Named("scuff", "bl", SoundNodeType::Sound, "Scuff"));
    s.libraries.push_back(lib);
    CHECK(SameNames(PathOf(s, "w", TrackLayout::Flat), {{"RAV Library", true}, {"Steps", true}, {"Wood", true}, {"Heel", false}}));
    CHECK(SameNames(PathOf(s, "scuff", TrackLayout::Flat),
                    {{"RAV Library", true}, {"Steps", true}, {"Footstep", true}, {"Scuff", false}}));
    CHECK(MarkedOnce(PathOf(s, "w", TrackLayout::Flat)));
}

static void TestMarks()
{
    std::string base;
    CHECK(LaneOfMark(LaneMark("L|x|lib|Y|heel", 3), &base) == 3 && base == "L|x|lib|Y|heel");
    CHECK(LaneOfMark("L|x|lib|Y|heel", &base) == 0 && base == "L|x|lib|Y|heel");
    CHECK(LaneOfMark(LaneMark(GuidTrackMark("{AB}"), 12), &base) == 12 && base == "#{AB}");
    CHECK(LaneOfMark("a|S|", &base) == 0 && base == "a|S|");
    CHECK(LaneOfMark("a|S|x1", nullptr) == 0);
    CHECK(LaneOfMark("a|S|0", nullptr) == 0);
    CHECK(PileChildMark("m", "{G}") != PileChildMark("m", "{H}"));
    CHECK(LayerMark("p", "a") != LayerMark("q", "a"));
}

static void TestLanes()
{
    // Two events overlap: Heel 2 s long, events 0.3 s apart -> Heel 1 (1st item), Heel 2 (2nd).
    std::vector<std::vector<LaneSpan>> lanes;
    std::vector<int> got = SpillOnLanes({{0.3, 2.3, 2}, {0.0, 2.0, 1}}, &lanes);
    CHECK(lanes.size() == 2 && got[1] == 0 && got[0] == 1);
    // No overlap: Heel 0.2 s, events 0.3 s apart -> one lane.
    lanes.clear();
    got = SpillOnLanes({{0.0, 0.2, 1}, {0.3, 0.5, 2}}, &lanes);
    CHECK(lanes.size() == 1 && got[0] == 0 && got[1] == 0);
    CHECK(FitsLane({{0.0, 0.2, 1}}, {0.3, 0.5, 2}));
    // Touching: end = start does not overlap.
    CHECK(!SpansOverlap({0.0, 1.0, 1}, {1.0, 2.0, 2}));
    CHECK(!SpansOverlap({1.0, 2.0, 2}, {0.0, 1.0000000001, 1}));
    CHECK(SpansOverlap({0.0, 1.01, 1}, {1.0, 2.0, 2}));
    // Nested: a short item inside a long one overlaps.
    CHECK(SpansOverlap({0.0, 5.0, 1}, {1.0, 1.5, 2}));
    lanes.clear();
    got = SpillOnLanes({{0.0, 5.0, 1}, {1.0, 1.5, 2}, {2.0, 2.5, 3}}, &lanes);
    CHECK(lanes.size() == 2 && got[0] == 0 && got[1] == 1 && got[2] == 1);
    // A gap reused: lane 0 is free again after its item ends.
    const std::vector<std::vector<LaneSpan>> two = {{{0.0, 1.0, 1}, {3.0, 4.0, 3}}, {{0.5, 1.5, 2}}};
    CHECK(PickLane(two, {1.0, 3.0, 9}) == 0);
    CHECK(PickLane(two, {0.9, 1.2, 9}) == -1);
    CHECK(PickLane(two, {1.2, 2.0, 9}) == 0);
    CHECK(PickLane(two, {3.5, 3.8, 9}) == 1);
    // The replaced item itself is excluded (it may stay where it is).
    CHECK(PickLane(two, {0.0, 1.2, 1}, 1) == 0);
    CHECK(PickLane(two, {0.0, 1.2, 1}) == -1);
    CHECK(FitsLane(two[0], {0.0, 1.2, 1}, 1));
    CHECK(!FitsLane(two[0], {0.0, 1.2, 1}));
    CHECK(PickLane({}, {0.0, 1.0, 1}) == -1);
    // Existing lanes are filled first.
    lanes = {{{0.0, 1.0, 7}}};
    got = SpillOnLanes({{2.0, 3.0, 1}, {0.5, 0.8, 2}}, &lanes);
    CHECK(lanes.size() == 2 && got[0] == 0 && got[1] == 1);
}

// The spill mover choice (B6, B6b, B7b) and the moved-by-user rule.
static void TestSpillPlan()
{
    const std::string base = "L|RAV Library|lib|Y|heel";
    SpillPlan         p;
    // Fits: nothing moves.
    CHECK(!PlanSpill({{0.0, 0.2, -1}}, {base}, base, {0.3, 0.5, -1}, &p) && p.lanes == 0 && p.lane_of[0] == -1);
    // Two events overlap: RAV's item to Heel 1, the new one to Heel 2.
    CHECK(PlanSpill({{0.0, 2.0, -1}}, {base}, base, {0.3, 2.3, -1}, &p));
    CHECK(p.lanes == 2 && p.lane_of[0] == 0 && p.item_lane == 1);
    CHECK(LaneMark(base, p.lane_of[0] + 1) == base + "|S|1" && LaneMark(base, p.item_lane + 1) == base + "|S|2");
    // The user's own item (tk "") and one the user moved there (another tk) stay; they still count
    // as overlapping (the spill happens), the new item takes lane 1.
    CHECK(PlanSpill({{0.0, 2.0, -1}, {5.0, 6.0, -1}}, {"", "PT|x|lib|g:v|Y|scuff"}, base, {0.3, 2.3, -1}, &p));
    CHECK(p.lanes == 1 && p.lane_of[0] == -1 && p.lane_of[1] == -1 && p.item_lane == 0);
    // RAV's items that do not overlap each other share lane 1; the new one takes the first that fits.
    CHECK(PlanSpill({{0.0, 1.0, -1}, {2.0, 3.0, -1}}, {base, base}, base, {0.5, 2.5, -1}, &p));
    CHECK(p.lanes == 2 && p.lane_of[0] == 0 && p.lane_of[1] == 0 && p.item_lane == 1);
    // A lane item of another base (tk base|S|1 of a moved item) is not this track's: stays.
    CHECK(PlanSpill({{0.0, 2.0, -1}}, {LaneMark(base, 1)}, base, {1.0, 3.0, -1}, &p) && p.lane_of[0] == -1);
    // Once spilled: the first child that fits, else a new k above the highest (a deleted child skipped).
    bool made = true;
    CHECK(PickLaneK({1, 3}, {{{0.0, 1.0, -1}}, {}}, {0.5, 0.8, -1}, &made) == 3 && !made);
    CHECK(PickLaneK({1, 3}, {{{0.0, 1.0, -1}}, {}}, {1.0, 1.5, -1}, &made) == 1 && !made);
    CHECK(PickLaneK({1, 3}, {{{0.0, 1.0, -1}}, {{0.0, 1.0, -1}}}, {0.5, 0.8, -1}, &made) == 4 && made);
    CHECK(PickLaneK({}, {}, {0.0, 1.0, -1}, &made) == 1 && made);
    // Moved by the user (B7b).
    std::string b;
    CHECK(PlacedSpotOf("", false, &b) == PlacedSpot::NotRav && b.empty());
    CHECK(PlacedSpotOf("", true) == PlacedSpot::NotRav);
    CHECK(PlacedSpotOf(base, true, &b) == PlacedSpot::OnRavTrack && b == base);
    CHECK(PlacedSpotOf(base, false, &b) == PlacedSpot::MovedByUser && b == base);
    CHECK(PlacedSpotOf(LaneMark(base, 2), true, &b) == PlacedSpot::OnRavTrack && b == base);
    CHECK(PlacedSpotOf(LaneMark(GuidTrackMark("{G}"), 1), false, &b) == PlacedSpot::MovedByUser && b == "#{G}");
}

// ---- Story 13-17 ----------------------------------------------------------------------------

static void TestSeparator()
{
    SoundStore s;
    CHECK(TrackSeparatorOf(s) == "_");  // no token: "_" (Default row)
    CHECK(!SetTrackSeparator(s, "_"));
    CHECK(!SetTrackSeparator(s, "/"));  // not offered
    s.kept = {"zz=1", "trk=flat"};
    CHECK(SetTrackSeparator(s, "-"));
    CHECK(TrackSeparatorOf(s) == "-");
    CHECK(s.kept.size() == 3 && s.kept[0] == "zz=1" && s.kept[1] == "trk=flat" && s.kept[2] == "sep=dash");
    CHECK(!SetTrackSeparator(s, "-"));
    CHECK(SetTrackSeparator(s, " ") && TrackSeparatorOf(s) == " " && s.kept[2] == "sep=space");
    CHECK(SetTrackSeparator(s, ".") && TrackSeparatorOf(s) == ".");
    CHECK(SetTrackSeparator(s, "_") && s.kept.size() == 2 && TrackSeparatorOf(s) == "_");
    s.kept = {"sep=tilde"};
    CHECK(TrackSeparatorOf(s) == "_");
    CHECK(TrackSeparators().size() == 4 && TrackSeparators()[0] == "_");
    // Round trip through the store's text.
    SoundStore r = NestedStore();
    CHECK(SetTrackSeparator(r, "."));
    SoundStore back;
    CHECK(ParseSoundStore(SerializeSoundStore(r), &back));
    CHECK(TrackSeparatorOf(back) == ".");
    // The joined path follows it; default "_".
    const std::vector<SwitchStep> p = SwitchPathOf(r, r.libraries.front(), "conc");
    CHECK(SoundTrackPath(p, BlendLayer(), TrackLayout::Flat, "RAV Library", "lib", "Feet").back().name == "Run_Shoes_Concrete");
    CHECK(SoundTrackPath(p, BlendLayer(), TrackLayout::Flat, "RAV Library", "lib", "Feet", "-").back().name ==
          "Run-Shoes-Concrete");
    CHECK(NumberedName("Heel", 1) == "Heel01" && NumberedName("Thud", 3) == "Thud03" && NumberedName("Heel", 100) == "Heel100");
}

static std::string NoTrack(const std::string&)
{
    return {};
}

static void TestNameForMark()
{
    SoundStore s = BlendUnderPath();
    // Path marks: the names SoundTrackPath gives (the separator of the store).
    for (TrackLayout lay : {TrackLayout::Flat, TrackLayout::Tree, TrackLayout::Off}) {
        const std::vector<TrackStep> p = SoundTrackPath(SwitchPathOf(s, s.libraries.front(), "heel"),
                                                        LayerOf(s.libraries.front(), "heel"), lay, "RAV Library", "lib",
                                                        "Steps", TrackSeparatorOf(s));
        for (size_t i = 1; i < p.size(); ++i) CHECK(NameForMark(s, p[i].mark, NoTrack) == p[i].name);
    }
    const std::vector<TrackStep> flat =
        SoundTrackPath(SwitchPathOf(s, s.libraries.front(), "heel"), LayerOf(s.libraries.front(), "heel"),
                       TrackLayout::Flat, "RAV|Library", "lib", "Steps");  // a '|' in the Lands-in value
    CHECK(NameForMark(s, flat[1].mark, NoTrack) == "Steps");
    CHECK(NameForMark(s, flat[2].mark, NoTrack) == "Run_Shoes_Concrete");  // Default: "_"
    CHECK(NameForMark(s, flat[3].mark, NoTrack) == "Heel");
    // Separator changed: Run-Shoes-Concrete.
    SoundStore d = s;
    SetTrackSeparator(d, "-");
    CHECK(NameForMark(d, flat[2].mark, NoTrack) == "Run-Shoes-Concrete");
    // Layer renamed: Heel; its spill children Heel01, Heel02.
    SoundStore r = s;
    r.libraries.front().nodes[4].name = "FEETCrea_CONCRETE-Gloves Rubber On Gloves Boxing_B00M_CFCK";
    CHECK(NameForMark(r, flat[3].mark, NoTrack) == "FEETCrea_CONCRETE-Gloves Rubber On Gloves Boxing_B00M_CFCK");
    r.libraries.front().nodes[4].name = "Heel";
    CHECK(NameForMark(r, LaneMark(flat[3].mark, 1), NoTrack) == "Heel01");
    CHECK(NameForMark(r, LaneMark(flat[3].mark, 2), NoTrack) == "Heel02");
    // Folders, value renamed Concrete -> Stone.
    const std::vector<TrackStep> tree = PathOf(s, "heel", TrackLayout::Tree);
    SoundStore v = s;
    v.groups[2].values[0].name = "Stone";
    CHECK(NameForMark(v, tree[4].mark, NoTrack) == "Stone");
    // Library and Blend renamed.
    v.libraries.front().name = "Feet";
    v.libraries.front().nodes[3].name = "Step";
    CHECK(NameForMark(v, tree[1].mark, NoTrack) == "Feet");
    const std::vector<TrackStep> off = PathOf(v, "heel", TrackLayout::Off);
    CHECK(NameForMark(v, off[2].mark, NoTrack) == "Step");
    // NVK pile child: its source child track's current name; its spill child numbered.
    auto names = [](const std::string& m) { return m == "#{SRC}" ? std::string("Kick") : std::string("Layer 1"); };
    const std::string pile = PileChildMark(flat[3].mark, "#{SRC}");
    CHECK(NameForMark(s, pile, names) == "Kick");
    CHECK(NameForMark(s, LaneMark(pile, 2), names) == "Kick02");
    // 13-11's "layer N" (a GuidTrackMark): never named; its spill children follow its name.
    CHECK(NameForMark(s, GuidTrackMark("{G}"), names).empty());
    CHECK(NameForMark(s, LaneMark(GuidTrackMark("{G}"), 1), names) == "Layer 101");
    // Unknown: a library or node gone, a foreign mark.
    SoundStore gone = s;
    gone.libraries.clear();
    CHECK(NameForMark(gone, flat[3].mark, NoTrack).empty());
    CHECK(NameForMark(s, "L|x|nolib", NoTrack).empty());
    CHECK(NameForMark(s, "whatever", NoTrack).empty());
    CHECK(NameForMark(s, LayerMark(flat[2].mark, "nonode"), NoTrack).empty());
    // A deleted Switch value or group: no Library name (the track keeps its name).
    SoundStore dv = s;
    dv.groups[1].values.clear();  // Shoes gone
    CHECK(NameForMark(dv, flat[2].mark, NoTrack).empty());
    CHECK(NameForMark(dv, tree[3].mark, NoTrack).empty());
    SoundStore dg = s;
    dg.groups.erase(dg.groups.begin() + 2);  // Material gone
    CHECK(NameForMark(dg, flat[2].mark, NoTrack).empty());
    CHECK(NameForMark(dg, tree[4].mark, NoTrack).empty());
    CHECK(NameForMark(dg, tree[2].mark, NoTrack) == "Run");  // the levels above still named
    // Same-name layers: Thud, Thud02, Thud03.
    SoundStore   t;
    SoundLibrary l;
    l.uid = "lt";
    l.name = "Hits";
    l.nodes.push_back(Named("bl", "", SoundNodeType::Blend, "Hit"));
    l.nodes.push_back(Named("a", "bl", SoundNodeType::Sound, "Thud"));
    l.nodes.push_back(Named("b", "bl", SoundNodeType::Sound, "Thud"));
    l.nodes.push_back(Named("c", "bl", SoundNodeType::Sound, "Thud"));
    t.libraries.push_back(l);
    const char* want[] = {"Thud", "Thud02", "Thud03"};
    const char* uids[] = {"a", "b", "c"};
    for (int i = 0; i < 3; ++i) {
        const std::vector<TrackStep> p = PathOf(t, uids[i], TrackLayout::Off);
        CHECK(p.back().name == want[i]);
        CHECK(NameForMark(t, p.back().mark, NoTrack) == want[i]);
    }
}

static void TestPlanTrackRename()
{
    TrackRename r = PlanTrackRename("Heel L", "Heel", "Heel2");  // renamed by the user: untouched
    CHECK(!r.rename && !r.record);
    r = PlanTrackRename("Heel 1", "", "Heel01");  // made before 13-17: renamed and recorded
    CHECK(r.rename && r.record);
    r = PlanTrackRename("FEET_CFCK", "FEET_CFCK", "Heel");  // untouched, Library renamed
    CHECK(r.rename && r.record);
    r = PlanTrackRename("Heel", "", "Heel");  // right name, nothing recorded: only recorded
    CHECK(!r.rename && r.record);
    r = PlanTrackRename("Heel", "Heel", "Heel");  // all equal: nothing
    CHECK(!r.rename && !r.record);
    r = PlanTrackRename("Heel", "Heel", "");  // no Library name: nothing
    CHECK(!r.rename && !r.record);
}

int main()
{
    TestPlanTrackRename();
    TestSeparator();
    TestNameForMark();
    TestSpillPlan();
    TestLayerUnderPath();
    TestLayerKinds();
    TestBlendAboveSwitch();
    TestMarks();
    TestLanes();
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
