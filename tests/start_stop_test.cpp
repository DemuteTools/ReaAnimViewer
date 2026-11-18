// SPDX-License-Identifier: MIT
//
// Story 10-12e-3: Start & Stop on every motion that is no impact (the scuffs, kick, swing, drag,
// spin), the Peak twins of Drag and Spin, and the one "Marker at" component's helper
// (AutoMarkAtOptions / AutoMarkAtOf / SetAutoMarkAt): the rows, the settings and their blocks,
// the stop events at the motion's end (AutoEventsAtEnd), items saved before, a mode switch.

#include "auto_detect.h"
#include "auto_types.h"
#include "motion_physics.h"
#include "sound_auto_assign.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

using V = std::vector<std::string>;
using M = AutoMarkAt;

// The types this story gives Start & Stop (with the body moves of 10-12e and 10-12e-2).
const AutoType kNew[] = {AutoType::Slide, AutoType::Pivot, AutoType::HandPivot, AutoType::Kick,
                         AutoType::Swing, AutoType::Drag,  AutoType::Spin};

bool IsNew(AutoType t)
{
    return std::find(std::begin(kNew), std::end(kNew), t) != std::end(kNew);
}

bool IsMove(AutoType t)
{
    return t == AutoType::Crouch || t == AutoType::StandUp || t == AutoType::Bend || t == AutoType::Rise ||
           t == AutoType::Dash;
}

std::vector<std::string> Words(const std::vector<Block>& blocks)
{
    std::vector<std::string> w;
    for (const Block& b : blocks) w.push_back(b.auto_type + ":" + std::string(1, b.auto_side));
    return w;
}

AutoSettings One(AutoType t, M m)
{
    AutoSettings s;
    s.type[static_cast<int>(t)].on = true;
    SetAutoMarkAt(t, m, s);
    return s;
}

// ---- The Marker at component's helper ------------------------------------------------------------

void TestMarkAt()
{
    CHECK(std::string(AutoMarkAtLabel(M::Stop)) == "Stop" && std::string(AutoMarkAtLabel(M::Peak)) == "Peak" &&
          std::string(AutoMarkAtLabel(M::Start)) == "Start" && std::string(AutoMarkAtLabel(M::StartStop)) == "Start & Stop");
    for (int ti = 0; ti < kAutoTypeCount; ++ti) {
        const AutoType          t = static_cast<AutoType>(ti);
        const std::vector<M>    o = AutoMarkAtOptions(t);
        // Punch: Stop / Peak / Start (its own mode, unchanged); every motion: Peak / Start /
        // Start & Stop; the impacts and contacts: none.
        if (t == AutoType::Punch) CHECK(o == std::vector<M>({M::Stop, M::Peak, M::Start}));
        else if (IsNew(t) || IsMove(t)) CHECK(o == std::vector<M>({M::Peak, M::Start, M::StartStop}));
        else CHECK(o.empty());
        // The defaults: the scuffs, drag and the body moves at the Start, Spin at Start & Stop, kick
        // and swing at the Peak, Punch at the Stop.
        const AutoSettings d;
        if (!o.empty()) {
            const M want = t == AutoType::Spin                                          ? M::StartStop
                           : t == AutoType::Punch                                       ? M::Stop
                           : (t == AutoType::Kick || t == AutoType::Swing)              ? M::Peak
                                                                                        : M::Start;
            CHECK(AutoMarkAtOf(t, d) == want);
        }
        // Each choice reads back, and sets the flags the blocks follow.
        for (M m : o) {
            AutoSettings s;
            SetAutoMarkAt(t, m, s);
            CHECK(AutoMarkAtOf(t, s) == m);
            CHECK(s.at_start[ti] == (m == M::Start || m == M::StartStop));
            CHECK(s.with_stop[ti] == (m == M::StartStop));
            if (t == AutoType::Punch) CHECK(s.punch_peak == (m == M::Peak) || m == M::Start);
            else CHECK(!s.punch_peak);
        }
        // A choice the type does not offer changes nothing.
        AutoSettings s, before;
        for (M m : {M::Stop, M::Peak, M::Start, M::StartStop})
            if (std::find(o.begin(), o.end(), m) == o.end()) {
                SetAutoMarkAt(t, m, s);
                CHECK(s == before && s.punch_peak == before.punch_peak);
            }
    }
    // Punch's Start keeps its Stop / Peak choice for when it comes back.
    AutoSettings p;
    SetAutoMarkAt(AutoType::Punch, M::Peak, p);
    SetAutoMarkAt(AutoType::Punch, M::Start, p);
    CHECK(p.punch_peak && AutoMarkAtOf(AutoType::Punch, p) == M::Start);
}

