// SPDX-License-Identifier: MIT
//
// Host test of story 10-9n: Arms > Strike (src/auto_types_strike.cpp) on a synthetic standing
// body (facing +Z): its right arm throws a hook (a horizontal arc from the side to the front, arm
// raised, accelerating then stopping in 0.16 s), its left arm a big windmill swing that slows down
// over 0.3 s (a follow-through). Through Detect (DetectAutoEvents): the strike, and the Swing it
// replaces when both are ticked; the same hook pulled back is none; a low blow that stops dead
// (a hit) is one.
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
constexpr double kDur = 3.0;
constexpr double kArm = 0.6;  // m, shoulder -> hand
// The hook: wind-up from kHook0 for kHookUp s (speed rising as u^2), the peak at kHook0 + kHookUp,
// then a linear stop over kHookStop s.
constexpr double kHook0 = 0.8, kHookUp = 0.25, kHookPeak = 13.0;  // rad/s at the peak

Vec3d V(double x, double y, double z)
{
    return Vec3d{x, y, z};
}

Vec3d Plus(const Vec3d& a, const Vec3d& b)
{
    return Vec3d{a.x + b.x, a.y + b.y, a.z + b.z};
}

enum class Blow { Hook, PulledBack, Chop, Bounce, None };

struct Body {
    Blow   right = Blow::Hook;
    double hook_stop = 0.16;   // s
    bool   windmill = true;    // the left arm's follow-through swing, from 2.0 s
    bool   right_hand = true;
};

// A profile's angle at t (rad): the angular speed rising as u^2 over `up` s from t0 to `peak`,
// then falling linearly to 0 over `stop` s.
double ProfileAngle(double t, double t0, double up, double stop, double peak)
{
    double a = 0.0;
    const double dt = 1.0 / (kRate * 8.0);
    const double end = std::min(t, t0 + up + stop);
    for (double s = t0; s < end; s += dt) {
        const double u = (s - t0) / up;
        double       w = 0.0;
        if (u < 1.0) w = peak * u * u;
        else if (s - t0 - up < stop) w = peak * (1.0 - (s - t0 - up) / stop);
        a += w * dt;
    }
    return a;
}

// The right hand at t: a hook sweeps the yaw from the side (-X) to the front (+Z) and across,
// the arm raised (elevation -0.2: rise -0.2); pulled back, the same sweep from the front to the
// side; a chop, the same sweep with the arm low (rise -0.75), stopping in 30 ms (a hit).
Vec3d RightHand(const Body& b, const Vec3d& sh, double t)
{
    if (b.right == Blow::None) return Plus(sh, V(0.0, -kArm, 0.0));
    const bool   low = b.right == Blow::Chop || b.right == Blow::Bounce;
    const double el = low ? -0.75 : -0.2, c = std::sqrt(1.0 - el * el);
    const double stop = low ? 0.03 : b.hook_stop;
    const double peak = low ? 1.3 * kHookPeak : kHookPeak;  // the low arm's shorter reach
    const double total = ProfileAngle(1e9, kHook0, kHookUp, stop, peak);
    double       th = ProfileAngle(t, kHook0, kHookUp, stop, peak);
    if (b.right == Blow::PulledBack) th = total - th;
    // Bounce: the chop's dead stop, then the hand snaps straight back (its speed back to 0.9 of
    // the peak within 50 ms): a reversal (a feint, a dance pop), no hit.
    if (b.right == Blow::Bounce) th -= ProfileAngle(t, kHook0 + kHookUp + stop, 0.05, 0.2, 0.9 * peak);
    return Plus(sh, V(-std::cos(th) * c * kArm, el * kArm, std::sin(th) * c * kArm));
}

// The left hand: a windmill about X through the shoulder from 60 deg behind, its speed a bell
// (sin^2) over 0.7 s peaking at 12 rad/s: it slows down over 0.2 s and more.
Vec3d LeftHand(const Body& b, const Vec3d& sh, double t)
{
    double phi = -60.0 * kPi / 180.0;
    if (b.windmill && t > 2.0) {
        const double D = 0.7, W = 12.0;
        const double u = std::min((t - 2.0) / D, 1.0);
        phi += W * D * (u / 2.0 - std::sin(2.0 * kPi * u) / (4.0 * kPi));
    }
    return Plus(sh, V(0.0, -std::cos(phi) * kArm, std::sin(phi) * kArm));
}

