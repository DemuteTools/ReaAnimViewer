// SPDX-License-Identifier: MIT
//
// See preset_store.h.

#include "preset_store.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

#include "factory_presets.h"  // generated (cmake/FactoryPresets.cmake)

namespace fs = std::filesystem;

namespace rav {
namespace {

bool StartsWith(const std::string& s, const char* pre)
{
    const size_t n = std::char_traits<char>::length(pre);
    return s.size() >= n && s.compare(0, n, pre) == 0;
}

fs::path U8(const std::string& s)
{
    return fs::u8path(s);
}

bool ReadFileText(const fs::path& p, std::string* out)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) return false;
    *out = ss.str();
    return true;
}

// Writes next to the target (flushed and checked), then renames over it: a crash or a
// full disk never leaves half a file.
bool WriteFileText(const fs::path& p, const std::string& text, std::string* err)
{
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (err) *err = "Cannot write " + p.u8string() + ".";
            return false;
        }
        out << text;
        out.flush();
        const bool written = static_cast<bool>(out);
        out.close();
        if (!written || out.fail()) {
            if (err) *err = "Cannot write " + p.u8string() + ".";
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        if (err) *err = "Cannot write " + p.u8string() + ".";
        return false;
    }
    return true;
}

std::string Lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(const std::string& s)
{
    std::string t = s;
    for (char& c : t)
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    const size_t b = t.find_first_not_of(' ');
    if (b == std::string::npos) return "";
    return t.substr(b, t.find_last_not_of(' ') - b + 1);
}

struct Factory {
    PresetInfo  info;
    std::string text;
    PresetData  data;
};

const std::vector<Factory>& Factories()
{
    static const std::vector<Factory> list = [] {
        std::vector<Factory> v;
        size_t               n = 0;
        const FactoryPresetSource* src = FactoryPresetSources(&n);
        for (size_t i = 0; i < n; ++i) {
            Factory f;
            f.text = src[i].text;
            if (!ParsePreset(f.text, &f.data)) continue;
            f.data.id = std::string(kFactoryPrefix) + src[i].stem;  // the location is the id
            f.info.id = f.data.id;
            f.info.name = f.data.name.empty() ? src[i].stem : f.data.name;
            f.info.version = f.data.version;
            f.info.factory = true;
            v.push_back(std::move(f));
        }
        return v;
    }();
    return list;
}

const Factory* FindFactory(const std::string& id)
{
    for (const Factory& f : Factories())
        if (f.info.id == id) return &f;
    return nullptr;
}

// A stem Windows reserves for a device ("aux", "com1"...). `s` lower-case.
bool IsReservedStem(const std::string& s)
{
    static const char* const kNames[] = {"con", "prn", "aux", "nul"};
    for (const char* n : kNames)
        if (s == n) return true;
    if (s.size() == 4 && (s.compare(0, 3, "com") == 0 || s.compare(0, 3, "lpt") == 0) && s[3] >= '1' && s[3] <= '9')
        return true;
    return false;
}

bool IsValidUtf8(const std::string& s)
{
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t              n = 0;
        uint32_t            cp = 0;
        if (c < 0x80) { ++i; continue; }
        if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
        else return false;
        if (i + n >= s.size()) return false;
        for (size_t k = 1; k <= n; ++k) {
            const unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += n + 1;
    }
    return true;
}

// A user id names one file in the folder: "user/<stem>", any stem a file can have (one
// dropped there by hand included), but never a path, a Windows device name or bad UTF-8.
// New ids (Save as, Import) are stricter: Slug.
bool ValidUserId(const std::string& id)
{
    if (!StartsWith(id, kUserPrefix)) return false;
    const std::string stem = id.substr(sizeof(kUserPrefix) - 1);
    if (stem.empty() || stem.back() == '.' || stem.back() == ' ' || !IsValidUtf8(stem)) return false;
    for (char ch : stem) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F || std::strchr("/\\:*?\"<>|", ch)) return false;
    }
    std::string base = Lower(stem.substr(0, stem.find('.')));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    return !IsReservedStem(base);
}

