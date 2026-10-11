// SPDX-License-Identifier: MIT
//
// Host test of src/sound_switch.h (story 13-4): inheritance, default fallback, the tree walk
// (gains, offsets, variations, missing media), blocked libraries, path text and type conversion.

#include "sound_switch.h"

#include <cmath>
#include <cstdio>
#include <string>

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

static SoundNode Node(const std::string& uid, const std::string& parent, SoundNodeType t, const std::string& name,
                      double vol = 0.0, double off = 0.0)
{
    SoundNode n;
    n.uid = uid;
    n.parent = parent;
    n.type = t;
    n.name = name;
    n.volume_db = vol;
    n.offset_ms = off;
    return n;
}

static SoundStore Store()
{
    SoundStore s;
    SwitchGroup g;
    g.uid = "gmat";
    g.name = "Material";
    g.values = {{"vcon", "Concrete", {}}, {"vwood", "Wood", {}}, {"vgrass", "Grass", {}}};
    s.groups.push_back(g);
    SwitchGroup g2;
    g2.uid = "gsurf";
    g2.name = "Surface";
    g2.values = {{"swet", "Wet Concrete", {}}, {"sgrass", "Grass", {}}};
    s.groups.push_back(g2);
    SwitchGroup g3;
    g3.uid = "gsize";
    g3.name = "Size";
    g3.values = {{"zs", "Small", {}}, {"zl", "Large", {}}};
    s.groups.push_back(g3);
    return s;
}

static void Case(SoundNode& sw, const std::string& v, const std::string& c) { sw.cases.push_back({v, c, {}}); }

// Material switch: Concrete -> Blend(2 sounds), Grass -> sound sg; no case for Wood.
static SoundLibrary MatLib()
{
    SoundLibrary lib;
    lib.uid = "L";
    lib.name = "Steps";
    lib.root = "sw";
    SoundNode sw = Node("sw", "", SoundNodeType::Switch, "Mat");
    sw.group_uid = "gmat";
    Case(sw, "vcon", "bl");
    Case(sw, "vgrass", "sg");
    lib.nodes.push_back(sw);
    lib.nodes.push_back(Node("bl", "sw", SoundNodeType::Blend, "B"));
    lib.nodes.push_back(Node("s1", "bl", SoundNodeType::Sound, "Heel", 0, 0));
    lib.nodes.push_back(Node("s2", "bl", SoundNodeType::Sound, "Toe", 0, 40));
    lib.nodes.push_back(Node("sg", "sw", SoundNodeType::Sound, "GrassStep"));
    return lib;
}

static void TestInheritance()
{
    SoundNode sw = Node("sw", "", SoundNodeType::Switch, "Mat");
    sw.group_uid = "gmat";
    ValueLayers L;
    ValueFrom f = ValueFrom::Event;
    CHECK(EffectiveValue(sw, L, &f).empty() && f == ValueFrom::Default);
    L.item = {{"gmat", "vwood", {}}};
    CHECK(EffectiveValue(sw, L, &f) == "vwood" && f == ValueFrom::Item);
    L.rule = {{"gmat", "vcon", {}}};
    CHECK(EffectiveValue(sw, L, &f) == "vcon" && f == ValueFrom::Rule);
    L.event = {{"gmat", "", {}}};  // empty value inherits
    CHECK(EffectiveValue(sw, L, &f) == "vcon" && f == ValueFrom::Rule);
    L.event = {{"gother", "x", {}}, {"gmat", "vgrass", {}}};
    CHECK(EffectiveValue(sw, L, &f) == "vgrass" && f == ValueFrom::Event);
    CHECK(EffectiveValue(sw, L, nullptr) == "vgrass");
    sw.default_value = "vcon";
    ValueLayers none;
    CHECK(EffectiveValue(sw, none, &f) == "vcon" && f == ValueFrom::Default);

    SoundStore st = Store();
    SoundNode d = Node("x", "", SoundNodeType::Switch, "X");
    d.group_uid = "gmat";
    CHECK(DefaultValue(st, d) == "vcon");
    d.default_value = "vwood";
    CHECK(DefaultValue(st, d) == "vwood");
    d.default_value.clear();
    d.group_uid = "missing";
    Case(d, "vq", "c");
    CHECK(DefaultValue(st, d) == "vq");
}

