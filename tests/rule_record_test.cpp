// SPDX-License-Identifier: MIT
//
// Host test of the per-item rules record (src/rule_record.h, story 10-2): round trip,
// a record written by a future version, neutral defaults, a corrupt record, signal
// names, missing roles; 10-6: the pool line and the pooled-copies helpers. No REAPER, no
// Windows: any C++17 compiler.

#include "rule_record.h"

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <map>
#include <string>
#include <vector>

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

int R(Role r)
{
    return static_cast<int>(r);
}

std::string ReadFixture(const char* name)
{
    std::ifstream in(std::string(RAV_TEST_DATA) + "/" + name, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str(), out;
    for (char c : s)
        if (c != '\r') out += c;  // a CRLF checkout reads the same
    return out;
}

// A record that sets every known field to a non-default value.
ItemRules FullRecord()
{
    ItemRules r;
    r.options.sensitivity = 0.25;
    r.options.edge_margin_ms = 12.5;
    r.options.smooth_ms = 6;
    r.analyse.floor_percentile = 3;
    r.analyse.position_fraction = 0.35;
    r.analyse.speed_percentile = 25;
    r.analyse.margin_ratio = 0.4;
    r.analyse.onset_fraction = 0.2;
    r.analyse.per_bone_floor = true;
    r.analyse.smooth_ms = 6;

    Block b;
    b.marker = "Foot L  (soft)";
    b.color = 0x1000000u | 0x000000u;  // black is a colour
    b.min_hold_ms = 10;
    b.cooldown_ms = 180;
    b.offset_ms = -7.5;
    b.landing = Landing::PeakOf;
    b.peak_condition = 2;
    b.peak_max = false;
    Condition c;
    c.signal.quantity = Quantity::Point;
    c.signal.bones = {R(Role::LeftHeel), BoneRefForBone("mixamorig:Left Toe,End=1%")};
    c.signal.combine = Combine::Highest;
    c.signal.reference = Reference::Bones;
    c.signal.ref_bones = {R(Role::Hips)};
    c.signal.ref_combine = Combine::Lowest;
    c.signal.floor_y = 0.01;
    c.signal.bone_floors = {0.02, -0.003};
    c.signal.measure = Measure::Acceleration;
    c.signal.axis = Axis::Z;
    c.signal.keep_sign = true;
    c.dir = Direction::Above;
    c.threshold = 1e-7;
    c.margin = 123456789.125;
    c.auto_threshold = false;
    b.conditions = {c};
    b.strength_signal.bones = {R(Role::LeftToe)};
    b.strength_signal.measure = Measure::Speed;
    b.strength_sign = -1;
    b.strength_window_ms = 80;

    Block b2;
    b2.marker = "";
    b2.color = 0x1000000u | 0xA1B2C3u;
    Condition y;
    y.signal.quantity = Quantity::Yaw;
    y.signal.bones = {R(Role::RightHeel), R(Role::RightToe)};
    y.signal.measure = Measure::Speed;
    b2.conditions = {y, c};

    r.blocks = {b, b2};
    r.has_preset = true;
    r.preset_copy.id = "user/my steps";
    r.preset_copy.version = 3;
    r.preset_copy.kept_version = 4;
    r.preset_copy.name = "My steps = v3";
    r.preset_copy.blocks = {b2};
    r.preset_copy.options.sensitivity = 0.5;
    r.preset_copy.options.smooth_ms = 4;
    r.preset_copy.analyse.speed_percentile = 12;
    r.preset_copy.analyse.smooth_ms = 4;
    EventEntry ev;
    ev.t = 1;
    ev.kind = EventKind::Detected;
    ev.has_block = false;
    r.events = {ev};
    return r;
}

}  // namespace

