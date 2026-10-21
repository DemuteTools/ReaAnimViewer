// SPDX-License-Identifier: MIT
//
// Host test of auto detection (src/auto_detect.h, Epic 10, story 10-8b; hands and categories:
// 10-8c): the specs' matrix rows that are pure (blocks, settings, Detect's remap, sensitivity,
// offset, the toe without a ball bone, missing roles, rules next to auto blocks, pooled copies,
// the record, categories, hand blocks; hand pivots: 10-8e). No REAPER.
//   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests

#include "auto_detect.h"
#include "auto_types.h"
#include "bone_roles.h"
#include "event_list.h"
#include "hand_touch.h"
#include "motion_physics.h"
#include "rule_record.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
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

// Story 10-9a: a foot planted at (x, 0, 0) that leaves at t0 and flies a ballistic arc of
// `flight` s (gravity 9.81 m/s^2), landing where it took off.
FootFn JumpFoot(double x, double t0, double flight)
{
    return [=](double t) {
        FootPose     f;
        const double u = t - t0;
        f.ball = Vec3d{x, 0.02, 0.0};
        if (u > 0.0 && u < flight) f.ball.y += 9.81 * flight / 2.0 * u - 0.5 * 9.81 * u * u;
        return f;
    };
}

// Story 10-9a: steps, then a jump, then steps. The foot at (x, 0, z) swings 0.4 s 0.6 m forward
// with a 10 cm lift from each time of `steps`, and both feet jump from t0 for `flight` s
// (ballistic, landing where they took off).
FootFn StepsAndJump(double x, std::vector<double> steps, double t0, double flight)
{
    return [=](double t) {
        FootPose f;
        f.ball = Vec3d{x, 0.02, 0.0};
        for (double s : steps) {
            const double u = (t - s) / 0.4;
            if (u <= 0.0) continue;
            f.ball.z += 0.6 * Ease(u);
            if (u < 1.0) f.ball.y += 0.10 * std::sin(kPi * u);
        }
        const double u = t - t0;
        if (u > 0.0 && u < flight) f.ball.y += 9.81 * flight / 2.0 * u - 0.5 * 9.81 * u * u;
        return f;
    };
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
            // Story 10-9a: a sideless kind has one colour.
            CHECK((AutoDefaultColor(static_cast<AutoKind>(k), 'L') != AutoDefaultColor(static_cast<AutoKind>(k), 'R')) !=
                  AutoKindSideless(static_cast<AutoKind>(k)));
        }
        CHECK(!AutoKindFromWord("future_kind", nullptr) && !AutoKindFromWord("", nullptr));
        Block rule = HeelRule("Step");
        CHECK(!IsAutoBlock(rule) && !AutoBlockKind(rule, nullptr, nullptr));
        Block future;
        future.auto_type = "future_kind";  // a type of a newer RAV: an auto block of no known kind
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

    // Story 10-10b: the strength in the marker names: given to new blocks, read back from the
    // first block, never pending (it applies at once), kept by a kept block and carried to the
    // blocks replacing a dropped one (combined -> separate).
    {
        AutoSettings s = Ticked(true, true, false, false);
        s.strength_name[static_cast<int>(AutoType::Step)] = StrengthName::Word;
        std::vector<Block> blocks;
        ApplyAutoSettings(blocks, s);
        CHECK(blocks.size() == 4 && blocks[0].strength_name == StrengthName::Word && blocks[1].strength_name == StrengthName::Word &&
              blocks[2].strength_name == StrengthName::Off);
        CHECK(ReadAutoSettings(blocks) == s);
        AutoSettings ui = ReadAutoSettings(blocks);
        ui.strength_name[static_cast<int>(AutoType::Step)] = StrengthName::Number;
        CHECK(!AutoPending(ui, ReadAutoSettings(blocks)));
        // Detect again: the kept blocks keep their own.
        std::vector<Block> again = blocks;
        ApplyAutoSettings(again, ReadAutoSettings(blocks));
        CHECK(BlocksEqual(again, blocks));
        // Steps switched to Separate: the heel and toe blocks take the dropped FS block's.
        AutoSettings sep = ReadAutoSettings(blocks);
        sep.separate_steps = true;
        sep.strength_name[static_cast<int>(AutoType::Step)] = StrengthName::Off;  // a stale UI value: the block's wins
        std::vector<Block> split = blocks;
        ApplyAutoSettings(split, sep);
        int heel_word = 0;
        for (const Block& b : split)
            if ((b.auto_type == "heel" || b.auto_type == "toe") && b.strength_name == StrengthName::Word) ++heel_word;
        CHECK(heel_word == 4);
    }

    // Separate steps: Heel L / R and Toe L / R rows.
    {
        std::vector<Block> blocks = AutoBlocks(Ticked(true, false, false, false, true));
        CHECK(Markers(blocks) == (std::vector<std::string>{"Heel L", "Heel R", "Toe L", "Toe R"}));
        CHECK(ReadAutoSettings(blocks).separate_steps);
    }

    // Decision 2026-10-12 (13-2 A): steps switched combined -> separate: the heel block replacing
    // FS L / FS R keeps its rid; the toe gets none (each rid goes to one block). Lift keeps its own.
    {
        std::vector<Block> blocks = AutoBlocks(Ticked(true, true, false, false));
        CHECK(Markers(blocks) == (std::vector<std::string>{"FS L", "FS R", "Lift L", "Lift R"}));
        blocks[0].rid = "000000000000000a";
        blocks[1].rid = "000000000000000b";
        blocks[2].rid = "000000000000000c";
        ApplyAutoSettings(blocks, Ticked(true, true, false, false, true));
        for (const Block& b : blocks) {
            if (b.marker == "Heel L") CHECK(b.rid == "000000000000000a");
            else if (b.marker == "Heel R") CHECK(b.rid == "000000000000000b");
            else if (b.marker == "Lift L") CHECK(b.rid == "000000000000000c");
            else CHECK(b.rid.empty());
        }
        // And back: the combined block takes the first dropped heel / toe block's rid.
        ApplyAutoSettings(blocks, Ticked(true, true, false, false));
        for (const Block& b : blocks) {
            if (b.marker == "FS L") CHECK(b.rid == "000000000000000a");
            if (b.marker == "FS R") CHECK(b.rid == "000000000000000b");
        }
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
        future.auto_type = "future_kind";
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
                              t == static_cast<int>(AutoType::HandPivot) || t == static_cast<int>(AutoType::Clap) ||
                              t == static_cast<int>(AutoType::HandOnBody) ||  // story 10-9d
                              t == static_cast<int>(AutoType::Swing) || t == static_cast<int>(AutoType::Punch);  // 10-9c
            // Story 10-9b: Elbow in Arms, Sit and Fall in Body.
            const bool arms = hand || t == static_cast<int>(AutoType::Elbow);
            const bool body = t == static_cast<int>(AutoType::Jolt) || t == static_cast<int>(AutoType::Spin) ||  // 10-9g, 10-9f
                              t == static_cast<int>(AutoType::Sit) || t == static_cast<int>(AutoType::Fall);
            CHECK(AutoCategoryOf(static_cast<AutoType>(t)) ==
                  (arms ? AutoCategory::Arms : body ? AutoCategory::Body : AutoCategory::Legs));
        }
        CHECK(std::string(AutoCategoryLabel(AutoCategory::Legs)) == "Legs" &&
              std::string(AutoCategoryLabel(AutoCategory::Arms)) == "Arms");
        // Every kind's default colour pair differs from the others'.
        for (int k = 0; k < kAutoKindCount; ++k)
            for (int j = k + 1; j < kAutoKindCount; ++j) {
                if (static_cast<AutoKind>(k) == AutoKind::Step && static_cast<AutoKind>(j) == AutoKind::Heel) continue;  // FS = Heel's
                if (static_cast<AutoKind>(k) == AutoKind::HandPivot && static_cast<AutoKind>(j) == AutoKind::HandPivotAny)
                    continue;  // one marker name, one colour
                // Story 10-8m: a heel scuff block takes its type's colour; a Longest part block its
                // Each part counterpart's.
                const AutoKind kk = static_cast<AutoKind>(k), jk = static_cast<AutoKind>(j);
                if ((kk == AutoKind::Slide && (jk == AutoKind::SlideHeel || jk == AutoKind::SlideHeelLongest)) ||
                    (kk == AutoKind::Pivot && (jk == AutoKind::PivotHeel || jk == AutoKind::PivotHeelLongest)) ||
                    (kk >= AutoKind::SlideHeel && kk <= AutoKind::PivotToe && j == k + 4))
                    continue;
                // Story 10-9a-3: a lift-off / jump / land heel block takes its type's colour.
                if ((kk == AutoKind::Lift && jk == AutoKind::LiftHeel) || (kk == AutoKind::Jump && jk == AutoKind::JumpHeel) ||
                    (kk == AutoKind::Land && jk == AutoKind::LandHeel))
                    continue;
                // Story 10-9c: a Start kind shares its Peak twin's colour; a kick heel block the kick's.
                const AutoKind pk = AutoKindRow(kk).sound_key, pj = AutoKindRow(jk).sound_key;
                if (pk == pj || (pk == AutoKind::Kick && pj == AutoKind::KickHeel) || (pk == AutoKind::KickHeel && pj == AutoKind::Kick))
                    continue;
                for (char side : {'L', 'R'})
                    CHECK(AutoDefaultColor(static_cast<AutoKind>(k), side) != AutoDefaultColor(static_cast<AutoKind>(j), side));
            }
    }

    // Categories: a new item, + -> Arms, tick Grab, Detect: Arms shown, Legs absent; Grab L / R.
    {
        std::vector<Block> blocks;
        CHECK(!AutoCategoryInBlocks(blocks, AutoCategory::Legs) && !AutoCategoryInBlocks(blocks, AutoCategory::Arms));
        AutoSettings ui = ReadAutoSettings(blocks);
        ui.type[static_cast<int>(AutoType::Grab)].on = true;
        CHECK(AutoCategoryOnCount(ui, AutoCategory::Arms) == 1 && AutoCategoryOnCount(ui, AutoCategory::Legs) == 0);
        CHECK(AutoPending(ui, ReadAutoSettings(blocks)));
        bool changed = false;
        ApplyAutoSettings(blocks, ui, &changed);
        CHECK(changed && Markers(blocks) == (std::vector<std::string>{"Grab L", "Grab R"}));
        CHECK(AutoCategoryInBlocks(blocks, AutoCategory::Arms) && !AutoCategoryInBlocks(blocks, AutoCategory::Legs));
        CHECK(ReadAutoSettings(blocks) == ui && !AutoPending(ui, ReadAutoSettings(blocks)));
        // An auto block of an unknown type belongs to no category.
        Block future;
        future.auto_type = "future_type";
        CHECK(!AutoCategoryInBlocks({future}, AutoCategory::Arms) && !AutoCategoryInBlocks({future}, AutoCategory::Legs));
    }

    // Remove category: x on Legs with Step on (and Grab on), Detect: the step blocks go with their
    // edits; the Grab blocks and theirs stay.
    {
        std::vector<Block> blocks;
        ApplyAutoSettings(blocks, Ticked(true, false, false, false, false, true));  // FS L, FS R, Grab L, Grab R
        CHECK(Markers(blocks) == (std::vector<std::string>{"FS L", "FS R", "Grab L", "Grab R"}));
        std::vector<EventEntry> ev = {MakeUserEvent(0, 0.5, 0, 0), MakeSuppression(1, 1.0), MakeUserEvent(2, 1.5, 0, 0),
                                      MakeSuppression(3, 2.0)};
        AutoSettings ui = ReadAutoSettings(blocks);
        CHECK(AutoCategoryOnCount(ui, AutoCategory::Legs) == 1);
        UntickAutoCategory(ui, AutoCategory::Legs);
        CHECK(AutoCategoryOnCount(ui, AutoCategory::Legs) == 0 && AutoCategoryOnCount(ui, AutoCategory::Arms) == 1);
        // Until Detect, the item still has its step blocks: Legs stays shown ("0 on").
        CHECK(AutoCategoryInBlocks(blocks, AutoCategory::Legs) && AutoPending(ui, ReadAutoSettings(blocks)));
        const std::vector<int> map = ApplyAutoSettings(blocks, ui);
        CHECK(map == (std::vector<int>{-1, -1, 0, 1}));
        CHECK(Markers(blocks) == (std::vector<std::string>{"Grab L", "Grab R"}));
        CHECK(!AutoCategoryInBlocks(blocks, AutoCategory::Legs) && AutoCategoryInBlocks(blocks, AutoCategory::Arms));
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
        unknown.auto_type = "future_kind";
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
            without({Role::Hips}),                        // story 10-9a: jump and land only
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
                const bool     sep = k == AutoKind::Heel || k == AutoKind::Toe || k == AutoKind::SlideHeel ||
                               k == AutoKind::SlideToe || k == AutoKind::SlideHeelLongest || k == AutoKind::SlideToeLongest ||
                               (k >= AutoKind::LiftHeel && k <= AutoKind::LandToe) ||  // story 10-9a-3: the lift-off / jump / land parts
                               k == AutoKind::KickHeel || k == AutoKind::KickToe ||  // story 10-9c: the kick parts
                               k == AutoKind::KickHeelStart || k == AutoKind::KickToeStart;
                const std::vector<std::string> listed = AutoMissingParts(AutoTypeOfKind(k), sep, map);
                for (char side : {'L', 'R'}) {
                    std::vector<Role> parts;
                    const Role heel = FootPartRole(side, 0), toe = FootPartRole(side, 1), tip = FootPartRole(side, 2);
                    switch (k) {
                    case AutoKind::Heel:
                    case AutoKind::SlideHeel:
                    case AutoKind::SlideHeelLongest: parts = {heel}; break;
                    case AutoKind::Toe:
                    case AutoKind::SlideToe:
                    case AutoKind::SlideToeLongest: parts = {toe}; break;
                    case AutoKind::Pivot:
                    case AutoKind::PivotHeel:
                    case AutoKind::PivotToe:
                    case AutoKind::PivotHeelLongest:
                    case AutoKind::PivotToeLongest: 
                    case AutoKind::Tap:   // story 10-9e: the heel and the toe, as a pivot
                    case AutoKind::Drag: parts = {heel, toe}; break;
                    case AutoKind::Step:
                    case AutoKind::Lift:
                    case AutoKind::Slide: parts = {heel, toe, tip}; break;
                    // Story 10-9a: the hips, or a foot with none of its parts (each foot's heel is
                    // listed only then).
                    case AutoKind::Jump:
                    case AutoKind::Land:
                    case AutoKind::Jolt: parts = {Role::Hips, Role::LeftHeel, Role::RightHeel}; break;  // 10-9g: as a jump
                    // Story 10-9a-3: the lift-off parts as the step parts; a jump / land part: the
                    // hips, its own part, or a foot with none (its heel and toe both listed, below).
                    case AutoKind::LiftHeel: parts = {heel}; break;
                    case AutoKind::LiftToe: parts = {toe}; break;
                    case AutoKind::JumpHeel:
                    case AutoKind::LandHeel: parts = {Role::Hips, heel}; break;
                    case AutoKind::JumpToe:
                    case AutoKind::LandToe: parts = {Role::Hips, toe}; break;
                    // Story 10-9b: its own part (sit: hips and chest; fall: the hips, listed only
                    // without a chest too), or no foot at all (each foot's heel listed only then).
                    case AutoKind::Knee:
                        parts = {side == 'R' ? Role::RightKnee : Role::LeftKnee, Role::LeftHeel, Role::RightHeel};
                        break;
                    case AutoKind::Elbow:
                        parts = {side == 'R' ? Role::RightElbow : Role::LeftElbow, Role::LeftHeel, Role::RightHeel};
                        break;
                    case AutoKind::Sit: parts = {Role::Hips, Role::Chest, Role::LeftHeel, Role::RightHeel}; break;
                    case AutoKind::Fall: parts = {Role::Hips, Role::LeftHeel, Role::RightHeel}; break;
                    // Story 10-9d: a clap needs both hands, a hand on body its hand (and a body
                    // segment, below).
                    case AutoKind::Clap: parts = {Role::LeftHand, Role::RightHand}; break;
                    // Story 10-9c: the whooshes: the torso (listed as the chest), then the kick's
                    // foot parts as a step's, or the hand.
                    case AutoKind::Kick:
                    case AutoKind::KickStart: parts = {Role::Chest, heel, toe, tip}; break;
                    case AutoKind::KickHeel:
                    case AutoKind::KickHeelStart: parts = {Role::Chest, heel}; break;
                    case AutoKind::KickToe:
                    case AutoKind::KickToeStart: parts = {Role::Chest, toe}; break;
                    case AutoKind::Swing:
                    case AutoKind::SwingStart:
                    case AutoKind::Punch:
                    case AutoKind::PunchStart: parts = {Role::Chest, side == 'R' ? Role::RightHand : Role::LeftHand}; break;
                    default: parts = {side == 'R' ? Role::RightHand : Role::LeftHand}; break;
                    }
                    auto is_listed = [&](Role r) {
                        return std::find(listed.begin(), listed.end(), RoleName(r)) != listed.end();
                    };
                    bool expect = no_scale || (k == AutoKind::HandOnBody && !HasBodySegment(map));
                    for (Role r : parts)
                        if (is_listed(r)) expect = true;
                    if (k == AutoKind::JumpHeel || k == AutoKind::JumpToe || k == AutoKind::LandHeel || k == AutoKind::LandToe)
                        for (char fs : {'L', 'R'})
                            if (is_listed(FootPartRole(fs, 0)) && is_listed(FootPartRole(fs, 1))) expect = true;
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
        CHECK(AutoCategoryOf(AutoType::HandPivot) == AutoCategory::Arms);
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
        CHECK(ReadAutoSettings(blocks) == s && AutoCategoryOnCount(s, AutoCategory::Arms) == 1 &&
              AutoCategoryOnCount(s, AutoCategory::Legs) == 0 && AutoCategoryInBlocks(blocks, AutoCategory::Arms));
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
        UntickAutoCategory(u, AutoCategory::Arms);
        CHECK(!u.type[hp].on && AutoCategoryOnCount(u, AutoCategory::Arms) == 0);
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
    // (the hand comes to rest 10 ms before the end, within the smoothing's held end so no arrival
    // at the clip end, story 10-8j; still at rest 210 ms from the start): none
    // off (searched), one on (not searched); Detect's apply step alike.
    {
        const HandFn loop_hand = [](double t) {
            Vec3d        p{0.3, 1.0, 0.3};
            const double a = 0.21, b = 1.99, v = 1.5 * 0.87;
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
    // Story 10-8j: the same hand coming to rest 30 ms before the end (7 samples, before the
    // smoothing's held end): off, an arrival at the clip end gives Grab R near 1.97 s and the
    // search has nothing to raise.
    {
        const HandFn arrive = [](double t) {
            const double v = 1.5 * 0.87, d = v * (std::min(t, 1.97) - 0.21);
            return t <= 0.21 ? Vec3d{0.3, 1.0, 0.3} : Vec3d{0.3, 1.0 - 0.6 * d, 0.3 + 0.8 * d};
        };
        Options o;
        o.dur = 2.0;
        o.hand[1] = arrive;
        const std::vector<BoneTrack> rig = Rig(Planted(-0.1, 0.0), Planted(0.1, 0.2), o);
        std::vector<int>             map(static_cast<size_t>(Role::Count), -1);
        for (int r = 0; r < static_cast<int>(Role::Count); ++r)
            if (!rig[static_cast<size_t>(r)].pos.empty()) map[static_cast<size_t>(r)] = r;
        const std::vector<Block> blocks = AutoBlocks(Ticked(false, false, false, false, false, true));
        const std::vector<Event> off = OfBlock(DetectAutoEvents(blocks, rig, nullptr, false), 1);
        CHECK(off.size() == 1);
        if (off.size() == 1) CHECK(std::fabs(off[0].time_s - 1.97) < 0.015);
        const std::vector<AutoSensResult> res = SearchAutoSensitivity(blocks, rig, {0, 0}, map, false);
        CHECK(res.size() == 2 && !res[1].searched);
    }

    // ---- Story 10-8m: scuffs by part ----------------------------------------------------------

    // Kinds, words, markers, colours.
    {
        CHECK(std::string(AutoKindWord(AutoKind::SlideHeel)) == "slide_heel" &&
              std::string(AutoKindWord(AutoKind::PivotToeLongest)) == "pivot_toe_longest");
        CHECK(AutoMarkerName(AutoKind::SlideHeel, 'L') == "Slide Heel L" && AutoMarkerName(AutoKind::PivotToe, 'R') == "Pivot Toe R");
        CHECK(AutoMarkerName(AutoKind::SlideToeLongest, 'L') == "Slide Toe L" &&
              AutoMarkerName(AutoKind::PivotHeelLongest, 'R') == "Pivot Heel R");
        CHECK(AutoTypeOfKind(AutoKind::SlideToeLongest) == AutoType::Slide && AutoTypeOfKind(AutoKind::PivotHeel) == AutoType::Pivot);
        CHECK(AutoDefaultColor(AutoKind::SlideHeel, 'L') == AutoDefaultColor(AutoKind::Slide, 'L'));
        CHECK(AutoDefaultColor(AutoKind::PivotToe, 'R') != AutoDefaultColor(AutoKind::Pivot, 'R'));
        CHECK(AutoDefaultColor(AutoKind::PivotToeLongest, 'R') == AutoDefaultColor(AutoKind::PivotToe, 'R'));
    }

    // Blocks, Read round trip and pending, per mode.
    {
        for (int sm = 0; sm < 3; ++sm)
            for (int pm = 0; pm < 3; ++pm) {
                AutoSettings s = Ticked(false, false, true, true);
                s.slide_parts = static_cast<ScuffParts>(sm);
                s.pivot_parts = static_cast<ScuffParts>(pm);
                const std::vector<Block> blocks = AutoBlocks(s);
                CHECK(blocks.size() == static_cast<size_t>(2 * ((sm ? 2 : 1) + (pm ? 2 : 1))));
                CHECK(ReadAutoSettings(blocks) == s);
                std::vector<Block> fresh;
                ApplyAutoSettings(fresh, s);
                CHECK(BlocksEqual(fresh, blocks));
            }
        AutoSettings s = Ticked(false, false, true, true);
        CHECK(Markers(AutoBlocks(s)) == (std::vector<std::string>{"Slide L", "Slide R", "Pivot L", "Pivot R"}));
        AutoSettings each = s;
        each.pivot_parts = ScuffParts::EachPart;
        CHECK(Markers(AutoBlocks(each)) ==
              (std::vector<std::string>{"Slide L", "Slide R", "Pivot Heel L", "Pivot Heel R", "Pivot Toe L", "Pivot Toe R"}));
        CHECK(AutoPending(each, s));
        AutoSettings off = Ticked(false, false, true, false);
        AutoSettings off_each = off;
        off_each.pivot_parts = ScuffParts::EachPart;
        CHECK(!AutoPending(off_each, off));  // a mode of an unticked type waits for nothing
    }

    // Combined -> Each part: edits go to the heel block; Each <-> Longest keeps each part's block
    // (marker, colour, on/off; only its kind changes); Separate -> Combined back to the combined.
    {
        AutoSettings       s = Ticked(false, true, true, false);  // Lift L, Lift R, Slide L, Slide R
        std::vector<Block> blocks = AutoBlocks(s);
        AutoSettings       each = s;
        each.slide_parts = ScuffParts::EachPart;
        std::vector<int> map = ApplyAutoSettings(blocks, each);
        CHECK(Markers(blocks) ==
              (std::vector<std::string>{"Lift L", "Lift R", "Slide Heel L", "Slide Heel R", "Slide Toe L", "Slide Toe R"}));
        CHECK(map == (std::vector<int>{0, 1, 2, 3}));
        blocks[4].marker = "Squeak L";
        blocks[4].color = 0x1123456u;
        blocks[4].enabled = false;
        AutoSettings longest = s;
        longest.slide_parts = ScuffParts::Longest;
        bool changed = false;
        map = ApplyAutoSettings(blocks, longest, &changed);
        CHECK(changed && map == (std::vector<int>{0, 1, 2, 3, 4, 5}));
        CHECK(blocks[4].marker == "Squeak L" && blocks[4].color == 0x1123456u && !blocks[4].enabled &&
              blocks[4].auto_type == "slide_toe_longest" && blocks[2].auto_type == "slide_heel_longest");
        CHECK(ReadAutoSettings(blocks).slide_parts == ScuffParts::Longest);
        map = ApplyAutoSettings(blocks, s);
        CHECK(Markers(blocks) == (std::vector<std::string>{"Lift L", "Lift R", "Slide L", "Slide R"}));
        CHECK(map == (std::vector<int>{0, 1, 2, 3, 2, 3}));
        // Combined -> Longest part in one go: the edits go to the Longest heel block.
        map = ApplyAutoSettings(blocks, longest);
        CHECK(blocks.size() == 6 && blocks[2].auto_type == "slide_heel_longest" && blocks[3].auto_type == "slide_heel_longest");
        CHECK(map == (std::vector<int>{0, 1, 2, 3}));
    }

    // FootPartScuffEvents: a whole-foot pivot gives a heel and a toe event at one time; a pivot on
    // the ball its toe event; part slides as they are. No ball bone: the toe is the tip.
    {
        PhysicsAnalysis a;
        a.ok = true;
        a.rate_hz = 100.0;
        a.samples = 0;  // no per-sample data: only the scuffs below
        a.foot[0].side = 'L';
        a.foot[1].side = 'R';
        for (int s = 0; s < 2; ++s)
            for (int q = 0; q < kFootPartCount; ++q) a.foot[s].part[q].present = true;
        Scuff whole;
        whole.start = 100;
        whole.end = 130;
        whole.part = -1;
        whole.strength = 40.0;
        whole.longest = 0;
        Scuff on_ball = whole;
        on_ball.start = 200;
        on_ball.end = 220;
        on_ball.part = static_cast<int>(FootPart::Ball);
        on_ball.longest = on_ball.part;
        a.foot[0].pivots = {whole, on_ball};
        Scuff heel_slide = whole;
        heel_slide.start = 50;
        heel_slide.end = 70;
        heel_slide.part = 0;
        Scuff toe_slide = heel_slide;
        toe_slide.part = static_cast<int>(FootPart::Tip);
        a.foot[1].part_slides = {heel_slide, toe_slide};
        a.foot[1].part[static_cast<int>(FootPart::Ball)].present = false;
        a.foot[1].pivots = {whole};
        const std::vector<PhysicsEvent> ev = FootPartScuffEvents(a);
        CHECK(Of(ev, PhysicsKind::Pivot, 'L').size() == 3);
        CHECK(Of(ev, PhysicsKind::Pivot, 'L', 0).size() == 1 && Of(ev, PhysicsKind::Pivot, 'L', 1).size() == 2);
        CHECK(Of(ev, PhysicsKind::Pivot, 'R', 0).size() == 1 && Of(ev, PhysicsKind::Pivot, 'R', 2).size() == 1);
        CHECK(Of(ev, PhysicsKind::Slide, 'R', 0).size() == 1 && Of(ev, PhysicsKind::Slide, 'R', 2).size() == 1);
        // The blocks: Each part, whole-foot pivot -> Pivot Heel L and Pivot Toe L at 1.0 s; Longest,
        // one Pivot Heel L there (its longest is the heel) and Pivot Toe L at 2.0 s.
        AutoAnalysis aa;
        aa.params = AutoPhysicsParams(AutoType::Pivot, kAutoDefaultSens, false);
        aa.analysis = a;
        AutoSettings s = Ticked(false, false, false, true);
        s.pivot_parts = ScuffParts::EachPart;
        std::vector<Block> blocks = AutoBlocks(s);  // Pivot Heel L, R, Pivot Toe L, R
        std::vector<Event> got = AutoEvents({aa}, blocks, false);
        CHECK(OfBlock(got, 0).size() == 1 && OfBlock(got, 2).size() == 2);
        if (OfBlock(got, 0).size() == 1 && OfBlock(got, 2).size() == 2)
            CHECK(OfBlock(got, 0)[0].time_s == OfBlock(got, 2)[0].time_s && OfBlock(got, 0)[0].marker == "Pivot Heel L");
        s.pivot_parts = ScuffParts::Longest;
        blocks = AutoBlocks(s);
        got = AutoEvents({aa}, blocks, false);
        CHECK(OfBlock(got, 0).size() == 1 && OfBlock(got, 2).size() == 1);
        if (OfBlock(got, 2).size() == 1) CHECK(std::fabs(OfBlock(got, 2)[0].time_s - 2.0) < 1e-9);
    }

    // End to end: the planted left foot turns 90 deg in 250 ms from 1.0 s. Combined: Pivot L
    // (unchanged); Each part: every pivot appears on the heel or toe block (both for a whole-foot
    // one), at the same time; Longest part: exactly one event, at that time.
    {
        Options o;
        o.dur = 3.0;
        const std::vector<BoneTrack> rig =
            Rig(Planted(-0.1, 0.0, [](double t) { return 0.5 * kPi * Ease((t - 1.0) / 0.25); }), Planted(0.1, 0.2), o);
        AutoSettings             s = Ticked(false, false, false, true);
        const std::vector<Event> comb = DetectAutoEvents(AutoBlocks(s), rig, nullptr, false);
        CHECK(comb.size() == 1);
        // The turn is on the ball (motion_physics_test): one Pivot Toe L in both modes.
        for (ScuffParts m : {ScuffParts::EachPart, ScuffParts::Longest}) {
            s.pivot_parts = m;
            const std::vector<Event> ev = DetectAutoEvents(AutoBlocks(s), rig, nullptr, false);
            CHECK(ev.size() == 1);
            for (const Event& e : ev) CHECK(!comb.empty() && e.time_s == comb[0].time_s && e.marker == "Pivot Toe L");
        }
    }

    // Missing heel, Separate slide: the heel block misses its part (its markers kept), the toe runs;
    // the section names the heel.
    {
        std::vector<int> map(static_cast<size_t>(Role::Count), -1);
        for (int r = 0; r < static_cast<int>(Role::Count); ++r) map[static_cast<size_t>(r)] = r;
        map[static_cast<size_t>(Role::LeftHeel)] = -1;
        CHECK(AutoBlockPartMissing(MakeAutoBlock(AutoKind::SlideHeel, 'L', kAutoDefaultSens, 0.0), map));
        CHECK(!AutoBlockPartMissing(MakeAutoBlock(AutoKind::SlideToe, 'L', kAutoDefaultSens, 0.0), map));
        CHECK(!AutoBlockPartMissing(MakeAutoBlock(AutoKind::Slide, 'L', kAutoDefaultSens, 0.0), map));
        const std::vector<std::string> sep = AutoMissingParts(AutoType::Slide, true, map);
        CHECK(std::find(sep.begin(), sep.end(), RoleName(Role::LeftHeel)) != sep.end());
        CHECK(AutoMissingParts(AutoType::Slide, false, map).empty());
    }

    // Story 10-8s: AutoBlockLanes on a synthetic analysis.
    {
        AutoAnalysis aa;
        aa.params = AutoPhysicsParams(AutoType::Step, 50.0, false);
        PhysicsAnalysis& a = aa.analysis;
        a.ok = true;
        a.rate_hz = 240.0;
        a.samples = 30;
        a.window_first = 10;  // a looping item: the clip is samples 10..19 of the unrolled run
        a.window_samples = 10;
        for (int pi = 0; pi < kFootPartCount; ++pi) {
            PartTrack& pt = a.foot[0].part[pi];
            pt.present = pi != static_cast<int>(FootPart::Ball);  // no ball bone
            if (!pt.present) { pt.missing = "no bone for left toe"; continue; }
            for (int i = 0; i < 30; ++i) {
                pt.speed.push_back(i);
                pt.hspeed.push_back(100 + i);
                pt.contact.push_back(i % 2);
            }
        }
        a.foot[0].yaw_rate_dps.assign(30, -50.0);
        a.foot[0].yaw_valid.assign(30, 0);
        a.foot[0].heading_rate_dps.assign(30, -20.0);
        a.foot[0].heading_valid.assign(30, 1);
        a.foot[0].has_yaw = true;  // heel + tip
        a.foot[0].yaw_valid[11] = 1;        // yaw before heading
        a.foot[0].heading_valid[12] = 0;    // neither: 0
        HandTrack& hl = a.hand[0];
        hl.part.present = true;
        hl.has_heading = true;
        for (int i = 0; i < 30; ++i) {
            hl.part.speed.push_back(200 + i);
            hl.part.hspeed.push_back(0.0);
            hl.part.contact.push_back(i >= 15);
            hl.heading_rate_dps.push_back(-(300.0 + i));
        }
        a.hand[1].part.missing = "no bone for right hand";
        std::string why;
        const std::vector<AutoLane> step = AutoBlockLanes(aa, MakeAutoBlock(AutoKind::Step, 'L', 50.0, 0.0), &why);
        CHECK(step.size() == 2 && why.empty());
        if (step.size() == 2) {
            CHECK(step[0].label == "Heel L  speed" && step[1].label == "Tip L  speed" && step[0].unit == "leg/s");
            CHECK(step[0].curve.size() == 10 && step[0].curve[0] == 10.0 && step[0].curve[9] == 19.0);
            CHECK(step[0].contact.size() == 10 && step[0].contact[0] == 0 && step[0].contact[1] == 1);
            CHECK(step[0].threshold == aa.params.contact_speed && step[0].rate_hz == 240.0);
        }
        const std::vector<AutoLane> toe = AutoBlockLanes(aa, MakeAutoBlock(AutoKind::Toe, 'L', 50.0, 0.0));
        CHECK(toe.size() == 1 && toe[0].label == "Tip L  speed");
        const std::vector<AutoLane> slide = AutoBlockLanes(aa, MakeAutoBlock(AutoKind::Slide, 'L', 50.0, 0.0));
        CHECK(slide.size() == 2 && slide[0].label == "Heel L  hspeed" && slide[0].curve[0] == 110.0 &&
              slide[0].threshold == aa.params.slide_speed);
        const std::vector<AutoLane> piv = AutoBlockLanes(aa, MakeAutoBlock(AutoKind::Pivot, 'L', 50.0, 0.0));
        CHECK(piv.size() == 1 && piv[0].unit == "deg/s" && piv[0].contact.empty() && piv[0].curve[0] == 20.0);
        if (piv.size() == 1)
            CHECK(piv[0].curve[1] == 50.0 && piv[0].curve[2] == 0.0 && piv[0].threshold == aa.params.pivot_rate_dps &&
                  piv[0].above);
        CHECK(!step.empty() && !step[0].above && !slide.empty() && slide[0].above);
        {  // 10-8m part kinds: a slide part reads that part's hspeed, a pivot part the foot's yaw rate
            const std::vector<AutoLane> sh = AutoBlockLanes(aa, MakeAutoBlock(AutoKind::SlideHeel, 'L', 50.0, 0.0));
            CHECK(sh.size() == 1 && sh[0].label == "Heel L  hspeed" && sh[0].above);
            const std::vector<AutoLane> st = AutoBlockLanes(aa, MakeAutoBlock(AutoKind::SlideToeLongest, 'L', 50.0, 0.0));
            CHECK(st.size() == 1 && st[0].label == "Tip L  hspeed" && st[0].threshold == aa.params.slide_speed);
            const std::vector<AutoLane> pt = AutoBlockLanes(aa, MakeAutoBlock(AutoKind::PivotToe, 'L', 50.0, 0.0));
            CHECK(pt.size() == 1 && pt[0].unit == "deg/s" && pt[0].curve[0] == 20.0);
            CHECK(AutoBlockLanes(aa, MakeAutoBlock(AutoKind::PivotHeelLongest, 'L', 50.0, 0.0)).size() == 1);
        }
        const std::vector<AutoLane> grab = AutoBlockLanes(aa, MakeAutoBlock(AutoKind::Grab, 'L', 50.0, 0.0));
        CHECK(grab.size() == 1);
        if (grab.size() == 1)
            CHECK(grab[0].label == "Hand L  speed" && grab[0].curve.size() == 10 && grab[0].curve[0] == 210.0 &&
                  grab[0].contact[4] == 0 && grab[0].contact[5] == 1 && grab[0].threshold == aa.params.hand_contact_speed &&
                  !grab[0].above);
        const std::vector<AutoLane> hpiv = AutoBlockLanes(aa, MakeAutoBlock(AutoKind::HandPivot, 'L', 50.0, 0.0));
        CHECK(hpiv.size() == 1);
        if (hpiv.size() == 1)
            CHECK(hpiv[0].label == "Hand L  heading rate" && hpiv[0].curve.size() == 10 && hpiv[0].curve[0] == 310.0 &&
                  hpiv[0].contact.empty() && hpiv[0].threshold == aa.params.hand_pivot_rate_dps && hpiv[0].above);
        {  // no heel -> toe yaw, or no hand rotation: no lane, and why
            AutoAnalysis ny = aa;
            ny.analysis.foot[0].has_yaw = false;
            CHECK(AutoBlockLanes(ny, MakeAutoBlock(AutoKind::Pivot, 'L', 50.0, 0.0), &why).empty() &&
                  why == "pivot needs the heel and the toe");
            ny.analysis.hand[0].has_heading = false;
            CHECK(AutoBlockLanes(ny, MakeAutoBlock(AutoKind::HandPivotAny, 'L', 50.0, 0.0), &why).empty() &&
                  why == "no usable hand rotation");
        }
        CHECK(AutoBlockLanes(aa, MakeAutoBlock(AutoKind::Grab, 'R', 50.0, 0.0), &why).empty() &&
              why == "no bone for right hand");
        CHECK(AutoBlockLanes(aa, MakeAutoBlock(AutoKind::HandPivot, 'R', 50.0, 0.0)).empty());
        Block rule;
        CHECK(AutoBlockLanes(aa, rule, &why).empty() && why.empty());
        // The threshold follows the sensitivity (the params the block's set was analysed with).
        AutoAnalysis hi = aa;
        hi.params = AutoPhysicsParams(AutoType::Step, 80.0, false);
        const std::vector<AutoLane> s80 = AutoBlockLanes(hi, MakeAutoBlock(AutoKind::Step, 'L', 80.0, 0.0));
        CHECK(!s80.empty() && std::fabs(s80[0].threshold - 0.4 * std::pow(2.0, 0.6)) < 1e-12);
        AutoAnalysis bad = aa;
        bad.analysis.ok = false;
        bad.analysis.error = "no leg";
        CHECK(AutoBlockLanes(bad, MakeAutoBlock(AutoKind::Step, 'L', 50.0, 0.0), &why).empty() && why == "no leg");
    }

    // ---- Story 10-9a: jump and land (story 10-9a-3: on the foot, L / R) ------------------------
    {
        AutoSettings s;
        s.type[static_cast<int>(AutoType::Jump)].on = true;
        s.type[static_cast<int>(AutoType::Land)].on = true;
        const std::vector<Block> jl = AutoBlocks(s);
        CHECK(jl.size() == 4);  // one block per kind and side
        CHECK(Markers(jl) == (std::vector<std::string>{"Jump L", "Jump R", "Land L", "Land R"}));
        for (const Block& b : jl) {
            AutoKind k;
            char     side = 0;
            CHECK(AutoBlockKind(b, &k, &side) && (side == 'L' || side == 'R') && !AutoKindSideless(k));
        }
        CHECK(jl.size() == 4 && jl[0].color == AutoDefaultColor(AutoKind::Jump, 'L') && jl[1].color != jl[0].color);
        CHECK(MakeAutoBlock(AutoKind::Land, 'R', 50.0, 0.0).auto_side == 'R');
        CHECK(ReadAutoSettings(jl) == s);
        // A sided kind on side B (a jump / land block saved side=B in development) is no known
        // block, and Detect drops it.
        Block jump_b = jl[0];
        jump_b.auto_side = 'B';
        CHECK(!AutoBlockKind(jump_b, nullptr, nullptr));
        Block step_b = MakeAutoBlock(AutoKind::Step, 'L', 50.0, 0.0);
        step_b.auto_side = 'B';
        CHECK(!AutoBlockKind(step_b, nullptr, nullptr));
        {
            Block land_b = jl[2];
            land_b.auto_side = 'B';
            land_b.marker = "Land";
            std::vector<Block> old = {HeelRule("Rule"), jump_b, land_b};
            CHECK(!ReadAutoSettings(old).type[static_cast<int>(AutoType::Jump)].on);
            bool                   changed = false;
            const std::vector<int> map = ApplyAutoSettings(old, s, &changed);
            CHECK(changed && Markers(old) == (std::vector<std::string>{"Rule", "Jump L", "Jump R", "Land L", "Land R"}));
            CHECK(map == (std::vector<int>{0, -1, -1}));
            Block future;
            future.auto_type = "future_kind";  // an auto type of a newer RAV still stays
            future.auto_side = 'B';
            std::vector<Block> keep = {future};
            ApplyAutoSettings(keep, s);
            CHECK(keep.size() == 5 && keep[0].auto_type == "future_kind");
        }

        // The record: side=L / R round-trips byte-identical; a pooled copy carries the blocks.
        ItemRules r;
        r.blocks = {HeelRule("Rule")};
        AutoSettings both = Ticked(true, true, false, false);
        both.type[static_cast<int>(AutoType::Jump)].on = true;
        both.type[static_cast<int>(AutoType::Land)].on = true;
        ApplyAutoSettings(r.blocks, both);
        CHECK(Markers(r.blocks) == (std::vector<std::string>{"Rule", "FS L", "FS R", "Lift L", "Lift R", "Jump L", "Jump R",
                                                             "Land L", "Land R"}));
        r.blocks[8].sens = 70;
        const std::string text = SerializeItemRules(r);
        CHECK(text.find("block auto=jump side=L sens=50 ") != std::string::npos);
        CHECK(text.find("block auto=land side=R sens=70 ") != std::string::npos);
        CHECK(text.find("marker=Jump L\n") != std::string::npos && text.find("marker=Land R\n") != std::string::npos);
        ItemRules back;
        CHECK(ParseItemRules(text, &back) && !HasKeptText(back));
        CHECK(BlocksEqual(back.blocks, r.blocks) && SerializeItemRules(back) == text);
        CHECK(ReadAutoSettings(back.blocks) == ReadAutoSettings(r.blocks));
        ItemRules copy;
        CopyPoolContent(back, copy);
        CHECK(BlocksEqual(copy.blocks, r.blocks));
        // Unticking Jump / Land drops their blocks only; the other blocks write back as before.
        ItemRules off = back;
        ApplyAutoSettings(off.blocks, Ticked(true, true, false, false));
        ItemRules plain;
        plain.blocks = {HeelRule("Rule")};
        ApplyAutoSettings(plain.blocks, Ticked(true, true, false, false));
        CHECK(BlocksEqual(off.blocks, plain.blocks));

        // Detection on a ballistic jump (both feet leave at 1.0 s, fly 0.5 s).
        Options o;
        o.dur = 2.5;
        const std::vector<BoneTrack> rig = Rig(JumpFoot(-0.1, 1.0, 0.5), JumpFoot(0.1, 1.0, 0.5), o);
        auto count = [](const std::vector<Event>& ev, const std::string& marker) {
            int n = 0;
            for (const Event& e : ev) n += e.marker == marker;
            return n;
        };
        // Steps and lift-offs alone: each foot's lift and landing.
        std::vector<Block> sl;
        ApplyAutoSettings(sl, Ticked(true, true, false, false));
        const std::vector<Event> ev_sl = DetectAutoEvents(sl, rig, nullptr, false);
        CHECK(count(ev_sl, "FS L") == 1 && count(ev_sl, "FS R") == 1 && count(ev_sl, "Lift L") == 1 &&
              count(ev_sl, "Lift R") == 1);
        // Jump and land alone: one of each, at the take-off and the landing; the feet tie, so the
        // first foot (L) names both.
        const std::vector<Event> ev_jl = DetectAutoEvents(jl, rig, nullptr, false);
        CHECK(ev_jl.size() == 2 && count(ev_jl, "Jump L") == 1 && count(ev_jl, "Land L") == 1);
        // Story 10-9a-2: the take-off's push is found by both signatures; merged into one, the
        // flight's (the first signature).
        {
            const PhysicsParams   jp = AutoPhysicsParams(AutoType::Jump, kAutoDefaultSens, false);
            const PhysicsAnalysis ja = AnalyseMotion(rig, jp);
            const std::vector<PhysicsEvent> push = JumpPushOffEvents(ja, jp), flight = JumpEvents(ja, jp);
            CHECK(push.size() == 1 && flight.size() == 2);
            if (push.size() == 1 && !flight.empty()) CHECK(std::fabs(push[0].time_s - flight[0].time_s) < kAutoMergeS);
            // Story 10-9a-3: and by the free fall (the third signature), merged too.
            const std::vector<PhysicsEvent> ff = JumpFreeFallEvents(ja, jp);
            CHECK(ff.size() == 1);
            if (ff.size() == 1 && !flight.empty()) CHECK(std::fabs(ff[0].time_s - flight[0].time_s) < kAutoMergeS);
        }
        for (const Event& e : ev_jl) CHECK(e.signature == 0);
        if (ev_jl.size() == 2) {
            CHECK(std::fabs(ev_jl[0].time_s - 1.0) < 0.03 && std::fabs(ev_jl[1].time_s - 1.5) < 0.04);
            CHECK(ev_jl[0].strength > 1.0 && std::fabs(ev_jl[0].speed - ev_jl[0].strength * 0.87) < 1e-6);
        }
        // All four: Land replaces the steps there, Jump the lift-offs.
        const std::vector<Event> ev_all = DetectAutoEvents(r.blocks, rig, nullptr, false);
        CHECK(count(ev_all, "Jump L") == 1 && count(ev_all, "Land L") == 1 && count(ev_all, "Jump R") == 0);
        CHECK(count(ev_all, "FS L") == 0 && count(ev_all, "FS R") == 0 && count(ev_all, "Lift L") == 0 &&
              count(ev_all, "Lift R") == 0);
        // A block's offset moves its events but never decides the replacement (physics times).
        std::vector<Block> moved = r.blocks;
        moved[7].offset_ms = 100.0;
        const std::vector<Event> ev_moved = DetectAutoEvents(moved, rig, nullptr, false);
        CHECK(count(ev_moved, "Land L") == 1 && count(ev_moved, "FS L") == 0 && count(ev_moved, "FS R") == 0);
        // Land off (blocks unticked in the list): the steps come back.
        std::vector<Block> land_off = r.blocks;
        land_off[7].enabled = false;
        land_off[8].enabled = false;
        const std::vector<Event> ev_lo = DetectAutoEvents(land_off, rig, nullptr, false);
        CHECK(count(ev_lo, "FS L") == 1 && count(ev_lo, "FS R") == 1 && count(ev_lo, "Lift L") == 0);
        // Walking steps before and after the jump: only the steps and lifts of the jump go.
        {
            Options                      ow;
            ow.dur = 4.3;
            const std::vector<BoneTrack> walk_jump = Rig(StepsAndJump(-0.1, {0.3, 2.9}, 1.8, 0.5),
                                                         StepsAndJump(0.1, {0.8, 3.4}, 1.8, 0.5), ow);
            const std::vector<Event> plain_ev = DetectAutoEvents(sl, walk_jump, nullptr, false);
            CHECK(count(plain_ev, "FS L") == 3 && count(plain_ev, "FS R") == 3 && count(plain_ev, "Lift L") == 3 &&
                  count(plain_ev, "Lift R") == 3);
            const std::vector<Event> all_ev = DetectAutoEvents(r.blocks, walk_jump, nullptr, false);
            CHECK(count(all_ev, "Jump L") + count(all_ev, "Jump R") == 1 && count(all_ev, "Land L") + count(all_ev, "Land R") == 1);
            CHECK(count(all_ev, "FS L") == 2 && count(all_ev, "FS R") == 2 && count(all_ev, "Lift L") == 2 &&
                  count(all_ev, "Lift R") == 2);
            for (const Event& e : all_ev)
                if (e.marker == "FS L" || e.marker == "FS R" || e.marker == "Lift L" || e.marker == "Lift R")
                    CHECK(e.time_s < 1.6 || e.time_s > 2.6);  // the far ones stay
            // Separate steps: the heel and toe steps within 30 ms of a (whole-foot) Land go, back
            // with Land off.
            std::vector<Block> sep = {HeelRule("Rule")};
            AutoSettings       ss = Ticked(true, true, false, false, true);
            ss.type[static_cast<int>(AutoType::Jump)].on = true;
            ss.type[static_cast<int>(AutoType::Land)].on = true;
            ApplyAutoSettings(sep, ss);
            const std::vector<Event> sep_ev = DetectAutoEvents(sep, walk_jump, nullptr, false);
            double land_t = -1.0;
            for (const Event& e : sep_ev)
                if (e.marker.compare(0, 4, "Land") == 0) land_t = e.time_s;
            CHECK(land_t > 2.2 && land_t < 2.4);
            int near = 0, far = 0;
            for (const Event& e : sep_ev)
                if (e.marker.compare(0, 4, "Heel") == 0 || e.marker.compare(0, 3, "Toe") == 0) {
                    if (std::fabs(e.time_s - land_t) <= kAutoReplaceS + 1e-9) ++near;
                    else if (e.time_s < 1.6 || e.time_s > 2.6) ++far;
                }
            CHECK(near == 0 && far == 8);
            // Story 10-8r: Land Separate too: a Land Heel / Land Toe replaces only the steps of its
            // own part (a Land Toe never drops the heel slap that follows it).
            {
                std::vector<Block> sp = {HeelRule("Rule")};
                AutoSettings       ps = ss;
                ps.separate_parts[static_cast<int>(AutoType::Land)] = true;
                ApplyAutoSettings(sp, ps);
                const std::vector<Event> sp_ev = DetectAutoEvents(sp, walk_jump, nullptr, false);
                double      lt = -1.0;
                std::string lp;  // the Land's part word: "Heel" or "Toe"
                for (const Event& e : sp_ev)
                    if (e.marker.compare(0, 5, "Land ") == 0 && e.marker.size() > 7) {
                        lt = e.time_s;
                        lp = e.marker.compare(0, 9, "Land Heel") == 0 ? "Heel" : "Toe";
                    }
                CHECK(lt > 2.2 && lt < 2.4 && !lp.empty());
                int own = 0, other = 0;
                for (const Event& e : sp_ev)
                    if ((e.marker.compare(0, 4, "Heel") == 0 || e.marker.compare(0, 3, "Toe") == 0) &&
                        std::fabs(e.time_s - lt) <= kAutoReplaceS + 1e-9)
                        ++(e.marker.compare(0, lp.size(), lp) == 0 ? own : other);
                CHECK(own == 0 && other > 0);
            }
            std::vector<Block> sep_off = sep;
            for (Block& b : sep_off)
                if (b.marker.compare(0, 4, "Land") == 0) b.enabled = false;
            const std::vector<Event> sep_off_ev = DetectAutoEvents(sep_off, walk_jump, nullptr, false);
            int back_near = 0;
            for (const Event& e : sep_off_ev)
                if ((e.marker.compare(0, 4, "Heel") == 0 || e.marker.compare(0, 3, "Toe") == 0) &&
                    std::fabs(e.time_s - land_t) <= kAutoReplaceS + 1e-9)
                    ++back_near;
            CHECK(back_near > 0);
        }
        // Detect's search reads each block's own detections: the replaced steps are not raised.
        // Story 10-9a-3: nor the right foot's Jump / Land blocks, silent because the left ones hold
        // this two-footed jump's events (one event for both feet).
        std::vector<int> full_map(static_cast<size_t>(Role::Count));
        for (size_t i = 0; i < full_map.size(); ++i) full_map[i] = static_cast<int>(i);
        std::vector<Block>          searched = r.blocks;
        std::vector<AutoSensResult> res;
        CHECK(!ApplyAutoSensSearch(searched, rig, full_map, &res, nullptr, false));
        for (const AutoSensResult& x : res) CHECK(!x.searched);
        // With the left ones off, the right ones are searched again.
        std::vector<Block> left_off = r.blocks;
        for (Block& b : left_off)
            if (b.marker == "Jump L" || b.marker == "Land L") b.enabled = false;
        CHECK(!ApplyAutoSensSearch(left_off, rig, full_map, &res, nullptr, false));
        for (size_t i = 0; i < res.size() && i < left_off.size(); ++i) {
            const bool right_jl = left_off[i].marker == "Jump R" || left_off[i].marker == "Land R";
            CHECK(res[i].searched == right_jl);
        }
        // Hips mapped off: no Jump / Land (listed as missing), the other types unaffected.
        std::vector<int> no_hips = full_map;
        no_hips[static_cast<size_t>(Role::Hips)] = -1;
        for (const Block& b : jl) CHECK(AutoBlockPartMissing(b, no_hips));
        CHECK(!AutoBlockPartMissing(r.blocks[1], no_hips) && !AutoBlockPartMissing(r.blocks[3], no_hips));
        const std::vector<std::string> miss = AutoMissingParts(AutoType::Jump, false, no_hips);
        CHECK(miss.size() == 1 && miss[0] == RoleName(Role::Hips));
        CHECK(AutoMissingParts(AutoType::Step, false, no_hips).empty());
        std::vector<BoneTrack> rig_nh = rig;
        rig_nh[static_cast<size_t>(Role::Hips)] = BoneTrack{};
        const std::vector<Event> ev_nh = DetectAutoEvents(r.blocks, rig_nh, nullptr, false);
        CHECK(count(ev_nh, "Jump L") + count(ev_nh, "Jump R") == 0 && count(ev_nh, "Land L") + count(ev_nh, "Land R") == 0 &&
              count(ev_nh, "FS L") == 1 &&
              count(ev_nh, "Lift R") == 1);
        // The lane: the hips' vertical speed, no threshold, shaded over the flight.
        AutoAnalysis aa;
        aa.params = AutoPhysicsParams(AutoType::Jump, 50.0, false);
        aa.analysis = AnalyseMotion(rig, aa.params);
        // Story 10-9a-2: Jump's second lane, the hips' upward acceleration vs the push-off threshold.
        const std::vector<AutoLane> lanes = AutoBlockLanes(aa, jl[2]);
        CHECK(lanes.size() == 1 && lanes[0].no_threshold && !lanes[0].shade_note.empty());
        const std::vector<AutoLane> jlanes = AutoBlockLanes(aa, jl[0]);
        CHECK(jlanes.size() == 2);
        if (jlanes.size() == 2) {
            CHECK(jlanes[1].unit == "g" && !jlanes[1].no_threshold && jlanes[1].above);
            CHECK(jlanes[1].threshold == aa.params.jump_impulse_g && jlanes[1].curve.size() == aa.analysis.samples);
            CHECK(jlanes[1].label == "Hips  upward acceleration" && !jlanes[1].shade_note.empty());
        }
        if (lanes.size() == 1) {
            int flight = 0;
            for (char c : lanes[0].contact) flight += c;
            CHECK(lanes[0].curve.size() == aa.analysis.samples && flight > 100 && flight < 140);  // about 0.5 s at 240 Hz
        }
        AutoAnalysis nh;
        nh.params = aa.params;
        nh.analysis = AnalyseMotion(rig_nh, aa.params);
        std::string why;
        CHECK(AutoBlockLanes(nh, jl[2], &why).empty() && why == "no bone for " + std::string(RoleName(Role::Hips)));
    }

    // ---- Story 10-9a-3: lift-off, jump and land Combined / Separate ----------------------------
    {
        const int lt = static_cast<int>(AutoType::LiftOff), jt = static_cast<int>(AutoType::Jump),
                  dt = static_cast<int>(AutoType::Land);
        CHECK(AutoTypeRow(AutoType::LiftOff).part_split && AutoTypeRow(AutoType::Jump).part_split &&
              AutoTypeRow(AutoType::Land).part_split && !AutoTypeRow(AutoType::Step).part_split);
        CHECK(AutoTypeRow(AutoType::Jump).shared_sides && AutoTypeRow(AutoType::Land).shared_sides &&
              !AutoTypeRow(AutoType::LiftOff).shared_sides);
        AutoSettings comb;
        comb.type[lt].on = comb.type[jt].on = comb.type[dt].on = true;
        AutoSettings sep = comb;
        sep.separate_parts[lt] = sep.separate_parts[jt] = sep.separate_parts[dt] = true;
        CHECK(Markers(AutoBlocks(comb)) ==
              (std::vector<std::string>{"Lift L", "Lift R", "Jump L", "Jump R", "Land L", "Land R"}));
        CHECK(Markers(AutoBlocks(sep)) ==
              (std::vector<std::string>{"Lift Heel L", "Lift Heel R", "Lift Toe L", "Lift Toe R", "Jump Heel L",
                                        "Jump Heel R", "Jump Toe L", "Jump Toe R", "Land Heel L", "Land Heel R",
                                        "Land Toe L", "Land Toe R"}));
        CHECK(ReadAutoSettings(AutoBlocks(sep)) == sep && ReadAutoSettings(AutoBlocks(comb)) == comb);
        CHECK(AutoPending(sep, comb) && !AutoPending(sep, sep));
        AutoSettings one = comb;
        one.separate_parts[jt] = true;
        CHECK(AutoPending(one, comb) && AutoTypeSeparate(AutoType::Jump, one) && !AutoTypeSeparate(AutoType::Land, one));
        AutoSettings off = sep;
        off.type[jt].on = false;
        CHECK(!AutoPending(off, ReadAutoSettings(AutoBlocks(off))));  // an unticked type's mode never waits
        // Words, keys and colours: the heel takes its type's colour, the toe a shade lighter.
        CHECK(std::string(AutoKindWord(AutoKind::LiftHeel)) == "lift_heel" &&
              std::string(AutoKindWord(AutoKind::LandToe)) == "land_toe");
        CHECK(AutoDefaultColor(AutoKind::JumpHeel, 'L') == AutoDefaultColor(AutoKind::Jump, 'L') &&
              AutoDefaultColor(AutoKind::JumpToe, 'L') != AutoDefaultColor(AutoKind::Jump, 'L'));
        CHECK(std::string(AutoKindRow(AutoKind::LandHeel).role_key) == "heel" &&
              std::string(AutoKindRow(AutoKind::LiftToe).role_key) == "toe");
        // Separate needs the heel and the toe (or toe end) of each foot.
        std::vector<int> no_heel(static_cast<size_t>(Role::Count));
        for (size_t i = 0; i < no_heel.size(); ++i) no_heel[i] = static_cast<int>(i);
        no_heel[static_cast<size_t>(Role::LeftHeel)] = -1;
        const std::vector<std::string> m_sep = AutoMissingParts(AutoType::Jump, true, no_heel);
        CHECK(std::find(m_sep.begin(), m_sep.end(), RoleName(Role::LeftHeel)) != m_sep.end());
        CHECK(AutoMissingParts(AutoType::Jump, false, no_heel).empty());
        CHECK(AutoBlockPartMissing(MakeAutoBlock(AutoKind::JumpHeel, 'L', 50.0, 0.0), no_heel) &&
              !AutoBlockPartMissing(MakeAutoBlock(AutoKind::JumpToe, 'L', 50.0, 0.0), no_heel) &&
              !AutoBlockPartMissing(MakeAutoBlock(AutoKind::JumpHeel, 'R', 50.0, 0.0), no_heel) &&
              AutoBlockPartMissing(MakeAutoBlock(AutoKind::LiftHeel, 'L', 50.0, 0.0), no_heel));

        // Detection: each event on the block of the part that leaves last / touches first (the
        // combined event's part; the toe = the ball or the tip).
        Options o;
        o.dur = 2.5;
        const std::vector<BoneTrack> rig = Rig(JumpFoot(-0.1, 1.0, 0.5), JumpFoot(0.1, 1.03, 0.5), o);
        const PhysicsParams          jp = AutoPhysicsParams(AutoType::Jump, kAutoDefaultSens, false);
        const std::vector<PhysicsEvent> flight = JumpEvents(AnalyseMotion(rig, jp), jp);
        const std::vector<Event>        ev = DetectAutoEvents(AutoBlocks(sep), rig, nullptr, false);
        const std::vector<Event>        evc = DetectAutoEvents(AutoBlocks(comb), rig, nullptr, false);
        CHECK(flight.size() == 2 && ev.size() == evc.size());
        for (const PhysicsEvent& f : flight) {
            const std::string want = std::string(f.kind == PhysicsKind::Jump ? "Jump" : "Land") +
                                     (f.part == static_cast<int>(FootPart::Heel) ? " Heel " : " Toe ") + f.side;
            int n = 0;
            for (const Event& e : ev) n += e.marker == want;
            CHECK(n == 1);
        }
        for (size_t i = 0; i < ev.size() && i < evc.size(); ++i) CHECK(std::fabs(ev[i].time_s - evc[i].time_s) < 1e-9);
        // Detect's search: a part block silent because its sibling (same type and side) holds the
        // event is not raised; with the sibling off, it is searched.
        {
            std::vector<int> full(static_cast<size_t>(Role::Count));
            for (size_t i = 0; i < full.size(); ++i) full[i] = static_cast<int>(i);
            AutoSettings lift;
            lift.type[lt].on = true;
            lift.separate_parts[lt] = true;
            std::vector<Block>          lb = AutoBlocks(lift);
            std::vector<int>            counts(lb.size(), 0);
            for (const Event& e : DetectAutoEvents(lb, rig, nullptr, false))
                if (e.block >= 0 && e.block < static_cast<int>(counts.size())) ++counts[static_cast<size_t>(e.block)];
            int checked = 0;
            for (size_t i = 0; i < lb.size(); ++i) {
                // the silent part of each foot (Lift Heel or Lift Toe), its sibling holding the lift-off
                if (counts[i] != 0) continue;
                ++checked;
                size_t sib = lb.size();
                for (size_t j = 0; j < lb.size(); ++j)
                    if (j != i && lb[j].auto_side == lb[i].auto_side && counts[j] > 0) sib = j;
                CHECK(sib < lb.size());
                if (sib == lb.size()) continue;
                std::vector<AutoSensResult> res = SearchAutoSensitivity(lb, rig, counts, full, false);
                CHECK(res.size() == lb.size() && !res[i].searched);
                std::vector<Block> off = lb;
                off[sib].enabled = false;
                res = SearchAutoSensitivity(off, rig, counts, full, false);
                CHECK(res.size() == lb.size() && res[i].searched);
            }
            CHECK(checked == 2);
        }

        // Switching modes maps the edits as steps do: combined -> heel, heel / toe -> combined.
        std::vector<Block> blocks = {HeelRule("Rule")};
        ApplyAutoSettings(blocks, comb);
        const std::vector<Block> before = blocks;
        const std::vector<int>   to_sep = ApplyAutoSettings(blocks, sep);
        auto at = [](const std::vector<Block>& b, int i) { return i >= 0 && i < static_cast<int>(b.size()) ? b[i].marker : "-"; };
        for (size_t i = 0; i < before.size(); ++i) {
            const std::string m = before[i].marker;
            if (m == "Rule") CHECK(at(blocks, to_sep[i]) == "Rule");
            else CHECK(at(blocks, to_sep[i]) == m.substr(0, m.size() - 2) + " Heel " + m.back());
        }
        const std::vector<Block> parts = blocks;
        const std::vector<int>   to_comb = ApplyAutoSettings(blocks, comb);
        for (size_t i = 0; i < parts.size(); ++i) {
            const std::string m = parts[i].marker;
            if (m == "Rule") continue;
            CHECK(at(blocks, to_comb[i]) == m.substr(0, 4) + " " + m.back());  // "Jump Toe L" -> "Jump L"
        }
    }

    // ---- Story 10-9a-2: several signatures per kind -----------------------------------------
    {
        // The merge: a later signature's event within kAutoMergeS of an earlier one's is dropped
        // (the earlier keeps its time and index); far apart, both stay; one signature never merges
        // its own events; through the seam on a loop.
        auto picks = [](const std::vector<std::vector<double>>& t, double period) {
            std::vector<std::pair<int, size_t>> out;
            for (const AutoSigPick& p : MergeSignatureEvents(t, period)) out.emplace_back(p.signature, p.index);
            return out;
        };
        using PV = std::vector<std::pair<int, size_t>>;
        CHECK(picks({{1.0}, {1.03}}, 0.0) == (PV{{0, 0}}));
        CHECK(picks({{1.03}, {1.0}}, 0.0) == (PV{{0, 0}}));
        CHECK(picks({{1.0}, {1.0 + kAutoMergeS}}, 0.0) == (PV{{0, 0}}));
        CHECK(picks({{1.0}, {1.5}}, 0.0) == (PV{{0, 0}, {1, 0}}));
        CHECK(picks({{1.0, 1.02}}, 0.0) == (PV{{0, 0}, {0, 1}}));
        CHECK(picks({{}, {1.0}}, 0.0) == (PV{{1, 0}}));
        CHECK(picks({{0.0, 1.0}, {0.5, 1.04, 1.06}}, 0.0) == (PV{{0, 0}, {0, 1}, {1, 0}}));
        CHECK(picks({{0.01}, {1.98}}, 2.0) == (PV{{0, 0}}));
        CHECK(picks({{0.01}, {1.98}}, 0.0) == (PV{{0, 0}, {1, 0}}));
        CHECK(picks({{1.0}, {1.04}, {1.08}}, 0.0) == (PV{{0, 0}, {2, 0}}));  // only earlier signatures' kept events count
        CHECK(MergeSignatureEvents({}, 0.0).empty());

        // A short hop (both feet off 250 ms, under the 300 ms minimum flight) pushed off hard (the
        // hips dip 3 cm just before it: about 1.2 g upward at 0.97 s): no flight, one Jump found by
        // push-off (signature 1); Land finds nothing.
        Options o;
        o.dur = 2.0;
        std::vector<BoneTrack> rig = Rig(JumpFoot(-0.1, 1.0, 0.25), JumpFoot(0.1, 1.0, 0.25), o);
        for (size_t i = 0; i < rig[static_cast<size_t>(Role::Hips)].pos.size(); ++i) {
            const double u = (i / o.rate - 0.95) / 0.04;
            rig[static_cast<size_t>(Role::Hips)].pos[i].y -= 0.03 * std::exp(-0.5 * u * u);
        }
        AutoSettings s;
        s.type[static_cast<int>(AutoType::Jump)].on = true;
        s.type[static_cast<int>(AutoType::Land)].on = true;
        const std::vector<Block> jl = AutoBlocks(s);
        const std::vector<Event> ev = DetectAutoEvents(jl, rig, nullptr, false);
        CHECK(ev.size() == 1);
        if (ev.size() == 1) {
            CHECK(ev[0].marker == "Jump L" && ev[0].signature == 1 && std::fabs(ev[0].time_s - 1.0) < 0.03);
            CHECK(AutoSignatureCount(AutoKindRow(AutoKind::Jump)) == 3 &&
                  std::string(AutoKindRow(AutoKind::Jump).sigs[ev[0].signature].name) == "push-off");
        }
        // The event list carries the signature.
        const std::vector<ShownEvent> shown = BuildEventList(ev, {}, jl.size());
        CHECK(shown.size() == 1 && !ev.empty() && shown[0].signature == ev[0].signature);
        // A lower Jump sensitivity raises the push-off threshold: none at 0 % (1.8 g).
        std::vector<Block> low = jl;
        for (Block& b : low) b.sens = 0.0;
        CHECK(DetectAutoEvents(low, rig, nullptr, false).empty());
        // Existing kinds (one signature): every event's signature is 0.
        std::vector<Block> sl;
        ApplyAutoSettings(sl, Ticked(true, true, true, true));
        for (const Event& e : DetectAutoEvents(sl, rig, nullptr, false)) CHECK(e.signature == 0);
    }

    // Deferred 10-9a-2 (2026-10-19): the merge on the Detect path, read as a loop
    // (DetectAutoEvents(..., true)): a take-off at the seam, found by the flight, the push-off and
    // the free fall, gives one Jump, at the flight's time, just before the seam or just after it.
    // Note: every signature times its take-off by the same last-foot lift-off (TakeOffAt, 10-8p),
    // so the three agree to the sample here; the merge through the seam with differing times is
    // checked on MergeSignatureEvents above.
    for (const double t0 : {0.0, 0.01}) {
        Options o;
        o.dur = 2.0;
        const std::vector<BoneTrack> rig = Rig(JumpFoot(-0.1, t0, 0.5), JumpFoot(0.1, t0, 0.5), o);
        const PhysicsParams          jp = AutoPhysicsParams(AutoType::Jump, kAutoDefaultSens, true);
        const PhysicsAnalysis        ja = AnalyseMotion(rig, jp);
        CHECK(ja.ok && ja.looping);
        CHECK(JumpPushOffEvents(ja, jp).size() == 1 && JumpFreeFallEvents(ja, jp).size() == 1);
        AutoSettings s;
        s.type[static_cast<int>(AutoType::Jump)].on = true;
        const std::vector<Event> on = DetectAutoEvents(AutoBlocks(s), rig, nullptr, true);
        CHECK(on.size() == 1);
        if (on.size() == 1) {
            CHECK(on[0].marker == "Jump L" && on[0].signature == 0);
            // t0 = 0: the take-off reads just before the seam (T = 2 s); t0 = 10 ms: just after it.
            CHECK(t0 == 0.0 ? on[0].time_s > 1.98 && on[0].time_s < 2.0 : on[0].time_s >= 0.0 && on[0].time_s < 0.02);
        }
    }

    // ---- Story 10-9g: the gear jolt (Body, sideless) ------------------------------------------
    {
        Options ow;
        ow.dur = 4.3;
        // Walking steps at 0.3 / 0.8 and 2.9 / 3.4 s (each lands 0.4 s later), a both-feet jump
        // from 1.8 s flying 0.5 s (landing at 2.3 s, 2.45 m/s down).
        const std::vector<BoneTrack> rig =
            Rig(StepsAndJump(-0.1, {0.3, 2.9}, 1.8, 0.5), StepsAndJump(0.1, {0.8, 3.4}, 1.8, 0.5), ow);
        AutoSettings s;
        s.type[static_cast<int>(AutoType::Jolt)].on = true;
        const std::vector<Block> jb = AutoBlocks(s);
        CHECK(jb.size() == 1 && Markers(jb) == (std::vector<std::string>{"Jolt"}));
        AutoKind k = AutoKind::Step;
        char     side = 0;
        CHECK(jb.size() == 1 && AutoBlockKind(jb[0], &k, &side) && k == AutoKind::Jolt && side == 'B' && AutoKindSideless(k));
        CHECK(std::string(AutoKindSides(AutoKind::Jolt)) == "B" && AutoMarkerName(AutoKind::Jolt, 'R') == "Jolt");
        CHECK(MakeAutoBlock(AutoKind::Jolt, 'L', 50.0, 0.0).auto_side == 'B' && ReadAutoSettings(jb) == s);
        CHECK(AutoCategoryOf(AutoType::Jolt) == AutoCategory::Body && std::string(AutoKindWord(AutoKind::Jolt)) == "jolt");
        // Default (1 g): the landing jolts, the walking steps (about 0.4 g) do not; strength in g,
        // no speed.
        const std::vector<Event> ev = DetectAutoEvents(jb, rig, nullptr, false);
        CHECK(ev.size() == 1);
        if (ev.size() == 1) CHECK(ev[0].time_s > 2.2 && ev[0].time_s < 2.4 && ev[0].strength > 2.0 && ev[0].speed == 0.0);
        // Every impact at 0 g: one jolt per step, the landing's two feet and its Land merged into one.
        const PhysicsAnalysis a = AnalyseMotion(rig);
        PhysicsParams         all;
        all.jolt_min_g = 0.0;
        const std::vector<PhysicsEvent> imp = JoltEvents(a, all);
        CHECK(imp.size() == 5);
        int soft = 0, hard = 0;
        for (size_t i = 0; i < imp.size(); ++i) {
            CHECK(imp[i].kind == PhysicsKind::Jolt && imp[i].side == 'B' && imp[i].part == -1);
            if (i) CHECK(imp[i - 1].time_s < imp[i].time_s);
            if (imp[i].time_s > 2.2 && imp[i].time_s < 2.4) hard += imp[i].strength > 2.0;
            else soft += imp[i].strength < 1.0;
        }
        CHECK(hard == 1 && soft == 4);
        // The sensitivity divides the threshold: at 0 % (2 g) the landing still jolts.
        CHECK(AutoPhysicsParams(AutoType::Jolt, 0.0, false).jolt_min_g == PhysicsParams().jolt_min_g * 2.0);
        if (!jb.empty()) {
            Block j0 = jb[0];
            j0.sens = 0.0;
            CHECK(DetectAutoEvents(std::vector<Block>{j0}, rig, nullptr, false).size() == 1);
        }
        // A merge window longer than the gaps: every impact in one jolt, at the first's time, with
        // the highest peak.
        PhysicsParams wide = all;
        wide.jolt_merge_s = 10.0;
        const std::vector<PhysicsEvent> one = JoltEvents(a, wide);
        CHECK(one.size() == 1 && !imp.empty());
        if (one.size() == 1 && !imp.empty()) CHECK(one[0].time_s == imp[0].time_s && one[0].strength > 2.0);
        // Ticking Jolt changes no other type's events.
        AutoSettings legs = Ticked(true, true, false, false);
        legs.type[static_cast<int>(AutoType::Jump)].on = true;
        legs.type[static_cast<int>(AutoType::Land)].on = true;
        AutoSettings with = legs;
        with.type[static_cast<int>(AutoType::Jolt)].on = true;
        const std::vector<Block> lb = AutoBlocks(legs), wb = AutoBlocks(with);
        std::vector<Event>       others;
        for (const Event& e : DetectAutoEvents(wb, rig, nullptr, false))
            if (e.marker != "Jolt") others.push_back(e);
        CHECK(SameEvents(others, DetectAutoEvents(lb, rig, nullptr, false)));
        // No hips: the part is missing and no jolt (no acceleration).
        std::vector<int> map(static_cast<size_t>(Role::Count), -1);
        for (int r = 0; r < static_cast<int>(Role::Count); ++r) map[static_cast<size_t>(r)] = r;
        if (!jb.empty()) {
            CHECK(!AutoBlockPartMissing(jb[0], map));
            map[static_cast<size_t>(Role::Hips)] = -1;
            CHECK(AutoBlockPartMissing(jb[0], map));
        }
        std::vector<BoneTrack> no_hips = rig;
        no_hips[static_cast<size_t>(Role::Hips)] = BoneTrack();
        const PhysicsAnalysis nh = AnalyseMotion(no_hips);
        CHECK(JoltAccelerationG(nh).empty() && JoltEvents(nh, all).empty());
        // Its lane: the upward acceleration against the threshold.
        if (!jb.empty()) {
            AutoAnalysis aa;
            aa.params = AutoPhysicsParams(AutoType::Jolt, kAutoDefaultSens, false);
            aa.analysis = AnalyseMotion(rig, aa.params);
            const std::vector<AutoLane> lanes = AutoBlockLanes(aa, jb[0]);
            CHECK(lanes.size() == 1);
            if (lanes.size() == 1) CHECK(lanes[0].unit == "g" && lanes[0].above && lanes[0].threshold == 1.0);
        }
        // Review: Detect never raises Jolt: a walk with no jolt is searched (Detect not lit for it),
        // neither raised nor "none up to 100 %", its sensitivity unchanged.
        {
            Options wo;
            wo.dur = 4.3;
            const std::vector<BoneTrack> walk =
                Rig(StepsAndJump(-0.1, {0.3, 2.9}, 10.0, 0.5), StepsAndJump(0.1, {0.8, 3.4}, 10.0, 0.5), wo);
            CHECK(DetectAutoEvents(jb, walk, nullptr, false).empty());
            std::vector<int> full(static_cast<size_t>(Role::Count), -1);
            for (int r = 0; r < static_cast<int>(Role::Count); ++r) full[static_cast<size_t>(r)] = r;
            const std::vector<AutoSensResult> sr = SearchAutoSensitivity(jb, walk, {0}, full, false);
            CHECK(sr.size() == 1);
            if (sr.size() == 1) CHECK(sr[0].searched && !sr[0].raised && !sr[0].none && sr[0].sens == jb[0].sens);
            CHECK(AutoTypeRow(AutoType::Jolt).no_sens_raise);
            for (int t = 0; t <= static_cast<int>(AutoType::Land); ++t)  // the Legs and Arms types still raise
                CHECK(!AutoTypeRow(static_cast<AutoType>(t)).no_sens_raise);
        }
        // Review: on a loop, a landing just before the seam and the other foot's just after it
        // (100 ms apart through the seam) make one jolt, at the earlier one (before the seam).
        {
            Options lo;
            lo.dur = 2.0;
            auto cyc = [](double x, double t0) {
                return FootFn([=](double t) {
                    FootPose     f;
                    const double u = std::fmod(t - t0 + 4.0, 2.0);
                    f.ball = Vec3d{x, 0.02, 0.0};
                    if (u > 0.0 && u < 0.5) f.ball.y += 9.81 * 0.5 / 2.0 * u - 0.5 * 9.81 * u * u;
                    return f;
                });
            };
            const std::vector<BoneTrack> rig2 = Rig(cyc(-0.1, 1.45), cyc(0.1, 1.55), lo);
            PhysicsParams                lp = AutoPhysicsParams(AutoType::Jolt, kAutoDefaultSens, true);
            lp.jolt_min_g = 0.0;
            const PhysicsAnalysis la = AnalyseMotion(rig2, lp);
            CHECK(la.ok && la.looping);
            int steps = 0;
            for (const PhysicsEvent& e : FootEvents(la, lp)) steps += e.kind == PhysicsKind::FootStep;
            CHECK(steps == 2);  // 0.05 and 1.95 s
            const std::vector<PhysicsEvent> lj = JoltEvents(la, lp);
            CHECK(lj.size() == 1);
            if (lj.size() == 1) CHECK(lj[0].time_s > 1.9 && lj[0].time_s < 2.0);
        }
    }
    // Story 10-9d: claps and hand on body (hand_touch.h), standing still. Leg 0.87 m; the left
    // thigh's axis runs x -0.1, z -0.14, y 0.50 -> 0.95.
    {
        // A hand moving straight from `from` to `to` at `speed` m/s from t0, then at rest there.
        const auto toward = [](Vec3d from, Vec3d to, double t0, double speed) {
            return [=](double t) {
                const Vec3d  d{to.x - from.x, to.y - from.y, to.z - from.z};
                const double len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                const double u = std::clamp((t - t0) * speed / len, 0.0, 1.0);
                return Vec3d{from.x + d.x * u, from.y + d.y * u, from.z + d.z * u};
            };
        };
        const Vec3d far_r{0.6, 1.2, 0.35}, meet_r{0.045, 1.2, 0.35}, still_l{-0.045, 1.2, 0.35};
        Options o;
        o.dur = 4.0;
        o.hand[0] = [=](double) { return still_l; };
        // The right hand claps on the still left one at 0.722 s, leaves slowly from 1.2 s, claps
        // again at 2.722 s.
        const HandFn in1 = toward(far_r, meet_r, 0.5, 2.5), out = toward(meet_r, far_r, 1.2, 1.0),
                     in2 = toward(far_r, meet_r, 2.5, 2.5);
        o.hand[1] = [=](double t) { return t < 1.2 ? in1(t) : t < 2.5 ? out(t) : in2(t); };
        const std::vector<BoneTrack> rig = Rig(Planted(-0.1, 0.0), Planted(0.1, 0.0), o);
        const PhysicsAnalysis        a = AnalyseMotion(rig);
        CHECK(a.ok && a.role_pos.size() == static_cast<size_t>(Role::Count));
        const std::vector<PhysicsEvent> claps = ClapEvents(a);
        CHECK(claps.size() == 2);
        if (claps.size() == 2) {
            CHECK(claps[0].kind == PhysicsKind::Clap && claps[0].side == 'R' && claps[0].part == -1);
            CHECK(std::fabs(claps[0].time_s - 0.722) < 0.03 && std::fabs(claps[1].time_s - 2.722) < 0.03);
            CHECK(claps[0].strength > 2.0 && claps[0].strength < 3.2);  // 2.5 m/s / 0.87 m = 2.9 leg/s
        }
        CHECK(HandOnBodyEvents(a).empty());  // both hands stay far from the body
        CHECK(TouchSummary(a, claps) == "touches: claps 2, hand on body 0");
        CHECK(PhysicsMarkerName(claps.empty() ? PhysicsEvent{} : claps[0]).rfind("PHY R clap ", 0) == 0);
        const std::vector<double> hd = HandsDistance(a);
        CHECK(hd.size() == a.samples && std::fabs(hd[static_cast<size_t>(0.722 * 240.0) + 24] - 0.09 / 0.87) < 0.01);
        // Through Detect: one Clap block per side, the right one marked.
        AutoSettings s;
        s.type[static_cast<int>(AutoType::Clap)].on = true;
        const std::vector<Block> blocks = AutoBlocks(s);
        CHECK(Markers(blocks) == (std::vector<std::string>{"Clap L", "Clap R"}));
        const std::vector<Event> ev = DetectAutoEvents(blocks, rig, nullptr, false);
        CHECK(ev.size() == 2 && ev[0].marker == "Clap R" && ev[1].marker == "Clap R");
        // A slow clap (1 m/s, 1.15 leg/s): none at the default sensitivity, found at 100 %.
        Options slow = o;
        slow.hand[1] = toward(far_r, meet_r, 0.5, 1.0);
        const std::vector<BoneTrack> srig = Rig(Planted(-0.1, 0.0), Planted(0.1, 0.0), slow);
        CHECK(ClapEvents(AnalyseMotion(srig)).empty());
        const PhysicsParams hi = AutoPhysicsParams(AutoType::Clap, 100.0, false);
        CHECK(ClapEvents(AnalyseMotion(srig, hi), hi).size() == 1);
        // Hands passing each other at speed, never stopping: no clap.
        Options pass = o;
        pass.hand[1] = toward(Vec3d{0.045, 1.2, 1.0}, Vec3d{0.045, 1.2, -0.3}, 0.5, 2.5);
        CHECK(ClapEvents(AnalyseMotion(Rig(Planted(-0.1, 0.0), Planted(0.1, 0.0), pass))).empty());
        // One hand missing: no clap, and the block says so.
        Options one = o;
        one.hand[1] = nullptr;
        CHECK(ClapEvents(AnalyseMotion(Rig(Planted(-0.1, 0.0), Planted(0.1, 0.0), one))).empty());
    }
    {
        const auto toward = [](Vec3d from, Vec3d to, double t0, double speed) {
            return [=](double t) {
                const Vec3d  d{to.x - from.x, to.y - from.y, to.z - from.z};
                const double len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                const double u = std::clamp((t - t0) * speed / len, 0.0, 1.0);
                return Vec3d{from.x + d.x * u, from.y + d.y * u, from.z + d.z * u};
            };
        };
        // The left hand lands on the front of the left thigh (12 cm from its axis) at 2 m/s:
        // arrives at 0.5 + 0.61 / 2 = 0.805 s. The right hand stays far.
        Options o;
        o.dur = 3.0;
        o.hand[0] = toward(Vec3d{-0.6, 0.75, 0.3}, Vec3d{-0.1, 0.75, -0.02}, 0.5, 2.0);
        o.hand[1] = [](double) { return Vec3d{0.7, 1.2, 0.4}; };
        const std::vector<BoneTrack>    rig = Rig(Planted(-0.1, 0.0), Planted(0.1, 0.0), o);
        const PhysicsAnalysis           a = AnalyseMotion(rig);
        const std::vector<PhysicsEvent> body = HandOnBodyEvents(a);
        CHECK(body.size() == 1);
        if (body.size() == 1)
            CHECK(body[0].kind == PhysicsKind::HandOnBody && body[0].side == 'L' && std::fabs(body[0].time_s - 0.805) < 0.03);
        CHECK(ClapEvents(a).empty());
        CHECK(HandBodyDistance(a, 'L').size() == a.samples && HandBodyDistance(a, 'R').size() == a.samples);
        AutoSettings s;
        s.type[static_cast<int>(AutoType::HandOnBody)].on = true;
        const std::vector<Event> ev = DetectAutoEvents(AutoBlocks(s), rig, nullptr, false);
        CHECK(ev.size() == 1 && ev[0].marker == "Hand on body L");
        // A hand swinging past the thigh (15 cm aside) at 2 m/s, never stopping: none.
        Options swing = o;
        swing.hand[0] = toward(Vec3d{-0.25, 0.75, 0.6}, Vec3d{-0.25, 0.75, -0.9}, 0.5, 2.0);
        CHECK(HandOnBodyEvents(AnalyseMotion(Rig(Planted(-0.1, 0.0), Planted(0.1, 0.0), swing))).empty());
        // A hand that comes to rest slowly (0.5 m/s) against the thigh: none.
        Options rest = o;
        rest.hand[0] = toward(Vec3d{-0.6, 0.75, 0.3}, Vec3d{-0.1, 0.75, -0.02}, 0.5, 0.5);
        CHECK(HandOnBodyEvents(AnalyseMotion(Rig(Planted(-0.1, 0.0), Planted(0.1, 0.0), rest))).empty());
        // Review 10-9d: a slower touch (1 m/s, 1.15 leg/s) is none at the default sensitivity and
        // found at 100 %, but Detect's search never raises a touch type (no_sens_raise): the
        // block counts as searched, neither raised nor none. Clap is kept out the same way.
        {
            Options                      soft = o;
            soft.hand[0] = toward(Vec3d{-0.1, 0.75, 0.4}, Vec3d{-0.1, 0.75, -0.02}, 0.5, 1.0);  // head-on
            const std::vector<BoneTrack> srig = Rig(Planted(-0.1, 0.0), Planted(0.1, 0.0), soft);
            CHECK(HandOnBodyEvents(AnalyseMotion(srig)).empty());
            const PhysicsParams hi = AutoPhysicsParams(AutoType::HandOnBody, 100.0, false);
            CHECK(HandOnBodyEvents(AnalyseMotion(srig, hi), hi).size() == 1);
            std::vector<int> smap(static_cast<size_t>(Role::Count), -1);
            for (int r = 0; r < static_cast<int>(Role::Count); ++r)
                if (!srig[static_cast<size_t>(r)].pos.empty()) smap[static_cast<size_t>(r)] = r;
            AutoSettings ss;
            ss.type[static_cast<int>(AutoType::HandOnBody)].on = true;
            ss.type[static_cast<int>(AutoType::Clap)].on = true;
            const std::vector<Block>          sb = AutoBlocks(ss);
            const std::vector<AutoSensResult> res = SearchAutoSensitivity(sb, srig, std::vector<int>(sb.size(), 0), smap, false);
            CHECK(res.size() == sb.size() && sb.size() == 4);
            for (const AutoSensResult& r : res) CHECK(r.searched && !r.raised && !r.none && r.sens == 50.0);
            CHECK(AutoTypeRow(AutoType::HandOnBody).no_sens_raise && AutoTypeRow(AutoType::Clap).no_sens_raise);
        }
        // Review 10-9d: the body coming onto a hand held still (a climb's last pull) is no touch:
        // the same closing, but made by the body. The whole body moves 0.4 m forward at 2 m/s and
        // stops, its left thigh 12 cm behind the left hand, which never moves.
        {
            const double t0 = 0.5, run = 0.4, v = 2.0;
            auto shift = [=](double t) { return std::clamp((t - t0) * v, 0.0, run) - run; };
            Options still = o;
            still.hand[0] = [](double) { return Vec3d{-0.1, 0.75, -0.02}; };
            std::vector<BoneTrack> body = Rig(Planted(-0.1, 0.0), Planted(0.1, 0.0), still);
            for (size_t r = 0; r < body.size(); ++r) {
                if (r == static_cast<size_t>(Role::LeftHand) || r == static_cast<size_t>(Role::RightHand)) continue;
                BoneTrack& t = body[r];
                for (size_t i = 0; i < t.pos.size(); ++i) t.pos[i].z += shift(static_cast<double>(i) / 240.0);
            }
            const PhysicsAnalysis     ba = AnalyseMotion(body);
            const std::vector<double> d = HandBodyDistance(ba, 'L');
            CHECK(!d.empty() && d.front() > 0.4 && d.back() < 0.2);  // the closing does happen
            CHECK(HandOnBodyEvents(ba).empty());
            PhysicsParams any;
            any.body_hand_frac = 0.0;  // without the hand's own approach, it would be one
            CHECK(HandOnBodyEvents(ba, any).size() == 1);
        }
        // The parts: a hand on body needs a body segment; the hips and up legs give one.
        std::vector<int> map(static_cast<size_t>(Role::Count), -1);
        map[static_cast<size_t>(Role::LeftHand)] = 1;
        map[static_cast<size_t>(Role::RightHand)] = 2;
        CHECK(!HasBodySegment(map));
        map[static_cast<size_t>(Role::Spine)] = 3;
        CHECK(!HasBodySegment(map));
        map[static_cast<size_t>(Role::Head)] = 4;
        CHECK(HasBodySegment(map));  // spine -> head (the missing chest and neck skipped)
    }

    // Story 10-10a: confidence. The steps of 0..5 strictness steps a kept event survived.
    {
        CHECK(AutoConfidenceOfSteps(0) == 0.0 && AutoConfidenceOfSteps(kAutoConfidenceSteps) == 1.0);
        CHECK(std::fabs(AutoConfidenceOfSteps(2) - 0.4) < 1e-12 && AutoConfidenceOfSteps(9) == 1.0 &&
              AutoConfidenceOfSteps(-1) == 0.0);
    }
    // The slow slide (none at 50 %, one at 80 %): rated at 80 %, its confidence is the share of
    // 10-point steps below 80 it is still found at, in a row; the rule's events stay at 1.
    {
        const FootFn slide = [](double t) {
            FootPose f;
            f.ball = Vec3d{-0.1, 0.02, 0.18 * std::clamp(t - 1.0, 0.0, 0.4)};
            return f;
        };
        Options o;
        o.dur = 3.0;
        const std::vector<BoneTrack> rig = Rig(slide, Planted(0.1, 0.2), o);
        std::vector<Block>           blocks = AutoBlocks(Ticked(false, false, true, false));
        for (Block& b : blocks) b.sens = 80.0;
        std::vector<Event> ev = DetectAutoEvents(blocks, rig, nullptr, false);
        CHECK(OfBlock(ev, 0).size() == 1);
        for (const Event& e : ev) CHECK(e.confidence == 1.0);  // not rated yet
        Event rule_ev;
        rule_ev.block = 99;  // not an auto block: untouched
        rule_ev.confidence = 0.7;
        ev.push_back(rule_ev);
        std::vector<AutoAnalysis> probes;
        RateAutoEvents(ev, blocks, rig, &probes, false);
        CHECK(ev.back().confidence == 0.7);
        int kept = 0;  // the steps the slide survives, by hand: the block at 70, 60, 50...
        for (int k = 1; k <= kAutoConfidenceSteps; ++k) {
            std::vector<Block> stricter = blocks;
            for (Block& b : stricter) b.sens = 80.0 - kAutoConfidenceStepSens * k;
            if (OfBlock(DetectAutoEvents(stricter, rig, nullptr, false), 0).empty()) break;
            ++kept;
        }
        CHECK(kept < 3);  // gone by 50 %
        const std::vector<Event> sl = OfBlock(ev, 0);
        CHECK(sl.size() == 1 && std::fabs(sl[0].confidence - AutoConfidenceOfSteps(kept)) < 1e-12);
        CHECK(!probes.empty());
        // Without a cache, the same; the cache reused (no new analysis).
        std::vector<Event> ev2 = DetectAutoEvents(blocks, rig, nullptr, false);
        RateAutoEvents(ev2, blocks, rig, nullptr, false);
        const size_t n = probes.size();
        std::vector<Event> ev3 = DetectRatedAutoEvents(blocks, rig, nullptr, &probes, false);
        CHECK(probes.size() == n);
        CHECK(ev2.size() == ev3.size());
        for (size_t i = 0; i < ev2.size() && i < ev3.size(); ++i) CHECK(ev2[i].confidence == ev3[i].confidence);
        // Review of 10-10a: a drag's preview rates from the cache only. An empty cache: false,
        // nothing changed, no analysis run; the filled one: the same confidences, no new analysis.
        std::vector<Event> ev4 = DetectAutoEvents(blocks, rig, nullptr, false);
        for (Event& e : ev4) e.confidence = 0.5;
        std::vector<AutoAnalysis> none;
        CHECK(!RateAutoEventsFromCache(ev4, blocks, rig, &none, false) && none.empty());
        CHECK(!RateAutoEventsFromCache(ev4, blocks, rig, nullptr, false));
        for (const Event& e : ev4) CHECK(e.confidence == 0.5);
        CHECK(RateAutoEventsFromCache(ev4, blocks, rig, &probes, false) && probes.size() == n);
        CHECK(ev4.size() == ev2.size());
        for (size_t i = 0; i < ev2.size() && i < ev4.size(); ++i) CHECK(ev2[i].confidence == ev4[i].confidence);
    }
    // A clear walk: its steps and lift-offs rated, each 0..1, the steps of a firm walk confident.
    {
        const std::vector<BoneTrack> rig = Walk();
        const std::vector<Block>     blocks = AutoBlocks(Ticked(true, true, false, false));
        const std::vector<Event>     ev = DetectRatedAutoEvents(blocks, rig, nullptr, nullptr, false);
        CHECK(!ev.empty());
        int firm = 0;
        for (const Event& e : ev) {
            CHECK(e.confidence >= 0.0 && e.confidence <= 1.0);
            firm += e.confidence >= kDefaultDoubtLimit;
        }
        CHECK(firm == static_cast<int>(ev.size()));
    }

    if (g_fails) std::printf("%d check(s) failed\n", g_fails);
    else std::printf("auto_detect: all checks passed\n");
    return g_fails ? 1 : 0;
}
