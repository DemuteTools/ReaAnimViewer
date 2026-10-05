// SPDX-License-Identifier: MIT
//
// See preset_menu_model.h.

#include "preset_menu_model.h"

#include <cstddef>

namespace rav {
namespace {

char LowerAscii(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string Lower(const std::string& s)
{
    std::string out = s;
    for (char& c : out) c = LowerAscii(c);
    return out;
}

bool IsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

}  // namespace

std::vector<int> PresetMenuRows(const std::vector<PresetInfo>& presets, const std::string& query,
                                PresetCascade cascade)
{
    std::vector<int> rows;
    if (!query.empty()) {
        const std::string q = Lower(query);
        for (int pass = 0; pass < 2; ++pass) {
            const bool factory = pass == 0;
            for (size_t i = 0; i < presets.size(); ++i)
                if (presets[i].factory == factory && Lower(presets[i].name).find(q) != std::string::npos)
                    rows.push_back(static_cast<int>(i));
        }
        return rows;
    }
    if (cascade == PresetCascade::None) return rows;
    const bool factory = cascade == PresetCascade::Factory;
    for (size_t i = 0; i < presets.size(); ++i)
        if (presets[i].factory == factory) rows.push_back(static_cast<int>(i));
    return rows;
}

std::string StepPresetSelection(const std::vector<std::string>& rows, const std::string& sel, int dir)
{
    if (rows.empty()) return std::string();
    int at = -1;
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i] == sel) at = static_cast<int>(i);
    if (at < 0) return dir < 0 ? rows.back() : rows.front();
    const int last = static_cast<int>(rows.size()) - 1;
    int to = at + (dir < 0 ? -1 : dir > 0 ? 1 : 0);
    if (to < 0) to = 0;
    if (to > last) to = last;
    return rows[static_cast<size_t>(to)];
}

int CountPresets(const std::vector<PresetInfo>& presets, bool factory)
{
    int n = 0;
    for (const PresetInfo& p : presets)
        if (p.factory == factory) ++n;
    return n;
}

std::string TrimPresetName(const std::string& name)
{
    size_t a = 0, b = name.size();
    while (a < b && IsSpace(name[a])) ++a;
    while (b > a && IsSpace(name[b - 1])) --b;
    return name.substr(a, b - a);
}

bool SamePresetName(const std::string& a, const std::string& b)
{
    return Lower(a) == Lower(b);
}

NameCheck CheckSaveAsName(const std::vector<PresetInfo>& presets, const std::string& name, PresetInfo* clash)
{
    const std::string n = TrimPresetName(name);
    if (n.empty()) return NameCheck::Empty;
    for (const PresetInfo& p : presets) {
        if (p.factory || !SamePresetName(p.name, n)) continue;
        if (clash) *clash = p;
        return NameCheck::UserClash;
    }
    return NameCheck::Ok;
}

NameCheck CheckRenameName(const std::string& name)
{
    return TrimPresetName(name).empty() ? NameCheck::Empty : NameCheck::Ok;
}

std::string ExportFileName(const std::string& preset_name)
{
    std::string base;
    for (char c : TrimPresetName(preset_name)) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool bad = u < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' ||
                         c == '|' || c == '?' || c == '*';
        base += bad ? '_' : c;
    }
    // Windows drops trailing dots and spaces from a file name.
    while (!base.empty() && (base.back() == '.' || base.back() == ' ')) base.pop_back();
    if (base.empty()) base = "preset";
    return base + kPresetExt;
}

const char* PresetMenuHint(PresetMenuMode mode)
{
    switch (mode) {
    case PresetMenuMode::List: return "Double-click or Enter loads the selected preset.";
    case PresetMenuMode::Naming: return "Enter saves, Esc cancels. An existing name asks first.";
    case PresetMenuMode::Renaming: return "Enter renames, Esc cancels.";
    case PresetMenuMode::Confirm: return "";
    }
    return "";
}

std::string SearchPlaceholder(int count)
{
    return "Search " + std::to_string(count) + (count == 1 ? " preset" : " presets");
}

std::string CascadeLabel(bool factory, int count)
{
    return std::string(factory ? "Factory" : "User") + " (" + std::to_string(count) + ")";
}

const char* EmptyCascadeText(bool factory)
{
    return factory ? "No factory presets." : "No user presets yet. Save as writes the first one here.";
}

std::string FactorySaveNote(const std::string& name)
{
    return name + " is a factory preset: Save as writes your version to User.";
}

std::string EmptyNameStatus(bool renaming)
{
    return renaming ? "A preset needs a name. Nothing has been renamed."
                    : "A preset needs a name. Nothing has been saved.";
}

std::string CancelStatus(PresetMenuMode mode, PresetConfirm confirm)
{
    if (mode == PresetMenuMode::Renaming) return "Nothing has been renamed.";
    if (mode == PresetMenuMode::Confirm && confirm == PresetConfirm::Delete) return "Nothing has been deleted.";
    return "Nothing has been saved.";
}

std::string ConfirmQuestion(PresetConfirm kind, const std::string& name)
{
    return (kind == PresetConfirm::Delete ? "Delete " : "Overwrite ") + name + "?";
}

std::string ConfirmDetail(PresetConfirm kind, bool clash)
{
    if (kind == PresetConfirm::Delete) return "Its file leaves the folder below. Ctrl+Z does not bring it back.";
    return std::string(clash ? "A preset already has this name. " : "") +
           "Its previous content is replaced. Ctrl+Z does not bring it back.";
}

const char* ConfirmAction(PresetConfirm kind)
{
    return kind == PresetConfirm::Delete ? "Delete" : "Overwrite";
}

const char* ConfirmHint(PresetConfirm kind)
{
    return kind == PresetConfirm::Delete ? "Enter deletes, Esc cancels." : "Enter overwrites, Esc cancels.";
}

std::string LegacyText(int disk_version)
{
    return "Legacy \xC2\xB7 v" + std::to_string(disk_version) + " is installed";
}

std::string KeepLabel(int copy_version)
{
    return "Keep v" + std::to_string(copy_version);
}

std::string KeptTag(int copy_version)
{
    return "kept v" + std::to_string(copy_version);
}

bool ShowKeptTag(bool has_preset, bool gone, int kept_version, int disk_version)
{
    return has_preset && !gone && kept_version != 0 && kept_version == disk_version;
}

}  // namespace rav
