// SPDX-License-Identifier: MIT
//
// Host test of src/normalize_presets.h (story 13-28): the presets' file round trip, bad lines, the defaults,
// an empty list kept, name checks, the lit chip, the toast text. Pure: any C++17 compiler.

#include "normalize_presets.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace rav;

namespace {

int g_fails = 0;

#define CHECK(c)                                             \
    do {                                                     \
        if (!(c)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            ++g_fails;                                       \
        }                                                    \
    } while (0)

bool Same(const std::vector<NormPreset>& a, const std::vector<NormPreset>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].name != b[i].name || a[i].scope != b[i].scope || a[i].mode != b[i].mode ||
            std::fabs(a[i].target - b[i].target) > 1e-9)
            return false;
    return true;
}

void TestRoundTrip()
{
    const std::vector<NormPreset> p = {
        {"Steps one", NormScope::Variation, NormMode::Peak, -1.0},
        {"Pas \xC3\xA9t\xC3\xA9 \xE2\x88\x92" "18", NormScope::Part, NormMode::LufsSMax, -18.0},  // unicode, U+2212
        {"Whole  loud ", NormScope::Whole, NormMode::TruePeak, -0.3},
    };
    const std::string text = SerializeNormPresets(p);
    CHECK(text.rfind("ravnormpresets 1\n", 0) == 0);
    CHECK(text.find("Steps one\tvariation\tpeak\t-1\n") != std::string::npos);
    bool                    header = false;
    std::vector<NormPreset> back = ParseNormPresets(text, &header);
    CHECK(header && back.size() == 3);
    // Names are trimmed when written.
    std::vector<NormPreset> want = p;
    want[2].name = "Whole  loud";
    CHECK(Same(back, want));
    CHECK(SerializeNormPresets(back) == text);
    // Every mode and scope word reads back.
    for (int i = 0; i < kNormModeCount; ++i) {
        NormMode m = NormMode::Peak;
        CHECK(NormModeFromWord(NormModeWord(static_cast<NormMode>(i)), &m) && m == static_cast<NormMode>(i));
    }
}

void TestBadLines()
{
    const std::string text =
        "ravnormpresets 1\n"
        "Good\tvariation\tpeak\t-1\n"
        "Bad scope\tsome\tpeak\t-1\n"
        "Bad mode\twhole\tloud\t-1\n"
        "Bad target\twhole\tlufsi\tx\n"
        "Short\twhole\n"
        "\tvariation\tpeak\t-1\n"
        "good\twhole\tlufsi\t-14\n"  // a name already taken (case-insensitive)
        "Clamped\tpart\trmsi\t-99\r\n"
        "Also good\tpart\tlufsm\t-9.5\n";
    const std::vector<NormPreset> p = ParseNormPresets(text);
    CHECK(p.size() == 3);
    if (p.size() == 3) {
        CHECK(p[0].name == "Good" && p[1].name == "Clamped" && p[1].target == kNormTargetMin && p[1].mode == NormMode::RmsI);
        CHECK(p[2].name == "Also good" && p[2].scope == NormScope::Part && p[2].mode == NormMode::LufsMMax &&
              p[2].target == -9.5);
    }
}

void TestFiles()
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "rav_normalize_presets_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    // No file: the 3 mockup defaults.
    std::vector<NormPreset> p = LoadNormPresets(root.u8string());
    CHECK(Same(p, DefaultNormPresets()) && p.size() == 3);
    CHECK(p[0].name == "Steps \xE2\x88\x92" "1 peak" && p[1].scope == NormScope::Part && p[1].mode == NormMode::LufsSMax &&
          p[2].scope == NormScope::Whole && p[2].mode == NormMode::LufsI && p[2].target == -14.0);
    CHECK(!fs::exists(fs::u8path(NormPresetsPath(root.u8string()))));  // nothing written by a load
    // Saved, then read back.
    p.push_back({"Mine", NormScope::Whole, NormMode::RmsI, -20.0});
    std::string err;
    CHECK(SaveNormPresets(root.u8string(), p, &err) && err.empty());
    CHECK(Same(LoadNormPresets(root.u8string()), p));
    CHECK(NormPresetsPath(root.u8string()).find("ReaAnimViewer") != std::string::npos);
    // An empty list once saved stays empty.
    CHECK(SaveNormPresets(root.u8string(), {}, &err));
    CHECK(LoadNormPresets(root.u8string()).empty());
    // A bad line in the file: skipped, the others loaded.
    {
        std::ofstream out(fs::u8path(NormPresetsPath(root.u8string())), std::ios::binary | std::ios::trunc);
        out << "ravnormpresets 1\nA\tvariation\tpeak\t-1\nbroken line\nB\twhole\tlufsi\t-14\n";
    }
    bool unread = true;
    p = LoadNormPresets(root.u8string(), &unread);
    CHECK(p.size() == 2 && p[0].name == "A" && p[1].name == "B" && !unread);
    // A preset whose name starts like the header is a preset (it has tabs), not the header.
    CHECK(ParseNormPresets("ravnormpresets 1\nravnormpresets 2\tvariation\tpeak\t-3\n").size() == 1);
    // The file there but unreadable: the defaults, reported (the caller never saves over it).
    fs::remove(fs::u8path(NormPresetsPath(root.u8string())), ec);
    fs::create_directories(fs::u8path(NormPresetsPath(root.u8string())), ec);  // a folder in its place
    p = LoadNormPresets(root.u8string(), &unread);
    CHECK(unread && Same(p, DefaultNormPresets()));
    fs::remove_all(root, ec);
    CHECK(!LoadNormPresets(root.u8string(), &unread).empty() && !unread);  // no file: not a read failure
}