static void TestDefaultFallbackAndPath()
{
    SoundStore st = Store();
    SoundLibrary lib = MatLib();
    ValueLayers L;
    L.item = {{"gmat", "vwood", {}}};
    Resolution r = ResolveEvent(st, lib, L, {});
    CHECK(!r.blocked);
    CHECK(r.sounds.size() == 2);
    CHECK(r.path_text == "Material Wood \xE2\x86\x92 Concrete (default) \xE2\x80\xBA Blend \xC3\x97" "2");
    CHECK(r.path.size() == 4 && r.path[0] == "sw" && r.path[1] == "bl");
    L.event = {{"gmat", "vgrass", {}}};
    r = ResolveEvent(st, lib, L, {});
    CHECK(r.sounds.size() == 1 && r.sounds[0].node == "sg");
    CHECK(r.path_text == "Material Grass \xE2\x80\xBA GrassStep");
    r = ResolveEvent(st, lib, {}, {});
    CHECK(r.path_text == "Material Concrete (default) \xE2\x80\xBA Blend \xC3\x97" "2");
    CHECK(r.sounds.size() == 2 && Near(r.sounds[1].offset_ms, 40));
    // A value deleted from the group but still set on a layer: placeholder, never the raw uid.
    L = ValueLayers();
    L.item = {{"gmat", "vgone", {}}};
    r = ResolveEvent(st, lib, L, {});
    CHECK(r.path_text == "Material (deleted value) \xE2\x86\x92 Concrete (default) \xE2\x80\xBA Blend \xC3\x97" "2");
}

static void TestNested()
{
    SoundStore st = Store();
    SoundLibrary lib;
    lib.root = "sw";
    SoundNode sw = Node("sw", "", SoundNodeType::Switch, "Mat", 0);
    sw.group_uid = "gmat";
    Case(sw, "vcon", "rn");
    lib.nodes.push_back(sw);
    lib.nodes.push_back(Node("rn", "sw", SoundNodeType::Random, "Rnd", -3));
    lib.nodes.push_back(Node("bl", "rn", SoundNodeType::Blend, "Bl", -2));
    lib.nodes.push_back(Node("a", "bl", SoundNodeType::Sound, "A", -1, 0));
    SoundNode b = Node("b", "bl", SoundNodeType::Sound, "B", -1, 40);
    b.sound.frames.resize(2);
    b.sound.frames[1].db = 1.5;
    lib.nodes.push_back(b);
    ResolveHooks h;
    h.random_child = [](const SoundNode&) { return 0; };
    h.variation = [](const SoundNode& n) { return n.uid == "b" ? 1 : 0; };
    Resolution r = ResolveEvent(st, lib, {}, h);
    CHECK(!r.blocked && r.sounds.size() == 2);
    CHECK(Near(r.sounds[0].gain_db, -6) && Near(r.sounds[1].gain_db, -4.5));
    CHECK(Near(r.sounds[0].offset_ms, 0) && Near(r.sounds[1].offset_ms, 40));
    CHECK(r.sounds[1].variation == 1);
    CHECK(r.path_text == "Material Concrete (default) \xE2\x80\xBA Random Bl \xE2\x80\xBA Blend \xC3\x97" "2");

    // Missing: one Blend part missing, the variation hook not called for it.
    int calls = 0;
    h.variation = [&](const SoundNode&) { ++calls; return 0; };
    h.is_missing = [](const SoundNode& n) { return n.uid == "a"; };
    r = ResolveEvent(st, lib, {}, h);
    CHECK(!r.blocked && r.sounds.size() == 2);
    CHECK(r.sounds[0].missing && r.sounds[0].variation == -1);
    CHECK(!r.sounds[1].missing && r.sounds[1].variation == 0 && calls == 1);

    // Random index out of range / -1: nothing, not blocked.
    h.random_child = [](const SoundNode&) { return 5; };
    r = ResolveEvent(st, lib, {}, h);
    CHECK(!r.blocked && r.sounds.empty());
    CHECK(r.path_text == "Material Concrete (default) \xE2\x80\xBA Random");
    h.random_child = [](const SoundNode&) { return -1; };
    r = ResolveEvent(st, lib, {}, h);
    CHECK(!r.blocked && r.sounds.empty());

    // A variation index out of range adds no frame gain.
    h.random_child = [](const SoundNode&) { return 0; };
    h.is_missing = nullptr;
    h.variation = [](const SoundNode&) { return 7; };
    r = ResolveEvent(st, lib, {}, h);
    CHECK(r.sounds.size() == 2 && Near(r.sounds[1].gain_db, -6));

    // Offsets sum across nested Blends; an offset under a Random or Switch is ignored.
    SoundLibrary nb;
    nb.root = "o";
    nb.nodes.push_back(Node("o", "", SoundNodeType::Blend, "O"));
    nb.nodes.push_back(Node("i", "o", SoundNodeType::Blend, "I", 0, 100));
    nb.nodes.push_back(Node("x", "i", SoundNodeType::Sound, "X", 0, 25));
    r = ResolveEvent(st, nb, {}, {});
    CHECK(r.sounds.size() == 1 && Near(r.sounds[0].offset_ms, 125));
    SoundLibrary ro;
    ro.root = "r";
    ro.nodes.push_back(Node("r", "", SoundNodeType::Random, "R"));
    ro.nodes.push_back(Node("x", "r", SoundNodeType::Sound, "X", 0, 50));
    r = ResolveEvent(st, ro, {}, {});
    CHECK(r.sounds.size() == 1 && Near(r.sounds[0].offset_ms, 0));
    SoundLibrary so;
    so.root = "w";
    SoundNode w = Node("w", "", SoundNodeType::Switch, "W");
    w.group_uid = "gmat";
    Case(w, "vcon", "x");
    so.nodes.push_back(w);
    so.nodes.push_back(Node("x", "w", SoundNodeType::Sound, "X", 0, 50));
    r = ResolveEvent(st, so, {}, {});
    CHECK(r.sounds.size() == 1 && Near(r.sounds[0].offset_ms, 0));
}