std::vector<BoneTrack> MakeBody(const Body& b)
{
    const size_t           n = static_cast<size_t>(kDur * kRate) + 1;
    std::vector<BoneTrack> rt(static_cast<size_t>(Role::Count));
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRate;
        auto         put = [&](Role r, const Vec3d& p) { rt[static_cast<size_t>(r)].pos.push_back(p); };
        put(Role::Hips, V(0.0, 0.95, 0.0));
        put(Role::Spine, V(0.0, 1.15, 0.0));
        put(Role::Chest, V(0.0, 1.35, 0.0));
        const Vec3d shl = V(0.2, 1.45, 0.0), shr = V(-0.2, 1.45, 0.0);
        put(Role::LeftShoulder, shl);
        put(Role::RightShoulder, shr);
        put(Role::LeftHand, LeftHand(b, shl, t));
        if (b.right_hand) put(Role::RightHand, RightHand(b, shr, t));
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

void TestStrike()
{
    const double peak_t = kHook0 + kHookUp;
    // Strike alone: the hook, at its fastest; the windmill (it swings on) is none.
    const std::vector<Found> st = Detect(Body{}, On({AutoType::Strike}));
    CHECK((Markers(st) == std::vector<std::string>{"Strike R"}));
    if (const Found* e = Get(st, "Strike R")) CHECK(std::fabs(e->t - peak_t) < 0.03 && e->strength > 6.0);

    // Swing alone: both are swings.
    const std::vector<Found> sw = Detect(Body{}, On({AutoType::Swing}));
    CHECK((Markers(sw) == std::vector<std::string>{"Swing R", "Swing L"}));

    // Both ticked: the strike takes the hook's swing (one motion, one sound), the windmill stays.
    CHECK((Markers(Detect(Body{}, On({AutoType::Swing, AutoType::Strike}))) == std::vector<std::string>{"Strike R", "Swing L"}));
    // Swing at its start (before the hook's peak): replaced over the whole run too.
    CHECK((Markers(Detect(Body{}, On({AutoType::Swing, AutoType::Strike}, true))) ==
           std::vector<std::string>{"Strike R", "Swing L"}));

    // The same hook pulled back (the hand moving backward), stopping as slowly: no strike.
    Body back;
    back.right = Blow::PulledBack;
    CHECK(Detect(back, On({AutoType::Strike})).empty());
    // A hook that slows down over 0.3 s swings on: no strike.
    Body slow;
    slow.hook_stop = 0.30;
    CHECK(Detect(slow, On({AutoType::Strike})).empty());

    // A low blow stopping dead (30 ms, a hit), the arm under the shoulder's level: a strike.
    Body chop;
    chop.right = Blow::Chop;
    CHECK((Markers(Detect(chop, On({AutoType::Strike}))) == std::vector<std::string>{"Strike R"}));

    // The same low blow snapping straight back after its stop (a reversal): no strike.
    Body bounce;
    bounce.right = Blow::Bounce;
    CHECK(Detect(bounce, On({AutoType::Strike})).empty());

    // No blow at all: none.
    Body none;
    none.right = Blow::None;
    none.windmill = false;
    CHECK(Detect(none, On({AutoType::Strike, AutoType::Swing})).empty());

    // The sensitivity divides the speed: at 0 (10 leg/s) the hook (about 8.8) is none, at 100
    // (2.5 leg/s) still one strike.
    AutoSettings lo = On({AutoType::Strike});
    lo.type[static_cast<int>(AutoType::Strike)].sens = 0.0;
    CHECK(Detect(Body{}, lo).empty());
    AutoSettings hi = On({AutoType::Strike});
    hi.type[static_cast<int>(AutoType::Strike)].sens = 100.0;
    CHECK((Markers(Detect(Body{}, hi)) == std::vector<std::string>{"Strike R"}));
}

void TestRows()
{
    // Missing parts: without the right hand, the right strike's part is missing.
    Body nohand;
    nohand.right_hand = false;
    const std::vector<int> map = RoleMap(MakeBody(nohand));
    CHECK(AutoBlockPartMissing(MakeAutoBlock(AutoKind::Strike, 'R', kAutoDefaultSens, 0.0), map));
    CHECK(!AutoBlockPartMissing(MakeAutoBlock(AutoKind::Strike, 'L', kAutoDefaultSens, 0.0), map));

    // Lanes: the hand's speed vs the type's speed, shaded where the arm makes an arc in front.
    AutoAnalysis aa;
    aa.params = AutoPhysicsParams(AutoType::Strike, kAutoDefaultSens, false);
    aa.analysis = AnalyseMotion(MakeBody(Body{}), aa.params);
    const std::vector<AutoLane> lanes = AutoBlockLanes(aa, MakeAutoBlock(AutoKind::Strike, 'R', kAutoDefaultSens, 0.0));
    CHECK(lanes.size() == 1);
    if (!lanes.empty()) {
        CHECK(lanes[0].above && lanes[0].threshold == aa.params.strike_speed && lanes[0].unit == "leg/s");
        CHECK(std::count(lanes[0].contact.begin(), lanes[0].contact.end(), 1) > 0 && !lanes[0].shade_note.empty());
    }
}

}  // namespace

int main()
{
    TestStrike();
    TestRows();
    if (g_fails) {
        std::printf("%d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("strike: all checks passed\n");
    return 0;
}