// ---- Rows --------------------------------------------------------------------------------------

void TestRows()
{
    for (int ki = 0; ki < kAutoKindCount; ++ki) {
        const AutoKind      k = static_cast<AutoKind>(ki);
        const AutoKindInfo& r = AutoKindRow(k);
        if (!r.at_stop) {
            // A Start kind of a stop type has its stop kind; any other kind none.
            const bool has = r.at_start && AutoTypeRow(r.type).stop_option;
            CHECK((AutoStopKindOf(k) != k) == has);
            if (has) CHECK(AutoStartKindOfStop(AutoStopKindOf(k)) == k);
            continue;
        }
        // A stop kind: "<its Start kind's marker> Stop", its own sound key (Sound assign lists it
        // apart), a lighter colour, its Start kind's part and mode.
        const AutoKind      s = AutoStartKindOfStop(k);
        const AutoKindInfo& sr = AutoKindRow(s);
        CHECK(s != k && AutoStopKindOf(s) == k && sr.at_start && sr.type == r.type);
        CHECK(std::string(r.marker) == std::string(sr.marker) + " Stop");
        CHECK(AutoSoundKey(k, 'L') != AutoSoundKey(s, 'L') &&
              (AutoSoundKey(k, 'L').find("_stop:") != std::string::npos || k == AutoKind::SpinEnd));
        CHECK(std::string(r.role_key) == sr.role_key && r.scuff == sr.scuff && r.scuff_heel == sr.scuff_heel &&
              r.degrees == sr.degrees);
        for (char side : {'L', 'R'}) CHECK(AutoDefaultColor(k, side) != AutoDefaultColor(s, side));
    }
    // The names in the spec, and the words (saved data).
    CHECK(AutoMarkerName(AutoKind::SlideHeelStop, 'L') == "Slide Heel Stop L");
    CHECK(AutoMarkerName(AutoKind::KickToeStop, 'R') == "Kick Toe Stop R");
    CHECK(AutoMarkerName(AutoKind::HandPivotAnyStop, 'L') == "Hand Pivot Stop L");
    CHECK(AutoMarkerName(AutoKind::SwingStop, 'R') == "Swing Stop R" && AutoMarkerName(AutoKind::DragStop, 'L') == "Drag Stop L");
    CHECK(AutoMarkerName(AutoKind::SpinEnd, 'B') == "Spin Stop" && std::string(AutoKindWord(AutoKind::SpinEnd)) == "spin_end");
    CHECK(AutoMarkerName(AutoKind::SpinPeak, 'B') == "Spin" && AutoMarkerName(AutoKind::DragPeak, 'R') == "Drag R");
    CHECK(std::string(AutoKindWord(AutoKind::SlideToeLongestStop)) == "slide_toe_longest_stop" &&
          std::string(AutoKindWord(AutoKind::KickHeelStop)) == "kick_heel_stop" &&
          std::string(AutoKindWord(AutoKind::DragPeak)) == "drag_peak" && std::string(AutoKindWord(AutoKind::SpinPeak)) == "spin_peak");
    // Two stop kinds of one marker share a sound (as their Start kinds do).
    CHECK(AutoSoundKey(AutoKind::HandPivotAnyStop, 'L') == AutoSoundKey(AutoKind::HandPivotStop, 'L'));
    CHECK(AutoSoundKey(AutoKind::SlideHeelLongestStop, 'R') == "slide_heel_stop:R");
    CHECK(AutoSoundKey(AutoKind::SpinEnd, 'B') == "spin_end:B" && AutoSoundKey(AutoKind::SpinPeak, 'B') == "spin:B");
    CHECK(AutoSoundKey(AutoKind::DragPeak, 'L') == "drag:L" && AutoSoundKey(AutoKind::DragStop, 'L') == "drag_stop:L");
    // Two kinds of one type and side either share a colour (one marker in two modes or flavours)
    // or are visibly apart (the sum of their channel differences 40 or more): a stop beside its
    // Start, a heel stop beside the toe start, on the strip together.
    for (int ki = 0; ki < kAutoKindCount; ++ki)
        for (int kj = ki + 1; kj < kAutoKindCount; ++kj) {
            const AutoKind a = static_cast<AutoKind>(ki), b = static_cast<AutoKind>(kj);
            if (AutoTypeOfKind(a) != AutoTypeOfKind(b)) continue;
            for (char side : {'L', 'R'}) {
                const uint32_t ca = AutoDefaultColor(a, side), cb = AutoDefaultColor(b, side);
                if (ca == cb) continue;
                int d = 0;
                for (int sh = 0; sh <= 16; sh += 8)
                    d += std::abs(static_cast<int>((ca >> sh) & 0xFFu) - static_cast<int>((cb >> sh) & 0xFFu));
                if (d < 40) std::printf("  close colours: %s %s %c (%d)\n", AutoKindWord(a), AutoKindWord(b), side, d);
                CHECK(d >= 40);
            }
        }
    // No impact or contact has a stop kind.
    for (AutoType t : {AutoType::Step, AutoType::LiftOff, AutoType::Jump, AutoType::Land, AutoType::Tap, AutoType::HeelTap,
                       AutoType::Knee, AutoType::Elbow, AutoType::Sit, AutoType::Fall, AutoType::Grab, AutoType::Release,
                       AutoType::Clap, AutoType::HandOnBody, AutoType::Slap, AutoType::Jolt, AutoType::ArmJump,
                       AutoType::ArmLand, AutoType::Punch})
        for (AutoKind k : AutoKindsOf(t)) CHECK(!AutoKindRow(k).at_stop);
}