int main()
{
    // Round trip: every known field.
    {
        const ItemRules r = FullRecord();
        const std::string s = SerializeItemRules(r);
        ItemRules p;
        CHECK(ParseItemRules(s, &p));
        CHECK(SerializeItemRules(p) == s);
        CHECK(BlocksEqual(p.blocks, r.blocks));
        CHECK(BlocksEqual(p.preset_copy.blocks, r.preset_copy.blocks));
        CHECK(p.blocks[0].marker == "Foot L  (soft)");
        CHECK(p.blocks[0].color == 0x1000000u);
        CHECK(p.blocks[1].color == (0x1000000u | 0xA1B2C3u));
        CHECK(p.blocks[0].conditions[0].signal.bones[1] == BoneRefForBone("mixamorig:Left Toe,End=1%"));
        CHECK(BoneRefKey(p.blocks[0].conditions[0].signal.bones[1]) == "bone:mixamorig:Left Toe,End=1%");
        CHECK(s.find("bone:mixamorig:Left%20Toe%2CEnd%3D1%25") != std::string::npos);
        CHECK(p.options.sensitivity == 0.25 && p.options.edge_margin_ms == 12.5 && p.options.smooth_ms == 6);
        CHECK(p.analyse.per_bone_floor && p.analyse.onset_fraction == 0.2 && p.analyse.smooth_ms == 6);
        CHECK(p.has_preset && p.preset_copy.id == "user/my steps" && p.preset_copy.version == 3 &&
              p.preset_copy.kept_version == 4 && p.preset_copy.name == "My steps = v3");
        CHECK(p.events.size() == 1 && p.events[0].kind == EventKind::Detected && p.events[0].t == 1);
        CHECK(s.find("\nevent t=1 kind=detected\n") != std::string::npos);
        CHECK(OptionsEqual(p.preset_copy.options, r.preset_copy.options) &&
              AnalyseEqual(p.preset_copy.analyse, r.preset_copy.analyse) && p.preset_copy.analyse.smooth_ms == 4);
        CHECK(p.format_version == 1);
        CHECK(s.compare(0, 11, "RAVRULES 1\n") == 0);
        CHECK(!HasKeptText(p));
    }

    // The spec's example lines read as written.
    {
        const std::string s =
            "RAVRULES 1\n"
            "options sensitivity=0 edge_ms=0 smooth_ms=8\n"
            "block color=#3FA7FF hold_ms=0 cooldown_ms=180 offset_ms=0 land=peak:1:max marker=Footstep L\n"
            "cond q=point bones=role:left_heel,role:left_toe comb=lowest ref=floor meas=position axis=vertical "
            "dir=below thr=0.044 margin=0.011 fixed=0\n";
        ItemRules p;
        CHECK(ParseItemRules(s, &p));
        CHECK(p.blocks.size() == 1);
        CHECK(p.blocks[0].marker == "Footstep L");
        CHECK(p.blocks[0].color == (0x1000000u | 0x3FA7FFu));
        CHECK(p.blocks[0].landing == Landing::PeakOf && p.blocks[0].peak_condition == 1 && p.blocks[0].peak_max);
        const Condition& c = p.blocks[0].conditions[0];
        CHECK(c.signal.bones == (std::vector<int>{R(Role::LeftHeel), R(Role::LeftToe)}));
        CHECK(c.signal.combine == Combine::Lowest && c.threshold == 0.044 && c.margin == 0.011 && c.auto_threshold);
        CHECK(!HasKeptText(p));
    }

    // A record written by a future version: unknown kinds and fields come back unchanged and in place.
    {
        const std::string fx = ReadFixture("rav_rules_future.txt");
        CHECK(!fx.empty());
        ItemRules p;
        CHECK(ParseItemRules(fx, &p));
        CHECK(p.format_version == 2);
        CHECK(p.options.sensitivity == 0.1 && p.options.edge_margin_ms == 5);
        CHECK(p.has_preset && p.preset_copy.id == "factory/footsteps" && p.preset_copy.blocks.size() == 1);
        CHECK(p.blocks.size() == 2);
        CHECK(p.blocks[0].conditions.size() == 2 && p.blocks[1].conditions.size() == 1);
        CHECK(p.blocks[0].conditions[0].threshold == 0.044);
        CHECK(p.blocks[0].cooldown_ms == 250);
        CHECK(p.events.size() == 2);
        CHECK(HasKeptText(p));
        // What is kept belongs to the object it was read on.
        CHECK(p.options.kept.fields.size() == 4 && p.options.kept.fields[3].key == "jitter_ms");
        CHECK(p.preset_copy.end_kept.lines == std::vector<std::string>{"group name=Feet blocks=0,1"});
        CHECK(p.blocks[0].conditions[1].kept.lines == std::vector<std::string>{"note free text from the future"});
        CHECK(p.blocks[1].kept.lines == std::vector<std::string>{"lane 2"});
        CHECK(p.tail.size() == 2 && p.tail[0].after_events == 1 && p.tail[1].after_events == 2);
        // A role key this version's record did not know is kept (and reported missing when
        // bound), never dropped. left_hand has been built-in since: it reads and binds the same.
        const std::vector<int>& bones = p.blocks[0].conditions[0].signal.bones;
        CHECK(bones.size() == 3 && BoneRefKey(bones[2]) == "role:left_hand");
        CHECK(BoneRefKey(p.blocks[0].conditions[1].signal.bones[1]) == "bone:my knee");
        const std::string back = SerializeItemRules(p);
        CHECK(back == fx);
        if (back != fx) std::printf("--- got ---\n%s--- want ---\n%s", back.c_str(), fx.c_str());

        // An edit of a known field keeps the unknown ones where they were.
        {
            ItemRules e = p;
            e.blocks[0].conditions[0].threshold = 0.05;
            const std::string edited = SerializeItemRules(e);
            CHECK(edited.find("dir=below thr=0.05 margin=0.011 fixed=0 weight=2\n") != std::string::npos);
            CHECK(edited.find("offset_ms=0 curve=smooth step land=peak:1:max marker=Footstep L\n") != std::string::npos);
            CHECK(edited.find("thr=95 margin=0 fixed=0\nnote free text from the future\nblock") != std::string::npos);
            CHECK(edited.find("end\ngroup name=Feet blocks=0,1\nblock") != std::string::npos);
            CHECK(edited.compare(0, 11, "RAVRULES 2\n") == 0);  // never downgraded
        }

        // The fixture from its first item block to its events: what must follow its objects.
        const std::string blk_l = fx.substr(fx.find("block color=#5F9EDD hold_ms=0 cooldown_ms=250 offset_ms=0 curve"));
        const std::string seg_l = blk_l.substr(0, blk_l.find("block color=#DD9E5F"));  // block L, its conds, the note
        const std::string seg_r = blk_l.substr(seg_l.size(), blk_l.find("event ") - seg_l.size());  // block R + lane

        // Insert a block in front: block L and R keep their own unknown parts.
        {
            ItemRules e = p;
            Block     nb;
            nb.marker = "New";
            Condition nc;
            nc.signal.bones = {R(Role::Hips)};
            nb.conditions = {nc};
            e.blocks.insert(e.blocks.begin(), nb);
            const std::string out = SerializeItemRules(e);
            CHECK(out.find("marker=New\ncond ") != std::string::npos);
            CHECK(out.find(seg_l + seg_r) != std::string::npos);
            ItemRules again;
            CHECK(ParseItemRules(out, &again) && again.blocks.size() == 3 && again.blocks[0].kept.empty());
        }
        // Delete block L: its unknown field, its conditions' and the note go with it; R keeps its line.
        {
            ItemRules e = p;
            e.blocks.erase(e.blocks.begin());
            const std::string out = SerializeItemRules(e);
            CHECK(out.find("curve=smooth") == std::string::npos);
            CHECK(out.find("weight=2") == std::string::npos);
            CHECK(out.find("note free text") == std::string::npos);
            CHECK(out.find("end\ngroup name=Feet blocks=0,1\n" + seg_r + "event ") != std::string::npos);
            CHECK(out.find("jitter_ms=3") != std::string::npos);
        }
        // Swap the blocks: each takes its kept text along.
        {
            ItemRules e = p;
            std::swap(e.blocks[0], e.blocks[1]);
            CHECK(SerializeItemRules(e).find("end\ngroup name=Feet blocks=0,1\n" + seg_r + seg_l + "event ") !=
                  std::string::npos);
        }
        // Insert a condition in front of block L's: weight=2 stays on its condition, the note on the joint one.
        {
            ItemRules e = p;
            Condition nc;
            nc.signal.bones = {R(Role::Hips)};
            nc.threshold = 7;
            e.blocks[0].conditions.insert(e.blocks[0].conditions.begin(), nc);
            const std::string out = SerializeItemRules(e);
            CHECK(out.find("thr=7 margin=0 fixed=0\ncond q=point bones=role:left_heel,role:left_toe,role:left_hand") !=
                  std::string::npos);
            CHECK(out.find("thr=0.044 margin=0.011 fixed=0 weight=2\n") != std::string::npos);
            CHECK(out.find("thr=7 margin=0 fixed=0 weight") == std::string::npos);
            CHECK(out.find("thr=95 margin=0 fixed=0\nnote free text from the future\nblock") != std::string::npos);
        }
        // Delete block L's first condition: weight=2 goes with it, the note stays on the joint one.
        {
            ItemRules e = p;
            e.blocks[0].conditions.erase(e.blocks[0].conditions.begin());
            const std::string out = SerializeItemRules(e);
            CHECK(out.find("weight=2") == std::string::npos);
            CHECK(out.find("land=peak:1:max marker=Footstep L\ncond q=joint") != std::string::npos);
            CHECK(out.find("thr=95 margin=0 fixed=0\nnote free text from the future\nblock") != std::string::npos);
        }
        // Delete the block R's only condition: its unknown field goes, "lane 2" stays on the block.
        {
            ItemRules e = p;
            e.blocks[1].conditions.clear();
            const std::string out = SerializeItemRules(e);
            CHECK(out.find("curve=ease") == std::string::npos);
            CHECK(out.find("marker=Footstep R\nlane 2\nevent ") != std::string::npos);
        }
        // Events (opaque until 10-4): the lines after them are never lost.
        {
            ItemRules e = p;
            e.events.pop_back();
            std::string out = SerializeItemRules(e);
            CHECK(out.find("strength=0.8\nmark t=1.3\nzone t0=0 t1=1\n") != std::string::npos);
            e.events.clear();
            out = SerializeItemRules(e);
            CHECK(out.find("lane 2\ncond q=point bones=role:right_heel") != std::string::npos);
            CHECK(out.size() >= 26 && out.compare(out.size() - 26, 26, "mark t=1.3\nzone t0=0 t1=1\n") == 0);
        }
        // Clearing the preset drops what was kept inside it, and only that.
        {
            ItemRules e = p;
            e.has_preset = false;
            const std::string out = SerializeItemRules(e);
            CHECK(out.find("group name=Feet") == std::string::npos);
            CHECK(out.find("weight=2") != std::string::npos && out.find("jitter_ms=3") != std::string::npos);
        }

        // Binding: the unmapped role is reported missing.
        std::vector<Block> blocks = p.blocks;
        std::vector<int>   map(static_cast<size_t>(Role::Count), -1);
        map[R(Role::LeftHeel)] = 0;
        map[R(Role::LeftToe)] = 1;
        map[R(Role::RightHeel)] = 0;
        std::string missing;
        CHECK(!BindBoneRefs(blocks, map, {"LeftFoot", "LeftToeBase", "mixamorig:LeftUpLeg", "my knee"}, &missing));
        CHECK(missing == "left hand");
        CHECK(blocks[0].conditions[1].signal.bones == (std::vector<int>{2, 3, 0}));
    }

    // A record missing `analyse` (and its copy): an unknown line inside a block stays in place.
    {
        const std::string s = "RAVRULES 3\n"
                              "options sensitivity=0 edge_ms=0 smooth_ms=8\n"
                              "block color=none hold_ms=0 cooldown_ms=0 offset_ms=0 land=cross marker=A\n"
                              "tag soft\n"
                              "cond q=point bones=role:hips comb=single ref=floor meas=position axis=vertical dir=below "
                              "thr=1 margin=0 fixed=0\n"
                              "block color=none hold_ms=0 cooldown_ms=0 offset_ms=0 land=cross marker=B\n"
                              "cond q=point bones=role:hips comb=single ref=floor meas=position axis=vertical dir=below "
                              "thr=2 margin=0 fixed=0\n"
                              "after b\n";
        ItemRules p;
        CHECK(ParseItemRules(s, &p));
        CHECK(p.blocks.size() == 2 && p.blocks[0].kept.lines == std::vector<std::string>{"tag soft"});
        CHECK(p.blocks[1].conditions[0].kept.lines == std::vector<std::string>{"after b"});
        const std::string out = SerializeItemRules(p);
        const std::string want =
            "RAVRULES 3\n"
            "options sensitivity=0 edge_ms=0 smooth_ms=8\n"
            "analyse floor_pct=2 pos_frac=0.25 speed_pct=30 margin_ratio=0.5 onset_frac=0.1 per_bone_floor=0\n" +
            s.substr(s.find("block "));
        CHECK(out == want);
        if (out != want) std::printf("--- got ---\n%s--- want ---\n%s", out.c_str(), want.c_str());
    }

    // A strength line that carries kept text is written even when its known values are defaults.
    {
        const std::string s = "RAVRULES 1\n"
                              "options sensitivity=0 edge_ms=0 smooth_ms=8\n"
                              "analyse floor_pct=2 pos_frac=0.25 speed_pct=30 margin_ratio=0.5 onset_frac=0.1 per_bone_floor=0\n"
                              "block color=none hold_ms=0 cooldown_ms=0 offset_ms=0 land=cross marker=A\n"
                              "strength q=point bones= comb=single ref=floor meas=position axis=vertical sign=1 "
                              "window_ms=100 curve=rms\n";
        ItemRules p;
        CHECK(ParseItemRules(s, &p));
        CHECK(SerializeItemRules(p) == s);
    }

    // Absent fields read as their neutral defaults.
    {
        ItemRules p;
        CHECK(ParseItemRules("RAVRULES 1\nblock marker=Step\ncond bones=role:left_heel\n", &p));
        const DetectOptions  dopt;
        const AnalyseOptions aopt;
        CHECK(p.options.sensitivity == dopt.sensitivity && p.options.edge_margin_ms == dopt.edge_margin_ms &&
              p.options.smooth_ms == dopt.smooth_ms);
        CHECK(p.analyse.floor_percentile == aopt.floor_percentile && p.analyse.per_bone_floor == aopt.per_bone_floor);
        CHECK(!p.has_preset && p.events.empty());
        CHECK(p.blocks.size() == 1);
        Block want;
        want.marker = "Step";
        Condition c;
        c.signal.bones = {R(Role::LeftHeel)};
        want.conditions = {c};
        CHECK(BlocksEqual(p.blocks, {want}));
        CHECK(p.blocks[0].color == 0 && p.blocks[0].landing == Landing::Crossing);
    }

    // Corrupt records read as no rules, never crash.
    {
        ItemRules p;
        p.blocks.resize(3);
        CHECK(!ParseItemRules("", &p));
        CHECK(!ParseItemRules("garbage\nblock marker=x\n", &p));
        CHECK(!ParseItemRules("RAVRULESX 1\n", &p));
        CHECK(!ParseItemRules(std::string("\x00\xff\xfe RAVRULES 1", 15), &p));
        CHECK(p.blocks.size() == 3);  // untouched
        std::string junk;
        for (int i = 0; i < 4000; ++i) junk += static_cast<char>((i * 7919) % 251 + 1);
        CHECK(ParseItemRules("RAVRULES 1\n" + junk, &p));  // best-effort: kept as unknown text
        CHECK(ParseItemRules("RAVRULES 1\ncond thr=1\nstrength x\nend\ncopy\nblock land=peak:x\n", &p));
        CHECK(ParseItemRules("\xEF\xBB\xBF\r\nRAVRULES 7\r\noptions smooth_ms=4\r\n", &p));
        CHECK(p.format_version == 7 && p.options.smooth_ms == 4);
    }

    // A known field whose value does not read is kept as written, once.
    {
        const std::string s = "RAVRULES 1\n"
                              "options sensitivity=0 edge_ms=0 smooth_ms=8\n"
                              "analyse floor_pct=2 pos_frac=0.25 speed_pct=30 margin_ratio=0.5 onset_frac=0.1 per_bone_floor=0\n"
                              "block color=#12345 hold_ms=0 cooldown_ms=0 offset_ms=0 land=cross marker=X\n"
                              "cond q=point bones=role:hips comb=single ref=floor meas=position axis=vertical dir=below "
                              "thr=abc margin=0 fixed=0\n";
        ItemRules p;
        CHECK(ParseItemRules(s, &p));
        CHECK(p.blocks[0].color == 0);
        CHECK(SerializeItemRules(p) == s);

        // A later edit of that field wins, in place; the other one stays as written.
        ItemRules e = p;
        e.blocks[0].conditions[0].threshold = 0.5;
        std::string out = SerializeItemRules(e);
        CHECK(out.find("dir=below thr=0.5 margin=0 fixed=0\n") != std::string::npos);
        CHECK(out.find("thr=abc") == std::string::npos);
        CHECK(out.find("color=#12345 hold_ms") != std::string::npos);
        e.blocks[0].color = 0x1000000u | 0x00FF00u;
        out = SerializeItemRules(e);
        CHECK(out.find("block color=#00FF00 hold_ms=0") != std::string::npos);
        ItemRules again;
        CHECK(ParseItemRules(out, &again) && again.blocks[0].conditions[0].threshold == 0.5 &&
              again.blocks[0].color == (0x1000000u | 0x00FF00u) && !HasKeptText(again));

        // An edit that drops the field from the line (a reference bone list) drops the raw text too.
        const std::string s2 = "RAVRULES 1\ncond_free\nblock marker=X\ncond bones=role:hips ref_bones=role:%ZZ\n";
        ItemRules q;
        CHECK(ParseItemRules(s2, &q));
        CHECK(SerializeItemRules(q).find("ref_bones=role:%ZZ") != std::string::npos);
        q.blocks[0].conditions[0].signal.ref_bones = {R(Role::Hips)};
        CHECK(SerializeItemRules(q).find("ref_bones=role:hips") != std::string::npos);
    }

    // Numbers do not follow the C library locale.
    {
        if (std::setlocale(LC_ALL, "de_DE.UTF-8") || std::setlocale(LC_ALL, "fr_FR.UTF-8")) {
            double v = 0;
            CHECK(FormatNumber(0.5) == "0.5");
            CHECK(ReadNumber("0.25", &v) && v == 0.25);
            std::setlocale(LC_ALL, "C");
        }
        double v = 0;
        CHECK(FormatNumber(0.1 + 0.2) != "0.3");  // shortest form that reads back exactly
        CHECK(ReadNumber(FormatNumber(0.1 + 0.2), &v) && v == 0.1 + 0.2);
        CHECK(!ReadNumber("1,5", &v) && !ReadNumber("nan", &v) && !ReadNumber("", &v));
    }

    // Signal names come from the menus.
    {
        SignalSpec h;
        h.bones = {R(Role::LeftHeel), R(Role::LeftToe)};
        h.combine = Combine::Lowest;
        CHECK(SignalName(h, SignalBoneLabel(h)) == "L heel+toe height");
        SignalSpec k;
        k.quantity = Quantity::JointAngle;
        k.bones = {R(Role::LeftUpLeg), R(Role::LeftKnee), R(Role::LeftHeel)};
        k.measure = Measure::Speed;
        CHECK(SignalName(k, SignalBoneLabel(k)) == "L knee bend speed");
        k.measure = Measure::Position;
        CHECK(SignalName(k, SignalBoneLabel(k)) == "L knee bend");
        SignalSpec y;
        y.quantity = Quantity::Yaw;
        y.bones = {R(Role::Hips), R(Role::LeftUpLeg)};
        y.measure = Measure::Speed;
        CHECK(SignalName(y, SignalBoneLabel(y)) == "hips turn speed");
        y.bones = {R(Role::RightHeel), R(Role::RightToe)};
        CHECK(SignalName(y, SignalBoneLabel(y)) == "R foot turn speed");
        SignalSpec s;
        s.bones = {R(Role::RightKnee)};
        s.measure = Measure::Speed;
        s.axis = Axis::Vertical;
        CHECK(SignalName(s, SignalBoneLabel(s)) == "R knee vertical speed");
        s.axis = Axis::Total;
        CHECK(SignalName(s, SignalBoneLabel(s)) == "R knee speed");
        s.measure = Measure::Position;
        s.axis = Axis::Horizontal;
        CHECK(SignalName(s, SignalBoneLabel(s)) == "R knee horizontal position");
        s.bones = {BoneRefForBone("Spine2")};
        s.measure = Measure::Acceleration;
        s.axis = Axis::X;
        CHECK(SignalName(s, SignalBoneLabel(s)) == "Spine2 X acceleration");
    }

    // Missing role: the list names it; bone keys bind by name.
    {
        Block b;
        Condition c;
        c.signal.bones = {R(Role::LeftHeel), R(Role::LeftToe)};
        b.conditions = {c};
        b.strength_signal.bones = {BoneRefForBone("mixamorig1_Hips")};
        std::vector<Block> blocks = {b};
        std::vector<int>   map(static_cast<size_t>(Role::Count), -1);
        map[R(Role::LeftHeel)] = 1;
        std::string missing;
        CHECK(!BindBoneRefs(blocks, map, {"mixamorig:Hips", "mixamorig:LeftFoot"}, &missing));
        CHECK(missing == "left toe");
        CHECK(blocks[0].strength_signal.bones[0] == 0);  // normalized name match
        map[R(Role::LeftToe)] = 0;
        blocks = {b};
        CHECK(BindBoneRefs(blocks, map, {"mixamorig:Hips", "mixamorig:LeftFoot"}, &missing));
        CHECK(missing.empty());
        CHECK(BoneRefsUsed({b}) == (std::vector<int>{R(Role::LeftHeel), R(Role::LeftToe), BoneRefForBone("mixamorig1_Hips")}));
    }

    // A floor-referenced signal ignores leftover reference bones (bind and used list).
    {
        Block     b;
        Condition c;
        c.signal.bones = {R(Role::LeftHeel)};
        c.signal.reference = Reference::Floor;
        c.signal.ref_bones = {BoneRefId("role:left_hand")};
        b.conditions = {c};
        std::vector<Block> blocks = {b};
        std::vector<int>   map(static_cast<size_t>(Role::Count), -1);
        map[R(Role::LeftHeel)] = 0;
        std::string missing;
        CHECK(BindBoneRefs(blocks, map, {"LeftFoot"}, &missing) && missing.empty());
        CHECK(BoneRefsUsed({b}) == std::vector<int>{R(Role::LeftHeel)});
        b.conditions[0].signal.reference = Reference::Bones;
        blocks = {b};
        CHECK(!BindBoneRefs(blocks, map, {"LeftFoot"}, &missing) && missing == "left hand");
    }

    // The header line after the version comes back as written.
    {
        const std::string s = "RAVRULES 2 flags=x  y\n"
                              "options sensitivity=0 edge_ms=0 smooth_ms=8\n"
                              "analyse floor_pct=2 pos_frac=0.25 speed_pct=30 margin_ratio=0.5 onset_frac=0.1 per_bone_floor=0\n";
        ItemRules p;
        CHECK(ParseItemRules(s, &p) && p.format_version == 2 && p.header_rest == " flags=x  y");
        CHECK(SerializeItemRules(p) == s && HasKeptText(p));
        PresetData d;
        CHECK(ParsePreset("RAVPRESET 1;beta\npreset id=x version=1 name=X\n", &d));
        CHECK(SerializePreset(d).compare(0, 17, "RAVPRESET 1;beta\n") == 0);
    }

    // Role keys.
    {
        for (int r = 0; r < static_cast<int>(Role::Count); ++r) {
            Role back;
            CHECK(RoleFromKey(RoleKey(static_cast<Role>(r)), &back) && back == static_cast<Role>(r));
            CHECK(BoneRefId(std::string("role:") + RoleKey(static_cast<Role>(r))) == r);
        }
        CHECK(!RoleFromKey("left_wing", nullptr));
        CHECK(BoneRefId("role:left_wing") >= kBoneRefExtra);
        CHECK(BoneRefId("role:left_wing") == BoneRefId("role:left_wing"));
        CHECK(BoneRefName(BoneRefId("role:left_wing")) == "left wing");
        CHECK(RoleShortLabel(Role::LeftHeel) == "L heel" && RoleShortLabel(Role::Hips) == "hips");
        // The arm and body roles: built-in ids, keys written back as read, names and labels.
        CHECK(BoneRefId("role:left_hand") == R(Role::LeftHand) && BoneRefKey(R(Role::LeftHand)) == "role:left_hand");
        CHECK(BoneRefName(R(Role::LeftHand)) == "left hand" && BoneRefLabel(R(Role::LeftHand)) == "L hand");
        CHECK(BoneRefId("role:head") == R(Role::Head) && BoneRefLabel(R(Role::Head)) == "head");
        CHECK(RoleKeyOfRef(R(Role::RightToeEnd)) == "right_toe_end");
        CHECK(CustomRoleKeysUsed({}).empty());
        SignalSpec s;
        s.bones = {R(Role::LeftHand), R(Role::LeftElbow)};
        s.measure = Measure::Speed;
        s.axis = Axis::Total;
        CHECK(SignalName(s, SignalBoneLabel(s)) == "L hand+elbow speed");
        s.quantity = Quantity::InteriorAngle;
        s.bones = {R(Role::RightElbow)};
        s.measure = Measure::Position;
        CHECK(SignalName(s, SignalBoneLabel(s)) == "R elbow angle");
    }

    // ---- Story 10-3e: custom roles bind through their mapping by key ----------------------
    {
        Block     b;
        Condition c;
        c.signal.bones = {BoneRefId("role:sword_tip")};
        c.signal.reference = Reference::Bones;
        c.signal.ref_bones = {R(Role::Hips)};
        b.conditions = {c};
        const std::vector<std::string> names = {"Hips", "weapon_r"};
        std::vector<int>               map(static_cast<size_t>(Role::Count), -1);
        map[R(Role::Hips)] = 0;
        std::map<std::string, int> custom;
        std::string                missing;
        // Unmapped (absent, then -1): missing, by its key-derived name.
        std::vector<Block> blocks = {b};
        CHECK(!BindBoneRefs(blocks, map, custom, names, {}, &missing) && missing == "sword tip");
        CHECK(MissingBoneRefs({b}, map, custom, names, {}) == std::vector<std::string>{"sword tip"});
        custom["sword_tip"] = -1;
        blocks = {b};
        CHECK(!BindBoneRefs(blocks, map, custom, names, {}, &missing) && missing == "sword tip");
        // Without the custom map (the older forms): never binds.
        blocks = {b};
        CHECK(!BindBoneRefs(blocks, map, names, &missing));
        // Mapped: binds to that bone.
        custom["sword_tip"] = 1;
        blocks = {b};
        CHECK(BindBoneRefs(blocks, map, custom, names, {}, &missing) && missing.empty());
        CHECK(blocks[0].conditions[0].signal.bones == std::vector<int>{1});
        CHECK(blocks[0].conditions[0].signal.ref_bones == std::vector<int>{0});
        CHECK(MissingBoneRefs({b}, map, custom, names, {}).empty());
        // A mapping past the skeleton does not bind.
        custom["sword_tip"] = 7;
        blocks = {b};
        CHECK(!BindBoneRefs(blocks, map, custom, names, {}, &missing));
        // The keys the blocks read: custom ones only, each once, in first-use order.
        Block b2 = b;
        b2.strength_signal.bones = {BoneRefId("role:tail_end"), BoneRefId("role:sword_tip"), R(Role::LeftHeel),
                                    BoneRefForBone("weapon_r")};
        CHECK(CustomRoleKeysUsed({b2}) == (std::vector<std::string>{"sword_tip", "tail_end"}));
        CHECK(RoleKeyOfRef(BoneRefId("role:sword_tip")) == "sword_tip");
        CHECK(RoleKeyOfRef(R(Role::Hips)) == "hips");
        CHECK(RoleKeyOfRef(BoneRefForBone("weapon_r")).empty());
        // Display names: the one set (rename), else the key-derived one; labels follow.
        SetCustomRoleNames({{"sword_tip", "blade tip"}});
        CHECK(BoneRefName(BoneRefId("role:sword_tip")) == "blade tip");
        CHECK(BoneRefLabel(BoneRefId("role:sword_tip")) == "blade tip");
        CHECK(BoneRefName(BoneRefId("role:tail_end")) == "tail end");
        // Spec 10-3c: "+" on a picked bone puts its role when it plays exactly one; when it plays
        // several, the one built-in role it is named for; else the bone.
        {
            std::vector<int> r2b(static_cast<size_t>(Role::Count), -1);
            r2b[static_cast<size_t>(Role::LeftHeel)] = 3;
            r2b[static_cast<size_t>(Role::LeftToe)] = 5;
            r2b[static_cast<size_t>(Role::RightToe)] = 5;  // bone 5 plays two roles
            const std::map<std::string, int> cust = {{"sword_tip", 7}, {"grip", 8}, {"also_heel", 9}};
            CHECK(BoneRefForPickedBone(3, "LeftFoot", r2b, cust) == R(Role::LeftHeel));
            CHECK(BoneRefForPickedBone(7, "weapon_r", r2b, cust) == BoneRefId("role:sword_tip"));
            CHECK(BoneRefForPickedBone(5, "Toe", r2b, cust) == BoneRefForBone("Toe"));  // named for neither
            CHECK(BoneRefForPickedBone(5, "LeftToeBase", r2b, cust) == R(Role::LeftToe));  // named for one
            CHECK(BoneRefForPickedBone(4, "RightHand", r2b, cust) == BoneRefForBone("RightHand"));  // no role
            CHECK(BoneRefForPickedBone(-1, "Gone", r2b, cust) == BoneRefForBone("Gone"));  // not on this skeleton
            r2b[static_cast<size_t>(Role::Hips)] = 7;  // built-in + custom on one bone: two roles
            CHECK(BoneRefForPickedBone(7, "weapon_r", r2b, cust) == BoneRefForBone("weapon_r"));
            // Two custom roles on one bone (no built-in): two roles, the raw bone.
            const std::map<std::string, int> cust2 = {{"sword_tip", 10}, {"blade", 10}};
            CHECK(BoneRefForPickedBone(10, "weapon_l", r2b, cust2) == BoneRefForBone("weapon_l"));
            // Unreal: ball_l is the toe and stands in for the toe end -> the toe. Mixamo without
            // ToeBase: Toe_End is the toe end and stands in for the toe -> the toe end.
            const std::vector<std::string> ue = {"foot_l", "ball_l"};
            const std::vector<int>         gu = GuessRoleMapping(ue);
            CHECK(gu[R(Role::LeftToe)] == 1 && gu[R(Role::LeftToeEnd)] == 1);
            CHECK(BoneRefForPickedBone(1, "ball_l", gu, {}) == R(Role::LeftToe));
            const std::vector<int> gm = GuessRoleMapping({"mixamorig:LeftFoot", "mixamorig:LeftToe_End"});
            CHECK(BoneRefForPickedBone(1, "mixamorig:LeftToe_End", gm, {}) == R(Role::LeftToeEnd));
            // A rig with Spine only: the spine (the chest's stand-in) -> the spine.
            const std::vector<int> gs = GuessRoleMapping({"Hips", "Spine"});
            CHECK(BoneRefForPickedBone(1, "Spine", gs, {}) == R(Role::Spine));
            // A custom role on the bone a built-in is named for: the built-in.
            CHECK(BoneRefForPickedBone(1, "ball_l", gu, {{"paw", 1}}) == R(Role::LeftToe));
            // Hand-made mappings on Foot_L: the heel and the hand -> the heel (named for it); both
            // toes -> named for neither: the raw bone.
            std::vector<int> two(static_cast<size_t>(Role::Count), -1);
            two[static_cast<size_t>(Role::LeftHand)] = 4;
            two[static_cast<size_t>(Role::LeftHeel)] = 4;
            CHECK(BoneRefForPickedBone(4, "Foot_L", two, {}) == R(Role::LeftHeel));
            two.assign(static_cast<size_t>(Role::Count), -1);
            two[static_cast<size_t>(Role::LeftToe)] = 4;
            two[static_cast<size_t>(Role::RightToe)] = 4;
            CHECK(BoneRefForPickedBone(4, "Foot_L", two, {}) == BoneRefForBone("Foot_L"));
        }
        // Spec 10-3c: the bones one rule reads on a skeleton (the 3D view colours them).
        {
            // 0 Hips <- 1 LeftUpLeg <- 2 LeftLeg <- 3 LeftFoot <- 4 LeftToeBase; 0 <- 5 Spine <- 6 RightHand
            const std::vector<std::string> nm = {"Hips", "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase", "Spine",
                                                 "RightHand"};
            const std::vector<int>         par = {-1, 0, 1, 2, 3, 0, 5};
            std::vector<int>               r2b(static_cast<size_t>(Role::Count), -1);
            r2b[static_cast<size_t>(Role::LeftHeel)] = 3;
            r2b[static_cast<size_t>(Role::Hips)] = 0;
            const std::map<std::string, int> cust = {{"sword_tip", 6}};
            // A joint angle binds to {parent, joint, child}.
            Block ja;
            ja.conditions.resize(1);
            ja.conditions[0].signal.quantity = Quantity::InteriorAngle;
            ja.conditions[0].signal.bones = {BoneRefForBone("LeftLeg")};
            CHECK(RuleBonesOnSkeleton(ja, r2b, cust, nm, par) == (std::vector<int>{1, 2, 3}));
            // Reference::Bones bones are included; a role and a custom role bind through the maps.
            Block rb;
            rb.conditions.resize(2);
            rb.conditions[0].signal.bones = {R(Role::LeftHeel)};
            rb.conditions[0].signal.reference = Reference::Bones;
            rb.conditions[0].signal.ref_bones = {R(Role::Hips)};
            rb.conditions[1].signal.bones = {BoneRefId("role:sword_tip"), R(Role::LeftHeel)};  // duplicate once
            CHECK(RuleBonesOnSkeleton(rb, r2b, cust, nm, par) == (std::vector<int>{3, 0, 6}));
            // ref_bones ignored when the reference is Floor.
            rb.conditions[0].signal.reference = Reference::Floor;
            CHECK(RuleBonesOnSkeleton(rb, r2b, cust, nm, par) == (std::vector<int>{3, 6}));
            // The strength signal's bones count; unbound refs (unmapped role, unknown bone, a
            // joint angle on a leaf) are left out.
            Block st;
            st.conditions.resize(2);
            st.conditions[0].signal.bones = {R(Role::RightHeel), BoneRefForBone("NoSuchBone")};
            st.conditions[1].signal.quantity = Quantity::InteriorAngle;
            st.conditions[1].signal.bones = {BoneRefForBone("LeftToeBase")};
            st.strength_signal.bones = {BoneRefForBone("Spine")};
            CHECK(RuleBonesOnSkeleton(st, r2b, cust, nm, par) == (std::vector<int>{5}));
            CHECK(RuleBonesOnSkeleton(Block{}, r2b, cust, nm, par).empty());
        }
        SignalSpec sp;
        sp.bones = {BoneRefId("role:sword_tip")};
        sp.measure = Measure::Speed;
        sp.axis = Axis::Total;
        CHECK(SignalBoneLabel(sp) == "blade tip");
        custom.erase("sword_tip");
        blocks = {b};
        CHECK(!BindBoneRefs(blocks, map, custom, names, {}, &missing) && missing == "blade tip");
        SetCustomRoleNames({});
        CHECK(BoneRefName(BoneRefId("role:sword_tip")) == "sword tip");
        // The record keeps the key whatever the name: role:sword_tip round-trips.
        ItemRules rr;
        rr.blocks = {b};
        ItemRules back;
        CHECK(ParseItemRules(SerializeItemRules(rr), &back) && back.blocks.size() == 1 &&
              back.blocks[0].conditions.size() == 1 &&
              back.blocks[0].conditions[0].signal.bones == std::vector<int>{BoneRefId("role:sword_tip")});
        CHECK(SerializeItemRules(rr).find("bones=role:sword_tip") != std::string::npos);
    }

    // ---- Story 10-3: the rule's on/off switch (`on=0`, written only when off) -------------
    {
        const std::string on_text = "RAVRULES 1\n"
                                    "options sensitivity=0 edge_ms=0 smooth_ms=8\n"
                                    "analyse floor_pct=2 pos_frac=0.25 speed_pct=30 margin_ratio=0.5 onset_frac=0.1 "
                                    "per_bone_floor=0\n"
                                    "block color=#5F9EDD hold_ms=0 cooldown_ms=250 offset_ms=0 land=cross marker=Step\n"
                                    "cond q=point bones=role:left_heel comb=single ref=floor meas=position axis=vertical "
                                    "dir=below thr=0.05 margin=0.02 fixed=0\n";
        ItemRules r;
        CHECK(ParseItemRules(on_text, &r));
        CHECK(r.blocks.size() == 1 && r.blocks[0].enabled);
        CHECK(SerializeItemRules(r) == on_text);  // an on rule writes no `on`: byte-identical
        CHECK(on_text.find("on=") == std::string::npos);

        ItemRules off = r;
        off.blocks[0].enabled = false;
        CHECK(!BlocksEqual(off.blocks, r.blocks));
        const std::string off_text = SerializeItemRules(off);
        CHECK(off_text.find("block on=0 color=#5F9EDD") != std::string::npos);
        ItemRules back;
        CHECK(ParseItemRules(off_text, &back));
        CHECK(back.blocks.size() == 1 && !back.blocks[0].enabled);
        CHECK(BlocksEqual(back.blocks, off.blocks));
        CHECK(SerializeItemRules(back) == off_text);  // round trip
        // Switched back on: the field goes away again.
        back.blocks[0].enabled = true;
        CHECK(SerializeItemRules(back) == on_text);
        // `on=1` reads as on; an unreadable value is kept as written (the rule stays on).
        ItemRules one;
        std::string t1 = on_text;
        t1.replace(t1.find("block "), 6, "block on=1 ");
        CHECK(ParseItemRules(t1, &one) && one.blocks[0].enabled);
        ItemRules bad;
        std::string t2 = on_text;
        t2.replace(t2.find("block "), 6, "block on=maybe ");
        CHECK(ParseItemRules(t2, &bad) && bad.blocks[0].enabled);
        CHECK(SerializeItemRules(bad) == t2);
    }

    // ---- Story 10-4: the event list, the applied state, the owned markers ----
    {
        const std::string text =
            "RAVRULES 1\n"
            "options sensitivity=0 edge_ms=0 smooth_ms=8\n"
            "analyse floor_pct=2 pos_frac=0.25 speed_pct=30 margin_ratio=0.5 onset_frac=0.1 per_bone_floor=0\n"
            "block color=#5F9EDD hold_ms=0 cooldown_ms=250 offset_ms=0 land=cross marker=Step\n"
            "cond q=point bones=role:left_heel comb=single ref=floor meas=position axis=vertical "
            "dir=below thr=0.05 margin=0.02 fixed=0\n"
            "anim path=C:\\anims\\walk%20cycle.glb\n"
            "event t=1.25 kind=detected block=0 strength=0.8 speed=1.9\n"
            "event t=2.5 kind=user block=0 strength=0.7 speed=1.2\n"
            "event t=3.1 kind=suppress block=0\n"
            "applied markers=project sig=0123456789ABCDEF\n"
            "tmarker t=1.25 name=Step  (soft)\n"
            "pmarker guid={4A1B2C3D-0000-1111-2222-333344445555} t=12.375\n";
        ItemRules r;
        CHECK(ParseItemRules(text, &r));
        // An `anim` line (written by an early 10-4 build) is unknown now: kept as written, in place.
        CHECK(r.blocks[0].conditions[0].kept.lines == std::vector<std::string>{"anim path=C:\\anims\\walk%20cycle.glb"});
        CHECK(r.events.size() == 3);
        if (r.events.size() == 3) {
            CHECK(r.events[0].kind == EventKind::Detected && r.events[0].t == 1.25 && r.events[0].block == 0 &&
                  r.events[0].strength == 0.8 && r.events[0].speed == 1.9);
            CHECK(r.events[1].kind == EventKind::User && r.events[1].strength == 0.7 && r.events[1].has_speed);
            CHECK(r.events[2].kind == EventKind::Suppress && r.events[2].t == 3.1 && !r.events[2].has_strength);
        }
        CHECK(r.has_applied && r.applied.mode == MarkerMode::Project && r.applied.sig == "0123456789ABCDEF");
        CHECK(r.tmarkers.size() == 1 && r.tmarkers[0].t == 1.25 && r.tmarkers[0].name == "Step  (soft)");
        CHECK(r.pmarkers.size() == 1 && r.pmarkers[0].guid == "{4A1B2C3D-0000-1111-2222-333344445555}" &&
              r.pmarkers[0].t == 12.375);
        CHECK(HasKeptText(r));  // the `anim` line
        CHECK(SerializeItemRules(r) == text);  // round trip, byte-identical

        // Built in code: every kind writes back as it reads.
        ItemRules c = r;
        c.events.clear();
        EventEntry u;
        u.t = 0.5;
        u.kind = EventKind::User;
        u.block = 1;
        u.has_strength = u.has_speed = true;
        u.strength = 2;
        u.speed = 3;
        c.events.push_back(u);
        ProjectMarkerRef pm;
        pm.guid = "{X}";
        pm.t = 1;
        c.pmarkers.push_back(pm);
        const std::string ct = SerializeItemRules(c);
        CHECK(ct.find("\nevent t=0.5 kind=user block=1 strength=2 speed=3\napplied ") != std::string::npos);
        CHECK(ct.find("pmarker guid={X} t=1\n") != std::string::npos);
        ItemRules cb;
        CHECK(ParseItemRules(ct, &cb) && SerializeItemRules(cb) == ct && cb.pmarkers.size() == 2);

        // Unknown fields on event lines (and an unknown kind) are kept, in place; an unknown
        // line after a pmarker stays with it.
        const std::string fut = "RAVRULES 3\n"
                                "options sensitivity=0 edge_ms=0 smooth_ms=8\n"
                                "analyse floor_pct=2 pos_frac=0.25 speed_pct=30 margin_ratio=0.5 onset_frac=0.1 "
                                "per_bone_floor=0\n"
                                "event t=1 kind=detected block=0 strength=0.5 foot=left speed=2\n"
                                "event t=2 kind=swing block=0 arc=90\n"
                                "event t=3 kind=user block=x\n"
                                "pmarker guid={A} t=1 lane=2\n"
                                "pmeta 7\n";
        ItemRules f;
        CHECK(ParseItemRules(fut, &f));
        CHECK(f.events.size() == 3 && f.events[0].kind == EventKind::Detected && f.events[0].speed == 2);
        CHECK(f.events[1].kind == EventKind::Other && f.events[2].kind == EventKind::User && !f.events[2].has_block);
        CHECK(HasKeptText(f));
        CHECK(SerializeItemRules(f) == fut);
        if (SerializeItemRules(f) != fut) std::printf("--- got ---\n%s--- want ---\n%s", SerializeItemRules(f).c_str(), fut.c_str());
        // An edit of a known field keeps the unknown one where it was.
        f.events[0].t = 1.5;
        CHECK(SerializeItemRules(f).find("event t=1.5 kind=detected block=0 strength=0.5 foot=left speed=2\n") !=
              std::string::npos);
        // Erasing an event takes its unknown fields along.
        f.events.erase(f.events.begin() + 1);
        CHECK(SerializeItemRules(f).find("arc=90") == std::string::npos);

        // A record from before 10-4 (no event) writes none of these lines.
        ItemRules old;
        old.blocks.push_back(Block{});
        const std::string ot = SerializeItemRules(old);
        CHECK(ot.find("anim") == std::string::npos && ot.find("applied") == std::string::npos &&
              ot.find("marker t=") == std::string::npos);

        CHECK(std::string(MarkerModeWord(MarkerMode::Both)) == "both");
        MarkerMode mm = MarkerMode::Both;
        CHECK(MarkerModeFromWord("take", &mm) && mm == MarkerMode::Take && !MarkerModeFromWord("all", &mm));
    }

    // 10-4 fb-4: the preview lines.
    {
        const std::string text =
            "RAVRULES 1\n"
            "options sensitivity=0 edge_ms=0 smooth_ms=8\n"
            "analyse floor_pct=2 pos_frac=0.25 speed_pct=30 margin_ratio=0.5 onset_frac=0.1 per_bone_floor=0\n"
            "block color=#5F9EDD hold_ms=0 cooldown_ms=250 offset_ms=0 land=cross marker=Step\n"
            "event t=1.25 kind=detected block=0 strength=0.8 speed=1.9\n"
            "applied markers=both sig=0123456789ABCDEF\n"
            "tmarker t=1.25 name=Step\n"
            "pmarker guid={A} t=12.25\n"
            "preview markers=take sig=FEDCBA9876543210\n"
            "ptmarker t=1.5 name=Step - Preview\n"
            "ppmarker guid={B} t=12.5\n";
        ItemRules r;
        CHECK(ParseItemRules(text, &r));
        CHECK(!HasKeptText(r));
        CHECK(r.has_previewed && r.previewed.mode == MarkerMode::Take && r.previewed.sig == "FEDCBA9876543210");
        CHECK(r.ptmarkers.size() == 1 && r.ptmarkers[0].t == 1.5 && r.ptmarkers[0].name == "Step - Preview");
        CHECK(r.ppmarkers.size() == 1 && r.ppmarkers[0].guid == "{B}" && r.ppmarkers[0].t == 12.5);
        CHECK(r.tmarkers.size() == 1 && r.pmarkers.size() == 1);  // the committed ones apart
        CHECK(SerializeItemRules(r) == text);  // byte-identical

        // Unknown fields and an unknown line after a preview line stay with it.
        const std::string fut = text.substr(0, text.size() - std::string("ppmarker guid={B} t=12.5\n").size()) +
                                "ppmarker guid={B} t=12.5 lane=3\n"
                                "pnote x\n";
        ItemRules f;
        CHECK(ParseItemRules(fut, &f) && HasKeptText(f) && SerializeItemRules(f) == fut);

        // An old record (no preview line) writes none.
        ItemRules o = r;
        o.has_previewed = false;
        o.previewed = AppliedInfo{};
        o.ptmarkers.clear();
        o.ppmarkers.clear();
        const std::string ot = SerializeItemRules(o);
        CHECK(ot.find("preview") == std::string::npos && ot.find("ptmarker") == std::string::npos &&
              ot.find("ppmarker") == std::string::npos);
        ItemRules ob;
        CHECK(ParseItemRules(ot, &ob) && SerializeItemRules(ob) == ot && !ob.has_previewed);
    }

    // 10-4b: the marker mirror's fields (owner, clip time, colour, name; empty GUID = hidden).
    {
        const std::string text =
            "RAVRULES 1\n"
            "options sensitivity=0 edge_ms=0 smooth_ms=8\n"
            "analyse floor_pct=2 pos_frac=0.25 speed_pct=30 margin_ratio=0.5 onset_frac=0.1 per_bone_floor=0\n"
            "block color=#5F9EDD hold_ms=0 cooldown_ms=250 offset_ms=0 land=cross marker=Step\n"
            "applied markers=both sig=0123456789ABCDEF item={1111-AAAA}\n"
            "pmarker guid={A} t=12.25 c=0.25 color=#5F9EDD name=Step  (soft)\n"
            "pmarker guid= t=14 c=2 color=none name=Step\n"
            "preview markers=project sig=FEDCBA9876543210 item={1111-AAAA}\n"
            "ppmarker guid={B} t=12.5 c=0.5 color=#304F6F name=Step - Preview\n";
        ItemRules r;
        CHECK(ParseItemRules(text, &r) && !HasKeptText(r));
        CHECK(r.applied.item == "{1111-AAAA}" && r.previewed.item == "{1111-AAAA}");
        CHECK(r.pmarkers.size() == 2 && r.ppmarkers.size() == 1);
        if (r.pmarkers.size() == 2 && r.ppmarkers.size() == 1) {
            CHECK(r.pmarkers[0].has_c && r.pmarkers[0].c == 0.25 && r.pmarkers[0].has_color &&
                  r.pmarkers[0].color == (0x1000000u | 0x5F9EDDu) && r.pmarkers[0].name == "Step  (soft)");
            CHECK(r.pmarkers[1].guid.empty() && r.pmarkers[1].c == 2 && r.pmarkers[1].color == 0);  // hidden
            CHECK(r.ppmarkers[0].name == "Step - Preview" && r.ppmarkers[0].c == 0.5);
        }
        CHECK(SerializeItemRules(r) == text);  // byte-identical

        // Built in code: written in that order, read back the same.
        ItemRules c;
        c.has_applied = true;
        c.applied.item = "{X}";
        ProjectMarkerRef pm;
        pm.guid = "{G}";
        pm.t = 1;
        pm.c = 0.5;
        pm.color = 0x1000000u | 0x00FF00u;
        pm.name = "Foot";
        pm.has_c = pm.has_color = pm.has_name = true;
        c.pmarkers.push_back(pm);
        const std::string ct = SerializeItemRules(c);
        CHECK(ct.find("item={X}\n") != std::string::npos);
        CHECK(ct.find("pmarker guid={G} t=1 c=0.5 color=#00FF00 name=Foot\n") != std::string::npos);
        ItemRules cb;
        CHECK(ParseItemRules(ct, &cb) && SerializeItemRules(cb) == ct);

        // An older record (no owner, refs without clip time) writes none of the new fields.
        const std::string old_text =
            "RAVRULES 1\n"
            "options sensitivity=0 edge_ms=0 smooth_ms=8\n"
            "analyse floor_pct=2 pos_frac=0.25 speed_pct=30 margin_ratio=0.5 onset_frac=0.1 per_bone_floor=0\n"
            "applied markers=both sig=0123456789ABCDEF\n"
            "pmarker guid={A} t=12.25\n";
        ItemRules o;
        CHECK(ParseItemRules(old_text, &o) && o.applied.item.empty() && !o.pmarkers[0].has_c &&
              !o.pmarkers[0].has_name && !o.pmarkers[0].has_color);
        CHECK(SerializeItemRules(o) == old_text);

        // Unknown fields on the new lines still round-trip, in place.
        const std::string fut =
            "RAVRULES 2\n"
            "options sensitivity=0 edge_ms=0 smooth_ms=8\n"
            "analyse floor_pct=2 pos_frac=0.25 speed_pct=30 margin_ratio=0.5 onset_frac=0.1 per_bone_floor=0\n"
            "applied markers=both sig=0123456789ABCDEF item={I} lane=2\n"
            "pmarker guid={A} t=12.25 c=0.25 lane=1 color=#5F9EDD name=Step\n"
            "pnote 1\n";
        ItemRules f;
        CHECK(ParseItemRules(fut, &f) && HasKeptText(f) && SerializeItemRules(f) == fut);
        if (SerializeItemRules(f) != fut) std::printf("--- got ---\n%s--- want ---\n%s", SerializeItemRules(f).c_str(), fut.c_str());
    }

    // 10-4 follow-up: joint-angle and rotation conditions.
    {
        // New words round-trip; the record holds the joint alone.
        const std::string rec =
            "RAVRULES 1\n"
            "options sensitivity=0 edge_ms=0 smooth_ms=8\n"
            "analyse floor_pct=2 pos_frac=0.25 speed_pct=30 margin_ratio=0.5 onset_frac=0.1 per_bone_floor=0\n"
            "block color=none hold_ms=0 cooldown_ms=250 offset_ms=0 land=cross marker=Knee\n"
            "cond q=angle bones=role:left_knee comb=single ref=floor meas=position axis=vertical dir=below thr=35 "
            "margin=5 fixed=0\n"
            "cond q=rot bones=role:left_toe comb=single ref=parent meas=speed axis=total dir=above thr=300 margin=50 "
            "fixed=0\n"
            "cond q=rot bones=bone:LeftFoot comb=single ref=floor meas=position axis=y dir=above thr=10 margin=1 "
            "fixed=1\n";
        ItemRules r;
        CHECK(ParseItemRules(rec, &r));
        CHECK(!HasKeptText(r));
        CHECK(SerializeItemRules(r) == rec);
        CHECK(r.blocks.size() == 1 && r.blocks[0].conditions.size() == 3);
        if (r.blocks.size() == 1 && r.blocks[0].conditions.size() == 3) {
            const SignalSpec& a = r.blocks[0].conditions[0].signal;
            const SignalSpec& t = r.blocks[0].conditions[1].signal;
            const SignalSpec& w = r.blocks[0].conditions[2].signal;
            CHECK(a.quantity == Quantity::InteriorAngle && a.bones == std::vector<int>{R(Role::LeftKnee)});
            CHECK(t.quantity == Quantity::Rotation && t.reference == Reference::Parent && t.axis == Axis::Total &&
                  t.measure == Measure::Speed);
            CHECK(w.quantity == Quantity::Rotation && w.reference == Reference::Floor && w.axis == Axis::Y);
            // Names.
            CHECK(SignalName(a, SignalBoneLabel(a)) == "L knee angle");
            SignalSpec as = a;
            as.measure = Measure::Speed;
            CHECK(SignalName(as, SignalBoneLabel(as)) == "L knee angle speed");
            CHECK(SignalName(t, SignalBoneLabel(t)) == "L toe rotation speed");
            SignalSpec tx = t;
            tx.axis = Axis::X;
            CHECK(SignalName(tx, SignalBoneLabel(tx)) == "L toe rotation X speed");
            tx.measure = Measure::Acceleration;
            tx.axis = Axis::Z;
            CHECK(SignalName(tx, SignalBoneLabel(tx)) == "L toe rotation Z acceleration");
            CHECK(SignalName(w, SignalBoneLabel(w)) == "LeftFoot rotation Y");
            SignalSpec wt = w;
            wt.axis = Axis::Total;  // an old record's angle on total reads as X
            CHECK(SignalName(wt, SignalBoneLabel(wt)) == "LeftFoot rotation X");
            // A bound joint angle ({parent, joint, child}) names its joint.
            SignalSpec ab = a;
            ab.bones = {R(Role::LeftUpLeg), R(Role::LeftKnee), R(Role::LeftHeel)};
            CHECK(SignalBoneLabel(ab) == "L knee");
        }

        // An old record (Footsteps v1 copies: q=joint, q=yaw) writes back byte-identical and reads as before.
        const std::string old =
            "RAVRULES 1\n"
            "options sensitivity=0 edge_ms=0 smooth_ms=8\n"
            "analyse floor_pct=2 pos_frac=0.25 speed_pct=30 margin_ratio=0.5 onset_frac=0.1 per_bone_floor=0\n"
            "block color=#5F9EDD hold_ms=0 cooldown_ms=250 offset_ms=0 land=peak:1:max marker=Footstep L\n"
            "cond q=point bones=role:left_heel,role:left_toe comb=lowest ref=floor meas=position axis=vertical "
            "dir=below thr=0.05 margin=0.025 fixed=0\n"
            "cond q=joint bones=role:left_up_leg,role:left_knee,role:left_heel comb=single ref=floor meas=speed "
            "axis=vertical signed=1 dir=above thr=30 margin=15 fixed=0\n"
            "cond q=yaw bones=role:left_heel,role:left_toe comb=single ref=floor meas=speed axis=vertical "
            "dir=below thr=95 margin=0 fixed=0\n";
        ItemRules o;
        CHECK(ParseItemRules(old, &o));
        CHECK(SerializeItemRules(o) == old);
        CHECK(o.blocks.size() == 1 && o.blocks[0].conditions.size() == 3 &&
              o.blocks[0].conditions[1].signal.quantity == Quantity::JointAngle &&
              o.blocks[0].conditions[2].signal.quantity == Quantity::Yaw);
        if (o.blocks.size() == 1 && o.blocks[0].conditions.size() == 3)
            CHECK(SignalName(o.blocks[0].conditions[1].signal, SignalBoneLabel(o.blocks[0].conditions[1].signal)) ==
                  "L knee bend speed");

        // Binding: a joint (role or raw bone) expands to {parent, joint, first child}; a root or
        // a leaf is no joint; the old 3-bone flexion binds as before.
        const std::vector<std::string> names = {"Hips", "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase",
                                                "LeftLegTwist"};
        const std::vector<int> parents = {-1, 0, 1, 2, 3, 2};
        std::vector<int> map(static_cast<size_t>(Role::Count), -1);
        map[R(Role::LeftKnee)] = 2;
        map[R(Role::LeftToe)] = 4;
        map[R(Role::LeftHeel)] = 3;
        map[R(Role::LeftUpLeg)] = 1;
        map[R(Role::Hips)] = 0;
        auto joint_block = [](int id) {
            Block b;
            Condition c;
            c.signal.quantity = Quantity::InteriorAngle;
            c.signal.bones = {id};
            b.conditions = {c};
            return b;
        };
        std::string missing;
        std::vector<Block> bl = {joint_block(R(Role::LeftKnee))};  // LeftLeg has two children: the first wins
        CHECK(BindBoneRefs(bl, map, names, parents, &missing) && missing.empty());
        CHECK(bl[0].conditions[0].signal.bones == (std::vector<int>{1, 2, 3}));
        bl = {joint_block(BoneRefForBone("LeftUpLeg"))};
        CHECK(BindBoneRefs(bl, map, names, parents, &missing));
        CHECK(bl[0].conditions[0].signal.bones == (std::vector<int>{0, 1, 2}));
        bl = {joint_block(BoneRefForBone("Hips"))};  // the root
        CHECK(!BindBoneRefs(bl, map, names, parents, &missing) && missing == "Hips (not a joint)");
        bl = {joint_block(R(Role::LeftToe))};  // a leaf
        CHECK(!BindBoneRefs(bl, map, names, parents, &missing) && missing == "left toe (not a joint)");
        CHECK(MissingBoneRefs({joint_block(R(Role::LeftToe)), joint_block(R(Role::RightKnee))}, map, names, parents) ==
              (std::vector<std::string>{"left toe (not a joint)", "right knee"}));
        bl = {joint_block(R(Role::LeftKnee))};  // no parents given: no joint angle binds
        CHECK(!BindBoneRefs(bl, map, names, &missing) && missing == "left knee (not a joint)");
        CHECK(BoneRefsUsed({joint_block(R(Role::LeftKnee))}) == std::vector<int>{R(Role::LeftKnee)});
        bl = o.blocks;
        CHECK(BindBoneRefs(bl, map, names, parents, &missing));
        CHECK(bl[0].conditions[1].signal.bones == (std::vector<int>{1, 2, 3}));
        // A rotation reads one bone and no reference bones.
        Block rb;
        Condition rc;
        rc.signal.quantity = Quantity::Rotation;
        rc.signal.reference = Reference::Parent;
        rc.signal.bones = {R(Role::LeftToe)};
        rb.conditions = {rc};
        bl = {rb};
        CHECK(BindBoneRefs(bl, map, names, parents, &missing) && bl[0].conditions[0].signal.bones == std::vector<int>{4});
    }

    // ---- 10-6: pooled copies ----
    // The pool line: right after the header, only when the record has one.
    {
        const std::string pid = "{0A1B2C3D-4E5F-4A6B-8C7D-9E0F1A2B3C4D}";
        ItemRules         r = FullRecord();
        const std::string without = SerializeItemRules(r);
        CHECK(without.find("pool") == std::string::npos);
        r.has_pool = true;
        r.pool_id = pid;
        const std::string with = SerializeItemRules(r);
        const std::string top = "RAVRULES 1\npool id=" + pid + "\noptions ";
        CHECK(with.compare(0, top.size(), top) == 0);
        CHECK(with.substr(top.size() - 8) == without.substr(11));  // the rest as without it
        ItemRules p;
        CHECK(ParseItemRules(with, &p));
        CHECK(p.has_pool && p.pool_id == pid && !HasKeptText(p));
        CHECK(SerializeItemRules(p) == with);
        CHECK(PoolContentEqual(p, r));
        // Absent line: no pool (the file's default one), written back as read.
        ItemRules q;
        CHECK(ParseItemRules(without, &q) && !q.has_pool && q.pool_id.empty() && SerializeItemRules(q) == without);
        // An id with a space, ',', '=' or '%' reads back (encoded as a key).
        r.pool_id = "my pool,=%";
        const std::string odd = SerializeItemRules(r);
        CHECK(odd.find("\npool id=my%20pool%2C%3D%25\n") != std::string::npos);
        CHECK(ParseItemRules(odd, &p) && p.pool_id == "my pool,=%" && SerializeItemRules(p) == odd);
        // A pool line only at the top level: inside the preset copy it is kept as text.
        const std::string in_copy =
            "RAVRULES 1\npreset id=user/a version=1 kept=0 name=A\ncopy\npool id=x\nend\n";
        CHECK(ParseItemRules(in_copy, &p) && !p.has_pool && p.preset_copy.copy_kept.lines ==
                                                                std::vector<std::string>{"pool id=x"});
    }
    // Unknown-version keep: a later version's pool line, its unknown field and lines, come back in
    // place; an edit of the id keeps them.
    {
        const std::string s = "RAVRULES 3 extra\n"
                              "note before\n"
                              "pool id=abc mode=link\n"
                              "pool lane 2\n"
                              "after pool\n"
                              "options sensitivity=0 edge_ms=0 smooth_ms=8\n"
                              "analyse floor_pct=2 pos_frac=0.25 speed_pct=30 margin_ratio=0.5 onset_frac=0.1 "
                              "per_bone_floor=0\n";
        ItemRules p;
        CHECK(ParseItemRules(s, &p));
        CHECK(p.format_version == 3 && p.has_pool && p.pool_id == "abc");
        CHECK(p.head.lines == std::vector<std::string>{"note before"});
        CHECK(p.pool_kept.fields.size() == 2 && p.pool_kept.fields[1].key == "mode");
        CHECK(p.pool_kept.lines == (std::vector<std::string>{"pool lane 2", "after pool"}));  // a second one: text
        CHECK(HasKeptText(p));
        const std::string back = SerializeItemRules(p);
        CHECK(back == s);
        if (back != s) std::printf("--- got ---\n%s--- want ---\n%s", back.c_str(), s.c_str());
        p.pool_id = "def";  // Make unique on it
        CHECK(SerializeItemRules(p).find("note before\npool id=def mode=link\npool lane 2\nafter pool\noptions ") !=
              std::string::npos);
        // A record that never had a pool line keeps having none after an edit.
        ItemRules o;
        CHECK(ParseItemRules("RAVRULES 1\nblock marker=Step\n", &o) && !o.has_pool);
        o.blocks[0].marker = "Step 2";
        CHECK(SerializeItemRules(o).find("pool") == std::string::npos);
    }
    // An older RAV (no `pool` line kind) reads the line as an unknown header line and writes it back
    // in place: the same path as any unknown line right after the header.
    {
        const std::string s = "RAVRULES 1\npoolx id={P}\noptions sensitivity=0 edge_ms=0 smooth_ms=8\n"
                              "analyse floor_pct=2 pos_frac=0.25 speed_pct=30 margin_ratio=0.5 onset_frac=0.1 "
                              "per_bone_floor=0\n";
        ItemRules p;
        CHECK(ParseItemRules(s, &p) && p.head.lines == std::vector<std::string>{"poolx id={P}"} && !p.has_pool);
        CHECK(SerializeItemRules(p) == s);
    }
    // The shared content: CopyPoolContent / PoolContentEqual.
    {
        ItemRules a = FullRecord();
        a.has_pool = true;
        a.pool_id = "A";
        a.has_applied = true;
        a.applied.sig = "SIGA";
        a.applied.item = "{IA}";
        TakeMarkerRef tm;
        tm.t = 1;
        tm.name = "Foot L";
        a.tmarkers = {tm};
        a.head.lines = {"head a"};
        a.tail = {{1, "tail a"}};
        ItemRules b;
        b.has_pool = true;
        b.pool_id = "B";
        b.has_previewed = true;
        b.previewed.sig = "SIGB";
        b.head.lines = {"head b"};
        CHECK(!PoolContentEqual(a, b));
        CopyPoolContent(a, b);
        CHECK(PoolContentEqual(a, b) && PoolContentEqual(b, a));
        // The content came: options, analyse, the preset and its copy, the blocks, the events.
        CHECK(OptionsEqual(b.options, a.options) && AnalyseEqual(b.analyse, a.analyse) && BlocksEqual(b.blocks, a.blocks));
        CHECK(b.has_preset && b.preset_copy.id == a.preset_copy.id && b.preset_copy.kept_version == 4 &&
              BlocksEqual(b.preset_copy.blocks, a.preset_copy.blocks));
        CHECK(b.events.size() == 1 && b.events[0].t == 1 && b.events[0].kind == EventKind::Detected);
        // b's own bookkeeping stayed: pool, applied / previewed, marker refs, head and tail.
        CHECK(b.has_pool && b.pool_id == "B" && !b.has_applied && b.has_previewed && b.previewed.sig == "SIGB" &&
              b.tmarkers.empty());
        CHECK(b.head.lines == std::vector<std::string>{"head b"} && b.tail.empty());
        ItemRules pb;
        CHECK(ParseItemRules(SerializeItemRules(b), &pb) && PoolContentEqual(pb, a) && pb.pool_id == "B");
        // Bookkeeping never makes two contents differ.
        ItemRules c = a;
        c.applied.sig = "other";
        c.pool_id = "C";
        c.has_previewed = true;
        c.head.lines.clear();
        c.tail.clear();
        c.tmarkers.clear();
        c.format_version = 2;
        CHECK(PoolContentEqual(a, c));
        // Each content field does.
        c = a;
        c.options.sensitivity = 0.3;
        CHECK(!PoolContentEqual(a, c));
        c = a;
        c.analyse.margin_ratio = 0.9;
        CHECK(!PoolContentEqual(a, c));
        c = a;
        c.has_preset = false;
        CHECK(!PoolContentEqual(a, c));
        c = a;
        c.preset_copy.kept_version = 9;
        CHECK(!PoolContentEqual(a, c));
        c = a;
        c.blocks[0].conditions[0].threshold = 2;
        CHECK(!PoolContentEqual(a, c));
        c = a;
        c.blocks[1].enabled = false;
        CHECK(!PoolContentEqual(a, c));
        c = a;
        EventEntry ue;
        ue.t = 2.5;
        ue.kind = EventKind::User;
        c.events.push_back(ue);
        CHECK(!PoolContentEqual(a, c));
        c = a;
        c.blocks[0].kept.lines.push_back("lane 9");  // kept text on a content line is written: content
        CHECK(!PoolContentEqual(a, c));
        // No rules at all (no record) is a content too: the empty one.
        CHECK(PoolContentEqual(ItemRules{}, ItemRules{}) && !PoolContentEqual(ItemRules{}, a));
        // A copy onto itself changes nothing.
        ItemRules d = a;
        CopyPoolContent(d, d);
        CHECK(SerializeItemRules(d) == SerializeItemRules(a));
    }
    // SamePoolPath: ASCII case and slashes ignored, nothing else.
    {
        CHECK(SamePoolPath("C:\\Anims\\Walk.fbx", "c:/anims/WALK.FBX"));
        CHECK(SamePoolPath("/home/a/walk.glb", "\\home\\a\\walk.glb"));
        CHECK(SamePoolPath("D:\\x\\Pas \xC3\xA9.fbx", "d:/X/pas \xC3\xA9.FBX"));  // other bytes compared as they are
        CHECK(!SamePoolPath("D:\\x\\\xC3\x89.fbx", "D:\\x\\\xC3\xA9.fbx"));      // only ASCII letters fold
        CHECK(!SamePoolPath("C:\\Anims\\Walk.fbx", "C:\\Anims\\Walk2.fbx"));
        CHECK(!SamePoolPath("C:\\Anims\\Walk.fbx", "C:\\Anims\\Walk.glb"));
        CHECK(!SamePoolPath("C:\\Anims\\Walk.fbx", "C:\\Anims\\Walk.fbx "));
        CHECK(!SamePoolPath("", ""));
        CHECK(!SamePoolPath("a", ""));
        CHECK(!SamePoolPath("", "a"));
    }

    // ---- 10-6: pool membership and Commit / Cancel's grouping (spec I/O matrix) ----
    // item_rules.cpp's scans (ScanPool, ItemPoolKey, WithPoolMembers) only read each item's path
    // and record into a PoolCandidate; these decide.
    {
        const std::string walk = "C:\\Anims\\Walk.fbx";
        const std::string run = "C:\\Anims\\Run.fbx";
        ItemRules         tagged = FullRecord();
        const std::string rec = SerializeItemRules(tagged);  // a tagged copy, default pool
        ItemRules         uniq = tagged;
        uniq.has_pool = true;
        uniq.pool_id = "{U}";
        const std::string rec_u = SerializeItemRules(uniq);  // made unique
        auto cand = [](const std::string& path, const std::string& record) {
            PoolCandidate c;
            c.path = path;
            c.record = record;
            return c;
        };
        std::string id;
        ItemRules   parsed;

        // Matrix "Copies before tagging": 5 copies, no records: one pool (the file's default one).
        {
            const std::vector<PoolCandidate> p(5, cand(walk, ""));
            CHECK(PoolKeyOf(walk, "", &id, &parsed) && id.empty() && parsed.blocks.empty());
            for (const PoolCandidate& c : p) CHECK(InPool(c, walk, ""));
            const PoolGrouping g = GroupPools(p, {0});  // Load preset on copy 1
            CHECK(g.order == (std::vector<size_t>{0, 1, 2, 3, 4}));
            CHECK(g.pool_of == (std::vector<int>{0, 0, 0, 0, 0}));
            // A tagged copy and an untagged one (no record) are one pool.
            CHECK(InPool(cand(walk, rec), walk, "") && InPool(cand(walk, ""), walk, ""));
            CHECK(InPool(cand(walk, rec), walk, "", &parsed) && PoolContentEqual(parsed, tagged));
        }
        // Matrix "Unique": Make unique on copy 4, then a tweak on copy 1: copy 4 is not in copy 1's pool.
        {
            const std::vector<PoolCandidate> p = {cand(walk, rec), cand(walk, rec), cand(walk, rec), cand(walk, rec_u),
                                                  cand(walk, rec)};
            CHECK(PoolKeyOf(walk, rec_u, &id) && id == "{U}");
            CHECK(!InPool(p[3], walk, ""));   // another pool id: out of the default pool
            CHECK(InPool(p[3], walk, "{U}"));  // its own
            PoolGrouping g = GroupPools(p, {0});
            CHECK(g.order == (std::vector<size_t>{0, 1, 2, 4}));
            g = GroupPools(p, {3});  // a tweak on copy 4 reaches no other copy
            CHECK(g.order == std::vector<size_t>{3} && g.pool_of == std::vector<int>{0});
        }
        // Matrix "Duplicate a unique": the duplicate carries the same pool id: linked with copy 4 only.
        {
            const std::vector<PoolCandidate> p = {cand(walk, rec), cand(walk, rec), cand(walk, rec), cand(walk, rec_u),
                                                  cand(walk, rec), cand(walk, rec_u)};
            PoolGrouping g = GroupPools(p, {3});
            CHECK(g.order == (std::vector<size_t>{3, 5}) && g.pool_of == (std::vector<int>{0, 0}));
            g = GroupPools(p, {5});
            CHECK(g.order == (std::vector<size_t>{5, 3}));
            g = GroupPools(p, {0});
            CHECK(g.order == (std::vector<size_t>{0, 1, 2, 4}));  // the other copies never reach them
        }
        // Matrix "Unreadable member": a record that does not parse is in no pool, never written.
        {
            const std::string bad = "garbage";
            const std::vector<PoolCandidate> p = {cand(walk, rec), cand(walk, bad), cand(walk, rec)};
            CHECK(!PoolKeyOf(walk, bad, &id));
            CHECK(!InPool(p[1], walk, "") && !InPool(p[1], walk, "{U}"));
            PoolGrouping g = GroupPools(p, {0});
            CHECK(g.order == (std::vector<size_t>{0, 2}));
            g = GroupPools(p, {1});  // selected itself: kept, a pool of its own, no member
            CHECK(g.order == std::vector<size_t>{1} && g.pool_of == std::vector<int>{0});
        }
        // Matrix "Relinked file": an item relinked to another file leaves the old pool, joins the new
        // file's. Case and slashes are no other file.
        {
            const std::vector<PoolCandidate> p = {cand(walk, rec), cand(run, rec), cand("c:/anims/WALK.fbx", rec),
                                                  cand(run, "")};
            CHECK(!InPool(p[1], walk, ""));
            CHECK(InPool(p[2], walk, ""));
            PoolGrouping g = GroupPools(p, {0});
            CHECK(g.order == (std::vector<size_t>{0, 2}));
            g = GroupPools(p, {1});
            CHECK(g.order == (std::vector<size_t>{1, 3}));
            // Not a RAV item (no path): in no pool, whatever its record.
            CHECK(!PoolKeyOf("", rec, &id) && !InPool(cand("", rec), "", ""));
        }
        // Commit / Cancel: the selected items first (selection order, each once), then, pool by pool
        // in the order of their first selected item, the other members in project order.
        {
            const std::vector<PoolCandidate> p = {
                cand(walk, rec),     // 0  walk, default pool
                cand(run, ""),       // 1  run, default pool
                cand(walk, rec_u),   // 2  walk, {U}
                cand(walk, ""),      // 3  walk, default pool (no record)
                cand("", ""),        // 4  not a RAV item
                cand(run, rec),      // 5  run, default pool
                cand(walk, rec),     // 6  walk, default pool
                cand(walk, rec_u),   // 7  walk, {U}
                cand(walk, "x"),     // 8  walk, unreadable
            };
            const PoolGrouping g = GroupPools(p, {6, 4, 5, 0, 6, 99, 2});
            // Selected: 6, 4, 5, 0 (6's pool again), 2; the repeat of 6 and 99 skipped. Then pool 0
            // (walk default): 3; pool 1 (no pool): none; pool 2 (run): 1; pool 3 ({U}): 7.
            CHECK(g.order == (std::vector<size_t>{6, 4, 5, 0, 2, 3, 1, 7}));
            CHECK(g.pool_of == (std::vector<int>{0, 1, 2, 0, 3, 0, 2, 3}));
            // Every candidate at most once; the unreadable one never drawn in.
            std::vector<size_t> sorted = g.order;
            std::sort(sorted.begin(), sorted.end());
            CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
            CHECK(std::find(g.order.begin(), g.order.end(), size_t{8}) == g.order.end());
            // An item asked about that is not one of the project's: it stands and draws its pool's
            // members in, but is never drawn in as another item's member.
            std::vector<PoolCandidate> q = p;
            q.push_back(cand(walk, rec));
            q.back().in_project = false;  // 9
            PoolGrouping h = GroupPools(q, {0});
            CHECK(h.order == (std::vector<size_t>{0, 3, 6}));
            h = GroupPools(q, {9});
            CHECK(h.order == (std::vector<size_t>{9, 0, 3, 6}));
            CHECK(GroupPools(q, {}).order.empty());
        }
    }

    // ---- 10-6: a gesture on a pool (item_rules.cpp ModifyItemRules: PoolGestureStart / PlanPoolGesture) ----
    {
        const std::string walk = "C:\\Anims\\Walk.fbx";
        const ItemRules   tagged = FullRecord();
        ItemRules         uniq = tagged;
        uniq.has_pool = true;
        uniq.pool_id = "{U}";
        uniq.blocks[0].conditions[0].threshold = 9;  // tuned on its own
        auto rec = [](bool present, const ItemRules& r) {
            PoolRecord p;
            p.present = present;
            p.rules = present ? r : ItemRules{};
            return p;
        };
        // The pool's other copies as a scan reads them: the candidates InPool keeps for (path, id).
        auto members_of = [&](const std::vector<PoolCandidate>& cands, size_t self, const std::string& id) {
            std::vector<PoolRecord> out;
            for (size_t i = 0; i < cands.size(); ++i) {
                ItemRules parsed;
                if (i != self && InPool(cands[i], walk, id, &parsed)) out.push_back(rec(!cands[i].record.empty(), parsed));
            }
            return out;
        };
        auto load_preset = [&](ItemRules& r) {  // a gesture that sets the content
            r.options = tagged.options;
            r.analyse = tagged.analyse;
            r.blocks = tagged.blocks;
            r.has_preset = true;
            r.preset_copy = tagged.preset_copy;
        };
        bool adopted = true;

        // Matrix "Copies before tagging": 5 copies, no records; Load preset on copy 1: copy 1 and the
        // 4 others are written.
        {
            const std::vector<PoolRecord> m(4, rec(false, ItemRules{}));
            ItemRules r = PoolGestureStart(false, false, ItemRules{}, m, &adopted);
            CHECK(!adopted && r.blocks.empty());
            const std::string before = SerializeItemRules(r);
            load_preset(r);
            const PoolGesturePlan plan = PlanPoolGesture(false, "", before, adopted, r, m);
            CHECK(plan.write_self && plan.members == (std::vector<size_t>{0, 1, 2, 3}));
            // A gesture that changes nothing there writes nothing.
            const ItemRules none = PoolGestureStart(false, false, ItemRules{}, m, &adopted);
            const PoolGesturePlan idle = PlanPoolGesture(false, "", SerializeItemRules(none), adopted, none, m);
            CHECK(!idle.write_self && idle.members.empty());
        }
        // Matrix "Unique": copy 4 made unique, then a tweak on copy 1: copy 4 is no member, so never
        // written; a tweak on copy 4 writes it alone.
        {
            PoolCandidate t, u;
            t.path = u.path = walk;
            t.record = SerializeItemRules(tagged);
            u.record = SerializeItemRules(uniq);
            const std::vector<PoolCandidate> cands = {t, t, t, u, t};
            const std::vector<PoolRecord>    m = members_of(cands, 0, "");
            CHECK(m.size() == 3);  // copies 2, 3, 5
            ItemRules r = PoolGestureStart(true, true, tagged, m, &adopted);
            CHECK(!adopted);
            const std::string before = SerializeItemRules(r);
            r.blocks[0].conditions[0].threshold = 0.5;  // the tweak
            PoolGesturePlan plan = PlanPoolGesture(true, before, before, adopted, r, m);
            CHECK(plan.write_self && plan.members == (std::vector<size_t>{0, 1, 2}));
            for (const PoolRecord& x : m) CHECK(x.rules.pool_id.empty());  // never the unique one
            const std::vector<PoolRecord> mu = members_of(cands, 3, "{U}");
            CHECK(mu.empty());
            ItemRules ru = PoolGestureStart(true, true, uniq, mu, &adopted);
            const std::string ub = SerializeItemRules(ru);
            ru.blocks[0].conditions[0].threshold = 0.6;
            plan = PlanPoolGesture(true, ub, ub, adopted, ru, mu);
            CHECK(plan.write_self && plan.members.empty());
        }
        // An item added later (no record) joins its pool: it holds the pool's tagging; a gesture on it
        // starts from that, and writes it AND the copies that differ.
        {
            ItemRules other = tagged;
            other.blocks.pop_back();  // a copy tagged differently (an earlier build)
            const std::vector<PoolRecord> m = {rec(false, ItemRules{}), rec(true, tagged), rec(true, other)};
            ItemRules r = PoolGestureStart(false, false, ItemRules{}, m, &adopted);
            CHECK(adopted && PoolContentEqual(r, tagged));  // the first copy that has content
            CHECK(!r.has_pool && !r.has_applied);           // content only, no bookkeeping
            const std::string before = SerializeItemRules(r);
            Block nb;
            nb.marker = "Added";
            r.blocks.push_back(nb);  // "+ Rule" on it: added to the shared rules
            PoolGesturePlan plan = PlanPoolGesture(false, "", before, adopted, r, m);
            CHECK(plan.write_self && plan.members == (std::vector<size_t>{0, 1, 2}));
            CHECK(r.blocks.size() == tagged.blocks.size() + 1);
            // The no-op case: an edit that changes nothing still writes the item (it takes the pool's
            // tagging as its record); the copies that hold it already are not written.
            ItemRules same = PoolGestureStart(false, false, ItemRules{}, m, &adopted);
            const std::string sb = SerializeItemRules(same);
            plan = PlanPoolGesture(false, "", sb, adopted, same, m);
            CHECK(plan.write_self && plan.members == (std::vector<size_t>{0, 2}));
            const std::vector<PoolRecord> agreeing = {rec(true, tagged), rec(true, tagged)};
            same = PoolGestureStart(false, false, ItemRules{}, agreeing, &adopted);
            plan = PlanPoolGesture(false, "", SerializeItemRules(same), adopted, same, agreeing);
            CHECK(adopted && plan.write_self && plan.members.empty());
        }
        // An item with a record of its own never adopts (even an empty one, or one that does not read);
        // a no-op gesture on it writes only the copies tagged differently.
        {
            ItemRules other = tagged;
            other.options.sensitivity = 0.9;
            const std::vector<PoolRecord> m = {rec(true, tagged), rec(true, other)};
            ItemRules r = PoolGestureStart(true, true, tagged, m, &adopted);
            CHECK(!adopted);
            const std::string raw = SerializeItemRules(tagged);
            PoolGesturePlan plan = PlanPoolGesture(true, raw, SerializeItemRules(r), adopted, r, m);
            CHECK(!plan.write_self && plan.members == std::vector<size_t>{1});
            ItemRules empty_own;
            r = PoolGestureStart(true, true, empty_own, m, &adopted);
            CHECK(!adopted && r.blocks.empty());
            r = PoolGestureStart(true, false, tagged, m, &adopted);  // unreadable: no rules, no adoption
            CHECK(!adopted && r.blocks.empty());
            // Nothing in the pool changes: nothing is written.
            const std::vector<PoolRecord> eq = {rec(true, tagged)};
            r = PoolGestureStart(true, true, tagged, eq, &adopted);
            plan = PlanPoolGesture(true, raw, SerializeItemRules(r), adopted, r, eq);
            CHECK(!plan.write_self && plan.members.empty());
        }
    }

    // ---- 10-5 frozen fixtures ----
    // Written by the 10-5 build (the shipped v1 grammar) and committed as text. NEVER
    // regenerate them: a later format change must keep reading these and writing them back
    // byte-identical, so a project saved with this build opens unchanged in any later one.
    {
        // The item record: preset copy, tuned rules (a fixed threshold, a peak landing, an
        // interior angle, a switched-off rule with a rotation, a yaw and a bone key), every
        // event kind, the committed markers (one hidden) and pending preview markers.
        const std::string fx = ReadFixture("rav_rules_v1_10_5.txt");
        CHECK(!fx.empty());
        ItemRules p;
        CHECK(ParseItemRules(fx, &p));
        CHECK(!HasKeptText(p));
        const std::string back = SerializeItemRules(p);
        CHECK(back == fx);
        if (back != fx) std::printf("--- got ---\n%s--- want ---\n%s", back.c_str(), fx.c_str());
        CHECK(p.format_version == 1);
        CHECK(p.options.sensitivity == 0.15 && p.options.edge_margin_ms == 20);
        // Keep current: the item copied v1 and dismissed the installed v2.
        CHECK(p.has_preset && p.preset_copy.id == "factory/footsteps" && p.preset_copy.version == 1 &&
              p.preset_copy.kept_version == 2 && p.preset_copy.name == "Footsteps" &&
              p.preset_copy.blocks.size() == 2);
        CHECK(p.blocks.size() == 3);
        if (p.blocks.size() == 3) {
            CHECK(p.blocks[0].conditions.size() == 1);
            if (p.blocks[0].conditions.size() == 1)
                CHECK(p.blocks[0].conditions[0].threshold == 0.0437 && !p.blocks[0].conditions[0].auto_threshold);
            CHECK(p.blocks[0].offset_ms == -4.5 && p.blocks[0].min_hold_ms == 12);
            CHECK(p.blocks[1].landing == Landing::PeakOf && p.blocks[1].peak_condition == 1 && !p.blocks[1].peak_max);
            CHECK(p.blocks[1].conditions.size() == 2);
            if (p.blocks[1].conditions.size() == 2)
                CHECK(p.blocks[1].conditions[1].signal.quantity == Quantity::InteriorAngle);
            CHECK(!p.blocks[2].enabled && p.blocks[2].marker == "Toe Off L" && p.blocks[2].conditions.size() == 3);
            if (p.blocks[2].conditions.size() == 3) {
                CHECK(p.blocks[2].conditions[0].signal.quantity == Quantity::Rotation &&
                      p.blocks[2].conditions[0].signal.reference == Reference::Parent);
                CHECK(p.blocks[2].conditions[1].signal.quantity == Quantity::Yaw);
                const std::vector<int>& bn = p.blocks[2].conditions[2].signal.bones;
                CHECK(bn.size() == 1);
                if (bn.size() == 1) CHECK(BoneRefKey(bn[0]) == "bone:mixamorig:Left Toe_End");
            }
        }
        CHECK(p.events.size() == 5);
        if (p.events.size() == 5) {
            CHECK(p.events[0].t == 0.4625 && p.events[0].kind == EventKind::Detected && p.events[0].block == 0 &&
                  p.events[0].strength == 0.82 && p.events[0].speed == 1.91);
            CHECK(p.events[2].kind == EventKind::User && p.events[2].t == 1.5125);
            CHECK(p.events[3].kind == EventKind::Suppress && p.events[3].block == 1 && !p.events[3].has_strength);
            CHECK(p.events[4].kind == EventKind::User && p.events[4].t == 2.5 && !p.events[4].has_speed);
        }
        CHECK(p.has_applied && p.applied.mode == MarkerMode::Both && p.applied.sig == "9a3f0c21d4e5b687" &&
              p.applied.item == "{6B1E2F3A-4C5D-4E6F-8A9B-0C1D2E3F4A5B}");
        CHECK(p.tmarkers.size() == 3 && p.tmarkers[1].t == 0.9875 && p.tmarkers[1].name == "Footstep R");
        CHECK(p.pmarkers.size() == 3);
        if (p.pmarkers.size() == 3) {
            CHECK(p.pmarkers[0].guid == "{0F1E2D3C-4B5A-4978-8695-A4B3C2D1E0F9}" && p.pmarkers[0].t == 12.4625 &&
                  p.pmarkers[0].has_c && p.pmarkers[0].c == 0.4625 && p.pmarkers[0].color == (0x1000000u | 0x5F9EDDu) &&
                  p.pmarkers[0].name == "Footstep L");
            CHECK(p.pmarkers[2].guid.empty() && p.pmarkers[2].c == 1.5125);  // hidden by the mirror
        }
        CHECK(p.has_previewed && p.previewed.sig == "5c7e19ab02f4d836");
        CHECK(p.ptmarkers.size() == 1 && p.ptmarkers[0].name == "Footstep R - Preview");
        CHECK(p.ppmarkers.size() == 1 && p.ppmarkers[0].c == 2.5 && p.ppmarkers[0].color == (0x1000000u | 0x6F4F30u));
        // 10-6: no pool line: the file's default pool. A copy of its content onto a record of
        // another pool leaves that record's pool, and the fixture's own text, as they are.
        CHECK(!p.has_pool && p.pool_id.empty());
        ItemRules other;
        other.has_pool = true;
        other.pool_id = "{U}";
        CopyPoolContent(p, other);
        CHECK(PoolContentEqual(other, p) && other.pool_id == "{U}" && !other.has_applied);
        CHECK(SerializeItemRules(p) == fx);
    }
    {
        // The Cancel snapshot (the record as of the last Commit, its own take key, same grammar).
        const std::string fx = ReadFixture("rav_rules_committed_v1_10_5.txt");
        CHECK(!fx.empty());
        ItemRules p;
        CHECK(ParseItemRules(fx, &p));
        CHECK(!HasKeptText(p));
        CHECK(SerializeItemRules(p) == fx);
        CHECK(p.events.size() == 4 && p.has_applied && p.pmarkers.size() == 3);
        CHECK(!p.has_previewed && p.ptmarkers.empty() && p.ppmarkers.empty());
        CHECK(!p.has_pool);  // 10-6
    }

    if (g_fails) {
        std::printf("rule_record_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("rule_record_test: all passed\n");
    return 0;
}
