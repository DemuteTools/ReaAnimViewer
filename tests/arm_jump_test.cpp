// SPDX-License-Identifier: MIT
//
// Host test of story 10-9k: Arms > Jump and Land (src/auto_types_arm_jump.cpp) on a synthetic
// body that jumps (take-off 1.0 s, landing 1.5 s), its left arm thrown up into the take-off and
// brought down at the landing, its right arm swinging later, away from the jump. Through Detect
// (DetectAutoEvents): the arms' events, and the Swing they replace when both are ticked.
//   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests

#include "auto_detect.h"
#include "auto_types.h"
#include "bone_roles.h"
#include "motion_physics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
constexpr double kRate = 240.0;
constexpr double kDur = 2.8;
constexpr double kTakeOff = 1.0, kFlight = 0.5, kG = 9.81;

double Smoothstep(double u)
{
    u = std::clamp(u, 0.0, 1.0);
    return u * u * (3.0 - 2.0 * u);
}

Vec3d V(double x, double y, double z)
{
    return Vec3d{x, y, z};
}

Vec3d Plus(const Vec3d& a, const Vec3d& b)
{
    return Vec3d{a.x + b.x, a.y + b.y, a.z + b.z};
}

// A point rotated about the X axis (forward = +Z, up = +Y) around `pivot` by `ang` radians.
Vec3d AboutX(const Vec3d& p, const Vec3d& pivot, double ang)
{
    const double y = p.y - pivot.y, z = p.z - pivot.z;
    return Vec3d{p.x, pivot.y + y * std::cos(ang) - z * std::sin(ang), pivot.z + y * std::sin(ang) + z * std::cos(ang)};
}

struct Body {
    bool jump = true;        // the whole body flies a ballistic arc from kTakeOff for kFlight
    bool left_arm = true;    // up 150 deg from 0.8 s (0.3 s), back down from 1.45 s (0.25 s)
    bool right_arm = true;   // up and down 120 deg from 2.1 s, away from the jump
    bool hips = true;
};

// The body's lift (m) at t: a ballistic flight (up at g x kFlight / 2).
double Lift(const Body& b, double t)
{
    if (!b.jump || t <= kTakeOff || t >= kTakeOff + kFlight) return 0.0;
    const double u = t - kTakeOff, v0 = kG * kFlight / 2.0;
    return v0 * u - 0.5 * kG * u * u;
}

double LeftArm(double t)
{
    const double up = 150.0 * kPi / 180.0;
    if (t < 1.45) return up * Smoothstep((t - 0.8) / 0.3);
    return up * (1.0 - Smoothstep((t - 1.45) / 0.25));
}

double RightArm(double t)
{
    const double up = 120.0 * kPi / 180.0;
    if (t < 2.35) return up * Smoothstep((t - 2.1) / 0.25);
    return up * (1.0 - Smoothstep((t - 2.35) / 0.25));
}

std::vector<BoneTrack> MakeBody(const Body& b)
{
    const size_t           n = static_cast<size_t>(kDur * kRate) + 1;
    std::vector<BoneTrack> rt(static_cast<size_t>(Role::Count));
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRate;
        const Vec3d  lift = V(0.0, Lift(b, t), 0.0);
        auto         put = [&](Role r, const Vec3d& p) { rt[static_cast<size_t>(r)].pos.push_back(Plus(p, lift)); };
        if (b.hips) put(Role::Hips, V(0.0, 0.95, 0.0));
        put(Role::Spine, V(0.0, 1.15, 0.0));
        put(Role::Chest, V(0.0, 1.35, 0.0));
        const Vec3d shl = V(0.2, 1.45, 0.0), shr = V(-0.2, 1.45, 0.0);
        put(Role::LeftShoulder, shl);
        put(Role::RightShoulder, shr);
        put(Role::LeftHand, AboutX(Plus(shl, V(0.0, -0.6, 0.0)), shl, b.left_arm ? LeftArm(t) : 0.0));
        put(Role::RightHand, AboutX(Plus(shr, V(0.0, -0.6, 0.0)), shr, b.right_arm ? RightArm(t) : 0.0));
        for (int s = 0; s < 2; ++s) {
            const double x = s ? -0.1 : 0.1;
            put(s ? Role::RightUpLeg : Role::LeftUpLeg, V(x, 0.95, 0.0));
            put(s ? Role::RightKnee : Role::LeftKnee, V(x, 0.5, 0.0));
            put(s ? Role::RightHeel : Role::LeftHeel, V(x, 0.08, -0.05));
            put(s ? Role::RightToe : Role::LeftToe, V(x, 0.02, 0.09));
            put(s ? Role::RightToeEnd : Role::LeftToeEnd, V(x, 0.0, 0.16));
        }
    }
    for (BoneTrack& t : rt) t.rate_hz = kRate;
    return rt;
}