// ---- The stop events: at the motion's end --------------------------------------------------------

PhysicsEvent Ev(double start, double time, double end)
{
    PhysicsEvent e;
    e.kind = PhysicsKind::Slide;
    e.start_s = start;
    e.time_s = time;
    e.end_s = end;
    e.strength = 3.0;
    return e;
}

void TestAtEnd()
{
    PhysicsAnalysis a;
    a.ok = true;
    a.rate_hz = 100.0;
    a.samples = 201;  // a 2 s clip
    // The end; a motion the clip's end cuts at its last sample; sorted by the new time.
    std::vector<PhysicsEvent> ev = AutoEventsAtEnd({Ev(0.5, 0.5, 1.5), Ev(0.2, 0.3, 0.9), Ev(1.8, 1.8, 2.01)}, a);
    CHECK(ev.size() == 3);
    if (ev.size() == 3) {
        CHECK(ev[0].time_s == 0.9 && ev[0].start_s == 0.2 && ev[0].strength == 3.0);
        CHECK(ev[1].time_s == 1.5 && ev[1].end_s == 1.5);
        CHECK(std::fabs(ev[2].time_s - 2.0) < 1e-12 && ev[2].start_s == 1.8);
    }
    // A loop: an end past the seam wraps the whole event one cycle back.
    PhysicsAnalysis l = a;
    l.looping = true;
    l.samples = 301;
    l.window_first = 100;
    l.window_samples = 101;  // T = 1 s
    ev = AutoEventsAtEnd({Ev(0.8, 0.8, 1.3), Ev(0.1, 0.1, 0.4)}, l);
    CHECK(ev.size() == 2);
    if (ev.size() == 2) {
        CHECK(std::fabs(ev[0].time_s - 0.3) < 1e-9 && std::fabs(ev[0].start_s + 0.2) < 1e-9 &&
              std::fabs(ev[0].end_s - 0.3) < 1e-9);
        CHECK(std::fabs(ev[1].time_s - 0.4) < 1e-9 && ev[1].start_s == 0.1);
    }
    // Every stop kind but Spin's reads its Start kind's source re-timed (Spin's own spin end).
    for (int ki = 0; ki < kAutoKindCount; ++ki) {
        const AutoKindInfo& r = AutoKindRow(static_cast<AutoKind>(ki));
        if (!r.at_stop) continue;
        const AutoKindInfo& s = AutoKindRow(AutoStartKindOfStop(r.kind));
        CHECK(AutoSignatureCount(r) == 1);
        if (r.kind != AutoKind::SpinEnd && !IsMove(r.type))
            CHECK(r.sigs[0].source != s.sigs[0].source && r.sigs[0].match == s.sigs[0].match);
    }
}

