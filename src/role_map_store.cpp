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
#include <iterator>
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

// "key=value" fields; a bare token joins the previous value; the free-text field (`bone`
// on a map line, `name` on a role line) runs to the end.
std::vector<std::pair<std::string, std::string>> Tokenize(const std::string& rest, const char* free_key)
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
            if (key == free_key) {
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

// Writes `text` to `p` through a .tmp file renamed over it.
bool WriteTextAtomic(const fs::path& p, const std::string& text, std::string* err)
{
    std::error_code ec;
    fs::path        tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (out) out << text;
        if (out) out.flush();
        const bool written = static_cast<bool>(out);
        out.close();
        if (!written || out.fail()) {
            fs::remove(tmp, ec);
            if (err) *err = "Cannot write " + p.u8string() + ".";
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
    return WriteTextAtomic(p, SerializeRoleMap(f), err);
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
                l.fields = Tokenize(lines[i].substr(4), "bone");
                if (l.Get("skeleton").empty() || l.Get("role").empty()) {
                    l = RoleMapLine{};
                    l.raw = lines[i];
                }
            } else if (lines[i].compare(0, 5, "role ") == 0) {
                l.is_role = true;
                l.fields = Tokenize(lines[i].substr(5), "name");
                if (l.Get("key").empty()) {
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
        if (!l.is_map && !l.is_role) {
            out += l.raw;
        } else {
            out += l.is_map ? "map" : "role";
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
    // An existing line is edited (its fields kept) and moved to the end: the file's order is the
    // order of the picks, newest last (the learned guess reads it backwards).
    for (auto it = file.lines.rbegin(); it != file.lines.rend(); ++it) {
        if (!it->is_map || it->Get("skeleton") != skeleton || it->Get("role") != role_key) continue;
        RoleMapLine l = std::move(*it);
        file.lines.erase(std::next(it).base());
        bool set = false;
        for (auto& kv : l.fields)
            if (kv.first == "bone") {
                kv.second = CleanName(bone_name);
                set = true;
            }
        if (!set) l.fields.push_back({"bone", CleanName(bone_name)});
        file.lines.push_back(std::move(l));
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

namespace {

bool IsBuiltinKey(const std::string& key)
{
    return RoleFromKey(key, nullptr);
}

// The bone of this rig a stored bone text names: exact, else the same NormalizeBoneName. -1 = none.
int FindBoneIndex(const std::vector<std::string>& bone_names, const std::string& bone)
{
    if (bone.empty()) return -1;
    for (size_t b = 0; b < bone_names.size(); ++b)
        if (bone_names[b] == bone) return static_cast<int>(b);
    const std::string n = NormalizeBoneName(bone);
    for (size_t b = 0; b < bone_names.size(); ++b)
        if (NormalizeBoneName(bone_names[b]) == n) return static_cast<int>(b);
    return -1;
}

std::string LowerAscii(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// A typed role name as stored: line breaks read as spaces, runs of spaces as one, trimmed,
// ASCII lower-cased (like the built-in roles: "Sword Tip" -> "sword tip").
std::string CleanRoleName(const std::string& typed)
{
    std::string out;
    bool        space = false;
    for (char c : typed) {
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            space = true;
            continue;
        }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return LowerAscii(out);
}

// Checks a name for a role (`self_key` = the role renamed, "" for a new one). The key and the
// cleaned name on success, else the reason.
bool CheckRoleName(const RoleMapFile& f, const std::string& typed, const std::string& self_key, std::string* key_out,
                   std::string* name_out, std::string* err)
{
    auto fail = [&](const std::string& why) {
        if (err) *err = why;
        return false;
    };
    const std::string name = CleanRoleName(typed);
    if (name.empty()) return fail("Type a name for the role.");
    // 10-3f: a role file is CSV (',' or ';' separated): a name never holds either.
    if (name.find_first_of(",;") != std::string::npos) return fail("A role name cannot contain , or ;");
    const std::string key = RoleKeyFromName(name);
    if (key.empty()) return fail("\"" + name + "\": a role name needs a letter or a digit (a-z, 0-9).");
    const std::string low = LowerAscii(name);
    for (int r = 0; r < static_cast<int>(Role::Count); ++r) {
        const Role role = static_cast<Role>(r);
        if (key == RoleKey(role) || low == LowerAscii(RoleName(role)) || low == LowerAscii(RoleShortLabel(role)))
            return fail("\"" + name + "\" is already a role (" + RoleName(role) + ").");
    }
    for (const CustomRole& c : CustomRolesInFile(f)) {
        if (c.key == self_key) continue;
        if (low == LowerAscii(c.name) || key == RoleKeyFromName(c.name))
            return fail("\"" + name + "\" is already a role (" + c.name + ").");
        if (key == c.key)  // the role was renamed since: its key keeps its first name
            return fail("\"" + name + "\" is taken: it was the first name of \"" + c.name + "\".");
    }
    if (key_out) *key_out = self_key.empty() ? key : self_key;
    if (name_out) *name_out = name;
    return true;
}

// Sets the name of a role's definition (the last line wins), adding one when there is none:
// before the custom role at `at_index` in the list order (-1 / past the end = at the end).
void SetRoleDefinition(RoleMapFile& f, const std::string& key, const std::string& name, int at_index = -1)
{
    for (auto it = f.lines.rbegin(); it != f.lines.rend(); ++it) {
        if (!it->is_role || it->Get("key") != key) continue;
        bool set = false;
        for (auto& kv : it->fields)
            if (kv.first == "name") {
                kv.second = name;
                set = true;
            }
        if (!set) it->fields.push_back({"name", name});
        return;
    }
    RoleMapLine l;
    l.is_role = true;
    l.fields = {{"key", key}, {"name", name}};
    if (at_index >= 0) {
        const std::vector<CustomRole> list = CustomRolesInFile(f);
        if (at_index < static_cast<int>(list.size())) {
            const std::string& before = list[static_cast<size_t>(at_index)].key;
            for (auto it = f.lines.begin(); it != f.lines.end(); ++it)
                if (it->is_role && it->Get("key") == before) {
                    f.lines.insert(it, std::move(l));
                    return;
                }
        }
    }
    f.lines.push_back(std::move(l));
}

std::vector<std::string> RoleKeysOf(const std::vector<CustomRole>& roles)
{
    std::vector<std::string> keys;
    for (const CustomRole& c : roles) keys.push_back(c.key);
    return keys;
}

}  // namespace

std::vector<CustomRole> CustomRolesInFile(const RoleMapFile& file)
{
    std::vector<CustomRole> out;
    for (const RoleMapLine& l : file.lines) {
        if (!l.is_role) continue;
        const std::string key = l.Get("key");
        if (key.empty() || IsBuiltinKey(key)) continue;  // a built-in key is never redefined
        bool found = false;
        for (CustomRole& c : out)
            if (c.key == key) {
                c.name = l.Get("name");  // the last line wins
                found = true;
            }
        if (!found) out.push_back({key, l.Get("name")});
    }
    for (CustomRole& c : out)
        if (c.name.empty()) c.name = RoleNameFromKey(c.key);
    return out;
}

int LearnedRoleGuess(const RoleMapFile& file, const std::vector<std::string>& bone_names, const std::string& role_key)
{
    if (bone_names.empty() || IsBuiltinKey(role_key)) return -1;
    const std::string skel = SkeletonKey(bone_names);
    for (auto it = file.lines.rbegin(); it != file.lines.rend(); ++it) {
        if (!it->is_map || it->Get("role") != role_key || it->Get("skeleton") == skel) continue;
        const std::string bone = it->Get("bone");
        if (bone.empty()) continue;
        const std::string n = NormalizeBoneName(bone);
        for (size_t b = 0; b < bone_names.size(); ++b)
            if (NormalizeBoneName(bone_names[b]) == n) return static_cast<int>(b);
    }
    return -1;
}

int ResolveRoleKey(const RoleMapFile& file, const std::vector<std::string>& bone_names, const std::string& role_key,
                   bool* stored)
{
    if (stored) *stored = false;
    const RoleMapLine* l = FindLine(file, SkeletonKey(bone_names), role_key);
    if (l) {
        if (stored) *stored = true;
        return FindBoneIndex(bone_names, l->Get("bone"));
    }
    Role r;
    if (RoleFromKey(role_key, &r)) return GuessRoleMapping(bone_names)[static_cast<size_t>(r)];
    return LearnedRoleGuess(file, bone_names, role_key);
}

int GuessRoleKey(const RoleMapFile& file, const std::vector<std::string>& bone_names, const std::string& role_key)
{
    Role r;
    if (RoleFromKey(role_key, &r)) return GuessRoleMapping(bone_names)[static_cast<size_t>(r)];
    return LearnedRoleGuess(file, bone_names, role_key);
}

RoleMapFile ReadRoleMapFile(const std::string& root)
{
    try {
        FileState st;
        return LoadFile(root, &st);
    } catch (...) {
        return RoleMapFile{};
    }
}

std::vector<CustomRole> GetCustomRoles(const std::string& root)
{
    try {
        return CustomRolesInFile(ReadRoleMapFile(root));
    } catch (...) {
        return {};
    }
}

RoleMapping GetFullRoleMapping(const std::string& root, const std::vector<std::string>& bone_names,
                               const std::vector<std::string>& extra_keys)
{
    RoleMapping out;
    try {
        const RoleMapFile f = ReadRoleMapFile(root);
        out.builtin = ResolveRoleMapping(f, bone_names);
        out.roles = CustomRolesInFile(f);
        for (const CustomRole& c : out.roles) out.names[c.key] = c.name;
        std::vector<std::string> keys = RoleKeysOf(out.roles);
        for (const std::string& k : extra_keys)
            if (!k.empty() && !IsBuiltinKey(k) && std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
        for (const std::string& k : keys) out.custom[k] = bone_names.empty() ? -1 : ResolveRoleKey(f, bone_names, k);
    } catch (...) {
        out = RoleMapping{};
        out.builtin = GuessRoleMapping(bone_names);
    }
    return out;
}

RoleMapping RoleMappingForBlocks(const std::string& root, const std::vector<std::string>& bone_names,
                                 const std::vector<Block>& blocks)
{
    RoleMapping rm = GetFullRoleMapping(root, bone_names, CustomRoleKeysUsed(blocks));
    SetCustomRoleNames(rm.names);
    return rm;
}

bool BindBlocksWithRoleMapping(const std::string& root, std::vector<Block>& blocks,
                               const std::vector<std::string>& bone_names, const std::vector<int>& bone_parents,
                               std::string* missing)
{
    const RoleMapping rm = RoleMappingForBlocks(root, bone_names, blocks);
    return BindBoneRefs(blocks, rm.builtin, rm.custom, bone_names, bone_parents, missing);
}

bool SetRoleBone(const std::string& root, const std::vector<std::string>& bone_names, const std::string& role_key,
                 const std::string& bone_name, std::string* err)
{
    try {
        FileState   st;
        RoleMapFile f = LoadFile(root, &st);
        SetRoleInFile(f, SkeletonKey(bone_names), role_key, bone_name);
        return SaveFile(root, f, st, err);
    } catch (...) {
        if (err) *err = "The role mapping could not be saved.";
        return false;
    }
}

bool SetRoleBone(const std::string& root, const std::vector<std::string>& bone_names, Role role,
                 const std::string& bone_name, std::string* err)
{
    return SetRoleBone(root, bone_names, std::string(RoleKey(role)), bone_name, err);
}

bool ClearRole(const std::string& root, const std::vector<std::string>& bone_names, const std::string& role_key,
               std::string* err)
{
    try {
        FileState   st;
        RoleMapFile f = LoadFile(root, &st);
        if (st == FileState::Unreadable) return SaveFile(root, f, st, err);  // refused, with the reason
        if (!ClearRoleInFile(f, SkeletonKey(bone_names), role_key) && st != FileState::NotRoles) return true;
        return SaveFile(root, f, st, err);
    } catch (...) {
        if (err) *err = "The role mapping could not be saved.";
        return false;
    }
}

bool ClearRole(const std::string& root, const std::vector<std::string>& bone_names, Role role, std::string* err)
{
    return ClearRole(root, bone_names, std::string(RoleKey(role)), err);
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

std::vector<StoredRoleEntry> GetStoredRoles(const std::string& root, const std::vector<std::string>& bone_names,
                                            const std::vector<std::string>& role_keys)
{
    std::vector<StoredRoleEntry> out(role_keys.size());
    try {
        FileState         st;
        const RoleMapFile f = LoadFile(root, &st);
        const std::string skel = SkeletonKey(bone_names);
        for (size_t i = 0; i < role_keys.size(); ++i) {
            const RoleMapLine* l = FindLine(f, skel, role_keys[i]);
            if (!l) continue;
            out[i].stored = true;
            out[i].bone = l->Get("bone");
        }
    } catch (...) {
        out.assign(role_keys.size(), StoredRoleEntry{});
    }
    return out;
}

std::vector<StoredRoleEntry> GetStoredRoles(const std::string& root, const std::vector<std::string>& bone_names)
{
    std::vector<std::string> keys;
    for (int r = 0; r < static_cast<int>(Role::Count); ++r) keys.push_back(RoleKey(static_cast<Role>(r)));
    return GetStoredRoles(root, bone_names, keys);
}

StoredRoleEntry GetStoredRole(const std::string& root, const std::vector<std::string>& bone_names,
                              const std::string& role_key)
{
    return GetStoredRoles(root, bone_names, std::vector<std::string>{role_key})[0];
}

StoredRoleEntry GetStoredRole(const std::string& root, const std::vector<std::string>& bone_names, Role role)
{
    const int r = static_cast<int>(role);
    if (r < 0 || r >= static_cast<int>(Role::Count)) return StoredRoleEntry{};
    return GetStoredRole(root, bone_names, std::string(RoleKey(role)));
}

bool RestoreRole(const std::string& root, const std::vector<std::string>& bone_names, const std::string& role_key,
                 const StoredRoleEntry& entry, std::string* err)
{
    return entry.stored ? SetRoleBone(root, bone_names, role_key, entry.bone, err)
                        : ClearRole(root, bone_names, role_key, err);
}

bool RestoreRole(const std::string& root, const std::vector<std::string>& bone_names, Role role,
                 const StoredRoleEntry& entry, std::string* err)
{
    return RestoreRole(root, bone_names, std::string(RoleKey(role)), entry, err);
}

bool ApplyRoleChange(const std::string& root, const std::vector<std::string>& bone_names, const std::string& role_key,
                     RoleChangeKind kind, const std::string& bone, StoredRoleEntry* prev_out, bool* changed,
                     std::string* err)
{
    if (changed) *changed = false;
    const StoredRoleEntry prev = GetStoredRole(root, bone_names, role_key);
    if (prev_out) *prev_out = prev;
    const bool ok = kind == RoleChangeKind::Auto
                        ? ClearRole(root, bone_names, role_key, err)
                        : SetRoleBone(root, bone_names, role_key,
                                      kind == RoleChangeKind::None ? std::string() : bone, err);
    if (ok && changed) *changed = !(GetStoredRole(root, bone_names, role_key) == prev);
    return ok;
}

bool ApplyRoleChange(const std::string& root, const std::vector<std::string>& bone_names, Role role,
                     RoleChangeKind kind, const std::string& bone, StoredRoleEntry* prev_out, bool* changed,
                     std::string* err)
{
    return ApplyRoleChange(root, bone_names, std::string(RoleKey(role)), kind, bone, prev_out, changed, err);
}

// ---- Story 10-3e: custom roles ---------------------------------------------------------------

bool AddCustomRole(const std::string& root, const std::string& name, std::string* key_out, std::string* err)
{
    try {
        FileState   st;
        RoleMapFile f = LoadFile(root, &st);
        if (st == FileState::Unreadable) return SaveFile(root, f, st, err);  // refused, with the reason
        std::string key, clean;
        if (!CheckRoleName(f, name, "", &key, &clean, err)) return false;
        SetRoleDefinition(f, key, clean);
        if (!SaveFile(root, f, st, err)) return false;
        if (key_out) *key_out = key;
        return true;
    } catch (...) {
        if (err) *err = "The role could not be saved.";
        return false;
    }
}

bool RenameCustomRole(const std::string& root, const std::string& role_key, const std::string& name,
                      std::string* old_name, std::string* err)
{
    try {
        FileState   st;
        RoleMapFile f = LoadFile(root, &st);
        if (st == FileState::Unreadable) return SaveFile(root, f, st, err);
        const std::vector<CustomRole> list = CustomRolesInFile(f);
        auto it = std::find_if(list.begin(), list.end(), [&](const CustomRole& c) { return c.key == role_key; });
        if (it == list.end()) {
            if (err) *err = "This role is not in your roles: create it first.";
            return false;
        }
        std::string clean;
        if (!CheckRoleName(f, name, role_key, nullptr, &clean, err)) return false;
        if (old_name) *old_name = it->name;
        if (clean == it->name) return true;  // nothing to write
        SetRoleDefinition(f, role_key, clean);
        return SaveFile(root, f, st, err);
    } catch (...) {
        if (err) *err = "The role could not be saved.";
        return false;
    }
}

bool DeleteCustomRole(const std::string& root, const std::string& role_key, std::string* name_out, int* index_out,
                      std::string* err)
{
    try {
        FileState   st;
        RoleMapFile f = LoadFile(root, &st);
        if (st == FileState::Unreadable) return SaveFile(root, f, st, err);
        const std::vector<CustomRole> list = CustomRolesInFile(f);
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].key == role_key) {
                if (name_out) *name_out = list[i].name;
                if (index_out) *index_out = static_cast<int>(i);
            }
        const size_t before = f.lines.size();
        f.lines.erase(std::remove_if(f.lines.begin(), f.lines.end(),
                                     [&](const RoleMapLine& l) { return l.is_role && l.Get("key") == role_key; }),
                      f.lines.end());
        if (f.lines.size() == before && st != FileState::NotRoles) return true;  // not defined: nothing to do
        return SaveFile(root, f, st, err);
    } catch (...) {
        if (err) *err = "The role could not be saved.";
        return false;
    }
}

bool RestoreCustomRole(const std::string& root, const std::string& role_key, const std::string& name, int index,
                       std::string* err)
{
    try {
        if (role_key.empty() || IsBuiltinKey(role_key)) {
            if (err) *err = "The role could not be saved.";
            return false;
        }
        FileState   st;
        RoleMapFile f = LoadFile(root, &st);
        if (st == FileState::Unreadable) return SaveFile(root, f, st, err);
        SetRoleDefinition(f, role_key, CleanName(name), index);
        return SaveFile(root, f, st, err);
    } catch (...) {
        if (err) *err = "The role could not be saved.";
        return false;
    }
}

// ---- Story 10-3f: role config Import / Export (CSV) -----------------------------------------

namespace {

constexpr const char kNoneBone[] = "(none)";
constexpr const char kNotRoleFile[] = "This is not a role file.";

std::string TrimSpaces(const std::string& s)
{
    const size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    const size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

std::string CsvField(const std::string& v, char sep)
{
    bool quote = !v.empty() && (v.front() == ' ' || v.back() == ' ');
    for (char c : v)
        if (c == sep || c == ',' || c == ';' || c == '\t' || c == '"' || c == '\r' || c == '\n') quote = true;
    if (!quote) return v;
    std::string out = "\"";
    for (char c : v) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + '"';
}

// The separator of a CSV text: the most frequent of ',' ';' tab outside quotes on the header
// line, the first non-blank one (ties: ',' then ';'); ',' when none. A '"' opens a quoted field
// only at the start of a field (as ParseCsv reads it).
char DetectSeparator(const std::string& text)
{
    int    comma = 0, semi = 0, tab = 0;
    bool   q = false, field_start = true;
    size_t i = text.find_first_not_of(" \t\r\n");
    if (i == std::string::npos) return ',';
    i = text.find_last_of("\r\n", i);  // the header line starts after the last line break before it
    i = i == std::string::npos ? 0 : i + 1;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (q) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') ++i;
                else q = false;
            }
            continue;
        }
        if (c == '"' && field_start) {
            q = true;
            field_start = false;
            continue;
        }
        if (c == '\n' || c == '\r') break;
        field_start = c == ',' || c == ';' || c == '\t';
        if (c == ',') ++comma;
        else if (c == ';') ++semi;
        else if (c == '\t') ++tab;
    }
    if (tab > comma && tab > semi) return '\t';
    if (semi > comma) return ';';
    return ',';
}

// RFC 4180 records: quoted fields may hold the separator, "" and line breaks; CRLF, LF or CR
// end a record. A '"' opens a quoted field only at the start of a field; elsewhere it is a
// literal character (a hand edit like weapon"r). Blank records (every field empty: a blank line, ",,") are dropped.
std::vector<std::vector<std::string>> ParseCsv(const std::string& text, char sep)
{
    std::vector<std::vector<std::string>> recs;
    std::vector<std::string>              rec;
    std::string                           field;
    bool                                  quoted = false, any = false, field_started = false;
    auto end_field = [&]() {
        rec.push_back(field);
        field.clear();
        field_started = false;
    };
    auto end_record = [&]() {
        end_field();
        bool blank = true;
        for (const std::string& v : rec) blank = blank && TrimSpaces(v).empty();
        if (!blank) recs.push_back(rec);
        rec.clear();
        any = false;
    };
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field += c;
            }
            continue;
        }
        any = true;
        if (c == '"' && field.empty() && !field_started) {
            quoted = true;
            field_started = true;
        } else if (c == sep) {
            end_field();
        } else if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            end_record();
        } else {
            field += c;
            field_started = true;
        }
    }
    if (any) end_record();
    return recs;
}

}  // namespace

std::vector<RoleCsvRow> RoleCsvRowsFromFile(const RoleMapFile& file, const std::vector<std::string>& bone_names,
                                            const std::vector<std::string>& extra_keys)
{
    std::vector<RoleCsvRow> rows;
    for (int r = 0; r < static_cast<int>(Role::Count); ++r)
        rows.push_back({RoleKey(static_cast<Role>(r)), RoleName(static_cast<Role>(r)), StoredRoleEntry{}});
    const std::vector<CustomRole> list = CustomRolesInFile(file);
    for (const CustomRole& c : list) rows.push_back({c.key, c.name, StoredRoleEntry{}});
    for (const std::string& k : extra_keys) {
        if (k.empty() || IsBuiltinKey(k)) continue;
        bool seen = false;
        for (const RoleCsvRow& row : rows) seen = seen || row.key == k;
        if (!seen) rows.push_back({k, RoleNameFromKey(k), StoredRoleEntry{}});
    }
    const std::string skel = SkeletonKey(bone_names);
    for (RoleCsvRow& row : rows) {
        const RoleMapLine* l = FindLine(file, skel, row.key);
        if (!l) continue;
        row.entry.stored = true;
        row.entry.bone = l->Get("bone");
    }
    return rows;
}

std::string ExportRolesCsv(const std::vector<RoleCsvRow>& rows, char sep)
{
    if (sep != ';') sep = ',';
    std::string out = "\xEF\xBB\xBF";  // UTF-8 BOM: spreadsheets then read the names right
    out += std::string("role") + sep + "name" + sep + "bone\r\n";
    for (const RoleCsvRow& r : rows) {
        const std::string bone = !r.entry.stored ? std::string() : r.entry.bone.empty() ? std::string(kNoneBone)
                                                                                       : r.entry.bone;
        out += CsvField(r.key, sep) + sep + CsvField(r.name, sep) + sep + CsvField(bone, sep) + "\r\n";
    }
    return out;
}

std::string RoleExportFileName(const std::string& anim_path)
{
    std::string stem = anim_path;
    const size_t slash = stem.find_last_of("\\/");
    if (slash != std::string::npos) stem.erase(0, slash + 1);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos && dot > 0) stem.erase(dot);
    for (char& c : stem) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' ||
            c == '?' || c == '*')
            c = '_';
    }
    stem = TrimSpaces(stem);
    while (!stem.empty() && (stem.back() == '.' || stem.back() == ' ')) stem.pop_back();
    return stem.empty() ? std::string("roles.csv") : "roles - " + stem + ".csv";
}

