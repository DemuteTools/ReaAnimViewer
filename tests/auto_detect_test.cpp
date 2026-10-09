// SPDX-License-Identifier: MIT
//
// Host test of auto detection (src/auto_detect.h, Epic 10, story 10-8b): the spec's matrix rows
// that are pure (blocks, settings, Detect's remap, sensitivity, offset, the toe without a ball
// bone, missing roles, rules next to auto blocks, pooled copies, the record). No REAPER.
//   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests

#include "auto_detect.h"
#include "bone_roles.h"
#include "event_list.h"
#include "motion_physics.h"
#include "rule_record.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
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

constexpr double kPi = 3.14159265358979323846;

// A synthetic rig, as tests/motion_physics_test.cpp builds it: one foot = its ball, heading and
// pitch; the heel 14 cm behind and 6 cm above the ball, the tip 7 cm ahead and 2 cm below; the
// knee 42 cm and the up leg 87 cm above the heel; the hips between the up legs.
struct FootPose {
    Vec3d  ball;
    double yaw = 0.0;
};

Vec3d Add(const Vec3d& a, const Vec3d& b)
{
    return Vec3d{a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3d Yawed(double yaw, double x, double y, double z)
{
    return Vec3d{x * std::cos(yaw) + z * std::sin(yaw), y, -x * std::sin(yaw) + z * std::cos(yaw)};
}

using FootFn = std::function<FootPose(double)>;

struct Options {
    double dur = 4.0;
    bool   ball = true;  // false: no toe base bone (the toe end alone)
    bool   heel = true;  // false: no heel bone
    double rate = 240.0;
};

std::vector<BoneTrack> Rig(const FootFn& left, const FootFn& right, const Options& o)
{
    std::vector<BoneTrack> t(static_cast<size_t>(Role::Count));
    const int              n = static_cast<int>(std::floor(o.dur * o.rate + 1e-9)) + 1;
    auto put = [&](Role r, const Vec3d& p) {
        BoneTrack& b = t[static_cast<size_t>(r)];
        b.rate_hz = o.rate;
        b.pos.push_back(p);
    };
    for (int i = 0; i < n; ++i) {
        const double time = i / o.rate;
        Vec3d        up_legs[2];
        for (int s = 0; s < 2; ++s) {
            const FootPose f = s ? right(time) : left(time);
            const Vec3d    heel = Add(f.ball, Yawed(f.yaw, 0.0, 0.06, -0.14));
            const Vec3d    tip = Add(f.ball, Yawed(f.yaw, 0.0, -0.02, 0.07));
            const char     side = s ? 'R' : 'L';
            if (o.heel) put(FootPartRole(side, 0), heel);
            if (o.ball) put(FootPartRole(side, 1), f.ball);
            put(FootPartRole(side, 2), tip);
            put(s ? Role::RightKnee : Role::LeftKnee, Add(heel, Vec3d{0.0, 0.42, 0.0}));
            up_legs[s] = Add(heel, Vec3d{0.0, 0.87, 0.0});
            put(s ? Role::RightUpLeg : Role::LeftUpLeg, up_legs[s]);
        }
        put(Role::Hips, Vec3d{0.5 * (up_legs[0].x + up_legs[1].x), 0.5 * (up_legs[0].y + up_legs[1].y) + 0.05,
                              0.5 * (up_legs[0].z + up_legs[1].z)});
    }
    return t;
}

double Ease(double u)
{
    u = std::clamp(u, 0.0, 1.0);
    return 0.5 - 0.5 * std::cos(kPi * u);
}

// A walk (motion_physics_test.cpp's): planted 0.6 s from each contact start, a 0.4 s swing.
FootFn WalkFoot(double first_contact, double x)
{
    return [first_contact, x](double t) {
        const double cycle = 1.0, swing = 0.4;
        const double rel = t - first_contact;
        const int    k = static_cast<int>(std::floor(rel / cycle));
        const double in = rel - k * cycle;
        FootPose     f;
        f.ball.x = x;
        if (in < cycle - swing) {
            f.ball.z = 0.6 * k;
            f.ball.y = 0.02;
        } else {
            const double u = (in - (cycle - swing)) / swing;
            f.ball.z = 0.6 * (k + Ease(u));
            f.ball.y = 0.02 + 0.10 * std::sin(kPi * u);
        }
        return f;
    };
}

FootFn Planted(double x, double z, const std::function<double(double)>& yaw = nullptr)
{
    return [=](double t) {
        FootPose f;
        f.ball = Vec3d{x, 0.02, z};
        f.yaw = yaw ? yaw(t) : 0.0;
        return f;
    };
}

bool SameEvents(const std::vector<Event>& a, const std::vector<Event>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].time_s != b[i].time_s || a[i].block != b[i].block || a[i].marker != b[i].marker ||
            a[i].strength != b[i].strength || a[i].speed != b[i].speed)
            return false;
    return true;
}