bool IsPresetExt(const fs::path& p)
{
    return Lower(p.extension().u8string()) == kPresetExt;
}

// The file of a user id: <stem>.ravpreset, else an existing <stem> file whose extension
// differs only in case ("x.RAVPRESET"). Throws (u8path) on a root that is not UTF-8:
// callers catch.
fs::path UserFile(const std::string& root, const std::string& id)
{
    const std::string stem = id.substr(sizeof(kUserPrefix) - 1);
    const fs::path    dir = U8(root) / "ReaAnimViewer" / "Presets";
    const fs::path    exact = dir / U8(stem + kPresetExt);
    std::error_code   ec;
    if (fs::exists(exact, ec)) return exact;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        if (IsPresetExt(p) && p.stem().u8string() == stem) return p;
    }
    return exact;
}

bool ReadUser(const std::string& root, const std::string& id, PresetData* data, PresetInfo* info)
{
    if (!ValidUserId(id)) return false;
    const fs::path p = UserFile(root, id);
    std::string    text;
    PresetData     d;
    if (!ReadFileText(p, &text) || !ParsePreset(text, &d)) return false;
    d.id = id;
    if (info) {
        info->id = id;
        info->name = d.name.empty() ? id.substr(sizeof(kUserPrefix) - 1) : d.name;
        info->version = d.version;
        info->factory = false;
        info->path = p.u8string();
    }
    if (data) *data = std::move(d);
    return true;
}

bool NameTaken(const std::string& root, const std::string& name, const std::string& except_id)
{
    const std::string n = Lower(name);
    for (const PresetInfo& p : ListPresets(root))
        if (p.id != except_id && Lower(p.name) == n) return true;
    return false;
}

std::string Slug(const std::string& name)
{
    std::string s;
    for (char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) && c < 0x80) s += static_cast<char>(std::tolower(c));
        else if (!s.empty() && s.back() != '-') s += '-';
    }
    if (s.size() > 48) s.resize(48);
    while (!s.empty() && s.back() == '-') s.pop_back();
    if (s.empty()) return "preset";
    if (IsReservedStem(s)) s += "-preset";
    return s;
}

std::string FreeUserId(const std::string& root, const std::string& name)
{
    const std::string base = Slug(name);
    std::error_code   ec;
    for (int n = 1;; ++n) {
        const std::string id = std::string(kUserPrefix) + base + (n == 1 ? "" : "-" + std::to_string(n));
        if (!fs::exists(UserFile(root, id), ec)) return id;
    }
}

bool Refuse(std::string* err, const std::string& why)
{
    if (err) *err = why;
    return false;
}

}  // namespace

bool IsFactoryPresetId(const std::string& id)
{
    return StartsWith(id, kFactoryPrefix);
}

std::string UserPresetDir(const std::string& root)
{
    try {
        return (U8(root) / "ReaAnimViewer" / "Presets").u8string();
    } catch (...) {
        return "";  // a root that is not UTF-8: no user folder
    }
}

std::vector<PresetInfo> ListPresets(const std::string& root)
{
    std::vector<PresetInfo> out;
    try {
        for (const Factory& f : Factories()) out.push_back(f.info);
        std::vector<PresetInfo> user;
        std::error_code         ec;
        const fs::path          dir = U8(root) / "ReaAnimViewer" / "Presets";
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            try {  // one odd file name never hides the others
                const fs::path& p = it->path();
                if (!IsPresetExt(p)) continue;
                PresetInfo        info;
                const std::string id = std::string(kUserPrefix) + p.stem().u8string();
                bool              dup = false;
                for (const PresetInfo& u : user) dup = dup || u.id == id;
                if (!dup && ReadUser(root, id, nullptr, &info)) user.push_back(info);
            } catch (...) {
            }
        }
        std::sort(user.begin(), user.end(), [](const PresetInfo& a, const PresetInfo& b) {
            const std::string la = Lower(a.name), lb = Lower(b.name);
            return la != lb ? la < lb : a.id < b.id;
        });
        out.insert(out.end(), user.begin(), user.end());
    } catch (...) {
    }
    return out;
}

