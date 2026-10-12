// SPDX-License-Identifier: MIT
//
// Story 13-8, Antho 2026-10-12 -- src/sound_auto_assign.h (pure): each auto marker type's sound
// (the key, the store's entries and their text, the row's rule edits, what a row shows, Detect's
// fill).

#include "sound_auto_assign.h"

#include <cstdio>
#include <string>
#include <vector>

#include "sound_record.h"

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

static Block Auto(AutoKind k, char side, const std::string& rid)
{
    Block b = MakeAutoBlock(k, side, kAutoDefaultSens, 0.0);
    b.rid = rid;
    return b;
}

static Block Rule(const std::string& rid)
{
    Block b;
    b.marker = "Rule";
    b.rid = rid;
    return b;
}

static void TestKeys()
{
    CHECK(AutoSoundKey(AutoKind::Step, 'L') == "step:L");
    CHECK(AutoSoundKey(AutoKind::Heel, 'R') == "heel:R");
    // One marker, one key: both hand pivot kinds, Each part / Longest part.
    CHECK(AutoSoundKey(AutoKind::HandPivotAny, 'L') == AutoSoundKey(AutoKind::HandPivot, 'L'));
    CHECK(AutoSoundKey(AutoKind::SlideHeelLongest, 'R') == "slide_heel:R");
    CHECK(AutoSoundKey(AutoKind::PivotToeLongest, 'L') == AutoSoundKey(AutoKind::PivotToe, 'L'));
    // Combined and separate are different markers.
    CHECK(AutoSoundKey(AutoKind::Step, 'L') != AutoSoundKey(AutoKind::Heel, 'L'));
    CHECK(AutoSoundKey(AutoKind::Slide, 'L') != AutoSoundKey(AutoKind::SlideHeel, 'L'));
    CHECK(AutoSoundKey(AutoKind::Step, 'X').empty());
    // Every kind and side has a distinct key except the shared ones.
    for (int k = 0; k < kAutoKindCount; ++k)
        for (char s : {'L', 'R'}) {
            const Block b = MakeAutoBlock(static_cast<AutoKind>(k), s, 50.0, 0.0);
            CHECK(AutoSoundKeyOf(b) == AutoSoundKey(static_cast<AutoKind>(k), s));
            CHECK(!AutoSoundKeyOf(b).empty());
        }
    CHECK(AutoSoundKeyOf(Rule("r1")).empty());
    Block unknown = Auto(AutoKind::Step, 'L', "x");
    unknown.auto_type = "future_kind";
    CHECK(AutoSoundKeyOf(unknown).empty());
}

static void TestStoreEdits()
{
    SoundStore s;
    CHECK(!SetAutoSoundLibrary(s, {"step:L"}, ""));  // nothing to clear
    CHECK(s.auto_sounds.empty());
    CHECK(SetAutoSoundLibrary(s, {"step:L", "step:R"}, "LIB1"));
    CHECK(s.auto_sounds.size() == 2 && FindAutoSound(s, "step:R") && FindAutoSound(s, "step:R")->library == "LIB1");
    CHECK(!SetAutoSoundLibrary(s, {"step:L", "step:R"}, "LIB1"));  // unchanged
    CHECK(!HasAutoSounds(s));  // LIB1 is not in the store
    s.libraries.push_back(SoundLibrary{});
    s.libraries.back().uid = "LIB1";
    CHECK(HasAutoSounds(s));
    s.libraries.clear();
    CHECK(SetAutoSoundValue(s, {"step:L"}, "G1", "V1"));
    CHECK(FindAutoSound(s, "step:L")->values.size() == 1);
    CHECK(SetAutoSoundValue(s, {"step:L"}, "G1", "V2"));
    CHECK(FindAutoSound(s, "step:L")->values[0].value_uid == "V2");
    CHECK(!SetAutoSoundValue(s, {"step:L"}, "", "V2"));
    // Cleared: step:R has nothing left and goes; step:L keeps its value.
    CHECK(SetAutoSoundLibrary(s, {"step:L", "step:R"}, ""));
    CHECK(s.auto_sounds.size() == 1 && s.auto_sounds[0].key == "step:L" && s.auto_sounds[0].library.empty());
    CHECK(!HasAutoSounds(s));
    CHECK(SetAutoSoundValue(s, {"step:L"}, "G1", ""));
    CHECK(s.auto_sounds.empty());
    CHECK(!SetAutoSoundValue(s, {"lift:L"}, "G1", ""));
    CHECK(FindAutoSound(s, "") == nullptr);
}

