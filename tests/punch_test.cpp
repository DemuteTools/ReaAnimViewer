// SPDX-License-Identifier: MIT
//
// Host test of story 10-9o: Arms > Punch, every fist blow (src/auto_types_punch.cpp,
// src/motion_whoosh.cpp), on a synthetic standing body (facing +Z). Its right arm throws a hook
// (a horizontal arc from the side to the front, arm raised, stopping in 0.16 s), a straight
// thrust (the hand from bent to straight in front, with an elbow), a torso-driven thrust (the
// shoulder travelling with the hand: no punch), a low chop that stops dead (a hit; a
// weapon blow, a low straight), or the hook pulled back; its left arm a windmill swing that slows
// over 0.3 s (a follow-through), or a mirrored hook. Through Detect (DetectAutoEvents): the Punch
// at the arm's stop (Stop, the default), at the hand's fastest (Peak), at the start (Start); the
// Swing kept beside it when both are ticked (the whoosh at the peak, the Punch at the stop); the
// counter-arm's Swing dropped; nothing for no blow.
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

namespace rav {
// auto_types_punch.cpp (declared in auto_types.cpp): the punch source at the stop.
std::vector<PhysicsEvent> AutoSourcePunchStop(const PhysicsAnalysis& a, const PhysicsParams& p);
}  // namespace rav

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
constexpr double kArm = 0.6;  // m, shoulder -> hand (upper arm and forearm 0.3 each)
// The hook: wind-up from kHook0 for kHookUp s (speed rising as u^2), the peak at kHook0 + kHookUp,
// then a linear stop over kHookStop s.
constexpr double kHook0 = 0.8, kHookUp = 0.25, kHookPeak = 13.0;  // rad/s at the peak
// The straight thrust: from kThrust0 over kThrustD s (smoothstep), the hand from bent to straight.
constexpr double kThrust0 = 0.8, kThrustD = 0.15;

Vec3d V(double x, double y, double z)
{
    return Vec3d{x, y, z};
}

