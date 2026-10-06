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
//   role key=sword_tip name=sword tip
//   map skeleton=<16 hex> role=left_heel bone=mixamorig:LeftFoot
//   map skeleton=<16 hex> role=sword_tip bone=weapon_r
// `bone` is free text, last; empty = "no bone plays this role". Bones are stored by name,
// never by index. Unknown lines, unknown fields and unknown role keys are kept in place.
// Story 10-3e: a `role` line defines one of the user's own roles (for every project): its key
// (RoleKeyFromName of the name it was created with, fixed for life) and its display name
// (`name`, free text, last; a rename changes only it). A key is mapped like a built-in role.
// Deleting a role removes its `role` line only: its map lines stay.
// A file that is not a roles file is moved aside to roles.txt.bad (.bad2, .bad3... when
// taken) before the first write; when that fails, or when the file exists but cannot be
// read, the write is refused with a reason and the file is left as it is.
//
// Pure C++17 (std::filesystem, no REAPER, no _WIN32). No-throw. Host-tested
// (tests/role_map_store_test.cpp).

#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "bone_roles.h"
#include "rule_record.h"

namespace rav {

std::string SkeletonKey(const std::vector<std::string>& bone_names);

// <root>/ReaAnimViewer/roles.txt (UTF-8).
std::string RoleMapPath(const std::string& root);

struct RoleMapLine {
    bool                                             is_map = false;
    bool                                             is_role = false;  // 10-3e: a `role` line (fields)
    std::string                                      raw;     // an unknown line, as written
    std::vector<std::pair<std::string, std::string>> fields;  // a map line, in order
    std::string Get(const char* key) const;
};

struct RoleMapFile {
    int                      version = 1;
    std::string              header_rest;  // the header line after the version, as written
    std::vector<RoleMapLine> lines;
};

// Story 10-3e -- one of the user's own roles: its key (no "role:") and display name.
struct CustomRole {
    std::string key;
    std::string name;
    bool operator==(const CustomRole& o) const { return key == o.key && name == o.name; }
};

// False when the text has no "RAVROLES" header.
bool ParseRoleMap(const std::string& text, RoleMapFile* out);
std::string SerializeRoleMap(const RoleMapFile& file);

// Bone index per role (indexed by Role, -1 = none): the stored bone for that skeleton,
// else the guess. `stored` (optional) says, per role, whether it came from the file.
std::vector<int> ResolveRoleMapping(const RoleMapFile& file, const std::vector<std::string>& bone_names,
                                    std::vector<char>* stored = nullptr);
// Sets (bone_name "" = none) or clears (back to the guess) one role of a skeleton.
// A set edits the role's line (its fields kept) and moves it last: the file lists picks oldest first.
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

// ---- Story 10-3e: custom roles -------------------------------------------------------------
//
// Every role key (built-in or custom) maps per skeleton with the same semantics: the key-based
// SetRoleBone / ClearRole / GetStoredRole(s) / RestoreRole / ApplyRoleChange below (the Role
// forms forward to them). A custom role with no entry for a skeleton is guessed from what the
// user picked for it on other skeletons: the most recent pick (last in the file) first, the
// first bone of this rig with the same NormalizeBoneName. A built-in role keeps its guess table.

// The user's roles in the file, in the order of their first line (the last line's name wins;
// an empty name reads as the key with '_' as ' '). A `role` line with a built-in key is ignored.
std::vector<CustomRole> CustomRolesInFile(const RoleMapFile& file);
// The learned guess of a custom role on this rig (-1 = none, or a built-in key).
int LearnedRoleGuess(const RoleMapFile& file, const std::vector<std::string>& bone_names, const std::string& role_key);
// The guess of any role key: the built-in table, or the learned guess.
int GuessRoleKey(const RoleMapFile& file, const std::vector<std::string>& bone_names, const std::string& role_key);
// The bone of any role key: the stored entry for this skeleton (`stored` = true), else the guess.
int ResolveRoleKey(const RoleMapFile& file, const std::vector<std::string>& bone_names, const std::string& role_key,
                   bool* stored = nullptr);

// roles.txt as read (empty when absent or not readable).
RoleMapFile ReadRoleMapFile(const std::string& root);
std::vector<CustomRole> GetCustomRoles(const std::string& root);

// The whole mapping on one skeleton, from one read: the built-in roles (indexed by Role), the
// bone of each custom role in the list AND of each extra key (the keys rules read; built-in
// keys skipped), and the list with its display names.
struct RoleMapping {
    std::vector<int>                   builtin;
    std::map<std::string, int>         custom;  // key -> bone (-1 = none)
    std::vector<CustomRole>            roles;   // the user's roles (the list)
    std::map<std::string, std::string> names;   // key -> display name, for the list's roles
};
RoleMapping GetFullRoleMapping(const std::string& root, const std::vector<std::string>& bone_names,
                               const std::vector<std::string>& extra_keys);

// The mapping for these blocks: GetFullRoleMapping with the custom role keys they read
// (CustomRoleKeysUsed) as extra keys. Also publishes the list's display names
// (SetCustomRoleNames) for signal names and missing lists.
RoleMapping RoleMappingForBlocks(const std::string& root, const std::vector<std::string>& bone_names,
                                 const std::vector<Block>& blocks);
// Binds blocks with RoleMappingForBlocks (built-in and custom roles). False + `missing` like BindBoneRefs.
bool BindBlocksWithRoleMapping(const std::string& root, std::vector<Block>& blocks,
                               const std::vector<std::string>& bone_names, const std::vector<int>& bone_parents,
                               std::string* missing);

bool SetRoleBone(const std::string& root, const std::vector<std::string>& bone_names, const std::string& role_key,
                 const std::string& bone_name, std::string* err);
bool ClearRole(const std::string& root, const std::vector<std::string>& bone_names, const std::string& role_key,
               std::string* err);
std::vector<StoredRoleEntry> GetStoredRoles(const std::string& root, const std::vector<std::string>& bone_names,
                                            const std::vector<std::string>& role_keys);
StoredRoleEntry GetStoredRole(const std::string& root, const std::vector<std::string>& bone_names,
                              const std::string& role_key);
bool RestoreRole(const std::string& root, const std::vector<std::string>& bone_names, const std::string& role_key,
                 const StoredRoleEntry& entry, std::string* err);
bool ApplyRoleChange(const std::string& root, const std::vector<std::string>& bone_names, const std::string& role_key,
                     RoleChangeKind kind, const std::string& bone, StoredRoleEntry* prev_out, bool* changed,
                     std::string* err);

// Creates a role from a typed name: key = RoleKeyFromName, name = the typed text cleaned (one
// line, spaces collapsed, trimmed, ASCII lower-cased). Refused (false, `err`, nothing written)
// when the key is empty, is already a role's key (built-in or custom), or the name is already
// a role's name (case-insensitive; a built-in's name or short label too), or roles.txt cannot
// be written.
bool AddCustomRole(const std::string& root, const std::string& name, std::string* key_out, std::string* err);
// Renames a role of the list (its key never changes). `old_name` gets the name it replaced.
// Refused like AddCustomRole (the role's own name excepted), or when the key is not in the list.
bool RenameCustomRole(const std::string& root, const std::string& role_key, const std::string& name,
                      std::string* old_name, std::string* err);
// Removes a role's definition (its `role` lines) and keeps its map lines. `name_out` /
// `index_out` get its name and place in the list (for the undo). True when it was not defined.
bool DeleteCustomRole(const std::string& root, const std::string& role_key, std::string* name_out, int* index_out,
                      std::string* err);
// The window's undo: puts a definition back as `name` (sets the name when it exists; else
// inserts it at `index` in the list, -1 = at the end). No name check (it was valid).
bool RestoreCustomRole(const std::string& root, const std::string& role_key, const std::string& name, int index,
                       std::string* err);

// ---- Story 10-3f: role config Import / Export (CSV) -----------------------------------------
//
// A role file: CSV (UTF-8, a BOM accepted on read, RFC 4180 quoting), a header row naming the
// columns: `role` (the key), `name` (the display name), `bone`. Columns are found by header
// name, in any order; unknown ones are ignored. The separator (',' ';' or tab) is read from
// the header line. In `bone`: a bone name, `(none)` (no bone), or empty (auto, the guess).
// One file = all the user's roles plus the choices of ONE skeleton (no skeleton column):
// import applies them to the skeleton it is opened on, matching bones by name.

// One exported row: the key, its display name and its stored entry on the skeleton.
struct RoleCsvRow {
    std::string     key;
    std::string     name;
    StoredRoleEntry entry;
};

// The rows of an export: the 9 built-in roles, the user's roles, then the extra keys (the keys
// the item's rules read; built-in and listed ones skipped), each with its stored entry.
std::vector<RoleCsvRow> RoleCsvRowsFromFile(const RoleMapFile& file, const std::vector<std::string>& bone_names,
                                            const std::vector<std::string>& extra_keys);
// The CSV text (BOM, header `role,name,bone`, CRLF lines), `sep` = ',' or ';'.
std::string ExportRolesCsv(const std::vector<RoleCsvRow>& rows, char sep = ',');

// What an import did. `skipped` rows were not applied; `first_skip` says why the first one was.
struct RoleImportReport {
    int         added = 0;    // roles created
    int         set = 0;      // choices changed on this skeleton
    int         skipped = 0;
    std::string first_skip;
    bool changed() const { return added > 0 || set > 0; }
};
// The one-line result: "Imported: 2 roles added, 5 choices set, 1 row skipped (row 4: ...)."
std::string RoleImportSummary(const RoleImportReport& report);

// Merges a role CSV into `file` for this skeleton: roles the file lists that the user lacks are
// added (a role the user has keeps its name); each listed role's choice replaces the stored one;
// everything else stays. A row is skipped (and counted) when its role is empty, its name holds
// ',' or ';' or is taken by another role, or its bone is not in this skeleton. False with
// "This is not a role file." (and `file` untouched) when there is no header with `role` and
// `name` columns.
bool ImportRolesCsv(RoleMapFile& file, const std::string& csv, const std::vector<std::string>& bone_names,
                    RoleImportReport* report, std::string* err);

// The same, on <root>/ReaAnimViewer/roles.txt. Export reads roles.txt (an unreadable one reads
// as no entries and lists the 9 built-in roles only) and writes `dest_path`.
bool ExportRoleConfig(const std::string& root, const std::vector<std::string>& bone_names,
                      const std::vector<std::string>& extra_keys, char sep, const std::string& dest_path,
                      std::string* err);

// roles.txt exactly as it was before a write (for the window's undo).
struct RoleMapSnapshot {
    bool        existed = false;
    std::string text;
};
// Import reads `src_path`, merges it (ImportRolesCsv) and writes roles.txt when anything
// changed. `prev` gets roles.txt as it was (byte-exact). Refused (false, `err`, nothing written)
// when the file cannot be read, is not a role file, or roles.txt cannot be written.
bool ImportRoleConfig(const std::string& root, const std::vector<std::string>& bone_names,
                      const std::string& src_path, RoleImportReport* report, RoleMapSnapshot* prev, std::string* err);
// The undo of an import: roles.txt put back byte-identical (removed when it did not exist).
// Refused when roles.txt exists but cannot be read.
bool RestoreRoleMapText(const std::string& root, const RoleMapSnapshot& snapshot, std::string* err);

}  // namespace rav