static void TestRootsAndNullHooks()
{
    SoundStore st = Store();
    // Sound as root.
    SoundLibrary s;
    s.root = "x";
    s.nodes.push_back(Node("x", "", SoundNodeType::Sound, "Solo", -2));
    Resolution r = ResolveEvent(st, s, {}, {});
    CHECK(r.sounds.size() == 1 && Near(r.sounds[0].gain_db, -2) && r.sounds[0].variation == -1 && !r.sounds[0].missing);
    CHECK(r.path_text == "Solo");
    // Random as root, null hooks -> child 0.
    SoundLibrary rl;
    rl.root = "r";
    rl.nodes.push_back(Node("r", "", SoundNodeType::Random, "R"));
    rl.nodes.push_back(Node("c0", "r", SoundNodeType::Sound, "First"));
    rl.nodes.push_back(Node("c1", "r", SoundNodeType::Sound, "Second"));
    r = ResolveEvent(st, rl, {}, {});
    CHECK(r.sounds.size() == 1 && r.sounds[0].node == "c0" && r.path_text == "Random First");
    ResolveHooks h;
    h.random_child = [](const SoundNode&) { return 1; };
    r = ResolveEvent(st, rl, {}, h);
    CHECK(r.sounds.size() == 1 && r.sounds[0].node == "c1");
    // Blend as root.
    SoundLibrary bl;
    bl.root = "b";
    bl.nodes.push_back(Node("b", "", SoundNodeType::Blend, "B"));
    bl.nodes.push_back(Node("c0", "b", SoundNodeType::Sound, "A", 0, 10));
    bl.nodes.push_back(Node("c1", "b", SoundNodeType::Sound, "B", 0, 20));
    bl.nodes.push_back(Node("c2", "b", SoundNodeType::Sound, "C", 0, 30));
    r = ResolveEvent(st, bl, {}, {});
    CHECK(r.sounds.size() == 3 && Near(r.sounds[2].offset_ms, 30) && r.path_text == "Blend \xC3\x97" "3");
    // Switch inside Switch.
    SoundLibrary ss;
    ss.root = "o";
    SoundNode o = Node("o", "", SoundNodeType::Switch, "O");
    o.group_uid = "gmat";
    Case(o, "vcon", "i");
    ss.nodes.push_back(o);
    SoundNode in = Node("i", "o", SoundNodeType::Switch, "I");
    in.group_uid = "gsize";
    Case(in, "zs", "small");
    Case(in, "zl", "large");
    ss.nodes.push_back(in);
    ss.nodes.push_back(Node("small", "i", SoundNodeType::Sound, "Small step"));
    ss.nodes.push_back(Node("large", "i", SoundNodeType::Sound, "Large step"));
    ValueLayers L;
    L.rule = {{"gsize", "zl", {}}};
    r = ResolveEvent(st, ss, L, {});
    CHECK(r.sounds.size() == 1 && r.sounds[0].node == "large");
    CHECK(r.path_text == "Material Concrete (default) \xE2\x80\xBA Size Large \xE2\x80\xBA Large step");
}