void TestNamesAndMatch()
{
    const std::vector<NormPreset> p = DefaultNormPresets();
    CHECK(NormPresetNameError(p, "   ") == "Type a name");
    CHECK(NormPresetNameError(p, "STEPS \xE2\x88\x92" "1 PEAK") == "A preset is already named Steps \xE2\x88\x92" "1 peak");
    CHECK(NormPresetNameError(p, "Steps \xE2\x88\x92" "1 peak", 0).empty());  // its own name when renamed
    CHECK(NormPresetNameError(p, " New one ").empty());
    CHECK(CleanNormPresetName("  a\tb\n ") == "a b");
    // Lit chip: the effective scope (Per part on an undivided Sound = Per variation), targets within 0.05.
    CHECK(MatchingNormPreset(p, NormScope::Variation, NormMode::Peak, -1.04, false) == 0);
    CHECK(MatchingNormPreset(p, NormScope::Variation, NormMode::Peak, -1.1, false) == -1);
    CHECK(MatchingNormPreset(p, NormScope::Part, NormMode::LufsSMax, -18.0, true) == 1);
    CHECK(MatchingNormPreset(p, NormScope::Variation, NormMode::LufsSMax, -18.0, true) == -1);
    CHECK(MatchingNormPreset(p, NormScope::Variation, NormMode::LufsSMax, -18.0, false) == 1);  // undivided
    CHECK(MatchingNormPreset(p, NormScope::Part, NormMode::Peak, -1.0, false) == 0);
    CHECK(MatchingNormPreset(p, NormScope::Whole, NormMode::LufsI, -14.0, true) == 2);
    CHECK(EffectiveNormScope(NormScope::Part, false) == NormScope::Variation &&
          EffectiveNormScope(NormScope::Part, true) == NormScope::Part);
}

void TestTexts()
{
    CHECK(FormatNormTarget(-1.0) == "-1" && FormatNormTarget(-14.5) == "-14.5" && FormatNormTarget(-0.04) == "0");
    CHECK(NormPresetDescription({"x", NormScope::Variation, NormMode::Peak, -1.0}) == "Per variation \xC2\xB7 Peak \xC2\xB7 -1 dBFS");
    CHECK(NormalizeToastText("Steps", 4, 0, NormScope::Variation, NormMode::Peak, -1.0) ==
          "Steps: 4 variations normalized to -1 dBFS (Peak)");
    CHECK(NormalizeToastText("", 6, 0, NormScope::Part, NormMode::LufsSMax, -18.0) ==
          "6 parts normalized to -18 LUFS (LUFS-S max)");
    CHECK(NormalizeToastText("W", 3, 1, NormScope::Whole, NormMode::LufsI, -14.0) ==
          "W: 3 variations normalized to -14 LUFS (LUFS-I, whole sound), 1 silent, left as is");
    CHECK(NormalizeToastText("", 1, 0, NormScope::Variation, NormMode::TruePeak, -1.0) ==
          "1 variation normalized to -1 dBTP (True peak)");
    CHECK(std::string(NormModeUnit(NormMode::RmsI)) == "dB" && NormModeFromIndex(9) == NormMode::Peak &&
          NormScopeFromIndex(2) == NormScope::Whole);
    // 13-62: regions with their own targets: one shared target names it, else "each to its own target".
    using T = std::vector<std::pair<NormMode, double>>;
    CHECK(NormalizeRegionsToastText("Normalize per part", 4, 0, NormScope::Part,
                                    T{{NormMode::Peak, -1.0}, {NormMode::Peak, -1.02}}) ==
          "Normalize per part: 4 parts normalized to -1 dBFS (Peak)");
    CHECK(NormalizeRegionsToastText("Normalize per part", 4, 1, NormScope::Part,
                                    T{{NormMode::Peak, -1.0}, {NormMode::LufsI, -18.0}}) ==
          "Normalize per part: 4 parts normalized, each to its own target, 1 silent, left as is");
    CHECK(NormalizeRegionsToastText("", 1, 0, NormScope::Part, T{{NormMode::Peak, -1.0}, {NormMode::Peak, -3.0}}) ==
          "1 part normalized, each to its own target");
    CHECK(NormalizeRegionsToastText("Toe", 2, 0, NormScope::Part, T{{NormMode::LufsSMax, -18.0}}) ==
          "Toe: 2 parts normalized to -18 LUFS (LUFS-S max)");
}

}  // namespace

int main()
{
    TestRoundTrip();
    TestBadLines();
    TestFiles();
    TestNamesAndMatch();
    TestTexts();
    if (g_fails) {
        std::printf("normalize_presets_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("normalize_presets_test: all passed\n");
    return 0;
}