bool FindPreset(const std::string& root, const std::string& id, PresetInfo* out)
{
    try {
        if (const Factory* f = FindFactory(id)) {
            if (out) *out = f->info;
            return true;
        }
        return ReadUser(root, id, nullptr, out);
    } catch (...) {
        return false;
    }
}

bool LoadPreset(const std::string& root, const std::string& id, PresetData* out, std::string* err)
{
    try {
        if (const Factory* f = FindFactory(id)) {
            if (out) *out = f->data;
            return true;
        }
        if (ReadUser(root, id, out, nullptr)) return true;
    } catch (...) {
    }
    return Refuse(err, "The preset is gone or cannot be read.");
}

bool SavePreset(const std::string& root, const std::string& id, const PresetData& content, int* new_version,
                std::string* err)
{
    if (IsFactoryPresetId(id)) return Refuse(err, "A factory preset cannot be overwritten. Use Save as.");
    try {
        PresetData cur;
        if (!ReadUser(root, id, &cur, nullptr)) return Refuse(err, "The preset is gone or cannot be read.");
        PresetData d = cur;  // keeps its name and what this version does not understand
        d.id = id;
        d.version = cur.version + 1;
        d.options = content.options;
        d.analyse = content.analyse;
        d.blocks = content.blocks;
        if (!WriteFileText(UserFile(root, id), SerializePreset(d), err)) return false;
        if (new_version) *new_version = d.version;
        return true;
    } catch (...) {
        return Refuse(err, "The preset could not be saved.");
    }
}

bool SavePresetAs(const std::string& root, const std::string& name, const PresetData& content, std::string* new_id,
                  std::string* err)
{
    try {
        const std::string n = Trim(name);
        if (n.empty()) return Refuse(err, "Type a name for the preset.");
        if (NameTaken(root, n, "")) return Refuse(err, "A preset named \"" + n + "\" already exists.");
        PresetData d;
        d.id = FreeUserId(root, n);
        d.version = 1;
        d.name = n;
        d.options = content.options;
        d.analyse = content.analyse;
        d.blocks = content.blocks;
        if (!WriteFileText(UserFile(root, d.id), SerializePreset(d), err)) return false;
        if (new_id) *new_id = d.id;
        return true;
    } catch (...) {
        return Refuse(err, "The preset could not be saved.");
    }
}

bool RenamePreset(const std::string& root, const std::string& id, const std::string& new_name, std::string* err)
{
    if (IsFactoryPresetId(id)) return Refuse(err, "A factory preset cannot be renamed.");
    try {
        const std::string n = Trim(new_name);
        if (n.empty()) return Refuse(err, "Type a name for the preset.");
        PresetData d;
        if (!ReadUser(root, id, &d, nullptr)) return Refuse(err, "The preset is gone or cannot be read.");
        if (NameTaken(root, n, id)) return Refuse(err, "A preset named \"" + n + "\" already exists.");
        d.name = n;
        return WriteFileText(UserFile(root, id), SerializePreset(d), err);
    } catch (...) {
        return Refuse(err, "The preset could not be renamed.");
    }
}

bool DeletePreset(const std::string& root, const std::string& id, std::string* err)
{
    if (IsFactoryPresetId(id)) return Refuse(err, "A factory preset cannot be deleted.");
    if (!ValidUserId(id)) return Refuse(err, "The preset is gone.");
    try {
        std::error_code ec;
        if (!fs::remove(UserFile(root, id), ec) || ec) return Refuse(err, "The preset could not be deleted.");
        return true;
    } catch (...) {
        return Refuse(err, "The preset could not be deleted.");
    }
}

