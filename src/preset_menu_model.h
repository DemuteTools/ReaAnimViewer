// SPDX-License-Identifier: MIT
//
// The Tagging view's preset menu, its pure logic (Epic 10, story 10-3b): the search filter,
// the row stepping, the name checks and the strings the menu shows. It ports DM-XYZ-Pad's
// lib/ui/preset_menu.lua behaviour (mock-up v7, renderPMenu):
//   - the list is a Factory (n) / User (n) cascade, flattened into one list while searching
//     (case-insensitive substring, Factory hits first);
//   - Up / Down step through the visible rows without wrapping (Down from nothing = the
//     first row, Up from nothing = the last);
//   - a name is trimmed; an empty one is refused; Save as clashes only with a User name
//     (any case): a Factory name can be reused.
//
// Pure C++17 (no ImGui, no REAPER, no _WIN32). Host-tested (tests/preset_menu_model_test.cpp).

#pragma once

#include <string>
#include <vector>

#include "preset_store.h"  // PresetInfo

namespace rav {

enum class PresetCascade { None, Factory, User };
enum class PresetMenuMode { List, Naming, Renaming, Confirm };
enum class PresetConfirm { None, Overwrite, Delete };

// The menu's state: one struct, reset when the popup closes.
struct PresetMenuState {
    bool           open = false;
    PresetCascade  cascade = PresetCascade::None;
    std::string    sel;                  // the selected row's preset id ("" = none)
    PresetMenuMode mode = PresetMenuMode::List;
    PresetConfirm  confirm = PresetConfirm::None;
    bool           confirm_clash = false;  // the overwrite comes from a Save as name clash
    std::string    target_id;              // the preset renamed / confirmed
    std::string    target_name;
    std::string    status;                 // the status line ("" = the mode's hint)
};

// The rows shown: indices into `presets`. A non-empty query flattens both sources (Factory
// hits first, then User hits, each in list order); an empty one shows the open cascade's
// presets (none when no cascade is open).
std::vector<int> PresetMenuRows(const std::vector<PresetInfo>& presets, const std::string& query,
                                PresetCascade cascade);

// Up (dir < 0) / Down (dir > 0) from `sel` among `rows` (ids): no wrap; from nothing (or an
// id no longer shown) Down picks the first row and Up the last. "" when there is no row.
std::string StepPresetSelection(const std::vector<std::string>& rows, const std::string& sel, int dir);

// How many presets each source has.
int CountPresets(const std::vector<PresetInfo>& presets, bool factory);

// ---- names -------------------------------------------------------------------------------

std::string TrimPresetName(const std::string& name);
// True when two names are the same ignoring ASCII case.
bool SamePresetName(const std::string& a, const std::string& b);

enum class NameCheck { Ok, Empty, UserClash };
// Save as: the trimmed name is empty, or names a User preset (any case; `clash` gets that
// preset). A Factory name is Ok: Save as writes a User preset of that name.
NameCheck CheckSaveAsName(const std::vector<PresetInfo>& presets, const std::string& name, PresetInfo* clash);
// Rename: only an empty name is refused here (a taken name is refused by the store, with
// its reason).
NameCheck CheckRenameName(const std::string& name);

// "<name>.ravpreset", with the characters Windows refuses in a file name replaced by '_'
// ("preset" when nothing is left).
std::string ExportFileName(const std::string& preset_name);

// ---- strings -----------------------------------------------------------------------------

// The hint line of a mode, shown when there is no status.
const char* PresetMenuHint(PresetMenuMode mode);
std::string SearchPlaceholder(int count);         // "Search 3 presets"
std::string CascadeLabel(bool factory, int count); // "Factory (1)"
const char* EmptyCascadeText(bool factory);
constexpr const char kNoPresetMatches[] = "No preset matches.";
constexpr const char kNothingLoadedToSave[] =
    "Nothing is loaded to save over. Use Save As to write a new preset.";
constexpr const char kFolderCopied[] = "Folder path copied.";
constexpr const char kFactoryReadOnly[] = "Factory presets are read-only.";
constexpr const char kPresetGoneTip[] = "This preset is no longer on disk";
std::string FactorySaveNote(const std::string& name);  // "<name> is a factory preset: Save as ..."
// "A preset needs a name. Nothing has been saved." (renaming: "...renamed.").
std::string EmptyNameStatus(bool renaming);
// Esc / Cancel / closing the menu while naming, renaming or confirming.
std::string CancelStatus(PresetMenuMode mode, PresetConfirm confirm);

// The amber confirm: question, detail, action button, hint.
std::string ConfirmQuestion(PresetConfirm kind, const std::string& name);
std::string ConfirmDetail(PresetConfirm kind, bool clash);
const char* ConfirmAction(PresetConfirm kind);
const char* ConfirmHint(PresetConfirm kind);

// The Legacy band and the kept tag.
std::string LegacyText(int disk_version);   // "Legacy <middle dot> v3 is installed" (UTF-8)
std::string KeepLabel(int copy_version);    // "Keep v2"
std::string KeptTag(int copy_version);      // "kept v2"
// The faint kept tag shows: Keep dismissed the version now on disk. (The Legacy band shows
// from the preset state, PresetStateOf.)
bool ShowKeptTag(bool has_preset, bool gone, int kept_version, int disk_version);

}  // namespace rav