// ---- Settings and blocks -----------------------------------------------------------------------

void TestBlocks()
{
    // Default settings, every type ticked: no stop block but Spin's spin_end (its two markers, as
    // before), no new kind.
    AutoSettings all;
    for (int t = 0; t < kAutoTypeCount; ++t) all.type[t].on = true;
    for (const Block& b : AutoBlocks(all)) {
        AutoKind k;
        CHECK(AutoBlockKind(b, &k, nullptr));
        CHECK(static_cast<int>(k) < static_cast<int>(AutoKind::SlideStop));
        if (AutoKindRow(k).at_stop) CHECK(k == AutoKind::SpinEnd);
    }
    {
        AutoSettings s;
        s.type[static_cast<int>(AutoType::Spin)].on = true;
        CHECK(Words(AutoBlocks(s)) == V({"spin:B", "spin_end:B"}));
        CHECK(Words(AutoBlocks(One(AutoType::Spin, M::Start))) == V({"spin:B"}));
        CHECK(Words(AutoBlocks(One(AutoType::Spin, M::Peak))) == V({"spin_peak:B"}));
    }
    // Start & Stop: the Start blocks, then each one's stop.
    CHECK(Words(AutoBlocks(One(AutoType::Slide, M::StartStop))) == V({"slide:L", "slide:R", "slide_stop:L", "slide_stop:R"}));
    {
        AutoSettings s = One(AutoType::Slide, M::StartStop);
        s.slide_parts = ScuffParts::EachPart;
        CHECK(Words(AutoBlocks(s)) == V({"slide_heel:L", "slide_heel:R", "slide_toe:L", "slide_toe:R", "slide_heel_stop:L",
                                         "slide_heel_stop:R", "slide_toe_stop:L", "slide_toe_stop:R"}));
        s.slide_parts = ScuffParts::Longest;
        CHECK(Words(AutoBlocks(s)) == V({"slide_heel_longest:L", "slide_heel_longest:R", "slide_toe_longest:L",
                                         "slide_toe_longest:R", "slide_heel_longest_stop:L", "slide_heel_longest_stop:R",
                                         "slide_toe_longest_stop:L", "slide_toe_longest_stop:R"}));
    }
    {
        AutoSettings s = One(AutoType::Pivot, M::StartStop);
        s.pivot_parts = ScuffParts::EachPart;
        CHECK(Words(AutoBlocks(s)) == V({"pivot_heel:L", "pivot_heel:R", "pivot_toe:L", "pivot_toe:R", "pivot_heel_stop:L",
                                         "pivot_heel_stop:R", "pivot_toe_stop:L", "pivot_toe_stop:R"}));
    }
    {
        AutoSettings s = One(AutoType::HandPivot, M::StartStop);
        CHECK(Words(AutoBlocks(s)) == V({"hand_pivot:L", "hand_pivot:R", "hand_pivot_stop:L", "hand_pivot_stop:R"}));
        s.hand_pivot_any = true;
        CHECK(Words(AutoBlocks(s)) == V({"hand_pivot_any:L", "hand_pivot_any:R", "hand_pivot_any_stop:L", "hand_pivot_any_stop:R"}));
    }
    {
        AutoSettings s = One(AutoType::Kick, M::StartStop);
        CHECK(Words(AutoBlocks(s)) == V({"kick_start:L", "kick_start:R", "kick_stop:L", "kick_stop:R"}));
        s.separate_parts[static_cast<int>(AutoType::Kick)] = true;
        CHECK(Words(AutoBlocks(s)) == V({"kick_heel_start:L", "kick_heel_start:R", "kick_toe_start:L", "kick_toe_start:R",
                                         "kick_heel_stop:L", "kick_heel_stop:R", "kick_toe_stop:L", "kick_toe_stop:R"}));
        // Separate at the Peak: no stop kind among the parts.
        CHECK(Words(AutoBlocks(One(AutoType::Kick, M::Peak))) == V({"kick:L", "kick:R"}));
    }
    CHECK(Words(AutoBlocks(One(AutoType::Swing, M::StartStop))) == V({"swing_start:L", "swing_start:R", "swing_stop:L", "swing_stop:R"}));
    CHECK(Words(AutoBlocks(One(AutoType::Drag, M::StartStop))) == V({"drag:L", "drag:R", "drag_stop:L", "drag_stop:R"}));
    CHECK(Words(AutoBlocks(One(AutoType::Drag, M::Peak))) == V({"drag_peak:L", "drag_peak:R"}));
    {
        AutoSettings s;
        s.type[static_cast<int>(AutoType::Drag)].on = true;
        CHECK(Words(AutoBlocks(s)) == V({"drag:L", "drag:R"}));  // Start, its default
    }
    // Round trip: the blocks say the mode, every motion type and part mode.
    for (int ti = 0; ti < kAutoTypeCount; ++ti) {
        const AutoType t = static_cast<AutoType>(ti);
        for (M m : AutoMarkAtOptions(t))
            for (int parts = 0; parts < 3; ++parts) {
                AutoSettings s = One(t, m);
                s.slide_parts = s.pivot_parts = static_cast<ScuffParts>(parts);
                s.hand_pivot_any = s.separate_parts[static_cast<int>(AutoType::Kick)] = parts == 1;
                const AutoSettings r = ReadAutoSettings(AutoBlocks(s));
                CHECK(AutoMarkAtOf(t, r) == m && !AutoPending(r, s));
                CHECK(Words(AutoBlocks(r)) == Words(AutoBlocks(s)));
            }
    }
    // Start <-> Start & Stop on a ticked type waits for Detect.
    CHECK(AutoPending(One(AutoType::Swing, M::Start), One(AutoType::Swing, M::StartStop)));
    CHECK(AutoPending(One(AutoType::Spin, M::Start), One(AutoType::Spin, M::StartStop)));
}

