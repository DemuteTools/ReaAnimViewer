// SPDX-License-Identifier: MIT
//
// Host test of src/library_tree_nav.h (13-48: folds kept, Expand / Collapse all, the row a rule opens
// the Library on) and src/library_view_keys.h (the Library view's key claim, 13-7a review G-d).

#include "library_tree_nav.h"
#include "library_view_keys.h"

#include <cstdio>
#include <string>

#include "sound_record.h"

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            ++g_fail;                                                         \
        }                                                                     \
    } while (0)

static SoundNode Node(const std::string& uid, const std::string& parent, SoundNodeType t, const std::string& name)
{
    SoundNode n;
    n.uid = uid;
    n.parent = parent;
    n.type = t;
    n.name = name;
    return n;
}

// Groups Action {Grab, Drop} and Material {Wood, Metal, Stone}. Library L: root Switch "sa" on Action;
// Grab = Switch "sm" on Material (Wood = Random "rw" holding Sound "w1", Metal = Sound "m1", Stone = no
// sound), Drop = Blend "bd" holding Sound "d1".
static SoundStore Store()
{
    SoundStore s;
    SwitchGroup a;
    a.uid = "gact";
    a.name = "Action";
    a.values = {{"vgrab", "Grab", {}, {}}, {"vdrop", "Drop", {}, {}}};
    a.default_value = "vgrab";
    SwitchGroup m;
    m.uid = "gmat";
    m.name = "Material";
    m.values = {{"vwood", "Wood", {}, {}}, {"vmetal", "Metal", {}, {}}, {"vstone", "Stone", {}, {}}};
    m.default_value = "vwood";
    s.groups = {a, m};
    SoundLibrary l;
    l.uid = "L";
    l.name = "Hand";
    l.root = "sa";
    SoundNode sa = Node("sa", "", SoundNodeType::Switch, "Hand");
    sa.group_uid = "gact";
    sa.cases = {{"vgrab", "sm", {}}, {"vdrop", "bd", {}}};
    SoundNode sm = Node("sm", "sa", SoundNodeType::Switch, "Grab");
    sm.group_uid = "gmat";
    sm.cases = {{"vwood", "rw", {}}, {"vmetal", "m1", {}}};
    l.nodes = {sa,
               sm,
               Node("rw", "sm", SoundNodeType::Random, "Wood"),
               Node("w1", "rw", SoundNodeType::Sound, "W1"),
               Node("m1", "sm", SoundNodeType::Sound, "Metal"),
               Node("bd", "sa", SoundNodeType::Blend, "Drop"),
               Node("d1", "bd", SoundNodeType::Sound, "D1")};
    s.libraries.push_back(l);
    return s;
}

static ValueLayers Rule(std::vector<std::pair<std::string, std::string>> sets)
{
    ValueLayers v;
    for (const auto& p : sets) v.rule.push_back({p.first, p.second, {}});
    return v;
}

static void TestFoldsText()
{
    const SoundStore    s = Store();
    const SoundLibrary& l = s.libraries.front();
    // Only this library's containers are kept (a Sound, another library's uid, a uid with ',' dropped), sorted.
    const std::set<std::string> closed = {"sm", "rw", "w1", "other", "bd", "a,b"};
    CHECK(EncodeTreeFolds(l, closed) == "bd,rw,sm");
    CHECK(EncodeTreeFolds(l, {}).empty());
    CHECK(DecodeTreeFolds("bd,rw,sm") == (std::set<std::string>{"bd", "rw", "sm"}));
    CHECK(DecodeTreeFolds("").empty());
    CHECK(DecodeTreeFolds(",, bd ,\r\n") == (std::set<std::string>{"bd"}));
    // Round trip.
    CHECK(EncodeTreeFolds(l, DecodeTreeFolds(EncodeTreeFolds(l, closed))) == "bd,rw,sm");
    // The root folded is kept too.
    CHECK(EncodeTreeFolds(l, {"sa"}) == "sa");
}

static void TestFoldAll()
{
    const SoundStore    s = Store();
    const SoundLibrary& l = s.libraries.front();
    CHECK(CollapsedTreeFolds(l) == (std::set<std::string>{"sm", "rw", "bd"}));  // never the root
    std::set<std::string> closed = {"elsewhere"};
    CHECK(!TreeCanExpandAll(l, closed));
    CHECK(TreeCanCollapseAll(l, closed));
    SetTreeFoldsAll(l, true, &closed);
    CHECK(closed == (std::set<std::string>{"elsewhere", "sm", "rw", "bd"}));
    CHECK(TreeCanExpandAll(l, closed));
    CHECK(!TreeCanCollapseAll(l, closed));
    // A folded root: Collapse all opens it (its children show, folded).
    closed.insert("sa");
    CHECK(TreeCanCollapseAll(l, closed));
    SetTreeFoldsAll(l, true, &closed);
    CHECK(!closed.count("sa"));
    SetTreeFoldsAll(l, false, &closed);
    CHECK(closed == (std::set<std::string>{"elsewhere"}));  // other libraries untouched
    CHECK(!TreeCanExpandAll(l, closed));
}