Vec3d Plus(const Vec3d& a, const Vec3d& b)
{
    return Vec3d{a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3d Lerp(const Vec3d& a, const Vec3d& b, double u)
{
    return Vec3d{a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u};
}

enum class Blow { Hook, PulledBack, Chop, Bounce, Thrust, WindUpThrust, Cross, None,
                  HookUp,      // the hook ending with the hand over the shoulder (a guard raised)
                  SideThrust,  // the straight thrust out to the side (no forward)
                  LoopThrust,  // a thrust whose stop passes the loop seam (2.88 s of a 3 s cycle)
                  Hammer,      // a downward blow stopping dead (60 ms) at the landing of a jump
                  Whip };      // the hand sweeping from the side to the front while the elbow opens

struct Body {
    Blow   right = Blow::Hook;
    Blow   left = Blow::None;     // Hook: the mirrored hook (at the right's time)
    double hook_stop = 0.16;      // s
    bool   windmill = true;       // the left arm's follow-through swing, from 2.0 s (left None)
    bool   right_hand = true;
    bool   jump = false;          // the whole body flies from 1.0 s, lands at 1.5 s
    int    palm = 0;              // the left palm where the hook ends: 1 held there, 2 parting after
};

constexpr double kTakeOff = 1.0, kFlight = 0.5;

// The body's lift (m) at t: a ballistic flight (up at g x kFlight / 2).
double Lift(const Body& b, double t)
{
    if (!b.jump || t <= kTakeOff || t >= kTakeOff + kFlight) return 0.0;
    const double u = t - kTakeOff, v0 = 9.81 * kFlight / 2.0;
    return v0 * u - 0.5 * 9.81 * u * u;
}

// A profile's angle at t (rad): the angular speed rising as u^2 over `up` s from t0 to `peak`,
// then falling linearly to 0 over `stop` s.
double ProfileAngle(double t, double t0, double up, double stop, double peak)
{
    double       a = 0.0;
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

double Smooth(double u)
{
    u = std::clamp(u, 0.0, 1.0);
    return u * u * (3.0 - 2.0 * u);
}

// The thrusts' progress at t (0 bent .. 1 straight). WindUp: a forward drift first, the
// extending speed dipping to 35 % of the blow's peak before it, without stopping.
double ThrustU(Blow b, double t)
{
    if (b == Blow::WindUpThrust) {
        // The progress speed: a drift (a sin^2 bell, half the blow's height, 0.45-0.6 s), never
        // under 35 % of the blow's peak from 0.5 to 0.7 s, then the blow (a bell, 0.6-0.75 s);
        // integrated and scaled to 0 .. 1.
        auto bell = [](double x, double a, double b) {
            if (x <= a || x >= b) return 0.0;
            const double s = std::sin(kPi * (x - a) / (b - a));
            return s * s;
        };
        auto w = [&](double x) {
            double v = 0.5 * bell(x, 0.45, 0.6) + bell(x, 0.6, 0.75);
            if (x > 0.5 && x < 0.7) v = std::max(v, 0.35);
            return v;
        };
        const double dt = 1.0 / (kRate * 8.0);
        double       u = 0.0, all = 0.0;
        for (double x = 0.4; x < 0.8; x += dt) {
            all += w(x) * dt;
            if (x < t) u += w(x) * dt;
        }
        return u / all;
    }
    if (b == Blow::Cross) return Smooth((t - kThrust0) / 0.25);  // slower: no dead stop (no hit)
    if (b == Blow::LoopThrust) {
        // One cycle: the thrust from 2.88 s through the seam, then a slow pull back at 0.88-1.48 s.
        const double tt = std::fmod(t - 2.88 + kDur, kDur);
        return Smooth(tt / kThrustD) - Smooth((tt - 1.0) / 0.6);
    }
    return Smooth((t - kThrust0) / kThrustD);
}

// The shoulder of a side at t: the cross carries the right one forward (the torso drives it).
Vec3d Shoulder(const Body& b, int s, double t)
{
    const Vec3d base = s ? V(-0.2, 1.45, 0.0) : V(0.2, 1.45, 0.0);
    if (s == 1 && b.right == Blow::Cross) return Plus(base, V(0.0, 0.0, 0.4 * ThrustU(Blow::Cross, t)));
    return base;
}

// A hook of a side (0 left: mirrored) at t, or pulled back / low (see Blow).
Vec3d HookHand(Blow blow, int s, const Body& b, const Vec3d& sh, double t)
{
    const bool   low = blow == Blow::Chop || blow == Blow::Bounce;
    const double el0 = low ? -0.75 : -0.2;
    const double stop = low ? 0.03 : b.hook_stop;
    const double peak = low ? 1.3 * kHookPeak : kHookPeak;  // the low arm's shorter reach
    const double total = ProfileAngle(1e9, kHook0, kHookUp, stop, peak);
    double       th = ProfileAngle(t, kHook0, kHookUp, stop, peak);
    if (blow == Blow::PulledBack) th = total - th;
    // Bounce: the chop's dead stop, then the hand snaps straight back (a reversal), no hit.
    if (blow == Blow::Bounce) th -= ProfileAngle(t, kHook0 + kHookUp + stop, 0.05, 0.2, 0.9 * peak);
    // HookUp: the arm rising as it sweeps, from -0.2 to +0.5 (the hand over the shoulder).
    const double el = blow == Blow::HookUp ? el0 + 0.7 * th / total : el0, c = std::sqrt(1.0 - el * el);
    const double side = s ? -1.0 : 1.0;  // the hand's own side: -X for the right
    return Plus(sh, V(side * std::cos(th) * c * kArm, el * kArm, std::sin(th) * c * kArm));
}

Vec3d RightHand(const Body& b, const Vec3d& sh, double t)
{
    switch (b.right) {
    case Blow::None: return Plus(sh, V(0.0, -kArm, 0.0));
    case Blow::SideThrust: {
        // The same thrust out to the right side (-X): the hand moving across the facing.
        const Vec3d a = Plus(sh, V(-0.2, -0.15, 0.0)), z = Plus(sh, V(-0.587, -0.05, 0.0));
        return Lerp(a, z, ThrustU(Blow::Thrust, t));
    }
    case Blow::Hammer: {
        // About X from 100 deg (raised in front) down to 40 deg (clear of the thigh: no hand on
        // body), its speed rising as u^2 over
        // 0.2 s to the landing (1.5 s), then stopping dead over 60 ms (it hits).
        const double sweep = 60.0 * kPi / 180.0, up = 0.2, stop = 0.06;
        const double peak = sweep / (up / 3.0 + stop / 2.0);
        const double phi = 100.0 * kPi / 180.0 - ProfileAngle(t, kTakeOff + kFlight - up, up, stop, peak);
        return Plus(sh, V(0.0, -std::cos(phi) * kArm, std::sin(phi) * kArm));
    }
    case Blow::Thrust:
    case Blow::LoopThrust:
    case Blow::WindUpThrust: {
        // From bent in front of the shoulder (reach 0.25 m) to straight forward (0.59 m).
        const Vec3d a = Plus(sh, V(0.0, -0.15, 0.2)), z = Plus(sh, V(0.0, -0.05, 0.587));
        return Lerp(a, z, ThrustU(b.right, t));
    }
    case Blow::Whip: {
        // Over 0.25 s (smoothstep) from kThrust0: the hand sweeps 120 deg about the shoulder, from
        // behind the side to the front, a little under the shoulder, while the elbow opens from 70
        // to 165 deg (reach 0.34 -> 0.59 m): fast (about 6 leg/s), its opening speed slow (about
        // 1.7 leg/s, under punch_speed and under punch_radial_frac of the hand's).
        const double u = Smooth((t - kThrust0) / 0.25);
        const double th = (-30.0 + 120.0 * u) * kPi / 180.0, el = (70.0 + 95.0 * u) * kPi / 180.0;
        const double r = 2.0 * 0.3 * std::sin(el / 2.0);
        return Plus(sh, V(-std::cos(th) * r * 0.99, -0.1 * r, std::sin(th) * r * 0.99));
    }
    case Blow::Cross: {
        // The hand travels 0.6 m while the shoulder travels 0.4: the arm opens 0.41 -> 0.54 m.
        // Low, the arm under the shoulder (rise about -0.6, as Fist Fight's left 2.8625).
        const Vec3d a = Plus(sh, V(0.0, -0.33, 0.25)), z = Plus(sh, V(0.0, -0.3, 0.45));
        const double u = ThrustU(Blow::Cross, t);
        return Lerp(a, z, u);
    }
    default: return HookHand(b.right, 1, b, sh, t);
    }
}

// The left hand: a windmill about X through the shoulder from 60 deg behind, its speed a bell
// (sin^2) over 0.7 s peaking at 12 rad/s: it slows down over 0.2 s and more; or a mirrored hook.
Vec3d LeftHand(const Body& b, const Vec3d& sh, double t)
{
    if (b.left == Blow::Hook) return HookHand(Blow::Hook, 0, b, sh, t);
    if (b.palm) {
        // The palm 8 cm beyond the hook's end, along its way (the right hand stops on it);
        // parting: it leaves 0.4 m to the left 20 ms after the hook stops.
        const Vec3d  shr = Shoulder(b, 1, t);
        const Vec3d  end = HookHand(Blow::Hook, 1, b, shr, 1e9);
        const double th = ProfileAngle(1e9, kHook0, kHookUp, b.hook_stop, kHookPeak);
        Vec3d        palm = Plus(end, V(0.08 * std::sin(th), 0.0, 0.08 * std::cos(th)));
        if (b.palm == 2) palm = Plus(palm, V(0.4 * Smooth((t - (kHook0 + kHookUp + b.hook_stop + 0.02)) / 0.1), 0.0, 0.0));
        return palm;
    }
    double phi = -60.0 * kPi / 180.0;
    if (b.windmill && t > 2.0) {
        const double D = 0.7, W = 12.0;
        const double u = std::min((t - 2.0) / D, 1.0);
        phi += W * D * (u / 2.0 - std::sin(2.0 * kPi * u) / (4.0 * kPi));
    }
    return Plus(sh, V(0.0, -std::cos(phi) * kArm, std::sin(phi) * kArm));
}

// The elbow between a shoulder and a hand (two 0.3 m bones), bent downward.
Vec3d Elbow(const Vec3d& sh, const Vec3d& h)
{
    const double dx = h.x - sh.x, dy = h.y - sh.y, dz = h.z - sh.z, d = std::sqrt(dx * dx + dy * dy + dz * dz);
    const double off = std::sqrt(std::max(0.0, 0.09 - 0.25 * d * d));
    return V(0.5 * (sh.x + h.x), 0.5 * (sh.y + h.y) - off, 0.5 * (sh.z + h.z));
}

std::vector<BoneTrack> MakeBody(const Body& b)
{
    const size_t           n = static_cast<size_t>(kDur * kRate) + 1;
    std::vector<BoneTrack> rt(static_cast<size_t>(Role::Count));
    const bool             elbows = b.right == Blow::Thrust || b.right == Blow::WindUpThrust || b.right == Blow::Cross ||
                        b.right == Blow::LoopThrust || b.right == Blow::Whip;
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRate;
        const Vec3d  lift = V(0.0, Lift(b, t), 0.0);
        auto         put = [&](Role r, const Vec3d& p) { rt[static_cast<size_t>(r)].pos.push_back(Plus(p, lift)); };
        put(Role::Hips, V(0.0, 0.95, 0.0));
        put(Role::Spine, V(0.0, 1.15, 0.0));
        put(Role::Chest, V(0.0, 1.35, 0.0));
        const Vec3d shl = Shoulder(b, 0, t), shr = Shoulder(b, 1, t);
        put(Role::LeftShoulder, shl);
        put(Role::RightShoulder, shr);
        const Vec3d hl = LeftHand(b, shl, t);
        put(Role::LeftHand, hl);
        if (b.right_hand) {
            const Vec3d hr = RightHand(b, shr, t);
            put(Role::RightHand, hr);
            if (elbows) put(Role::RightElbow, Elbow(shr, hr));
        }
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

enum class Mark { Stop, Peak, Start };

AutoSettings On(std::initializer_list<AutoType> types, Mark punch = Mark::Stop, bool swing_start = false)
{
    AutoSettings s;
    for (AutoType t : types) s.type[static_cast<int>(t)].on = true;
    s.at_start[static_cast<int>(AutoType::Swing)] = swing_start;
    s.at_start[static_cast<int>(AutoType::Punch)] = punch == Mark::Start;
    s.punch_peak = punch == Mark::Peak;
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

const std::vector<std::string> kPunchR{"Punch R"};

void TestArcBlows()
{
    const double peak_t = kHook0 + kHookUp;
    // The hook alone: a Punch at its stop (the speed under 40 % of the peak: 96 ms after it in a
    // linear 0.16 s stop), Peak at its fastest, Start before both; the windmill (it swings on)
    // is none.
    const std::vector<Found> st = Detect(Body{}, On({AutoType::Punch}));
    CHECK(Markers(st) == kPunchR);
    const Found* stop = Get(st, "Punch R");
    if (stop) CHECK(stop->t > peak_t + 0.07 && stop->t < peak_t + 0.12 && stop->strength > 6.0);
    const std::vector<Found> pk = Detect(Body{}, On({AutoType::Punch}, Mark::Peak));
    CHECK(Markers(pk) == kPunchR);
    if (const Found* e = Get(pk, "Punch R")) CHECK(std::fabs(e->t - peak_t) < 0.03);
    const std::vector<Found> sa = Detect(Body{}, On({AutoType::Punch}, Mark::Start));
    CHECK(Markers(sa) == kPunchR);
    if (const Found* e = Get(sa, "Punch R")) CHECK(e->t < peak_t - 0.03);

    // Swing alone: both are swings (a user who wants arm foley ticks Swing).
    CHECK((Markers(Detect(Body{}, On({AutoType::Swing}))) == std::vector<std::string>{"Swing R", "Swing L"}));
    // Both ticked: Swing and Punch are independent (2026-10-24): the hook keeps its swing beside
    // its punch, the windmill stays; at its stop, its peak or its start, and with Swing at its
    // start too.
    for (Mark m : {Mark::Stop, Mark::Peak, Mark::Start})
        for (bool ss : {false, true}) {
            std::vector<std::string> got = Markers(Detect(Body{}, On({AutoType::Swing, AutoType::Punch}, m, ss)));
            std::sort(got.begin(), got.end());
            CHECK((got == std::vector<std::string>{"Punch R", "Swing L", "Swing R"}));
        }
    // The hook, both ticked: the Swing at the hand's fastest, the Punch at the arm's stop, apart.
    const std::vector<Found> both = Detect(Body{}, On({AutoType::Swing, AutoType::Punch}));
    const Found*             bs = Get(both, "Swing R");
    const Found*             bp = Get(both, "Punch R");
    CHECK(bs && bp);
    if (bs && bp) {
        CHECK(std::fabs(bs->t - peak_t) < 0.03);
        CHECK(bp->t > peak_t + 0.07 && bp->t < peak_t + 0.12);
        CHECK(bp->t - bs->t > 0.05);
    }

    // The same hook pulled back (the hand moving backward): no punch. A hook slowing over 0.3 s
    // swings on: no punch.
    Body back;
    back.right = Blow::PulledBack;
    CHECK(Detect(back, On({AutoType::Punch})).empty());
    Body slow;
    slow.hook_stop = 0.30;
    CHECK(Detect(slow, On({AutoType::Punch})).empty());

    // A low blow stopping dead (30 ms: a hit, a weapon into the floor, a low body shot): a punch.
    // The same blow snapping straight back after its stop (a reversal): none.
    Body chop;
    chop.right = Blow::Chop;
    CHECK(Markers(Detect(chop, On({AutoType::Punch}))) == kPunchR);
    Body bounce;
    bounce.right = Blow::Bounce;
    CHECK(Detect(bounce, On({AutoType::Punch})).empty());

    // No blow at all: none.
    Body none;
    none.right = Blow::None;
    none.windmill = false;
    CHECK(Detect(none, On({AutoType::Punch, AutoType::Swing})).empty());

    // The sensitivity divides the speeds: at 0 (the arc speed 10 leg/s) the hook (about 8.8) is
    // none, at 100 still one punch.
    AutoSettings lo = On({AutoType::Punch});
    lo.type[static_cast<int>(AutoType::Punch)].sens = 0.0;
    CHECK(Detect(Body{}, lo).empty());
    AutoSettings hi = On({AutoType::Punch});
    hi.type[static_cast<int>(AutoType::Punch)].sens = 100.0;
    CHECK(Markers(Detect(Body{}, hi)) == kPunchR);
}

void TestCounterArm()
{
    // The left hand hooks while the right is pulled back at the same moment: the left is a
    // Punch (and its hook's Swing L, the whoosh); the right, moving backward within 0.1 s of it,
    // is the counter-arm: no Swing, no Punch. Swing alone: the right's swing stays dropped (it is
    // no blow's whoosh either).
    Body b;
    b.right = Blow::PulledBack;
    b.left = Blow::Hook;
    b.windmill = false;
    std::vector<std::string> cb = Markers(Detect(b, On({AutoType::Punch, AutoType::Swing})));
    std::sort(cb.begin(), cb.end());
    CHECK((cb == std::vector<std::string>{"Punch L", "Swing L"}));
    const std::vector<std::string> sw = Markers(Detect(b, On({AutoType::Swing})));
    CHECK(std::find(sw.begin(), sw.end(), "Swing R") == sw.end());
    // Without the other hand's blow, the arm pulled back is a Swing.
    Body alone;
    alone.right = Blow::PulledBack;
    alone.windmill = false;
    CHECK((Markers(Detect(alone, On({AutoType::Punch, AutoType::Swing}))) == std::vector<std::string>{"Swing R"}));
}

void TestStraight()
{
    // The straight thrust: one Punch, at the stop (the arm at 99 % of its farthest reach, near
    // the end of a 0.15 s smoothstep), Peak at its fastest.
    Body th;
    th.right = Blow::Thrust;
    th.windmill = false;
    const double             peak_t = kThrust0 + 0.5 * kThrustD;
    const std::vector<Found> st = Detect(th, On({AutoType::Punch}));
    CHECK(Markers(st) == kPunchR);
    if (const Found* e = Get(st, "Punch R")) CHECK(e->t > peak_t + 0.025 && e->t <= kThrust0 + kThrustD + 0.01);
    const std::vector<Found> pk = Detect(th, On({AutoType::Punch}, Mark::Peak));
    CHECK(Markers(pk) == kPunchR);
    if (const Found* e = Get(pk, "Punch R")) CHECK(std::fabs(e->t - peak_t) < 0.015);
    // Swing ticked too: this thrust's hand stays under Swing's speed, so no Swing (Swing is read
    // on its own, Punch does not remove it).
    CHECK(Markers(Detect(th, On({AutoType::Punch, AutoType::Swing}))) == kPunchR);

    // A wind-up (a slow drift forward whose extending speed dips) then the blow: one Punch, on
    // the blow.
    Body wu;
    wu.right = Blow::WindUpThrust;
    wu.windmill = false;
    const std::vector<Found> w = Detect(wu, On({AutoType::Punch}));
    CHECK(Markers(w) == kPunchR);
    if (const Found* e = Get(w, "Punch R")) CHECK(e->t > 0.675);

    // A low torso-driven thrust: the shoulder travels with the hand, so the arm's own opening is
    // under 40 % of the hand's speed, the arm stays under the shoulder and slows without a dead
    // stop (no arc, no hit): no Punch (Antho, Fist Fight's left 2.8625: a punch start
    // blocked, a Swing; the elbow-opening test of the first build was dropped).
    Body cr;
    cr.right = Blow::Cross;
    cr.windmill = false;
    CHECK(Detect(cr, On({AutoType::Punch})).empty());
}

int CountPunch(const std::vector<PhysicsEvent>& ev)
{
    return static_cast<int>(std::count_if(ev.begin(), ev.end(), [](const PhysicsEvent& e) { return e.kind == PhysicsKind::Punch; }));
}

void TestDetails()
{
    const PhysicsParams p = AutoPhysicsParams(AutoType::Punch, kAutoDefaultSens, false);
    // (a) A downward blow stopping dead at a jump's landing, at its fastest where Arms Land marks
    // the arm coming down: alone each finds it; both ticked, only the Punch (its marker at the
    // stop, more than 30 ms after the peak: replaced over its span, replaces_span).
    Body hm;
    hm.right = Blow::Hammer;
    hm.windmill = false;
    hm.jump = true;
    CHECK(Markers(Detect(hm, On({AutoType::Punch}))) == kPunchR);
    const std::vector<std::string> al = Markers(Detect(hm, On({AutoType::ArmLand})));
    CHECK(std::find(al.begin(), al.end(), "Arms Land R") != al.end());
    const std::vector<std::string> both = Markers(Detect(hm, On({AutoType::Punch, AutoType::ArmLand})));
    CHECK(std::find(both.begin(), both.end(), "Punch R") != both.end() &&
          std::find(both.begin(), both.end(), "Arms Land R") == both.end());

    // (b) Strength = the hand's speed relative to the torso at its peak (not the radial speed,
    // which is lower on this diagonal thrust).
    Body th;
    th.right = Blow::Thrust;
    th.windmill = false;
    const PhysicsAnalysis ta = AnalyseMotion(MakeBody(th), p);
    const std::vector<Found> pk = Detect(th, On({AutoType::Punch}, Mark::Peak));
    if (ta.ok && pk.size() == 1) {
        const size_t i = static_cast<size_t>(std::lround(pk[0].t * kRate));
        const double v = ta.whoosh_hand[1].speed[i], r = ta.whoosh_hand[1].radial[i];
        CHECK(std::fabs(pk[0].strength - v) < 1e-6 && std::fabs(v - r) > 0.05 * v);
    } else {
        CHECK(false);
    }

    // (c) The wind-up's drift then the blow, the extending speed dipping between them without
    // stopping: at its start, the punch starts after the drift (split at the dip); without the
    // split, one run from the drift's start.
    Body wu;
    wu.right = Blow::WindUpThrust;
    wu.windmill = false;
    const PhysicsAnalysis wa = AnalyseMotion(MakeBody(wu), p);
    const std::vector<PhysicsEvent> split = WhooshEvents(wa, p, WhooshMark::Start);
    PhysicsParams nosplit = p;
    nosplit.punch_split_frac = 0.0;
    const std::vector<PhysicsEvent> merged = WhooshEvents(wa, nosplit, WhooshMark::Start);
    CHECK(CountPunch(split) == 1 && CountPunch(merged) == 1);
    for (const PhysicsEvent& e : split)
        if (e.kind == PhysicsKind::Punch) CHECK(e.time_s > 0.55);  // the dip at about 0.58 s
    for (const PhysicsEvent& e : merged)
        if (e.kind == PhysicsKind::Punch) CHECK(e.time_s < 0.52);  // the drift at about 0.5 s

    // (d) The same straight thrust out to the side: no Punch; a Punch with the forward share's
    // minimum lifted.
    Body sd;
    sd.right = Blow::SideThrust;
    sd.windmill = false;
    const PhysicsAnalysis sa = AnalyseMotion(MakeBody(sd), p);
    CHECK(CountPunch(WhooshEvents(sa, p, WhooshMark::Stop)) == 0);
    PhysicsParams anyway = p;
    anyway.punch_min_fwd = -2.0;
    CHECK(CountPunch(WhooshEvents(sa, anyway, WhooshMark::Stop)) == 1);

    // (e) An arc in the air ending with the hand over the shoulder (a guard raised): no Punch.
    Body up;
    up.right = Blow::HookUp;
    up.windmill = false;
    CHECK(Detect(up, On({AutoType::Punch})).empty());

    // (f) The hook ending in the other hand's palm: the hands held together after it (an elbow
    // blow pushed into the palm) leave the Punch; a palm that parts at once makes it a clap: none.
    Body held;
    held.windmill = false;
    held.palm = 1;
    const std::vector<std::string> hc = Markers(Detect(held, On({AutoType::Clap})));
    CHECK(hc.size() == 1);  // the hands meet as a clap
    CHECK(Markers(Detect(held, On({AutoType::Punch}))) == kPunchR);
    Body parts = held;
    parts.palm = 2;
    CHECK(Markers(Detect(parts, On({AutoType::Clap}))).size() == 1);
    CHECK(Detect(parts, On({AutoType::Punch})).empty());

    // (g) A loop whose thrust stops past the seam: its stop wraps to the cycle's start, its span
    // with it (start <= time <= end on one cycle), so it replaces nothing but its own swing.
    Body lp;
    lp.right = Blow::LoopThrust;
    lp.windmill = false;
    PhysicsParams lpp = p;
    lpp.looping = true;
    const PhysicsAnalysis la = AnalyseMotion(MakeBody(lp), lpp);
    const double          T = kDur;
    int                   n = 0;
    for (const PhysicsEvent& e : AutoSourcePunchStop(la, lpp)) {
        ++n;
        CHECK(e.time_s >= 0.0 && e.time_s < 0.1);  // the stop past the seam (about 3.01 s)
        CHECK(e.start_s <= e.time_s && e.time_s <= e.end_s && e.end_s - e.start_s < 0.5 * T);
    }
    CHECK(n == 1);
}

void TestRows()
{
    // Missing parts: without the right hand, the right punch's part is missing.
    Body nohand;
    nohand.right_hand = false;
    const std::vector<int> map = RoleMap(MakeBody(nohand));
    for (AutoKind k : {AutoKind::Punch, AutoKind::PunchPeak, AutoKind::PunchStart}) {
        CHECK(AutoBlockPartMissing(MakeAutoBlock(k, 'R', kAutoDefaultSens, 0.0), map));
        CHECK(!AutoBlockPartMissing(MakeAutoBlock(k, 'L', kAutoDefaultSens, 0.0), map));
    }
    // Lanes: the extending speed (the straight test), then the hand's speed vs the arc speed,
    // shaded where the arm makes an arc in front.
    AutoAnalysis aa;
    aa.params = AutoPhysicsParams(AutoType::Punch, kAutoDefaultSens, false);
    aa.analysis = AnalyseMotion(MakeBody(Body{}), aa.params);
    const std::vector<AutoLane> lanes = AutoBlockLanes(aa, MakeAutoBlock(AutoKind::Punch, 'R', kAutoDefaultSens, 0.0));
    CHECK(lanes.size() == 2);
    if (lanes.size() == 2) {
        CHECK(lanes[0].threshold == aa.params.punch_speed);
        CHECK(lanes[1].above && lanes[1].threshold == aa.params.punch_arc_speed && lanes[1].unit == "leg/s");
        CHECK(std::count(lanes[1].contact.begin(), lanes[1].contact.end(), 1) > 0 && !lanes[1].shade_note.empty());
    }

    // The settings: Stop by default; Peak and Start switch the one block (its place kept).
    AutoSettings s = On({AutoType::Punch});
    std::vector<Block> blocks = AutoBlocks(s);
    CHECK(blocks.size() == 2 && blocks[0].auto_type == "punch");
    s.punch_peak = true;
    CHECK(AutoPending(s, ReadAutoSettings(blocks)));
    ApplyAutoSettings(blocks, s, nullptr);
    CHECK(blocks.size() == 2 && blocks[0].auto_type == "punch_peak");
    CHECK(ReadAutoSettings(blocks).punch_peak && !ReadAutoSettings(blocks).at_start[static_cast<int>(AutoType::Punch)]);
    s.at_start[static_cast<int>(AutoType::Punch)] = true;
    ApplyAutoSettings(blocks, s, nullptr);
    CHECK(blocks.size() == 2 && blocks[0].auto_type == "punch_start");
    CHECK(ReadAutoSettings(blocks).at_start[static_cast<int>(AutoType::Punch)]);
    s.at_start[static_cast<int>(AutoType::Punch)] = false;
    s.punch_peak = false;
    ApplyAutoSettings(blocks, s, nullptr);
    CHECK(blocks.size() == 2 && blocks[0].auto_type == "punch" && !ReadAutoSettings(blocks).punch_peak);

    // A project saved with 10-9n's Strike: its block is an unknown word, kept inert (no event,
    // never dropped by Detect, no crash).
    std::vector<Block> old = AutoBlocks(On({AutoType::Punch}));
    Block strike = old[0];
    strike.auto_type = "strike";
    old.push_back(strike);
    AutoKind k;
    CHECK(!AutoBlockKind(strike, &k, nullptr) && !AutoKindFromWord("strike", nullptr));
    ApplyAutoSettings(old, ReadAutoSettings(old), nullptr);
    CHECK(old.size() == 3 && old[2].auto_type == "strike");
    std::vector<AutoAnalysis> cache;
    for (const Event& e : DetectAutoEvents(old, MakeBody(Body{}), &cache, false)) CHECK(e.block != 2);
}

}  // namespace

// Story 10-9o: a straight blow read from the elbow (the radial test failing) and the contact stop.
void TestElbowAndContact()
{
    // The whip: the hand fast, its opening slow; the elbow opens 95 deg and ends straight: a
    // straight punch (motion_whoosh.cpp's runs, not an arc blow). Without the elbow test: none.
    Body wb;
    wb.right = Blow::Whip;
    wb.windmill = false;
    const PhysicsParams   p = AutoPhysicsParams(AutoType::Punch, kAutoDefaultSens, false);
    const PhysicsAnalysis a = AnalyseMotion(MakeBody(wb), p);
    CHECK(CountPunch(WhooshEvents(a, p, WhooshMark::Peak)) == 1);
    PhysicsParams no_open = p;
    no_open.punch_open_deg = 360.0;
    CHECK(CountPunch(WhooshEvents(a, no_open, WhooshMark::Peak)) == 0);
    // Its hand never reaching punch_arc_speed: none (a slow opening needs a fast hand).
    PhysicsParams slow = p;
    slow.punch_arc_speed = 50.0;
    CHECK(CountPunch(WhooshEvents(a, slow, WhooshMark::Peak)) == 0);
    // Nor when the elbow must end straighter than it does.
    PhysicsParams bent = p;
    bent.punch_open_end_deg = 170.0;
    CHECK(CountPunch(WhooshEvents(a, bent, WhooshMark::Peak)) == 0);

    // The contact stop on a hand track: the speed rising to its peak at sample 20, on a plateau
    // (96 %) to 30, then braking to 10 % by 40.
    WhooshTrack h;
    for (int i = 0; i < 80; ++i) {
        double v = i <= 20 ? 6.0 * i / 20.0 : i <= 30 ? 6.0 - 0.024 * (i - 20) : std::max(0.6, 5.76 - 0.516 * (i - 30));
        h.speed.push_back(v);
        h.elbow.push_back(std::min(175.0, 60.0 + 2.0 * i));  // 100 deg at the peak, 120 at 30
    }
    // The elbow at 120 deg as the hand brakes (over punch_contact_elbow_deg): a blow in the air.
    CHECK(PunchContactStop(h, 20, kRate, p) == -1);
    // Bent at the brake (104 deg) and never past 140: a hit, stopped where the speed first falls
    // under 90 % of the peak (5.4: sample 31).
    for (int i = 0; i < 80; ++i) h.elbow[static_cast<size_t>(i)] = std::min(140.0, 70.0 + i);
    CHECK(PunchContactStop(h, 20, kRate, p) == 31);
    // The elbow passing punch_contact_max_elbow_deg within the stop window: in the air.
    h.elbow[60] = 155.0;
    CHECK(PunchContactStop(h, 20, kRate, p) == -1);
    // No elbow: the caller's stop.
    h.elbow.clear();
    CHECK(PunchContactStop(h, 20, kRate, p) == -1);
}

int main()
{
    TestArcBlows();
    TestCounterArm();
    TestStraight();
    TestDetails();
    TestElbowAndContact();
    TestRows();
    if (g_fails) {
        std::printf("%d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("punch: all checks passed\n");
    return 0;
}
