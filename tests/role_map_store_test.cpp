// SPDX-License-Identifier: MIT
//
// Host test of the per-skeleton role mapping (src/role_map_store.h, story 10-2): the
// skeleton key, stored mapping over the guess, set / clear one role, an edited mapping
// reused by another item of the same skeleton, unknown lines kept, a corrupt file kept
// aside. Works in a temp folder.

#include "role_map_store.h"
#include "rule_record.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace rav;
namespace fs = std::filesystem;

namespace {

int g_fails = 0;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);        \
            ++g_fails;                                              \
        }                                                           \
    } while (0)

int R(Role r)
{
    return static_cast<int>(r);
}

std::string ReadAll(const fs::path& p)
{
    std::ifstream      in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A committed fixture (tests/data), CR dropped (a CRLF checkout reads the same).
std::string ReadFixture(const char* name)
{
    std::string s = ReadAll(fs::u8path(std::string(RAV_TEST_DATA) + "/" + name)), out;
    for (char c : s)
        if (c != '\r') out += c;
    return out;
}

}  // namespace

int main()
{
    std::random_device rd;
    const fs::path     root = fs::temp_directory_path() / ("rav_roles_test_" + std::to_string(rd()));
    fs::create_directories(root);
    const std::string R0 = root.u8string();

    const std::vector<std::string> a = {"mixamorig:Hips", "mixamorig:LeftUpLeg", "mixamorig:LeftLeg",
                                        "mixamorig:LeftFoot", "mixamorig:LeftToeBase", "mixamorig:LeftToe_End"};
    // The same rig: other order, other namespace.
    const std::vector<std::string> b = {"mixamorig1_LeftToe_End", "mixamorig1_LeftFoot", "mixamorig1_Hips",
                                        "mixamorig1_LeftToeBase", "mixamorig1_LeftLeg", "mixamorig1_LeftUpLeg"};
    const std::vector<std::string> other = {"Hips", "LeftFoot"};

    // Skeleton key.
    {
        CHECK(SkeletonKey(a) == SkeletonKey(b));
        CHECK(SkeletonKey(a) != SkeletonKey(other));
        CHECK(SkeletonKey(a).size() == 16);
    }

    // No file: the guess.
    {
        std::vector<char> stored;
        const std::vector<int> m = GetRoleMapping(R0, a, &stored);
        CHECK(m == GuessRoleMapping(a));
        CHECK(m[R(Role::LeftToe)] == 4);
        for (char s : stored) CHECK(!s);
    }

    // An edited mapping is used by another item of the same skeleton.
    {
        std::string err;
        CHECK(SetRoleBone(R0, a, Role::LeftToe, "mixamorig:LeftToe_End", &err));
        std::vector<char> stored;
        const std::vector<int> mb = GetRoleMapping(R0, b, &stored);
        CHECK(mb[R(Role::LeftToe)] == 0);  // LeftToe_End in b's order
        CHECK(stored[R(Role::LeftToe)] && !stored[R(Role::LeftHeel)]);
        CHECK(mb[R(Role::LeftHeel)] == 1);  // still the guess
        CHECK(GetRoleMapping(R0, other) == GuessRoleMapping(other));  // another skeleton: untouched

        // "No bone" for a role: a preset that needs it reports it missing.
        CHECK(SetRoleBone(R0, a, Role::LeftToe, "", &err));
        const std::vector<int> ma = GetRoleMapping(R0, a);
        CHECK(ma[R(Role::LeftToe)] == -1);
        Block blk;
        Condition c;
        c.signal.bones = {R(Role::LeftHeel), R(Role::LeftToe)};
        blk.conditions = {c};
        std::vector<Block> blocks = {blk};
        std::string missing;
        CHECK(!BindBoneRefs(blocks, ma, a, &missing));
        CHECK(missing == "left toe");

        // Clear: back to the guess.
        CHECK(ClearRole(R0, a, Role::LeftToe, &err));
        CHECK(GetRoleMapping(R0, a)[R(Role::LeftToe)] == 4);
    }

    // Unknown lines, fields and role keys are kept in place.
    {
        const std::string skel = SkeletonKey(a);
        const std::string text = "RAVROLES 3 flags=x\n"
                                 "# written by a later RAV\n"
                                 "map skeleton=" + skel + " role=left_hand side=L bone=mixamorig:LeftHand\n"
                                 "map skeleton=" + skel + " role=left_heel weight=1 bone=mixamorig:Left Foot  x\n"
                                 "alias LeftFoot=heel\n";
        RoleMapFile f;
        CHECK(ParseRoleMap(text, &f));
        CHECK(f.version == 3);
        CHECK(SerializeRoleMap(f) == text);
        SetRoleInFile(f, skel, "left_heel", "mixamorig:LeftFoot");
        const std::string out = SerializeRoleMap(f);
        CHECK(f.header_rest == " flags=x");
        // 10-3e: the edited line keeps its fields and moves last (the newest pick).
        CHECK(out == "RAVROLES 3 flags=x\n"
                     "# written by a later RAV\n"
                     "map skeleton=" + skel + " role=left_hand side=L bone=mixamorig:LeftHand\n"
                     "alias LeftFoot=heel\n"
                     "map skeleton=" + skel + " role=left_heel weight=1 bone=mixamorig:LeftFoot\n");

        // On disk: a set keeps the other lines.
        const fs::path p = fs::u8path(RoleMapPath(R0));
        std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
        std::string err;
        CHECK(SetRoleBone(R0, a, Role::LeftToe, "mixamorig:LeftToeBase", &err));
        const std::string disk = ReadAll(p);
        CHECK(disk.compare(0, text.size(), text) == 0);
        CHECK(disk.find("role=left_toe bone=mixamorig:LeftToeBase\n") != std::string::npos);
        CHECK(GetRoleMapping(R0, a)[R(Role::LeftHeel)] == -1);  // "mixamorig:Left Foot  x" is not a bone here
    }

    // A file that does not read is kept aside, never lost.
    {
        const fs::path p = fs::u8path(RoleMapPath(R0));
        std::ofstream(p, std::ios::binary | std::ios::trunc) << "garbage";
        CHECK(GetRoleMapping(R0, a) == GuessRoleMapping(a));
        std::string err;
        CHECK(SetRoleBone(R0, a, Role::Hips, "mixamorig:Hips", &err));
        fs::path bad = p;
        bad += ".bad";
        CHECK(ReadAll(bad) == "garbage");
        RoleMapFile f;
        CHECK(ParseRoleMap(ReadAll(p), &f));

        // A second bad file never overwrites the first one kept aside.
        std::ofstream(p, std::ios::binary | std::ios::trunc) << "garbage 2";
        CHECK(SetRoleBone(R0, a, Role::Hips, "mixamorig:Hips", &err));
        fs::path bad2 = p;
        bad2 += ".bad2";
        CHECK(ReadAll(bad) == "garbage" && ReadAll(bad2) == "garbage 2");
        CHECK(ParseRoleMap(ReadAll(p), &f));
    }

    // A file that exists but cannot be read is never replaced: the write is refused.
    {
        const fs::path p = fs::u8path(RoleMapPath(R0));
        fs::remove(p);
        fs::create_directories(p);  // a folder where the file should be
        std::string err;
        CHECK(!SetRoleBone(R0, a, Role::Hips, "mixamorig:Hips", &err) && !err.empty());
        err.clear();
        CHECK(!ClearRole(R0, a, Role::Hips, &err) && !err.empty());
        CHECK(fs::is_directory(p));
        CHECK(GetRoleMapping(R0, a) == GuessRoleMapping(a));
        fs::remove(p);

#ifndef _WIN32
        // Permissions (skipped as root, which reads anything).
        std::ofstream(p, std::ios::binary | std::ios::trunc) << "RAVROLES 1\n";
        fs::permissions(p, fs::perms::none);
        if (!std::ifstream(p)) {
            err.clear();
            CHECK(!SetRoleBone(R0, a, Role::Hips, "mixamorig:Hips", &err) && !err.empty());
            fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write);
            CHECK(ReadAll(p) == "RAVROLES 1\n");
        }
        fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write);

        // The bad file cannot be moved aside (read-only folder): refused, the file stays.
        std::ofstream(p, std::ios::binary | std::ios::trunc) << "garbage 3";
        const fs::path dir = p.parent_path();
        fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec);
        fs::path probe = dir / "probe";
        const bool dir_locked = !std::ofstream(probe);
        if (dir_locked) {
            err.clear();
            CHECK(!SetRoleBone(R0, a, Role::Hips, "mixamorig:Hips", &err) && !err.empty());
            CHECK(ReadAll(p) == "garbage 3");
        }
        fs::permissions(dir, fs::perms::owner_all);
        std::error_code pec;
        fs::remove(probe, pec);