static void TestText()
{
    // Round trip: the auto lines right after the header, their values, unknown fields / lines kept.
    SoundStore s;
    SwitchGroup g;
    g.uid = "G1";
    g.name = "Surface";
    g.values.push_back({"V1", "Grass", {}});
    s.groups.push_back(g);
    SetAutoSoundLibrary(s, {"step:L"}, "LIB 1");
    SetAutoSoundValue(s, {"step:L"}, "G1", "V1");
    s.auto_sounds[0].kept.push_back("future=1");
    s.auto_sounds[0].kept_lines.push_back("aextra x=2");
    const std::string text = SerializeSoundStore(s);
    CHECK(text.find("RAVSOUNDS 1\nauto key=step:L lib=LIB%201 future=1\navalue group=G1 value=V1\naextra x=2\ngroup ") == 0);
    SoundStore r;
    CHECK(ParseSoundStore(text, &r));
    CHECK(r.auto_sounds.size() == 1);
    const AutoSoundAssign& a = r.auto_sounds[0];
    CHECK(a.key == "step:L" && a.library == "LIB 1" && a.values.size() == 1 && a.values[0].group_uid == "G1" &&
          a.values[0].value_uid == "V1" && a.kept.size() == 1 && a.kept_lines.size() == 1);
    CHECK(r.groups.size() == 1 && r.groups[0].values.size() == 1 && r.kept_lines.empty());
    CHECK(SerializeSoundStore(r) == text);

    // A store of only auto lines is not empty (it is saved).
    SoundStore only;
    SetAutoSoundLibrary(only, {"grab:R"}, "L");
    CHECK(!SoundStoreEmpty(only));
    CHECK(SerializeSoundStore(only) == "RAVSOUNDS 1\nauto key=grab:R lib=L\n");

    // Backward: a store written before (no auto line) reads with none and writes back the same.
    const std::string old = "RAVSOUNDS 1\ngroup uid=G1 name=Surface\nvalue uid=V1 name=Grass\n"
                            "library uid=L1 name=Feet root=N1\nnode uid=N1 type=random\n";
    SoundStore o;
    CHECK(ParseSoundStore(old, &o));
    CHECK(o.auto_sounds.empty() && o.libraries.size() == 1);
    CHECK(SerializeSoundStore(o) == old);

    // A version without auto lines keeps them as the header's unknown lines, written back right after
    // the header: read by this version again, they are auto lines (the same text).
    SoundStore legacy;
    legacy.kept_lines = {"auto key=step:L lib=L1", "avalue group=G1 value=V1"};
    legacy.groups.push_back(g);
    SoundStore back;
    CHECK(ParseSoundStore(SerializeSoundStore(legacy), &back));
    CHECK(back.auto_sounds.size() == 1 && back.auto_sounds[0].values.size() == 1 && back.kept_lines.empty());
    CHECK(SerializeSoundStore(back) == SerializeSoundStore(legacy));

    // An avalue with no auto line before it is kept as an unknown line.
    SoundStore stray;
    CHECK(ParseSoundStore("RAVSOUNDS 1\navalue group=G1 value=V1\n", &stray));
    CHECK(stray.auto_sounds.empty() && stray.kept_lines.size() == 1);
    // A group after an auto line: its values are the group's, not the auto's.
    SoundStore order;
    CHECK(ParseSoundStore("RAVSOUNDS 1\nauto key=lift:L lib=X\ngroup uid=G1 name=S\nvalue uid=V1 name=A\n"
                          "avalue group=G1 value=V1\n",
                          &order));
    CHECK(order.auto_sounds.size() == 1 && order.auto_sounds[0].values.empty() && order.groups[0].values.size() == 1 &&
          order.groups[0].kept_lines.size() == 1);
}

