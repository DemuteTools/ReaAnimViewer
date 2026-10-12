// SPDX-License-Identifier: MIT
//
// Story 13-8, reworked by 13-15a -- src/sound_auto_assign.h (pure): the key of an auto marker type,
// the row's rule edits and what a row shows.

#include "sound_auto_assign.h"

#include <cstdio>
#include <string>
#include <vector>

#include "sound_assign.h"
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

static void TestRulesAndShown()
{
    std::vector<Block> blocks = {Rule("r0"), Auto(AutoKind::Step, 'L', "a1"), Auto(AutoKind::Step, 'R', "a2"),
                                 Auto(AutoKind::Lift, 'L', "")};
    CHECK((AutoKeyRids(blocks, {"step:L", "step:R"}) == std::vector<std::string>{"a1", "a2"}));
    CHECK(AutoKeyRids(blocks, {"lift:L"}).empty());
    CHECK(AutoKeysNeedRids(blocks, {"lift:L"}));
    CHECK(!AutoKeysNeedRids(blocks, {"step:L"}));

    ItemSounds item;
    CHECK(!SetRulesLibrary(item, {"a1"}, LibState::Inherit, ""));  // nothing to clear: no record added
    CHECK(item.rules.empty());
    CHECK(SetRulesLibrary(item, {"a1", "a2"}, LibState::Library, "LIB1"));
    CHECK(item.rules.size() == 2 && FindRuleSounds(item, "a2")->library == "LIB1");
    CHECK(!SetRulesLibrary(item, {"a1", "a2"}, LibState::Library, "LIB1"));
    CHECK(SetRulesValue(item, {"a1"}, "G1", "V1"));
    CHECK(!SetRulesValue(item, {"a2"}, "G1", ""));
    // None is not inherit.
    CHECK(SetRulesLibrary(item, {"a2"}, LibState::None, ""));
    CHECK(FindRuleSounds(item, "a2")->none && FindRuleSounds(item, "a2")->library.empty());
    CHECK(SetRulesLibrary(item, {"a2"}, LibState::Inherit, ""));
    CHECK(!FindRuleSounds(item, "a2")->none);

    SoundStore store;
    SetAssignLibraryIn(store.project, {"feet.step", "feet.lift", "hands.grab"}, LibState::Library, "STORE");
    // On the item: its rule (the same as the Sound card).
    AutoSoundShown sh = AutoSoundFor(store, item, nullptr, blocks, "step:L");
    CHECK(sh.on_item && sh.own_state == LibState::Library && sh.own_library == "LIB1" && sh.values.size() == 1);
    CHECK(sh.effective.library == "LIB1" && sh.effective.from == "this type");
    // A block of the key with no rule: it inherits the Project's.
    sh = AutoSoundFor(store, item, nullptr, blocks, "lift:L");
    CHECK(sh.on_item && sh.own_state == LibState::Inherit && sh.effective.library == "STORE");
    CHECK(sh.effective.from == "Project \xC2\xB7 Feet \xC2\xB7 Lift-off");
    // Not on the item: what a new block would get.
    sh = AutoSoundFor(store, item, nullptr, blocks, "grab:L");
    CHECK(!sh.on_item && sh.effective.library == "STORE");
    sh = AutoSoundFor(store, item, nullptr, blocks, "grab:R");
    CHECK(!sh.on_item && sh.effective.library == "STORE");
    sh = AutoSoundFor(store, item, nullptr, blocks, "release:R");
    CHECK(!sh.on_item && sh.effective.library.empty() && !sh.effective.none);
}

int main()
{
    TestKeys();
    TestRulesAndShown();
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("sound_auto_assign: all checks passed\n");
    return 0;
}
