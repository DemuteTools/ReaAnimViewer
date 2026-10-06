// SPDX-License-Identifier: MIT
//
// See role_map_store.h.

#include "role_map_store.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace rav {
namespace {

constexpr const char kMagic[] = "RAVROLES";

bool IsKeyChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// "key=value" fields; a bare token joins the previous value; `bone` runs to the end.
std::vector<std::pair<std::string, std::string>> Tokenize(const std::string& rest)
{
    std::vector<std::pair<std::string, std::string>> f;
    size_t                                           pos = 0;
    while (pos <= rest.size()) {
        size_t sp = rest.find(' ', pos);
        if (sp == std::string::npos) sp = rest.size();
        const std::string tok = rest.substr(pos, sp - pos);
        const size_t      eq = tok.find('=');
        bool              key_ok = eq != std::string::npos && eq > 0;
        for (size_t i = 0; key_ok && i < eq; ++i) key_ok = IsKeyChar(tok[i]);
        if (key_ok) {
            const std::string key = tok.substr(0, eq);
            if (key == "bone") {
                f.push_back({key, rest.substr(pos + eq + 1)});
                break;
            }
            f.push_back({key, tok.substr(eq + 1)});
        } else if (!f.empty()) {
            f.back().second += ' ' + tok;
        } else {
            f.push_back({"", tok});
        }
        if (sp >= rest.size()) break;
        pos = sp + 1;
    }
    return f;
}

std::string CleanName(const std::string& s)
{
    std::string t = s;
    for (char& c : t)
        if (c == '\r' || c == '\n') c = ' ';
    return t;
}

// The stored bone of one role (`found` = a map line exists for it).
const RoleMapLine* FindLine(const RoleMapFile& file, const std::string& skeleton, const std::string& role_key)
{
    const RoleMapLine* hit = nullptr;
    for (const RoleMapLine& l : file.lines)
        if (l.is_map && l.Get("skeleton") == skeleton && l.Get("role") == role_key) hit = &l;  // the last one wins
    return hit;
}

enum class FileState {
    Absent,      // no file yet
    Ok,          // read and parsed
    NotRoles,    // read, but not a roles file: moved aside before the first write
    Unreadable,  // exists but cannot be read: never replaced
};

FileState ReadText(const fs::path& p, std::string* out)
{
    std::error_code ec;
    const fs::file_status st = fs::status(p, ec);
    if (st.type() == fs::file_type::not_found) return FileState::Absent;
    if (ec || st.type() != fs::file_type::regular) return FileState::Unreadable;
    std::ifstream in(p, std::ios::binary);
    if (!in) return FileState::Unreadable;
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) return FileState::Unreadable;
    *out = ss.str();
    return FileState::Ok;
}

RoleMapFile LoadFile(const std::string& root, FileState* state)
{
    RoleMapFile f;
    std::string text;
    *state = ReadText(fs::u8path(RoleMapPath(root)), &text);
    if (*state == FileState::Ok && !ParseRoleMap(text, &f)) {
        *state = FileState::NotRoles;
        f = RoleMapFile{};
    }
    return f;
}

bool SaveFile(const std::string& root, const RoleMapFile& f, FileState state, std::string* err)
{
    const fs::path p = fs::u8path(RoleMapPath(root));
    auto           fail = [&](const std::string& why) {
        if (err) *err = why;
        return false;
    };
    if (state == FileState::Unreadable)
        return fail("Cannot read " + p.u8string() + ": the role mapping was not changed.");
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (state == FileState::NotRoles) {
        // Keep what we could not read under a free name; never overwrite an older one.
        fs::path bad = p;
        bad += ".bad";
        for (int n = 2; fs::exists(bad, ec) || ec; ++n) {
            if (n > 999) return fail("Cannot keep " + p.u8string() + " aside: the role mapping was not changed.");
            bad = p;
            bad += ".bad" + std::to_string(n);
        }
        fs::rename(p, bad, ec);
        if (ec) return fail("Cannot keep " + p.u8string() + " aside: the role mapping was not changed.");
    }
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (out) out << SerializeRoleMap(f);
        if (out) out.flush();
        const bool written = static_cast<bool>(out);
        out.close();
        if (!written || out.fail()) {
            fs::remove(tmp, ec);
            return fail("Cannot write " + p.u8string() + ".");
        }
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return fail("Cannot write " + p.u8string() + ".");
    }
    return true;
}

}  // namespace

std::string RoleMapLine::Get(const char* key) const
{
    for (const auto& kv : fields)
        if (kv.first == key) return kv.second;
    return "";
}

