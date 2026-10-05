// SPDX-License-Identifier: MIT
//
// Host test of the per-item rules record (src/rule_record.h, story 10-2): round trip,
// a record written by a future version, neutral defaults, a corrupt record, signal
// names, missing roles. No REAPER, no Windows: any C++17 compiler.

#include "rule_record.h"

#include <clocale>
#include <cstdio>
#include <fstream>
#include <sstream>
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
        // An unknown role key is kept (and reported missing when bound), never dropped.
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

        // Binding: the unknown role is reported missing.
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
        CHECK(!RoleFromKey("left_hand", nullptr));
        CHECK(BoneRefId("role:left_hand") >= kBoneRefExtra);
        CHECK(BoneRefId("role:left_hand") == BoneRefId("role:left_hand"));
        CHECK(BoneRefName(BoneRefId("role:left_hand")) == "left hand");
        CHECK(RoleShortLabel(Role::LeftHeel) == "L heel" && RoleShortLabel(Role::Hips) == "hips");
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

    if (g_fails) {
        std::printf("rule_record_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("rule_record_test: all passed\n");
    return 0;
}