// ---- Items saved before ----------------------------------------------------------------------------

void TestOldItems()
{
    // Blocks saved before this story: kick and swing at the Peak, the scuffs and drag at the
    // Start, Spin with its "Spin End" block. They read in their saved mode, and Detect keeps them.
    std::vector<Block> blocks;
    for (char side : {'L', 'R'}) {
        blocks.push_back(MakeAutoBlock(AutoKind::Kick, side, 50.0, 0.0));
        blocks.push_back(MakeAutoBlock(AutoKind::Swing, side, 50.0, 0.0));
        blocks.push_back(MakeAutoBlock(AutoKind::SlideHeel, side, 50.0, 0.0));
        blocks.push_back(MakeAutoBlock(AutoKind::SlideToe, side, 50.0, 0.0));
        blocks.push_back(MakeAutoBlock(AutoKind::HandPivotAny, side, 50.0, 0.0));
        blocks.push_back(MakeAutoBlock(AutoKind::Drag, side, 50.0, 0.0));
    }
    blocks.push_back(MakeAutoBlock(AutoKind::Spin, 'B', 50.0, 0.0));
    Block end = MakeAutoBlock(AutoKind::SpinEnd, 'B', 50.0, 0.0);
    end.marker = "Spin End";  // its saved name
    blocks.push_back(end);
    const std::vector<Block> saved = blocks;
    const AutoSettings       r = ReadAutoSettings(blocks);
    CHECK(AutoMarkAtOf(AutoType::Kick, r) == M::Peak && AutoMarkAtOf(AutoType::Swing, r) == M::Peak);
    CHECK(AutoMarkAtOf(AutoType::Slide, r) == M::Start && r.slide_parts == ScuffParts::EachPart);
    CHECK(AutoMarkAtOf(AutoType::HandPivot, r) == M::Start && r.hand_pivot_any);
    CHECK(AutoMarkAtOf(AutoType::Drag, r) == M::Start && AutoMarkAtOf(AutoType::Spin, r) == M::StartStop);
    bool changed = true;
    const std::vector<int> map = ApplyAutoSettings(blocks, r, &changed);
    CHECK(!changed && blocks.size() == saved.size() && blocks.back().marker == "Spin End");
    for (size_t i = 0; i < map.size(); ++i) CHECK(map[i] == static_cast<int>(i));
    // A Spin saved with its start only reads Start (no stop).
    const AutoSettings only = ReadAutoSettings({MakeAutoBlock(AutoKind::Spin, 'B', 50.0, 0.0)});
    CHECK(AutoMarkAtOf(AutoType::Spin, only) == M::Start && Words(AutoBlocks(only)) == V({"spin:B"}));
    // An item with no Spin block: Spin's default (Start & Stop) when it is ticked.
    AutoSettings none = ReadAutoSettings({});
    none.type[static_cast<int>(AutoType::Spin)].on = true;
    CHECK(Words(AutoBlocks(none)) == V({"spin:B", "spin_end:B"}));
}