#endif
    }

    // ---- 10-3d: the Skeleton & roles window ----
    {
        const fs::path p = fs::u8path(RoleMapPath(R0));
        std::error_code rec;
        fs::remove_all(p, rec);
        std::string err;

        // The file's status.
        CHECK(GetRoleMapStatus(R0) == RoleMapStatus::Absent);
        CHECK(SetRoleBone(R0, a, Role::LeftToe, "mixamorig:LeftToe_End", &err));
        CHECK(GetRoleMapStatus(R0) == RoleMapStatus::Ok);
        std::ofstream(p, std::ios::binary | std::ios::trunc) << "garbage";
        CHECK(GetRoleMapStatus(R0) == RoleMapStatus::NotRolesFile);
        fs::remove(p);
        fs::create_directories(p);  // exists, cannot be read
        CHECK(GetRoleMapStatus(R0) == RoleMapStatus::Unreadable);
        {
            std::vector<char> st;
            CHECK(GetRoleMapping(R0, a, &st) == GuessRoleMapping(a));
            for (char s : st) CHECK(!s);
            const std::vector<StoredRoleEntry> e = GetStoredRoles(R0, a);
            CHECK(e.size() == static_cast<size_t>(Role::Count));
            for (const StoredRoleEntry& x : e) CHECK(!x.stored);
        }
        fs::remove(p);

        // A stored bone this rig does not have: no bone, but stored.
        CHECK(SetRoleBone(R0, a, Role::LeftHeel, "Bip01 L Foot", &err));
        {
            std::vector<char> st;
            const std::vector<int> m = GetRoleMapping(R0, a, &st);
            CHECK(m[R(Role::LeftHeel)] == -1 && st[R(Role::LeftHeel)]);
            CHECK(m[R(Role::LeftToe)] == 4 && !st[R(Role::LeftToe)]);
        }

        // The stored entry, exactly: none, "" (no bone), a bone absent from the rig.
        CHECK(SetRoleBone(R0, a, Role::RightToe, "", &err));
        {
            const StoredRoleEntry none = GetStoredRole(R0, a, Role::LeftToe);
            CHECK(!none.stored && none.bone.empty());
            const StoredRoleEntry empty = GetStoredRole(R0, a, Role::RightToe);
            CHECK(empty.stored && empty.bone.empty());
            const StoredRoleEntry absent = GetStoredRole(R0, a, Role::LeftHeel);
            CHECK(absent.stored && absent.bone == "Bip01 L Foot");
            CHECK(GetStoredRole(R0, b, Role::LeftHeel) == absent);  // the same skeleton
            CHECK(!GetStoredRole(R0, other, Role::LeftHeel).stored);
        }

        // The window's undo: each change records the entry it replaces; restoring them in
        // reverse gives the original mapping back (a cleared line comes back at the end).
        const std::vector<StoredRoleEntry> original = GetStoredRoles(R0, a);
        const std::vector<int> orig_map = GetRoleMapping(R0, a);
        struct Step { Role role; StoredRoleEntry prev; };
        std::vector<Step> undo;
        // The window's rule (ApplyRoleChange): a step only when the write went through and the
        // stored entry changed.
        auto change = [&](Role r, RoleChangeKind kind, const std::string& bone, bool want_step) {
            StoredRoleEntry prev;
            bool            changed = true;
            const bool      ok = ApplyRoleChange(R0, a, r, kind, bone, &prev, &changed, &err);
            CHECK(ok);
            CHECK(changed == want_step);
            if (ok && changed) undo.push_back({r, prev});
        };
        change(Role::LeftToe, RoleChangeKind::Auto, "", false);                      // auto on an unstored role: no step
        change(Role::LeftToe, RoleChangeKind::Bone, "mixamorig:LeftFoot", true);     // auto -> a bone
        change(Role::LeftToe, RoleChangeKind::Bone, "mixamorig:LeftFoot", false);    // the same bone again: no step
        change(Role::LeftToe, RoleChangeKind::Bone, "mixamorig:LeftToe_End", true);  // a bone -> another
        change(Role::LeftHeel, RoleChangeKind::Auto, "", true);                      // an absent bone -> auto
        change(Role::RightToe, RoleChangeKind::Bone, "mixamorig:Hips", true);        // none -> a bone
        change(Role::Hips, RoleChangeKind::None, "", true);                          // auto -> none
        change(Role::Hips, RoleChangeKind::None, "", false);                         // none again: no step
        CHECK(undo.size() == 5);
        CHECK(GetRoleMapping(R0, a)[R(Role::Hips)] == -1);
        CHECK(GetRoleMapping(R0, a)[R(Role::LeftToe)] == 5);
        while (!undo.empty()) {
            CHECK(RestoreRole(R0, a, undo.back().role, undo.back().prev, &err));
            undo.pop_back();
        }
        CHECK(GetStoredRoles(R0, a) == original);
        CHECK(GetRoleMapping(R0, a) == orig_map);
        CHECK(GetStoredRole(R0, a, Role::LeftHeel).bone == "Bip01 L Foot");

        // A refused restore leaves the file alone and says why.
        fs::remove(p);
        fs::create_directories(p);
        err.clear();
        CHECK(!RestoreRole(R0, a, Role::LeftToe, StoredRoleEntry{true, "x"}, &err) && !err.empty());
        err.clear();
        CHECK(!RestoreRole(R0, a, Role::LeftToe, StoredRoleEntry{}, &err) && !err.empty());
        CHECK(fs::is_directory(p));
        fs::remove(p);
    }

    // ---- Story 10-3e: custom roles ----
    {
        const fs::path    croot = root / "custom";
        const std::string C = croot.u8string();
        const fs::path    p = fs::u8path(RoleMapPath(C));
        std::string       key, err, old_name, name;
        int               index = -1;

        // Create: key from the name, name cleaned and lower-cased; listed.
        CHECK(GetCustomRoles(C).empty());
        CHECK(AddCustomRole(C, "  Sword   Tip ", &key, &err) && key == "sword_tip");
        CHECK(GetCustomRoles(C) == (std::vector<CustomRole>{{"sword_tip", "sword tip"}}));
        CHECK(ReadAll(p).find("role key=sword_tip name=sword tip\n") != std::string::npos);

        // Refused: empty key, a built-in key / name / short label, a custom key or name taken.
        const std::string before = ReadAll(p);
        err.clear();
        CHECK(!AddCustomRole(C, "", &key, &err) && !err.empty());
        err.clear();
        CHECK(!AddCustomRole(C, " -- ", &key, &err) && !err.empty());
        err.clear();
        CHECK(!AddCustomRole(C, "\xC3\xA9\xC3\xA8", &key, &err) && !err.empty());
        err.clear();
        CHECK(!AddCustomRole(C, "Left Heel", &key, &err) && !err.empty());
        err.clear();
        CHECK(!AddCustomRole(C, "L heel", &key, &err) && !err.empty());
        err.clear();
        CHECK(!AddCustomRole(C, "left-heel", &key, &err) && !err.empty());  // key left_heel
        err.clear();
        CHECK(!AddCustomRole(C, "sword-tip", &key, &err) && !err.empty());  // key sword_tip taken
        err.clear();
        CHECK(!AddCustomRole(C, "SWORD TIP", &key, &err) && !err.empty());
        CHECK(ReadAll(p) == before);

        // Map it per skeleton, by key, like a built-in role.
        const std::vector<std::string> rigA = {"Hips", "Spine", "weapon_r"};
        const std::vector<std::string> rigB = {"mixamorig:Hips", "mixamorig:Weapon_R", "mixamorig:Hand_R"};
        const std::vector<std::string> rigC = {"pelvis", "hand_r"};
        CHECK(AddCustomRole(C, "Blade Root", &key, &err) && key == "blade_root");
        RoleMapping rm = GetFullRoleMapping(C, rigA, {});
        CHECK(rm.roles.size() == 2 && rm.custom.size() == 2 && rm.custom["sword_tip"] == -1);
        CHECK(rm.names["sword_tip"] == "sword tip" && rm.builtin.size() == static_cast<size_t>(Role::Count));
        StoredRoleEntry prev;
        bool            changed = false;
        CHECK(ApplyRoleChange(C, rigA, "sword_tip", RoleChangeKind::Bone, "weapon_r", &prev, &changed, &err));
        CHECK(changed && !prev.stored);
        CHECK(GetFullRoleMapping(C, rigA, {}).custom["sword_tip"] == 2);
        CHECK(GetStoredRole(C, rigA, std::string("sword_tip")) == (StoredRoleEntry{true, "weapon_r"}));

        // Learned guess: rig B has no entry; its Weapon_R matches by normalised name -> auto.
        RoleMapFile f = ReadRoleMapFile(C);
        bool        stored = true;
        CHECK(ResolveRoleKey(f, rigB, "sword_tip", &stored) == 1 && !stored);
        CHECK(GuessRoleKey(f, rigB, "sword_tip") == 1);
        CHECK(GetFullRoleMapping(C, rigB, {}).custom["sword_tip"] == 1);
        CHECK(!GetStoredRole(C, rigB, std::string("sword_tip")).stored);
        // No similar bone: no guess.
        CHECK(GetFullRoleMapping(C, rigC, {}).custom["sword_tip"] == -1);
        // Newest pick first: a later pick of hand_r on another rig wins on a rig that has both.
        const std::vector<std::string> rigD = {"Hand_R", "Spine2"};
        CHECK(SetRoleBone(C, rigD, std::string("sword_tip"), "Hand_R", &err));
        f = ReadRoleMapFile(C);
        CHECK(ResolveRoleKey(f, rigB, "sword_tip") == 2);  // hand_r (the newest pick) before weapon_r
        CHECK(ResolveRoleKey(f, rigC, "sword_tip") == 1);
        // A "no bone" pick is never learned; a stored entry wins over the guess.
        CHECK(SetRoleBone(C, rigC, std::string("sword_tip"), "", &err));
        f = ReadRoleMapFile(C);
        CHECK(ResolveRoleKey(f, rigC, "sword_tip", &stored) == -1 && stored);
        CHECK(ResolveRoleKey(f, rigB, "sword_tip") == 2);
        CHECK(SetRoleBone(C, rigB, std::string("sword_tip"), "mixamorig:Weapon_R", &err));
        f = ReadRoleMapFile(C);
        CHECK(ResolveRoleKey(f, rigB, "sword_tip", &stored) == 1 && stored);
        CHECK(ClearRole(C, rigB, std::string("sword_tip"), &err));
        CHECK(ClearRole(C, rigC, std::string("sword_tip"), &err));
        // A built-in role keeps its table guess (never learned).
        f = ReadRoleMapFile(C);
        CHECK(LearnedRoleGuess(f, rigB, "hips") == -1);
        CHECK(ResolveRoleKey(f, rigB, "hips") == 0);

        // A colleague's key (rules read it, not in the list): mapped when asked for.
        rm = GetFullRoleMapping(C, rigA, {"tail_end", "hips", "sword_tip"});
        CHECK(rm.custom.count("tail_end") == 1 && rm.custom["tail_end"] == -1 && rm.custom.count("hips") == 0);
        CHECK(rm.names.count("tail_end") == 0);
        CHECK(ApplyRoleChange(C, rigA, "tail_end", RoleChangeKind::Bone, "Spine", &prev, &changed, &err) && changed);
        CHECK(GetFullRoleMapping(C, rigA, {"tail_end"}).custom["tail_end"] == 1);
        CHECK(GetCustomRoles(C).size() == 2);  // mapping a key does not define it

        // Rename: the key stays, the map lines stay, the name changes; a taken name is refused.
        CHECK(RenameCustomRole(C, "sword_tip", "Blade Tip", &old_name, &err) && old_name == "sword tip");
        CHECK(GetCustomRoles(C)[0] == (CustomRole{"sword_tip", "blade tip"}));
        CHECK(GetFullRoleMapping(C, rigA, {}).custom["sword_tip"] == 2);
        err.clear();
        CHECK(!RenameCustomRole(C, "sword_tip", "blade root", &old_name, &err) && !err.empty());
        err.clear();
        CHECK(!RenameCustomRole(C, "sword_tip", "hips", &old_name, &err) && !err.empty());
        err.clear();
        CHECK(!RenameCustomRole(C, "sword_tip", "--", &old_name, &err) && !err.empty());
        err.clear();
        CHECK(!RenameCustomRole(C, "tail_end", "tail", &old_name, &err) && !err.empty());  // not in the list
        CHECK(RenameCustomRole(C, "sword_tip", "Blade Tip", &old_name, &err));             // its own name: allowed
        CHECK(GetCustomRoles(C)[0].name == "blade tip");
        // A name whose key would be another role's key is fine on a rename (the key never changes).
        CHECK(RenameCustomRole(C, "sword_tip", "blade-root x", &old_name, &err));
        CHECK(RestoreCustomRole(C, "sword_tip", "blade tip", -1, &err));

        // Delete: the definition goes, its map lines stay; re-creating brings the bone back.
        CHECK(DeleteCustomRole(C, "sword_tip", &name, &index, &err) && name == "blade tip" && index == 0);
        CHECK(GetCustomRoles(C) == (std::vector<CustomRole>{{"blade_root", "blade root"}}));
        CHECK(ReadAll(p).find("role=sword_tip bone=weapon_r") != std::string::npos);
        CHECK(GetFullRoleMapping(C, rigA, {"sword_tip"}).custom["sword_tip"] == 2);  // still mappable by key
        CHECK(DeleteCustomRole(C, "nothing_here", nullptr, nullptr, &err));         // not defined: nothing to do
        // Undo of the delete: back in its place, with its name.
        CHECK(RestoreCustomRole(C, "sword_tip", "blade tip", index, &err));
        CHECK(GetCustomRoles(C) ==
              (std::vector<CustomRole>{{"sword_tip", "blade tip"}, {"blade_root", "blade root"}}));
        CHECK(!RestoreCustomRole(C, "hips", "x", -1, &err));  // a built-in key is never defined
        // Re-create after a delete: the bones come back.
        CHECK(DeleteCustomRole(C, "sword_tip", nullptr, nullptr, &err));
        CHECK(AddCustomRole(C, "Sword Tip", &key, &err) && key == "sword_tip");
        CHECK(GetFullRoleMapping(C, rigA, {}).custom["sword_tip"] == 2);

        // Undo round trip: create, map, rename, then undo x3 = the file as it was.
        const std::string start = ReadAll(p);
        CHECK(AddCustomRole(C, "Tail Tip", &key, &err) && key == "tail_tip");
        CHECK(ApplyRoleChange(C, rigA, key, RoleChangeKind::Bone, "Spine", &prev, &changed, &err) && changed);
        CHECK(RenameCustomRole(C, key, "Tail End Tip", &old_name, &err) && old_name == "tail tip");
        CHECK(RestoreCustomRole(C, key, old_name, -1, &err));  // undo the rename
        CHECK(GetCustomRoles(C).back() == (CustomRole{"tail_tip", "tail tip"}));
        CHECK(RestoreRole(C, rigA, key, prev, &err));          // undo the mapping
        CHECK(!GetStoredRole(C, rigA, key).stored);
        CHECK(DeleteCustomRole(C, key, nullptr, nullptr, &err));  // undo the create
        CHECK(ReadAll(p) == start);

        // Unknown fields and lines round-trip; `name` runs to the end; the last line's name wins;
        // a `role` line without a key, or with a built-in key, is not a custom role.
        const std::string text = "RAVROLES 1\n"
                                 "role key=fx_a future=1 name=FX  a = b\n"
                                 "role name=orphan\n"
                                 "role key=hips name=my hips\n"
                                 "role key=fx_a name=fx renamed\n"
                                 "map skeleton=0123456789abcdef role=fx_a bone=Bone 1 tag=x\n";
        RoleMapFile rf;
        CHECK(ParseRoleMap(text, &rf) && SerializeRoleMap(rf) == text);
        CHECK(rf.lines.size() == 5 && rf.lines[0].is_role && rf.lines[0].Get("future") == "1" &&
              rf.lines[0].Get("name") == "FX  a = b" && !rf.lines[1].is_role && rf.lines[2].is_role);
        CHECK(CustomRolesInFile(rf) == (std::vector<CustomRole>{{"fx_a", "fx renamed"}}));
        // A rename keeps the unknown field of the line it edits.
        fs::remove(p);
        {
            std::ofstream o(p, std::ios::binary);
            o << "RAVROLES 1\nrole key=fx_b future=1 name=fx b\n";
        }
        CHECK(RenameCustomRole(C, "fx_b", "FX Bee", &old_name, &err));
        CHECK(ReadAll(p) == "RAVROLES 1\nrole key=fx_b future=1 name=fx bee\n");

        // Unreadable roles.txt: writes refused with a reason, the list reads empty.
        fs::remove(p);
        fs::create_directories(p);
        err.clear();
        CHECK(!AddCustomRole(C, "New One", &key, &err) && !err.empty());
        err.clear();
        CHECK(!RenameCustomRole(C, "fx_b", "x", &old_name, &err) && !err.empty());
        err.clear();
        CHECK(!DeleteCustomRole(C, "fx_b", nullptr, nullptr, &err) && !err.empty());
        CHECK(GetCustomRoles(C).empty());
        CHECK(GetFullRoleMapping(C, rigA, {}).builtin.size() == static_cast<size_t>(Role::Count));
        CHECK(fs::is_directory(p));
    }

    // ---- Story 10-3e review: composition, pick order, rename keys, restore order ----
    {
        const std::string C = (root / "custom2").u8string();
        std::string       key, err, old_name, name, missing;
        int               index = -1;
        StoredRoleEntry   prev;
        bool              changed = false;
        const std::vector<std::string> rigA = {"Hips", "Spine", "weapon_r"};
        const std::vector<int>         parA = {-1, 0, 1};

        // A colleague's key (not in the list) mapped on rig A binds through the composition.
        Block     b;
        Condition c;
        c.signal.bones = {BoneRefId("role:tail_end")};
        b.conditions = {c};
        std::vector<Block> blocks = {b};
        CHECK(!BindBlocksWithRoleMapping(C, blocks, rigA, parA, &missing) && missing == "tail end");
        CHECK(ApplyRoleChange(C, rigA, "tail_end", RoleChangeKind::Bone, "Spine", &prev, &changed, &err) && changed);
        blocks = {b};
        CHECK(BindBlocksWithRoleMapping(C, blocks, rigA, parA, &missing) && missing.empty());
        CHECK(blocks[0].conditions[0].signal.bones == std::vector<int>{1});
        // An unmapped custom role: `missing` uses its name, then its new name after a rename.
        CHECK(AddCustomRole(C, "Fin Tip", &key, &err) && key == "fin_tip");
        Block b2 = b;
        b2.conditions[0].signal.bones = {BoneRefId("role:fin_tip")};
        blocks = {b2};
        CHECK(!BindBlocksWithRoleMapping(C, blocks, rigA, parA, &missing) && missing == "fin tip");
        CHECK(RenameCustomRole(C, "fin_tip", "Dorsal Tip", &old_name, &err));
        blocks = {b2};
        CHECK(!BindBlocksWithRoleMapping(C, blocks, rigA, parA, &missing) && missing == "dorsal tip");
        const RoleMapping rm = RoleMappingForBlocks(C, rigA, {b, b2});
        CHECK(rm.custom.count("tail_end") == 1 && rm.custom.at("tail_end") == 1 && rm.custom.at("fin_tip") == -1);
        CHECK(BoneRefName(BoneRefId("role:fin_tip")) == "dorsal tip");

        // A re-pick moves the pick last: pick on A, pick on D, re-pick on A -> A's bone first.
        const std::vector<std::string> rigD = {"Hand_R", "Spine2"};
        const std::vector<std::string> rigBoth = {"hand_r", "Weapon_R"};
        CHECK(SetRoleBone(C, rigA, std::string("fin_tip"), "Spine", &err));
        CHECK(SetRoleBone(C, rigD, std::string("fin_tip"), "Hand_R", &err));
        CHECK(ResolveRoleKey(ReadRoleMapFile(C), rigBoth, "fin_tip") == 0);
        CHECK(SetRoleBone(C, rigA, std::string("fin_tip"), "weapon_r", &err));
        CHECK(ResolveRoleKey(ReadRoleMapFile(C), rigBoth, "fin_tip") == 1);
        {
            const std::string t = ReadAll(fs::u8path(RoleMapPath(C)));
            CHECK(t.rfind("bone=weapon_r") > t.rfind("bone=Hand_R"));
        }

        // A rename is refused when its key would be a built-in's or another role's.
        CHECK(AddCustomRole(C, "Sword Tip", &key, &err));
        err.clear();
        CHECK(!RenameCustomRole(C, "fin_tip", "left-heel", &old_name, &err) && !err.empty());
        err.clear();
        CHECK(!RenameCustomRole(C, "fin_tip", "sword-tip", &old_name, &err) && !err.empty());
        err.clear();
        CHECK(RenameCustomRole(C, "fin_tip", "Fin-Tip x", &old_name, &err));  // a fresh key: fine
        CHECK(RenameCustomRole(C, "fin_tip", "dorsal tip", &old_name, &err));
        // A new name whose key is a renamed role's first name: says so.
        CHECK(RenameCustomRole(C, "sword_tip", "Blade Tip", &old_name, &err));
        err.clear();
        CHECK(!AddCustomRole(C, "Sword Tip", &key, &err));
        CHECK(err == "\"sword tip\" is taken: it was the first name of \"blade tip\".");

        // Restore keeps the order: delete the middle of 3 and restore it at its index.
        const std::string R2 = (root / "custom3").u8string();
        CHECK(AddCustomRole(R2, "one", &key, &err) && AddCustomRole(R2, "two", &key, &err) &&
              AddCustomRole(R2, "three", &key, &err));
        const std::vector<CustomRole> three = GetCustomRoles(R2);
        CHECK(DeleteCustomRole(R2, "two", &name, &index, &err) && index == 1 && name == "two");
        CHECK(GetCustomRoles(R2) == (std::vector<CustomRole>{{"one", "one"}, {"three", "three"}}));
        CHECK(RestoreCustomRole(R2, "two", name, index, &err));
        CHECK(GetCustomRoles(R2) == three);
        // An index past the end appends.
        CHECK(DeleteCustomRole(R2, "one", &name, &index, &err) && index == 0);
        CHECK(RestoreCustomRole(R2, "one", name, 7, &err));
        CHECK(GetCustomRoles(R2) == (std::vector<CustomRole>{{"two", "two"}, {"three", "three"}, {"one", "one"}}));
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    // ---- 10-5 frozen fixtures ----
    // A roles file written by the 10-5 build (two skeletons, one role set to "no bone"),
    // committed as text and NEVER regenerated: it must read and write back byte-identical.
    {
        const std::string fx = ReadFixture("roles_v1_10_5.txt");
        CHECK(!fx.empty());
        RoleMapFile f;
        CHECK(ParseRoleMap(fx, &f));
        const std::string back = SerializeRoleMap(f);
        CHECK(back == fx);
        if (back != fx) std::printf("--- got ---\n%s--- want ---\n%s", back.c_str(), fx.c_str());
        CHECK(f.version == 1 && f.header_rest.empty() && f.lines.size() == 5);
        bool all_map = true;
        for (const RoleMapLine& l : f.lines) all_map = all_map && l.is_map;
        CHECK(all_map);
        if (f.lines.size() == 5) {
            CHECK(f.lines[0].Get("skeleton") == "0123456789abcdef" && f.lines[0].Get("role") == "left_heel" &&
                  f.lines[0].Get("bone") == "mixamorig:LeftFoot");
            CHECK(f.lines[1].Get("skeleton") == "0123456789abcdef" && f.lines[1].Get("role") == "left_toe" &&
                  f.lines[1].Get("bone") == "mixamorig:LeftToeBase");
            CHECK(f.lines[2].Get("role") == "right_toe" && f.lines[2].Get("bone").empty());
            CHECK(f.lines[3].Get("skeleton") == "fedcba9876543210" && f.lines[3].Get("role") == "hips" &&
                  f.lines[3].Get("bone") == "Root Hips");
            CHECK(f.lines[4].Get("skeleton") == "fedcba9876543210" && f.lines[4].Get("role") == "right_heel" &&
                  f.lines[4].Get("bone") == "R_Ankle");
        }
    }

    if (g_fails) {
        std::printf("role_map_store_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("role_map_store_test: all passed\n");
    return 0;
}