std::vector<int> RoleMap(const std::vector<BoneTrack>& rt)
{
    std::vector<int> m(static_cast<size_t>(Role::Count), -1);
    for (size_t i = 0; i < rt.size(); ++i)
        if (!rt[i].pos.empty()) m[i] = static_cast<int>(i);
    return m;
}

AutoSettings On(std::initializer_list<AutoType> types, bool swing_start = false)
{
    AutoSettings s;
    for (AutoType t : types) s.type[static_cast<int>(t)].on = true;
    s.at_start[static_cast<int>(AutoType::Swing)] = swing_start;
    return s;
}

struct Found {
    std::string marker;
    double      t;
    double      strength;
};

std::vector<Found> Detect(const Body& b, const AutoSettings& s)
{
    const std::vector<BoneTrack> rt = MakeBody(b);
    std::vector<Block>           blocks = AutoBlocks(s);
    std::vector<AutoAnalysis>    cache;
    std::vector<Found>           out;
    for (const Event& e : DetectAutoEvents(blocks, rt, &cache, false)) out.push_back({e.marker, e.time_s, e.strength});
    return out;
}

std::vector<std::string> Markers(const std::vector<Found>& f)
{
    std::vector<std::string> out;
    for (const Found& e : f) out.push_back(e.marker);
    return out;
}

const Found* Get(const std::vector<Found>& f, const std::string& marker)
{
    for (const Found& e : f)
        if (e.marker == marker) return &e;
    return nullptr;
}

void TestPhysicsBase()
{
    // The rig jumps as Legs > Jump / Land see it: one take-off at 1.0 s, one landing at 1.5 s.
    const std::vector<Found> legs = Detect(Body{}, On({AutoType::Jump, AutoType::Land}));
    const Found *j = nullptr, *l = nullptr;
    for (const Found& e : legs) {
        if (e.marker.rfind("Jump", 0) == 0) j = &e;
        if (e.marker.rfind("Land", 0) == 0) l = &e;
    }
    CHECK(j && l);
    if (j) CHECK(std::fabs(j->t - kTakeOff) < 0.04);
    if (l) CHECK(std::fabs(l->t - (kTakeOff + kFlight)) < 0.05);
}

