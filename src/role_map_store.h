// SPDX-License-Identifier: MIT
//
// The role mapping, remembered per skeleton (Epic 10, story 10-2). A preset names roles
// (bone_roles.h); which bone plays each role is guessed from the bone names
// (GuessRoleMapping) unless the user set it for that skeleton.
//
// A skeleton is its set of bone names: SkeletonKey = FNV-1a 64 over the sorted,
// normalized names (NormalizeBoneName), so two files of the same rig share one mapping
// whatever their bone order or namespace.
//
// File <root>/ReaAnimViewer/roles.txt (root = REAPER's resource path; the user folder):
//   RAVROLES 1
//   map skeleton=<16 hex> role=left_heel bone=mixamorig:LeftFoot
// `bone` is free text, last; empty = "no bone plays this role". Bones are stored by name,
// never by index. Unknown lines, unknown fields and unknown role keys are kept in place.
// A file that is not a roles file is moved aside to roles.txt.bad (.bad2, .bad3... when
// taken) before the first write; when that fails, or when the file exists but cannot be
// read, the write is refused with a reason and the file is left as it is.
//
// Pure C++17 (std::filesystem, no REAPER, no _WIN32). No-throw. Host-tested
// (tests/role_map_store_test.cpp).

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "bone_roles.h"

namespace rav {

std::string SkeletonKey(const std::vector<std::string>& bone_names);

// <root>/ReaAnimViewer/roles.txt (UTF-8).
std::string RoleMapPath(const std::string& root);

struct RoleMapLine {
    bool                                             is_map = false;
    std::string                                      raw;     // an unknown line, as written
    std::vector<std::pair<std::string, std::string>> fields;  // a map line, in order
    std::string Get(const char* key) const;
};

struct RoleMapFile {
    int                      version = 1;
    std::string              header_rest;  // the header line after the version, as written
    std::vector<RoleMapLine> lines;
};

// False when the text has no "RAVROLES" header.
bool ParseRoleMap(const std::string& text, RoleMapFile* out);
std::string SerializeRoleMap(const RoleMapFile& file);

// Bone index per role (indexed by Role, -1 = none): the stored bone for that skeleton,
// else the guess. `stored` (optional) says, per role, whether it came from the file.
std::vector<int> ResolveRoleMapping(const RoleMapFile& file, const std::vector<std::string>& bone_names,
                                    std::vector<char>* stored = nullptr);
// Sets (bone_name "" = none) or clears (back to the guess) one role of a skeleton.
void SetRoleInFile(RoleMapFile& file, const std::string& skeleton, const std::string& role_key,
                   const std::string& bone_name);
bool ClearRoleInFile(RoleMapFile& file, const std::string& skeleton, const std::string& role_key);

// The same, on <root>/ReaAnimViewer/roles.txt.
std::vector<int> GetRoleMapping(const std::string& root, const std::vector<std::string>& bone_names,
                                std::vector<char>* stored = nullptr);
bool SetRoleBone(const std::string& root, const std::vector<std::string>& bone_names, Role role,
                 const std::string& bone_name, std::string* err);
bool ClearRole(const std::string& root, const std::vector<std::string>& bone_names, Role role, std::string* err);

// Story 10-3d -- the Skeleton & roles window.
// The state of roles.txt: none yet, read, not a roles file (moved aside at the first write),
// or there but unreadable (every write is refused; the mapping is the guess).
enum class RoleMapStatus { Absent, Ok, NotRolesFile, Unreadable };
RoleMapStatus GetRoleMapStatus(const std::string& root);

// One role's entry in the file for a skeleton, exactly as stored: none (the guess plays it),
// or a bone text ("" = no bone; possibly a bone this rig does not have).
struct StoredRoleEntry {
    bool        stored = false;
    std::string bone;
    bool operator==(const StoredRoleEntry& o) const { return stored == o.stored && bone == o.bone; }
};
// Every role's entry (indexed by Role). An unreadable or foreign file reads as no entry.
std::vector<StoredRoleEntry> GetStoredRoles(const std::string& root, const std::vector<std::string>& bone_names);
StoredRoleEntry GetStoredRole(const std::string& root, const std::vector<std::string>& bone_names, Role role);
// Puts an entry back as it was read (the window's undo): SetRoleBone(bone) or ClearRole.
bool RestoreRole(const std::string& root, const std::vector<std::string>& bone_names, Role role,
                 const StoredRoleEntry& entry, std::string* err);

// One role change from the window: this bone, no bone, or back to the guess. `prev_out` gets
// the entry it replaced (read before the write); `changed` is true only when the write went
// through and the stored entry differs from it (the window then pushes an undo step). False
// (and `err`) when the write was refused.
enum class RoleChangeKind { Bone, None, Auto };
bool ApplyRoleChange(const std::string& root, const std::vector<std::string>& bone_names, Role role,
                     RoleChangeKind kind, const std::string& bone, StoredRoleEntry* prev_out, bool* changed,
                     std::string* err);

}  // namespace rav