static void TestUnassigned()
{
    SoundStore st = Store();
    SoundLibrary lib = MatLib();
    lib.nodes[0].unassigned.push_back("sg");
    ValueLayers L;
    L.event = {{"gmat", "vgrass", {}}};
    Resolution r = ResolveEvent(st, lib, L, {});
    CHECK(!r.blocked);
    for (const ResolvedSound& s : r.sounds) CHECK(s.node != "sg");
    CHECK(r.sounds.size() == 2);  // falls back to the default
}

static void TestBlocked()
{
    SoundStore st = Store();
    // Default value with no content.
    SoundLibrary lib = MatLib();
    lib.nodes[0].default_value = "vwood";
    Resolution r = ResolveEvent(st, lib, {}, {});
    CHECK(r.blocked && r.sounds.empty() && !r.reason.empty());
    LibraryCheck c = CheckLibrary(st, lib);
    CHECK(c.blocked && c.nodes.size() == 1 && c.nodes[0] == "sw");
    CHECK(c.reason == "Mat: default value has no sound");
    // Empty Random / Blend.
    SoundLibrary e;
    e.root = "r";
    e.nodes.push_back(Node("r", "", SoundNodeType::Random, "Empty R"));
    c = CheckLibrary(st, e);
    CHECK(c.blocked && c.reason.find("Empty R") != std::string::npos);
    CHECK(ResolveEvent(st, e, {}, {}).blocked);
    e.nodes[0].type = SoundNodeType::Blend;
    CHECK(CheckLibrary(e).blocked);
    // Empty Blend deeper inside the tree.
    lib = MatLib();
    lib.nodes.push_back(Node("eb", "sg", SoundNodeType::Blend, "Deep"));
    lib.nodes[4].type = SoundNodeType::Blend;  // sg becomes a Blend holding only eb (empty)
    c = CheckLibrary(st, lib);
    CHECK(c.blocked && c.nodes.size() == 1 && c.nodes[0] == "eb");
    // Unassigned subtrees are not reachable.
    lib.nodes[0].unassigned.push_back("sg");
    CHECK(!CheckLibrary(st, lib).blocked);
    // No root.
    SoundLibrary nr;
    CHECK(CheckLibrary(st, nr).blocked && ResolveEvent(st, nr, {}, {}).blocked);
    nr.root = "nope";
    CHECK(CheckLibrary(nr).blocked);
    // Cycle (malformed store).
    SoundLibrary cy;
    cy.root = "a";
    cy.nodes.push_back(Node("a", "b", SoundNodeType::Random, "A"));
    cy.nodes.push_back(Node("b", "a", SoundNodeType::Random, "B"));
    c = CheckLibrary(st, cy);
    CHECK(c.blocked && c.reason == "A: cycle");
    CHECK(ResolveEvent(st, cy, {}, {}).blocked);
    // Missing / empty group (2-argument form only).
    lib = MatLib();
    lib.nodes[0].group_uid = "missing";
    c = CheckLibrary(st, lib);
    CHECK(c.blocked && c.reason == "Mat: Switch Group not found");
    CHECK(ResolveEvent(st, lib, {}, {}).blocked);
    CHECK(!CheckLibrary(lib).blocked);  // 1-arg: default "" -> first case (vcon -> bl)
    SoundStore st2 = st;
    st2.groups[0].values.clear();
    lib = MatLib();
    c = CheckLibrary(st2, lib);
    CHECK(c.blocked && c.reason == "Mat: Switch Group has no value");
    CHECK(ResolveEvent(st2, lib, {}, {}).blocked);
    // 1-arg form: an explicit default with no case blocks.
    lib = MatLib();
    lib.nodes[0].default_value = "vwood";
    CHECK(CheckLibrary(lib).blocked);
    // Several blocking nodes are all listed.
    SoundLibrary two;
    two.root = "b";
    two.nodes.push_back(Node("b", "", SoundNodeType::Blend, "B"));
    two.nodes.push_back(Node("e1", "b", SoundNodeType::Random, "E1"));
    two.nodes.push_back(Node("e2", "b", SoundNodeType::Blend, "E2"));
    c = CheckLibrary(st, two);
    CHECK(c.blocked && c.nodes.size() == 2 && c.nodes[0] == "e1" && c.nodes[1] == "e2");
    // Healthy library.
    CHECK(!CheckLibrary(st, MatLib()).blocked);
    // A case child whose parent is not the Switch is not content.
    lib = MatLib();
    lib.nodes[1].parent = "elsewhere";
    CHECK(CheckLibrary(st, lib).blocked);
}

