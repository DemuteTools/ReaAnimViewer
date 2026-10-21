// SPDX-License-Identifier: MIT
//
// Story 10-9j: Start / Peak on the scuffs (Legs > Slide scuff, Legs > Pivot scuff, Arms > Pivot
// scuff): the peak timing (src/scuff_peak.h) and the settings (src/auto_detect.h): Start by
// default with the kinds of before, Peak with their twins, a switch keeping each block.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "auto_detect.h"
#include "auto_types.h"
#include "scuff_peak.h"

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

// A 100 Hz analysis of `n` samples whose parts are all present and down, at rest.
PhysicsAnalysis Still(size_t n)
{
    PhysicsAnalysis a;
    a.ok = true;
    a.rate_hz = 100.0;
    a.samples = n;
    a.leg_length = 1.0;
    auto part = [&](PartTrack& pt) {
        pt.present = true;
        pt.hspeed.assign(n, 0.01);
        pt.contact.assign(n, 1);
    };
    for (FootTrack& f : a.foot)
        for (PartTrack& pt : f.part) part(pt);
    for (HandTrack& h : a.hand) part(h.part);
    a.foot[1].side = 'R';
    a.hand[1].side = 'R';
    return a;
}

PhysicsEvent Scuff(PhysicsKind k, char side, int part, double start, double end)
{
    PhysicsEvent e;
    e.kind = k;
    e.side = side;
    e.part = part;
    e.time_s = e.start_s = start;
    e.end_s = end;
    e.strength = 40.0;
    return e;
}

std::vector<std::string> Words(const std::vector<Block>& blocks)
{
    std::vector<std::string> out;
    for (const Block& b : blocks) out.push_back(b.auto_type + ":" + std::string(1, b.auto_side));
    return out;
}

constexpr int kBall = static_cast<int>(FootPart::Ball), kHeel = static_cast<int>(FootPart::Heel);

void TestPeakTiming()
{
    PhysicsAnalysis a = Still(200);
    // The left ball rubs fastest at sample 60, the heel at 80; the hand at 130.
    a.foot[0].part[kBall].hspeed[60] = 0.2;
    a.foot[0].part[kHeel].hspeed[80] = 0.3;
    a.hand[0].part.hspeed[130] = 0.5;
    // A pivot on the ball [0.4, 1.0): its peak at 0.6; span and strength kept.
    const PhysicsEvent pv = ScuffPeakEvent(a, Scuff(PhysicsKind::Pivot, 'L', kBall, 0.4, 1.0));
    CHECK(std::fabs(pv.time_s - 0.6) < 1e-9 && pv.start_s == 0.4 && pv.end_s == 1.0 && pv.strength == 40.0);
    // The same on the heel: the heel's peak.
    CHECK(std::fabs(ScuffPeakEvent(a, Scuff(PhysicsKind::Pivot, 'L', kHeel, 0.4, 1.0)).time_s - 0.8) < 1e-9);
    // A whole-foot pivot (part -1): the forefoot (the ball).
    CHECK(std::fabs(ScuffPeakEvent(a, Scuff(PhysicsKind::Slide, 'L', -1, 0.4, 1.0)).time_s - 0.6) < 1e-9);
    // Longest part: the part that holds longest, when asked.
    PhysicsEvent lg = Scuff(PhysicsKind::Pivot, 'L', -1, 0.4, 1.0);
    lg.longest = kHeel;
    CHECK(std::fabs(ScuffPeakEvent(a, lg, true).time_s - 0.8) < 1e-9);
    CHECK(std::fabs(ScuffPeakEvent(a, lg, false).time_s - 0.6) < 1e-9);
    // Only inside the span: a span missing the peak keeps its own fastest sample (the first on a tie).
    CHECK(std::fabs(ScuffPeakEvent(a, Scuff(PhysicsKind::Pivot, 'L', kBall, 0.7, 1.0)).time_s - 0.7) < 1e-9);
    // The part lifted at its peak: the fastest sample while down.
    a.foot[0].part[kBall].contact[60] = 0;
    a.foot[0].part[kBall].hspeed[55] = 0.1;
    CHECK(std::fabs(ScuffPeakEvent(a, Scuff(PhysicsKind::Pivot, 'L', kBall, 0.4, 1.0)).time_s - 0.55) < 1e-9);
    // A hand pivot: the hand's.
    CHECK(std::fabs(ScuffPeakEvent(a, Scuff(PhysicsKind::HandPivot, 'L', -1, 1.0, 1.5)).time_s - 1.3) < 1e-9);
    // The other side reads its own foot (at rest: the first sample).
    CHECK(std::fabs(ScuffPeakEvent(a, Scuff(PhysicsKind::Pivot, 'R', kBall, 0.4, 1.0)).time_s - 0.4) < 1e-9);
    // Any other event is unchanged.
    PhysicsEvent st = Scuff(PhysicsKind::Step, 'L', kBall, 0.4, 0.4);
    CHECK(ScuffPeakEvent(a, st).time_s == 0.4);
    // A loop: clip time = analysed time less the window's first sample.
    PhysicsAnalysis l = Still(200);
    l.looping = true;
    l.window_first = 50;
    l.window_samples = 100;
    l.foot[0].part[kBall].hspeed[110] = 0.2;  // clip 0.6
    CHECK(std::fabs(ScuffPeakEvent(l, Scuff(PhysicsKind::Pivot, 'L', kBall, 0.4, 1.0)).time_s - 0.6) < 1e-9);
    // The list is sorted again by time.
    const std::vector<PhysicsEvent> ev =
        ScuffPeakEvents(a, {Scuff(PhysicsKind::Pivot, 'R', kBall, 0.5, 0.9), Scuff(PhysicsKind::Pivot, 'L', kHeel, 0.2, 1.0)});
    CHECK(ev.size() == 2 && ev[0].side == 'R' && ev[1].side == 'L' && std::fabs(ev[1].time_s - 0.8) < 1e-9);
}

