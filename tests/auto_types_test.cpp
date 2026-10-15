// SPDX-License-Identifier: MIT
//
// Host test of the auto event types' table (src/auto_types.h, story 10-9-0): one row per enum
// value in enum order, unique save words and sound words, each kind's type and each type's
// category known, every row complete, and the categories' types (the Tagging view's rows, the
// sound parts' types) each type once, in enum order. No REAPER.
//   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests

#include "auto_types.h"
#include "sound_assign.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace rav;

namespace {

int g_fails = 0;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);        \
            ++g_fails;                                              \
        }                                                           \
    } while (0)

bool Filled(const char* s)
{
    return s && s[0];
}

void TestOrder()
{
    for (int c = 0; c < kAutoCategoryCount; ++c)
        CHECK(static_cast<int>(AutoCategoryRow(static_cast<AutoCategory>(c)).category) == c);
    for (int t = 0; t < kAutoTypeCount; ++t) CHECK(static_cast<int>(AutoTypeRow(static_cast<AutoType>(t)).type) == t);
    for (int k = 0; k < kAutoKindCount; ++k) CHECK(static_cast<int>(AutoKindRow(static_cast<AutoKind>(k)).kind) == k);
}

void TestUniqueWords()
{
    std::set<std::string> kind_words, sound_words, cat_parts, cat_labels;
    for (int k = 0; k < kAutoKindCount; ++k) {
        const AutoKindInfo& r = AutoKindRow(static_cast<AutoKind>(k));
        CHECK(Filled(r.word));
        CHECK(kind_words.insert(r.word).second);
        AutoKind back = AutoKind::Step;
        CHECK(AutoKindFromWord(r.word, &back) && back == r.kind);
    }
    for (int t = 0; t < kAutoTypeCount; ++t) {
        const AutoTypeInfo& r = AutoTypeRow(static_cast<AutoType>(t));
        CHECK(Filled(r.sound_word));
        CHECK(sound_words.insert(r.sound_word).second);
        AutoType back = AutoType::Step;
        CHECK(AutoTypeFromWord(r.sound_word, &back) && back == r.type);
    }
    for (int c = 0; c < kAutoCategoryCount; ++c) {
        const AutoCategoryInfo& r = AutoCategoryRow(static_cast<AutoCategory>(c));
        CHECK(Filled(r.label) && Filled(r.sound_part));
        CHECK(cat_parts.insert(r.sound_part).second);
        CHECK(cat_labels.insert(r.label).second);
        CHECK(FindBodyPart(r.sound_part) != nullptr);
    }
}

void TestRowsComplete()
{
    for (int t = 0; t < kAutoTypeCount; ++t) {
        const AutoTypeInfo& r = AutoTypeRow(static_cast<AutoType>(t));
        CHECK(Filled(r.label) && Filled(r.undo_label) && Filled(r.hint));
        CHECK(static_cast<int>(r.category) >= 0 && static_cast<int>(r.category) < kAutoCategoryCount);
        CHECK(r.sens[0].field != nullptr);  // the sensitivity moves a constant
        CHECK(r.parts != nullptr);
        CHECK(!AutoKindsOf(r.type).empty());  // a type has a kind
    }
    for (int k = 0; k < kAutoKindCount; ++k) {
        const AutoKindInfo& r = AutoKindRow(static_cast<AutoKind>(k));
        CHECK(static_cast<int>(r.type) >= 0 && static_cast<int>(r.type) < kAutoTypeCount);  // the kind's type exists
        CHECK(Filled(r.marker));
        CHECK(r.role_key != nullptr);
        CHECK(r.source && r.match && r.part_missing && r.lanes && r.roles);
        CHECK(!r.roles('L').empty() && !r.roles('R').empty());
        // The sound key's kind is a kind of the same type with the same marker.
        const AutoKindInfo& key = AutoKindRow(r.sound_key);
        CHECK(key.type == r.type);
        CHECK(std::strcmp(key.marker, r.marker) == 0);
        CHECK(key.sound_key == key.kind);
        CHECK((r.color[0] & 0xFF000000u) == 0 && (r.color[1] & 0xFF000000u) == 0);
    }
}

