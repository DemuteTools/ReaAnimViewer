// SPDX-License-Identifier: MIT
//
// Host test of the reference markers (src/ref_labels.h, spike 10-7a): "REF" / "REF <label>"
// names, the CSV's '# ref.<label>=' / '# ref_bone.<label>=' lines written and read back, and
// the reports' "REF by bone" line. No REAPER, no Windows.
//   c++ -std=c++17 -I src tests/ref_labels_test.cpp src/bone_events.cpp src/role_map_store.cpp src/rule_record.cpp -o t && ./t

#include "ref_labels.h"

#include <cstdio>
#include <string>

using namespace rav;

namespace {

int g_fails = 0;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);        \
            ++g_fails;                                              \
        }                                                           \
    } while (0)

bool Is(const char* name, const char* want_label)
{
    std::string label = "unset";
    return ParseRefMarkerName(name, &label) && label == want_label;
}

bool IsNot(const char* name)
{
    std::string label = "unset";
    return !ParseRefMarkerName(name, &label) && label == "unset";
}

// Feeds "# key=value\n" lines to ReadRefHeader, as detection_eval's ReadClip does.
RefTimes ReadLines(const std::string& text)
{
    RefTimes r;
    size_t   a = 0;
    while (a < text.size()) {
        size_t e = text.find('\n', a);
        if (e == std::string::npos) e = text.size();
        const std::string line = text.substr(a, e - a);
        a = e + 1;
        if (line.size() < 2 || line.compare(0, 2, "# ") != 0) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        ReadRefHeader(line.substr(2, eq - 2), line.substr(eq + 1), &r);
    }
    return r;
}

}  // namespace

