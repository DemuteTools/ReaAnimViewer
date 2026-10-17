// SPDX-License-Identifier: MIT
//
// Reference markers for "RAV: Measure detection against reference markers" (Epic 10, spike
// 10-7a): "REF" marks a contact of any bone, "REF <label>" a contact of the bone that plays
// role <label> (bone_roles.h: a built-in or a custom role key), or else of the bone named
// <label> ("REF LeftToe_End", "REF mixamorig:LeftHand": no role needed). The label is
// normalized with RoleKeyFromName, so "REF left_hand" and "REF Left hand" both read
// "left_hand", and "REF Sword Tip" (the name a custom role was created with) "sword_tip". Role
// keys, not shown names: a built-in role's shown name can differ from its key ("left hip (up
// leg)" is left_up_leg), and a renamed custom role keeps its first key.
//
// The action dumps them to its CSV (detection_measure.cpp) and tests/detection_eval.cpp reads
// them back. After '# ref=<t>,<t>...' (every REF time, labelled or not), one pair per label,
// in label order:
//   # ref.<label>=<t>,<t>...         clip time
//   # ref_bone.<label>=<bone name>   the bone that plays the role on this skeleton ("" = none)
//
// Story 10-8n: event labels. "REF <kind> <side>" marks the moment a non-step sound starts:
// <kind> = lift (also "liftoff", "lift off"), slide, pivot, grab or release; <side> = L, R,
// left or right. Case, spaces, '_' and '-' between the words do not matter ("REF lift L",
// "REF Slide right", "REF grab_l"). An event label is no contact: it is kept in by_label only
// (never in `all`, so the Footsteps measure never counts it), has no bone and is never unknown.
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

// Story 10-9a appends the sideless jump (take-off) and land.
enum class RefEventKind { Lift, Slide, Pivot, Grab, Release, Jump, Land };

// True when `key` (a label normalized by RoleKeyFromName) is an event label:
// "<kind>_<l|r|left|right>", plus "lift_off_<side>" and "liftoff_<side>". `side` gets 'L' / 'R'.
// Story 10-9a: "jump" and "land" are sideless (no side suffix): `side` gets 'B'.
inline bool ParseEventRefLabel(const std::string& key, RefEventKind* kind, char* side)
{
    if (key == "jump" || key == "land") {
        if (kind) *kind = key == "jump" ? RefEventKind::Jump : RefEventKind::Land;
        if (side) *side = 'B';
        return true;
    }
    const size_t u = key.rfind('_');
    if (u == std::string::npos || u == 0) return false;
    const std::string k = key.substr(0, u), sd = key.substr(u + 1);
    char              c = 0;
    if (sd == "l" || sd == "left") c = 'L';
    else if (sd == "r" || sd == "right") c = 'R';
    else return false;
    RefEventKind e;
    if (k == "lift" || k == "liftoff" || k == "lift_off") e = RefEventKind::Lift;
    else if (k == "slide") e = RefEventKind::Slide;
    else if (k == "pivot") e = RefEventKind::Pivot;
    else if (k == "grab") e = RefEventKind::Grab;
    else if (k == "release") e = RefEventKind::Release;
    else return false;
    if (kind) *kind = e;
    if (side) *side = c;
    return true;
}

inline bool IsEventRefLabel(const std::string& key) { return ParseEventRefLabel(key, nullptr, nullptr); }

struct RefTimes {
    std::vector<double>                        all;       // every contact REF time, labelled or not (no event label)
    std::map<std::string, std::vector<double>> by_label;  // the labelled ones, per label
    std::map<std::string, std::string>         bone;      // label -> its bone ("" = none plays it)
    std::set<std::string>                      unknown;   // labels that name no role and no bone

    void Add(const std::string& label, double t)
    {
        if (!IsEventRefLabel(label)) all.push_back(t);
        if (!label.empty()) by_label[label].push_back(t);
    }
};

// The bone of each label on this skeleton. A role key (built-in, or one of the user's roles)
// maps as the Tagging view maps roles: the stored entry in roles.txt, else the guess. Any other
// label names a bone: the first whose name, without or with its namespace, gives that key
// ("lefttoe_end" = mixamorig:LeftToe_End). A label that names neither goes to `unknown`.
inline void ResolveRefBones(const RoleMapFile& roles, const std::vector<std::string>& bone_names, RefTimes* r)
{
    std::set<std::string> custom;
    for (const CustomRole& c : CustomRolesInFile(roles)) custom.insert(c.key);
    r->bone.clear();
    r->unknown.clear();
    for (const auto& kv : r->by_label) {
        if (IsEventRefLabel(kv.first)) {  // an event: no bone, never unknown
            r->bone[kv.first] = std::string();
            continue;
        }
        const bool is_role = RoleFromKey(kv.first, nullptr) || custom.count(kv.first) > 0;
        int        b = ResolveRoleKey(roles, bone_names, kv.first);
        bool       found = b >= 0 && b < static_cast<int>(bone_names.size());
        for (size_t i = 0; !found && !is_role && i < bone_names.size(); ++i)
            if (RoleKeyFromName(NormalizeBoneName(bone_names[i])) == kv.first ||
                RoleKeyFromName(bone_names[i]) == kv.first) {
                b = static_cast<int>(i);
                found = true;
            }
        r->bone[kv.first] = found ? bone_names[static_cast<size_t>(b)] : std::string();
        if (!found && !is_role) r->unknown.insert(kv.first);
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
// plays this role), lft_heel 1 (no role or bone has this name), grab_l 2 (event), unlabelled 3\n".
// "" when no REF has a label. Event labels are not in `all`, so they never count as unlabelled.
inline std::string RefLabelsText(const RefTimes& r)
{
    if (r.by_label.empty()) return "";
    std::string out = "  REF by bone:";
    size_t      labelled = 0;
    bool        first = true;
    for (const auto& kv : r.by_label) {
        if (IsEventRefLabel(kv.first)) {
            out += (first ? " " : ", ") + kv.first + " " + std::to_string(kv.second.size()) + " (event)";
            first = false;
            continue;
        }
        labelled += kv.second.size();
        const auto        b = r.bone.find(kv.first);
        const std::string bone = b == r.bone.end() ? std::string() : b->second;
        const char* none = r.unknown.count(kv.first) ? "no role or bone has this name" : "no bone plays this role";
        out += (first ? " " : ", ") + kv.first + " " + std::to_string(kv.second.size()) + " (" +
               (bone.empty() ? std::string(none) : bone) + ")";
        first = false;
    }
    if (r.all.size() > labelled) out += ", unlabelled " + std::to_string(r.all.size() - labelled);
    return out + "\n";
}

}  // namespace rav