void TestArms()
{
    // Arms alone: the left arm's throw into the take-off (its fastest at 0.95 s) and its way down
    // at the landing (1.575 s); nothing on the right arm (its swing at 2.2 s follows no jump).
    const std::vector<Found> arms = Detect(Body{}, On({AutoType::ArmJump, AutoType::ArmLand}));
    CHECK((Markers(arms) == std::vector<std::string>{"Arms Jump L", "Arms Land L"}));
    if (const Found* e = Get(arms, "Arms Jump L")) CHECK(std::fabs(e->t - 0.95) < 0.02 && e->strength > 4.0);
    if (const Found* e = Get(arms, "Arms Land L")) CHECK(std::fabs(e->t - 1.575) < 0.02 && e->strength > 3.0);

    // Swing alone: the same arm motions are swings (and the right arm's).
    const std::vector<Found> sw = Detect(Body{}, On({AutoType::Swing}));
    CHECK((Markers(sw) == std::vector<std::string>{"Swing L", "Swing L", "Swing R", "Swing R"}));

    // Both ticked: the arms of the jump take their swings (one motion, one sound); the right
    // arm's stay Swing.
    const std::vector<Found> both = Detect(Body{}, On({AutoType::Swing, AutoType::ArmJump, AutoType::ArmLand}));
    CHECK((Markers(both) == std::vector<std::string>{"Arms Jump L", "Arms Land L", "Swing R", "Swing R"}));
    // Swing at its start (before the arms' peaks): replaced over the whole run too.
    const std::vector<Found> start =
        Detect(Body{}, On({AutoType::Swing, AutoType::ArmJump, AutoType::ArmLand}, true));
    CHECK((Markers(start) == std::vector<std::string>{"Arms Jump L", "Arms Land L", "Swing R", "Swing R"}));
    // Only one of them ticked: only its swing goes.
    const std::vector<Found> jonly = Detect(Body{}, On({AutoType::Swing, AutoType::ArmJump}));
    CHECK((Markers(jonly) == std::vector<std::string>{"Arms Jump L", "Swing L", "Swing R", "Swing R"}));

    // No jump (the same arms, the feet never leave the floor): no arm event, the swings stay.
    Body still;
    still.jump = false;
    CHECK(Detect(still, On({AutoType::ArmJump, AutoType::ArmLand})).empty());
    CHECK(Markers(Detect(still, On({AutoType::Swing, AutoType::ArmJump, AutoType::ArmLand}))).size() == 4);

    // The sensitivity divides the speed: at 0 (8 leg/s) this 9 leg/s throw is still one.
    AutoSettings low = On({AutoType::ArmJump});
    low.type[static_cast<int>(AutoType::ArmJump)].sens = 0.0;
    CHECK(Detect(Body{}, low).size() == 1);
}

void TestRows()
{
    // Missing parts: without hips no jump, so no arms of a jump.
    Body nohips;
    nohips.hips = false;
    const std::vector<int> map = RoleMap(MakeBody(nohips));
    CHECK(AutoBlockPartMissing(MakeAutoBlock(AutoKind::ArmJump, 'L', kAutoDefaultSens, 0.0), map));
    CHECK(AutoBlockPartMissing(MakeAutoBlock(AutoKind::ArmLand, 'R', kAutoDefaultSens, 0.0), map));
    const std::vector<int> full = RoleMap(MakeBody(Body{}));
    CHECK(!AutoBlockPartMissing(MakeAutoBlock(AutoKind::ArmJump, 'L', kAutoDefaultSens, 0.0), full));

    // Lanes: the hand's speed vs the type's speed, shaded around the take-offs (landings).
    AutoAnalysis aa;
    aa.params = AutoPhysicsParams(AutoType::ArmJump, kAutoDefaultSens, false);
    aa.analysis = AnalyseMotion(MakeBody(Body{}), aa.params);
    const std::vector<AutoLane> lanes = AutoBlockLanes(aa, MakeAutoBlock(AutoKind::ArmJump, 'L', kAutoDefaultSens, 0.0));
    CHECK(lanes.size() == 1);
    if (!lanes.empty()) {
        CHECK(lanes[0].above && lanes[0].threshold == aa.params.arm_jump_speed && lanes[0].unit == "leg/s");
        CHECK(std::count(lanes[0].contact.begin(), lanes[0].contact.end(), 1) > 0 && !lanes[0].shade_note.empty());
    }
}

}  // namespace

int main()
{
    TestPhysicsBase();
    TestArms();
    TestRows();
    if (g_fails) {
        std::printf("%d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("arm_jump: all checks passed\n");
    return 0;
}