std::string RoleImportSummary(const RoleImportReport& rep)
{
    auto n = [](int v, const char* one, const char* many) { return std::to_string(v) + ' ' + (v == 1 ? one : many); };
    std::string s = "Imported: " + n(rep.added, "role added", "roles added") + ", " +
                    n(rep.set, "choice set", "choices set");
    if (rep.skipped > 0) {
        s += ", " + n(rep.skipped, "row skipped", "rows skipped");
        if (!rep.first_skip.empty()) s += " (" + rep.first_skip + ")";
    }
    return s + ".";
}

bool ImportRolesCsv(RoleMapFile& file, const std::string& csv_in, const std::vector<std::string>& bone_names,
                    RoleImportReport* report, std::string* err)
{
    RoleImportReport rep;
    try {
        std::string csv = csv_in.substr(0, csv_in.find('\0'));
        if (csv.compare(0, 3, "\xEF\xBB\xBF") == 0) csv.erase(0, 3);
        const char                                  sep = DetectSeparator(csv);
        const std::vector<std::vector<std::string>> recs = ParseCsv(csv, sep);
        int col_role = -1, col_name = -1, col_bone = -1;
        if (!recs.empty()) {
            const std::vector<std::string>& h = recs[0];
            for (size_t i = 0; i < h.size(); ++i) {
                const std::string c = LowerAscii(TrimSpaces(h[i]));
                const int         ci = static_cast<int>(i);
                if (c == "role" && col_role < 0) col_role = ci;
                else if (c == "name" && col_name < 0) col_name = ci;
                else if (c == "bone" && col_bone < 0) col_bone = ci;
            }
        }
        if (col_role < 0 || col_name < 0) {
            if (err) *err = kNotRoleFile;
            return false;
        }
        RoleMapFile       f = file;
        const std::string skel = SkeletonKey(bone_names);
        auto cell = [](const std::vector<std::string>& r, int c) {
            return c >= 0 && c < static_cast<int>(r.size()) ? r[static_cast<size_t>(c)] : std::string();
        };
        for (size_t ri = 1; ri < recs.size(); ++ri) {
            const std::vector<std::string>& r = recs[ri];
            const std::string row_tag = "row " + std::to_string(ri + 1) + ": ";
            auto skip = [&](const std::string& why) {
                if (rep.skipped++ == 0) rep.first_skip = row_tag + why;
            };
            std::string       role = TrimSpaces(cell(r, col_role));
            const std::string name = TrimSpaces(cell(r, col_name));
            if (role.compare(0, 5, "role:") == 0) role.erase(0, 5);  // as rules and tooltips write it
            std::string key;
            if (role.empty()) {
                // No key: the name of one of the user's roles names it (a renamed role keeps its
                // key), else the key comes from the name.
                const std::string low = LowerAscii(CleanRoleName(name));
                for (const CustomRole& c : CustomRolesInFile(f))
                    if (!low.empty() && LowerAscii(c.name) == low) key = c.key;
                if (key.empty()) key = RoleKeyFromName(name);
            } else {
                key = RoleKeyFromName(role) == role ? role : RoleKeyFromName(role);
            }
            if (key.empty()) {
                skip(role.empty() && name.empty() ? std::string("no role") : "\"" + (role.empty() ? name : role) +
                                                                                 "\" is not a role name");
                continue;
            }
            const bool builtin = IsBuiltinKey(key);
            // A built-in role's name is ignored; any other row's name never holds ',' or ';'.
            if (!builtin && name.find_first_of(",;") != std::string::npos) {
                skip("A role name cannot contain , or ;");
                continue;
            }
            // The bone first: a row whose bone this skeleton lacks is skipped whole.
            enum class Choice { Keep, Auto, Set } choice = Choice::Keep;  // Keep: no bone column
            std::string bone;  // Set: as stored ("" = no bone)
            if (col_bone >= 0) {
                const std::string b = TrimSpaces(cell(r, col_bone));
                if (b.empty()) {
                    choice = Choice::Auto;
                } else if (LowerAscii(b) == kNoneBone) {
                    choice = Choice::Set;
                } else {
                    const int idx = FindBoneIndex(bone_names, b);
                    if (idx < 0) {
                        skip("the bone \"" + b + "\" is not in this skeleton");
                        continue;
                    }
                    choice = Choice::Set;
                    bone = bone_names[static_cast<size_t>(idx)];
                }
            }
            // A role the user lacks is added with the file's name (checked like + Role).
            bool        add = false;
            std::string clean;
            if (!builtin) {
                const std::vector<CustomRole> list = CustomRolesInFile(f);
                bool                          have = false;
                for (const CustomRole& c : list) have = have || c.key == key;
                if (!have) {
                    std::string why;
                    const std::string typed = name.empty() ? RoleNameFromKey(key) : name;
                    if (!CheckRoleName(f, typed, key, nullptr, &clean, &why)) {
                        skip(why);
                        continue;
                    }
                    add = true;
                }
            }
            if (add) {
                SetRoleDefinition(f, key, clean);
                ++rep.added;
            }
            if (choice == Choice::Keep) continue;
            const RoleMapLine* l = FindLine(f, skel, key);
            if (choice == Choice::Auto) {
                if (l && ClearRoleInFile(f, skel, key)) ++rep.set;
            } else if (!l || l->Get("bone") != bone) {
                SetRoleInFile(f, skel, key, bone);
                ++rep.set;
            }
        }
        file = std::move(f);
        if (report) *report = rep;
        return true;
    } catch (...) {
        if (err) *err = "The file could not be imported.";
        return false;
    }
}

