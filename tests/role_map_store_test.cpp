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
        CHECK(out == "RAVROLES 3 flags=x\n"
                     "# written by a later RAV\n"
                     "map skeleton=" + skel + " role=left_hand side=L bone=mixamorig:LeftHand\n"
                     "map skeleton=" + skel + " role=left_heel weight=1 bone=mixamorig:LeftFoot\n"
                     "alias LeftFoot=heel\n");

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