std::string SkeletonKey(const std::vector<std::string>& bone_names)
{
    std::vector<std::string> norm;
    norm.reserve(bone_names.size());
    for (const std::string& n : bone_names) norm.push_back(NormalizeBoneName(n));
    std::sort(norm.begin(), norm.end());
    uint64_t h = 1469598103934665603ull;  // FNV-1a 64
    auto     add = [&](unsigned char c) {
        h ^= c;
        h *= 1099511628211ull;
    };
    for (const std::string& n : norm) {
        for (char c : n) add(static_cast<unsigned char>(c));
        add('\n');
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

std::string RoleMapPath(const std::string& root)
{
    return (fs::u8path(root) / "ReaAnimViewer" / "roles.txt").u8string();
}

bool ParseRoleMap(const std::string& text_in, RoleMapFile* out)
{
    if (!out) return false;
    try {
        std::string text = text_in.substr(0, text_in.find('\0'));
        if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
        std::vector<std::string> lines;
        for (size_t pos = 0; pos < text.size();) {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();
            std::string l = text.substr(pos, nl - pos);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
            pos = nl + 1;
        }
        size_t first = 0;
        while (first < lines.size() && lines[first].find_first_not_of(" \t") == std::string::npos) ++first;
        if (first >= lines.size()) return false;
        const std::string& h = lines[first];
        const size_t       ml = sizeof(kMagic) - 1;
        if (h.compare(0, ml, kMagic) != 0 || (h.size() > ml && h[ml] != ' ')) return false;
        RoleMapFile f;
        // "RAVROLES <n><rest>": the rest of the header line is kept as written.
        size_t b = ml;
        while (b < h.size() && h[b] == ' ') ++b;
        size_t e = b;
        while (e < h.size() && h[e] >= '0' && h[e] <= '9') ++e;
        int v = 1;
        if (e > b && e - b < 9) v = std::atoi(h.substr(b, e - b).c_str());
        else e = b;
        f.version = v < 1 ? 1 : v;
        f.header_rest = h.substr(e);
        if (e == b && !f.header_rest.empty()) f.header_rest = ' ' + f.header_rest;
        for (size_t i = first + 1; i < lines.size(); ++i) {
            RoleMapLine l;
            if (lines[i].compare(0, 4, "map ") == 0) {
                l.is_map = true;
                l.fields = Tokenize(lines[i].substr(4));
                if (l.Get("skeleton").empty() || l.Get("role").empty()) {
                    l = RoleMapLine{};
                    l.raw = lines[i];
                }
            } else {
                l.raw = lines[i];
            }
            f.lines.push_back(std::move(l));
        }
        *out = std::move(f);
        return true;
    } catch (...) {
        return false;
    }
}

std::string SerializeRoleMap(const RoleMapFile& file)
{
    std::string out = std::string(kMagic) + ' ' + std::to_string(file.version < 1 ? 1 : file.version) + file.header_rest + '\n';
    for (const RoleMapLine& l : file.lines) {
        if (!l.is_map) {
            out += l.raw;
        } else {
            out += "map";
            for (const auto& kv : l.fields) {
                out += ' ';
                if (!kv.first.empty()) out += kv.first + '=';
                out += kv.second;
            }
        }
        out += '\n';
    }
    return out;
}

std::vector<int> ResolveRoleMapping(const RoleMapFile& file, const std::vector<std::string>& bone_names,
                                    std::vector<char>* stored)
{
    std::vector<int> map = GuessRoleMapping(bone_names);
    if (stored) stored->assign(map.size(), 0);
    const std::string skel = SkeletonKey(bone_names);
    for (int r = 0; r < static_cast<int>(Role::Count); ++r) {
        const RoleMapLine* l = FindLine(file, skel, RoleKey(static_cast<Role>(r)));
        if (!l) continue;
        if (stored) (*stored)[r] = 1;
        const std::string bone = l->Get("bone");
        int               idx = -1;
        if (!bone.empty()) {
            for (size_t b = 0; b < bone_names.size() && idx < 0; ++b)
                if (bone_names[b] == bone) idx = static_cast<int>(b);
            const std::string n = NormalizeBoneName(bone);
            for (size_t b = 0; b < bone_names.size() && idx < 0; ++b)
                if (NormalizeBoneName(bone_names[b]) == n) idx = static_cast<int>(b);
        }
        map[r] = idx;
    }
    return map;
}

void SetRoleInFile(RoleMapFile& file, const std::string& skeleton, const std::string& role_key,
                   const std::string& bone_name)
{
    for (auto it = file.lines.rbegin(); it != file.lines.rend(); ++it) {
        if (!it->is_map || it->Get("skeleton") != skeleton || it->Get("role") != role_key) continue;
        bool set = false;
        for (auto& kv : it->fields)
            if (kv.first == "bone") {
                kv.second = CleanName(bone_name);
                set = true;
            }
        if (!set) it->fields.push_back({"bone", CleanName(bone_name)});
        return;
    }
    RoleMapLine l;
    l.is_map = true;
    l.fields = {{"skeleton", skeleton}, {"role", role_key}, {"bone", CleanName(bone_name)}};
    file.lines.push_back(std::move(l));
}

bool ClearRoleInFile(RoleMapFile& file, const std::string& skeleton, const std::string& role_key)
{
    const size_t before = file.lines.size();
    file.lines.erase(std::remove_if(file.lines.begin(), file.lines.end(),
                                    [&](const RoleMapLine& l) {
                                        return l.is_map && l.Get("skeleton") == skeleton && l.Get("role") == role_key;
                                    }),
                     file.lines.end());
    return file.lines.size() != before;
}

std::vector<int> GetRoleMapping(const std::string& root, const std::vector<std::string>& bone_names,
                                std::vector<char>* stored)
{
    try {
        FileState st;
        return ResolveRoleMapping(LoadFile(root, &st), bone_names, stored);
    } catch (...) {
        if (stored) stored->assign(static_cast<size_t>(Role::Count), 0);
        return GuessRoleMapping(bone_names);
    }
}

bool SetRoleBone(const std::string& root, const std::vector<std::string>& bone_names, Role role,
                 const std::string& bone_name, std::string* err)
{
    try {
        FileState   st;
        RoleMapFile f = LoadFile(root, &st);
        SetRoleInFile(f, SkeletonKey(bone_names), RoleKey(role), bone_name);
        return SaveFile(root, f, st, err);
    } catch (...) {
        if (err) *err = "The role mapping could not be saved.";
        return false;
    }
}

bool ClearRole(const std::string& root, const std::vector<std::string>& bone_names, Role role, std::string* err)
{
    try {
        FileState   st;
        RoleMapFile f = LoadFile(root, &st);
        if (st == FileState::Unreadable) return SaveFile(root, f, st, err);  // refused, with the reason
        if (!ClearRoleInFile(f, SkeletonKey(bone_names), RoleKey(role)) && st != FileState::NotRoles) return true;
        return SaveFile(root, f, st, err);
    } catch (...) {
        if (err) *err = "The role mapping could not be saved.";
        return false;
    }
}

RoleMapStatus GetRoleMapStatus(const std::string& root)
{
    try {
        FileState st;
        LoadFile(root, &st);
        switch (st) {
        case FileState::Absent: return RoleMapStatus::Absent;
        case FileState::Ok: return RoleMapStatus::Ok;
        case FileState::NotRoles: return RoleMapStatus::NotRolesFile;
        default: return RoleMapStatus::Unreadable;
        }
    } catch (...) {
        return RoleMapStatus::Unreadable;
    }
}

std::vector<StoredRoleEntry> GetStoredRoles(const std::string& root, const std::vector<std::string>& bone_names)
{
    std::vector<StoredRoleEntry> out(static_cast<size_t>(Role::Count));
    try {
        FileState         st;
        const RoleMapFile f = LoadFile(root, &st);
        const std::string skel = SkeletonKey(bone_names);
        for (int r = 0; r < static_cast<int>(Role::Count); ++r) {
            const RoleMapLine* l = FindLine(f, skel, RoleKey(static_cast<Role>(r)));
            if (!l) continue;
            out[static_cast<size_t>(r)].stored = true;
            out[static_cast<size_t>(r)].bone = l->Get("bone");
        }
    } catch (...) {
        out.assign(static_cast<size_t>(Role::Count), StoredRoleEntry{});
    }
    return out;
}

StoredRoleEntry GetStoredRole(const std::string& root, const std::vector<std::string>& bone_names, Role role)
{
    const int r = static_cast<int>(role);
    if (r < 0 || r >= static_cast<int>(Role::Count)) return StoredRoleEntry{};
    return GetStoredRoles(root, bone_names)[static_cast<size_t>(r)];
}

bool RestoreRole(const std::string& root, const std::vector<std::string>& bone_names, Role role,
                 const StoredRoleEntry& entry, std::string* err)
{
    return entry.stored ? SetRoleBone(root, bone_names, role, entry.bone, err)
                        : ClearRole(root, bone_names, role, err);
}

bool ApplyRoleChange(const std::string& root, const std::vector<std::string>& bone_names, Role role,
                     RoleChangeKind kind, const std::string& bone, StoredRoleEntry* prev_out, bool* changed,
                     std::string* err)
{
    if (changed) *changed = false;
    const StoredRoleEntry prev = GetStoredRole(root, bone_names, role);
    if (prev_out) *prev_out = prev;
    const bool ok = kind == RoleChangeKind::Auto
                        ? ClearRole(root, bone_names, role, err)
                        : SetRoleBone(root, bone_names, role, kind == RoleChangeKind::None ? std::string() : bone, err);
    if (ok && changed) *changed = !(GetStoredRole(root, bone_names, role) == prev);
    return ok;
}

}  // namespace rav