bool ExportRoleConfig(const std::string& root, const std::vector<std::string>& bone_names,
                      const std::vector<std::string>& extra_keys, char sep, const std::string& dest_path,
                      std::string* err)
{
    try {
        FileState         st;
        const RoleMapFile f = LoadFile(root, &st);
        // An unreadable roles.txt: the window shows the 9 built-in roles only (all on the guess).
        const std::vector<std::string> extra = st == FileState::Unreadable ? std::vector<std::string>{} : extra_keys;
        return WriteTextAtomic(fs::u8path(dest_path), ExportRolesCsv(RoleCsvRowsFromFile(f, bone_names, extra), sep),
                               err);
    } catch (...) {
        if (err) *err = "The roles could not be exported.";
        return false;
    }
}

bool ImportRoleConfig(const std::string& root, const std::vector<std::string>& bone_names,
                      const std::string& src_path, RoleImportReport* report, RoleMapSnapshot* prev, std::string* err)
{
    try {
        if (report) *report = RoleImportReport{};
        std::string csv;
        const fs::path src = fs::u8path(src_path);
        if (ReadText(src, &csv) != FileState::Ok) {
            if (err) *err = "Cannot read " + src.u8string() + ".";
            return false;
        }
        RoleMapSnapshot snap;
        const FileState pst = ReadText(fs::u8path(RoleMapPath(root)), &snap.text);
        if (pst == FileState::Unreadable) return SaveFile(root, RoleMapFile{}, FileState::Unreadable, err);  // refused
        snap.existed = pst == FileState::Ok;
        if (!snap.existed) snap.text.clear();
        FileState   st;
        RoleMapFile f = LoadFile(root, &st);
        if (st == FileState::Unreadable) return SaveFile(root, f, st, err);
        RoleImportReport rep;
        if (!ImportRolesCsv(f, csv, bone_names, &rep, err)) return false;
        if (rep.changed() && !SaveFile(root, f, st, err)) return false;
        if (report) *report = rep;
        if (prev) *prev = snap;
        return true;
    } catch (...) {
        if (err) *err = "The file could not be imported.";
        return false;
    }
}

bool RestoreRoleMapText(const std::string& root, const RoleMapSnapshot& snapshot, std::string* err)
{
    try {
        const fs::path  p = fs::u8path(RoleMapPath(root));
        std::string     cur;
        const FileState st = ReadText(p, &cur);
        if (st == FileState::Unreadable) return SaveFile(root, RoleMapFile{}, st, err);  // refused, with the reason
        if (!snapshot.existed) {
            std::error_code ec;
            if (st == FileState::Absent) return true;
            fs::remove(p, ec);
            if (ec) {
                if (err) *err = "Cannot write " + p.u8string() + ".";
                return false;
            }
            return true;
        }
        if (st == FileState::Ok && cur == snapshot.text) return true;
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        return WriteTextAtomic(p, snapshot.text, err);
    } catch (...) {
        if (err) *err = "The role mapping could not be saved.";
        return false;
    }
}

}  // namespace rav
