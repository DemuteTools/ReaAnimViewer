// SPDX-License-Identifier: MIT
//
// Host test of auto detection (src/auto_detect.h, Epic 10, story 10-8b; hands and categories:
// 10-8c): the specs' matrix rows that are pure (blocks, settings, Detect's remap, sensitivity,
// offset, the toe without a ball bone, missing roles, rules next to auto blocks, pooled copies,
// the record, categories, hand blocks; hand pivots: 10-8e). No REAPER.
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
// knee 42 cm and the up leg 87 cm above the heel; the hips between the up legs; the hands, when
// given, where their functions say.
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

using HandFn = std::function<Vec3d(double)>;

struct Options {
    double dur = 4.0;
    bool   ball = true;  // false: no toe base bone (the toe end alone)
    bool   heel = true;  // false: no heel bone
    double rate = 240.0;
    HandFn hand[2];      // L, R; empty = no bone for that hand
    std::function<double(double)> hand_yaw[2];  // L, R: the hand's heading (rad about +Y); empty = no rotation
};

Quatd Mul(const Quatd& a, const Quatd& b)
{
    return Quatd{a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                 a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

Quatd AxisAngle(double x, double y, double z, double angle)
{
    const double s = std::sin(0.5 * angle);
    return Quatd{std::cos(0.5 * angle), x * s, y * s, z * s};
}

// The hand bone's world rotation: the heading about +Y, then a fixed bone-space frame.
Quatd HandRotationOf(double yaw)
{
    static const Quatd kBoneFrame = Mul(AxisAngle(0.0, 0.0, 1.0, 0.9), AxisAngle(0.8, 0.0, 0.6, 0.5));
    return Mul(AxisAngle(0.0, 1.0, 0.0, yaw), kBoneFrame);
}

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
        for (int s = 0; s < 2; ++s)
            if (o.hand[s]) {
                put(s ? Role::RightHand : Role::LeftHand, o.hand[s](time));
                if (o.hand_yaw[s])
                    t[static_cast<size_t>(s ? Role::RightHand : Role::LeftHand)].rot_world.push_back(
                        HandRotationOf(o.hand_yaw[s](time)));
            }
    }
    return t;
}

