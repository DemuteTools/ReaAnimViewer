// SPDX-License-Identifier: MIT
//
// Reference markers for "RAV: Measure detection against reference markers" (Epic 10, spike
// 10-7a): "REF" marks a contact of any bone, "REF <label>" a contact of the bone that plays
// role <label> (bone_roles.h: a built-in or a custom role key). The label is normalized with
// RoleKeyFromName, so "REF left_hand" and "REF Left hand" (the name the role was created
// with) both read "left_hand". Keys only: a built-in role's shown name can differ from its
// key ("left hip (up leg)" is left_up_leg), and a renamed custom role keeps its first key.
//
// The action dumps them to its CSV (detection_measure.cpp) and tests/detection_eval.cpp reads
// them back. After '# ref=<t>,<t>...' (every REF time, labelled or not), one pair per label,
// in label order:
//   # ref.<label>=<t>,<t>...         clip time
//   # ref_bone.<label>=<bone name>   the bone that plays the role on this skeleton ("" = none)
//
// Pure C++17, header-only (ResolveRefBones needs role_map_store.cpp). Host-tested
// (tests/ref_labels_test.cpp).

#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "bone_roles.h"
#include "role_map_store.h"

namespace rav {

// True when `name` is a reference marker: "REF" alone, or "REF" + whitespace + a label (ASCII
// case-insensitive, surrounding whitespace ignored). `label` gets the label as a role key: ""
// for a bare REF, and for a label a key keeps nothing of ("REF ?").
inline bool ParseRefMarkerName(const char* name, std::string* label)
{
    if (!name) return false;
    const auto space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    std::string s = name;
    while (!s.empty() && space(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && space(s[i])) ++i;
    s.erase(0, i);
    if (s.size() < 3) return false;
    for (size_t k = 0; k < 3; ++k)
        if (std::toupper(static_cast<unsigned char>(s[k])) != "REF"[k]) return false;
    if (s.size() > 3 && !space(s[3])) return false;  // "REFERENCE", "REF:x"
    if (label) *label = RoleKeyFromName(s.substr(3));
    return true;
}

struct RefTimes {
    std::vector<double>                        all;       // every REF time, labelled or not
    std::map<std::string, std::vector<double>> by_label;  // the labelled ones, per label
    std::map<std::string, std::string>         bone;      // label -> its bone ("" = none plays it)
    std::set<std::string>                      not_role;  // labels no role has as its key

    void Add(const std::string& label, double t)
    {
        all.push_back(t);
        if (!label.empty()) by_label[label].push_back(t);
    }
};

// The bone of each label on this skeleton, as the Tagging view maps roles: the stored entry in
// roles.txt, else the guess. A label with no bone that is neither a built-in role key nor one
// of the user's roles goes to `not_role`.
inline void ResolveRefBones(const RoleMapFile& roles, const std::vector<std::string>& bone_names, RefTimes* r)
{
    std::set<std::string> custom;
    for (const CustomRole& c : CustomRolesInFile(roles)) custom.insert(c.key);
    r->bone.clear();
    r->not_role.clear();
    for (const auto& kv : r->by_label) {
        const int  b = ResolveRoleKey(roles, bone_names, kv.first);
        const bool found = b >= 0 && b < static_cast<int>(bone_names.size());
        r->bone[kv.first] = found ? bone_names[static_cast<size_t>(b)] : std::string();
        if (!found && !RoleFromKey(kv.first, nullptr) && !custom.count(kv.first)) r->not_role.insert(kv.first);
    }
}

// The '# ref.<label>=' and '# ref_bone.<label>=' lines, each ending with '\n'.
inline std::string RefLabelLines(const RefTimes& r)
{
    std::string out;
    char        buf[64];
    for (const auto& kv : r.by_label) {
        out += "# ref." + kv.first + "=";
        for (size_t i = 0; i < kv.second.size(); ++i) {
            std::snprintf(buf, sizeof(buf), "%s%.6f", i ? "," : "", kv.second[i]);
            out += buf;
        }
        const auto b = r.bone.find(kv.first);
        out += "\n# ref_bone." + kv.first + "=" + (b == r.bone.end() ? std::string() : b->second) + "\n";
    }
    return out;
}

// Reads one CSV header line (its key and value) into `r`: 'ref', 'ref.<label>' or
// 'ref_bone.<label>'. Times that are not numbers are skipped. False for any other key, and for
// an empty label.
inline bool ReadRefHeader(const std::string& key, const std::string& value, RefTimes* r)
{
    const auto times = [&value]() {
        std::vector<double> t;
        size_t              a = 0;
        for (;;) {
            const size_t      b = value.find(',', a);
            const std::string s = value.substr(a, b == std::string::npos ? std::string::npos : b - a);
            char*             end = nullptr;
            const double      x = std::strtod(s.c_str(), &end);
            if (!s.empty() && end && *end == '\0') t.push_back(x);
            if (b == std::string::npos) break;
            a = b + 1;
        }
        return t;
    };
    if (key == "ref") {
        const std::vector<double> t = times();
        r->all.insert(r->all.end(), t.begin(), t.end());
        return true;
    }
    if (key.size() > 4 && key.compare(0, 4, "ref.") == 0) {
        const std::vector<double> t = times();
        std::vector<double>&      dst = r->by_label[key.substr(4)];
        dst.insert(dst.end(), t.begin(), t.end());
        return true;
    }
    if (key.size() > 9 && key.compare(0, 9, "ref_bone.") == 0) {
        r->bone[key.substr(9)] = value;
        return true;
    }
    return false;
}

// The reports' line: "  REF by bone: left_hand 12 (mixamorig:LeftHand), right_foot 8 (no bone
// plays this role), lft_heel 1 (not a role key), unlabelled 3\n". "" when no REF has a label.
inline std::string RefLabelsText(const RefTimes& r)
{
    if (r.by_label.empty()) return "";
    std::string out = "  REF by bone:";
    size_t      labelled = 0;
    bool        first = true;
    for (const auto& kv : r.by_label) {
        labelled += kv.second.size();
        const auto        b = r.bone.find(kv.first);
        const std::string bone = b == r.bone.end() ? std::string() : b->second;
        const char* none = r.not_role.count(kv.first) ? "not a role key" : "no bone plays this role";
        out += (first ? " " : ", ") + kv.first + " " + std::to_string(kv.second.size()) + " (" +
               (bone.empty() ? std::string(none) : bone) + ")";
        first = false;
    }
    if (r.all.size() > labelled) out += ", unlabelled " + std::to_string(r.all.size() - labelled);
    return out + "\n";
}

}  // namespace rav