// ---- A mode switch ---------------------------------------------------------------------------------

void TestSwitch()
{
    // Start & Stop -> Peak: the stop blocks go, the Start block becomes the Peak one with its edits.
    std::vector<Block> blocks = AutoBlocks(One(AutoType::Slide, M::StartStop));
    blocks[0].marker = "My slide";
    blocks[0].rid = "r1";
    blocks[2].rid = "r2";
    bool             changed = false;
    std::vector<int> map = ApplyAutoSettings(blocks, One(AutoType::Slide, M::Peak), &changed);
    CHECK(changed && Words(blocks) == V({"slide_peak:L", "slide_peak:R"}) && blocks[0].marker == "My slide");
    CHECK(map.size() == 4 && map[0] == 0 && map[1] == 1 && map[2] == -1 && map[3] == -1);
    // Peak -> Start & Stop: the block kept, the stops added.
    map = ApplyAutoSettings(blocks, One(AutoType::Slide, M::StartStop), &changed);
    CHECK(changed && Words(blocks) == V({"slide:L", "slide:R", "slide_stop:L", "slide_stop:R"}) && blocks[0].marker == "My slide");
    // Combined -> Each part, at Start & Stop: the stop block's edits go to the heel's stop block,
    // and its sound (rule id) to a stop block, never to a start one.
    blocks = AutoBlocks(One(AutoType::Slide, M::StartStop));
    blocks[0].rid = "start";
    blocks[2].rid = "stop";
    AutoSettings each = One(AutoType::Slide, M::StartStop);
    each.slide_parts = ScuffParts::EachPart;
    map = ApplyAutoSettings(blocks, each, &changed);
    CHECK(Words(blocks) == Words(AutoBlocks(each)));
    CHECK(map.size() == 4 && map[0] >= 0 && blocks[static_cast<size_t>(map[0])].auto_type == "slide_heel");
    CHECK(map[2] >= 0 && blocks[static_cast<size_t>(map[2])].auto_type == "slide_heel_stop");
    for (const Block& b : blocks) {
        if (b.rid == "start") CHECK(b.auto_type.find("_stop") == std::string::npos);
        if (b.rid == "stop") CHECK(b.auto_type.find("_stop") != std::string::npos);
    }
    // Each part <-> Longest part keeps the stop blocks (one sound each).
    blocks[4].marker = "My heel stop";
    each.slide_parts = ScuffParts::Longest;
    ApplyAutoSettings(blocks, each, &changed);
    CHECK(Words(blocks) == Words(AutoBlocks(each)) && blocks[4].marker == "My heel stop");
    // Kick Combined -> Separate at Start & Stop: the kick stop's edits go to the heel stop.
    blocks = AutoBlocks(One(AutoType::Kick, M::StartStop));
    AutoSettings sep = One(AutoType::Kick, M::StartStop);
    sep.separate_parts[static_cast<int>(AutoType::Kick)] = true;
    map = ApplyAutoSettings(blocks, sep, &changed);
    CHECK(Words(blocks) == Words(AutoBlocks(sep)));
    CHECK(map.size() == 4 && map[2] >= 0 && blocks[static_cast<size_t>(map[2])].auto_type == "kick_heel_stop");
    // The reverse switches at Start & Stop: Each part -> Combined (the heel stop's edits to the
    // slide stop), Separate -> Combined (the kick heel stop's to the kick stop).
    {
        AutoSettings ep = One(AutoType::Slide, M::StartStop);
        ep.slide_parts = ScuffParts::EachPart;
        std::vector<Block> eb = AutoBlocks(ep);
        // slide_heel L, R, slide_toe L, R, slide_heel_stop L, R, slide_toe_stop L, R
        const AutoSettings comb = One(AutoType::Slide, M::StartStop);
        map = ApplyAutoSettings(eb, comb, &changed);
        CHECK(changed && Words(eb) == Words(AutoBlocks(comb)) && map.size() == 8);
        if (map.size() == 8) {
            CHECK(map[0] >= 0 && eb[static_cast<size_t>(map[0])].auto_type == "slide" && eb[static_cast<size_t>(map[0])].auto_side == 'L');
            CHECK(map[4] >= 0 && eb[static_cast<size_t>(map[4])].auto_type == "slide_stop" &&
                  eb[static_cast<size_t>(map[4])].auto_side == 'L');
            CHECK(map[5] >= 0 && eb[static_cast<size_t>(map[5])].auto_type == "slide_stop" &&
                  eb[static_cast<size_t>(map[5])].auto_side == 'R');
        }
        std::vector<Block> kb = AutoBlocks(sep);
        // kick_heel_start L, R, kick_toe_start L, R, kick_heel_stop L, R, kick_toe_stop L, R
        const AutoSettings kc = One(AutoType::Kick, M::StartStop);
        map = ApplyAutoSettings(kb, kc, &changed);
        CHECK(changed && Words(kb) == Words(AutoBlocks(kc)) && map.size() == 8);
        if (map.size() == 8) {
            CHECK(map[0] >= 0 && kb[static_cast<size_t>(map[0])].auto_type == "kick_start");
            CHECK(map[4] >= 0 && kb[static_cast<size_t>(map[4])].auto_type == "kick_stop" &&
                  kb[static_cast<size_t>(map[4])].auto_side == 'L');
            CHECK(map[5] >= 0 && kb[static_cast<size_t>(map[5])].auto_type == "kick_stop" &&
                  kb[static_cast<size_t>(map[5])].auto_side == 'R');
        }
    }
    // Spin Start & Stop -> Start: the stop goes, the start stays.
    blocks = AutoBlocks(One(AutoType::Spin, M::StartStop));
    map = ApplyAutoSettings(blocks, One(AutoType::Spin, M::Start), &changed);
    CHECK(changed && Words(blocks) == V({"spin:B"}) && map[0] == 0 && map[1] == -1);
}

}  // namespace

int main()
{
    TestMarkAt();
    TestRows();
    TestAtEnd();
    TestBlocks();
    TestOldItems();
    TestSwitch();
    if (g_fails) {
        std::printf("start_stop: %d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("start_stop: all checks passed\n");
    return 0;
}