// The categories' types: each type once, under its category, in enum order; the sound parts list
// the same types.
void TestCategoryTypes()
{
    std::vector<int> seen(kAutoTypeCount, 0);
    for (int c = 0; c < kAutoCategoryCount; ++c) {
        const AutoCategory          cat = static_cast<AutoCategory>(c);
        const std::vector<AutoType> types = AutoTypesOf(cat);
        CHECK(!types.empty());
        for (size_t i = 0; i < types.size(); ++i) {
            CHECK(AutoCategoryOf(types[i]) == cat);
            if (i) CHECK(static_cast<int>(types[i - 1]) < static_cast<int>(types[i]));
            ++seen[static_cast<size_t>(types[i])];
        }
        const BodyPart* part = FindBodyPart(AutoCategoryRow(cat).sound_part);
        CHECK(part && part->has_category && part->category == cat);
        if (part) {
            std::vector<AutoType> part_types;
            for (const PartType& pt : part->types)
                if (pt.is_auto) part_types.push_back(pt.auto_type);
            CHECK(part_types == types);
        }
    }
    for (int t = 0; t < kAutoTypeCount; ++t) CHECK(seen[static_cast<size_t>(t)] == 1);
    // Each type's kinds, in enum order, all of that type.
    for (int t = 0; t < kAutoTypeCount; ++t) {
        const std::vector<AutoKind> kinds = AutoKindsOf(static_cast<AutoType>(t));
        for (size_t i = 0; i < kinds.size(); ++i) {
            CHECK(AutoTypeOfKind(kinds[i]) == static_cast<AutoType>(t));
            if (i) CHECK(static_cast<int>(kinds[i - 1]) < static_cast<int>(kinds[i]));
        }
    }
}

// A few rows read back through the public functions (the values saved data and the UI rely on).
void TestKnownValues()
{
    CHECK(std::string(AutoKindWord(AutoKind::HandPivotAny)) == "hand_pivot_any");
    CHECK(AutoMarkerName(AutoKind::Step, 'L') == "FS L");
    CHECK(AutoMarkerName(AutoKind::PivotToeLongest, 'R') == "Pivot Toe R");
    CHECK(AutoDefaultColor(AutoKind::Grab, 'R') == (0x1000000u | 0xDDB45Fu));
    CHECK(std::string(AutoTypeLabel(AutoType::HandPivot)) == "Pivot scuff");
    CHECK(std::string(AutoTypeRow(AutoType::HandPivot).undo_label) == "Hands Pivot scuff");
    CHECK(std::string(AutoTypeWord(AutoType::LiftOff)) == "lift");
    CHECK(std::string(AutoCategoryLabel(AutoCategory::Hands)) == "Hands");
    CHECK(AutoKindRow(AutoKind::PivotHeel).degrees && !AutoKindRow(AutoKind::SlideHeel).degrees);
    CHECK(AutoKindRow(AutoKind::SlideToeLongest).scuff == ScuffParts::Longest && !AutoKindRow(AutoKind::SlideToeLongest).scuff_heel);
    const PhysicsParams d;
    const PhysicsParams p = AutoPhysicsParams(AutoType::Pivot, 100.0, false);
    CHECK(p.pivot_min_deg == d.pivot_min_deg / 2.0 && p.pivot_rate_dps == d.pivot_rate_dps / 2.0);
    CHECK(p.contact_speed == d.contact_speed && p.slide_speed == d.slide_speed);
}