static void TestFoldScopeButtons()
{
    // One foldable card (the node panel today): no buttons.
    CHECK(!FoldScopeButtonsFor(0, 0).shown);
    CHECK(!FoldScopeButtonsFor(1, 1).shown);
    const FoldScopeButtons all_open = FoldScopeButtonsFor(3, 3);
    CHECK(all_open.shown && !all_open.can_expand && all_open.can_collapse);
    const FoldScopeButtons all_shut = FoldScopeButtonsFor(2, 0);
    CHECK(all_shut.shown && all_shut.can_expand && !all_shut.can_collapse);
    const FoldScopeButtons mixed = FoldScopeButtonsFor(2, 1);
    CHECK(mixed.can_expand && mixed.can_collapse);
}

static void TestRevealRow()
{
    const SoundStore    s = Store();
    const SoundLibrary& l = s.libraries.front();
    // No value set: the defaults (Grab > Wood) down to the Random, on its value row.
    CHECK(RevealRowFor(s, l, {}) == (LibraryRow{"sm", "vwood"}));
    // Grab > Metal: the Sound on its value row.
    CHECK(RevealRowFor(s, l, Rule({{"gmat", "vmetal"}})) == (LibraryRow{"sm", "vmetal"}));
    // Drop: the Blend (not a Switch: the walk stops there).
    CHECK(RevealRowFor(s, l, Rule({{"gact", "vdrop"}})) == (LibraryRow{"sa", "vdrop"}));
    // Stone holds no sound: ResolveEvent plays the default's (Wood).
    CHECK(RevealRowFor(s, l, Rule({{"gmat", "vstone"}})) == (LibraryRow{"sm", "vwood"}));
    // The item layer counts too, under the rule's.
    ValueLayers v = Rule({{"gmat", "vmetal"}});
    v.item.push_back({"gact", "vdrop", {}});
    CHECK(RevealRowFor(s, l, v) == (LibraryRow{"sa", "vdrop"}));
    // A default value with no sound: that empty value row.
    SoundStore s2 = Store();
    s2.groups[1].default_value = "vstone";
    CHECK(RevealRowFor(s2, s2.libraries.front(), {}) == (LibraryRow{"sm", "vstone"}));
    // A Switch whose group is gone: the Switch's own row.
    SoundStore s3 = Store();
    s3.groups.pop_back();
    CHECK(RevealRowFor(s3, s3.libraries.front(), {}) == (LibraryRow{"sa", "vgrab"}));
    // A rule value no longer in the group, with an empty default: the Switch row, not a row that is gone.
    SoundStore s4 = Store();
    s4.groups[1].default_value = "vstone";
    CHECK(RevealRowFor(s4, s4.libraries.front(), Rule({{"gmat", "vgone"}})) == (LibraryRow{"sa", "vgrab"}));
    // A root that is not a Switch: the root row.
    SoundLibrary r;
    r.uid = "R";
    r.root = "x";
    r.nodes = {Node("x", "", SoundNodeType::Random, "X")};
    CHECK(RevealRowFor(s, r, {}) == (LibraryRow{"", "x"}));
}

static void TestKeyClaim()
{
    LibraryKeyState off;
    CHECK(!LibraryViewClaimsKey(off, 0x20, false, false, false));  // not focused: Space is REAPER's
    LibraryKeyState on;
    on.focused = true;
    CHECK(LibraryViewClaimsKey(on, 0x20, false, false, false));   // Space
    CHECK(LibraryViewClaimsKey(on, 0x2E, false, false, false));   // Delete
    CHECK(LibraryViewClaimsKey(on, 0x71, false, false, false));   // F2
    CHECK(LibraryViewClaimsKey(on, 0x1B, false, false, false));   // Esc
    CHECK(!LibraryViewClaimsKey(on, 0x20, false, false, true));   // never with Alt
    CHECK(LibraryViewClaimsKey(on, 'D', true, false, false));     // Ctrl+D
    CHECK(LibraryViewClaimsKey(on, 'B', true, false, false));
    CHECK(LibraryViewClaimsKey(on, 'X', true, false, false));
    CHECK(LibraryViewClaimsKey(on, 'V', true, false, false));
    CHECK(!LibraryViewClaimsKey(on, 'D', true, true, false));     // never Ctrl+Shift
    CHECK(!LibraryViewClaimsKey(on, 'Z', true, false, false));    // Ctrl+Z stays REAPER's undo
    CHECK(!LibraryViewClaimsKey(on, 'A', false, false, false));
    CHECK(!LibraryViewClaimsKey(on, 0x28, false, false, false));  // arrows: only when a list takes them
    on.media_takes_arrows = true;
    CHECK(LibraryViewClaimsKey(on, 0x28, false, false, false));
    CHECK(!LibraryViewClaimsKey(on, 0x25, false, false, false));  // the Media Explorer takes Up / Down only
    CHECK(!LibraryViewClaimsKey(on, 0x28, false, true, false));   // not with Shift
    on.media_takes_arrows = false;
    on.tree_takes_arrows = true;
    CHECK(LibraryViewClaimsKey(on, 0x25, false, false, false));
    CHECK(LibraryViewClaimsKey(on, 0x27, false, false, false));
    CHECK(!LibraryViewClaimsKey(on, 0x25, true, false, false));
}

int main()
{
    TestFoldsText();
    TestFoldAll();
    TestFoldScopeButtons();
    TestRevealRow();
    TestKeyClaim();
    if (g_fail) {
        std::printf("library_tree_nav_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("library_tree_nav_test: all passed\n");
    return 0;
}
