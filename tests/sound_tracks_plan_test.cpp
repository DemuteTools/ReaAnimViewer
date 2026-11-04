// SPDX-License-Identifier: MIT
//
// Host tests of src/sound_tracks_plan.h (stories 13-13, 13-16, 13-17, 13-39): every row of the specs' I/O
// matrices (the NVK pile row is REAPER-side: sound_populate.cpp) and their test plans.

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

// 13-39: the 13-13 "One per path" and "Folders" layouts as templates of a Switch path's groups.
static std::vector<std::string> FlatWords(const std::vector<SwitchStep>& path, bool layer = false)
{
    std::vector<std::string> w = {"lib", "end"};
    for (const SwitchStep& s : path) w.push_back("g:" + s.group_uid);
    if (layer) w.push_back("layer");
    return w;
}

// The 13-13 "Folders" layout as a template: the library, the path's groups in order, the layer, "end".
static std::vector<std::string> TreeWords(const std::vector<SwitchStep>& path, bool layer = false)
{
    std::vector<std::string> w = {"lib"};
    for (const SwitchStep& s : path) w.push_back("g:" + s.group_uid);
    if (layer) w.push_back("layer");
    w.push_back("end");
    return w;
}

// A Switch path with no Blend layer (13-13's TrackPathFor cases, on templates since 13-39). Off = an empty
// template (nothing gives a value: today's track).
enum class Shape { Off, Flat, Tree };
static std::vector<TrackStep> PathOnly(const std::vector<SwitchStep>& path, Shape shape, const std::string& lands,
                                       const std::string& lib_name)
{
    const std::vector<std::string> words = shape == Shape::Off    ? std::vector<std::string>{"end"}
                                           : shape == Shape::Flat ? FlatWords(path)
                                                                  : TreeWords(path);
    return TemplateTrackPath(words, path, BlendLayer(), lands, "lib", lib_name);
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
    CHECK(SameSteps(PathOnly(SwitchPathOf(s, lib, "s4"), Shape::Flat, "RAV Library", "L"), {{"RAV Library", true}, {"L", false}}));
    CHECK(SameSteps(PathOnly(SwitchPathOf(s, lib, "s3"), Shape::Flat, "RAV Library", "L"),
                    {{"RAV Library", true}, {"L", true}, {"Wood", false}}));
    // 13-16: with its Blend layer (the tree order), Wood becomes the folder of the layer (the Switch "mat2").
    const std::vector<SwitchStep> p3 = SwitchPathOf(s, lib, "s3");
    CHECK(SameSteps(TemplateTrackPath(TreeWords(p3, true), p3, LayerOf(lib, "s3"), "RAV Library",
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
    CHECK(SameSteps(PathOnly(p, Shape::Flat, "RAV Library", "Feet"),
                    {{"RAV Library", true}, {"Feet", true}, {"Run_Shoes_Concrete", false}}));
    CHECK(SameSteps(PathOnly(p, Shape::Tree, "RAV Library", "Feet"),
                    {{"RAV Library", true}, {"Feet", true}, {"Run", true}, {"Shoes", true}, {"Concrete", false}}));
    CHECK(PathOnly(p, Shape::Off, "RAV Library", "Feet").empty());
    // No Switch path: the library track holds the sounds (13-39 T2: the library level gives a value).
    CHECK(SameSteps(PathOnly({}, Shape::Tree, "RAV Library", "Feet"), {{"RAV Library", true}, {"Feet", false}}));
    // A user library's Lands-in folder.
    CHECK(SameSteps(PathOnly(p, Shape::Flat, "Foley", "Steps"),
                    {{"Foley", true}, {"Steps", true}, {"Run_Shoes_Concrete", false}}));
    // Unnamed value.
    const std::vector<SwitchStep> e = SwitchPathOf(s, s.libraries.front(), "empty");
    CHECK(SameSteps(PathOnly(e, Shape::Flat, "RAV Library", "Feet"),
                    {{"RAV Library", true}, {"Feet", true}, {"Run_Shoes_(unnamed)", false}}));
    CHECK(SameSteps(PathOnly(e, Shape::Tree, "RAV Library", "Feet"),
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

// A Sound's path from template `words` (none = the library's, TrackTemplateOf: its tree order when none is stored).
static std::vector<TrackStep> PathOf(const SoundStore& s, const std::string& sound, std::vector<std::string> words = {},
                                     const std::string& lands = "RAV Library")
{
    const SoundLibrary&           lib = s.libraries.front();
    const std::vector<SwitchStep> path = SwitchPathOf(s, lib, sound);
    const BlendLayer              layer = LayerOf(lib, sound);
    if (words.empty()) words = TrackTemplateOf(s, lib);
    return TemplateTrackPath(words, path, layer, lands, lib.uid, lib.name, TrackSeparatorOf(s));
}

static const std::vector<std::string> kBlendWords = {"lib", "blend", "layer", "end"};

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
    const std::vector<TrackStep> th = PathOf(s, "heel");
    const std::vector<TrackStep> ts = PathOf(s, "scuff");
    CHECK(SameNames(th, {{"RAV Library", true}, {"Steps", true}, {"Run", true}, {"Shoes", true}, {"Concrete", true}, {"Heel", false}}));
    CHECK(SameNames(ts, {{"RAV Library", true}, {"Steps", true}, {"Run", true}, {"Shoes", true}, {"Concrete", true}, {"Scuff", false}}));
    CHECK(MarkedOnce(th) && MarkedOnce(ts));
    for (size_t i = 0; i + 1 < th.size(); ++i) CHECK(th[i].mark == ts[i].mark);  // one Concrete
    CHECK(th.back().mark != ts.back().mark);
    // The values joined after "folders end", the layer with them.
    CHECK(SameNames(PathOf(s, "heel", FlatWords(SwitchPathOf(s, s.libraries.front(), "heel"), true)),
                    {{"RAV Library", true}, {"Steps", true}, {"Run_Shoes_Concrete_Heel", false}}));
    // A sound outside any Blend stays on the path track itself (13-13).
    const std::vector<TrackStep> tw = PathOf(s, "wood");
    CHECK(SameNames(tw, {{"RAV Library", true}, {"Steps", true}, {"Run", true}, {"Shoes", true}, {"Wood", false}}));
    CHECK(tw[3].mark == th[3].mark && tw[4].mark != th[4].mark);  // the same Shoes, another value
    // A Blend level: the Blend folder in the library folder (B4); outside the Blend, skipped.
    CHECK(SameNames(PathOf(s, "heel", kBlendWords), {{"RAV Library", true}, {"Steps", true}, {"Blend", true}, {"Heel", false}}));
    CHECK(SameNames(PathOf(s, "wood", kBlendWords), {{"RAV Library", true}, {"Steps", false}}));
    CHECK(PathOf(s, "wood", {"end"}).empty());
    // Marks are uids: the same path twice gives the same marks; another Lands-in, another library folder.
    const std::vector<TrackStep> again = PathOf(s, "heel");
    for (size_t i = 0; i < th.size(); ++i) CHECK(again[i].mark == th[i].mark);
    CHECK(PathOf(s, "heel", {}, "Foley")[1].mark != th[1].mark);
    // A renamed value or layer: the same marks (found by mark, the name used only when made).
    SoundStore r = s;
    r.groups[2].values[0].name = "Stone";
    r.libraries.front().nodes[4].name = "Heel2";
    const std::vector<TrackStep> tr = PathOf(r, "heel");
    CHECK(tr[4].name == "Stone" && tr[5].name == "Heel2");
    for (size_t i = 0; i < th.size(); ++i) CHECK(tr[i].mark == th[i].mark);
    // A joined track never shares a folder's mark.
    CHECK(PathOf(s, "heel", FlatWords(SwitchPathOf(s, s.libraries.front(), "heel")))[2].mark != th[4].mark);
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
    const std::vector<TrackStep> pb = PathOf(s, "body");
    const std::vector<TrackStep> pc = PathOf(s, "click");  // T1: no Blend level by default, the layers in the library
    CHECK(SameNames(pb, {{"RAV Library", true}, {"Foley", true}, {"Body", false}}));
    CHECK(SameNames(pc, {{"RAV Library", true}, {"Foley", true}, {"Click", false}}));
    CHECK(pb[1].mark == pc[1].mark && pb[2].mark != pc[2].mark);
    // With a Blend level: one Blend folder, side by side, no folder per inner Blend.
    const std::vector<TrackStep> qb = PathOf(s, "body", kBlendWords), qc = PathOf(s, "click", kBlendWords);
    CHECK(SameNames(qb, {{"RAV Library", true}, {"Foley", true}, {"Hit", true}, {"Body", false}}));
    CHECK(SameNames(qc, {{"RAV Library", true}, {"Foley", true}, {"Hit", true}, {"Click", false}}));
    CHECK(qb[2].mark == qc[2].mark && qb[3].mark != qc[3].mark);

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
    CHECK(PathOf(r, "c1").back().mark == PathOf(r, "c2").back().mark);
    CHECK(PathOf(r, "c1").back().mark != PathOf(r, "heel").back().mark);
    // No Switch: RAV Library > library > Heel (T1: no Blend level by default); with one, > Footstep > Heel.
    CHECK(SameNames(PathOf(r, "heel"), {{"RAV Library", true}, {"Steps", true}, {"Heel", false}}));
    CHECK(SameNames(PathOf(r, "heel", kBlendWords), {{"RAV Library", true}, {"Steps", true}, {"Footstep", true}, {"Heel", false}}));
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
    CHECK(SameNames(PathOf(s, "w"), {{"RAV Library", true}, {"Steps", true}, {"Wood", true}, {"Heel", false}}));
    CHECK(SameNames(PathOf(s, "scuff"), {{"RAV Library", true}, {"Steps", true}, {"Scuff", false}}));
    CHECK(SameNames(PathOf(s, "scuff", kBlendWords),
                    {{"RAV Library", true}, {"Steps", true}, {"Footstep", true}, {"Scuff", false}}));
    CHECK(MarkedOnce(PathOf(s, "w")));
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

// 13-62h review (R1): two pieces of one placement crossfading at a Gap stay on one lane whatever spills later.
static LaneSpan Piece(double a, double b, const std::string& group, double fin, double fout)
{
    LaneSpan s{a, b, -1};
    s.group = group;
    s.fade_in = fin;
    s.fade_out = fout;
    return s;
}

static void TestCrossfadeButted()
{
    PlacedTag t;
    t.owner = "{A}";
    t.rid = "r1";
    t.t = 1.25;
    t.node = "n";
    const std::string g = PlacementGroup(t);
    CHECK(!g.empty() && g != PlacementGroup(PlacedTag{}) && PlacementGroup(PlacedTag{}).empty());
    PlacedTag u = t;
    u.t = 1.30;
    CHECK(PlacementGroup(u) != g);
    // Heel [10.0, 10.305] fading out over its last 10 ms, Heel [10.295, 10.55] fading in over 10 ms: one placement.
    const LaneSpan L = Piece(10.0, 10.305, g, 0.005, 0.010), R = Piece(10.295, 10.55, g, 0.010, 0.005);
    CHECK(MayCrossfade(L, R) && MayCrossfade(R, L) && CrossfadeButted(L, R) && !SpansOverlap(L, R) && !SpansOverlap(R, L));
    // another placement, no group, or an overlap longer than the fades: an overlap
    CHECK(SpansOverlap(L, Piece(10.295, 10.55, PlacementGroup(u), 0.010, 0.005)));
    CHECK(SpansOverlap(L, Piece(10.295, 10.55, "", 0.010, 0.005)));
    CHECK(SpansOverlap(L, Piece(10.290, 10.55, g, 0.010, 0.005)));
    CHECK(SpansOverlap(Piece(10.0, 10.6, g, 0.0, 0.6), Piece(10.1, 10.5, g, 0.4, 0.0)));  // inside it
    // Another event spills on the track: the pair keeps one lane, the new item goes to another.
    std::vector<std::vector<LaneSpan>> lanes;
    std::vector<int>                   got = SpillOnLanes({L, R, Piece(10.2, 10.4, "x", 0.0, 0.0)}, &lanes);
    CHECK(lanes.size() == 2 && got[0] == 0 && got[1] == 0 && got[2] == 1);
    // and a later fix of either piece finds it fits next to the other
    CHECK(FitsLane({L}, R) && FitsLane({R}, L));
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
    // 13-19: whatever the tags say, PlanSpill stays (false) exactly when the item fits the track, so
    // SettleOnLanes reads the tags only once the item does not fit.
    {
        const std::vector<std::vector<LaneSpan>> tracks = {
            {}, {{0.0, 0.2, -1}}, {{0.0, 2.0, -1}}, {{0.0, 1.0, -1}, {2.0, 3.0, -1}}, {{0.5, 0.5, -1}}};
        const std::vector<LaneSpan> items = {{0.3, 0.5, -1}, {0.3, 2.3, -1}, {1.0, 2.0, -1}, {3.0, 4.0, -1}, {0.5, 0.5, -1}};
        for (const auto& t : tracks)
            for (const LaneSpan& it : items)
                for (const std::string& tk : {std::string(), base, std::string("other")}) {
                    const std::vector<std::string> tks(t.size(), tk);
                    CHECK(PlanSpill(t, tks, base, it, &p) == !FitsLane(t, it));
                }
    }
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
    s.kept = {"zz=1", "trk=flat"};  // an old 13-13 token: kept as read, never used
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
    CHECK(TemplateTrackPath(FlatWords(p), p, BlendLayer(), "RAV Library", "lib", "Feet").back().name == "Run_Shoes_Concrete");
    CHECK(TemplateTrackPath(FlatWords(p), p, BlendLayer(), "RAV Library", "lib", "Feet", "-").back().name ==
          "Run-Shoes-Concrete");
    CHECK(NumberedName("Heel", 1) == "Heel01" && NumberedName("Thud", 3) == "Thud03" && NumberedName("Heel", 100) == "Heel100");
}

static std::string NoTrack(const std::string&)
{
    return {};
}

static void TestNameForMark()
{
    SoundStore                     s = BlendUnderPath();
    const SoundLibrary&            L = s.libraries.front();
    const std::vector<SwitchStep>  hp = SwitchPathOf(s, L, "heel");
    const std::vector<std::string> joined = FlatWords(hp);           // Steps > Run_Shoes_Concrete
    std::vector<std::string>       with_layer = FlatWords(hp, true);  // Steps > Run_Shoes_Concrete_Heel
    // Template marks: the names TemplateTrackPath gives (the separator of the store), every shape.
    for (const std::vector<std::string>& w :
         {std::vector<std::string>{}, joined, with_layer, kBlendWords,
          std::vector<std::string>{"c:My|odd,folder>x", "g:gMat", "end", "layer", "c:B"}}) {
        const std::vector<TrackStep> p = PathOf(s, "heel", w);
        CHECK(!p.empty());
        for (size_t i = 1; i < p.size(); ++i) CHECK(NameForMark(s, p[i].mark, NoTrack) == p[i].name);
    }
    const std::vector<TrackStep> c = PathOf(s, "heel", {"c:My|odd,folder>x", "g:gMat", "end", "layer", "c:B"});
    CHECK(SameNames(c, {{"RAV Library", true}, {"My|odd,folder>x", true}, {"Concrete", true}, {"Heel_B", false}}));
    const std::vector<TrackStep> flat = PathOf(s, "heel", with_layer, "RAV|Library");  // a '|' in the Lands-in value
    CHECK(NameForMark(s, flat[1].mark, NoTrack) == "Steps");
    CHECK(NameForMark(s, flat[2].mark, NoTrack) == "Run_Shoes_Concrete_Heel");  // Default: "_"
    // Separator changed: Run-Shoes-Concrete-Heel.
    SoundStore d = s;
    SetTrackSeparator(d, "-");
    CHECK(NameForMark(d, flat[2].mark, NoTrack) == "Run-Shoes-Concrete-Heel");
    // Layer renamed: Heel; its spill children Heel01, Heel02.
    const std::vector<TrackStep> tree = PathOf(s, "heel");
    SoundStore                   r = s;
    r.libraries.front().nodes[4].name = "FEETCrea_CONCRETE-Gloves Rubber On Gloves Boxing_B00M_CFCK";
    CHECK(NameForMark(r, tree[5].mark, NoTrack) == "FEETCrea_CONCRETE-Gloves Rubber On Gloves Boxing_B00M_CFCK");
    r.libraries.front().nodes[4].name = "Heel";
    CHECK(NameForMark(r, LaneMark(tree[5].mark, 1), NoTrack) == "Heel01");
    CHECK(NameForMark(r, LaneMark(tree[5].mark, 2), NoTrack) == "Heel02");
    // Folders, value renamed Concrete -> Stone.
    SoundStore v = s;
    v.groups[2].values[0].name = "Stone";
    CHECK(NameForMark(v, tree[4].mark, NoTrack) == "Stone");
    CHECK(NameForMark(v, PathOf(s, "heel", joined)[2].mark, NoTrack) == "Run_Shoes_Stone");
    // Library and Blend renamed.
    v.libraries.front().name = "Feet";
    v.libraries.front().nodes[3].name = "Step";
    CHECK(NameForMark(v, tree[1].mark, NoTrack) == "Feet");
    const std::vector<TrackStep> bl = PathOf(v, "heel", kBlendWords);
    CHECK(NameForMark(v, bl[2].mark, NoTrack) == "Step");
    // NVK pile child: its source child track's current name; its spill child numbered.
    auto names = [](const std::string& m) { return m == "#{SRC}" ? std::string("Kick") : std::string("Layer 1"); };
    const std::string pile = PileChildMark(tree[5].mark, "#{SRC}");
    CHECK(NameForMark(s, pile, names) == "Kick");
    CHECK(NameForMark(s, LaneMark(pile, 2), names) == "Kick02");
    // 13-11's "layer N" (a GuidTrackMark): never named; its spill children follow its name.
    CHECK(NameForMark(s, GuidTrackMark("{G}"), names).empty());
    CHECK(NameForMark(s, LaneMark(GuidTrackMark("{G}"), 1), names) == "Layer 101");
    // Unknown: a library or node gone, a foreign mark, a 13-13 / 13-16 mark (not adopted, T4 / Never).
    SoundStore gone = s;
    gone.libraries.clear();
    CHECK(NameForMark(gone, tree[5].mark, NoTrack).empty());
    CHECK(NameForMark(s, "T|x|nolib|l", NoTrack).empty());
    CHECK(NameForMark(s, "whatever", NoTrack).empty());
    CHECK(NameForMark(s, "L|RAV Library|lib", NoTrack).empty());
    CHECK(NameForMark(s, "PT|RAV Library|lib|gLoco:vRun", NoTrack).empty());
    CHECK(NameForMark(s, tree[4].mark + ",ynonode", NoTrack).empty());
    // A deleted Switch value or group: no Library name (the track keeps its name).
    SoundStore dv = s;
    dv.groups[1].values.clear();  // Shoes gone
    CHECK(NameForMark(dv, PathOf(s, "heel", joined)[2].mark, NoTrack).empty());
    CHECK(NameForMark(dv, tree[3].mark, NoTrack).empty());
    SoundStore dg = s;
    dg.groups.erase(dg.groups.begin() + 2);  // Material gone
    CHECK(NameForMark(dg, PathOf(s, "heel", joined)[2].mark, NoTrack).empty());
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
        const std::vector<TrackStep> p = PathOf(t, uids[i]);
        CHECK(p.back().name == want[i]);
        CHECK(NameForMark(t, p.back().mark, NoTrack) == want[i]);
    }
    // The preview's level labels.
    CHECK(MarkLevelLabel(s, tree[1].mark) == "library");
    CHECK(MarkLevelLabel(s, tree[2].mark) == "Loco");
    CHECK(MarkLevelLabel(s, tree[5].mark) == "Layer");
    CHECK(MarkLevelLabel(s, bl[2].mark) == "Blend");
    CHECK(MarkLevelLabel(s, c[1].mark) == "your folder");
    CHECK(MarkLevelLabel(s, c[3].mark) == "name: Layer + your folder");
    CHECK(MarkLevelLabel(s, "#{G}").empty());
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

// ---- Story 13-39: the Track template ------------------------------------------------------------

// The spec's library Footstep: Switch SurfaceMaterial (Concrete, Wood) > LocomotionType (Walk, Run); Concrete >
// Walk = Blend Step (Heel, Toe); Concrete > Run = Random "Run concrete" (one Sound, no layer); Wood > Walk and
// Wood > Run = a Sound each.
static SoundStore Footstep()
{
    SoundStore s;
    s.groups.push_back(Group("gSurf", "SurfaceMaterial", {{"vConc", "Concrete"}, {"vWood", "Wood"}}));
    s.groups.push_back(Group("gLoco", "LocomotionType", {{"vWalk", "Walk"}, {"vRun", "Run"}}));
    SoundLibrary lib;
    lib.uid = "fs";
    lib.name = "Footstep";
    lib.root = "surf";
    lib.nodes.push_back(Switch("surf", "", "gSurf", {{"vConc", "cloc", {}}, {"vWood", "wloc", {}}}));
    lib.nodes.push_back(Switch("cloc", "surf", "gLoco", {{"vWalk", "step", {}}, {"vRun", "runc", {}}}));
    lib.nodes.push_back(Named("step", "cloc", SoundNodeType::Blend, "Step"));
    lib.nodes.push_back(Named("heel", "step", SoundNodeType::Sound, "Heel"));
    lib.nodes.push_back(Named("toe", "step", SoundNodeType::Sound, "Toe"));
    lib.nodes.push_back(Named("runc", "cloc", SoundNodeType::Random, "Run concrete"));
    lib.nodes.push_back(Named("rc1", "runc", SoundNodeType::Sound, "rc1"));
    lib.nodes.push_back(Switch("wloc", "surf", "gLoco", {{"vWalk", "walkw", {}}, {"vRun", "runw", {}}}));
    lib.nodes.push_back(Named("walkw", "wloc", SoundNodeType::Sound, "Walk wood"));
    lib.nodes.push_back(Named("runw", "wloc", SoundNodeType::Sound, "Run wood"));
    s.libraries.push_back(lib);
    return s;
}

static std::vector<TrackStep> LibPath(const SoundStore& s, const std::string& sound)
{
    return SoundTemplatePath(s, s.libraries.front(), sound);
}

static void TestTemplateMatrix()
{
    SoundStore s = Footstep();
    // Default (T1): Lands-in > Footstep > Concrete > Walk > Heel / Toe; the chips: the tree order.
    CHECK(SameNames(LibPath(s, "heel"),
                    {{"RAV Library", true}, {"Footstep", true}, {"Concrete", true}, {"Walk", true}, {"Heel", false}}));
    CHECK(SameNames(LibPath(s, "toe"),
                    {{"RAV Library", true}, {"Footstep", true}, {"Concrete", true}, {"Walk", true}, {"Toe", false}}));
    CHECK(SameNames(LibPath(s, "rc1"), {{"RAV Library", true}, {"Footstep", true}, {"Concrete", true}, {"Run", false}}));
    CHECK(MarkedOnce(LibPath(s, "heel")));
    CHECK(LibPath(s, "heel")[3].mark == LibPath(s, "toe")[3].mark);  // one Walk folder
    CHECK(LibPath(s, "heel")[2].mark == LibPath(s, "rc1")[2].mark);  // one Concrete
    CHECK((DefaultTrackTemplate(s, s.libraries.front()) == std::vector<std::string>{"lib", "g:gSurf", "g:gLoco", "layer", "end"}));
    CHECK(TrackTemplateOf(s, s.libraries.front()) == DefaultTrackTemplate(s, s.libraries.front()));
    // Reordered: [LocomotionType, SurfaceMaterial, Layer, end] -> Lands-in > Walk > Concrete > Heel.
    s.libraries.front().track_levels = {"g:gLoco", "g:gSurf", "layer", "end"};
    CHECK(SameNames(LibPath(s, "heel"), {{"RAV Library", true}, {"Walk", true}, {"Concrete", true}, {"Heel", false}}));
    CHECK(SameNames(LibPath(s, "walkw"), {{"RAV Library", true}, {"Walk", true}, {"Wood", false}}));
    CHECK(LibPath(s, "heel")[1].mark == LibPath(s, "walkw")[1].mark);  // one Walk for both surfaces
    // Name join: [lib, SurfaceMaterial, end, LocomotionType, Layer], sep "_".
    s.libraries.front().track_levels = {"lib", "g:gSurf", "end", "g:gLoco", "layer"};
    CHECK(SameNames(LibPath(s, "heel"), {{"RAV Library", true}, {"Footstep", true}, {"Concrete", true}, {"Walk_Heel", false}}));
    CHECK(SameNames(LibPath(s, "rc1"), {{"RAV Library", true}, {"Footstep", true}, {"Concrete", true}, {"Run", false}}));
    CHECK(LibPath(s, "heel").back().mark != LibPath(s, "toe").back().mark);
    SetTrackSeparator(s, "-");
    CHECK(LibPath(s, "toe").back().name == "Walk-Toe");
    SetTrackSeparator(s, "_");
    // Custom: [FS Player, LocomotionType, end] -> Lands-in > FS Player > Walk, holding every Walk sound.
    s.libraries.front().track_levels = {"c:FS Player", "g:gLoco", "end"};
    CHECK(SameNames(LibPath(s, "heel"), {{"RAV Library", true}, {"FS Player", true}, {"Walk", false}}));
    CHECK(LibPath(s, "heel").back().mark == LibPath(s, "toe").back().mark);
    CHECK(LibPath(s, "heel").back().mark == LibPath(s, "walkw").back().mark);
    CHECK(LibPath(s, "rc1").back().name == "Run" && LibPath(s, "rc1").back().mark != LibPath(s, "heel").back().mark);
    // Missing group: skipped for every sound.
    s.libraries.front().track_levels = {"lib", "g:gGone", "g:gSurf", "end"};
    CHECK(SameNames(LibPath(s, "heel"), {{"RAV Library", true}, {"Footstep", true}, {"Concrete", false}}));
    // Nothing gives a value: 13-11's track (empty path); an unknown word is ignored, "end" added when missing.
    s.libraries.front().track_levels = {"end"};
    CHECK(LibPath(s, "heel").empty());
    s.libraries.front().track_levels = {"zzz", "g:gLoco"};
    CHECK(SameNames(LibPath(s, "heel"), {{"RAV Library", true}, {"Walk", false}}));
    // Child Lands in: Wood's sounds under "Wood FS", the whole template below it.
    s.libraries.front().track_levels.clear();
    FindNode(s.libraries.front(), "wloc")->lands_in = "Wood FS";
    CHECK(SameNames(LibPath(s, "walkw"), {{"Wood FS", true}, {"Footstep", true}, {"Wood", true}, {"Walk", false}}));
    CHECK(LibPath(s, "walkw")[1].mark != LibPath(s, "heel")[1].mark);  // another Lands in: another Footstep
    // Marks are uids (T4): renamed values keep them, the names follow.
    const std::vector<TrackStep> before = LibPath(s, "heel");
    SoundStore                   r = s;
    r.groups[0].values[0].name = "Stone";
    FindNode(r.libraries.front(), "heel")->name = "Heel2";
    const std::vector<TrackStep> after = LibPath(r, "heel");
    CHECK(after[2].name == "Stone" && after[4].name == "Heel2");
    for (size_t i = 0; i < before.size(); ++i) CHECK(before[i].mark == after[i].mark);
    CHECK(NameForMark(r, before[2].mark, NoTrack) == "Stone" && NameForMark(r, before[4].mark, NoTrack) == "Heel2");
    // The same group twice on a path: the n-th level takes the n-th step.
    SoundStore t;
    t.groups.push_back(Group("gM", "Mat", {{"a", "A"}, {"b", "B"}}));
    SoundLibrary two;
    two.uid = "two";
    two.name = "Two";
    two.nodes.push_back(Switch("m1", "", "gM", {{"a", "m2", {}}}));
    two.nodes.push_back(Switch("m2", "m1", "gM", {{"b", "x", {}}}));
    two.nodes.push_back(Node("x", "m2", SoundNodeType::Sound));
    t.libraries.push_back(two);
    CHECK(SameNames(LibPath(t, "x"), {{"RAV Library", true}, {"Two", true}, {"A", true}, {"B", false}}));
    // T1 (loop 1): a group nested twice on one path appears twice in the chips, and Populate uses them.
    CHECK((DefaultTrackTemplate(t, t.libraries.front()) == std::vector<std::string>{"lib", "g:gM", "g:gM", "end"}));
    CHECK(TemplateGroupRepeats(t.libraries.front()).at("gM") == 2);
    // ... and kept by an edit (adding a folder of one's own).
    CHECK(AddTemplateLevel(t, "two", "c:Y"));
    CHECK(SameNames(LibPath(t, "x"), {{"RAV Library", true}, {"Y", true}, {"Two", true}, {"A", true}, {"B", false}}));
    // Addable again while the template holds it fewer times than the tree nests it.
    CHECK(!AddTemplateLevel(t, "two", "g:gM"));
    CHECK(RemoveTemplateLevel(t, "two", 2));
    CHECK(CanAddTemplateLevel(t.libraries.front(), TrackTemplateOf(t, t.libraries.front()), "g:gM"));
    CHECK(AddTemplateLevel(t, "two", "g:gM"));
    CHECK((TrackTemplateOf(t, t.libraries.front()) == std::vector<std::string>{"c:Y", "lib", "g:gM", "g:gM", "end"}));
    CHECK(!CanAddTemplateLevel(t.libraries.front(), TrackTemplateOf(t, t.libraries.front()), "g:gM"));
    CHECK(!AddTemplateLevel(t, "two", "g:gM"));
    // A group no Switch uses any more may still be added once (its level is skipped).
    CHECK(CanAddTemplateLevel(t.libraries.front(), {"lib", "end"}, "g:gNone"));
    CHECK(!CanAddTemplateLevel(t.libraries.front(), {"lib", "g:gNone", "end"}, "g:gNone"));
    // A Blend under a Switch: "layer", never "blend".
    SoundStore b = BlendUnderPath();
    CHECK((DefaultTrackTemplate(b, b.libraries.front()) == std::vector<std::string>{"lib", "g:gLoco", "g:gFeet", "g:gMat", "layer", "end"}));
    SoundStore k;
    SoundLibrary kl;
    kl.uid = "k";
    kl.nodes.push_back(Named("bl", "", SoundNodeType::Blend, "Hit"));
    kl.nodes.push_back(Named("a", "bl", SoundNodeType::Sound, "A"));
    k.libraries.push_back(kl);
    // A Blend with no Switch above: "layer" only, its layers straight under the library folder (T1).
    CHECK((DefaultTrackTemplate(k, k.libraries.front()) == std::vector<std::string>{"lib", "layer", "end"}));
    CHECK(SameNames(LibPath(k, "a"), {{"RAV Library", true}, {"(unnamed)", true}, {"A", false}}));
    // No Switch, no Blend: the library track (T1, "library track").
    SoundStore   plain;
    SoundLibrary pl;
    pl.uid = "p";
    pl.name = "Ui";
    pl.nodes.push_back(Named("r", "", SoundNodeType::Random, "Clicks"));
    pl.nodes.push_back(Named("c1", "r", SoundNodeType::Sound, "c1"));
    plain.libraries.push_back(pl);
    CHECK((DefaultTrackTemplate(plain, plain.libraries.front()) == std::vector<std::string>{"lib", "end"}));
    CHECK(SameNames(LibPath(plain, "c1"), {{"RAV Library", true}, {"Ui", false}}));
    // Words.
    TemplateLevel l;
    CHECK(ParseTemplateLevel("g:x", &l) && l.kind == LevelKind::Group && l.arg == "x" && TemplateLevelWord(l) == "g:x");
    CHECK(ParseTemplateLevel("c:My folder", &l) && l.kind == LevelKind::Custom && TemplateLevelWord(l) == "c:My folder");
    CHECK(!ParseTemplateLevel("g:", &l) && !ParseTemplateLevel("c:", &l) && !ParseTemplateLevel("Lib", &l));
    CHECK((CleanTemplate({"end", "lib", "end", "nope"}) == std::vector<std::string>{"end", "lib"}));
}

// Loop 1: the chips are what Populate does. A library whose branches order groups differently and that mixes Blends
// with and without a Switch above: an edit that adds a folder of one's own on top moves no other level of any sound.
static void TestTemplateChipsArePaths()
{
    SoundStore s;
    s.groups.push_back(Group("gLoco", "Loco", {{"vWalk", "Walk"}}));
    s.groups.push_back(Group("gMat", "Mat", {{"vWood", "Wood"}}));
    SoundLibrary lib;
    lib.uid = "mx";
    lib.name = "Mix";
    lib.nodes.push_back(Named("top", "", SoundNodeType::Random, "Top"));
    // Branch 1: Mat > Loco > Blend [h1]; branch 2: Loco > Mat > s2; branch 3: Blend [h3] with no Switch.
    lib.nodes.push_back(Switch("m1", "top", "gMat", {{"vWood", "l1", {}}}));
    lib.nodes.push_back(Switch("l1", "m1", "gLoco", {{"vWalk", "b1", {}}}));
    lib.nodes.push_back(Named("b1", "l1", SoundNodeType::Blend, "B1"));
    lib.nodes.push_back(Named("h1", "b1", SoundNodeType::Sound, "H1"));
    lib.nodes.push_back(Switch("l2", "top", "gLoco", {{"vWalk", "m2", {}}}));
    lib.nodes.push_back(Switch("m2", "l2", "gMat", {{"vWood", "s2", {}}}));
    lib.nodes.push_back(Named("s2", "m2", SoundNodeType::Sound, "S2"));
    lib.nodes.push_back(Named("b3", "top", SoundNodeType::Blend, "B3"));
    lib.nodes.push_back(Named("h3", "b3", SoundNodeType::Sound, "H3"));
    s.libraries.push_back(lib);
    CHECK((TrackTemplateOf(s, s.libraries.front()) == std::vector<std::string>{"lib", "g:gMat", "g:gLoco", "layer", "end"}));
    // One row for the whole library: branch 2 follows it too (Wood > Walk).
    CHECK(SameNames(LibPath(s, "h1"), {{"RAV Library", true}, {"Mix", true}, {"Wood", true}, {"Walk", true}, {"H1", false}}));
    CHECK(SameNames(LibPath(s, "s2"), {{"RAV Library", true}, {"Mix", true}, {"Wood", true}, {"Walk", false}}));
    CHECK(SameNames(LibPath(s, "h3"), {{"RAV Library", true}, {"Mix", true}, {"H3", false}}));
    std::vector<std::vector<TrackStep>> before;
    for (const char* u : {"h1", "s2", "h3"}) before.push_back(LibPath(s, u));
    CHECK(AddTemplateLevel(s, "mx", "c:Top folder"));
    int i = 0;
    for (const char* u : {"h1", "s2", "h3"}) {
        const std::vector<TrackStep> after = LibPath(s, u);
        const std::vector<TrackStep>& b = before[static_cast<size_t>(i++)];
        CHECK(after.size() == b.size() + 1 && after[1].name == "Top folder");
        for (size_t k = 1; k < b.size() && k + 1 < after.size(); ++k) CHECK(after[k + 1].name == b[k].name);
    }
    // The preview's rows are those paths.
    const std::vector<StructureRow> rows = TemplateStructure(s, s.libraries.front());
    CHECK(!rows.empty() && rows[1].name == "Top folder" && rows[1].why == "your folder");
}

// Loop 1: odd shapes. "end" first: one joined track under Lands in; only "end" left: the usual track; a custom folder
// with spaces around its text; a RAV-owned group's label carries "RAV - " as its chip.
static void TestTemplateShapes()
{
    SoundStore s = Footstep();
    s.libraries.front().track_levels = {"end", "lib", "g:gSurf"};
    const std::vector<TrackStep> j = LibPath(s, "heel");
    CHECK(SameNames(j, {{"RAV Library", true}, {"Footstep_Concrete", false}}));
    if (j.size() == 2) {
        CHECK(NameForMark(s, j[1].mark, NoTrack) == "Footstep_Concrete");
        CHECK(MarkLevelLabel(s, j[1].mark) == "name: library + SurfaceMaterial");
    }
    CHECK(LibPath(s, "heel").back().mark == LibPath(s, "toe").back().mark);  // every Concrete sound shares it
    // Every level removed but "end": the usual track (empty path), "end" never removed.
    s.libraries.front().track_levels.clear();
    while (TrackTemplateOf(s, s.libraries.front()).size() > 1) CHECK(RemoveTemplateLevel(s, "fs", 0));
    CHECK((s.libraries.front().track_levels == std::vector<std::string>{"end"}));
    CHECK(!RemoveTemplateLevel(s, "fs", 0));
    CHECK(LibPath(s, "heel").empty() && TemplateStructure(s, s.libraries.front()).empty());
    // A custom folder with outer spaces (a hand-edited file): its text trimmed, the same track as the trimmed one.
    s.libraries.front().track_levels = {"c:  FS Player  ", "end"};
    const std::vector<TrackStep> c1 = LibPath(s, "heel");
    s.libraries.front().track_levels = {"c:FS Player", "end"};
    const std::vector<TrackStep> c2 = LibPath(s, "heel");
    CHECK(SameNames(c1, {{"RAV Library", true}, {"FS Player", false}}) && c1.size() == 2 && c2.size() == 2 &&
          c1[1].mark == c2[1].mark);
    if (c1.size() == 2) CHECK(NameForMark(s, c1[1].mark, NoTrack) == "FS Player");
    // A RAV-owned group: "RAV - <name>" in the preview, as on its chip.
    s.libraries.front().track_levels = {"g:gSurf", "end"};
    s.groups[0].rav_owned = true;
    const std::vector<TrackStep> g = LibPath(s, "heel");
    if (g.size() == 2) CHECK(MarkLevelLabel(s, g[1].mark) == "RAV - SurfaceMaterial");
}

static void TestTemplateEdits()
{
    SoundStore     s = Footstep();
    const auto&    stored = [&]() -> const std::vector<std::string>& { return s.libraries.front().track_levels; };
    const auto     shown = [&]() { return TrackTemplateOf(s, s.libraries.front()); };
    // Move: LocomotionType onto SurfaceMaterial takes its place.
    CHECK(MoveTemplateLevel(s, "fs", 2, 1));
    CHECK((stored() == std::vector<std::string>{"lib", "g:gLoco", "g:gSurf", "layer", "end"}));
    CHECK(!MoveTemplateLevel(s, "fs", 1, 1) && !MoveTemplateLevel(s, "fs", 0, 9) && !MoveTemplateLevel(s, "nolib", 0, 1));
    // Moving back to the tree order stores none.
    CHECK(MoveTemplateLevel(s, "fs", 1, 2) && stored().empty());
    // "end" moves too: the levels after it join.
    CHECK(MoveTemplateLevel(s, "fs", 4, 2));
    CHECK((shown() == std::vector<std::string>{"lib", "g:gSurf", "end", "g:gLoco", "layer"}));
    // Remove: any level but "end".
    CHECK(!RemoveTemplateLevel(s, "fs", 2) && !RemoveTemplateLevel(s, "fs", 9));
    CHECK(RemoveTemplateLevel(s, "fs", 0));
    CHECK((shown() == std::vector<std::string>{"g:gSurf", "end", "g:gLoco", "layer"}));
    // Add: a data level before "end" (refused twice), a custom folder first (refused empty).
    CHECK(AddTemplateLevel(s, "fs", "lib"));
    CHECK((shown() == std::vector<std::string>{"g:gSurf", "lib", "end", "g:gLoco", "layer"}));
    CHECK(!AddTemplateLevel(s, "fs", "lib") && !AddTemplateLevel(s, "fs", "layer") && !AddTemplateLevel(s, "fs", "end"));
    CHECK(!AddTemplateLevel(s, "fs", "c:   ") && !AddTemplateLevel(s, "fs", "c:") && !AddTemplateLevel(s, "fs", "zzz"));
    CHECK(AddTemplateLevel(s, "fs", "c:  FS Player "));
    CHECK((shown() == std::vector<std::string>{"c:FS Player", "g:gSurf", "lib", "end", "g:gLoco", "layer"}));
    CHECK(AddTemplateLevel(s, "fs", "blend"));
    CHECK(shown()[3] == "blend" && shown()[4] == "end");
    // Reset: back to the tree order (none stored); false once there.
    CHECK(ResetTrackTemplate(s, "fs") && stored().empty());
    CHECK(!ResetTrackTemplate(s, "fs"));
    // Round trip through the project's text (T3).
    CHECK(AddTemplateLevel(s, "fs", "c:A,b=c%d"));
    SoundStore back;
    CHECK(ParseSoundStore(SerializeSoundStore(s), &back));
    CHECK(back.libraries.front().track_levels == stored());
    CHECK(back.libraries.front().track_levels.front() == "c:A,b=c%d");
}

static void TestTemplateStructure()
{
    SoundStore                      s = Footstep();
    const std::vector<StructureRow> rows = TemplateStructure(s, s.libraries.front());
    // RAV Library > Footstep > Concrete > Walk > Heel, Toe; Run; Wood > Walk, Run.
    const char* want[] = {"RAV Library", "Footstep", "Concrete", "Walk", "Heel", "Toe", "Run", "Wood", "Walk", "Run"};
    const int   depth[] = {0, 1, 2, 3, 4, 4, 3, 2, 3, 3};
    CHECK(rows.size() == 10);
    for (size_t i = 0; i < rows.size() && i < 10; ++i) CHECK(rows[i].name == want[i] && rows[i].depth == depth[i]);
    if (rows.size() == 10) {
        CHECK(rows[0].folder && !rows[0].holds && rows[0].mark.empty() && rows[0].why == "Lands in");
        CHECK(rows[0].sounds.size() == 5);
        CHECK(rows[3].folder && !rows[3].holds && rows[3].sounds.size() == 2);
        CHECK(!rows[4].folder && rows[4].holds && rows[4].sounds == std::vector<std::string>{"heel"});
        CHECK(rows[6].holds && rows[6].sounds == std::vector<std::string>{"rc1"});
        CHECK(rows[1].why == "library" && rows[2].why == "SurfaceMaterial" && rows[4].why == "Layer");
    }
    // A template that gives nothing: no rows.
    s.libraries.front().track_levels = {"end"};
    CHECK(TemplateStructure(s, s.libraries.front()).empty());
    // A folder that also holds sounds: [lib, layer, end] -> Footstep holds the sounds outside the Blend.
    s.libraries.front().track_levels = {"lib", "layer", "end"};
    const std::vector<StructureRow> r2 = TemplateStructure(s, s.libraries.front());
    CHECK(r2.size() == 4 && r2[1].name == "Footstep" && r2[1].folder && r2[1].holds);
}

int main()
{
    TestCrossfadeButted();
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
    TestTemplateMatrix();
    TestTemplateEdits();
    TestTemplateStructure();
    TestTemplateChipsArePaths();
    TestTemplateShapes();
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("sound_tracks_plan: all passed\n");
    return 0;
}
