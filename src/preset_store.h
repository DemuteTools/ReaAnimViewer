// SPDX-License-Identifier: MIT
//
// Presets on disk (Epic 10, story 10-2). Two sources, one grammar (rule_record.h):
//   factory  compiled into the DLL from presets/factory/*.ravpreset (id "factory/<stem>").
//            Read-only by construction: Save, Rename and Delete are refused with a reason.
//            They update with the extension by every route; the menu shows them as built-in.
//   user     <root>/ReaAnimViewer/Presets/<slug>.ravpreset (id "user/<slug>", slug =
//            [a-z0-9-], never a Windows device name), a folder no package touches. The id stays when the preset is renamed (only `name` changes).
// `root` is REAPER's resource path in the extension (GetResourcePath) and a temp folder
// in the tests.
//
// An item embeds the copy of the preset it uses (ItemRules::preset_copy). Its state is
// derived by comparison, never stored:
//   Legacy    the preset on disk has another version than the copy (newer, older, or the id
//             re-created), and the user has not dismissed that version with Keep current.
//             The item changes only on Update.
//   Edited    the item's blocks, options or analyse settings differ from its copy (a
//             project tweak). Save or reload clears it.
//   UpToDate  neither.
//   Unknown   no preset on the item, or the preset is gone (deleted, or a newer extension's).
//
// Pure C++17 (std::filesystem, no REAPER, no _WIN32). No-throw: failures return false with
// a reason in `err`. Host-tested (tests/preset_store_test.cpp).

#pragma once

#include <string>
#include <vector>

#include "rule_record.h"

namespace rav {

enum class PresetState { UpToDate, Edited, Legacy, Unknown };

struct PresetInfo {
    std::string id;       // "factory/footsteps", "user/my-steps"
    std::string name;
    int         version = 1;
    bool        factory = false;
    std::string path;     // the user file (UTF-8), "" for a factory preset
};

constexpr const char kFactoryPrefix[] = "factory/";
constexpr const char kUserPrefix[] = "user/";
constexpr const char kPresetExt[] = ".ravpreset";

bool IsFactoryPresetId(const std::string& id);

// <root>/ReaAnimViewer/Presets (UTF-8).
std::string UserPresetDir(const std::string& root);

// The factory presets (stem order), then the user presets (by name, case-insensitive).
// A user file that does not parse is left out.
std::vector<PresetInfo> ListPresets(const std::string& root);
bool FindPreset(const std::string& root, const std::string& id, PresetInfo* out);
bool LoadPreset(const std::string& root, const std::string& id, PresetData* out, std::string* err);

// Overwrites a user preset with `content`'s options, analyse and blocks; its version goes
// up by one (`new_version`). Refused on a factory preset.
bool SavePreset(const std::string& root, const std::string& id, const PresetData& content, int* new_version,
                std::string* err);
// A new user preset named `name`, version 1. Refused on an empty name or a name already
// used by a preset.
bool SavePresetAs(const std::string& root, const std::string& name, const PresetData& content, std::string* new_id,
                  std::string* err);
// Changes a user preset's name (its id stays). Refused on a factory preset.
bool RenamePreset(const std::string& root, const std::string& id, const std::string& new_name, std::string* err);
// Refused on a factory preset.
bool DeletePreset(const std::string& root, const std::string& id, std::string* err);
// Copies a .ravpreset file into the user folder (a new id; the name gets " (2)"... when
// taken). Refused when the file is not a preset.
bool ImportPreset(const std::string& root, const std::string& src_path, std::string* new_id, std::string* err);
// Copies a preset (factory or user) out to `dest_path`, as written.
bool ExportPreset(const std::string& root, const std::string& id, const std::string& dest_path, std::string* err);

// ---- An item and its preset ------------------------------------------------------------

// The item's state against the preset as it is now (`current` = null when it is gone).
PresetState PresetStateOf(const ItemRules& rules, const PresetInfo* current);
PresetState GetPresetState(const std::string& root, const ItemRules& rules);
// The item's blocks, options or analyse settings differ from its preset copy.
bool IsEdited(const ItemRules& rules);

// Sets the preset up on the item: options, analyse and blocks from the preset, and the
// copy (id, version, name, options, analyse, blocks). A dismissed version is forgotten. Events are kept.
void ApplyPreset(ItemRules& rules, const PresetData& preset);
// Update / reload: ApplyPreset with the preset as it is now. False when it is gone.
bool UpdateFromPreset(const std::string& root, ItemRules& rules, std::string* err);
// Keep current: the item stays as it is and the preset's current version is dismissed
// (no Legacy until a newer one). False when the item has no preset or it is gone.
bool KeepCurrent(const std::string& root, ItemRules& rules, std::string* err);
// After the item's rules were saved as `saved` (Save / Save as): the copy becomes the
// item's blocks, options and analyse settings, so it is no longer edited. The item's
// rules do not change.
void AdoptSavedPreset(ItemRules& rules, const PresetData& saved);

}  // namespace rav