// AutoTypeSeparate: only steps, slides and pivots have the mode.
void TestTypeSeparate()
{
    AutoSettings s;
    for (int t = 0; t < kAutoTypeCount; ++t) CHECK(!AutoTypeSeparate(static_cast<AutoType>(t), s));
    s.separate_steps = true;
    CHECK(AutoTypeSeparate(AutoType::Step, s));
    CHECK(!AutoTypeSeparate(AutoType::LiftOff, s) && !AutoTypeSeparate(AutoType::Slide, s) &&
          !AutoTypeSeparate(AutoType::Pivot, s));
    for (ScuffParts m : {ScuffParts::EachPart, ScuffParts::Longest}) {
        AutoSettings sl;
        sl.slide_parts = m;
        CHECK(AutoTypeSeparate(AutoType::Slide, sl) && !AutoTypeSeparate(AutoType::Pivot, sl) &&
              !AutoTypeSeparate(AutoType::Step, sl));
        AutoSettings pv;
        pv.pivot_parts = m;
        CHECK(AutoTypeSeparate(AutoType::Pivot, pv) && !AutoTypeSeparate(AutoType::Slide, pv) &&
              !AutoTypeSeparate(AutoType::Step, pv));
    }
    AutoSettings all;
    all.separate_steps = true;
    all.hand_pivot_any = true;
    all.slide_parts = ScuffParts::EachPart;
    all.pivot_parts = ScuffParts::Longest;
    for (AutoType t : {AutoType::LiftOff, AutoType::Grab, AutoType::Release, AutoType::HandPivot})
        CHECK(!AutoTypeSeparate(t, all));
}

// The kinds' highlighted roles: a hand kind its hand, a foot kind its foot's parts in order.
void TestRoles()
{
    for (int k = 0; k < kAutoKindCount; ++k) {
        const AutoKindInfo& r = AutoKindRow(static_cast<AutoKind>(k));
        for (char side : {'L', 'R'}) {
            std::vector<Role> want;
            if (AutoCategoryOf(r.type) == AutoCategory::Hands) {
                want.push_back(side == 'R' ? Role::RightHand : Role::LeftHand);
            } else {
                for (int q = 0; q < kFootPartCount; ++q) want.push_back(FootPartRole(side, q));
            }
            CHECK(r.roles(side) == want);
        }
    }
}

// The kinds' flags agree with each other and with their types.
void TestRowInvariants()
{
    for (int k = 0; k < kAutoKindCount; ++k) {
        const AutoKindInfo& r = AutoKindRow(static_cast<AutoKind>(k));
        if (r.scuff_heel) CHECK(r.scuff != ScuffParts::Combined);
        if (r.scuff != ScuffParts::Combined) CHECK(r.type == AutoType::Slide || r.type == AutoType::Pivot);
        CHECK(r.degrees == (r.type == AutoType::Pivot || r.type == AutoType::HandPivot));
        CHECK((std::string(r.role_key) == "hand") == (AutoCategoryOf(r.type) == AutoCategory::Hands));
        for (int j = 0; j < kAutoKindCount; ++j) {
            const AutoKindInfo& o = AutoKindRow(static_cast<AutoKind>(j));
            if (j == k || o.type != r.type || o.scuff != r.scuff) continue;
            const bool hand_pivot_pair = (r.kind == AutoKind::HandPivot && o.kind == AutoKind::HandPivotAny) ||
                                         (r.kind == AutoKind::HandPivotAny && o.kind == AutoKind::HandPivot);
            if (hand_pivot_pair) CHECK(r.source != o.source);
            else CHECK(r.source == o.source);
        }
        if (r.scuff != ScuffParts::Combined) {
            const AutoKindInfo& combined = AutoKindRow(r.type == AutoType::Slide ? AutoKind::Slide : AutoKind::Pivot);
            CHECK(combined.scuff == ScuffParts::Combined && combined.type == r.type);
            if (r.scuff == ScuffParts::EachPart) CHECK(r.source != combined.source);
            else CHECK(r.source == combined.source);
        }
    }
}

}  // namespace

int main()
{
    TestOrder();
    TestUniqueWords();
    TestRowsComplete();
    TestCategoryTypes();
    TestKnownValues();
    TestTypeSeparate();
    TestRoles();
    TestRowInvariants();
    if (g_fails) {
        std::printf("auto_types_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("auto_types_test: all passed\n");
    return 0;
}
