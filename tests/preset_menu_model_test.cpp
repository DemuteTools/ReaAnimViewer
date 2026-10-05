// SPDX-License-Identifier: MIT
//
// Host test of the Tagging view's preset menu logic (src/preset_menu_model.h, story 10-3b):
// the search filter, the row stepping, the name checks and the strings. No REAPER, no
// Windows: any C++17 compiler.

#include "preset_menu_model.h"

#include <cstdio>
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

PresetInfo P(const std::string& id, const std::string& name, bool factory)
{
    PresetInfo p;
    p.id = id;
    p.name = name;
    p.factory = factory;
    return p;
}

std::vector<PresetInfo> Sample()
{
    // As ListPresets returns them: factory first, then user by name.
    return {P("factory/footsteps", "Footsteps", true), P("factory/footsteps-heel", "Footsteps Heel", true),
            P("user/boots", "Heavy boots", false), P("user/steps", "My steps", false),
            P("user/tiptoe", "Tiptoe Footsteps", false)};
}

std::vector<std::string> Ids(const std::vector<PresetInfo>& ps, const std::vector<int>& rows)
{
    std::vector<std::string> out;
    for (int i : rows) out.push_back(ps[static_cast<size_t>(i)].id);
    return out;
}

}  // namespace

int main()
{
    const std::vector<PresetInfo> ps = Sample();

    // ---- filter ----
    {
        // No query: the open cascade only; none open = no rows.
        CHECK(PresetMenuRows(ps, "", PresetCascade::None).empty());
        CHECK((Ids(ps, PresetMenuRows(ps, "", PresetCascade::Factory)) ==
               std::vector<std::string>{"factory/footsteps", "factory/footsteps-heel"}));
        CHECK((Ids(ps, PresetMenuRows(ps, "", PresetCascade::User)) ==
               std::vector<std::string>{"user/boots", "user/steps", "user/tiptoe"}));
        // A query flattens both sources, case-insensitive substring, Factory hits first.
        CHECK((Ids(ps, PresetMenuRows(ps, "STEPS", PresetCascade::User)) ==
               std::vector<std::string>{"factory/footsteps", "factory/footsteps-heel", "user/steps", "user/tiptoe"}));
        CHECK((Ids(ps, PresetMenuRows(ps, "boo", PresetCascade::None)) == std::vector<std::string>{"user/boots"}));
        CHECK(PresetMenuRows(ps, "zzz", PresetCascade::Factory).empty());
        CHECK(PresetMenuRows({}, "a", PresetCascade::None).empty());
        CHECK(CountPresets(ps, true) == 2 && CountPresets(ps, false) == 3);
    }

    // ---- stepping ----
    {
        const std::vector<std::string> rows = {"a", "b", "c"};
        CHECK(StepPresetSelection(rows, "", 1) == "a");   // Down from nothing = first
        CHECK(StepPresetSelection(rows, "", -1) == "c");  // Up from nothing = last
        CHECK(StepPresetSelection(rows, "gone", 1) == "a");
        CHECK(StepPresetSelection(rows, "gone", -1) == "c");
        CHECK(StepPresetSelection(rows, "a", 1) == "b");
        CHECK(StepPresetSelection(rows, "b", -1) == "a");
        CHECK(StepPresetSelection(rows, "c", 1) == "c");   // no wrap at the end
        CHECK(StepPresetSelection(rows, "a", -1) == "a");  // ...nor at the start
        CHECK(StepPresetSelection({}, "a", 1).empty());
        CHECK(StepPresetSelection({"x"}, "x", 1) == "x");
    }

    // ---- names ----
    {
        CHECK(TrimPresetName("  My steps \t") == "My steps");
        CHECK(TrimPresetName("   ").empty());
        PresetInfo clash;
        CHECK(CheckSaveAsName(ps, "   ", &clash) == NameCheck::Empty);
        CHECK(CheckSaveAsName(ps, "", nullptr) == NameCheck::Empty);
        // A User name clashes in any case, trimmed.
        CHECK(CheckSaveAsName(ps, "  my STEPS ", &clash) == NameCheck::UserClash && clash.id == "user/steps");
        CHECK(CheckSaveAsName(ps, "heavy boots", nullptr) == NameCheck::UserClash);
        // A Factory name can be reused.
        CHECK(CheckSaveAsName(ps, "Footsteps", nullptr) == NameCheck::Ok);
        CHECK(CheckSaveAsName(ps, "footsteps heel", nullptr) == NameCheck::Ok);
        CHECK(CheckSaveAsName(ps, "New one", nullptr) == NameCheck::Ok);
        CHECK(CheckSaveAsName(ps, "My step", nullptr) == NameCheck::Ok);  // substring is no clash
        CHECK(CheckRenameName(" ") == NameCheck::Empty);
        CHECK(CheckRenameName("x") == NameCheck::Ok);
        CHECK(SamePresetName("AbC", "aBc") && !SamePresetName("abc", "abd"));

        CHECK(ExportFileName("My steps") == "My steps.ravpreset");
        CHECK(ExportFileName("a/b:c*?") == "a_b_c__.ravpreset");
        CHECK(ExportFileName("  ") == "preset.ravpreset");
        CHECK(ExportFileName("end. ") == "end.ravpreset");
    }

    // ---- strings ----
    {
        CHECK(std::string(PresetMenuHint(PresetMenuMode::List)) == "Double-click or Enter loads the selected preset.");
        CHECK(std::string(PresetMenuHint(PresetMenuMode::Renaming)) == "Enter renames, Esc cancels.");
        CHECK(SearchPlaceholder(5) == "Search 5 presets");
        CHECK(CascadeLabel(true, 2) == "Factory (2)" && CascadeLabel(false, 0) == "User (0)");
        CHECK(FactorySaveNote("Footsteps") == "Footsteps is a factory preset: Save as writes your version to User.");
        CHECK(EmptyNameStatus(false) == "A preset needs a name. Nothing has been saved.");
        CHECK(EmptyNameStatus(true) == "A preset needs a name. Nothing has been renamed.");
        CHECK(CancelStatus(PresetMenuMode::Confirm, PresetConfirm::Delete) == "Nothing has been deleted.");
        CHECK(CancelStatus(PresetMenuMode::Confirm, PresetConfirm::Overwrite) == "Nothing has been saved.");
        CHECK(CancelStatus(PresetMenuMode::Naming, PresetConfirm::None) == "Nothing has been saved.");
        CHECK(CancelStatus(PresetMenuMode::Renaming, PresetConfirm::None) == "Nothing has been renamed.");
        CHECK(ConfirmQuestion(PresetConfirm::Overwrite, "My steps") == "Overwrite My steps?");
        CHECK(ConfirmQuestion(PresetConfirm::Delete, "My steps") == "Delete My steps?");
        CHECK(ConfirmDetail(PresetConfirm::Overwrite, true).rfind("A preset already has this name. ", 0) == 0);
        CHECK(ConfirmDetail(PresetConfirm::Overwrite, false).find("Ctrl+Z does not bring it back") != std::string::npos);
        CHECK(ConfirmDetail(PresetConfirm::Delete, false).find("Ctrl+Z does not bring it back") != std::string::npos);
        CHECK(std::string(ConfirmAction(PresetConfirm::Delete)) == "Delete");
        CHECK(std::string(ConfirmHint(PresetConfirm::Overwrite)) == "Enter overwrites, Esc cancels.");
        CHECK(LegacyText(3) == "Legacy \xC2\xB7 v3 is installed");
        CHECK(KeepLabel(2) == "Keep v2" && KeptTag(2) == "kept v2");
    }

    // ---- kept tag ----
    {
        CHECK(ShowKeptTag(true, false, 3, 3));
        CHECK(!ShowKeptTag(true, false, 3, 4));   // a newer one after Keep
        CHECK(!ShowKeptTag(true, false, 0, 0));   // never kept
        CHECK(!ShowKeptTag(true, true, 3, 3));    // gone
        CHECK(!ShowKeptTag(false, false, 3, 3));  // no preset
    }

    if (g_fails) {
        std::printf("preset_menu_model_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("preset_menu_model_test: all passed\n");
    return 0;
}
