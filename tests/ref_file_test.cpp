// SPDX-License-Identifier: MIT
//
// Host test of the all-trigger bench's pure pieces (tests/ref_file.h, story 10-8o): the
// reference file's parse / write round trip, the old REF markers as references, which
// configuration a reference counts in, and the row match (sideless, deleted, outside the
// visible window). No REAPER.

#include "ref_file.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace rav;
using namespace rav::bench;

namespace {

int g_fails = 0;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);        \
            ++g_fails;                                              \
        }                                                           \
    } while (0)

bool Near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }

// The refs of one configuration's row, as trigger_bench builds them (deleted lines left out).
std::vector<RowRef> RowRefs(const std::vector<RefLine>& lines, BenchConfig c, const std::string& target, char side)
{
    std::vector<RowRef> out;
    for (const RefLine& r : lines) {
        if (r.status == "deleted") continue;
        const std::string tg = RefTarget(r, c);
        if (tg != target || RowSide(tg, r.side) != side) continue;
        out.push_back({r.time_s, r.status == "confirmed"});
    }
    return out;
}

}  // namespace

int main()
{
    // ---- Parse / write round trip.
    {
        const std::string text = "# kind\tside\tpart\ttime_s\tstatus\tsource\n"
                                 "step\tL\theel\t1.2000\tconfirmed\tREF\n"
                                 "\n"
                                 "jump  R  any  0.56  draft  motion\n"   // spaces read too
                                 "slide_heel\tR\theel\t2.5\tdeleted\tantho\n"
                                 "bogus\tL\tany\t1\tdraft\tmotion\n"     // unknown kind
                                 "step\tX\tany\t1\tdraft\tmotion\n"      // bad side
                                 "step\tL\tany\tabc\tdraft\tmotion\n"    // bad time
                                 "step\tL\tany\t1\tmaybe\tmotion\n"      // bad status
                                 "step\tL\tany\t1\tdraft\n"              // 5 fields
                                 "step\tB\tany\t1\tdraft\tmotion\n"      // B only for jump / land
                                 "land\tB\tany\t1.5\tconfirmed\tREF\n";
        std::vector<std::string> errors;
        const std::vector<RefLine> lines = ParseRefFile(text, &errors);
        CHECK(lines.size() == 4);
        CHECK(errors.size() == 6);
        CHECK(lines[3].kind == "land" && lines[3].side == 'B');
        std::string e;
        CHECK(!ParseRefLine("lift\tB\tany\t1\tdraft\tmotion", nullptr, &e) && e.find("L or R") != std::string::npos &&
              e.find("jump") == std::string::npos);
        CHECK(!ParseRefLine("jump\tX\tany\t1\tdraft\tmotion", nullptr, &e) && e.find("B for jump / land") != std::string::npos);
        CHECK(lines[0].kind == "step" && lines[0].side == 'L' && lines[0].part == "heel" && Near(lines[0].time_s, 1.2) &&
              lines[0].status == "confirmed" && lines[0].source == "REF");
        CHECK(lines[1].kind == "jump" && lines[1].side == 'R' && Near(lines[1].time_s, 0.56) && lines[1].status == "draft");
        CHECK(lines[2].status == "deleted" && lines[2].source == "antho");
        const std::string w = FormatRefFile(lines);
        CHECK(FormatRefLine(lines[0]) == "step\tL\theel\t1.2000\tconfirmed\tREF");
        std::vector<std::string> e2;
        const std::vector<RefLine> back = ParseRefFile(w, &e2);
        CHECK(e2.empty() && back.size() == 4);
        CHECK(FormatRefFile(back) == w);
    }
    // ---- Story 10-9h: the other wave-3 kinds; jolt, spin, spin_end are side B; a kick as a step.
    {
        RefLine r;
        std::string e;
        for (const char* k : {"punch", "swing", "clap", "hand_body", "tap", "drag", "knee", "elbow"}) {
            CHECK(ParseRefLine(std::string(k) + "\tL\tany\t1\tdraft\tmotion", &r, &e));
            CHECK(!ParseRefLine(std::string(k) + "\tB\tany\t1\tdraft\tmotion", nullptr, &e));
        }
        for (const char* k : {"jolt", "spin", "spin_end", "sit", "fall"}) CHECK(ParseRefLine(std::string(k) + "\tB\tany\t1\tdraft\tmotion", &r, &e));
        CHECK(ParseRefLine("punch\tR\tany\t0.97\tdraft\tmotion", &r, &e) && RefTarget(r, BenchConfig::Default) == "punch" &&
              RefTarget(r, BenchConfig::Parts) == "punch");
        CHECK(ParseRefLine("kick\tL\tany\t1.18\tdraft\tmotion", &r, &e) && RefTarget(r, BenchConfig::Default) == "kick" &&
              RefTarget(r, BenchConfig::Parts).empty());
        CHECK(ParseRefLine("kick\tL\ttoe\t1.18\tdraft\tmotion", &r, &e) && RefTarget(r, BenchConfig::Default).empty() &&
              RefTarget(r, BenchConfig::Parts) == "kick_toe");
        CHECK(ParseRefLine("kick_heel\tR\tany\t1\tdraft\tmotion", &r, &e) && RefTarget(r, BenchConfig::Parts) == "kick_heel");
        CHECK(ParseRefLine("spin_end\tB\tany\t1.55\tdraft\tmotion", &r, &e) && RefTarget(r, BenchConfig::Default) == "spin_end");
    }

    // ---- Configuration targets.
    {
        RefLine r;
        r.kind = "step", r.part = "heel";
        CHECK(RefTarget(r, BenchConfig::Default).empty());  // a heel ref: parts only
        CHECK(RefTarget(r, BenchConfig::Parts) == "heel");
        r.part = "any";
        CHECK(RefTarget(r, BenchConfig::Default) == "step");
        CHECK(RefTarget(r, BenchConfig::Parts).empty());
        r.part = "tip";
        CHECK(RefTarget(r, BenchConfig::Parts) == "toe");
        r.kind = "heel", r.part = "any";  // a part kind reads its part
        CHECK(RefTarget(r, BenchConfig::Parts) == "heel" && RefTarget(r, BenchConfig::Default).empty());
        r.kind = "pivot", r.part = "toe";
        CHECK(RefTarget(r, BenchConfig::Parts) == "pivot_toe" && RefTarget(r, BenchConfig::Default).empty());
        r.kind = "slide_heel", r.part = "any";
        CHECK(RefTarget(r, BenchConfig::Parts) == "slide_heel");
        r.kind = "lift", r.part = "toe";
        CHECK(RefTarget(r, BenchConfig::Default) == "lift" && RefTarget(r, BenchConfig::Parts) == "lift");
        r.kind = "jump", r.part = "any";
        // Story 10-9a-3: jump and land are scored per side.
        CHECK(RefTarget(r, BenchConfig::Default) == "jump" && RowSide("jump", 'R') == 'R' && RowSide("lift", 'R') == 'R');
        r.kind = "jump_toe", r.part = "any";
        CHECK(RefTarget(r, BenchConfig::Default) == "jump" && RefTarget(r, BenchConfig::Parts) == "jump");
        CHECK(FoundTarget("lift_heel") == "lift" && FoundTarget("jump_toe") == "jump" && FoundTarget("land_heel") == "land");
        r.kind = "hand_pivot_any";
        CHECK(RefTarget(r, BenchConfig::Default) == "hand_pivot");
        CHECK(FoundTarget("pivot_toe_longest") == "pivot_toe" && FoundTarget("hand_pivot_any") == "hand_pivot" &&
              FoundTarget("step") == "step");
    }

    // ---- Old REF markers (Jumping Down's '# ref.left_heel=1.2').
    {
        RefTimes refs;
        ReadRefHeader("ref", "1.2,0.9,2.0", &refs);
        ReadRefHeader("ref.left_heel", "1.2", &refs);
        ReadRefHeader("ref_bone.left_heel", "mixamorig:LeftFoot", &refs);
        ReadRefHeader("ref.mixamorig_righttoe_end", "0.9", &refs);
        ReadRefHeader("ref_bone.mixamorig_righttoe_end", "mixamorig:RightToe_End", &refs);
        ReadRefHeader("ref.grab_l", "1.5", &refs);
        ReadRefHeader("ref.jump", "0.4", &refs);
        ReadRefHeader("ref.spine", "2.0", &refs);  // a role with no step meaning
        const std::vector<std::string> names = {"mixamorig:Hips", "mixamorig:LeftFoot", "mixamorig:RightToe_End",
                                                "mixamorig:Spine"};
        std::vector<int> role_bone(static_cast<size_t>(Role::Count), -1);
        role_bone[static_cast<size_t>(Role::Hips)] = 0;
        role_bone[static_cast<size_t>(Role::LeftHeel)] = 1;
        role_bone[static_cast<size_t>(Role::RightToeEnd)] = 2;
        role_bone[static_cast<size_t>(Role::Spine)] = 3;
        std::vector<std::string> skipped;
        const std::vector<RefLine> lines = OldRefLines(refs, names, role_bone, &skipped);
        CHECK(lines.size() == 4);
        bool heel = false, tip = false, grab = false, jump = false;
        for (const RefLine& r : lines) {
            CHECK(r.status == "confirmed" && r.source == "REF");
            if (r.kind == "step" && r.side == 'L' && r.part == "heel" && Near(r.time_s, 1.2)) heel = true;
            if (r.kind == "step" && r.side == 'R' && r.part == "tip" && Near(r.time_s, 0.9)) tip = true;
            if (r.kind == "grab" && r.side == 'L' && r.part == "any" && Near(r.time_s, 1.5)) grab = true;
            if (r.kind == "jump" && r.side == 'B' && Near(r.time_s, 0.4)) jump = true;
        }
        CHECK(heel && tip && grab && jump);
        CHECK(skipped.size() == 1 && skipped[0].find("spine") == 0);
    }

    // ---- Row match.
    {
        // Jump R at 0.575, ref Jump R 0.56 -> matched, +15 ms; the L row expects nothing.
        std::vector<RefLine> lines(1);
        lines[0].kind = "jump", lines[0].side = 'R', lines[0].time_s = 0.56, lines[0].status = "confirmed";
        RowScore s = ScoreRow(RowRefs(lines, BenchConfig::Default, "jump", 'R'), {0.575}, 0.150, 0.0, 2.0);
        CHECK(s.exp_c == 1 && s.matched_c == 1 && s.missed_c == 0 && s.extra == 0);
        CHECK(s.err_s.size() == 1 && Near(s.err_s[0], 0.015, 1e-9));
        CHECK(RowRefs(lines, BenchConfig::Default, "jump", 'L').empty());

        // Deleted: kept in the file, never expected.
        lines[0].status = "deleted";
        s = ScoreRow(RowRefs(lines, BenchConfig::Default, "jump", 'R'), {}, 0.150, 0.0, 2.0);
        CHECK(s.exp_c == 0 && s.exp_d == 0 && s.missed_c == 0);

        // Outside # visible: neither the ref nor the detection counts.
        s = ScoreRow({{0.1, true}, {1.0, false}}, {0.12, 1.9}, 0.150, 0.5, 1.5);
        CHECK(s.exp_c == 0 && s.exp_d == 1 && s.found == 0 && s.missed_d == 1 && s.extra == 0);

        // A heel ref in default: not in the default step row, in the parts heel row.
        std::vector<RefLine> h(1);
        h[0].kind = "step", h[0].side = 'L', h[0].part = "heel", h[0].time_s = 1.2, h[0].status = "confirmed";
        CHECK(RowRefs(h, BenchConfig::Default, "step", 'L').empty());
        CHECK(RowRefs(h, BenchConfig::Parts, "heel", 'L').size() == 1);

        // One-to-one, smallest |err| first; confirmed / draft apart; beyond the window = missed + extra.
        s = ScoreRow({{1.00, true}, {1.10, false}, {3.0, true}}, {1.09, 1.20, 3.2}, 0.150, 0.0, 5.0);
        // 1.09 <-> 1.10 (0.01) first, then 1.20 <-> 1.00? 0.20 > window; 1.00 <-> 1.09 taken -> 1.00 missed.
        CHECK(s.exp_c == 2 && s.exp_d == 1 && s.found == 3);
        CHECK(s.matched_d == 1 && s.matched_c == 0 && s.missed_c == 2 && s.extra == 2);
        const ErrStats st = StatsOf({0.010, -0.020, 0.030});
        CHECK(st.n == 3 && Near(st.mean_ms, 20.0 / 3.0, 1e-6) && Near(st.med_abs_ms, 20.0, 1e-6));
        CHECK(RowText(ScoreRow({{1.0, true}}, {1.008}, 0.15, 0, 2)).find("exp 1/0  found 1  matched 1/0  missed 0/0  extra 0  mean +8.0") == 0);
    }

    if (g_fails) {
        std::printf("ref_file_test: %d FAILED\n", g_fails);
        return 1;
    }
    std::printf("ref_file_test: all passed\n");
    return 0;
}