bool ImportPreset(const std::string& root, const std::string& src_path, std::string* new_id, std::string* err)
{
    try {
        std::string text;
        PresetData  d;
        if (!ReadFileText(U8(src_path), &text)) return Refuse(err, "Cannot read " + src_path + ".");
        if (!ParsePreset(text, &d)) return Refuse(err, "This file is not a ReaAnimViewer preset.");
        std::string name = Trim(d.name);
        if (name.empty()) name = U8(src_path).stem().u8string();
        if (NameTaken(root, name, "")) {
            for (int n = 2;; ++n) {
                const std::string cand = name + " (" + std::to_string(n) + ")";
                if (!NameTaken(root, cand, "")) {
                    name = cand;
                    break;
                }
            }
        }
        d.name = name;
        d.id = FreeUserId(root, name);
        if (!WriteFileText(UserFile(root, d.id), SerializePreset(d), err)) return false;
        if (new_id) *new_id = d.id;
        return true;
    } catch (...) {
        return Refuse(err, "The preset could not be imported.");
    }
}

bool ExportPreset(const std::string& root, const std::string& id, const std::string& dest_path, std::string* err)
{
    try {
        std::string text;
        if (const Factory* f = FindFactory(id)) {
            text = f->text;
        } else if (!ValidUserId(id) || !ReadFileText(UserFile(root, id), &text)) {
            return Refuse(err, "The preset is gone or cannot be read.");
        }
        return WriteFileText(U8(dest_path), text, err);
    } catch (...) {
        return Refuse(err, "The preset could not be exported.");
    }
}

// ---- An item and its preset ------------------------------------------------------------

bool IsEdited(const ItemRules& rules)
{
    const PresetCopy& c = rules.preset_copy;
    return rules.has_preset && (!BlocksEqual(rules.blocks, c.blocks) || !OptionsEqual(rules.options, c.options) ||
                                !AnalyseEqual(rules.analyse, c.analyse));
}

PresetState PresetStateOf(const ItemRules& rules, const PresetInfo* current)
{
    if (!rules.has_preset || !current) return PresetState::Unknown;
    const PresetCopy& c = rules.preset_copy;
    // Any other version (newer, older, or the id re-created) is not the one copied.
    if (current->version != c.version && current->version != c.kept_version) return PresetState::Legacy;
    if (IsEdited(rules)) return PresetState::Edited;
    return PresetState::UpToDate;
}

PresetState GetPresetState(const std::string& root, const ItemRules& rules)
{
    if (!rules.has_preset) return PresetState::Unknown;
    PresetInfo info;
    return PresetStateOf(rules, FindPreset(root, rules.preset_copy.id, &info) ? &info : nullptr);
}

void ApplyPreset(ItemRules& rules, const PresetData& preset)
{
    rules.options = preset.options;
    rules.analyse = preset.analyse;
    rules.blocks = preset.blocks;
    rules.has_preset = true;
    rules.preset_copy.id = preset.id;
    rules.preset_copy.version = preset.version;
    rules.preset_copy.kept_version = 0;
    rules.preset_copy.name = preset.name;
    rules.preset_copy.options = preset.options;
    rules.preset_copy.analyse = preset.analyse;
    rules.preset_copy.blocks = preset.blocks;
}

bool UpdateFromPreset(const std::string& root, ItemRules& rules, std::string* err)
{
    if (!rules.has_preset) return Refuse(err, "The item has no preset.");
    PresetData d;
    if (!LoadPreset(root, rules.preset_copy.id, &d, err)) return false;
    ApplyPreset(rules, d);
    return true;
}

bool KeepCurrent(const std::string& root, ItemRules& rules, std::string* err)
{
    if (!rules.has_preset) return Refuse(err, "The item has no preset.");
    PresetInfo info;
    if (!FindPreset(root, rules.preset_copy.id, &info)) return Refuse(err, "The preset is gone or cannot be read.");
    rules.preset_copy.kept_version = info.version;
    return true;
}

void AdoptSavedPreset(ItemRules& rules, const PresetData& saved)
{
    rules.has_preset = true;
    rules.preset_copy.id = saved.id;
    rules.preset_copy.version = saved.version;
    rules.preset_copy.kept_version = 0;
    rules.preset_copy.name = saved.name;
    rules.preset_copy.options = rules.options;
    rules.preset_copy.analyse = rules.analyse;
    rules.preset_copy.blocks = rules.blocks;
}

}  // namespace rav