static void TestRulesAndShown()
{
    std::vector<Block> blocks = {Rule("r0"), Auto(AutoKind::Step, 'L', "a1"), Auto(AutoKind::Step, 'R', "a2"),
                                 Auto(AutoKind::Lift, 'L', "")};
    CHECK((AutoKeyRids(blocks, {"step:L", "step:R"}) == std::vector<std::string>{"a1", "a2"}));
    CHECK(AutoKeyRids(blocks, {"lift:L"}).empty());
    CHECK(AutoKeysNeedRids(blocks, {"lift:L"}));
    CHECK(!AutoKeysNeedRids(blocks, {"step:L"}));

    ItemSounds item;
    CHECK(!SetRulesLibrary(item, {"a1"}, ""));  // nothing to clear: no record added
    CHECK(item.rules.empty());
    CHECK(SetRulesLibrary(item, {"a1", "a2"}, "LIB1"));
    CHECK(item.rules.size() == 2 && FindRuleSounds(item, "a2")->library == "LIB1");
    CHECK(!SetRulesLibrary(item, {"a1", "a2"}, "LIB1"));
    CHECK(SetRulesValue(item, {"a1"}, "G1", "V1"));
    CHECK(!SetRulesValue(item, {"a2"}, "G1", ""));

    SoundStore store;
    SetAutoSoundLibrary(store, {"step:L", "lift:L", "grab:L"}, "STORE");
    // On the item: its rule (the same as the Sound card), not the store's entry.
    AutoSoundShown sh = AutoSoundFor(store, item, blocks, "step:L");
    CHECK(sh.on_item && sh.library == "LIB1" && sh.values.size() == 1);
    // A block of the key with no rid / no assignment: none (its Sound card says None too).
    sh = AutoSoundFor(store, item, blocks, "lift:L");
    CHECK(sh.on_item && sh.library.empty());
    // Not on the item: the store's entry (what Detect will give).
    sh = AutoSoundFor(store, item, blocks, "grab:L");
    CHECK(!sh.on_item && sh.library == "STORE");
    sh = AutoSoundFor(store, item, blocks, "grab:R");
    CHECK(!sh.on_item && sh.library.empty());
}

static void AddLib(SoundStore& s, const std::string& uid)
{
    SoundLibrary l;
    l.uid = uid;
    l.name = uid;
    s.libraries.push_back(l);
}

static void TestFill()
{
    SoundStore store;
    AddLib(store, "LIBS");
    SetAutoSoundLibrary(store, {"step:L", "step:R", "heel:L", "hand_pivot:R"}, "LIBS");
    SetAutoSoundValue(store, {"step:L"}, "G1", "V1");
    std::vector<Block> blocks = {Rule("r0"), Auto(AutoKind::Step, 'L', "a1"), Auto(AutoKind::Step, 'R', "a2"),
                                 Auto(AutoKind::Lift, 'L', "a3"), Auto(AutoKind::HandPivotAny, 'R', "a4"),
                                 Auto(AutoKind::Heel, 'L', "")};
    CHECK(AutoFillNeedsRids(blocks, store));  // heel:L has an entry, no rid yet
    ItemSounds item;
    RuleSounds own;
    own.rid = "a2";
    own.library = "MINE";
    item.rules.push_back(own);
    RuleSounds none;
    none.rid = "a1";  // a record with no library: filled
    none.values.push_back({"G2", "W", {}});
    item.rules.push_back(none);
    CHECK(FillAutoSounds(item, blocks, store));
    const RuleSounds* r1 = FindRuleSounds(item, "a1");
    CHECK(r1 && r1->library == "LIBS" && r1->values.size() == 2);
    CHECK(FindRuleSounds(item, "a2")->library == "MINE");   // its own kept
    CHECK(FindRuleSounds(item, "a3") == nullptr);           // lift: no entry
    CHECK(FindRuleSounds(item, "a4") && FindRuleSounds(item, "a4")->library == "LIBS");  // any-hand pivot = hand_pivot
    CHECK(FindRuleSounds(item, "r0") == nullptr);           // a rule is never filled
    CHECK(!FillAutoSounds(item, blocks, store));           // nothing more
    blocks[5].rid = "a5";
    CHECK(!AutoFillNeedsRids(blocks, store));
    CHECK(FillAutoSounds(item, blocks, store));
    CHECK(FindRuleSounds(item, "a5")->library == "LIBS");
    // No store entry with a library: nothing.
    ItemSounds empty;
    CHECK(!FillAutoSounds(empty, blocks, SoundStore{}));
    CHECK(empty.rules.empty());
}

