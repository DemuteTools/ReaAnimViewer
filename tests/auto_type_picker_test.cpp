// SPDX-License-Identifier: MIT
//
// Story 10-10e: the Smart Detection type picker's data (src/auto_type_picker.h): the common types,
// the search, the presets' stored value (never breaks on a bad one) and applying a preset.

#include "auto_type_picker.h"
#include "auto_types.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

int main()
{
    // Common types first in their category, every type of the category once.
    for (int c = 0; c < kAutoCategoryCount; ++c) {
        const std::vector<AutoType> order = AutoPickerOrder(static_cast<AutoCategory>(c));
        CHECK(order.size() == AutoTypesOf(static_cast<AutoCategory>(c)).size());
        bool seen_rest = false;
        for (AutoType t : order) {
            if (!AutoTypeCommon(t)) seen_rest = true;
            else CHECK(!seen_rest);
        }
    }
    CHECK(AutoTypeCommon(AutoType::Step) && AutoTypeCommon(AutoType::Release) && !AutoTypeCommon(AutoType::Jolt));
    CHECK(AutoPickerOrder(AutoCategory::Legs).front() == AutoType::Step);

    // Search: label, category, marker word, case-insensitive; empty matches all.
    CHECK(AutoTypeMatches(AutoType::Clap, ""));
    CHECK(AutoTypeMatches(AutoType::Clap, "  "));
    CHECK(AutoTypeMatches(AutoType::Clap, "CLAP"));
    CHECK(AutoTypeMatches(AutoType::Step, "fs"));  // its marker word
    CHECK(AutoTypeMatches(AutoType::HandPivot, "arms pivot"));
    CHECK(!AutoTypeMatches(AutoType::Jolt, "clap"));

    // Names: control characters out, trimmed, capped without cutting UTF-8.
    CHECK(CleanAutoPresetName("  Fight\n ") == "Fight");
    CHECK(CleanAutoPresetName("\t\r") .empty());
    std::string longname(60, 'a');
    CHECK(CleanAutoPresetName(longname).size() == kAutoPresetNameMax);
    std::string utf(kAutoPresetNameMax - 1, 'a');
    utf += "\xC3\xA9";  // é straddles the cap
    CHECK(CleanAutoPresetName(utf) == std::string(kAutoPresetNameMax - 1, 'a'));

    // Round trip, with the separators in a name.
    AutoSettings s;
    s.type[static_cast<int>(AutoType::Step)].on = true;
    s.type[static_cast<int>(AutoType::Kick)].on = true;
    s.type[static_cast<int>(AutoType::HeelTap)].on = true;
    std::vector<AutoPreset> ps = {AutoPresetFromTicks("Dance | 50%; a,b", s), AutoPresetFromTicks("Empty", AutoSettings())};
    const std::string enc = EncodeAutoPresets(ps);
    CHECK(enc.find('\n') == std::string::npos);
    const std::vector<AutoPreset> dec = DecodeAutoPresets(enc);
    CHECK(dec.size() == 2);
    if (dec.size() == 2) {
        CHECK(dec[0].name == "Dance | 50%; a,b");
        CHECK(dec[0].types.size() == 3);
        CHECK(AutoPresetMatches(dec[0], s));
        CHECK(dec[1].name == "Empty" && dec[1].types.empty());
    }

    // Bad values never break: unknown words and bad escapes skipped, empty names dropped.
    CHECK(DecodeAutoPresets("").empty());
    CHECK(DecodeAutoPresets("garbage").empty());
    const std::vector<AutoPreset> bad = DecodeAutoPresets("A;step,nope,step|%ZZ;clap|;clap|B;|C;clap,,jolt");
    CHECK(bad.size() == 3);
    if (bad.size() == 3) {
        CHECK(bad[0].name == "A" && bad[0].types.size() == 1);
        CHECK(bad[1].name == "B" && bad[1].types.empty());
        CHECK(bad[2].name == "C" && bad[2].types.size() == 2);
    }
    std::string many;
    for (int i = 0; i < kAutoPresetMax + 10; ++i) many += (i ? "|P" : "P") + std::to_string(i) + ";step";
    CHECK(static_cast<int>(DecodeAutoPresets(many).size()) == kAutoPresetMax);

    // Applying: exactly its ticks (others unticked), values kept; a second apply changes nothing.
    AutoSettings cur;
    cur.type[static_cast<int>(AutoType::Jolt)].on = true;
    cur.type[static_cast<int>(AutoType::Jolt)].sens = 70.0;
    CHECK(!AutoPresetMatches(ps[0], cur));
    CHECK(ApplyAutoPreset(ps[0], cur));
    CHECK(AutoPresetMatches(ps[0], cur));
    CHECK(!cur.type[static_cast<int>(AutoType::Jolt)].on && cur.type[static_cast<int>(AutoType::Jolt)].sens == 70.0);
    CHECK(!ApplyAutoPreset(ps[0], cur));

    std::printf(g_fail ? "auto_type_picker: %d failure(s)\n" : "auto_type_picker: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