int main()
{
    // Bare REF, any case, surrounding whitespace ignored.
    CHECK(Is("REF", ""));
    CHECK(Is("ref", ""));
    CHECK(Is("  Ref \t", ""));
    // A label is read as a role key.
    CHECK(Is("REF left_hand", "left_hand"));
    CHECK(Is("REF Left Hand", "left_hand"));
    CHECK(Is("ref\tLEFT-HAND ", "left_hand"));
    CHECK(Is("REF  right_heel", "right_heel"));
    CHECK(Is("REF sword tip 2", "sword_tip_2"));
    // A bone name reads the same way (ResolveRefBones matches it to the bone).
    CHECK(Is("REF LeftToe_End", "lefttoe_end"));
    CHECK(Is("REF mixamorig:LeftHand", "mixamorig_lefthand"));
    // A label a key keeps nothing of reads as a bare REF.
    CHECK(Is("REF ?", ""));
    // Not a reference marker.
    CHECK(IsNot("REFERENCE"));
    CHECK(IsNot("REF:left_hand"));
    CHECK(IsNot("REF_left_hand"));
    CHECK(IsNot("XREF"));
    CHECK(IsNot("RE"));
    CHECK(IsNot("RAV?"));
    CHECK(IsNot(""));
    CHECK(IsNot("   "));
    CHECK(IsNot(nullptr));
    CHECK(ParseRefMarkerName("REF x", nullptr));  // no label wanted

    // Add: every time goes to `all`, labelled ones to their label too.
    RefTimes r;
    r.Add("", 0.5);
    r.Add("left_hand", 1.25);
    r.Add("right_hand", 2.0);
    r.Add("left_hand", 3.0);
    CHECK(r.all.size() == 4);
    CHECK(r.by_label.size() == 2);
    CHECK(r.by_label["left_hand"].size() == 2);
    CHECK(r.by_label["right_hand"].size() == 1);
    r.bone["left_hand"] = "mixamorig:LeftHand";
    r.bone["right_hand"] = "";

    // The CSV lines, in label order, each pair ending with the bone ("" = none).
    const std::string lines = RefLabelLines(r);
    CHECK(lines ==
          "# ref.left_hand=1.250000,3.000000\n"
          "# ref_bone.left_hand=mixamorig:LeftHand\n"
          "# ref.right_hand=2.000000\n"
          "# ref_bone.right_hand=\n");
    CHECK(RefLabelLines(RefTimes{}).empty());

    // Written then read back, with the '# ref=' line before them.
    const RefTimes back = ReadLines("# ref=0.500000,1.250000,2.000000,3.000000\n" + lines);
    CHECK(back.all.size() == 4);
    CHECK(back.by_label.size() == 2);
    CHECK(back.by_label.count("left_hand") && back.by_label.at("left_hand").size() == 2);
    CHECK(back.by_label.count("left_hand") && back.by_label.at("left_hand")[1] == 3.0);
    CHECK(back.by_label.count("right_hand") && back.by_label.at("right_hand")[0] == 2.0);
    CHECK(back.bone.count("left_hand") && back.bone.at("left_hand") == "mixamorig:LeftHand");
    CHECK(back.bone.count("right_hand") && back.bone.at("right_hand").empty());

    // An older dump: '# ref=' only.
    const RefTimes old = ReadLines("# item=a.fbx\n# ref=1.000000,2.000000\n# parent=-1,0\n");
    CHECK(old.all.size() == 2);
    CHECK(old.by_label.empty());
    CHECK(RefLabelsText(old).empty());

    // Other keys, an empty label, and times that are not numbers.
    RefTimes o;
    CHECK(!ReadRefHeader("item", "x", &o));
    CHECK(!ReadRefHeader("parent", "-1,0", &o));
    CHECK(!ReadRefHeader("ref.", "1.0", &o));
    CHECK(!ReadRefHeader("ref_bone.", "Hand", &o));
    CHECK(!ReadRefHeader("refx", "1.0", &o));
    CHECK(ReadRefHeader("ref", "", &o));
    CHECK(o.all.empty());
    CHECK(ReadRefHeader("ref.left_foot", "1.0,x,,2.5", &o));
    CHECK(o.by_label["left_foot"].size() == 2);
    CHECK(o.by_label.size() == 1);
    CHECK(o.all.empty());  // a label's line never adds to '# ref='
    // A bone name keeps its '=' and ',' (the value is the rest of the line).
    CHECK(ReadRefHeader("ref_bone.left_foot", "a=b,c", &o));
    CHECK(o.bone["left_foot"] == "a=b,c");

    // The reports' line: per label its count and bone, then the unlabelled count.
    CHECK(RefLabelsText(r) ==
          "  REF by bone: left_hand 2 (mixamorig:LeftHand), right_hand 1 (no bone plays this role), unlabelled 1\n");
    RefTimes only;
    only.Add("left_foot", 1.0);
    CHECK(RefLabelsText(only) == "  REF by bone: left_foot 1 (no bone plays this role)\n");
    CHECK(RefLabelsText(RefTimes{}).empty());

    // ResolveRefBones: each label's bone as the Tagging view maps roles (stored, else guessed).
    {
        const std::vector<std::string> names = {"mixamorig:Hips",        "mixamorig:LeftFoot",  "mixamorig:RightFoot",
                                                "mixamorig:LeftHand",    "mixamorig:RightHand", "mixamorig:LeftToe_End",
                                                "mixamorig:RightToe_End"};
        const std::string              skel = SkeletonKey(names);
        RoleMapFile                    roles;
        CHECK(ParseRoleMap("RAVROLES 1\n"
                           "role key=left_hand name=Left hand\n"
                           "role key=tail name=tail\n"
                           "map skeleton=" + skel + " role=left_hand bone=mixamorig:LeftHand\n"
                           "map skeleton=" + skel + " role=left_heel bone=mixamorig:RightFoot\n",
                           &roles));
        RefTimes t;
        t.Add("left_heel", 1.0);   // built-in, stored for this skeleton
        t.Add("right_heel", 1.5);  // built-in, guessed (Mixamo heel = Foot)
        t.Add("left_hand", 2.0);   // once custom, built-in since: its stored line still plays it
        t.Add("left_knee", 2.5);   // built-in, no bone on this rig
        t.Add("tail", 3.0);        // custom, no bone on this rig
        t.Add("lft_heel", 3.5);    // a typo: no role and no bone has this name
        t.Add("lefttoe_end", 4.0);          // "REF LeftToe_End": a bone, no role needed
        t.Add("mixamorig_righthand", 4.5);  // "REF mixamorig:RightHand": a bone with its namespace
        t.bone["stale"] = "x";     // cleared
        ResolveRefBones(roles, names, &t);
        CHECK(t.bone.size() == 8);
        CHECK(t.bone["left_heel"] == "mixamorig:RightFoot");
        CHECK(t.bone["right_heel"] == "mixamorig:RightFoot");
        CHECK(t.bone["left_hand"] == "mixamorig:LeftHand");
        CHECK(t.bone["left_knee"].empty());
        CHECK(t.bone["tail"].empty());
        CHECK(t.bone["lft_heel"].empty());
        CHECK(t.bone["lefttoe_end"] == "mixamorig:LeftToe_End");
        CHECK(t.bone["mixamorig_righthand"] == "mixamorig:RightHand");
        CHECK(t.unknown.size() == 1 && t.unknown.count("lft_heel"));
        CHECK(RefLabelsText(t) ==
              "  REF by bone: left_hand 1 (mixamorig:LeftHand), left_heel 1 (mixamorig:RightFoot), left_knee 1 (no "
              "bone plays this role), lefttoe_end 1 (mixamorig:LeftToe_End), lft_heel 1 (no role or bone has this "
              "name), mixamorig_righthand 1 (mixamorig:RightHand), right_heel 1 (mixamorig:RightFoot), tail 1 (no "
              "bone plays this role)\n");
        // Its bones are what the dump writes.
        CHECK(RefLabelLines(t).find("# ref_bone.left_hand=mixamorig:LeftHand\n") != std::string::npos);
    }

    // The toe tip, arm and body roles are role keys with no roles.txt: guessed on Mixamo names.
    // A role no bone plays is still a role (never "no role or bone has this name").
    {
        const std::vector<std::string> names = {"mixamorig:Hips", "mixamorig1_LeftToe_End", "mixamorig:RightHand",
                                                "mixamorig:Spine2"};
        RefTimes    t;
        std::string label;
        CHECK(ParseRefMarkerName("REF Left Toe End", &label) && label == "left_toe_end");
        t.Add(label, 1.0);
        CHECK(ParseRefMarkerName("ref RIGHT-HAND", &label) && label == "right_hand");
        t.Add(label, 2.0);
        t.Add("chest", 3.0);
        t.Add("head", 4.0);
        ResolveRefBones(RoleMapFile{}, names, &t);
        CHECK(t.bone["left_toe_end"] == "mixamorig1_LeftToe_End");
        CHECK(t.bone["right_hand"] == "mixamorig:RightHand");
        CHECK(t.bone["chest"] == "mixamorig:Spine2");
        CHECK(t.bone["head"].empty() && t.unknown.empty());
    }

    // Story 10-8n: event labels "REF <kind> <side>".
    {
        const auto ev = [](const char* name, RefEventKind want_kind, char want_side) {
            std::string  label;
            RefEventKind k = RefEventKind::Release;
            char         sd = 0;
            return ParseRefMarkerName(name, &label) && ParseEventRefLabel(label, &k, &sd) && k == want_kind &&
                   sd == want_side;
        };
        CHECK(ev("REF lift L", RefEventKind::Lift, 'L'));
        CHECK(ev("REF liftoff right", RefEventKind::Lift, 'R'));
        CHECK(ev("REF Lift Off left", RefEventKind::Lift, 'L'));
        CHECK(ev("REF lift-off_R", RefEventKind::Lift, 'R'));
        CHECK(ev("REF Slide right", RefEventKind::Slide, 'R'));
        CHECK(ev("REF PIVOT l", RefEventKind::Pivot, 'L'));
        CHECK(ev("REF grab_l", RefEventKind::Grab, 'L'));
        CHECK(ev("ref release-R", RefEventKind::Release, 'R'));
        CHECK(!IsEventRefLabel("lift_x"));
        CHECK(!IsEventRefLabel("lift"));
        CHECK(!IsEventRefLabel("left_heel"));
        CHECK(!IsEventRefLabel("grab"));
        CHECK(!IsEventRefLabel("_l"));
        CHECK(!IsEventRefLabel(""));

        RefTimes t;
        t.Add("lift_l", 1.2);
        t.Add("left_heel", 1.5);
        t.Add("", 2.0);
        t.Add("lift_x", 2.5);  // not an event: a contact label (unknown)
        CHECK(t.all.size() == 3 && t.all[0] == 1.5 && t.all[1] == 2.0 && t.all[2] == 2.5);
        CHECK(t.by_label["lift_l"].size() == 1);
        ResolveRefBones(RoleMapFile{}, {"mixamorig:Hips", "mixamorig:LeftFoot"}, &t);
        CHECK(t.bone.count("lift_l") && t.bone["lift_l"].empty());
        CHECK(t.unknown.count("lift_l") == 0 && t.unknown.count("lift_x") == 1);
        const std::string txt = RefLabelsText(t);
        CHECK(txt.find("lift_l 1 (event)") != std::string::npos);
        CHECK(txt.find("unlabelled 1") != std::string::npos);

        // Dump and read back: the event label's line reads back into by_label, with no bone.
        const RefTimes back = ReadLines("# ref=1.5,2.0,2.5\n" + RefLabelLines(t));
        CHECK(back.all.size() == 3);
        CHECK(back.by_label.count("lift_l") && back.by_label.at("lift_l").size() == 1 &&
              back.by_label.at("lift_l")[0] == 1.2);
        CHECK(back.bone.count("lift_l") && back.bone.at("lift_l").empty());
    }

    // Story 10-9a: the sideless "REF jump" / "REF land" (side B); a side suffix is no event label.
    {
        const auto ev = [](const char* name, RefEventKind want_kind) {
            std::string  label;
            RefEventKind k = RefEventKind::Lift;
            char         sd = 0;
            return ParseRefMarkerName(name, &label) && ParseEventRefLabel(label, &k, &sd) && k == want_kind && sd == 'B';
        };
        CHECK(ev("REF jump", RefEventKind::Jump));
        CHECK(ev("REF Jump", RefEventKind::Jump));
        CHECK(ev("  ref LAND ", RefEventKind::Land));
        CHECK(IsEventRefLabel("jump") && IsEventRefLabel("land"));
        CHECK(!IsEventRefLabel("jump_l") && !IsEventRefLabel("land_r") && !IsEventRefLabel("jumps"));
        RefTimes t;
        t.Add("jump", 0.5);
        t.Add("land", 1.2);
        t.Add("left_toe", 1.2);
        CHECK(t.all.size() == 1 && t.by_label["jump"].size() == 1 && t.by_label["land"].size() == 1);
        ResolveRefBones(RoleMapFile{}, {"mixamorig:Hips", "mixamorig:LeftToeBase"}, &t);
        CHECK(t.bone.count("jump") && t.bone["jump"].empty() && t.unknown.empty());
        CHECK(RefLabelsText(t).find("jump 1 (event)") != std::string::npos);
    }

    if (g_fails) {
        std::printf("%d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("ref_labels: all checks passed\n");
    return 0;
}