// A row edit's step on the project's other items (Antho 2026-10-12): only the edited keys, only a
// rule with no library yet (never an explicit one), only an entry whose library is in the store.
static void TestFillOtherItems()
{
    SoundStore store;
    AddLib(store, "WOOD");
    AddLib(store, "MINE");
    SetAutoSoundLibrary(store, {"step:L", "step:R", "lift:L"}, "WOOD");
    SetAutoSoundValue(store, {"step:L"}, "G1", "V1");
    const std::vector<Block> blocks = {Rule("r0"), Auto(AutoKind::Step, 'L', "a1"), Auto(AutoKind::Step, 'R', "a2"),
                                       Auto(AutoKind::Lift, 'L', "a3"), Auto(AutoKind::Grab, 'L', "a4")};
    ItemSounds other;
    RuleSounds own;  // step:R set by hand on this item: kept
    own.rid = "a2";
    own.library = "MINE";
    other.rules.push_back(own);
    RuleSounds bare;  // step:L with only a value of its own, no library: filled, its value kept
    bare.rid = "a1";
    bare.values.push_back({"G2", "W", {}});
    other.rules.push_back(bare);

    // The row edited step L / R: a1 only (a2 explicit, lift / grab not edited).
    CHECK((AutoFillRids(other, blocks, store, {"step:L", "step:R"}) == std::vector<std::string>{"a1"}));
    CHECK(!AutoFillNeedsRids(blocks, store, {"step:L", "step:R"}));
    ItemSounds after = other;
    CHECK(FillAutoSounds(after, blocks, store, {"step:L", "step:R"}));
    CHECK(FindRuleSounds(after, "a1")->library == "WOOD" && FindRuleSounds(after, "a1")->values.size() == 2);
    CHECK(FindRuleSounds(after, "a2")->library == "MINE");
    CHECK(FindRuleSounds(after, "a3") == nullptr);  // lift:L has an entry but was not edited
    CHECK(!FillAutoSounds(after, blocks, store, {"step:L", "step:R"}));  // a second edit: nothing more
    // Once filled, the rule is the item's own: a later row edit of the key does not replace it.
    SetAutoSoundLibrary(store, {"step:L"}, "MINE");
    CHECK(AutoFillRids(after, blocks, store, {"step:L"}).empty());
    // Every key (Detect): lift too.
    CHECK((AutoFillRids(other, blocks, store) == std::vector<std::string>{"a1", "a3"}));
    // Cleared to None: nothing given.
    SetAutoSoundLibrary(store, {"step:L", "step:R", "lift:L"}, "");
    CHECK(AutoFillRids(other, blocks, store).empty());
    CHECK(!HasAutoSounds(store));

    // A block of the key with no rid yet: the caller gives rids first.
    SoundStore s2;
    AddLib(s2, "WOOD");
    SetAutoSoundLibrary(s2, {"grab:L"}, "WOOD");
    std::vector<Block> fresh = {Auto(AutoKind::Grab, 'L', ""), Auto(AutoKind::Step, 'L', "")};
    CHECK(AutoFillNeedsRids(fresh, s2, {"grab:L"}));
    CHECK(!AutoFillNeedsRids(fresh, s2, {"step:L"}));  // no entry of that key
    CHECK(AutoFillRids(ItemSounds{}, fresh, s2, {"grab:L"}).empty());  // no rid: not written yet

    // A library removed from the store since: never given, and Detect has nothing to give.
    SoundStore gone;
    SetAutoSoundLibrary(gone, {"grab:L"}, "DELETED");
    fresh[0].rid = "g1";
    CHECK(!HasAutoSounds(gone));
    CHECK(!AutoFillNeedsRids(fresh, gone));
    ItemSounds none;
    CHECK(!FillAutoSounds(none, fresh, gone) && none.rules.empty());
}

int main()
{
    TestKeys();
    TestStoreEdits();
    TestText();
    TestRulesAndShown();
    TestFill();
    TestFillOtherItems();
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("sound_auto_assign: all checks passed\n");
    return 0;
}