AutoSettings Scuffs(bool peak, ScuffParts parts, bool any_hand)
{
    AutoSettings s;
    for (AutoType t : {AutoType::Slide, AutoType::Pivot, AutoType::HandPivot}) {
        s.type[static_cast<int>(t)].on = true;
        if (peak) s.at_start[static_cast<int>(t)] = false;
    }
    s.slide_parts = s.pivot_parts = parts;
    s.hand_pivot_any = any_hand;
    return s;
}

void TestSettings()
{
    // Rows: Start by default for the scuffs (their kinds of before), Peak for the whooshes.
    for (AutoType t : {AutoType::Slide, AutoType::Pivot, AutoType::HandPivot}) {
        CHECK(AutoTypeRow(t).start_option && AutoStartDefault(t) && AutoSettings().at_start[static_cast<int>(t)]);
        CHECK(!AutoTypeRow(t).no_sens_raise);
    }
    CHECK(!AutoStartDefault(AutoType::Kick) && !AutoStartDefault(AutoType::Step));
    // Each Peak twin: its Start kind's marker, colour, role key, sound key and part mode.
    const std::pair<AutoKind, AutoKind> twins[] = {
        {AutoKind::Slide, AutoKind::SlidePeak},
        {AutoKind::SlideHeel, AutoKind::SlideHeelPeak},
        {AutoKind::SlideToe, AutoKind::SlideToePeak},
        {AutoKind::SlideHeelLongest, AutoKind::SlideHeelLongestPeak},
        {AutoKind::SlideToeLongest, AutoKind::SlideToeLongestPeak},
        {AutoKind::Pivot, AutoKind::PivotPeak},
        {AutoKind::PivotHeel, AutoKind::PivotHeelPeak},
        {AutoKind::PivotToe, AutoKind::PivotToePeak},
        {AutoKind::PivotHeelLongest, AutoKind::PivotHeelLongestPeak},
        {AutoKind::PivotToeLongest, AutoKind::PivotToeLongestPeak},
        {AutoKind::HandPivot, AutoKind::HandPivotPeak},
        {AutoKind::HandPivotAny, AutoKind::HandPivotAnyPeak},
    };
    for (const auto& tw : twins) {
        const AutoKindInfo &s = AutoKindRow(tw.first), &p = AutoKindRow(tw.second);
        CHECK(s.at_start && !p.at_start && s.type == p.type && std::string(s.marker) == p.marker);
        CHECK(std::string(s.role_key) == p.role_key && s.sound_key == p.sound_key && s.scuff == p.scuff &&
              s.scuff_heel == p.scuff_heel && s.degrees == p.degrees);
        CHECK(std::string(p.word) == std::string(s.word) + "_peak");
        for (char side : {'L', 'R'}) {
            CHECK(AutoMarkerName(tw.first, side) == AutoMarkerName(tw.second, side));
            CHECK(AutoDefaultColor(tw.first, side) == AutoDefaultColor(tw.second, side));
        }
        CHECK(s.sigs[0].source != p.sigs[0].source && s.sigs[0].match == p.sigs[0].match);
    }

    // Start (default): the blocks of before, word for word.
    CHECK((Words(AutoBlocks(Scuffs(false, ScuffParts::Combined, false))) ==
           std::vector<std::string>{"slide:L", "slide:R", "pivot:L", "pivot:R", "hand_pivot:L", "hand_pivot:R"}));
    CHECK((Words(AutoBlocks(Scuffs(true, ScuffParts::Combined, false))) ==
           std::vector<std::string>{"slide_peak:L", "slide_peak:R", "pivot_peak:L", "pivot_peak:R", "hand_pivot_peak:L",
                                    "hand_pivot_peak:R"}));
    CHECK((Words(AutoBlocks(Scuffs(true, ScuffParts::Longest, true))) ==
           std::vector<std::string>{"slide_heel_longest_peak:L", "slide_heel_longest_peak:R", "slide_toe_longest_peak:L",
                                    "slide_toe_longest_peak:R", "pivot_heel_longest_peak:L", "pivot_heel_longest_peak:R",
                                    "pivot_toe_longest_peak:L", "pivot_toe_longest_peak:R", "hand_pivot_any_peak:L",
                                    "hand_pivot_any_peak:R"}));
    // Read back in every mode; a switch is pending.
    for (bool peak : {false, true})
        for (ScuffParts parts : {ScuffParts::Combined, ScuffParts::EachPart, ScuffParts::Longest})
            for (bool any : {false, true}) {
                const AutoSettings s = Scuffs(peak, parts, any);
                CHECK(ReadAutoSettings(AutoBlocks(s)) == s);
            }
    CHECK(AutoPending(Scuffs(true, ScuffParts::Combined, false), Scuffs(false, ScuffParts::Combined, false)));

    // A project saved before Peak (slide, pivot, hand pivot blocks; steps) reads back Start, and
    // re-applying what it reads changes nothing.
    {
        AutoSettings old = Scuffs(false, ScuffParts::EachPart, true);
        old.type[static_cast<int>(AutoType::Step)].on = true;
        std::vector<Block>       saved = AutoBlocks(old);
        const std::vector<Block> before = saved;
        const AutoSettings       read = ReadAutoSettings(saved);
        CHECK(read == old && read.at_start[static_cast<int>(AutoType::Pivot)]);
        bool ch = true;
        ApplyAutoSettings(saved, read, &ch);
        CHECK(!ch && Words(saved) == Words(before) && !AutoPending(read, read));
        // With no scuff at all, the type keeps its default (Start) for when it is ticked.
        CHECK(ReadAutoSettings({}).at_start[static_cast<int>(AutoType::Slide)]);
    }

    // Start -> Peak keeps each block (its marker, colour, rule id, sensitivity, edits): only the
    // kind changes; and back.
    std::vector<Block> blocks = AutoBlocks(Scuffs(false, ScuffParts::Combined, false));
    blocks[2].marker = "My Pivot";
    blocks[2].rid = "0123456789abcdef";
    blocks[2].sens = 70.0;
    blocks[5].offset_ms = 12.0;
    bool                   changed = false;
    const std::vector<int> map = ApplyAutoSettings(blocks, Scuffs(true, ScuffParts::Combined, false), &changed);
    CHECK(changed && blocks.size() == 6);
    for (size_t i = 0; i < map.size(); ++i) CHECK(map[i] == static_cast<int>(i));
    CHECK(blocks[2].auto_type == "pivot_peak" && blocks[2].marker == "My Pivot" && blocks[2].rid == "0123456789abcdef" &&
          blocks[2].sens == 70.0 && blocks[5].auto_type == "hand_pivot_peak" && blocks[5].offset_ms == 12.0);
    // Peak, hand pivot after a grab -> any resting hand: the block is kept.
    ApplyAutoSettings(blocks, Scuffs(true, ScuffParts::Combined, true), &changed);
    CHECK(changed && blocks[5].auto_type == "hand_pivot_any_peak" && blocks[5].offset_ms == 12.0);
    // Peak, Combined -> Each part: the pivot's edits go to the heel block of that foot, which
    // takes its rule id.
    const std::vector<int> map2 = ApplyAutoSettings(blocks, Scuffs(true, ScuffParts::EachPart, true), &changed);
    CHECK(changed && map2.size() == 6 && map2[2] >= 0);
    if (map2[2] >= 0) {
        const Block& b = blocks[static_cast<size_t>(map2[2])];
        CHECK(b.auto_type == "pivot_heel_peak" && b.auto_side == 'L' && b.rid == "0123456789abcdef");
    }
    // Each part -> Longest part and Peak -> Start at once: each part block kept.
    const std::vector<int> map3 = ApplyAutoSettings(blocks, Scuffs(false, ScuffParts::Longest, true), &changed);
    for (int m : map3) CHECK(m >= 0);
    CHECK((Words(blocks) ==
           std::vector<std::string>{"hand_pivot_any:L", "hand_pivot_any:R", "slide_heel_longest:L", "slide_heel_longest:R",
                                    "slide_toe_longest:L", "slide_toe_longest:R", "pivot_heel_longest:L",
                                    "pivot_heel_longest:R", "pivot_toe_longest:L", "pivot_toe_longest:R"}));
    // Longest part, Start -> Combined, Peak: the part blocks' edits go to the combined block.
    const std::vector<int> map4 = ApplyAutoSettings(blocks, Scuffs(true, ScuffParts::Combined, true), &changed);
    CHECK(Words(blocks).size() == 6);
    for (size_t i = 0; i < map4.size(); ++i)
        if (map4[i] >= 0) {
            const std::string w = blocks[static_cast<size_t>(map4[i])].auto_type;
            CHECK(w == "slide_peak" || w == "pivot_peak" || w == "hand_pivot_any_peak");
        }
}

}  // namespace

int main()
{
    TestPeakTiming();
    TestSettings();
    if (g_fails) {
        std::printf("scuff_peak_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("scuff_peak_test: all passed\n");
    return 0;
}