std::vector<BoneTrack> Walk(const Options& o = {})
{
    return Rig(WalkFoot(0.4, -0.1), WalkFoot(-0.1, 0.1), o);
}

std::vector<Event> OfBlock(const std::vector<Event>& ev, int block)
{
    std::vector<Event> out;
    for (const Event& e : ev)
        if (e.block == block) out.push_back(e);
    return out;
}

std::vector<PhysicsEvent> Of(const std::vector<PhysicsEvent>& ev, PhysicsKind kind, char side, int part = -2)
{
    std::vector<PhysicsEvent> out;
    for (const PhysicsEvent& e : ev)
        if (e.kind == kind && e.side == side && (part == -2 || e.part == part)) out.push_back(e);
    return out;
}

AutoSettings Ticked(bool step, bool lift, bool slide, bool pivot, bool separate = false)
{
    AutoSettings s;
    s.type[static_cast<int>(AutoType::Step)].on = step;
    s.type[static_cast<int>(AutoType::LiftOff)].on = lift;
    s.type[static_cast<int>(AutoType::Slide)].on = slide;
    s.type[static_cast<int>(AutoType::Pivot)].on = pivot;
    s.separate_steps = separate;
    return s;
}

std::vector<std::string> Markers(const std::vector<Block>& blocks)
{
    std::vector<std::string> m;
    for (const Block& b : blocks) m.push_back(b.marker);
    return m;
}

// A rule on track 0 (a record's id 0 = the left heel role): its height above the floor below 10 cm.
Block HeelRule(const std::string& marker)
{
    Block b;
    b.marker = marker;
    Condition c;
    c.signal.bones = {0};
    c.signal.measure = Measure::Position;
    c.signal.axis = Axis::Vertical;
    c.dir = Direction::Below;
    c.threshold = 0.10;  // the synthetic heel rests 8 cm up and swings to 18 cm
    c.margin = 0.02;
    b.conditions.push_back(c);
    return b;
}

}  // namespace