static void TestNoForeignUid()
{
    SoundStore st = Store();
    SoundLibrary lib = MatLib();
    lib.nodes[0].cases.push_back({"vwood", "foreign", {}});
    ValueLayers L;
    L.item = {{"gmat", "vwood", {}}};
    Resolution r = ResolveEvent(st, lib, L, {});
    CHECK(!r.blocked);
    for (const ResolvedSound& s : r.sounds) CHECK(s.node == "s1" || s.node == "s2");
}

static void TestConvert()
{
    SoundStore st = Store();
    SoundLibrary lib;
    lib.root = "r";
    lib.nodes.push_back(Node("r", "", SoundNodeType::Random, "R"));
    lib.nodes.push_back(Node("a", "r", SoundNodeType::Sound, "wet_concrete"));
    lib.nodes.push_back(Node("b", "r", SoundNodeType::Sound, "Grass"));
    lib.nodes.push_back(Node("c", "r", SoundNodeType::Sound, "Foo"));
    CHECK(!ConvertNodeType(lib, "r", SoundNodeType::Switch, "unknown", st));
    CHECK(!ConvertNodeType(lib, "r", SoundNodeType::Sound, "", st));
    CHECK(!ConvertNodeType(lib, "a", SoundNodeType::Blend, "", st));
    CHECK(!ConvertNodeType(lib, "zz", SoundNodeType::Blend, "", st));
    CHECK(!ConvertNodeType(lib, "r", SoundNodeType::Random, "", st));
    CHECK(ConvertNodeType(lib, "r", SoundNodeType::Blend, "", st) && lib.nodes[0].type == SoundNodeType::Blend);
    CHECK(ConvertNodeType(lib, "r", SoundNodeType::Random, "", st) && lib.nodes[0].type == SoundNodeType::Random);

    CHECK(ConvertNodeType(lib, "r", SoundNodeType::Switch, "gsurf", st));
    const SoundNode& sw = lib.nodes[0];
    CHECK(sw.type == SoundNodeType::Switch && sw.group_uid == "gsurf");
    CHECK(sw.cases.size() == 2);
    CHECK(sw.cases[0].value_uid == "swet" && sw.cases[0].child_uid == "a");
    CHECK(sw.cases[1].value_uid == "sgrass" && sw.cases[1].child_uid == "b");
    CHECK(sw.unassigned.size() == 1 && sw.unassigned[0] == "c");
    CHECK(lib.nodes[1].uid == "a" && lib.nodes[2].uid == "b" && lib.nodes[3].uid == "c");
    CHECK(!ConvertNodeType(lib, "r", SoundNodeType::Switch, "gsurf", st));  // same group
    CHECK(sw.default_value == "swet");  // decision 2026-10-12: first group value given a child
    {
        // Only the group's second value gets a child: it is the default (group order, not child order).
        SoundLibrary l2;
        l2.root = "r";
        l2.nodes.push_back(Node("r", "", SoundNodeType::Blend, "B"));
        l2.nodes.push_back(Node("x", "r", SoundNodeType::Sound, "Foo"));
        l2.nodes.push_back(Node("y", "r", SoundNodeType::Sound, "grass"));
        CHECK(ConvertNodeType(l2, "r", SoundNodeType::Switch, "gsurf", st));
        CHECK(l2.nodes[0].default_value == "sgrass");
        // No value gets a child: no default.
        SoundLibrary l3;
        l3.root = "r";
        l3.nodes.push_back(Node("r", "", SoundNodeType::Random, "R"));
        l3.nodes.push_back(Node("x", "r", SoundNodeType::Sound, "Foo"));
        CHECK(ConvertNodeType(l3, "r", SoundNodeType::Switch, "gsurf", st));
        CHECK(l3.nodes[0].default_value.empty() && l3.nodes[0].unassigned.size() == 1);
    }

    // Switch -> Switch by name: Surface{Wet Concrete, Grass} -> Material{Concrete, Wood, Grass}.
    lib.nodes[0].default_value = "sgrass";
    CHECK(ConvertNodeType(lib, "r", SoundNodeType::Switch, "gmat", st));
    CHECK(lib.nodes[0].group_uid == "gmat");
    CHECK(lib.nodes[0].cases.size() == 1 && lib.nodes[0].cases[0].value_uid == "vgrass" &&
          lib.nodes[0].cases[0].child_uid == "b");
    CHECK(lib.nodes[0].unassigned.size() == 2 && lib.nodes[0].unassigned[0] == "c" &&
          lib.nodes[0].unassigned[1] == "a");
    CHECK(lib.nodes[0].default_value == "vgrass");

    // Switch -> Switch: preview maps by name; two old cases on one new value: the second child
    // goes to Unassigned; a child dropped from one case but kept by another is not Unassigned;
    // an implicit default (the old group's first value) maps by name.
    {
        SoundStore s2 = Store();
        SwitchGroup dupg;
        dupg.uid = "gdup";
        dupg.name = "Dup";
        dupg.values = {{"d1", "grass", {}}, {"d2", "GRASS", {}}, {"d3", "Wood", {}}};
        s2.groups.push_back(dupg);
        SwitchGroup tg;
        tg.uid = "gt";
        tg.name = "T";
        tg.values = {{"tgrass", "Grass", {}}, {"twood", "Wood", {}}};
        s2.groups.push_back(tg);
        SoundLibrary dl;
        dl.root = "s";
        SoundNode s = Node("s", "", SoundNodeType::Switch, "S");
        s.group_uid = "gdup";
        Case(s, "d1", "k1");
        Case(s, "d2", "k2");
        Case(s, "d3", "k3");
        s.preview_value = "d3";
        dl.nodes.push_back(s);
        dl.nodes.push_back(Node("k1", "s", SoundNodeType::Sound, "K1"));
        dl.nodes.push_back(Node("k2", "s", SoundNodeType::Sound, "K2"));
        dl.nodes.push_back(Node("k3", "s", SoundNodeType::Sound, "K3"));
        CHECK(ConvertNodeType(dl, "s", SoundNodeType::Switch, "gt", s2));
        const SoundNode& t = dl.nodes[0];
        CHECK(t.cases.size() == 2 && t.cases[0].value_uid == "tgrass" && t.cases[0].child_uid == "k1" &&
              t.cases[1].value_uid == "twood" && t.cases[1].child_uid == "k3");
        CHECK(t.unassigned.size() == 1 && t.unassigned[0] == "k2");
        CHECK(t.preview_value == "twood");
        CHECK(t.default_value == "tgrass");  // implicit default d1 "grass" -> Grass

        // Same child on an unmatched case then a matched one: cased, not Unassigned.
        SoundLibrary sl;
        sl.root = "s";
        SoundNode u = Node("s", "", SoundNodeType::Switch, "S");
        u.group_uid = "gmat";
        Case(u, "vcon", "k");
        Case(u, "vgrass", "k");
        sl.nodes.push_back(u);
        sl.nodes.push_back(Node("k", "s", SoundNodeType::Sound, "K"));
        CHECK(ConvertNodeType(sl, "s", SoundNodeType::Switch, "gt", s2));
        CHECK(sl.nodes[0].cases.size() == 1 && sl.nodes[0].cases[0].child_uid == "k" && sl.nodes[0].unassigned.empty());
    }

    // Switch -> Switch when the old group is gone: every case child goes to Unassigned.
    {
        SoundLibrary gl;
        gl.root = "s";
        SoundNode s = Node("s", "", SoundNodeType::Switch, "S");
        s.group_uid = "gone";
        Case(s, "o1", "k1");
        Case(s, "o2", "k2");
        s.default_value = "o1";
        s.preview_value = "o2";
        gl.nodes.push_back(s);
        gl.nodes.push_back(Node("k1", "s", SoundNodeType::Sound, "Concrete"));
        gl.nodes.push_back(Node("k2", "s", SoundNodeType::Sound, "Wood"));
        CHECK(ConvertNodeType(gl, "s", SoundNodeType::Switch, "gmat", st));
        const SoundNode& t = gl.nodes[0];
        CHECK(t.cases.empty() && t.default_value.empty() && t.preview_value.empty());
        CHECK(t.unassigned.size() == 2 && t.unassigned[0] == "k1" && t.unassigned[1] == "k2");
        // Switch -> Blend clears the Switch fields and keeps the children.
        CHECK(ConvertNodeType(gl, "s", SoundNodeType::Blend, "", st));
        CHECK(t.type == SoundNodeType::Blend && t.group_uid.empty() && t.cases.empty() && t.unassigned.empty() &&
              t.default_value.empty() && t.preview_value.empty());
        CHECK(gl.nodes[1].parent == "s" && gl.nodes[2].parent == "s");
    }
    // Random -> Switch: two children with the same key, only the first takes the value;
    // Blend -> Random keeps the children's offset_ms.
    {
        SoundLibrary kl;
        kl.root = "r";
        kl.nodes.push_back(Node("r", "", SoundNodeType::Blend, "R"));
        kl.nodes.push_back(Node("a", "r", SoundNodeType::Sound, "Grass", 0, 30));
        kl.nodes.push_back(Node("b", "r", SoundNodeType::Sound, "grass"));
        CHECK(ConvertNodeType(kl, "r", SoundNodeType::Random, "", st));
        CHECK(Near(kl.nodes[1].offset_ms, 30));
        CHECK(ConvertNodeType(kl, "r", SoundNodeType::Switch, "gsurf", st));
        const SoundNode& t = kl.nodes[0];
        CHECK(t.cases.size() == 1 && t.cases[0].value_uid == "sgrass" && t.cases[0].child_uid == "a");
        CHECK(t.unassigned.size() == 1 && t.unassigned[0] == "b");
        CHECK(Near(kl.nodes[1].offset_ms, 30));
    }

    // Switch -> Random keeps every child and clears the Switch fields.
    CHECK(ConvertNodeType(lib, "r", SoundNodeType::Random, "", st));
    const SoundNode& rn = lib.nodes[0];
    CHECK(rn.type == SoundNodeType::Random && rn.group_uid.empty() && rn.cases.empty() && rn.unassigned.empty() &&
          rn.default_value.empty() && rn.preview_value.empty());
    for (size_t i = 1; i < lib.nodes.size(); ++i) CHECK(lib.nodes[i].parent == "r");

    CHECK(SwitchNameKey("Wet_Concrete-x Y") == "wetconcretexy");
    CHECK(SwitchNameKey("\xC3\x89t\xC3\xA9") == "\xC3\x89t\xC3\xA9");
}

int main()
{
    TestInheritance();
    TestDefaultFallbackAndPath();
    TestNested();
    TestRootsAndNullHooks();
    TestUnassigned();
    TestBlocked();
    TestNoForeignUid();
    TestConvert();
    if (g_fail) {
        std::printf("sound_switch_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("sound_switch_test: all passed\n");
    return 0;
}