// A hand that moves forward and down at `speed` m/s, at rest over [a, b) (a grab at a, a release
// at b), at height 1.4 m at t = 0.
HandFn HandGrab(double x, double speed, double a, double b)
{
    return [=](double t) {
        const double d = speed * (t - (std::clamp(t, a, b) - a));
        return Vec3d{x, 1.4 - 0.6 * d, 0.8 * d};
    };
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

AutoSettings Ticked(bool step, bool lift, bool slide, bool pivot, bool separate = false, bool grab = false,
                    bool release = false)
{
    AutoSettings s;
    s.type[static_cast<int>(AutoType::Grab)].on = grab;
    s.type[static_cast<int>(AutoType::Release)].on = release;
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
        // Story 10-8h: the kept step blocks keep their own sensitivity and offset (a side's value,
        // perhaps raised by Detect), whatever the settings say.
        blocks[1].sens = 73.0;
        blocks[1].sens_from = 50.0;
        AutoSettings s = Ticked(true, false, false, false);
        s.type[static_cast<int>(AutoType::Step)].sens = 70.0;
        s.type[static_cast<int>(AutoType::Step)].offset_ms = 12.0;
        blocks[1].color = 0x1123456;  // the user's colour on FS L survives
        bool changed = false;
        const std::vector<int> map = ApplyAutoSettings(blocks, s, &changed);
        CHECK(changed);
        CHECK(map == (std::vector<int>{0, 1, 2, -1, -1, 3}));
        CHECK(Markers(blocks) == (std::vector<std::string>{"Rule A", "FS L", "FS R", "Rule B"}));
        CHECK(blocks[1].color == 0x1123456 && blocks[1].sens == 73.0 && blocks[1].sens_from == 50.0);
        CHECK(blocks[2].sens == 50.0 && blocks[2].offset_ms == 0.0 && blocks[2].sens_from < 0.0);
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
        CHECK(SameAutoAnalysis(AutoPhysicsParams(AutoType::Step, 50.0, false), d));
        CHECK(SameAutoAnalysis(AutoPhysicsParams(AutoType::Slide, 50.0, false), d));
        CHECK(AutoPhysicsParams(AutoType::Step, 100.0, false).contact_speed == 2.0 * d.contact_speed);
        CHECK(AutoPhysicsParams(AutoType::LiftOff, 0.0, false).contact_speed == 0.5 * d.contact_speed);
        CHECK(AutoPhysicsParams(AutoType::Slide, 100.0, false).slide_speed == 0.5 * d.slide_speed);
        const PhysicsParams pv = AutoPhysicsParams(AutoType::Pivot, 100.0, false);
        CHECK(pv.pivot_min_deg == 0.5 * d.pivot_min_deg && pv.pivot_rate_dps == 0.5 * d.pivot_rate_dps &&
              pv.contact_speed == d.contact_speed);
        // One analysis per distinct parameter set.
        std::vector<Block> blocks = AutoBlocks(Ticked(true, true, true, true));
        CHECK(AutoParamSets(blocks, false).size() == 1);
        for (Block& b : blocks)
            if (b.auto_type == "slide") b.sens = 80.0;
        CHECK(AutoParamSets(blocks, false).size() == 2);
        blocks[0].sens = 60.0;  // FS L only
        CHECK(AutoParamSets(blocks, false).size() == 3);
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
        const std::vector<Event>  ev = DetectAutoEvents(blocks, rig, &cache, false);
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
        const std::vector<Event> ev2 = DetectAutoEvents(blocks, rig, nullptr, false);
        CHECK(ev2.size() == ev.size());
        DetectAutoEvents(blocks, rig, &cache, false);
        CHECK(cache.size() == 1);

        // Offset: step +20 ms -> every step 20 ms later.
        std::vector<Block> later = blocks;
        for (Block& b : later)
            if (b.auto_type == "step") b.offset_ms = 20.0;  // as the section's field writes it
        const std::vector<Event> fsl20 = OfBlock(DetectAutoEvents(later, rig, nullptr, false), 1);
        CHECK(fsl20.size() == fsl.size());
        for (size_t i = 0; i < fsl20.size() && i < fsl.size(); ++i)
            CHECK(std::fabs(fsl20[i].time_s - (fsl[i].time_s + 0.020)) < 1e-12);

        // An off auto block has no event and needs no analysis.
        std::vector<Block> off = blocks;
        for (Block& b : off) b.enabled = !IsAutoBlock(b);
        CHECK(DetectAutoEvents(off, rig, nullptr, false).empty() && !HasActiveAutoBlocks(off));

        // Separate: one marker per part contact (heel on the heel, toe on the ball).
        std::vector<Block> sep = AutoBlocks(Ticked(true, false, false, false, true));  // Heel L, Heel R, Toe L, Toe R
        const std::vector<Event> se = DetectAutoEvents(sep, rig, nullptr, false);
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
        const std::vector<Event>        tip_ev = OfBlock(DetectAutoEvents(sep, tip_rig, nullptr, false), 2);
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
            const std::vector<Event> cached = DetectAutoEvents(blocks, rig, &cache, false);
            CHECK(cache.size() <= 8);
            CHECK(SameEvents(cached, DetectAutoEvents(blocks, rig, nullptr, false)));
            CHECK(!cached.empty());
        }
        CHECK(cache.size() == 8);
        // Two sets in use at once (steps and lift-offs at other sensitivities) stay cached.
        std::vector<Block> two = AutoBlocks(Ticked(true, true, false, false));
        for (Block& b : two) b.sens = b.auto_type == "lift" ? 3.0 : 97.0;
        CHECK(SameEvents(DetectAutoEvents(two, rig, &cache, false), DetectAutoEvents(two, rig, nullptr, false)) && cache.size() <= 8);
        CHECK(SameEvents(DetectAutoEvents(two, rig, &cache, false), DetectAutoEvents(two, rig, nullptr, false)));
    }

    // Pivot end to end: the planted left foot turns 90 deg on the ball in 250 ms (from 1.0 s): one
    // Pivot L event on its block, its strength the swept angle (degrees), its speed 0.
    {
        Options o;
        o.dur = 3.0;
        const std::vector<BoneTrack> rig =
            Rig(Planted(-0.1, 0.0, [](double t) { return 0.5 * kPi * Ease((t - 1.0) / 0.25); }), Planted(0.1, 0.2), o);
        const std::vector<Block> blocks = AutoBlocks(Ticked(false, false, false, true));  // Pivot L, Pivot R
        const std::vector<Event> ev = DetectAutoEvents(blocks, rig, nullptr, false);
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
        const size_t                 at50 = OfBlock(DetectAutoEvents(blocks, rig, nullptr, false), 0).size();
        for (Block& b : blocks) b.sens = 80.0;
        const std::vector<Event> sl80 = OfBlock(DetectAutoEvents(blocks, rig, nullptr, false), 0);
        CHECK(at50 == 0 && sl80.size() == 1);
        if (sl80.size() == 1) {
            CHECK(std::fabs(sl80[0].time_s - 1.0) < 0.05 && sl80[0].marker == "Slide L");
            CHECK(sl80[0].speed > 0.0);
        }
        for (int s = 0; s <= 100; s += 10) {
            for (Block& b : blocks) b.sens = s;
            const size_t lo = OfBlock(DetectAutoEvents(blocks, rig, nullptr, false), 0).size();
            for (Block& b : blocks) b.sens = s + 10;
            CHECK(OfBlock(DetectAutoEvents(blocks, rig, nullptr, false), 0).size() >= lo);
        }
    }

    // Story 10-8h: Detect's sensitivity search, on the slow slide (Slide L: none at 50 %, one at
    // 80 %; the right foot stays planted: no slide up to 100 %).
    {
        const FootFn slide = [](double t) {
            FootPose f;
            f.ball = Vec3d{-0.1, 0.02, 0.18 * std::clamp(t - 1.0, 0.0, 0.4)};
            return f;
        };
        Options o;
        o.dur = 3.0;
        const std::vector<BoneTrack> rig = Rig(slide, Planted(0.1, 0.2), o);
        std::vector<int>             map(static_cast<size_t>(Role::Count), -1);
        for (int r = 0; r < static_cast<int>(Role::Count); ++r)
            if (!rig[static_cast<size_t>(r)].pos.empty()) map[static_cast<size_t>(r)] = r;
        auto count = [&](std::vector<Block> blocks, int bi, double sens) {
            blocks[static_cast<size_t>(bi)].sens = sens;
            return static_cast<int>(OfBlock(DetectAutoEvents(blocks, rig, nullptr, false), bi).size());
        };
        auto counts_of = [&](const std::vector<Block>& blocks) {
            std::vector<int> c(blocks.size(), 0);
            for (const Event& e : DetectAutoEvents(blocks, rig, nullptr, false)) ++c[static_cast<size_t>(e.block)];
            return c;
        };
        std::vector<Block> blocks = {HeelRule("Rule")};
        ApplyAutoSettings(blocks, Ticked(false, false, true, false));  // Rule, Slide L, Slide R
        CHECK(count(blocks, 1, 50.0) == 0 && count(blocks, 1, 80.0) >= 1 && count(blocks, 2, 100.0) == 0);

        // Raise: Slide L gets the lowest whole % with an event (sens_from = 50); Nothing: Slide R
        // has no event up to 100 % (unchanged). The rule is never searched.
        std::vector<AutoSensResult> res = SearchAutoSensitivity(blocks, rig, counts_of(blocks), map, false);
        CHECK(res.size() == 3 && !res[0].searched);
        double raised = 0.0;
        if (res.size() == 3) {
            CHECK(res[1].searched && res[1].raised && !res[1].none && res[1].sens_from == 50.0);
            raised = res[1].sens;
            CHECK(raised > 50.0 && raised <= 80.0 && raised == std::floor(raised));
            CHECK(count(blocks, 1, raised) >= 1 && count(blocks, 1, raised - 1.0) == 0);
            CHECK(res[2].searched && res[2].none && !res[2].raised && res[2].sens == 50.0);
        }
        // Applied as Detect does: the raised side detects; events at an unchanged sensitivity are
        // the same.
        std::vector<Block> after = blocks;
        after[1].sens = raised;
        after[1].sens_from = 50.0;
        CHECK(OfBlock(DetectAutoEvents(after, rig, nullptr, false), 1).size() >= 1);
        CHECK(SameEvents(OfBlock(DetectAutoEvents(after, rig, nullptr, false), 2), OfBlock(DetectAutoEvents(blocks, rig, nullptr, false), 2)));
        // Searched again: the raised side has its event now, untouched.
        res = SearchAutoSensitivity(after, rig, counts_of(after), map, false);
        CHECK(res.size() == 3 && !res[1].searched && res[1].sens == raised);

        // Has events: a block whose count is not 0 is never searched (its value stays).
        std::vector<int> c = counts_of(blocks);
        c[1] = 3;
        res = SearchAutoSensitivity(blocks, rig, c, map, false);
        CHECK(res.size() == 3 && !res[1].searched && !res[1].raised && res[1].sens == 50.0);

        // Already 100: no event, no raise; "none".
        std::vector<Block> full = blocks;
        full[2].sens = 100.0;
        res = SearchAutoSensitivity(full, rig, counts_of(full), map, false);
        CHECK(res.size() == 3 && res[2].searched && res[2].none && res[2].sens == 100.0 && res[2].sens_from < 0.0);

        // User lowered to 30: bisection from 30, sens_from = 30, the same lowest value.
        std::vector<Block> low = blocks;
        low[1].sens = 30.0;
        res = SearchAutoSensitivity(low, rig, counts_of(low), map, false);
        CHECK(res.size() == 3 && res[1].raised && res[1].sens_from == 30.0 && res[1].sens == raised);

        // Missing bone: the left foot's parts unmapped -> Slide L not searched; Slide R still is.
        std::vector<int> nomap = map;
        for (Role r : {Role::LeftHeel, Role::LeftToe, Role::LeftToeEnd}) nomap[static_cast<size_t>(r)] = -1;
        CHECK(AutoBlockPartMissing(blocks[1], nomap) && !AutoBlockPartMissing(blocks[2], nomap));
        res = SearchAutoSensitivity(blocks, rig, counts_of(blocks), nomap, false);
        CHECK(res.size() == 3 && !res[1].searched && res[1].sens == 50.0 && res[2].searched);

        // An off block, and a failed analysis (no role track): not searched.
        std::vector<Block> off = blocks;
        off[1].enabled = false;
        res = SearchAutoSensitivity(off, rig, std::vector<int>(off.size(), 0), map, false);
        CHECK(res.size() == 3 && !res[1].searched && res[2].searched);
        res = SearchAutoSensitivity(blocks, std::vector<BoneTrack>(rig.size()), std::vector<int>(blocks.size(), 0), map, false);
        CHECK(res.size() == 3 && !res[1].searched && !res[2].searched);

        // A raised block keeps its first start value when searched again (it lost its events).
        std::vector<Block> again = blocks;
        again[1].sens = 40.0;
        again[1].sens_from = 25.0;
        res = SearchAutoSensitivity(again, rig, counts_of(again), map, false);
        CHECK(res.size() == 3 && res[1].raised && res[1].sens_from == 25.0);

        // Detect keeps a block's own (raised) sensitivity: ApplyAutoSettings leaves it.
        bool changed = true;
        ApplyAutoSettings(after, ReadAutoSettings(after), &changed);
        CHECK(!changed && after[1].sens == raised && after[1].sens_from == 50.0 && after[2].sens == 50.0);
        // Detect's apply step on the unchanged settings: Slide L raised (sens_from 50), Slide R
        // (none up to 100 %) and the rule unchanged.
        std::vector<Block>          applied = blocks;
        std::vector<AutoSensResult> ar;
        std::vector<AutoAnalysis>   cache;
        CHECK(ApplyAutoSensSearch(applied, rig, map, &ar, &cache, false));
        CHECK(applied[1].sens == raised && applied[1].sens_from == 50.0);
        CHECK(applied[2].sens == 50.0 && applied[2].sens_from < 0.0 && ar.size() == 3 && ar[2].none);
        CHECK(applied[0].sens == blocks[0].sens && applied[0].marker == "Rule");
        // Again: nothing left to raise.
        CHECK(!ApplyAutoSensSearch(applied, rig, map, nullptr, nullptr, false));
    }

    // Story 10-8h: per-row values before Detect: a new block takes its row's value (others the
    // type's); rows never make Detect pending nor change ==; a new block replacing a dropped one
    // of the same type and side still takes that block's start value.
    {
        AutoSettings s = Ticked(false, false, true, false);  // Slide
        AutoTypeSettings& sl = s.type[static_cast<int>(AutoType::Slide)];
        sl.sens = 55.0;
        SetAutoRowValue(sl, "slide", 'R', true, 83.0);
        SetAutoRowValue(sl, "slide", 'R', false, -7.0);
        CHECK(sl.rows.size() == 1 && FindAutoRow(sl, "slide", 'R') && !FindAutoRow(sl, "slide", 'L'));
        CHECK(FindAutoRow(sl, "slide", 'R')->sens == 83.0 && FindAutoRow(sl, "slide", 'R')->offset_ms == -7.0);
        AutoSettings no_rows = s;
        no_rows.type[static_cast<int>(AutoType::Slide)].rows.clear();
        CHECK(s == no_rows && !AutoPending(s, no_rows));
        std::vector<Block> blocks;
        ApplyAutoSettings(blocks, s);
        CHECK(Markers(blocks) == (std::vector<std::string>{"Slide L", "Slide R"}));
        if (blocks.size() == 2) {
            CHECK(blocks[0].sens == 55.0 && blocks[0].offset_ms == 0.0);
            CHECK(blocks[1].sens == 83.0 && blocks[1].offset_ms == -7.0 && blocks[1].sens_from < 0.0);
        }
        // Steps: a separate Toe R row reaches its block; switched from combined, the dropped
        // block's start value wins.
        AutoSettings st = Ticked(true, false, false, false, true);
        SetAutoRowValue(st.type[static_cast<int>(AutoType::Step)], "toe", 'R', true, 66.0);
        std::vector<Block> sep;
        ApplyAutoSettings(sep, st);
        for (const Block& b : sep) CHECK(b.sens == (b.auto_type == "toe" && b.auto_side == 'R' ? 66.0 : 50.0));
        std::vector<Block> comb = AutoBlocks(Ticked(true, false, false, false));  // FS L, FS R
        comb[1].sens = 40.0;
        ApplyAutoSettings(comb, st);
        for (const Block& b : comb) CHECK(b.sens == (b.auto_side == 'R' ? 40.0 : 50.0));
    }

    // Story 10-8h: steps switched combined -> separate with FS L raised to 73 from 50 and FS R at
    // 60: Heel L / Toe L start again at 50, Heel R / Toe R at 60, none raised; the offsets follow.
    {
        std::vector<Block> blocks = AutoBlocks(Ticked(true, false, false, false));  // FS L, FS R
        blocks[0].sens = 73.0;
        blocks[0].sens_from = 50.0;
        blocks[0].offset_ms = 5.0;
        blocks[1].sens = 60.0;
        AutoSettings s = ReadAutoSettings(blocks);
        s.separate_steps = true;
        ApplyAutoSettings(blocks, s);
        CHECK(Markers(blocks) == (std::vector<std::string>{"Heel L", "Heel R", "Toe L", "Toe R"}));
        for (const Block& b : blocks) {
            CHECK(b.sens_from < 0.0);
            CHECK(b.sens == (b.auto_side == 'L' ? 50.0 : 60.0));
            CHECK(b.offset_ms == (b.auto_side == 'L' ? 5.0 : 0.0));
        }
        // A hand pivot switched to any resting hand drops a raise found for the other kind.
        AutoSettings hs;
        hs.type[static_cast<int>(AutoType::HandPivot)].on = true;
        std::vector<Block> hp = AutoBlocks(hs);
        CHECK(hp.size() == 2);
        if (hp.size() == 2) {
            hp[0].sens = 80.0;
            hp[0].sens_from = 50.0;
            AutoSettings any = ReadAutoSettings(hp);
            any.hand_pivot_any = true;
            ApplyAutoSettings(hp, any);
            CHECK(hp[0].auto_type == "hand_pivot_any" && hp[0].sens == 50.0 && hp[0].sens_from < 0.0);
        }
    }

    // No foot roles: no auto events, and the missing parts are named.
    {
        Options o;
        o.heel = false;
        o.ball = false;
        std::vector<BoneTrack> rig = Walk(o);
        for (Role r : {Role::LeftToeEnd, Role::RightToeEnd}) rig[static_cast<size_t>(r)] = BoneTrack{};
        CHECK(DetectAutoEvents(AutoBlocks(Ticked(true, true, true, true)), rig, nullptr, false).empty());

        std::vector<int> map(static_cast<size_t>(Role::Count), -1);
        const std::vector<std::string> all = AutoMissingParts(AutoType::Step, false, map);
        CHECK(std::find(all.begin(), all.end(), "left heel") != all.end());
        CHECK(std::find(all.begin(), all.end(), "left knee") != all.end());
        CHECK(std::find(all.begin(), all.end(), "right toe end") != all.end());
        // A full Mixamo-like mapping: nothing missing for any type.
        for (int r = 0; r < static_cast<int>(Role::Count); ++r) map[static_cast<size_t>(r)] = r;
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
        CHECK(!OfBlock(DetectAutoEvents(blocks, rig, nullptr, false), 1).empty());
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

    // ---- Story 10-8c: hands and categories ---------------------------------------------------

    // Types, kinds and categories of the hands.
    {
        CHECK(AutoMarkerName(AutoKind::Grab, 'L') == "Grab L" && AutoMarkerName(AutoKind::Release, 'R') == "Release R");
        CHECK(std::string(AutoKindWord(AutoKind::Grab)) == "grab" && std::string(AutoKindWord(AutoKind::Release)) == "release");
        CHECK(std::string(AutoTypeLabel(AutoType::Grab)) == "Grab" && std::string(AutoTypeLabel(AutoType::Release)) == "Release");
        CHECK(AutoTypeOfKind(AutoKind::Grab) == AutoType::Grab && AutoTypeOfKind(AutoKind::Release) == AutoType::Release);
        for (int t = 0; t < kAutoTypeCount; ++t) {
            const bool hand = t == static_cast<int>(AutoType::Grab) || t == static_cast<int>(AutoType::Release) ||
                              t == static_cast<int>(AutoType::HandPivot);
            CHECK(AutoCategoryOf(static_cast<AutoType>(t)) == (hand ? AutoCategory::Hands : AutoCategory::Feet));
        }
        CHECK(std::string(AutoCategoryLabel(AutoCategory::Feet)) == "Feet" &&
              std::string(AutoCategoryLabel(AutoCategory::Hands)) == "Hands");
        // Every kind's default colour pair differs from the others'.
        for (int k = 0; k < kAutoKindCount; ++k)
            for (int j = k + 1; j < kAutoKindCount; ++j) {
                if (static_cast<AutoKind>(k) == AutoKind::Step && static_cast<AutoKind>(j) == AutoKind::Heel) continue;  // FS = Heel's
                if (static_cast<AutoKind>(k) == AutoKind::HandPivot && static_cast<AutoKind>(j) == AutoKind::HandPivotAny)
                    continue;  // one marker name, one colour
                CHECK(AutoDefaultColor(static_cast<AutoKind>(k), 'L') != AutoDefaultColor(static_cast<AutoKind>(j), 'L'));
            }
    }

    // Categories: a new item, + -> Hands, tick Grab, Detect: Hands shown, Feet absent; Grab L / R.
    {
        std::vector<Block> blocks;
        CHECK(!AutoCategoryInBlocks(blocks, AutoCategory::Feet) && !AutoCategoryInBlocks(blocks, AutoCategory::Hands));
        AutoSettings ui = ReadAutoSettings(blocks);
        ui.type[static_cast<int>(AutoType::Grab)].on = true;
        CHECK(AutoCategoryOnCount(ui, AutoCategory::Hands) == 1 && AutoCategoryOnCount(ui, AutoCategory::Feet) == 0);
        CHECK(AutoPending(ui, ReadAutoSettings(blocks)));
        bool changed = false;
        ApplyAutoSettings(blocks, ui, &changed);
        CHECK(changed && Markers(blocks) == (std::vector<std::string>{"Grab L", "Grab R"}));
        CHECK(AutoCategoryInBlocks(blocks, AutoCategory::Hands) && !AutoCategoryInBlocks(blocks, AutoCategory::Feet));
        CHECK(ReadAutoSettings(blocks) == ui && !AutoPending(ui, ReadAutoSettings(blocks)));
        // An auto block of an unknown type belongs to no category.
        Block future;
        future.auto_type = "clap";
        CHECK(!AutoCategoryInBlocks({future}, AutoCategory::Hands) && !AutoCategoryInBlocks({future}, AutoCategory::Feet));
    }

    // Remove category: x on Feet with Step on (and Grab on), Detect: the step blocks go with their
    // edits; the Grab blocks and theirs stay.
    {
        std::vector<Block> blocks;
        ApplyAutoSettings(blocks, Ticked(true, false, false, false, false, true));  // FS L, FS R, Grab L, Grab R
        CHECK(Markers(blocks) == (std::vector<std::string>{"FS L", "FS R", "Grab L", "Grab R"}));
        std::vector<EventEntry> ev = {MakeUserEvent(0, 0.5, 0, 0), MakeSuppression(1, 1.0), MakeUserEvent(2, 1.5, 0, 0),
                                      MakeSuppression(3, 2.0)};
        AutoSettings ui = ReadAutoSettings(blocks);
        CHECK(AutoCategoryOnCount(ui, AutoCategory::Feet) == 1);
        UntickAutoCategory(ui, AutoCategory::Feet);
        CHECK(AutoCategoryOnCount(ui, AutoCategory::Feet) == 0 && AutoCategoryOnCount(ui, AutoCategory::Hands) == 1);
        // Until Detect, the item still has its step blocks: Feet stays shown ("0 on").
        CHECK(AutoCategoryInBlocks(blocks, AutoCategory::Feet) && AutoPending(ui, ReadAutoSettings(blocks)));
        const std::vector<int> map = ApplyAutoSettings(blocks, ui);
        CHECK(map == (std::vector<int>{-1, -1, 0, 1}));
        CHECK(Markers(blocks) == (std::vector<std::string>{"Grab L", "Grab R"}));
        CHECK(!AutoCategoryInBlocks(blocks, AutoCategory::Feet) && AutoCategoryInBlocks(blocks, AutoCategory::Hands));
        RemapEventBlocks(ev, map);
        CHECK(ev.size() == 2);
        if (ev.size() == 2) CHECK(ev[0].block == 0 && ev[0].t == 1.5 && ev[1].block == 1 && ev[1].t == 2.0);
    }

    // Grab and release end to end: the right hand moves at 1.5 leg/s, rests from 1.0 to 1.5 s.
    // Grab R at 1.0, Release R at 1.5, on their blocks; speed = strength x leg; offset added.
    {
        Options o;
        o.dur = 2.5;
        o.hand[1] = HandGrab(0.3, 1.5 * 0.87, 1.0, 1.5);
        const std::vector<BoneTrack> rig = Rig(Planted(-0.1, 0.0), Planted(0.1, 0.2), o);
        std::vector<Block>           blocks = AutoBlocks(Ticked(false, false, false, false, false, true, true));
        CHECK(Markers(blocks) == (std::vector<std::string>{"Grab L", "Grab R", "Release L", "Release R"}));
        const std::vector<Event> ev = DetectAutoEvents(blocks, rig, nullptr, false);
        CHECK(ev.size() == 2);
        CHECK(OfBlock(ev, 0).empty() && OfBlock(ev, 2).empty());  // no left hand bone
        const std::vector<Event> grab = OfBlock(ev, 1), rel = OfBlock(ev, 3);
        CHECK(grab.size() == 1 && rel.size() == 1);
        if (grab.size() == 1 && rel.size() == 1) {
            CHECK(std::fabs(grab[0].time_s - 1.0) < 0.02 && grab[0].marker == "Grab R");
            CHECK(std::fabs(rel[0].time_s - 1.5) < 0.02 && rel[0].marker == "Release R");
            CHECK(grab[0].strength > 1.3 && std::fabs(grab[0].speed - grab[0].strength * 0.87) < 1e-9);
        }
        std::vector<Block> later = blocks;
        for (Block& b : later)
            if (b.auto_type == "grab") b.offset_ms = 25.0;  // as the section's field writes it
        const std::vector<Event> grab25 = OfBlock(DetectAutoEvents(later, rig, nullptr, false), 1);
        CHECK(grab25.size() == 1 && grab.size() == 1);
        if (grab25.size() == 1 && grab.size() == 1) CHECK(std::fabs(grab25[0].time_s - (grab[0].time_s + 0.025)) < 1e-12);
        // Hand blocks next to foot blocks: the feet's events are unchanged by the hand ones.
        std::vector<Block> both = AutoBlocks(Ticked(true, true, false, false, false, true, true));
        std::vector<Block> feet = AutoBlocks(Ticked(true, true, false, false));
        const std::vector<Event> eb = DetectAutoEvents(both, rig, nullptr, false), ef = DetectAutoEvents(feet, rig, nullptr, false);
        std::vector<Event>       eb_feet;
        for (const Event& e : eb)
            if (e.block < 4) eb_feet.push_back(e);
        CHECK(SameEvents(eb_feet, ef));
    }

    // Sensitivity: grab and release move the hand contact speed only; one analysis per set.
    {
        const PhysicsParams d;
        const PhysicsParams g = AutoPhysicsParams(AutoType::Grab, 100.0, false), r = AutoPhysicsParams(AutoType::Release, 0.0, false);
        CHECK(g.hand_contact_speed == 2.0 * d.hand_contact_speed && r.hand_contact_speed == 0.5 * d.hand_contact_speed);
        CHECK(g.contact_speed == d.contact_speed && g.slide_speed == d.slide_speed && g.pivot_min_deg == d.pivot_min_deg);
        CHECK(AutoPhysicsParams(AutoType::Step, 100.0, false).hand_contact_speed == d.hand_contact_speed);
        CHECK(SameAutoAnalysis(AutoPhysicsParams(AutoType::Grab, 50.0, false), d) && !SameAutoAnalysis(g, d));
        std::vector<Block> blocks = AutoBlocks(Ticked(true, false, false, false, false, true, true));
        CHECK(AutoParamSets(blocks, false).size() == 1);
        for (Block& b : blocks)
            if (b.auto_type == "grab") b.sens = 70.0;
        CHECK(AutoParamSets(blocks, false).size() == 2);
        // A less sensitive grab (a slower contact speed) still finds the 1.5 leg/s landing, at or
        // after the default's time; the strength stays above the min approach.
        Options o;
        o.dur = 2.5;
        o.hand[1] = HandGrab(0.3, 1.5 * 0.87, 1.0, 1.5);
        const std::vector<BoneTrack> rig = Rig(Planted(-0.1, 0.0), Planted(0.1, 0.2), o);
        std::vector<Block>           gb = AutoBlocks(Ticked(false, false, false, false, false, true));  // Grab L, Grab R
        const std::vector<Event>     at50 = OfBlock(DetectAutoEvents(gb, rig, nullptr, false), 1);
        for (Block& b : gb) b.sens = 25.0;
        const std::vector<Event> at25 = OfBlock(DetectAutoEvents(gb, rig, nullptr, false), 1);
        CHECK(at50.size() == 1 && at25.size() == 1);
        if (at50.size() == 1 && at25.size() == 1) CHECK(at25[0].time_s >= at50[0].time_s);
    }

    // 10-8b fb-1: an auto block whose own part has no bone (its earlier markers are kept).
    {
        std::vector<int> map(static_cast<size_t>(Role::Count), -1);
        for (int r = 0; r < static_cast<int>(Role::Count); ++r) map[static_cast<size_t>(r)] = r;
        auto blk = [](AutoKind k, char side) { return MakeAutoBlock(k, side, kAutoDefaultSens, 0.0); };
        for (int k = 0; k < kAutoKindCount; ++k)
            for (char side : {'L', 'R'}) CHECK(!AutoBlockPartMissing(blk(static_cast<AutoKind>(k), side), map));
        map[static_cast<size_t>(Role::LeftHand)] = -1;
        CHECK(AutoBlockPartMissing(blk(AutoKind::Grab, 'L'), map) && AutoBlockPartMissing(blk(AutoKind::Release, 'L'), map));
        CHECK(!AutoBlockPartMissing(blk(AutoKind::Grab, 'R'), map));
        // No right toe nor toe end: the toe and the pivot miss it, the heel and a combined step do not.
        map[static_cast<size_t>(Role::RightToe)] = map[static_cast<size_t>(Role::RightToeEnd)] = -1;
        CHECK(AutoBlockPartMissing(blk(AutoKind::Toe, 'R'), map) && AutoBlockPartMissing(blk(AutoKind::Pivot, 'R'), map));
        CHECK(!AutoBlockPartMissing(blk(AutoKind::Heel, 'R'), map) && !AutoBlockPartMissing(blk(AutoKind::Step, 'R'), map));
        CHECK(!AutoBlockPartMissing(blk(AutoKind::Toe, 'L'), map));
        // No right heel either: every right foot kind misses its part.
        map[static_cast<size_t>(Role::RightHeel)] = -1;
        for (AutoKind k : {AutoKind::Step, AutoKind::Heel, AutoKind::Lift, AutoKind::Slide})
            CHECK(AutoBlockPartMissing(blk(k, 'R'), map));
        // An off block, a rule, an unknown type: never.
        Block off = blk(AutoKind::Step, 'R');
        off.enabled = false;
        Block unknown = blk(AutoKind::Step, 'R');
        unknown.auto_type = "kick";
        CHECK(!AutoBlockPartMissing(off, map) && !AutoBlockPartMissing(Block{}, map) && !AutoBlockPartMissing(unknown, map));
        CHECK((AutoBlocksPartMissing({Block{}, blk(AutoKind::Step, 'R'), blk(AutoKind::Step, 'L')}, map) ==
               std::vector<char>{0, 1, 0}));
        // No body scale: every kind misses.
        std::vector<int> none(static_cast<size_t>(Role::Count), -1);
        none[static_cast<size_t>(Role::LeftHand)] = 5;
        CHECK(AutoBlockPartMissing(blk(AutoKind::Grab, 'L'), none));
    }

    // fb-1: only the right heel unmapped (toe mapped): the heel and the pivot miss it, a combined
    // step, a lift-off and a slide do not (the toe stands in).
    {
        std::vector<int> map(static_cast<size_t>(Role::Count), -1);
        for (int r = 0; r < static_cast<int>(Role::Count); ++r) map[static_cast<size_t>(r)] = r;
        map[static_cast<size_t>(Role::RightHeel)] = -1;
        auto blk = [](AutoKind k, char side) { return MakeAutoBlock(k, side, kAutoDefaultSens, 0.0); };
        CHECK(AutoBlockPartMissing(blk(AutoKind::Heel, 'R'), map) && AutoBlockPartMissing(blk(AutoKind::Pivot, 'R'), map));
        for (AutoKind k : {AutoKind::Step, AutoKind::Lift, AutoKind::Slide, AutoKind::Toe})
            CHECK(!AutoBlockPartMissing(blk(k, 'R'), map));
        CHECK(!AutoBlockPartMissing(blk(AutoKind::Heel, 'L'), map));
    }

    // fb-1: AutoBlockPartMissing agrees with AutoMissingParts, every kind x side x several maps: a
    // block misses when there is no body scale, or when the section lists one of its side's parts.
    {
        auto full = [] {
            std::vector<int> m(static_cast<size_t>(Role::Count), -1);
            for (int r = 0; r < static_cast<int>(Role::Count); ++r) m[static_cast<size_t>(r)] = r;
            return m;
        };
        auto without = [&](std::initializer_list<Role> gone) {
            std::vector<int> m = full();
            for (Role r : gone) m[static_cast<size_t>(r)] = -1;
            return m;
        };
        std::vector<std::vector<int>> maps = {
            full(),
            without({Role::RightHeel}),
            without({Role::LeftToe, Role::LeftToeEnd}),
            without({Role::RightHeel, Role::RightToe, Role::RightToeEnd}),
            without({Role::LeftToe}),
            without({Role::LeftHand}),
            without({Role::RightHand, Role::LeftHeel}),
            without({Role::LeftHeel, Role::RightHeel, Role::LeftToe, Role::RightToe, Role::LeftToeEnd, Role::RightToeEnd}),
            without({Role::LeftKnee, Role::RightKnee}),  // no body scale
            std::vector<int>(static_cast<size_t>(Role::Count), -1),
        };
        for (const std::vector<int>& map : maps) {
            // No body scale <=> the hand types miss something with both hands mapped.
            std::vector<int> hands = map;
            hands[static_cast<size_t>(Role::LeftHand)] = static_cast<int>(Role::LeftHand);
            hands[static_cast<size_t>(Role::RightHand)] = static_cast<int>(Role::RightHand);
            const bool no_scale = !AutoMissingParts(AutoType::Grab, false, hands).empty();
            for (int ki = 0; ki < kAutoKindCount; ++ki) {
                const AutoKind k = static_cast<AutoKind>(ki);
                const bool     sep = k == AutoKind::Heel || k == AutoKind::Toe;
                const std::vector<std::string> listed = AutoMissingParts(AutoTypeOfKind(k), sep, map);
                for (char side : {'L', 'R'}) {
                    std::vector<Role> parts;
                    const Role heel = FootPartRole(side, 0), toe = FootPartRole(side, 1), tip = FootPartRole(side, 2);
                    switch (k) {
                    case AutoKind::Heel: parts = {heel}; break;
                    case AutoKind::Toe: parts = {toe}; break;
                    case AutoKind::Pivot: parts = {heel, toe}; break;
                    case AutoKind::Step:
                    case AutoKind::Lift:
                    case AutoKind::Slide: parts = {heel, toe, tip}; break;
                    default: parts = {side == 'R' ? Role::RightHand : Role::LeftHand}; break;
                    }
                    bool expect = no_scale;
                    for (Role r : parts)
                        if (std::find(listed.begin(), listed.end(), RoleName(r)) != listed.end()) expect = true;
                    const bool got = AutoBlockPartMissing(MakeAutoBlock(k, side, kAutoDefaultSens, 0.0), map);
                    if (got != expect) std::printf("  kind %d side %c: %d, expected %d\n", ki, side, got, expect);
                    CHECK(got == expect);
                }
            }
        }
    }

    // No hand bone: no Grab / Release L, and the section names "left hand" (the right hand's
    // events stay). A rig without feet but with legs still finds them.
    {
        std::vector<int> map(static_cast<size_t>(Role::Count), -1);
        for (int r = 0; r < static_cast<int>(Role::Count); ++r) map[static_cast<size_t>(r)] = r;
        map[static_cast<size_t>(Role::LeftHand)] = -1;
        CHECK(AutoMissingParts(AutoType::Grab, false, map) == std::vector<std::string>{"left hand"});
        CHECK(AutoMissingParts(AutoType::Release, false, map) == std::vector<std::string>{"left hand"});
        CHECK(AutoMissingParts(AutoType::Step, false, map).empty());
        // No foot bone: the thigh gives the body scale, so the hands need no foot.
        for (Role r : {Role::LeftHeel, Role::RightHeel, Role::LeftToe, Role::RightToe, Role::LeftToeEnd, Role::RightToeEnd})
            map[static_cast<size_t>(r)] = -1;
        CHECK(AutoMissingParts(AutoType::Grab, false, map) == std::vector<std::string>{"left hand"});
        CHECK(!AutoMissingParts(AutoType::Step, false, map).empty());
        // Heels gone but toes, knees and up legs mapped: combined steps miss nothing (the thigh
        // gives the scale).
        {
            std::vector<int> toes(static_cast<size_t>(Role::Count), -1);
            for (int r = 0; r < static_cast<int>(Role::Count); ++r) toes[static_cast<size_t>(r)] = r;
            toes[static_cast<size_t>(Role::LeftHeel)] = toes[static_cast<size_t>(Role::RightHeel)] = -1;
            toes[static_cast<size_t>(Role::Hips)] = -1;
            CHECK(AutoMissingParts(AutoType::Step, false, toes).empty());
        }
        // Neither the heel route nor the thigh: the body scale's roles, then the hand.
        map[static_cast<size_t>(Role::LeftKnee)] = map[static_cast<size_t>(Role::RightKnee)] = -1;
        const std::vector<std::string> all = AutoMissingParts(AutoType::Grab, false, map);
        CHECK(!all.empty() && all.back() == "left hand" &&
              std::find(all.begin(), all.end(), "left knee") != all.end());

        Options o;
        o.dur = 2.5;
        o.heel = false;
        o.ball = false;
        o.hand[1] = HandGrab(0.3, 1.5 * 0.87, 1.0, 1.5);
        std::vector<BoneTrack> rig = Rig(Planted(-0.1, 0.0), Planted(0.1, 0.2), o);
        for (Role r : {Role::LeftToeEnd, Role::RightToeEnd}) rig[static_cast<size_t>(r)] = BoneTrack{};
        const std::vector<Block> blocks = AutoBlocks(Ticked(true, false, false, false, false, true));  // FS L/R, Grab L/R
        const std::vector<Event> ev = DetectAutoEvents(blocks, rig, nullptr, false);
        CHECK(OfBlock(ev, 0).empty() && OfBlock(ev, 1).empty() && OfBlock(ev, 2).empty());
        CHECK(OfBlock(ev, 3).size() == 1);
    }

    // Grab / Release blocks round-trip in the record, reach a pooled copy, and a REAPER-side edit
    // of their marker finds its block.
    {
        ItemRules r;
        r.blocks = {HeelRule("Rule")};
        ApplyAutoSettings(r.blocks, Ticked(false, false, false, false, false, true, true));
        r.blocks[1].sens = 70;
        r.blocks[4].offset_ms = 12;
        r.events = {MakeUserEvent(2, 1.25, 0, 0)};
        const std::string text = SerializeItemRules(r);
        CHECK(text.find("block auto=grab side=L sens=70 ") != std::string::npos);
        CHECK(text.find("block auto=release side=R sens=50 ") != std::string::npos);
        CHECK(text.find("marker=Release R") != std::string::npos);
        ItemRules back;
        CHECK(ParseItemRules(text, &back) && !HasKeptText(back));
        CHECK(BlocksEqual(back.blocks, r.blocks) && SerializeItemRules(back) == text);
        CHECK(ReadAutoSettings(back.blocks) == ReadAutoSettings(r.blocks));
        ItemRules copy;
        CopyPoolContent(back, copy);
        CHECK(BlocksEqual(copy.blocks, r.blocks));
        MarkerEdit e;
        e.kind = MarkerEditKind::Delete;
        e.c = 1.25;
        e.name = "Grab R";
        e.color = r.blocks[2].color;
        e.has_color = true;
        CHECK(MarkerEditBlock(back, e) == 2);
    }

    // ---- Story 10-8e: hand pivots -------------------------------------------------------------

    // The type, its two kinds (after a grab, any resting hand), names, category.
    {
        const int hp = static_cast<int>(AutoType::HandPivot);
        CHECK(std::string(AutoTypeLabel(AutoType::HandPivot)) == "Pivot scuff");
        CHECK(AutoCategoryOf(AutoType::HandPivot) == AutoCategory::Hands);
        CHECK(std::string(AutoKindWord(AutoKind::HandPivot)) == "hand_pivot" &&
              std::string(AutoKindWord(AutoKind::HandPivotAny)) == "hand_pivot_any");
        CHECK(AutoTypeOfKind(AutoKind::HandPivot) == AutoType::HandPivot &&
              AutoTypeOfKind(AutoKind::HandPivotAny) == AutoType::HandPivot);
        CHECK(AutoMarkerName(AutoKind::HandPivot, 'L') == "Hand Pivot L" && AutoMarkerName(AutoKind::HandPivotAny, 'R') == "Hand Pivot R");
        // After a grab by default: hand_pivot blocks; any resting hand: hand_pivot_any.
        AutoSettings s;
        CHECK(!s.hand_pivot_any);
        s.type[hp].on = true;
        std::vector<Block> blocks = AutoBlocks(s);
        CHECK(blocks.size() == 2 && blocks[0].auto_type == "hand_pivot" && blocks[1].auto_type == "hand_pivot" &&
              blocks[0].auto_side == 'L' && blocks[1].auto_side == 'R');
        CHECK(ReadAutoSettings(blocks) == s && AutoCategoryOnCount(s, AutoCategory::Hands) == 1 &&
              AutoCategoryOnCount(s, AutoCategory::Feet) == 0 && AutoCategoryInBlocks(blocks, AutoCategory::Hands));
        // Switching to any resting hand waits for Detect, and keeps the user's edits on each hand.
        std::vector<EventEntry> ev = {MakeUserEvent(0, 0.5, 0, 0), MakeSuppression(1, 1.0)};
        AutoSettings            any = s;
        any.hand_pivot_any = true;
        CHECK(AutoPending(any, ReadAutoSettings(blocks)));
        std::vector<int> map = ApplyAutoSettings(blocks, any);
        CHECK(blocks.size() == 2 && blocks[0].auto_type == "hand_pivot_any" && blocks[1].auto_type == "hand_pivot_any");
        CHECK(blocks[0].marker == "Hand Pivot L" && blocks[1].auto_side == 'R');
        CHECK(map == (std::vector<int>{0, 1}));
        RemapEventBlocks(ev, map);
        CHECK(ev.size() == 2 && ev[0].block == 0 && ev[1].block == 1);
        CHECK(ReadAutoSettings(blocks) == any && !AutoPending(any, ReadAutoSettings(blocks)));
        // The switch alone, the type unticked, waits for nothing.
        AutoSettings off = ReadAutoSettings({}), off_any = off;
        off_any.hand_pivot_any = true;
        CHECK(!AutoPending(off_any, off));
        // Back to after a grab.
        map = ApplyAutoSettings(blocks, s);
        CHECK(blocks.size() == 2 && blocks[0].auto_type == "hand_pivot" && map == (std::vector<int>{0, 1}));
        // Both kinds on one item (an edited record): after a grab wins, as combined steps do.
        std::vector<Block> mixed = {MakeAutoBlock(AutoKind::HandPivot, 'L', 50, 0), MakeAutoBlock(AutoKind::HandPivotAny, 'R', 50, 0)};
        CHECK(!ReadAutoSettings(mixed).hand_pivot_any);
        // The category's x unticks it.
        AutoSettings u = s;
        UntickAutoCategory(u, AutoCategory::Hands);
        CHECK(!u.type[hp].on && AutoCategoryOnCount(u, AutoCategory::Hands) == 0);
        // Missing parts: the hand only.
        std::vector<int> roles(static_cast<size_t>(Role::Count), -1);
        for (int r = 0; r < static_cast<int>(Role::Count); ++r) roles[static_cast<size_t>(r)] = r;
        roles[static_cast<size_t>(Role::LeftHand)] = -1;
        roles[static_cast<size_t>(Role::LeftToe)] = roles[static_cast<size_t>(Role::LeftToeEnd)] = -1;
        CHECK(AutoMissingParts(AutoType::HandPivot, false, roles) == std::vector<std::string>{"left hand"});
    }

    // Switching Only after a Grab keeps each hand pivot block where it is, with the user's marker,
    // colour and on/off; only its kind changes. Other blocks are untouched.
    {
        AutoSettings s = Ticked(false, false, false, false, false, true);  // Grab L, Grab R
        s.type[static_cast<int>(AutoType::HandPivot)].on = true;
        std::vector<Block> blocks = {HeelRule("Rule")};
        ApplyAutoSettings(blocks, s);  // Rule, Grab L, Grab R, Hand Pivot L, Hand Pivot R
        CHECK(Markers(blocks) == (std::vector<std::string>{"Rule", "Grab L", "Grab R", "Hand Pivot L", "Hand Pivot R"}));
        blocks.push_back(HeelRule("Last"));  // a rule after the hand pivots
        blocks[3].marker = "Palm Twist L";
        blocks[3].color = 0x1123456u;
        blocks[3].enabled = false;
        const Block before = blocks[3];
        AutoSettings any = s;
        any.hand_pivot_any = true;
        bool                   changed = false;
        const std::vector<int> map = ApplyAutoSettings(blocks, any, &changed);
        CHECK(changed && map == (std::vector<int>{0, 1, 2, 3, 4, 5}));
        CHECK(Markers(blocks) == (std::vector<std::string>{"Rule", "Grab L", "Grab R", "Palm Twist L", "Hand Pivot R", "Last"}));
        CHECK(blocks[3].auto_type == "hand_pivot_any" && blocks[4].auto_type == "hand_pivot_any");
        CHECK(blocks[3].color == before.color && !blocks[3].enabled && blocks[3].auto_side == 'L');
        CHECK(ReadAutoSettings(blocks).hand_pivot_any);
        // And back.
        ApplyAutoSettings(blocks, s);
        CHECK(blocks[3].auto_type == "hand_pivot" && blocks[3].marker == "Palm Twist L" && blocks[3].color == before.color &&
              !blocks[3].enabled && blocks.size() == 6 && blocks[5].marker == "Last");
    }

    // Sensitivity: the hand pivot's angle and rate thresholds only.
    {
        const PhysicsParams d;
        const PhysicsParams hi = AutoPhysicsParams(AutoType::HandPivot, 100.0, false), lo = AutoPhysicsParams(AutoType::HandPivot, 0.0, false);
        CHECK(hi.hand_pivot_min_deg == 0.5 * d.hand_pivot_min_deg && hi.hand_pivot_rate_dps == 0.5 * d.hand_pivot_rate_dps);
        CHECK(lo.hand_pivot_min_deg == 2.0 * d.hand_pivot_min_deg && lo.hand_pivot_rate_dps == 2.0 * d.hand_pivot_rate_dps);
        CHECK(hi.pivot_min_deg == d.pivot_min_deg && hi.pivot_rate_dps == d.pivot_rate_dps &&
              hi.hand_contact_speed == d.hand_contact_speed && hi.contact_speed == d.contact_speed);
        CHECK(AutoPhysicsParams(AutoType::Pivot, 100.0, false).hand_pivot_min_deg == d.hand_pivot_min_deg);
        CHECK(SameAutoAnalysis(AutoPhysicsParams(AutoType::HandPivot, 50.0, false), d) && !SameAutoAnalysis(hi, d));
        PhysicsParams rate_only = d;
        rate_only.hand_pivot_rate_dps = 45.0;
        CHECK(!SameAutoAnalysis(rate_only, d));
    }

    // End to end: the right hand rests 1.0 to 1.6 s and turns 90 deg in 250 ms from 1.175 s.
    // Landed with a grab: one Hand Pivot R with either kind; placed gently (0.7 leg/s): none after
    // a grab, one with any resting hand. Speed 0, strength in degrees, the offset added.
    {
        for (double speed : {1.5, 0.7}) {
            Options o;
            o.dur = 2.5;
            o.hand[1] = HandGrab(0.3, speed * 0.87, 1.0, 1.6);
            o.hand_yaw[1] = [](double t) { return 0.5 * kPi * Ease((t - 1.175) / 0.25); };
            const std::vector<BoneTrack> rig = Rig(Planted(-0.1, 0.0), Planted(0.1, 0.2), o);
            AutoSettings                 s;
            s.type[static_cast<int>(AutoType::HandPivot)].on = true;
            AutoSettings any = s;
            any.hand_pivot_any = true;
            const std::vector<Event> strict = DetectAutoEvents(AutoBlocks(s), rig, nullptr, false);
            const std::vector<Event> lax = DetectAutoEvents(AutoBlocks(any), rig, nullptr, false);
            CHECK(OfBlock(strict, 0).empty() && OfBlock(lax, 0).empty());  // no left hand bone
            CHECK(strict.size() == (speed > 1.0 ? 1u : 0u) && lax.size() == 1);
            for (const Event& e : lax) {
                CHECK(e.block == 1 && e.marker == "Hand Pivot R" && e.speed == 0.0);
                CHECK(e.strength > 80.0 && e.strength < 95.0 && e.time_s > 1.1 && e.time_s < 1.2);
            }
            any.type[static_cast<int>(AutoType::HandPivot)].offset_ms = 30.0;
            const std::vector<Event> later = DetectAutoEvents(AutoBlocks(any), rig, nullptr, false);
            CHECK(later.size() == 1 && lax.size() == 1);
            if (later.size() == 1 && lax.size() == 1) CHECK(std::fabs(later[0].time_s - (lax[0].time_s + 0.03)) < 1e-12);
            // Next to Grab blocks: the grab events are the same with or without hand pivots.
            AutoSettings g = Ticked(false, false, false, false, false, true), gp = g;
            gp.type[static_cast<int>(AutoType::HandPivot)].on = true;
            std::vector<Event> eg = DetectAutoEvents(AutoBlocks(g), rig, nullptr, false), egp = DetectAutoEvents(AutoBlocks(gp), rig, nullptr, false);
            egp.erase(std::remove_if(egp.begin(), egp.end(), [](const Event& e) { return e.block >= 2; }), egp.end());
            CHECK(SameEvents(eg, egp));
        }
    }

    // Hand pivot blocks round-trip in the record unchanged, with an unknown auto word beside them.
    {
        ItemRules    r;
        AutoSettings s;
        s.type[static_cast<int>(AutoType::HandPivot)].on = true;
        s.type[static_cast<int>(AutoType::HandPivot)].sens = 70.0;
        r.blocks = {HeelRule("Rule")};
        ApplyAutoSettings(r.blocks, s);
        AutoSettings any = s;
        any.hand_pivot_any = true;
        std::vector<Block> lax_blocks = AutoBlocks(any);
        r.blocks.push_back(lax_blocks[0]);
        Block future;
        future.auto_type = "hand_slide";  // a newer RAV's
        future.auto_side = 'L';
        future.marker = "Hand Slide L";
        r.blocks.push_back(future);
        const std::string text = SerializeItemRules(r);
        CHECK(text.find("block auto=hand_pivot side=L sens=70 ") != std::string::npos);
        CHECK(text.find("block auto=hand_pivot_any side=L ") != std::string::npos);
        CHECK(text.find("block auto=hand_slide side=L ") != std::string::npos);
        ItemRules back;
        CHECK(ParseItemRules(text, &back) && !HasKeptText(back));
        CHECK(BlocksEqual(back.blocks, r.blocks) && SerializeItemRules(back) == text);
        CHECK(!AutoBlockKind(back.blocks.back(), nullptr, nullptr) && IsAutoBlock(back.blocks.back()));
        // Detect keeps the unknown block.
        ApplyAutoSettings(back.blocks, s);
        CHECK(back.blocks.size() == 4 && back.blocks.back().auto_type == "hand_slide");
        ItemRules copy;
        CopyPoolContent(back, copy);
        CHECK(BlocksEqual(copy.blocks, back.blocks));
    }

    // Story 10-8i: "Is looping" goes into every parameter set (its own analysis, cached apart);
    // the walk read as a loop gets its left lift-off at the seam (the foot leaves at t = 0 = 4 s),
    // once, every event in [0, T).
    {
        const PhysicsParams d;
        const PhysicsParams l = AutoPhysicsParams(AutoType::LiftOff, 50.0, true);
        CHECK(l.looping && !AutoPhysicsParams(AutoType::LiftOff, 50.0, false).looping && !SameAutoAnalysis(l, d));
        CHECK(SameAutoAnalysis(l, AutoPhysicsParams(AutoType::Step, 50.0, true)));
        std::vector<Block> blocks = AutoBlocks(Ticked(false, true, false, false));
        CHECK(Markers(blocks) == (std::vector<std::string>{"Lift L", "Lift R"}));
        for (const PhysicsParams& p : AutoParamSets(blocks, true)) CHECK(p.looping);
        for (const PhysicsParams& p : AutoParamSets(blocks, false)) CHECK(!p.looping);
        const std::vector<BoneTrack> rig = Walk();
        std::vector<AutoAnalysis>    cache;
        const std::vector<Event>     off = DetectAutoEvents(blocks, rig, &cache, false);
        const std::vector<Event>     on = DetectAutoEvents(blocks, rig, &cache, true);
        CHECK(cache.size() == 2);
        CHECK(SameEvents(off, DetectAutoEvents(blocks, rig, nullptr, false)) && SameEvents(on, DetectAutoEvents(blocks, rig, nullptr, true)));
        CHECK(OfBlock(off, 0).size() == 3 && OfBlock(on, 0).size() == 4);
        CHECK(OfBlock(off, 1).size() == OfBlock(on, 1).size());
        for (const Event& e : on) CHECK(e.time_s >= 0.0 && e.time_s < 4.0);
        // The search runs on the item's analysis: Lift L finds its events at loop on as off.
        std::vector<int> map(static_cast<size_t>(Role::Count), -1);
        for (int r = 0; r < static_cast<int>(Role::Count); ++r)
            if (!rig[static_cast<size_t>(r)].pos.empty()) map[static_cast<size_t>(r)] = r;
        const std::vector<AutoSensResult> res = SearchAutoSensitivity(blocks, rig, {0, 0}, map, true);
        CHECK(res.size() == 2 && !res[0].searched && !res[1].searched);
    }
    // Story 10-8i: a block offset wraps round the loop: the seam lift-off (just before 4 s) with
    // +20 ms lands near 0.02 s, every event in [0, T); off, it is pushed past the end.
    {
        const std::vector<BoneTrack> rig = Walk();
        std::vector<Block>           blocks = AutoBlocks(Ticked(false, true, false, false));  // Lift L, Lift R
        for (Block& b : blocks) b.offset_ms = 20.0;
        const std::vector<Event> on = OfBlock(DetectAutoEvents(blocks, rig, nullptr, true), 0);
        CHECK(on.size() == 4);
        for (const Event& e : on) CHECK(e.time_s >= 0.0 && e.time_s < 4.0);
        if (on.size() == 4) CHECK(on[0].time_s > 0.01 && on[0].time_s < 0.03);
        const std::vector<Event> off = OfBlock(DetectAutoEvents(blocks, rig, nullptr, false), 0);
        CHECK(off.size() == 3);
    }
    // Story 10-8i: the search runs on the item's analysis. Grab R's only grab crosses the seam
    // (the hand comes to rest 30 ms before the end, still at rest 210 ms from the start): none
    // off (searched), one on (not searched); Detect's apply step alike.
    {
        const HandFn loop_hand = [](double t) {
            Vec3d        p{0.3, 1.0, 0.3};
            const double a = 0.21, b = 1.97, v = 1.5 * 0.87;
            if (t <= a || t >= b) return p;
            const double side = (b - a) / 4.0, len = v * side, u = t - a;
            const int    k = std::min(3, static_cast<int>(u / side));
            const double d = (u - k * side) * v;
            const double corner[4][2] = {{0.0, 0.0}, {0.0, len}, {len, len}, {len, 0.0}};
            const double dir[4][2] = {{0.0, 1.0}, {1.0, 0.0}, {0.0, -1.0}, {-1.0, 0.0}};
            p.y += corner[k][0] + d * dir[k][0];
            p.z += corner[k][1] + d * dir[k][1];
            return p;
        };
        Options o;
        o.dur = 2.0;
        o.hand[1] = loop_hand;
        const std::vector<BoneTrack> rig = Rig(Planted(-0.1, 0.0), Planted(0.1, 0.2), o);
        std::vector<int>             map(static_cast<size_t>(Role::Count), -1);
        for (int r = 0; r < static_cast<int>(Role::Count); ++r)
            if (!rig[static_cast<size_t>(r)].pos.empty()) map[static_cast<size_t>(r)] = r;
        std::vector<Block> blocks = AutoBlocks(Ticked(false, false, false, false, false, true));  // Grab L, Grab R
        CHECK(Markers(blocks) == (std::vector<std::string>{"Grab L", "Grab R"}));
        CHECK(OfBlock(DetectAutoEvents(blocks, rig, nullptr, false), 1).empty());
        CHECK(OfBlock(DetectAutoEvents(blocks, rig, nullptr, true), 1).size() == 1);
        const std::vector<AutoSensResult> off = SearchAutoSensitivity(blocks, rig, {0, 0}, map, false);
        const std::vector<AutoSensResult> on = SearchAutoSensitivity(blocks, rig, {0, 0}, map, true);
        CHECK(off.size() == 2 && on.size() == 2);
        if (off.size() == 2 && on.size() == 2) CHECK(off[1].searched && !on[1].searched);
        std::vector<Block>          a = blocks;
        std::vector<AutoSensResult> ar;
        CHECK(!ApplyAutoSensSearch(a, rig, map, &ar, nullptr, true) && BlocksEqual(a, blocks));
        CHECK(ar.size() == 2 && !ar[1].searched);
        ApplyAutoSensSearch(a, rig, map, &ar, nullptr, false);
        CHECK(ar.size() == 2 && ar[1].searched);
    }

    if (g_fails) std::printf("%d check(s) failed\n", g_fails);
    else std::printf("auto_detect: all checks passed\n");
    return g_fails ? 1 : 0;
}