int main()
{
    // Types, kinds, names and colours.
    {
        CHECK(AutoMarkerName(AutoKind::Step, 'L') == "FS L" && AutoMarkerName(AutoKind::Step, 'R') == "FS R");
        CHECK(AutoMarkerName(AutoKind::Heel, 'L') == "Heel L" && AutoMarkerName(AutoKind::Toe, 'R') == "Toe R");
        CHECK(AutoMarkerName(AutoKind::Lift, 'L') == "Lift L" && AutoMarkerName(AutoKind::Slide, 'R') == "Slide R" &&
              AutoMarkerName(AutoKind::Pivot, 'L') == "Pivot L");
        for (int k = 0; k < kAutoKindCount; ++k) {
            AutoKind back;
            CHECK(AutoKindFromWord(AutoKindWord(static_cast<AutoKind>(k)), &back) && back == static_cast<AutoKind>(k));
            CHECK((AutoDefaultColor(static_cast<AutoKind>(k), 'L') & 0x1000000u) != 0);
            CHECK(AutoDefaultColor(static_cast<AutoKind>(k), 'L') != AutoDefaultColor(static_cast<AutoKind>(k), 'R'));
        }
        CHECK(!AutoKindFromWord("tap", nullptr) && !AutoKindFromWord("", nullptr));
        Block rule = HeelRule("Step");
        CHECK(!IsAutoBlock(rule) && !AutoBlockKind(rule, nullptr, nullptr));
        Block future;
        future.auto_type = "tap";  // a type of a newer RAV: an auto block of no known kind
        CHECK(IsAutoBlock(future) && !AutoBlockKind(future, nullptr, nullptr));
    }

    // First detect: steps combined + lift-off -> FS L / R, Lift L / R; no condition; settings read back.
    {
        const AutoSettings s = Ticked(true, true, false, false);
        std::vector<Block> blocks;
        bool changed = false;
        const std::vector<int> map = ApplyAutoSettings(blocks, s, &changed);
        CHECK(changed && map.empty());
        CHECK(Markers(blocks) == (std::vector<std::string>{"FS L", "FS R", "Lift L", "Lift R"}));
        for (const Block& b : blocks) {
            CHECK(IsAutoBlock(b) && b.conditions.empty() && b.enabled && b.sens == 50.0 && b.offset_ms == 0.0);
            CHECK(b.color != 0);
        }
        CHECK(blocks[0].auto_type == "step" && blocks[0].auto_side == 'L' && blocks[3].auto_type == "lift" &&
              blocks[3].auto_side == 'R');
        CHECK(ReadAutoSettings(blocks) == s);
        CHECK(!AutoPending(s, ReadAutoSettings(blocks)));
        // Detect again with nothing changed: nothing changes.
        std::vector<Block> again = blocks;
        ApplyAutoSettings(again, s, &changed);
        CHECK(!changed && BlocksEqual(again, blocks));
    }

    // Separate steps: Heel L / R and Toe L / R rows.
    {
        std::vector<Block> blocks = AutoBlocks(Ticked(true, false, false, false, true));
        CHECK(Markers(blocks) == (std::vector<std::string>{"Heel L", "Heel R", "Toe L", "Toe R"}));
        CHECK(ReadAutoSettings(blocks).separate_steps);
    }

    // Pending: ticking, unticking and Separate / Combined wait for Detect; a sensitivity does not.
    {
        const AutoSettings item = Ticked(true, false, false, false);
        AutoSettings       ui = item;
        CHECK(!AutoPending(ui, item));
        ui.type[static_cast<int>(AutoType::Step)].sens = 80.0;
        CHECK(!AutoPending(ui, item));
        ui.separate_steps = true;
        CHECK(AutoPending(ui, item));
        ui = item;
        ui.type[static_cast<int>(AutoType::Pivot)].on = true;
        CHECK(AutoPending(ui, item));
        // Separate / combined with steps unticked on both sides is no change.
        AutoSettings a = Ticked(false, true, false, false), b = a;
        b.separate_steps = true;
        CHECK(!AutoPending(a, b));
    }

    // Untick: lift-off unticked -> Lift blocks and their edits removed, the other blocks' edits
    // remapped. Rules (before and after the auto blocks) stay with their edits.
    {
        std::vector<Block> blocks = {HeelRule("Rule A")};
        ApplyAutoSettings(blocks, Ticked(true, true, false, false));
        blocks.push_back(HeelRule("Rule B"));
        CHECK(Markers(blocks) == (std::vector<std::string>{"Rule A", "FS L", "FS R", "Lift L", "Lift R", "Rule B"}));
        std::vector<EventEntry> ev = {MakeUserEvent(0, 0.5, 1, 1), MakeUserEvent(1, 1.0, 0, 0), MakeSuppression(2, 1.5),
                                      MakeUserEvent(3, 2.0, 0, 0), MakeSuppression(4, 2.5), MakeUserEvent(5, 3.0, 1, 1)};
        // Keep the step blocks' own settings while unticking: a new sensitivity reaches them.
        AutoSettings s = Ticked(true, false, false, false);
        s.type[static_cast<int>(AutoType::Step)].sens = 70.0;
        s.type[static_cast<int>(AutoType::Step)].offset_ms = 12.0;
        blocks[1].color = 0x1123456;  // the user's colour on FS L survives
        bool changed = false;
        const std::vector<int> map = ApplyAutoSettings(blocks, s, &changed);
        CHECK(changed);
        CHECK(map == (std::vector<int>{0, 1, 2, -1, -1, 3}));
        CHECK(Markers(blocks) == (std::vector<std::string>{"Rule A", "FS L", "FS R", "Rule B"}));
        CHECK(blocks[1].color == 0x1123456 && blocks[1].sens == 70.0 && blocks[2].offset_ms == 12.0);
        RemapEventBlocks(ev, map);
        CHECK(ev.size() == 4);
        if (ev.size() == 4) {
            CHECK(ev[0].block == 0 && ev[0].t == 0.5);
            CHECK(ev[1].block == 1 && ev[1].t == 1.0);
            CHECK(ev[2].block == 2 && ev[2].t == 1.5 && ev[2].kind == EventKind::Suppress);
            CHECK(ev[3].block == 3 && ev[3].t == 3.0);
        }
    }

    // Separate <-> combined: the step type stays ticked, so its edits stay on that foot.
    {
        std::vector<Block> blocks = AutoBlocks(Ticked(true, true, false, false));  // FS L, FS R, Lift L, Lift R
        std::vector<int>   map = ApplyAutoSettings(blocks, Ticked(true, true, false, false, true));
        CHECK(Markers(blocks) == (std::vector<std::string>{"Lift L", "Lift R", "Heel L", "Heel R", "Toe L", "Toe R"}));
        CHECK(map == (std::vector<int>{2, 3, 0, 1}));
        map = ApplyAutoSettings(blocks, Ticked(true, true, false, false, false));
        CHECK(Markers(blocks) == (std::vector<std::string>{"Lift L", "Lift R", "FS L", "FS R"}));
        CHECK(map == (std::vector<int>{0, 1, 2, 3, 2, 3}));
    }

    // An auto block of an unknown type (a newer RAV's) is kept as it is by Detect.
    {
        Block future;
        future.auto_type = "tap";
        future.marker = "Tap L";
        std::vector<Block> blocks = {future};
        const std::vector<int> map = ApplyAutoSettings(blocks, Ticked(false, true, false, false));
        CHECK(map == (std::vector<int>{0}));
        CHECK(Markers(blocks) == (std::vector<std::string>{"Tap L", "Lift L", "Lift R"}));
        CHECK(!ReadAutoSettings(blocks).type[static_cast<int>(AutoType::Step)].on);
    }

    // Sensitivity: 50 % = the spike's constants; each type moves its own constant.
    {
        CHECK(SensitivityFactor(50.0) == 1.0 && SensitivityFactor(100.0) == 2.0 && SensitivityFactor(0.0) == 0.5);
        CHECK(SensitivityFactor(150.0) == 2.0 && SensitivityFactor(std::nan("")) == 1.0);
        const PhysicsParams d;
        CHECK(SameAutoAnalysis(AutoPhysicsParams(AutoType::Step, 50.0), d));
        CHECK(SameAutoAnalysis(AutoPhysicsParams(AutoType::Slide, 50.0), d));
        CHECK(AutoPhysicsParams(AutoType::Step, 100.0).contact_speed == 2.0 * d.contact_speed);
        CHECK(AutoPhysicsParams(AutoType::LiftOff, 0.0).contact_speed == 0.5 * d.contact_speed);
        CHECK(AutoPhysicsParams(AutoType::Slide, 100.0).slide_speed == 0.5 * d.slide_speed);
        const PhysicsParams pv = AutoPhysicsParams(AutoType::Pivot, 100.0);
        CHECK(pv.pivot_min_deg == 0.5 * d.pivot_min_deg && pv.pivot_rate_dps == 0.5 * d.pivot_rate_dps &&
              pv.contact_speed == d.contact_speed);
        // One analysis per distinct parameter set.
        std::vector<Block> blocks = AutoBlocks(Ticked(true, true, true, true));
        CHECK(AutoParamSets(blocks).size() == 1);
        for (Block& b : blocks)
            if (b.auto_type == "slide") b.sens = 80.0;
        CHECK(AutoParamSets(blocks).size() == 2);
        blocks[0].sens = 60.0;  // FS L only
        CHECK(AutoParamSets(blocks).size() == 3);
    }

    // The walk: combined steps = the physics' foot steps, separate = its heel and toe steps,
    // lift-offs; each event on its block, with the block's marker; speed = strength x leg.
    {
        const std::vector<BoneTrack> rig = Walk();
        const PhysicsAnalysis        a = AnalyseMotion(rig);
        CHECK(a.ok);
        const std::vector<PhysicsEvent> phys = FootEvents(a);
        std::vector<Block>              blocks = {HeelRule("Rule")};
        ApplyAutoSettings(blocks, Ticked(true, true, false, false));  // Rule, FS L, FS R, Lift L, Lift R
        std::vector<AutoAnalysis> cache;
        const std::vector<Event>  ev = DetectAutoEvents(blocks, rig, &cache);
        CHECK(cache.size() == 1);
        CHECK(OfBlock(ev, 0).empty());
        const std::vector<Event>        fsl = OfBlock(ev, 1);
        const std::vector<PhysicsEvent> pfl = Of(phys, PhysicsKind::FootStep, 'L');
        CHECK(fsl.size() == 4 && fsl.size() == pfl.size());
        for (size_t i = 0; i < fsl.size() && i < pfl.size(); ++i) {
            CHECK(fsl[i].time_s == pfl[i].time_s && fsl[i].marker == "FS L" && fsl[i].strength == pfl[i].strength);
            CHECK(std::fabs(fsl[i].speed - pfl[i].strength * a.leg_length) < 1e-12);
        }
        CHECK(OfBlock(ev, 2).size() == Of(phys, PhysicsKind::FootStep, 'R').size());
        CHECK(OfBlock(ev, 3).size() == Of(phys, PhysicsKind::LiftOff, 'L').size() && !OfBlock(ev, 3).empty());
        CHECK(OfBlock(ev, 4).size() == Of(phys, PhysicsKind::LiftOff, 'R').size());
        CHECK(std::is_sorted(ev.begin(), ev.end(), [](const Event& x, const Event& y) { return x.time_s < y.time_s; }));
        // The same events without a cache; the cache reused (no new analysis).
        const std::vector<Event> ev2 = DetectAutoEvents(blocks, rig);
        CHECK(ev2.size() == ev.size());
        DetectAutoEvents(blocks, rig, &cache);
        CHECK(cache.size() == 1);

        // Offset: step +20 ms -> every step 20 ms later.
        std::vector<Block> later = blocks;
        AutoSettings       s = ReadAutoSettings(later);
        s.type[static_cast<int>(AutoType::Step)].offset_ms = 20.0;
        ApplyAutoSettings(later, s);
        const std::vector<Event> fsl20 = OfBlock(DetectAutoEvents(later, rig), 1);
        CHECK(fsl20.size() == fsl.size());
        for (size_t i = 0; i < fsl20.size() && i < fsl.size(); ++i)
            CHECK(std::fabs(fsl20[i].time_s - (fsl[i].time_s + 0.020)) < 1e-12);

        // An off auto block has no event and needs no analysis.
        std::vector<Block> off = blocks;
        for (Block& b : off) b.enabled = !IsAutoBlock(b);
        CHECK(DetectAutoEvents(off, rig).empty() && !HasActiveAutoBlocks(off));

        // Separate: one marker per part contact (heel on the heel, toe on the ball).
        std::vector<Block> sep = AutoBlocks(Ticked(true, false, false, false, true));  // Heel L, Heel R, Toe L, Toe R
        const std::vector<Event> se = DetectAutoEvents(sep, rig);
        CHECK(OfBlock(se, 0).size() == Of(phys, PhysicsKind::Step, 'L', 0).size() && !OfBlock(se, 0).empty());
        CHECK(OfBlock(se, 2).size() == Of(phys, PhysicsKind::Step, 'L', 1).size() && !OfBlock(se, 2).empty());
        const std::vector<PhysicsEvent> toe_l = Of(phys, PhysicsKind::Step, 'L', 1);
        const std::vector<Event>        se_toe = OfBlock(se, 2);
        for (size_t i = 0; i < se_toe.size() && i < toe_l.size(); ++i)
            CHECK(se_toe[i].time_s == toe_l[i].time_s && se_toe[i].marker == "Toe L");

        // No ball bone: the toe steps land on the tip.
        Options nb;
        nb.ball = false;
        const std::vector<BoneTrack>    tip_rig = Walk(nb);
        const std::vector<PhysicsEvent> tip_phys = FootEvents(AnalyseMotion(tip_rig));
        const std::vector<Event>        tip_ev = OfBlock(DetectAutoEvents(sep, tip_rig), 2);
        const std::vector<PhysicsEvent> tip_l = Of(tip_phys, PhysicsKind::Step, 'L', 2);
        CHECK(!tip_ev.empty() && tip_ev.size() == tip_l.size());
        for (size_t i = 0; i < tip_ev.size() && i < tip_l.size(); ++i) CHECK(tip_ev[i].time_s == tip_l[i].time_s);
    }

    // The analysis cache: many sensitivities in a row on one cache keep it at 8 analyses at most,
    // and the events are always those of an uncached run.
    {
        const std::vector<BoneTrack> rig = Walk();
        std::vector<Block>           blocks = AutoBlocks(Ticked(true, false, false, false));  // FS L, FS R
        std::vector<AutoAnalysis>    cache;
        for (int s = 0; s <= 100; s += 8) {  // 13 distinct sets
            for (Block& b : blocks) b.sens = s;
            const std::vector<Event> cached = DetectAutoEvents(blocks, rig, &cache);
            CHECK(cache.size() <= 8);
            CHECK(SameEvents(cached, DetectAutoEvents(blocks, rig)));
            CHECK(!cached.empty());
        }
        CHECK(cache.size() == 8);
        // Two sets in use at once (steps and lift-offs at other sensitivities) stay cached.
        std::vector<Block> two = AutoBlocks(Ticked(true, true, false, false));
        for (Block& b : two) b.sens = b.auto_type == "lift" ? 3.0 : 97.0;
        CHECK(SameEvents(DetectAutoEvents(two, rig, &cache), DetectAutoEvents(two, rig)) && cache.size() <= 8);
        CHECK(SameEvents(DetectAutoEvents(two, rig, &cache), DetectAutoEvents(two, rig)));
    }

    // Pivot end to end: the planted left foot turns 90 deg on the ball in 250 ms (from 1.0 s): one
    // Pivot L event on its block, its strength the swept angle (degrees), its speed 0.
    {
        Options o;
        o.dur = 3.0;
        const std::vector<BoneTrack> rig =
            Rig(Planted(-0.1, 0.0, [](double t) { return 0.5 * kPi * Ease((t - 1.0) / 0.25); }), Planted(0.1, 0.2), o);
        const std::vector<Block> blocks = AutoBlocks(Ticked(false, false, false, true));  // Pivot L, Pivot R
        const std::vector<Event> ev = DetectAutoEvents(blocks, rig);
        CHECK(ev.size() == 1);
        if (ev.size() == 1) {
            CHECK(ev[0].block == 0 && ev[0].marker == "Pivot L" && ev[0].speed == 0.0);
            CHECK(ev[0].strength > 80.0 && ev[0].strength < 95.0);
            CHECK(ev[0].time_s > 0.95 && ev[0].time_s < 1.05);
        }
    }

    // Sensitivity on a slow slide: the planted left foot slides 0.18 m/s for 400 ms (0.21 leg/s,
    // under the default 0.25): no slide at 50 %, one at 80 % (more or equal slides).
    {
        const FootFn slide = [](double t) {
            FootPose f;
            f.ball = Vec3d{-0.1, 0.02, 0.18 * std::clamp(t - 1.0, 0.0, 0.4)};
            return f;
        };
        Options o;
        o.dur = 3.0;
        const std::vector<BoneTrack> rig = Rig(slide, Planted(0.1, 0.2), o);
        std::vector<Block>           blocks = AutoBlocks(Ticked(false, false, true, false));  // Slide L, Slide R
        const size_t                 at50 = OfBlock(DetectAutoEvents(blocks, rig), 0).size();
        for (Block& b : blocks) b.sens = 80.0;
        const std::vector<Event> sl80 = OfBlock(DetectAutoEvents(blocks, rig), 0);
        CHECK(at50 == 0 && sl80.size() == 1);
        if (sl80.size() == 1) {
            CHECK(std::fabs(sl80[0].time_s - 1.0) < 0.05 && sl80[0].marker == "Slide L");
            CHECK(sl80[0].speed > 0.0);
        }
        for (int s = 0; s <= 100; s += 10) {
            for (Block& b : blocks) b.sens = s;
            const size_t lo = OfBlock(DetectAutoEvents(blocks, rig), 0).size();
            for (Block& b : blocks) b.sens = s + 10;
            CHECK(OfBlock(DetectAutoEvents(blocks, rig), 0).size() >= lo);
        }
    }

    // No foot roles: no auto events, and the missing parts are named.
    {
        Options o;
        o.heel = false;
        o.ball = false;
        std::vector<BoneTrack> rig = Walk(o);
        for (Role r : {Role::LeftToeEnd, Role::RightToeEnd}) rig[static_cast<size_t>(r)] = BoneTrack{};
        CHECK(DetectAutoEvents(AutoBlocks(Ticked(true, true, true, true)), rig).empty());

        std::vector<int> map(static_cast<size_t>(Role::Count), -1);
        const std::vector<std::string> all = AutoMissingParts(AutoType::Step, false, map);
        CHECK(std::find(all.begin(), all.end(), "left heel") != all.end());
        CHECK(std::find(all.begin(), all.end(), "left knee") != all.end());
        CHECK(std::find(all.begin(), all.end(), "right toe end") != all.end());
        // A full Mixamo-like mapping: nothing missing for any type.
        for (int r = 0; r <= static_cast<int>(Role::RightToeEnd); ++r) map[static_cast<size_t>(r)] = r;
        for (int t = 0; t < kAutoTypeCount; ++t)
            for (bool sep : {false, true}) CHECK(AutoMissingParts(static_cast<AutoType>(t), sep, map).empty());
        // No toe nor toe end on the right: separate steps and pivots need it, combined steps do not.
        map[static_cast<size_t>(Role::RightToe)] = map[static_cast<size_t>(Role::RightToeEnd)] = -1;
        CHECK(AutoMissingParts(AutoType::Step, true, map) == std::vector<std::string>{"right toe"});
        CHECK(AutoMissingParts(AutoType::Pivot, false, map) == std::vector<std::string>{"right toe"});
        CHECK(AutoMissingParts(AutoType::Step, false, map).empty());
        // The toe end alone stands in for the toe.
        map[static_cast<size_t>(Role::RightToeEnd)] = 3;
        CHECK(AutoMissingParts(AutoType::Step, true, map).empty());
        // The hips stand in for a missing up leg; without both, and no knee, no body scale.
        map[static_cast<size_t>(Role::LeftUpLeg)] = map[static_cast<size_t>(Role::RightUpLeg)] = -1;
        CHECK(AutoMissingParts(AutoType::LiftOff, false, map).empty());
        map[static_cast<size_t>(Role::Hips)] = map[static_cast<size_t>(Role::LeftKnee)] =
            map[static_cast<size_t>(Role::RightKnee)] = -1;
        CHECK(AutoMissingParts(AutoType::LiftOff, false, map) ==
              (std::vector<std::string>{"left knee", "left hip (up leg)", "right knee", "right hip (up leg)"}));
    }

    // Mixed item: rules and auto blocks both detect; the rule engine skips the auto blocks.
    {
        const std::vector<BoneTrack> rig = Walk();
        std::vector<Block>           blocks = {HeelRule("Rule")};
        blocks[0].conditions[0].signal.bones = {0};
        ApplyAutoSettings(blocks, Ticked(true, false, false, false));
        // The rule reads track 0 = the left heel.
        const std::vector<BoneTrack> tracks = {rig[static_cast<size_t>(Role::LeftHeel)]};
        const DetectionTrace         tr = DetectTrace(blocks, tracks);
        CHECK(tr.blocks.size() == 3 && tr.blocks[0].ran && !tr.blocks[1].ran && !tr.blocks[2].ran);
        CHECK(!tr.events.empty());
        for (const Event& e : tr.events) CHECK(e.block == 0);
        CHECK(!OfBlock(DetectAutoEvents(blocks, rig), 1).empty());
        // An auto block with a condition (a hand edit) still never fires as a rule.
        std::vector<Block> odd = blocks;
        odd[1].conditions = blocks[0].conditions;
        const DetectionTrace tr2 = DetectTrace(odd, tracks);
        CHECK(!tr2.blocks[1].ran && OfBlock(tr2.events, 1).empty());
        double s = 1.0, v = 1.0;
        CHECK(!EventValuesAt(odd[1], tracks, DetectOptions{}, 1.0, &s, &v) && s == 0.0 && v == 0.0);
        CHECK(EventValuesAt(blocks[0], tracks, DetectOptions{}, 1.0, &s, &v));
    }

    // The record: an item with auto blocks round-trips; a pooled copy gets them; a REAPER-side edit
    // of an auto marker finds its block by name.
    {
        ItemRules r;
        r.blocks = {HeelRule("Rule")};
        ApplyAutoSettings(r.blocks, Ticked(true, true, false, false));
        r.blocks[2].sens = 62.5;
        r.blocks[2].offset_ms = -8;
        r.blocks[4].enabled = false;
        r.events = {MakeUserEvent(2, 1.25, 0, 0)};
        const std::string text = SerializeItemRules(r);
        CHECK(text.find("block auto=step side=L sens=50 color=#5F9EDD") != std::string::npos);
        CHECK(text.find("block auto=step side=R sens=62.5 ") != std::string::npos);
        CHECK(text.find("block on=0 auto=lift side=R sens=50 ") != std::string::npos);
        ItemRules back;
        CHECK(ParseItemRules(text, &back) && !HasKeptText(back));
        CHECK(BlocksEqual(back.blocks, r.blocks) && SerializeItemRules(back) == text);
        CHECK(ReadAutoSettings(back.blocks) == ReadAutoSettings(r.blocks));
        // Pooled copy: CopyPoolContent carries them.
        ItemRules copy;
        CopyPoolContent(back, copy);
        CHECK(BlocksEqual(copy.blocks, r.blocks) && PoolContentEqual(copy, back));
        // MarkerEditBlock: an edit of "FS R" in REAPER is the FS R block's.
        MarkerEdit e;
        e.kind = MarkerEditKind::Delete;
        e.c = 1.25;
        e.name = "FS R";
        e.color = r.blocks[2].color;
        e.has_color = true;
        CHECK(MarkerEditBlock(back, e) == 2);
        e.name = PreviewMarkerName("Lift L");
        e.preview = true;
        e.color = PreviewColor(r.blocks[3].color);
        CHECK(MarkerEditBlock(back, e) == 3);
        // BlocksEqual sees the auto fields (Legacy / Edited).
        std::vector<Block> other = r.blocks;
        other[1].sens = 51;
        CHECK(!BlocksEqual(other, r.blocks));
        other = r.blocks;
        other[1].auto_side = 'R';
        CHECK(!BlocksEqual(other, r.blocks));
        other = r.blocks;
        other[1].auto_type = "heel";
        CHECK(!BlocksEqual(other, r.blocks));
        // An unreadable type or side is kept as written.
        const std::string head = "RAVRULES 1\noptions sensitivity=0 edge_ms=0 smooth_ms=8\nanalyse floor_pct=2 "
                                 "pos_frac=0.25 speed_pct=30 margin_ratio=0.5 onset_frac=0.1 per_bone_floor=0\n";
        for (const std::string& odd :
             {head + "block auto=Step! color=none hold_ms=0 cooldown_ms=0 offset_ms=0 land=cross marker=FS L\n",
              head + "block auto=step side=X sens=50 color=none hold_ms=0 cooldown_ms=0 offset_ms=0 land=cross marker=FS L\n"}) {
            ItemRules oddr;
            CHECK(ParseItemRules(odd, &oddr) && HasKeptText(oddr) && SerializeItemRules(oddr) == odd);
        }
    }

    if (g_fails) std::printf("%d check(s) failed\n", g_fails);
    else std::printf("auto_detect: all checks passed\n");
    return g_fails ? 1 : 0;
}
