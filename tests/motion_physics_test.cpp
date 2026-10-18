// SPDX-License-Identifier: MIT
//
// Host test of the motion physics (src/motion_physics.h, Epic 10, spike 10-8a; hands: story
// 10-8c; looping: 10-8i): the specs' matrix rows on synthetic feet and hands. No REAPER, no Windows: any C++17
// compiler.
//   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests

#include "bone_roles.h"
#include "motion_physics.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
constexpr double kDeg = kPi / 180.0;

// Deterministic noise in [-a, a] (a small LCG), one value per call.
struct Noise {
    uint32_t state = 12345u;
    double   operator()(double a)
    {
        state = state * 1664525u + 1013904223u;
        return a * (2.0 * (static_cast<double>(state >> 8) / 16777216.0) - 1.0);
    }
};

// One foot at one instant: the ball (toe base) position, the heading (yaw, radians, 0 = +Z),
// the pitch (radians, the heel raised about the ball) and the toe pitch (radians, the toes
// raised about the ball). Flat, the heel (the foot bone, the ankle) is 14 cm behind the ball
// and 6 cm above it, the tip 7 cm ahead and 2 cm below.
struct FootPose {
    Vec3d  ball;
    double yaw = 0.0;
    double pitch = 0.0;
    double toe_pitch = 0.0;
};

Vec3d Yawed(double yaw, double x, double y, double z)
{
    return Vec3d{x * std::cos(yaw) + z * std::sin(yaw), y, -x * std::sin(yaw) + z * std::cos(yaw)};
}

Vec3d Add(const Vec3d& a, const Vec3d& b)
{
    return Vec3d{a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3d HeelOf(const FootPose& f)
{
    const double y = 0.06 * std::cos(f.pitch) + 0.14 * std::sin(f.pitch);
    const double z = 0.06 * std::sin(f.pitch) - 0.14 * std::cos(f.pitch);
    return Add(f.ball, Yawed(f.yaw, 0.0, y, z));
}

Vec3d TipOf(const FootPose& f)
{
    const double c = std::cos(f.toe_pitch), s = std::sin(f.toe_pitch);
    return Add(f.ball, Yawed(f.yaw, 0.0, -0.02 * c + 0.07 * s, 0.02 * s + 0.07 * c));
}

Quatd Mul(const Quatd& a, const Quatd& b)
{
    return Quatd{a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                 a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

// A rotation of `angle` radians about the unit axis (x, y, z).
Quatd AxisAngle(double x, double y, double z, double angle)
{
    const double s = std::sin(0.5 * angle);
    return Quatd{std::cos(0.5 * angle), x * s, y * s, z * s};
}

// The toe bone's world rotation: the heading, then the toes raised (+Z towards +Y), then a
// fixed bone-space frame (a rig's bone axes are not the foot's), so the across-the-foot axis
// in bone space is not a bone axis.
Quatd ToeRotationOf(const FootPose& f)
{
    static const Quatd kBoneFrame = Mul(AxisAngle(0.0, 0.0, 1.0, 1.1), AxisAngle(0.6, 0.0, 0.8, -0.7));
    return Mul(Mul(AxisAngle(0.0, 1.0, 0.0, f.yaw), AxisAngle(1.0, 0.0, 0.0, -f.toe_pitch)), kBoneFrame);
}

using FootFn = std::function<FootPose(double)>;

struct Options {
    double dur = 4.0;
    double scale = 1.0;       // every position times this (a bigger character)
    double ground = 0.0;      // m/s: an in-place clip, everything moves back on Z
    double climb = 0.0;       // m/s: an in-place climb, everything moves down on Y
    double noise = 0.0;       // +- m on every axis of the foot parts
    bool   toes = true;       // false: no toe and no toe end bones
    bool   same_tip = false;  // the toe end plays on the toe's bone
    bool   tip = true;        // false: no toe end bone (the toe stays)
    bool   rot = false;       // the toe bones carry their world rotation (ToeRotationOf)
    double rate = 240.0;      // samples per second
};

// One track per Role: both feet, the knee 42 cm and the up leg 87 cm above the heel (a 0.87 m
// leg), the hips between the up legs.
std::vector<BoneTrack> Rig(const FootFn& left, const FootFn& right, const Options& o)
{
    std::vector<BoneTrack> t(static_cast<size_t>(Role::Count));
    Noise                  noise;
    const int              n = static_cast<int>(std::floor(o.dur * o.rate + 1e-9)) + 1;
    auto put = [&](Role r, const Vec3d& p, bool jitter) {
        Vec3d q = p;
        if (jitter) {
            q.x += noise(o.noise);
            q.y += noise(o.noise);
            q.z += noise(o.noise);
        }
        BoneTrack& b = t[static_cast<size_t>(r)];
        b.rate_hz = o.rate;
        b.pos.push_back(Vec3d{q.x * o.scale, q.y * o.scale, q.z * o.scale});
    };
    for (int i = 0; i < n; ++i) {
        const double time = i / o.rate;
        const Vec3d  back{0.0, -o.climb * time, -o.ground * time};
        Vec3d        up_legs[2];
        for (int s = 0; s < 2; ++s) {
            const FootPose f = s ? right(time) : left(time);
            const Vec3d    heel = Add(HeelOf(f), back), ball = Add(f.ball, back), tip = Add(TipOf(f), back);
            const char     side = s ? 'R' : 'L';
            put(FootPartRole(side, 0), heel, o.noise > 0.0);
            if (o.toes) {
                put(FootPartRole(side, 1), ball, o.noise > 0.0);
                if (o.rot) t[static_cast<size_t>(FootPartRole(side, 1))].rot_world.push_back(ToeRotationOf(f));
                if (!o.same_tip && o.tip) put(FootPartRole(side, 2), tip, o.noise > 0.0);
            }
            put(s ? Role::RightKnee : Role::LeftKnee, Add(heel, Vec3d{0.0, 0.42, 0.0}), false);
            up_legs[s] = Add(heel, Vec3d{0.0, 0.87, 0.0});
            put(s ? Role::RightUpLeg : Role::LeftUpLeg, up_legs[s], false);
        }
        put(Role::Hips, Vec3d{0.5 * (up_legs[0].x + up_legs[1].x), 0.5 * (up_legs[0].y + up_legs[1].y) + 0.05,
                              0.5 * (up_legs[0].z + up_legs[1].z)},
            false);
    }
    if (o.same_tip) {
        t[static_cast<size_t>(Role::LeftToeEnd)] = t[static_cast<size_t>(Role::LeftToe)];
        t[static_cast<size_t>(Role::RightToeEnd)] = t[static_cast<size_t>(Role::RightToe)];
    }
    return t;
}

double Ease(double u)
{
    u = std::clamp(u, 0.0, 1.0);
    return 0.5 - 0.5 * std::cos(kPi * u);
}

// A walk: each foot is planted from each contact start for 0.6 s, then swings 0.4 s, 0.6 m
// forward (eased) with a 10 cm lift (it lands moving down at about 0.8 m/s). The left foot
// swings first (its contacts start at 0.4, 1.4, 2.4, 3.4 s); the right one is planted at t = 0
// (its contacts start at 0, then 0.9, 1.9, 2.9, 3.9 s).
FootFn WalkFoot(double first_contact, double x)
{
    return [first_contact, x](double t) {
        const double cycle = 1.0, swing = 0.4;
        // k = the index of the contact at or before t (contact k starts at first + k).
        const double rel = t - first_contact;
        const int    k = static_cast<int>(std::floor(rel / cycle));
        const double in = rel - k * cycle;  // time since contact k started
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

// Story 10-8t: a run. Each foot is planted for 0.3 s, then swings 0.5 s, 1.2 m forward (eased)
// with a 15 cm lift; the feet 0.4 s apart, so a 0.1 s flight follows each contact. The left
// contacts start at first_contact + 0.8 k. `stride`: the swing's length (m).
FootFn RunFoot(double first_contact, double x, double stride = 1.2)
{
    return [first_contact, x, stride](double t) {
        const double cycle = 0.8, contact = 0.3;
        const double rel = t - first_contact;
        const int    k = static_cast<int>(std::floor(rel / cycle));
        const double in = rel - k * cycle;
        FootPose     f;
        f.ball.x = x;
        if (in < contact) {
            f.ball.z = stride * k;
            f.ball.y = 0.02;
        } else {
            const double u = (in - contact) / (cycle - contact);
            f.ball.z = stride * (k + Ease(u));
            f.ball.y = 0.02 + 0.15 * std::sin(kPi * u);
        }
        return f;
    };
}

// A foot planted at (x, 0, z), turned by yaw(t).
FootFn Planted(double x, double z, const std::function<double(double)>& yaw = nullptr)
{
    return [=](double t) {
        FootPose f;
        f.ball = Vec3d{x, 0.02, z};
        f.yaw = yaw ? yaw(t) : 0.0;
        return f;
    };
}

// Story 10-9a: a foot planted at (x, 0, 0) that leaves at t0 and flies a ballistic arc (gravity
// 9.81 m/s^2) of `flight` s, moving `forward` m/s on Z during it (landing that much further, a
// run's stride). `period` > 0: the motion repeats with that period (a looping clip).
FootFn JumpFoot(double x, double t0, double flight, double period = 0.0, double forward = 0.0)
{
    return [=](double t) {
        FootPose f;
        f.ball = Vec3d{x, 0.02, 0.0};
        double u = t - t0;
        if (period > 0.0) u = std::fmod(u + 10.0 * period, period);
        if (u > 0.0 && u < flight) {
            const double v0 = 9.81 * flight / 2.0;
            f.ball.y += v0 * u - 0.5 * 9.81 * u * u;
        }
        f.ball.z = forward * std::clamp(u, 0.0, flight);
        return f;
    };
}

std::vector<PhysicsEvent> Of(const std::vector<PhysicsEvent>& ev, PhysicsKind kind, char side = 0, int part = -2)
{
    std::vector<PhysicsEvent> out;
    for (const PhysicsEvent& e : ev)
        if (e.kind == kind && (!side || e.side == side) && (part == -2 || e.part == part)) out.push_back(e);
    return out;
}

// Speed timing, no offset: the module's mechanics alone.
PhysicsParams SpeedTiming()
{
    PhysicsParams p;
    for (int q = 0; q < kFootPartCount; ++q) {
        p.step_timing[q] = StepTiming::Speed;
        p.step_offset_s[q] = 0.0;
    }
    return p;
}

// A foot that swings from 1.0 s and reaches the floor at 1.4 s (the walk's 10 cm arc, landing
// at about 0.8 m/s down), moving forward on Z all along: its horizontal speed ramps up to
// `speed` (leg/s, a 0.87 m leg) over 50 ms, stays there until `glide_s` after the landing, then
// falls to 0 over 40 ms (it stops at 1.44 + glide_s).
FootFn GlideFoot(double glide_s, double speed)
{
    return [glide_s, speed](double t) {
        const double a = 1.0, r = 0.05, land = 1.4, dcl = 0.04;
        const double v = speed * 0.87, tg = land + glide_s;
        const double zg = v * r / 2.0 + v * (tg - a - r);
        FootPose     f;
        f.ball.y = 0.02;
        if (t < a)
            f.ball.z = 0.0;
        else if (t < a + r)
            f.ball.z = v * (t - a) * (t - a) / (2.0 * r);
        else if (t < tg)
            f.ball.z = v * r / 2.0 + v * (t - a - r);
        else if (t < tg + dcl)
            f.ball.z = zg + v * (t - tg) - v * (t - tg) * (t - tg) / (2.0 * dcl);
        else
            f.ball.z = zg + v * dcl / 2.0;
        if (t >= a && t < land) f.ball.y = 0.02 + 0.10 * std::sin(kPi * (t - a) / (land - a));
        return f;
    };
}

// Each combined step is the earliest separate step of its landing (that side's steps within
// `span` of it).
bool CombinedIsEarliest(const std::vector<PhysicsEvent>& ev, double span)
{
    for (const PhysicsEvent& f : ev) {
        if (f.kind != PhysicsKind::FootStep) continue;
        double first = HUGE_VAL;
        for (const PhysicsEvent& s : ev)
            if (s.kind == PhysicsKind::Step && s.side == f.side && std::fabs(s.time_s - f.time_s) <= span)
                first = std::min(first, s.time_s);
        if (first != f.time_s) return false;
    }
    return true;
}

// A foot whose heel follows `heel(t)` and whose pitch is `pitch(t)` (negative = toe up).
FootPose FromHeel(const Vec3d& heel, double pitch)
{
    FootPose f;
    f.pitch = pitch;
    f.ball = Vec3d{heel.x, heel.y - (0.06 * std::cos(pitch) + 0.14 * std::sin(pitch)),
                   heel.z - (0.06 * std::sin(pitch) - 0.14 * std::cos(pitch))};
    return f;
}

bool StepsNear(const std::vector<PhysicsEvent>& steps, const std::vector<double>& truth, double tol)
{
    if (steps.size() != truth.size()) return false;
    for (size_t k = 0; k < steps.size(); ++k)
        if (std::fabs(steps[k].time_s - truth[k]) > tol) return false;
    return true;
}

// Story 10-8c: a hand's position over time, and the rig's tracks with both hands added (an empty
// function: no bone for that hand). Same length and rate as Rig's. Story 10-8e: a hand's heading
// over time (radians about +Y; an empty function: no rotation on that hand's track), as the
// hand bone's world rotation HandRotationOf.
using HandFn = std::function<Vec3d(double)>;
using YawFn = std::function<double(double)>;

// The hand bone's world rotation: the heading about +Y, then a fixed bone-space frame (a rig's
// hand axes are not the world's, and the palm is not flat in bone space).
Quatd HandBoneFrame()
{
    return Mul(AxisAngle(0.0, 0.0, 1.0, 0.9), AxisAngle(0.8, 0.0, 0.6, 0.5));
}

Quatd HandRotationOf(double yaw)
{
    return Mul(AxisAngle(0.0, 1.0, 0.0, yaw), HandBoneFrame());
}

// v rotated by the unit quaternion q.
Vec3d RotateBy(const Quatd& q, const Vec3d& v)
{
    const Quatd r = Mul(Mul(q, Quatd{0.0, v.x, v.y, v.z}), Quatd{q.w, -q.x, -q.y, -q.z});
    return Vec3d{r.x, r.y, r.z};
}

void AddHands(std::vector<BoneTrack>& t, const HandFn& left, const HandFn& right, const Options& o,
              const YawFn& left_yaw = nullptr, const YawFn& right_yaw = nullptr)
{
    const int n = static_cast<int>(std::floor(o.dur * o.rate + 1e-9)) + 1;
    for (int s = 0; s < 2; ++s) {
        const HandFn& fn = s ? right : left;
        const YawFn&  yaw = s ? right_yaw : left_yaw;
        if (!fn) continue;
        BoneTrack& b = t[static_cast<size_t>(s ? Role::RightHand : Role::LeftHand)];
        b.rate_hz = o.rate;
        for (int i = 0; i < n; ++i) {
            b.pos.push_back(fn(i / o.rate));
            if (yaw) b.rot_world.push_back(HandRotationOf(yaw(i / o.rate)));
        }
    }
}

// A heading that turns by `deg` degrees over [a, a + len) (eased), still before and after.
YawFn TurnYaw(double a, double len, double deg)
{
    return [=](double t) { return deg * kDeg * Ease((t - a) / len); };
}

// A hand that moves along `dir` (unit) at `speed` m/s, except while at rest over the [a, b)
// spans: at rest it holds where it stopped. `x` sets it apart from the other hand.
HandFn HandMove(double x, double speed, std::vector<std::pair<double, double>> rests)
{
    return [=](double t) {
        double moving = t;  // time spent moving up to t
        for (const auto& r : rests) moving -= std::clamp(t, r.first, r.second) - r.first;
        const double d = speed * moving;
        return Vec3d{x, 1.4 - 0.6 * d, 0.8 * d};  // forward and down
    };
}

// A still hand, hanging by the hip.
HandFn HandStill(double x)
{
    return [x](double) { return Vec3d{x, 0.8, 0.0}; };
}

// Story 10-8i: a looping hand. At rest at (x, 1.0, 0.3) over [0, rest_a) and [rest_b, period],
// in between it runs once round a square (four sides of equal time, in the vertical plane
// facing it) at `speed` m/s, back where it started: the clip is one closed cycle.
HandFn HandLoop(double x, double speed, double rest_a, double rest_b)
{
    return [=](double t) {
        Vec3d p{x, 1.0, 0.3};
        if (t <= rest_a || t >= rest_b) return p;
        const double side = (rest_b - rest_a) / 4.0, len = speed * side;
        const double u = t - rest_a;
        const int    k = std::min(3, static_cast<int>(u / side));
        const double d = (u - k * side) * speed;
        const double corner[4][2] = {{0.0, 0.0}, {0.0, len}, {len, len}, {len, 0.0}};  // (up, forward)
        const double dir[4][2] = {{0.0, 1.0}, {1.0, 0.0}, {0.0, -1.0}, {-1.0, 0.0}};
        p.y += corner[k][0] + d * dir[k][0];
        p.z += corner[k][1] + d * dir[k][1];
        return p;
    };
}

// Every track shifted by `d` times t / dur (a slow rigid drift: the end pose = the start pose + d).
void Drift(std::vector<BoneTrack>& t, const Vec3d& d, double dur)
{
    for (BoneTrack& b : t)
        for (size_t i = 0; i < b.pos.size(); ++i) {
            const double f = (i / b.rate_hz) / dur;
            b.pos[i] = Add(b.pos[i], Vec3d{d.x * f, d.y * f, d.z * f});
        }
}

}  // namespace

int main()
{
    const std::vector<double> kLeft = {0.4, 1.4, 2.4, 3.4}, kRight = {0.9, 1.9, 2.9, 3.9};
    const FootFn              walk_l = WalkFoot(-0.6, -0.1), walk_r = WalkFoot(-0.1, 0.1);
    {
        // The walk itself: body scale, ground frame, one step per contact start per part, the
        // combined step and the lift-offs; no scuff.
        const PhysicsAnalysis a = AnalyseMotion(Rig(walk_l, walk_r, Options{}));
        CHECK(a.ok && a.missing.empty());
        CHECK(std::fabs(a.leg_length - 0.87) < 1e-6);
        CHECK(std::fabs(a.ground_velocity.x) < 1e-3 && std::fabs(a.ground_velocity.z) < 1e-3);
        const std::vector<PhysicsEvent> ev = FootEvents(a, SpeedTiming());
        for (int q = 0; q < kFootPartCount; ++q) {
            CHECK(StepsNear(Of(ev, PhysicsKind::Step, 'L', q), kLeft, 0.025));
            CHECK(StepsNear(Of(ev, PhysicsKind::Step, 'R', q), kRight, 0.025));
        }
        CHECK(StepsNear(Of(ev, PhysicsKind::FootStep, 'L'), kLeft, 0.025));
        CHECK(StepsNear(Of(ev, PhysicsKind::FootStep, 'R'), kRight, 0.025));
        // A flat foot lands on its three parts at once: the first in part order names the step.
        for (const PhysicsEvent& e : Of(ev, PhysicsKind::FootStep)) CHECK(e.part == static_cast<int>(FootPart::Heel));
        // Lift-offs where each swing starts, never at the clip end.
        CHECK(StepsNear(Of(ev, PhysicsKind::LiftOff, 'L'), {1.0, 2.0, 3.0}, 0.025));
        CHECK(StepsNear(Of(ev, PhysicsKind::LiftOff, 'R'), {0.5, 1.5, 2.5, 3.5}, 0.025));
        CHECK(Of(ev, PhysicsKind::Slide).empty() && Of(ev, PhysicsKind::Pivot).empty());
        for (const PhysicsEvent& e : Of(ev, PhysicsKind::Step)) CHECK(e.strength > 0.5 && e.strength < 3.0);  // leg/s
        // At rest from start: the right foot is planted at t = 0: no step there.
        for (const PhysicsEvent& e : ev)
            if (e.kind == PhysicsKind::Step || e.kind == PhysicsKind::FootStep) CHECK(e.time_s > 0.3);
        // The default timing: each part's steps are its Descent times plus that part's offset,
        // and the descent ends where the swing lands (the smoothing, sigma 8 ms, delays the end
        // of an abrupt 0.79 m/s descent by about 1.2 sigma: 10 ms).
        const PhysicsParams             dp;
        const std::vector<PhysicsEvent> def = FootEvents(a);
        CHECK(Of(def, PhysicsKind::Step).size() == 24);
        for (char side : {'L', 'R'})
            for (int q = 0; q < kFootPartCount; ++q) {
                CHECK(dp.step_timing[q] == StepTiming::Descent);
                std::vector<double> got;
                for (const PhysicsEvent& e : Of(def, PhysicsKind::Step, side, q)) got.push_back(e.time_s);
                CHECK(got == PartStepTimes(a, side, q, StepTiming::Descent, dp.step_offset_s[q]));
                const std::vector<double>  d = PartStepTimes(a, side, q, StepTiming::Descent, 0.0);
                const std::vector<double>& truth = side == 'L' ? kLeft : kRight;
                CHECK(d.size() == truth.size());
                for (size_t k = 0; k < d.size() && k < truth.size(); ++k)
                    CHECK(d[k] - truth[k] >= 0.005 && d[k] - truth[k] <= 0.015);
            }
        CHECK(CombinedIsEarliest(def, 0.4));
        // Deterministic.
        const std::vector<PhysicsEvent> again = FootEvents(AnalyseMotion(Rig(walk_l, walk_r, Options{})), SpeedTiming());
        CHECK(again.size() == ev.size());
        for (size_t i = 0; i < ev.size() && i < again.size(); ++i)
            CHECK(again[i].time_s == ev[i].time_s && PhysicsMarkerName(again[i]) == PhysicsMarkerName(ev[i]));

        // Body units: the same walk on a character 1.5 times bigger gives the same steps.
        Options big;
        big.scale = 1.5;
        const PhysicsAnalysis b = AnalyseMotion(Rig(walk_l, walk_r, big));
        CHECK(b.ok && std::fabs(b.leg_length - 1.305) < 1e-6);
        const std::vector<PhysicsEvent> bev = FootEvents(b, SpeedTiming());
        CHECK(Of(bev, PhysicsKind::Step).size() == Of(ev, PhysicsKind::Step).size());
        for (size_t i = 0; i < bev.size() && i < ev.size(); ++i) CHECK(std::fabs(bev[i].time_s - ev[i].time_s) < 1e-3);
    }
    {
        // Step timing on a gliding landing (story 10-8f): a Height or Descent crossing where the
        // part still moves faster than a swing (slide_max_speed) keeps the Speed time.
        Options o;
        o.dur = 3.0;
        const FootFn still = Planted(-0.2, 0.0);
        auto times = [](const PhysicsAnalysis& a, int q, StepTiming t) { return PartStepTimes(a, 'L', q, t, 0.0); };
        // Gliding: lands at 1.4 s, glides 200 ms at 3 leg/s, stops at 1.64 s: the step is where it
        // stops, not where its descent ends.
        {
            const PhysicsAnalysis a = AnalyseMotion(Rig(GlideFoot(0.2, 3.0), still, o));
            CHECK(a.ok);
            for (int q = 0; q < kFootPartCount; ++q) {
                const std::vector<double> sp = times(a, q, StepTiming::Speed);
                const std::vector<double> d = times(a, q, StepTiming::Descent);
                CHECK(sp.size() == 1 && d.size() == 1);
                if (sp.size() != 1 || d.size() != 1) continue;
                CHECK(sp[0] > 1.6 && sp[0] < 1.66);
                CHECK(d == sp);
                CHECK(times(a, q, StepTiming::Height) == sp);
            }
            // The default timing follows (each part's Descent plus its offset).
            CHECK(StepsNear(Of(FootEvents(a), PhysicsKind::FootStep, 'L'), {1.61}, 0.02));
        }
        // Slow slide-in: lands at 1.4 s, slides 150 ms at 1 leg/s (a slide, not a swing): the
        // step stays at the end of the descent.
        {
            const PhysicsAnalysis a = AnalyseMotion(Rig(GlideFoot(0.15, 1.0), still, o));
            CHECK(a.ok);
            for (int q = 0; q < kFootPartCount; ++q) {
                const std::vector<double> sp = times(a, q, StepTiming::Speed);
                const std::vector<double> d = times(a, q, StepTiming::Descent);
                CHECK(sp.size() == 1 && d.size() == 1);
                if (sp.size() != 1 || d.size() != 1) continue;
                CHECK(sp[0] > 1.55);
                CHECK(d[0] - 1.4 >= 0.0 && d[0] - 1.4 <= 0.025);
            }
        }
        // Glide past the search window: the descent ends 440 ms before the stop (search_back_s
        // is 300 ms), so Descent finds no crossing and keeps the Speed time, as before.
        {
            const PhysicsAnalysis a = AnalyseMotion(Rig(GlideFoot(0.4, 3.0), still, o));
            CHECK(a.ok);
            for (int q = 0; q < kFootPartCount; ++q) {
                const std::vector<double> sp = times(a, q, StepTiming::Speed);
                CHECK(sp.size() == 1 && sp[0] > 1.8);
                CHECK(times(a, q, StepTiming::Descent) == sp);
            }
        }
    }
    {
        // Jitter: +-2 mm on every axis of every foot part: 4 steps per part and foot, no extra.
        Options o;
        o.noise = 0.002;
        const PhysicsAnalysis a = AnalyseMotion(Rig(walk_l, walk_r, o));
        CHECK(a.ok);
        const std::vector<PhysicsEvent> ev = FootEvents(a, SpeedTiming());
        for (int q = 0; q < kFootPartCount; ++q) {
            CHECK(StepsNear(Of(ev, PhysicsKind::Step, 'L', q), kLeft, 0.025));
            CHECK(StepsNear(Of(ev, PhysicsKind::Step, 'R', q), kRight, 0.025));
        }
        CHECK(Of(ev, PhysicsKind::FootStep).size() == 8);
        CHECK(Of(ev, PhysicsKind::Slide).empty() && Of(ev, PhysicsKind::Pivot).empty());
    }
    {
        // In-place walk: the walk with its 0.6 m/s taken off (the planted foot slides back at
        // 0.6 m/s). The ground frame reads it, and the steps are the moving walk's. Story 10-8t:
        // the frame is per sample (0.6 m/s at every one) with a slack of 15 % of its speed on
        // the thresholds, so a step crosses its threshold a little earlier and a lift-off a little
        // later (about 2.5 ms here).
        Options o;
        o.ground = 0.6;
        const PhysicsAnalysis a = AnalyseMotion(Rig(walk_l, walk_r, o));
        CHECK(a.ok && std::fabs(a.ground_velocity.z + 0.6) < 1e-3 && std::fabs(a.ground_velocity.x) < 1e-3);
        CHECK(a.ground_vel.size() == a.samples && a.ground_slack.size() == a.samples);
        for (size_t i = 0; i < a.ground_vel.size(); ++i)
            CHECK(std::fabs(a.ground_vel[i].z + 0.6) < 1e-3 && std::fabs(a.ground_slack[i] - 0.15 * 0.6 / a.leg_length) < 1e-3);
        const std::vector<PhysicsEvent> ev = FootEvents(a, SpeedTiming());
        const std::vector<PhysicsEvent> moving = FootEvents(AnalyseMotion(Rig(walk_l, walk_r, Options{})), SpeedTiming());
        CHECK(ev.size() == moving.size());
        for (PhysicsKind kind : {PhysicsKind::Step, PhysicsKind::FootStep, PhysicsKind::LiftOff})
            for (char side : {'L', 'R'})
                for (int q = -2; q < kFootPartCount; ++q) {  // -2: every part
                    const std::vector<PhysicsEvent> x = Of(ev, kind, side, q), y = Of(moving, kind, side, q);
                    CHECK(x.size() == y.size());
                    for (size_t i = 0; i < x.size() && i < y.size(); ++i) {
                        const double d = x[i].time_s - y[i].time_s;
                        CHECK(std::fabs(d) < 0.004);
                        if (kind == PhysicsKind::Step) CHECK(d < -0.001);     // the slack: earlier
                        if (kind == PhysicsKind::LiftOff) CHECK(d > 0.001);   // and later
                    }
                }
    }
    {
        // In-place climb (story 10-8g): the walk, both hands on a rung, with a 0.5 m/s climb
        // taken off (every planted part slides down at 0.57 leg/s, past the contact speed; the
        // lowest part is often the swinging foot). The ground frame reads it from the limbs that
        // move together, and the foot events are the moving walk's, at the default timing too.
        // In place, the hips stay where they are, bobbing with the walk (story 10-8t: the climb
        // frame needs the hips' travel over the clip below half the climbing speed).
        auto climbing = [&](double v) {
            Options o;
            o.climb = v;
            std::vector<BoneTrack> t = Rig(walk_l, walk_r, o);
            BoneTrack& hips = t[static_cast<size_t>(Role::Hips)];
            for (size_t i = 0; i < hips.pos.size(); ++i) {
                hips.pos[i].y += v * static_cast<double>(i) / o.rate;
                hips.pos[i].z = hips.pos[0].z;
            }
            auto rung = [v](double x) { return [v, x](double tm) { return Vec3d{x, 1.6 - v * tm, 0.3}; }; };
            AddHands(t, rung(-0.3), rung(0.3), o);
            return AnalyseMotion(t);
        };
        const PhysicsAnalysis a = climbing(0.5);
        CHECK(a.ok && std::fabs(a.ground_velocity.y + 0.5) < 1e-3 && std::fabs(a.ground_velocity.z) < 1e-3);
        const PhysicsAnalysis moving = climbing(0.0);
        for (const PhysicsParams& tp : {SpeedTiming(), PhysicsParams{}}) {
            const std::vector<PhysicsEvent> x = FootEvents(a, tp), y = FootEvents(moving, tp);
            CHECK(x.size() == y.size());
            for (size_t i = 0; i < x.size() && i < y.size(); ++i)
                CHECK(PhysicsMarkerName(x[i]) == PhysicsMarkerName(y[i]) && std::fabs(x[i].time_s - y[i].time_s) < 0.002);
        }
        // A slower drift (0.2 m/s, 0.23 leg/s, under ground_vertical_min) stays in the frame.
        CHECK(climbing(0.2).ground_velocity.y == 0.0);
        // Feet only (two limbs): no climb frame, the walk's horizontal one as before.
        Options o;
        o.climb = 0.5;
        CHECK(AnalyseMotion(Rig(walk_l, walk_r, o)).ground_velocity.y == 0.0);
        // Story 10-8t: the climb frame is one constant frame, with no slack.
        CHECK(a.ground_vel.size() == a.samples && a.ground_slack.size() == a.samples);
        for (size_t i = 0; i < a.ground_vel.size(); ++i)
            CHECK(a.ground_vel[i].y == a.ground_velocity.y && a.ground_vel[i].z == a.ground_velocity.z &&
                  a.ground_slack[i] == 0.0);
        // Story 10-8t, carried limbs (Run Strafing Jump): the same climb carried sideways at
        // 3 m/s, the hips with it. The limbs still share a velocity, but the hips travel at half
        // its speed or more: no climb frame, and the ground is the world.
        {
            Options c;
            c.climb = 0.5;
            std::vector<BoneTrack> t = Rig(walk_l, walk_r, c);
            auto rung = [](double x) { return [x](double tm) { return Vec3d{x, 1.6 - 0.5 * tm, 0.3}; }; };
            AddHands(t, rung(-0.3), rung(0.3), c);
            BoneTrack& hips = t[static_cast<size_t>(Role::Hips)];
            for (size_t i = 0; i < hips.pos.size(); ++i) {
                hips.pos[i].y += 0.5 * static_cast<double>(i) / c.rate;
                hips.pos[i].z = hips.pos[0].z;
            }
            Drift(t, Vec3d{3.0 * c.dur, 0.0, 0.0}, c.dur);
            const PhysicsAnalysis s = AnalyseMotion(t);
            CHECK(s.ok && s.ground_velocity.x == 0.0 && s.ground_velocity.y == 0.0 && s.ground_velocity.z == 0.0);
            for (size_t i = 0; i < s.ground_vel.size(); ++i)
                CHECK(s.ground_vel[i].x == 0.0 && s.ground_vel[i].z == 0.0 && s.ground_slack[i] == 0.0);
        }
    }
    {
        // Story 10-8t: the ground frame follows the clip, per stance.
        auto world = [](const PhysicsAnalysis& a) {
            bool ok = a.ok && a.ground_vel.size() == a.samples && a.ground_slack.size() == a.samples &&
                      a.ground_velocity.x == 0.0 && a.ground_velocity.y == 0.0 && a.ground_velocity.z == 0.0;
            for (size_t i = 0; ok && i < a.ground_vel.size(); ++i)
                ok = a.ground_vel[i].x == 0.0 && a.ground_vel[i].y == 0.0 && a.ground_vel[i].z == 0.0 &&
                     a.ground_slack[i] == 0.0;
            return ok;
        };
        // A world-frame clip (the walk, root motion kept): ground 0 and slack 0 at every sample.
        CHECK(world(AnalyseMotion(Rig(walk_l, walk_r, Options{}))));
        // A treadmill that speeds up (Treadmill Running): a run in place on a belt at 1 m/s, then
        // 2 m/s (eased over 1.8 - 2.2 s); the hips end where they started. One clip-wide value
        // would be wrong on either half; the frame follows each stance, and the foot events are
        // the moving run's (the slack moves a lift-off a few ms).
        {
            const Options          o;
            const FootFn           run_l = RunFoot(-0.6, -0.1), run_r = RunFoot(-0.2, 0.1);
            std::vector<BoneTrack> t = Rig(run_l, run_r, o);
            auto belt = [](double tm) { return 1.0 + 1.0 * Ease((tm - 1.8) / 0.4); };
            for (BoneTrack& b : t) {
                double g = 0.0;  // the belt's travel at sample i
                for (size_t i = 0; i < b.pos.size(); ++i) {
                    if (i > 0) g += 0.5 * (belt((i - 1) / o.rate) + belt(i / o.rate)) / o.rate;
                    b.pos[i].z -= g;
                }
            }
            const PhysicsAnalysis a = AnalyseMotion(t);
            CHECK(a.ok && a.ground_vel.size() == a.samples);
            if (a.ok && a.ground_vel.size() == a.samples) {
                CHECK(std::fabs(a.ground_vel[static_cast<size_t>(1.1 * o.rate)].z + 1.0) < 0.02);
                CHECK(std::fabs(a.ground_vel[static_cast<size_t>(3.1 * o.rate)].z + 2.0) < 0.02);
                CHECK(std::fabs(a.ground_slack[static_cast<size_t>(3.1 * o.rate)] - 0.15 * 2.0 / a.leg_length) < 0.01);
            }
            const std::vector<PhysicsEvent> ev = FootEvents(a, SpeedTiming());
            const std::vector<PhysicsEvent> moving = FootEvents(AnalyseMotion(Rig(run_l, run_r, o)), SpeedTiming());
            CHECK(Of(moving, PhysicsKind::FootStep).size() >= 8);
            for (PhysicsKind kind : {PhysicsKind::Step, PhysicsKind::FootStep, PhysicsKind::LiftOff})
                for (char side : {'L', 'R'}) {
                    const std::vector<PhysicsEvent> x = Of(ev, kind, side), y = Of(moving, kind, side);
                    CHECK(x.size() == y.size());
                    for (size_t i = 0; i < x.size() && i < y.size(); ++i) CHECK(std::fabs(x[i].time_s - y[i].time_s) < 0.01);
                }
        }
        // Feet dragged by a root motion (Crawling on hands and knees): both feet planted, the
        // whole body carried forward at 0.6 m/s, the hips at 0.5 m/s. The feet hold a steady
        // velocity, but the hips travel at half of it or more: the ground is the world.
        {
            Options o;
            o.dur = 3.0;
            std::vector<BoneTrack> t = Rig(Planted(-0.1, 0.0), Planted(0.1, 0.2), o);
            Drift(t, Vec3d{0.0, 0.0, 0.6 * o.dur}, o.dur);
            BoneTrack& hips = t[static_cast<size_t>(Role::Hips)];
            for (size_t i = 0; i < hips.pos.size(); ++i) hips.pos[i].z -= 0.1 * static_cast<double>(i) / o.rate;
            CHECK(world(AnalyseMotion(t)));
            // No hips bone: no travel check, the stances are used as found (a 0.6 m/s ground).
            hips.pos.clear();
            const PhysicsAnalysis b = AnalyseMotion(t);
            CHECK(b.ok && std::fabs(b.ground_velocity.z - 0.6) < 0.01 && !b.hips_missing.empty());
        }
        // A keyframed in-place sprint follows its belt only roughly (Two Cycle Sprint): a run at
        // 4.5 m/s in place on a 4.5 m/s belt, the planted foot wobbling +-13 % of the belt's speed
        // (0.67 leg/s, past the contact and slide speeds). The slack (15 % of the ground's speed)
        // keeps the planted foot in contact and not sliding: the moving run's events, no Slide.
        {
            const Options          o;
            const FootFn           run_l = RunFoot(-0.6, -0.1, 3.6), run_r = RunFoot(-0.2, 0.1, 3.6);
            std::vector<BoneTrack> t = Rig(run_l, run_r, o);
            // The wobble: during each 0.3 s contact the planted foot's speed on the belt swings
            // by wob x sin (one period per contact, so each contact still averages the belt).
            const double belt = 4.5, wob = 0.13 * belt, w = 2.0 * kPi / 0.3;
            for (int s = 0; s < 2; ++s)
                for (int q = 0; q < kFootPartCount; ++q) {
                    BoneTrack& b = t[static_cast<size_t>(FootPartRole(s ? 'R' : 'L', q))];
                    for (size_t i = 0; i < b.pos.size(); ++i) {
                        const double rel = i / o.rate - (s ? -0.2 : -0.6);
                        const double in = rel - 0.8 * std::floor(rel / 0.8);
                        if (in < 0.3) b.pos[i].z += wob / w * (1.0 - std::cos(w * in));
                    }
                }
            for (BoneTrack& b : t)
                for (size_t i = 0; i < b.pos.size(); ++i) b.pos[i].z -= belt * i / o.rate;
            const PhysicsAnalysis a = AnalyseMotion(t);
            CHECK(a.ok && std::fabs(a.ground_velocity.z + belt) < 0.1);
            const std::vector<PhysicsEvent> ev = FootEvents(a, SpeedTiming());
            const std::vector<PhysicsEvent> moving = FootEvents(AnalyseMotion(Rig(run_l, run_r, o)), SpeedTiming());
            CHECK(Of(moving, PhysicsKind::FootStep).size() >= 8);
            CHECK(Of(ev, PhysicsKind::Slide).empty());
            // From 0.3 s (the clip's first lift-off is under the smoothing's edge); the wobble
            // moves a contact's ends by up to about 20 ms.
            auto after = [](std::vector<PhysicsEvent> v) {
                v.erase(std::remove_if(v.begin(), v.end(), [](const PhysicsEvent& e) { return e.time_s < 0.3; }), v.end());
                return v;
            };
            for (PhysicsKind kind : {PhysicsKind::Step, PhysicsKind::FootStep, PhysicsKind::LiftOff})
                for (char side : {'L', 'R'}) {
                    const std::vector<PhysicsEvent> x = after(Of(ev, kind, side)), y = after(Of(moving, kind, side));
                    CHECK(x.size() == y.size());
                    for (size_t i = 0; i < x.size() && i < y.size(); ++i) CHECK(std::fabs(x[i].time_s - y[i].time_s) < 0.03);
                }
        }
        // An isolated glide is not a treadmill (the median of 3 stances): feet stepping in place,
        // root motion kept (the hips do not travel), contacts of 0.7 s with a 0.1 s flight
        // between; in its second contact the left foot glides 0.4 s at 0.5 m/s. That stance reads
        // 0.5 m/s, its neighbours 0: the ground stays 0 there and the glide is a Slide.
        {
            auto stepping = [](double first_contact, double x, bool glide) {
                return [=](double tm) {
                    const double cycle = 1.6, contact = 0.7;
                    const double rel = tm - first_contact;
                    const int    k = static_cast<int>(std::floor(rel / cycle));
                    const double in = rel - k * cycle;
                    FootPose     f;
                    f.ball.x = x;
                    f.ball.z = glide ? 0.5 * std::clamp(tm - 1.95, 0.0, 0.4) : 0.0;
                    f.ball.y = in < contact ? 0.02 : 0.02 + 0.15 * std::sin(kPi * (in - contact) / (cycle - contact));
                    return f;
                };
            };
            Options o;
            o.dur = 4.0;
            const PhysicsAnalysis a = AnalyseMotion(Rig(stepping(-1.4, -0.1, true), stepping(-0.6, 0.1, false), o));
            CHECK(a.ok && a.ground_vel.size() == a.samples);
            for (size_t i = 0; i < a.ground_vel.size(); ++i) CHECK(a.ground_vel[i].z == 0.0 && a.ground_slack[i] == 0.0);
            const std::vector<PhysicsEvent> sl = Of(FootEvents(a, SpeedTiming()), PhysicsKind::Slide, 'L');
            CHECK(sl.size() == 1);
            if (sl.size() == 1) CHECK(std::fabs(sl[0].start_s - 1.95) < 0.05 && std::fabs(sl[0].end_s - 2.35) < 0.05);
        }
        // Mostly airborne (Running Jump_Long): both feet always moving, round a 0.3 m circle
        // every 0.6 s, out of phase; never 100 ms steady: no stance, the ground is the world.
        {
            auto circling = [](double x, double phase) {
                return [=](double tm) {
                    FootPose     f;
                    const double w = 2.0 * kPi / 0.6 * tm + phase;
                    f.ball = Vec3d{x, 0.35 + 0.3 * std::sin(w), 0.3 * std::cos(w)};
                    return f;
                };
            };
            CHECK(world(AnalyseMotion(Rig(circling(-0.1, 0.0), circling(0.1, kPi), Options{}))));
        }
    }
    {
        // Raised support: the left foot steps up onto a 0.4 m block at t = 0.9 (a swing of 0.4 s
        // with a 10 cm lift). Found, and the contact's support is the block.
        const FootFn up = [](double t) {
            FootPose     f;
            const double u = std::clamp((t - 0.5) / 0.4, 0.0, 1.0);
            f.ball = Vec3d{-0.1, 0.02 + 0.4 * Ease(u) + 0.10 * std::sin(kPi * u), 0.4 * Ease(u)};
            return f;
        };
        Options o;
        o.dur = 2.0;
        const PhysicsAnalysis a = AnalyseMotion(Rig(up, Planted(0.1, 0.2), o));
        CHECK(a.ok);
        const std::vector<PhysicsEvent> ev = FootEvents(a, SpeedTiming());
        for (int q = 0; q < kFootPartCount; ++q) CHECK(StepsNear(Of(ev, PhysicsKind::Step, 'L', q), {0.9}, 0.025));
        CHECK(Of(ev, PhysicsKind::Step, 'R').empty());
        const PartTrack& ball = a.foot[0].part[static_cast<int>(FootPart::Ball)];
        CHECK(ball.contacts.size() == 2);
        if (ball.contacts.size() == 2) {
            CHECK(std::fabs(ball.contacts[0].support_y - 0.02) < 0.003);
            CHECK(std::fabs(ball.contacts[1].support_y - 0.42) < 0.003);
        }
        CHECK(Of(FootEvents(a), PhysicsKind::Step, 'L').size() == 3);  // default timing too
    }
    {
        // Toe first: the left foot lands on the ball and tip at t = 1.0, pitched 60 deg, and the
        // heel comes down about the ball over 300 ms. Separate: toe and tip at 1.0, heel at 1.3.
        // Combined: one step, at the toe.
        const FootFn toe_first = [](double t) {
            FootPose     f;
            const double u = std::clamp((t - 0.6) / 0.4, 0.0, 1.0);
            f.ball = Vec3d{-0.1, 0.02 + 0.10 * std::sin(kPi * u), 0.5 * Ease(u)};
            f.pitch = t < 0.6 ? 0.0 : (t < 1.0 ? 60.0 * kDeg * Ease(u) : 60.0 * kDeg * std::max(0.0, 1.0 - (t - 1.0) / 0.3));
            return f;
        };
        Options o;
        o.dur = 2.0;
        const PhysicsAnalysis a = AnalyseMotion(Rig(toe_first, Planted(0.1, 0.2), o));
        CHECK(a.ok);
        const std::vector<PhysicsEvent> ev = FootEvents(a, SpeedTiming());
        CHECK(StepsNear(Of(ev, PhysicsKind::Step, 'L', 1), {1.0}, 0.025));
        CHECK(StepsNear(Of(ev, PhysicsKind::Step, 'L', 2), {1.0}, 0.025));
        CHECK(StepsNear(Of(ev, PhysicsKind::Step, 'L', 0), {1.3}, 0.025));
        const std::vector<PhysicsEvent> foot = Of(ev, PhysicsKind::FootStep, 'L');
        CHECK(foot.size() == 1);
        if (foot.size() == 1) CHECK(foot[0].part != static_cast<int>(FootPart::Heel) && std::fabs(foot[0].time_s - 1.0) < 0.025);
        CHECK(Of(ev, PhysicsKind::Slide).empty() && Of(ev, PhysicsKind::Pivot).empty());
        CHECK(CombinedIsEarliest(FootEvents(a), 0.4));
    }
    {
        // A slow heel: the toe lands at 1.0 s with the foot pitched 20 deg, then the heel is
        // lowered over 270 ms, slower than the contact speed (about 0.23 L/s), so its speed
        // contact starts with the toe's. The default timing still puts the heel step where the
        // lowering ends (1.27 s), not at the toe landing.
        const FootFn slow = [](double t) {
            FootPose     f;
            const double u = std::clamp((t - 0.6) / 0.4, 0.0, 1.0);
            f.ball = Vec3d{-0.1, 0.02 + 0.10 * std::sin(kPi * u), 0.5 * Ease(u)};
            f.pitch = t < 1.0 ? 20.0 * kDeg * Ease(u) : 20.0 * kDeg * std::max(0.0, 1.0 - (t - 1.0) / 0.27);
            return f;
        };
        Options o;
        o.dur = 2.0;
        const PhysicsAnalysis a = AnalyseMotion(Rig(slow, Planted(0.1, 0.2), o));
        CHECK(a.ok && !a.foot[0].part[0].contacts.empty());
        if (!a.foot[0].part[0].contacts.empty()) CHECK(a.foot[0].part[0].contacts.back().start / 240.0 < 1.05);
        const std::vector<PhysicsEvent> ev = FootEvents(a);
        CHECK(StepsNear(Of(ev, PhysicsKind::Step, 'L', 0), {1.27}, 0.02));
        CHECK(StepsNear(Of(ev, PhysicsKind::Step, 'L', 1), {1.0}, 0.04));
        const std::vector<PhysicsEvent> foot = Of(ev, PhysicsKind::FootStep, 'L');
        CHECK(foot.size() == 1 && CombinedIsEarliest(ev, 0.4));
        if (foot.size() == 1) CHECK(foot[0].part != static_cast<int>(FootPart::Heel));
    }
    {
        // Heel first: the heel lands at 1.0 s and the foot slaps flat about it in 20 ms. The heel's
        // contact starts first, but by the default timing the toe's step comes earlier: the
        // combined step is that earliest separate step, not the heel's.
        const FootFn heel_first = [](double t) {
            const double u = std::clamp((t - 0.6) / 0.4, 0.0, 1.0);
            const Vec3d  heel{-0.1, 0.08 + 0.10 * std::sin(kPi * u), 0.5 * Ease(u)};
            return FromHeel(heel, t < 1.0 ? -15.0 * kDeg * Ease(u) : -15.0 * kDeg * std::max(0.0, 1.0 - (t - 1.0) / 0.02));
        };
        Options o;
        o.dur = 2.0;
        const PhysicsAnalysis a = AnalyseMotion(Rig(heel_first, Planted(0.1, 0.2), o));
        CHECK(a.ok && !a.foot[0].part[0].contacts.empty() && !a.foot[0].part[1].contacts.empty());
        if (!a.foot[0].part[0].contacts.empty() && !a.foot[0].part[1].contacts.empty())
            CHECK(a.foot[0].part[0].contacts.back().start < a.foot[0].part[1].contacts.back().start);
        const std::vector<PhysicsEvent> ev = FootEvents(a);
        const std::vector<PhysicsEvent> foot = Of(ev, PhysicsKind::FootStep, 'L');
        CHECK(foot.size() == 1 && CombinedIsEarliest(ev, 0.4));
        if (foot.size() == 1) CHECK(foot[0].part == static_cast<int>(FootPart::Ball));
    }
    {
        // Slow landing (0.5 m/s), fast take-off (2 m/s): the lift-off's strength, its departure
        // speed, is above the step's approach speed.
        const FootFn touch = [](double t) {
            FootPose     f;
            const double before = std::max(0.0, 1.0 - t), after = std::max(0.0, t - 1.3);
            f.ball = Vec3d{-0.1, 0.02 + std::min(0.1, 0.5 * before) + std::min(0.1, 2.0 * after), 2.0 * after - 0.5 * before};
            return f;
        };
        Options o;
        o.dur = 2.0;
        const std::vector<PhysicsEvent> ev = FootEvents(AnalyseMotion(Rig(touch, Planted(0.1, 0.2), o)));
        const std::vector<PhysicsEvent> st = Of(ev, PhysicsKind::FootStep, 'L'), up = Of(ev, PhysicsKind::LiftOff, 'L');
        CHECK(st.size() == 1 && up.size() == 1);
        if (st.size() == 1 && up.size() == 1) {
            CHECK(up[0].strength > 2.0 * st[0].strength);
            CHECK(std::fabs(up[0].time_s - 1.3) < 0.025 && std::fabs(st[0].time_s - 1.0) < 0.04);
        }
    }
    {
        // Slide: the planted left foot slides 0.5 m/s forward for 200 ms (from 1.0 s): one
        // slide scuff with its start and end, and no step.
        const FootFn slide = [](double t) {
            FootPose f;
            f.ball = Vec3d{-0.1, 0.02, 0.5 * std::clamp(t - 1.0, 0.0, 0.2)};
            return f;
        };
        Options o;
        o.dur = 3.0;
        const PhysicsAnalysis a = AnalyseMotion(Rig(slide, Planted(0.1, 0.2), o));
        CHECK(a.ok);
        const std::vector<PhysicsEvent> ev = FootEvents(a, SpeedTiming());
        const std::vector<PhysicsEvent> sl = Of(ev, PhysicsKind::Slide);
        CHECK(sl.size() == 1);
        if (sl.size() == 1) {
            CHECK(sl[0].side == 'L' && std::fabs(sl[0].start_s - 1.0) < 0.03 && std::fabs(sl[0].end_s - 1.2) < 0.03);
            CHECK(sl[0].time_s == sl[0].start_s);
            CHECK(std::fabs(sl[0].strength - 0.5 / 0.87) < 0.05);  // leg/s
        }
        CHECK(Of(ev, PhysicsKind::Step).empty() && Of(ev, PhysicsKind::FootStep).empty());
        CHECK(Of(ev, PhysicsKind::LiftOff).empty() && Of(ev, PhysicsKind::Pivot).empty());
        // Story 10-8m: the whole foot slides, heel and forefoot at once: one heel and one forefoot
        // part slide; the combined slide's longest part is a tie, so the forefoot.
        const int heel = static_cast<int>(FootPart::Heel);
        CHECK(a.foot[0].part_slides.size() == 2);
        if (a.foot[0].part_slides.size() == 2) {
            const int nh = (a.foot[0].part_slides[0].part == heel) + (a.foot[0].part_slides[1].part == heel);
            CHECK(nh == 1);
        }
        if (sl.size() == 1) CHECK(sl[0].longest >= 0 && sl[0].longest != heel);
        const std::vector<PhysicsEvent> parts = Of(FootPartScuffEvents(a), PhysicsKind::Slide, 'L');
        CHECK(parts.size() == 2 && Of(parts, PhysicsKind::Slide, 'L', heel).size() == 1);
    }
    {
        // Pivot: the planted left foot turns 90 deg on the ball in 250 ms (from 1.0 s): one
        // pivot scuff, on the ball, about 90 deg; the heel sweeping round is not a slide or a step.
        Options o;
        o.dur = 3.0;
        const PhysicsAnalysis a =
            AnalyseMotion(Rig(Planted(-0.1, 0.0, [](double t) { return 90.0 * kDeg * Ease((t - 1.0) / 0.25); }),
                              Planted(0.1, 0.2), o));
        CHECK(a.ok);
        const std::vector<PhysicsEvent> ev = FootEvents(a, SpeedTiming());
        const std::vector<PhysicsEvent> pv = Of(ev, PhysicsKind::Pivot);
        CHECK(pv.size() == 1);
        if (pv.size() == 1) {
            CHECK(pv[0].side == 'L' && pv[0].part == static_cast<int>(FootPart::Ball));
            CHECK(pv[0].strength > 80.0 && pv[0].strength < 95.0);
            CHECK(pv[0].start_s > 0.95 && pv[0].start_s < 1.05 && pv[0].end_s > 1.2 && pv[0].end_s < 1.3);
            CHECK(PhysicsMarkerName(pv[0]).compare(0, 17, "PHY L pivot ball ") == 0);
        }
        CHECK(ev.size() == 1);
        // The same turn on the heel (the ball sweeping round) says heel.
        const FootFn on_heel = [](double t) {
            FootPose     f;
            const double yaw = 90.0 * kDeg * Ease((t - 1.0) / 0.25);
            const Vec3d  heel{-0.1, 0.08, -0.14};
            f.yaw = yaw;
            f.ball = Add(heel, Yawed(yaw, 0.0, -0.06, 0.14));
            return f;
        };
        const std::vector<PhysicsEvent> hv = Of(FootEvents(AnalyseMotion(Rig(on_heel, Planted(0.1, 0.2), o))), PhysicsKind::Pivot);
        CHECK(hv.size() == 1 && hv[0].part == static_cast<int>(FootPart::Heel));
        // Story 10-8m: a pivot on one part holds longest on that part; the sweeping part's slide
        // during the pivot is the pivot's, never a part slide.
        if (pv.size() == 1) CHECK(pv[0].longest == static_cast<int>(FootPart::Ball));
        if (hv.size() == 1) CHECK(hv[0].longest == static_cast<int>(FootPart::Heel));
        CHECK(a.foot[0].part_slides.empty() && Of(FootPartScuffEvents(a), PhysicsKind::Slide).empty());
        // A foot that turns 10 deg is no pivot (under 15 deg swept).
        const std::vector<PhysicsEvent> small = FootEvents(
            AnalyseMotion(Rig(Planted(-0.1, 0.0, [](double t) { return 10.0 * kDeg * Ease((t - 1.0) / 0.05); }),
                              Planted(0.1, 0.2), o)));
        CHECK(Of(small, PhysicsKind::Pivot).empty());
    }
    {
        // A steep foot (story 10-8d). Tiptoe: the left heel rises (pitched 60 deg about the
        // ball from 0.5 to 0.75 s) and holds, the ball planted; the foot turns 90 deg on the
        // ball in 250 ms (from 1.0 s) and stays 750 ms. The heel -> toe yaw does not hold from
        // above; the heading comes from the ball -> toe end vector (the toes stay flat), else
        // from the toe bone's rotation: one pivot on the ball, about 90 deg, either way.
        Options o;
        o.dur = 2.0;
        const FootFn tiptoe = [](double t) {
            FootPose f;
            f.ball = Vec3d{-0.1, 0.02, 0.0};
            f.pitch = 60.0 * kDeg * Ease((t - 0.5) / 0.25);
            f.yaw = 90.0 * kDeg * Ease((t - 1.0) / 0.25);
            return f;
        };
        auto one_pivot = [](const PhysicsAnalysis& a, int part) {
            const std::vector<PhysicsEvent> pv = Of(FootEvents(a, SpeedTiming()), PhysicsKind::Pivot);
            return pv.size() == 1 && pv[0].side == 'L' && pv[0].part == part && pv[0].strength > 80.0 &&
                   pv[0].strength < 95.0 && pv[0].start_s > 0.95 && pv[0].start_s < 1.05 && pv[0].end_s > 1.2 &&
                   pv[0].end_s < 1.3;
        };
        const int             ball = static_cast<int>(FootPart::Ball), heel = static_cast<int>(FootPart::Heel);
        const PhysicsAnalysis te = AnalyseMotion(Rig(tiptoe, Planted(0.1, 0.2), o));
        CHECK(te.ok && te.foot[0].heading_source == HeadingSource::ToeEnd);
        CHECK(one_pivot(te, ball));
        Options ro = o;
        ro.tip = false;
        ro.rot = true;
        const PhysicsAnalysis tr = AnalyseMotion(Rig(tiptoe, Planted(0.1, 0.2), ro));
        CHECK(tr.ok && !tr.foot[0].part[2].present && tr.foot[0].heading_source == HeadingSource::Rotation);
        CHECK(one_pivot(tr, ball));
        // With a toe end the toe end is the source, rotations or not.
        Options both = o;
        both.rot = true;
        CHECK(AnalyseMotion(Rig(tiptoe, Planted(0.1, 0.2), both)).foot[0].heading_source == HeadingSource::ToeEnd);

        // Neither source (no toe end, no rotations): as before 10-8d, the turn is lost.
        Options no = o;
        no.tip = false;
        const PhysicsAnalysis nn = AnalyseMotion(Rig(tiptoe, Planted(0.1, 0.2), no));
        CHECK(nn.ok && nn.foot[0].heading_source == HeadingSource::None);
        CHECK(Of(FootEvents(nn), PhysicsKind::Pivot).empty());

        // Heel, toes up: the left foot turns its toes up about the heel (heel -> ball 70 deg
        // above level, from 0.5 to 0.75 s), turns 90 deg on the heel in 250 ms (from 1.0 s) and
        // stays. The toe bone's across axis stays level: one pivot on the heel, about 90 deg.
        const FootFn toes_up = [](double t) {
            FootPose     f;
            const Vec3d  heel_at{-0.1, 0.08, -0.14};
            const double flat = std::atan2(0.06, 0.14);  // heel -> ball, flat: below level
            const double up = (70.0 * kDeg + flat) * Ease((t - 0.5) / 0.25);
            f.yaw = 90.0 * kDeg * Ease((t - 1.0) / 0.25);
            f.pitch = -up;
            f.toe_pitch = up;
            f.ball = Add(heel_at, Yawed(f.yaw, 0.0, -0.06 * std::cos(up) + 0.14 * std::sin(up),
                                        0.06 * std::sin(up) + 0.14 * std::cos(up)));
            return f;
        };
        const PhysicsAnalysis hr = AnalyseMotion(Rig(toes_up, Planted(0.1, 0.2), ro));
        CHECK(hr.ok && hr.foot[0].heading_source == HeadingSource::Rotation);
        CHECK(one_pivot(hr, heel));
        // Without rotations the heel turn is lost, as before.
        CHECK(Of(FootEvents(AnalyseMotion(Rig(toes_up, Planted(0.1, 0.2), no))), PhysicsKind::Pivot).empty());

        // A flat pivot is unchanged by a fallback source: the same events with rotations.
        const FootFn flat_turn = Planted(-0.1, 0.0, [](double t) { return 90.0 * kDeg * Ease((t - 1.0) / 0.25); });
        const std::vector<PhysicsEvent> fa = FootEvents(AnalyseMotion(Rig(flat_turn, Planted(0.1, 0.2), no)));
        const std::vector<PhysicsEvent> fr = FootEvents(AnalyseMotion(Rig(flat_turn, Planted(0.1, 0.2), ro)));
        CHECK(fa.size() == fr.size() && Of(fr, PhysicsKind::Pivot).size() == 1);
        for (size_t i = 0; i < std::min(fa.size(), fr.size()); ++i)
            CHECK(fa[i].kind == fr[i].kind && fa[i].part == fr[i].part && fa[i].start_s == fr[i].start_s &&
                  fa[i].end_s == fr[i].end_s && fa[i].strength == fr[i].strength);
    }
    {
        // Story 10-8k: a step after a pivot. The left foot stands on its heel, toes up (heel ->
        // ball 45 deg above level), turns 90 deg on the heel in 250 ms (from 1.0 s), then slaps
        // its toes down in 50 ms from `slap`: a foot step on the toe (the earliest landing, not
        // the tip too) when the toes land within pivot_planted_s of the pivot's end; none later.
        Options o;
        o.dur = 2.0;
        o.rot = true;
        auto slap_foot = [](double slap) {
            return [slap](double t) {
                FootPose     f;
                const Vec3d  heel_at{-0.1, 0.08, -0.14};
                const double flat = std::atan2(0.06, 0.14);
                const double up = (45.0 * kDeg + flat) * (Ease((t - 0.2) / 0.25) - std::clamp((t - slap) / 0.05, 0.0, 1.0));
                f.yaw = 90.0 * kDeg * Ease((t - 1.0) / 0.25);
                f.pitch = -up;
                f.ball = Add(heel_at, Yawed(f.yaw, 0.0, -0.06 * std::cos(up) + 0.14 * std::sin(up),
                                            0.06 * std::sin(up) + 0.14 * std::cos(up)));
                return f;
            };
        };
        const int             heel = static_cast<int>(FootPart::Heel);
        const PhysicsAnalysis a = AnalyseMotion(Rig(slap_foot(1.26), Planted(0.1, 0.2), o));
        CHECK(a.ok);
        const std::vector<PhysicsEvent> ev = FootEvents(a, SpeedTiming());
        const std::vector<PhysicsEvent> pv = Of(ev, PhysicsKind::Pivot, 'L');
        CHECK(pv.size() == 1 && pv[0].part == heel);
        const std::vector<PhysicsEvent> fs = Of(ev, PhysicsKind::FootStep, 'L');
        CHECK(fs.size() == 1);
        if (fs.size() == 1 && pv.size() == 1) {
            CHECK(fs[0].part != heel && std::abs(fs[0].time_s - pv[0].end_s) <= 0.1);
            CHECK(PhysicsMarkerName(fs[0]).compare(0, 10, "PHY L step") == 0);
        }
        // Its separate steps are the per-part landings, unchanged by the rule.
        const std::vector<PhysicsEvent> st = Of(ev, PhysicsKind::Step, 'L');
        CHECK(!st.empty() && Of(st, PhysicsKind::Step, 'L', heel).empty());
        if (fs.size() == 1) {
            bool same = false;
            for (const PhysicsEvent& e : st) same = same || (e.part == fs[0].part && e.time_s == fs[0].time_s);
            CHECK(same);
        }
        // Toes down 0.15 s after the pivot ends: no foot step (the separate steps stay).
        const std::vector<PhysicsEvent> late = FootEvents(AnalyseMotion(Rig(slap_foot(1.42), Planted(0.1, 0.2), o)), SpeedTiming());
        CHECK(Of(late, PhysicsKind::Pivot, 'L').size() == 1 && Of(late, PhysicsKind::FootStep, 'L').empty());
        CHECK(!Of(late, PhysicsKind::Step, 'L').empty());
        // A part's offset moves the step but never creates or removes it: the window reads the
        // raw landing (+0.2 s keeps the 1.26 slap's step, 0.2 s later; -0.2 s adds none at 1.42).
        PhysicsParams shifted = SpeedTiming();
        for (int q = 0; q < kFootPartCount; ++q) shifted.step_offset_s[q] = 0.2;
        const std::vector<PhysicsEvent> moved = Of(FootEvents(a, shifted), PhysicsKind::FootStep, 'L');
        CHECK(moved.size() == 1);
        if (moved.size() == 1 && fs.size() == 1) CHECK(std::abs(moved[0].time_s - fs[0].time_s - 0.2) < 1e-9);
        for (int q = 0; q < kFootPartCount; ++q) shifted.step_offset_s[q] = -0.2;
        CHECK(Of(FootEvents(AnalyseMotion(Rig(slap_foot(1.42), Planted(0.1, 0.2), o)), shifted), PhysicsKind::FootStep, 'L').empty());
        // A landing that starts the foot's contact (the walk) has one foot step, as before.
        const std::vector<PhysicsEvent> walk =
            FootEvents(AnalyseMotion(Rig(WalkFoot(0.4, -0.1), WalkFoot(0.0, 0.1), Options{})), SpeedTiming());
        CHECK(Of(walk, PhysicsKind::FootStep, 'L').size() == 4);
    }
    {
        // A heel whip at toe-off (story 10-8d, narrowed by 10-8l): the foot rolls onto its toes
        // (pitched 80 deg by 1.25 s) and turns on the ball as it rolls, then the ball leaves
        // 80 ms after the turn (from 1.28 s, up and forward at 1 m/s). A small whip (25 deg,
        // under 2 x pivot_min_deg) is no pivot; a 40 deg spin turn is one Pivot on the ball.
        Options o;
        o.dur = 2.0;
        auto roll = [](double leave, double deg) {
            return [leave, deg](double t) {
                FootPose     f;
                const double up = std::max(0.0, t - leave);
                f.ball = Vec3d{-0.1, 0.02 + up, up};
                f.pitch = 80.0 * kDeg * std::clamp((t - 1.0) / 0.25, 0.0, 1.0);
                f.yaw = deg * kDeg * Ease((t - 1.0) / 0.2);
                return f;
            };
        };
        PhysicsParams sens;
        sens.pivot_min_deg = 7.5;  // pivot sensitivity x2
        for (bool rot : {false, true}) {
            Options v = o;
            v.tip = !rot;
            v.rot = rot;
            const PhysicsAnalysis a = AnalyseMotion(Rig(roll(1.28, 25.0), Planted(0.1, 0.2), v));
            CHECK(a.ok && Of(FootEvents(a), PhysicsKind::Pivot).empty());
            // Spin turn: 40 deg with the same roll-off is a pivot on the ball, about 40 deg.
            const std::vector<PhysicsEvent> sp = Of(FootEvents(AnalyseMotion(Rig(roll(1.28, 40.0), Planted(0.1, 0.2), v))), PhysicsKind::Pivot);
            CHECK(sp.size() == 1);
            if (sp.size() == 1) CHECK(sp[0].part == static_cast<int>(FootPart::Ball) && sp[0].strength > 33.0 && sp[0].strength < 45.0);
            // Sensitive: the 25 deg whip clears 2 x 7.5 deg.
            CHECK(Of(FootEvents(AnalyseMotion(Rig(roll(1.28, 25.0), Planted(0.1, 0.2), v), sens)), PhysicsKind::Pivot).size() == 1);
            // Swivel: the same roll with the ball staying (on its toes to the clip end) is a
            // pivot on the ball, about 40 deg.
            const std::vector<PhysicsEvent> sw = Of(FootEvents(AnalyseMotion(Rig(roll(10.0, 40.0), Planted(0.1, 0.2), v))), PhysicsKind::Pivot);
            CHECK(sw.size() == 1);
            if (sw.size() == 1) CHECK(sw[0].part == static_cast<int>(FootPart::Ball) && sw[0].strength > 33.0 && sw[0].strength < 45.0);
        }
        // Late leave: a flat 40 deg turn whose foot leaves 200 ms after is a pivot (planted rule).
        const FootFn late = [](double t) {
            FootPose     f;
            const double up = std::max(0.0, t - 1.4);
            f.ball = Vec3d{-0.1, 0.02 + up, up};
            f.yaw = 40.0 * kDeg * Ease((t - 1.0) / 0.2);
            return f;
        };
        CHECK(Of(FootEvents(AnalyseMotion(Rig(late, Planted(0.1, 0.2), o))), PhysicsKind::Pivot).size() == 1);
        // Late roll-off: the same flat turn, then the foot pitches up (80 deg from 1.3 to 1.45 s,
        // no toe end nor toe rotation: no fallback heading) and the ball leaves at 1.5 s. Out of
        // the spin window, so the planted rule holds: one pivot.
        const FootFn late_roll = [](double t) {
            FootPose     f;
            const double up = std::max(0.0, t - 1.5);
            f.ball = Vec3d{-0.1, 0.02 + up, up};
            f.pitch = 80.0 * kDeg * std::clamp((t - 1.3) / 0.15, 0.0, 1.0);
            f.yaw = 40.0 * kDeg * Ease((t - 1.0) / 0.2);
            return f;
        };
        Options nt = o;
        nt.tip = false;
        CHECK(Of(FootEvents(AnalyseMotion(Rig(late_roll, Planted(0.1, 0.2), nt))), PhysicsKind::Pivot).size() == 1);
        // A flat turn whose foot leaves as it ends: 40 deg is a spin turn, 25 deg is not.
        auto leave = [](double deg) {
            return [deg](double t) {
                FootPose     f;
                const double up = std::max(0.0, t - 1.2);
                f.ball = Vec3d{-0.1, 0.02 + up, up};
                f.yaw = deg * kDeg * Ease((t - 1.0) / 0.2);
                return f;
            };
        };
        CHECK(Of(FootEvents(AnalyseMotion(Rig(leave(40.0), Planted(0.1, 0.2), o))), PhysicsKind::Pivot).size() == 1);
        CHECK(Of(FootEvents(AnalyseMotion(Rig(leave(25.0), Planted(0.1, 0.2), o))), PhysicsKind::Pivot).empty());
        // The same turn with the foot flat is a pivot.
        const FootFn flat = [](double t) {
            FootPose f;
            f.ball = Vec3d{-0.1, 0.02, 0.0};
            f.yaw = 40.0 * kDeg * Ease((t - 1.0) / 0.2);
            return f;
        };
        CHECK(Of(FootEvents(AnalyseMotion(Rig(flat, Planted(0.1, 0.2), o))), PhysicsKind::Pivot).size() == 1);
    }
    {
        // Another sample rate: the switch cost is a time, not a sample count. At 120 Hz the walk
        // gives the same steps, and a 160 ms tap of the left foot is still a contact (a contact
        // between two free stretches pays two switches: about 85 ms of clear rest at least).
        // The tap: the foot comes in at 1 m/s, down 10 cm over the last 100 ms, touches from
        // 1.0 to 1.16 s, and leaves the same way.
        const FootFn tap = [](double t) {
            FootPose     f;
            const double before = std::max(0.0, 1.0 - t), after = std::max(0.0, t - 1.16);
            f.ball = Vec3d{-0.1, 0.02 + std::min(0.1, before) + std::min(0.1, after), after - before};
            return f;
        };
        for (double rate : {240.0, 120.0}) {
            Options o;
            o.rate = rate;
            o.dur = 2.0;
            const std::vector<PhysicsEvent> ev = FootEvents(AnalyseMotion(Rig(tap, Planted(0.1, 0.2), o)), SpeedTiming());
            CHECK(StepsNear(Of(ev, PhysicsKind::Step, 'L', 0), {1.0}, 0.02));
            Options w;
            w.rate = rate;
            const std::vector<PhysicsEvent> wev = FootEvents(AnalyseMotion(Rig(walk_l, walk_r, w)), SpeedTiming());
            CHECK(StepsNear(Of(wev, PhysicsKind::Step, 'L', 1), kLeft, 0.025));
            CHECK(StepsNear(Of(wev, PhysicsKind::Step, 'R', 1), kRight, 0.025));
        }
    }
    {
        // Missing role: no toe and no toe end bone. Heel events only, and the report names the
        // missing parts (and the pivot that needs the toe).
        Options o;
        o.toes = false;
        const PhysicsAnalysis a = AnalyseMotion(Rig(walk_l, walk_r, o));
        CHECK(a.ok && !a.foot[0].part[1].present && !a.foot[0].part[2].present && !a.foot[0].has_yaw);
        const std::vector<PhysicsEvent> ev = FootEvents(a, SpeedTiming());
        CHECK(StepsNear(Of(ev, PhysicsKind::Step, 'L', 0), kLeft, 0.025));
        CHECK(StepsNear(Of(ev, PhysicsKind::Step, 'R', 0), kRight, 0.025));
        for (const PhysicsEvent& e : ev) CHECK(e.part == static_cast<int>(FootPart::Heel));
        const std::string sum = PhysicsSummary(a, ev);
        CHECK(sum.find("missing: L toe: no bone for left toe") != std::string::npos);
        CHECK(sum.find("L tip: no bone for left toe end") != std::string::npos);
        CHECK(sum.find("R pivot: needs the heel and the toe") != std::string::npos);
        CHECK(sum.find("steps heel 8, toe 0, tip 0") != std::string::npos);

        // A toe end that plays on the toe's bone (Unreal's ball_l) is dropped as a duplicate.
        Options d;
        d.same_tip = true;
        const PhysicsAnalysis b = AnalyseMotion(Rig(walk_l, walk_r, d));
        CHECK(b.ok && b.foot[0].part[1].present && !b.foot[0].part[2].present);
        CHECK(PhysicsSummary(b, FootEvents(b)).find("L tip: same bone as the toe") != std::string::npos);

        // No knee: no body scale, nothing runs, and it says why.
        std::vector<BoneTrack> t = Rig(walk_l, walk_r, Options{});
        t[static_cast<size_t>(Role::LeftKnee)].pos.clear();
        t[static_cast<size_t>(Role::RightKnee)].pos.clear();
        const PhysicsAnalysis c = AnalyseMotion(t);
        CHECK(!c.ok && c.error.find("no body scale") == 0 && FootEvents(c).empty());
        CHECK(PhysicsSummary(c, {}) == "skipped: " + c.error);
        // One knee: the other leg's length.
        t = Rig(walk_l, walk_r, Options{});
        t[static_cast<size_t>(Role::LeftKnee)].pos.clear();
        const PhysicsAnalysis e1 = AnalyseMotion(t);
        CHECK(e1.ok && e1.scale_note == "one leg" && std::fabs(e1.leg_length - 0.87) < 1e-6);
        CHECK(!AnalyseMotion({}).ok && AnalyseMotion({}).error == "the bones could not be sampled");
    }
    {
        // Role tracks from a bone mapping and a sampler: only the physics roles are sampled.
        std::vector<int> bone_of_role(static_cast<size_t>(Role::Count), -1);
        bone_of_role[static_cast<size_t>(Role::LeftHeel)] = 7;
        bone_of_role[static_cast<size_t>(Role::Head)] = 3;
        std::vector<int>             asked;
        const std::vector<BoneTrack> t = PhysicsRoleTracks(bone_of_role, [&](const std::vector<int>& b) {
            asked = b;
            BoneTrack one;
            one.pos.push_back(Vec3d{1.0, 2.0, 3.0});
            return std::vector<BoneTrack>(b.size(), one);
        });
        CHECK(asked == std::vector<int>{7});
        CHECK(t.size() == static_cast<size_t>(Role::Count) && t[static_cast<size_t>(Role::LeftHeel)].pos.size() == 1);
        CHECK(t[static_cast<size_t>(Role::Head)].pos.empty());
        CHECK(PhysicsRoleTracks(bone_of_role, [](const std::vector<int>&) { return std::vector<BoneTrack>(); }).empty());
    }
    {
        // Marker names and the PHY prefix.
        PhysicsEvent e;
        e.side = 'L';
        e.part = 0;
        e.strength = 1.84;
        CHECK(PhysicsMarkerName(e) == "PHY L heel 1.8");
        e.kind = PhysicsKind::FootStep;
        e.side = 'R';
        e.part = 1;
        CHECK(PhysicsMarkerName(e) == "PHY R step toe 1.8");
        e.kind = PhysicsKind::LiftOff;
        e.part = 2;
        CHECK(PhysicsMarkerName(e) == "PHY R lift tip 1.8");
        e.kind = PhysicsKind::Slide;
        e.time_s = e.start_s = 1.0;
        e.end_s = 1.21;
        CHECK(PhysicsMarkerName(e) == "PHY R slide tip 1.8 210ms");
        e.kind = PhysicsKind::Pivot;
        e.part = -1;
        e.strength = 87.4;
        CHECK(PhysicsMarkerName(e) == "PHY R pivot neither 87deg 210ms");
        e.part = static_cast<int>(FootPart::Tip);  // a rig without a ball bone turns on the tip
        CHECK(PhysicsMarkerName(e) == "PHY R pivot tip 87deg 210ms");
        e.part = static_cast<int>(FootPart::Ball);
        CHECK(PhysicsMarkerName(e) == "PHY R pivot ball 87deg 210ms");
        e.part = -1;
        CHECK(IsPhysicsMarkerName("PHY L heel 1.8") && !IsPhysicsMarkerName("PHYSICS") && !IsPhysicsMarkerName("RAV?") &&
              !IsPhysicsMarkerName(nullptr));
        CHECK(PhysicsEventLines({e}, "  ") == "     1.000 s  PHY R pivot neither 87deg 210ms  (to 1.210 s)\n");
    }

    // ---- Hands (story 10-8c) -------------------------------------------------------------------
    static constexpr double kLeg = 0.87;  // the rig's leg (m): hand speeds below in leg/s x kLeg (static: MSVC C3493 in the lambdas)
    {
        // Grab: the right hand moves at 1.5 leg/s, lands at 1.0 s and rests 500 ms: one Grab at
        // 1.0, one Release at the end of its rest (1.5). The left hand hangs still: nothing.
        Options o;
        o.dur = 2.5;
        std::vector<BoneTrack> t = Rig(walk_l, walk_r, o);
        AddHands(t, HandStill(-0.3), HandMove(0.3, 1.5 * kLeg, {{1.0, 1.5}}), o);
        const PhysicsAnalysis a = AnalyseMotion(t);
        CHECK(a.ok && a.hand[1].part.present && a.hand_missing.empty());
        const std::vector<PhysicsEvent> ev = HandEvents(a);
        CHECK(StepsNear(Of(ev, PhysicsKind::Grab, 'R'), {1.0}, 0.02));
        CHECK(StepsNear(Of(ev, PhysicsKind::Release, 'R'), {1.5}, 0.02));
        CHECK(ev.size() == 2);
        for (const PhysicsEvent& e : ev) CHECK(e.part == -1 && e.strength > 1.3 && e.strength < 1.7);  // leg/s
        CHECK(HandSummary(a, ev) == "hands: grabs 1, releases 1, pivots 0");
        // Foot events never include hand events, and the reverse.
        for (const PhysicsEvent& e : FootEvents(a)) CHECK(e.kind != PhysicsKind::Grab && e.kind != PhysicsKind::Release);
        // The timing and the offset: Speed by default, the offset added.
        PhysicsParams later;
        later.hand_offset_s = 0.02;
        const std::vector<PhysicsEvent> ev2 = HandEvents(a, later);
        CHECK(ev2.size() == 2 && ev.size() == 2);
        if (ev2.size() == 2 && ev.size() == 2) {
            CHECK(std::fabs(ev2[0].time_s - (ev[0].time_s + 0.02)) < 1e-9);
            CHECK(ev2[1].time_s == ev[1].time_s);  // a release has no offset of its own here
        }
        CHECK(PhysicsParams{}.hand_timing == StepTiming::Speed);
        // A more sensitive contact speed (x2) still finds the same grab.
        PhysicsParams sens;
        sens.hand_contact_speed *= 2.0;
        CHECK(StepsNear(Of(HandEvents(AnalyseMotion(t, sens), sens), PhysicsKind::Grab, 'R'), {1.0}, 0.03));
    }
    {
        // Story 10-8q: the reach's own threshold. The right hand reaches at 2 leg/s, brakes evenly
        // to rest at 1.0 s (0.8-1.0 s), rests, then speeds up evenly from 1.5 s to 2 leg/s at
        // 1.7 s. 10 % of the peak (0.2 leg/s) is crossed at 0.98 s and 1.52 s; the absolute contact
        // speed (0.4 leg/s, hand_reach_frac = 0) at 0.96 s and 1.54 s.
        Options o;
        o.dur = 2.5;
        std::vector<BoneTrack> t = Rig(walk_l, walk_r, o);
        const HandFn brake = [](double time) {
            const double v = 2.0 * kLeg;  // m/s
            double       d = v * std::min(time, 0.8);
            if (time > 0.8) {
                const double u = std::min(time, 1.0) - 0.8;
                d += v * (u - u * u / 0.4);
            }
            if (time > 1.5) {
                const double u = std::min(time, 1.7) - 1.5;
                d += v * u * u / 0.4;
            }
            if (time > 1.7) d += v * (time - 1.7);
            return Vec3d{0.3, 1.4 - 0.6 * d, 0.8 * d};
        };
        AddHands(t, HandStill(-0.3), brake, o);
        const std::vector<PhysicsEvent> ev = HandEvents(AnalyseMotion(t));
        CHECK(StepsNear(Of(ev, PhysicsKind::Grab, 'R'), {0.98}, 0.008));
        CHECK(StepsNear(Of(ev, PhysicsKind::Release, 'R'), {1.52}, 0.008));
        PhysicsParams abs_rule;
        abs_rule.hand_reach_frac = 0.0;
        const std::vector<PhysicsEvent> old = HandEvents(AnalyseMotion(t, abs_rule), abs_rule);
        CHECK(StepsNear(Of(old, PhysicsKind::Grab, 'R'), {0.96}, 0.008));
        CHECK(StepsNear(Of(old, PhysicsKind::Release, 'R'), {1.54}, 0.008));
        // The fraction scales with the reach: the same move at 60 % of the speed is timed the same.
        std::vector<BoneTrack> slow = Rig(walk_l, walk_r, o);
        AddHands(slow, HandStill(-0.3), [&](double time) {
            const Vec3d p = brake(time);
            return Vec3d{p.x, 1.4 - 0.6 * (1.4 - p.y), 0.6 * p.z};
        }, o);
        const std::vector<PhysicsEvent> ev_slow = HandEvents(AnalyseMotion(slow));
        CHECK(StepsNear(Of(ev_slow, PhysicsKind::Grab, 'R'), {0.98}, 0.008));
        CHECK(StepsNear(Of(ev_slow, PhysicsKind::Release, 'R'), {1.52}, 0.008));
    }
    {
        // Story 10-8q review: the relative crossing is searched within search_fwd_s of the
        // contact's start on both sides. A fast reach (20 leg/s to 0.5 s) glides at 1.5 leg/s
        // (under 10 % of its peak, over the contact speed) to 0.8 s, then brakes evenly to rest
        // at 1.0 s: the glide's start (0.55 s) is no grab; the absolute crossing (0.4 leg/s at
        // 0.947 s) stays.
        Options o;
        o.dur = 2.0;
        std::vector<BoneTrack> t = Rig(walk_l, walk_r, o);
        const HandFn glide = [](double time) {
            auto v = [](double s) {  // leg/s
                if (s < 0.5) return 20.0;
                if (s < 0.55) return 20.0 + (1.5 - 20.0) * (s - 0.5) / 0.05;
                if (s < 0.8) return 1.5;
                if (s < 1.0) return 1.5 * (1.0 - (s - 0.8) / 0.2);
                return 0.0;
            };
            double d = 0.0;
            const int steps = 2000;
            for (int i = 0; i < steps; ++i) d += v((i + 0.5) * time / steps) * time / steps;
            d *= kLeg;
            return Vec3d{0.3 + 0.6 * d, 1.0, 0.8 * d};  // level
        };
        AddHands(t, HandStill(-0.3), glide, o);
        CHECK(StepsNear(Of(HandEvents(AnalyseMotion(t)), PhysicsKind::Grab, 'R'), {0.947}, 0.01));
    }
    {
        // Still in the air: the right hand approaches at 0.7 leg/s (under the min approach), then
        // drifts at 0.2 leg/s (under the contact speed) to the end: no Grab, no Release.
        Options o;
        o.dur = 2.5;
        std::vector<BoneTrack> t = Rig(walk_l, walk_r, o);
        const HandFn drift = [](double time) {
            const double d = 0.7 * kLeg * std::min(time, 1.0) + 0.2 * kLeg * std::max(0.0, time - 1.0);
            return Vec3d{0.3, 1.4 - 0.6 * d, 0.8 * d};
        };
        AddHands(t, HandStill(-0.3), drift, o);
        const PhysicsAnalysis a = AnalyseMotion(t);
        CHECK(a.ok && !a.hand[1].part.contacts.empty());  // at rest, but no grab
        CHECK(HandEvents(a).empty());
    }
    {
        // Hand at rest at clip start: still until 1.0 s, then it leaves at 1.5 leg/s. No Grab at
        // 0; one Release at 1.0.
        Options o;
        o.dur = 2.0;
        std::vector<BoneTrack> t = Rig(walk_l, walk_r, o);
        AddHands(t, HandMove(-0.3, 1.5 * kLeg, {{0.0, 1.0}}), nullptr, o);
        const PhysicsAnalysis a = AnalyseMotion(t);
        CHECK(a.ok && !a.hand[1].part.present);
        const std::vector<PhysicsEvent> ev = HandEvents(a);
        CHECK(Of(ev, PhysicsKind::Grab).empty());
        CHECK(StepsNear(Of(ev, PhysicsKind::Release, 'L'), {1.0}, 0.02));
        // No bone for the right hand: said, and the left hand's events stay.
        CHECK(a.hand_missing == std::vector<std::string>{"R hand: no bone for right hand"});
        CHECK(HandSummary(a, ev) == "hands: grabs 0, releases 1, pivots 0; missing: R hand: no bone for right hand");
    }
    {
        // Brief stop: the hand stops 60 ms (1.0 to 1.06 s) between two fast moves: no Grab, no
        // Release.
        Options o;
        o.dur = 2.0;
        std::vector<BoneTrack> t = Rig(walk_l, walk_r, o);
        AddHands(t, HandMove(-0.3, 1.5 * kLeg, {{1.0, 1.06}}), HandStill(0.3), o);
        CHECK(HandEvents(AnalyseMotion(t)).empty());
        // A longer stop (150 ms) is a grab and a release.
        std::vector<BoneTrack> u = Rig(walk_l, walk_r, o);
        AddHands(u, HandMove(-0.3, 1.5 * kLeg, {{1.0, 1.2}}), HandStill(0.3), o);
        const std::vector<PhysicsEvent> ev = HandEvents(AnalyseMotion(u));
        CHECK(Of(ev, PhysicsKind::Grab).size() == 1 && Of(ev, PhysicsKind::Release).size() == 1);
    }
    {
        // No foot bones: legs (knees, up legs, hips) and hands, no heel, toe or toe end. The body
        // scale is twice the thigh (0.45 m here), and the hand events are still found.
        Options o;
        o.dur = 2.5;
        o.toes = false;
        std::vector<BoneTrack> t = Rig(walk_l, walk_r, o);
        t[static_cast<size_t>(Role::LeftHeel)].pos.clear();
        t[static_cast<size_t>(Role::RightHeel)].pos.clear();
        AddHands(t, HandStill(-0.3), HandMove(0.3, 1.5 * kLeg, {{1.0, 1.5}}), o);
        const PhysicsAnalysis a = AnalyseMotion(t);
        CHECK(a.ok && a.scale_note == "thigh x2" && std::fabs(a.leg_length - 0.90) < 1e-6);
        CHECK(a.ground_velocity.x == 0.0 && a.ground_velocity.z == 0.0);
        CHECK(FootEvents(a).empty());
        const std::vector<PhysicsEvent> ev = HandEvents(a);
        CHECK(StepsNear(Of(ev, PhysicsKind::Grab, 'R'), {1.0}, 0.02));
        CHECK(StepsNear(Of(ev, PhysicsKind::Release, 'R'), {1.5}, 0.02));
        // Neither foot part nor hand: no analysis, as before.
        std::vector<BoneTrack> bare = Rig(walk_l, walk_r, o);
        bare[static_cast<size_t>(Role::LeftHeel)].pos.clear();
        bare[static_cast<size_t>(Role::RightHeel)].pos.clear();
        const PhysicsAnalysis b = AnalyseMotion(bare);
        CHECK(!b.ok && b.error == "no foot part (heel, toe or toe end) and no hand" && HandEvents(b).empty());
        CHECK(HandSummary(b, {}) == "hands: skipped: " + b.error);
        // The heels give the scale when they are there: the thigh never replaces them.
        o.toes = true;
        CHECK(AnalyseMotion(Rig(walk_l, walk_r, o)).scale_note.empty());
        // Toes but no heel: the thigh gives the scale, and the toes still give the steps.
        Options w;
        std::vector<BoneTrack> toes = Rig(walk_l, walk_r, w);
        toes[static_cast<size_t>(Role::LeftHeel)].pos.clear();
        toes[static_cast<size_t>(Role::RightHeel)].pos.clear();
        const PhysicsAnalysis tc = AnalyseMotion(toes);
        CHECK(tc.ok && tc.scale_note == "thigh x2" && std::fabs(tc.leg_length - 0.90) < 1e-6);
        const std::vector<PhysicsEvent> tev = FootEvents(tc, SpeedTiming());
        CHECK(StepsNear(Of(tev, PhysicsKind::FootStep, 'L'), kLeft, 0.025));
        CHECK(StepsNear(Of(tev, PhysicsKind::FootStep, 'R'), kRight, 0.025));
        CHECK(Of(tev, PhysicsKind::Step, 'L', static_cast<int>(FootPart::Heel)).empty());
    }
    {
        // A grab cut short by the clip end: the hand lands 80 ms before it (under the min
        // contact) and is still found. A stop of 80 ms inside the clip is not.
        Options o;
        o.dur = 2.08;
        std::vector<BoneTrack> t = Rig(walk_l, walk_r, o);
        AddHands(t, HandStill(-0.3), HandMove(0.3, 1.5 * kLeg, {{2.0, 3.0}}), o);
        CHECK(StepsNear(Of(HandEvents(AnalyseMotion(t)), PhysicsKind::Grab, 'R'), {2.0}, 0.02));
        o.dur = 2.5;
        std::vector<BoneTrack> u = Rig(walk_l, walk_r, o);
        AddHands(u, HandStill(-0.3), HandMove(0.3, 1.5 * kLeg, {{1.0, 1.08}}), o);
        CHECK(HandEvents(AnalyseMotion(u)).empty());
    }
    {
        // Feet untouched: the walk with and without hands gives the same foot analysis and events.
        Options o;
        const std::vector<BoneTrack> feet = Rig(walk_l, walk_r, o);
        std::vector<BoneTrack>       both = feet;
        AddHands(both, HandMove(-0.3, 1.5 * kLeg, {{1.0, 1.5}}), HandStill(0.3), o);
        const PhysicsAnalysis a = AnalyseMotion(feet), b = AnalyseMotion(both);
        CHECK(a.ok && b.ok && a.leg_length == b.leg_length && a.scale_note == b.scale_note);
        CHECK(a.ground_velocity.x == b.ground_velocity.x && a.ground_velocity.z == b.ground_velocity.z);
        CHECK(a.missing == b.missing);
        for (const PhysicsParams& p : {PhysicsParams{}, SpeedTiming()}) {
            const std::vector<PhysicsEvent> fa = FootEvents(a, p), fb = FootEvents(b, p);
            CHECK(fa.size() == fb.size() && PhysicsSummary(a, fa) == PhysicsSummary(b, fb));
            CHECK(PhysicsEventLines(fa, "  ") == PhysicsEventLines(fb, "  "));
        }
        CHECK(HandEvents(a).empty() && HandSummary(a, {}) ==
                                           "hands: grabs 0, releases 0, pivots 0; missing: L hand: no bone for left hand, R hand: "
                                           "no bone for right hand");
        CHECK(!HandEvents(b).empty());
    }
    {
        // The hand roles are physics roles; hand marker names.
        const std::vector<Role>& roles = PhysicsRoles();
        CHECK(std::find(roles.begin(), roles.end(), Role::LeftHand) != roles.end() &&
              std::find(roles.begin(), roles.end(), Role::RightHand) != roles.end());
        PhysicsEvent e;
        e.kind = PhysicsKind::Grab;
        e.side = 'L';
        e.part = -1;
        e.strength = 1.44;
        CHECK(PhysicsMarkerName(e) == "PHY L grab 1.4");
        e.kind = PhysicsKind::Release;
        e.side = 'R';
        e.strength = 0.92;
        CHECK(PhysicsMarkerName(e) == "PHY R release 0.9" && IsPhysicsMarkerName(PhysicsMarkerName(e).c_str()));
    }

    // ---- Hand pivots (story 10-8e) --------------------------------------------------------------
    {
        // Planted turn: the right hand lands with a grab (1.5 leg/s) at 1.0 s, rests 0.6 s, and its
        // heading turns 90 deg in 250 ms mid-rest (1.175 s): one Hand Pivot of about 90 deg, with
        // the hand after a grab or not. The left hand hangs still, never turning.
        Options o;
        o.dur = 2.5;
        std::vector<BoneTrack> t = Rig(walk_l, walk_r, o);
        AddHands(t, HandStill(-0.3), HandMove(0.3, 1.5 * kLeg, {{1.0, 1.6}}), o, [](double) { return 0.4; },
                 TurnYaw(1.175, 0.25, 90.0));
        const PhysicsAnalysis a = AnalyseMotion(t);
        CHECK(a.ok && a.hand[0].has_heading && a.hand[1].has_heading);
        CHECK(a.hand[0].pivots.empty() && a.hand[1].pivots.size() == 1);
        PhysicsParams lax;
        lax.hand_pivot_after_grab = false;
        for (const PhysicsParams& p : {PhysicsParams{}, lax}) {
            const std::vector<PhysicsEvent> ev = HandEvents(a, p);
            const std::vector<PhysicsEvent> pv = Of(ev, PhysicsKind::HandPivot);
            CHECK(pv.size() == 1);
            if (pv.size() == 1) {
                CHECK(pv[0].side == 'R' && pv[0].part == -1);
                CHECK(pv[0].strength > 80.0 && pv[0].strength < 95.0);
                CHECK(pv[0].start_s == pv[0].time_s && pv[0].time_s > 1.1 && pv[0].time_s < 1.2);
                CHECK(pv[0].end_s > 1.4 && pv[0].end_s < 1.5);
            }
            CHECK(StepsNear(Of(ev, PhysicsKind::Grab, 'R'), {1.0}, 0.02));
            CHECK(StepsNear(Of(ev, PhysicsKind::Release, 'R'), {1.6}, 0.02));
            CHECK(HandSummary(a, ev) == "hands: grabs 1, releases 1, pivots 1");
        }
        // No heading source: the same hands without rotations: no hand pivot, the same grab and
        // release.
        std::vector<BoneTrack> u = Rig(walk_l, walk_r, o);
        AddHands(u, HandStill(-0.3), HandMove(0.3, 1.5 * kLeg, {{1.0, 1.6}}), o);
        const PhysicsAnalysis b = AnalyseMotion(u);
        CHECK(b.ok && !b.hand[1].has_heading && b.hand[1].pivots.empty());
        const std::vector<PhysicsEvent> eb = HandEvents(b, lax);
        CHECK(Of(eb, PhysicsKind::HandPivot).empty());
        std::vector<PhysicsEvent> ea = HandEvents(a);
        ea.erase(std::remove_if(ea.begin(), ea.end(), [](const PhysicsEvent& e) { return e.kind == PhysicsKind::HandPivot; }),
                 ea.end());
        CHECK(PhysicsEventLines(ea, "") == PhysicsEventLines(eb, ""));
        // Feet untouched: the same foot events with turning hands, still hands and no hands.
        const PhysicsAnalysis f = AnalyseMotion(Rig(walk_l, walk_r, o));
        for (const PhysicsParams& p : {PhysicsParams{}, SpeedTiming()}) {
            const std::vector<PhysicsEvent> fa = FootEvents(a, p), fb = FootEvents(b, p), ff = FootEvents(f, p);
            CHECK(PhysicsEventLines(fa, "") == PhysicsEventLines(ff, "") && PhysicsEventLines(fb, "") == PhysicsEventLines(ff, ""));
            CHECK(PhysicsSummary(a, fa) == PhysicsSummary(f, ff));
            for (const PhysicsEvent& e : fa) CHECK(e.kind != PhysicsKind::HandPivot);
        }
    }
    {
        // Gentle placement: the same turn, the hand placed at 0.7 leg/s (under the grab's
        // approach): none after a grab only; one with any resting hand.
        Options o;
        o.dur = 2.5;
        std::vector<BoneTrack> t = Rig(walk_l, walk_r, o);
        AddHands(t, HandStill(-0.3), HandMove(0.3, 0.7 * kLeg, {{1.0, 1.6}}), o, nullptr, TurnYaw(1.175, 0.25, 90.0));
        const PhysicsAnalysis a = AnalyseMotion(t);
        PhysicsParams         lax;
        lax.hand_pivot_after_grab = false;
        CHECK(Of(HandEvents(a), PhysicsKind::Grab).empty() && Of(HandEvents(a), PhysicsKind::HandPivot).empty());
        const std::vector<PhysicsEvent> pv = Of(HandEvents(a, lax), PhysicsKind::HandPivot, 'R');
        CHECK(pv.size() == 1 && pv[0].strength > 80.0);
    }
    {
        // A hand at rest from the clip start pivots after a grab or not (it never landed).
        Options o;
        o.dur = 2.0;
        std::vector<BoneTrack> t = Rig(walk_l, walk_r, o);
        AddHands(t, HandMove(-0.3, 1.5 * kLeg, {{0.0, 1.0}}), nullptr, o, TurnYaw(0.4, 0.25, -90.0));
        const std::vector<PhysicsEvent> pv = Of(HandEvents(AnalyseMotion(t)), PhysicsKind::HandPivot, 'L');
        CHECK(pv.size() == 1 && pv[0].strength > 80.0 && pv[0].strength < 95.0);
    }
    {
        // Small turn: 10 deg, none; one at sensitivity 100 (the angle and rate thresholds halved).
        Options o;
        o.dur = 2.5;
        std::vector<BoneTrack> t = Rig(walk_l, walk_r, o);
        AddHands(t, HandStill(-0.3), HandMove(0.3, 1.5 * kLeg, {{1.0, 1.6}}), o, nullptr, TurnYaw(1.175, 0.25, 10.0));
        CHECK(Of(HandEvents(AnalyseMotion(t)), PhysicsKind::HandPivot).empty());
        PhysicsParams sens;
        sens.hand_pivot_min_deg /= 2.0;
        sens.hand_pivot_rate_dps /= 2.0;
        const std::vector<PhysicsEvent> pv = Of(HandEvents(AnalyseMotion(t, sens), sens), PhysicsKind::HandPivot, 'R');
        CHECK(pv.size() == 1 && pv[0].strength > 8.0 && pv[0].strength < 11.0);
        // The foot pivot's thresholds never move the hand's.
        PhysicsParams foot;
        foot.pivot_min_deg /= 2.0;
        foot.pivot_rate_dps /= 2.0;
        CHECK(Of(HandEvents(AnalyseMotion(t, foot), foot), PhysicsKind::HandPivot).empty());
    }
    {
        // Turn while moving, lands turning, leaves at once: none.
        Options o;
        o.dur = 2.5;
        PhysicsParams lax;
        lax.hand_pivot_after_grab = false;
        for (double turn_at : {0.5, 1.05, 1.30}) {
            // 0.5: the hand moves (it rests 1.0 to 1.6); 1.05: the turn starts 50 ms into the
            // rest; 1.30: it ends 50 ms before the hand leaves.
            std::vector<BoneTrack> t = Rig(walk_l, walk_r, o);
            AddHands(t, HandStill(-0.3), HandMove(0.3, 1.5 * kLeg, {{1.0, 1.6}}), o, nullptr, TurnYaw(turn_at, 0.25, 90.0));
            const PhysicsAnalysis a = AnalyseMotion(t);
            CHECK(a.ok && a.hand[1].has_heading && a.hand[1].pivots.empty());
            CHECK(Of(HandEvents(a, lax), PhysicsKind::HandPivot).empty());
            CHECK(StepsNear(Of(HandEvents(a), PhysicsKind::Grab, 'R'), {1.0}, 0.02));
        }
        // A hand that never rests: no heading source (nothing to calibrate on).
        std::vector<BoneTrack> t = Rig(walk_l, walk_r, o);
        AddHands(t, HandStill(-0.3), HandMove(0.3, 1.5 * kLeg, {}), o, nullptr, TurnYaw(1.0, 0.25, 90.0));
        const PhysicsAnalysis a = AnalyseMotion(t);
        CHECK(a.ok && a.hand[1].part.contacts.empty() && !a.hand[1].has_heading && a.hand[1].pivots.empty());
    }
    {
        // The level check and the heading after the turn: the planted right hand (resting 1.0 to
        // 2.6 s) rolls 80 deg about a horizontal axis (1.4 to 1.55 s), stays rolled to 1.75 s and
        // rolls back by 1.9 s. The roll axis is across the hand's heading axis (the module's: the
        // bone axis least along the hand's up, made perpendicular to it), so that axis tilts
        // 70 deg or more: no heading over the rolled span. Rolled alone, or while the heading
        // also turns 90 deg (1.4 to 1.75 s): no Hand Pivot, even with any resting hand (the part
        // of the turn before the tilt has no heading after it).
        const Vec3d m = RotateBy(Quatd{HandBoneFrame().w, -HandBoneFrame().x, -HandBoneFrame().y, -HandBoneFrame().z},
                                 Vec3d{0.0, 1.0, 0.0});
        Vec3d       e{1.0, 0.0, 0.0};
        if (std::fabs(m.y) < std::fabs(m.x) && std::fabs(m.y) <= std::fabs(m.z)) e = Vec3d{0.0, 1.0, 0.0};
        else if (std::fabs(m.z) < std::fabs(m.x) && std::fabs(m.z) < std::fabs(m.y)) e = Vec3d{0.0, 0.0, 1.0};
        const double d = e.x * m.x + e.y * m.y + e.z * m.z;
        const Vec3d  v = RotateBy(HandBoneFrame(), Vec3d{e.x - d * m.x, e.y - d * m.y, e.z - d * m.z});  // horizontal
        const double vl = std::sqrt(v.x * v.x + v.z * v.z);
        CHECK(std::fabs(v.y) < 1e-9 && vl > 0.1);
        const Vec3d  across{-v.z / vl, 0.0, v.x / vl};  // up x v, horizontal
        auto roll = [](double t) {
            return 80.0 * kDeg * (t < 1.75 ? Ease((t - 1.4) / 0.15) : 1.0 - Ease((t - 1.75) / 0.15));
        };
        PhysicsParams lax;
        lax.hand_pivot_after_grab = false;
        for (bool turn : {false, true}) {
            Options o;
            o.dur = 3.2;
            std::vector<BoneTrack> t = Rig(walk_l, walk_r, o);
            AddHands(t, HandStill(-0.3), HandMove(0.3, 1.5 * kLeg, {{1.0, 2.6}}), o);
            BoneTrack& h = t[static_cast<size_t>(Role::RightHand)];
            for (size_t i = 0; i < h.pos.size(); ++i) {
                const double time = i / o.rate;
                const double yaw = turn ? 90.0 * kDeg * Ease((time - 1.4) / 0.35) : 0.0;
                h.rot_world.push_back(Mul(AxisAngle(0.0, 1.0, 0.0, yaw),
                                          Mul(AxisAngle(across.x, 0.0, across.z, roll(time)), HandBoneFrame())));
            }
            const PhysicsAnalysis a = AnalyseMotion(t);
            CHECK(a.ok && a.hand[1].has_heading && a.hand[1].pivots.empty());
            CHECK(Of(HandEvents(a, lax), PhysicsKind::HandPivot).empty());
            bool tilted = a.hand[1].heading_valid.size() == h.pos.size();
            for (double time = 1.55; tilted && time < 1.75; time += 1.0 / o.rate)
                tilted = a.hand[1].heading_valid[static_cast<size_t>(std::lround(time * o.rate))] == 0;
            CHECK(tilted);
            CHECK(a.hand[1].heading_valid[static_cast<size_t>(std::lround(1.2 * o.rate))] &&
                  a.hand[1].heading_valid[static_cast<size_t>(std::lround(2.2 * o.rate))]);
            // The same turn with no roll pivots (the roll is what stops it).
            if (turn) {
                std::vector<BoneTrack> flat = Rig(walk_l, walk_r, o);
                AddHands(flat, HandStill(-0.3), HandMove(0.3, 1.5 * kLeg, {{1.0, 2.6}}), o, nullptr,
                         [](double time) { return 90.0 * kDeg * Ease((time - 1.4) / 0.35); });
                CHECK(Of(HandEvents(AnalyseMotion(flat), lax), PhysicsKind::HandPivot).size() == 1);
            }
        }
    }
    {
        // No hand bone: no hand pivot on that side; the other hand's is found.
        Options o;
        o.dur = 2.5;
        std::vector<BoneTrack> t = Rig(walk_l, walk_r, o);
        AddHands(t, nullptr, HandMove(0.3, 1.5 * kLeg, {{1.0, 1.6}}), o, nullptr, TurnYaw(1.175, 0.25, 90.0));
        const PhysicsAnalysis a = AnalyseMotion(t);
        CHECK(a.ok && !a.hand[0].part.present && !a.hand[0].has_heading);
        const std::vector<PhysicsEvent> ev = HandEvents(a);
        CHECK(Of(ev, PhysicsKind::HandPivot, 'L').empty() && Of(ev, PhysicsKind::HandPivot, 'R').size() == 1);
        // Hand pivot marker names and event lines.
        PhysicsEvent e;
        e.kind = PhysicsKind::HandPivot;
        e.side = 'L';
        e.part = -1;
        e.strength = 87.4;
        e.time_s = e.start_s = 1.0;
        e.end_s = 1.25;
        CHECK(PhysicsMarkerName(e) == "PHY L hand pivot 87deg 250ms" && IsPhysicsMarkerName(PhysicsMarkerName(e).c_str()));
        CHECK(PhysicsEventLines({e}, "  ") == "     1.000 s  PHY L hand pivot 87deg 250ms  (to 1.250 s)\n");
        CHECK(PhysicsSummary(a, {e}) == PhysicsSummary(a, {}));
        CHECK(PhysicsParams{}.hand_pivot_rate_dps == PhysicsParams{}.pivot_rate_dps &&
              PhysicsParams{}.hand_pivot_min_deg == PhysicsParams{}.pivot_min_deg && PhysicsParams{}.hand_pivot_after_grab);
    }

    // ---- Arrival at the clip end (story 10-8j) ----------------------------------------------
    {
        // The right hand comes to rest 30 ms before the end (7 samples: too few to pay the switch
        // cost) after moving at 1.5 leg/s: a grab where it brakes, no release.
        Options o;
        o.dur = 2.0;
        const FootFn lf = Planted(-0.1, 0.0), rf = Planted(0.1, 0.0);
        auto grabs = [&](const HandFn& hand, const PhysicsParams& p) {
            std::vector<BoneTrack> t = Rig(lf, rf, o);
            AddHands(t, nullptr, hand, o);
            return HandEvents(AnalyseMotion(t, p), p);
        };
        const std::vector<PhysicsEvent> ev = grabs(HandMove(0.3, 1.5 * kLeg, {{1.97, 3.0}}), PhysicsParams{});
        CHECK(StepsNear(Of(ev, PhysicsKind::Grab, 'R'), {1.97}, 0.015) && Of(ev, PhysicsKind::Release, 'R').empty());
        // Approached under the min approach (0.6 leg/s): none.
        CHECK(Of(grabs(HandMove(0.3, 0.6 * kLeg, {{1.97, 3.0}}), PhysicsParams{}), PhysicsKind::Grab).empty());
        // Speeds up again before the end (rests 40 ms, leaves 40 ms before the end): none.
        CHECK(Of(grabs(HandMove(0.3, 1.5 * kLeg, {{1.92, 1.96}}), PhysicsParams{}), PhysicsKind::Grab).empty());
        // Brakes within the last ~25 ms, where the held end of the smoothing lowers every speed: none.
        CHECK(Of(grabs(HandMove(0.3, 1.5 * kLeg, {{1.99, 3.0}}), PhysicsParams{}), PhysicsKind::Grab).empty());
        // Brakes earlier than the window (200 ms before the end): a contact the Viterbi finds, the
        // same grab as before the story.
        CHECK(StepsNear(Of(grabs(HandMove(0.3, 1.5 * kLeg, {{1.8, 3.0}}), PhysicsParams{}), PhysicsKind::Grab, 'R'),
                        {1.8}, 0.015));
        // Looping: no arrival (the unrolled track's end is the next cycle, the hand moves at t = 0).
        PhysicsParams loop;
        loop.looping = true;
        std::vector<BoneTrack> t = Rig(lf, rf, o);
        AddHands(t, nullptr, HandMove(0.3, 1.5 * kLeg, {{1.97, 3.0}}), o);
        const PhysicsAnalysis on = AnalyseMotion(t, loop);
        CHECK(on.ok && !on.hand[1].part.contact.empty() && !on.hand[1].part.contact.back());
        CHECK(Of(HandEvents(on, loop), PhysicsKind::Grab).empty());
        // The planted feet get nothing at the end.
        CHECK(FootEvents(AnalyseMotion(t)).empty());
    }
    {
        // A foot that lands 30 ms before the end (the walk's left foot, contacts at 0.97 and
        // 1.97 s): a step of each part near its landing, a foot step, no lift-off at the end;
        // looping, no arrival.
        Options o;
        o.dur = 2.0;
        const PhysicsParams             p = SpeedTiming();
        const PhysicsAnalysis           a = AnalyseMotion(Rig(WalkFoot(0.97 - 1.0, -0.1), Planted(0.1, 0.0), o), p);
        const std::vector<PhysicsEvent> ev = FootEvents(a, p);
        CHECK(a.ok);
        for (int q = 0; q < kFootPartCount; ++q) {
            CHECK(StepsNear(Of(ev, PhysicsKind::Step, 'L', q), {0.97, 1.97}, 0.03));
            CHECK(a.foot[0].part[q].contacts.size() == 3 && a.foot[0].part[q].contacts.back().end == static_cast<int>(a.samples));
        }
        CHECK(StepsNear(Of(ev, PhysicsKind::FootStep, 'L'), {0.97, 1.97}, 0.03));
        CHECK(StepsNear(Of(ev, PhysicsKind::LiftOff, 'L'), {0.57, 1.57}, 0.03));
        CHECK(Of(ev, PhysicsKind::Slide).empty() && Of(ev, PhysicsKind::Pivot).empty());
    }

    // ---- Looping clips (story 10-8i) ---------------------------------------------------------
    {
        PhysicsParams loop;
        loop.looping = true;
        // Seam grab: the right hand comes to rest 30 ms before the end (too short to pay the
        // switch cost) and is still at rest for 210 ms from the start. Off: the end rest is an
        // arrival at the clip end (story 10-8j: its grab near 1.97 s); the start's, cut by the
        // clip start, gives its release. On: one
        // contact across the seam, read whole: its grab near 1.97 s and its release near 0.21 s.
        // The feet are planted the whole cycle: no foot event (no start / end waiver).
        Options o;
        o.dur = 2.0;
        std::vector<BoneTrack> t = Rig(Planted(-0.1, 0.0), Planted(0.1, 0.0), o);
        AddHands(t, HandStill(-0.3), HandLoop(0.3, 1.5 * kLeg, 0.21, 1.97), o);
        const PhysicsAnalysis off = AnalyseMotion(t);
        CHECK(off.ok && !off.looping && off.window_first == 0 && off.window_samples == off.samples);
        const std::vector<PhysicsEvent> eoff = HandEvents(off);
        CHECK(StepsNear(Of(eoff, PhysicsKind::Grab, 'R'), {1.97}, 0.02));
        CHECK(StepsNear(Of(eoff, PhysicsKind::Release, 'R'), {0.21}, 0.02));
        const PhysicsAnalysis on = AnalyseMotion(t, loop);
        const size_t          n = t[static_cast<size_t>(Role::RightHand)].pos.size();
        CHECK(on.ok && on.looping && on.window_first == n - 1 && on.window_samples == n && on.samples == 3 * (n - 1) + 1);
        CHECK(std::fabs(on.leg_length - off.leg_length) < 1e-9);
        const std::vector<PhysicsEvent> eon = HandEvents(on, loop);
        CHECK(StepsNear(Of(eon, PhysicsKind::Grab, 'R'), {1.97}, 0.02));
        CHECK(StepsNear(Of(eon, PhysicsKind::Release, 'R'), {0.21}, 0.02));
        CHECK(eon.size() == 2);
        CHECK(FootEvents(on, loop).empty() && FootEvents(off).empty());
        const double T = (n - 1) / o.rate;
        for (const PhysicsEvent& e : eon) CHECK(e.time_s >= 0.0 && e.time_s < T);

        // Rigid end-pose shift: the same cycle drifting 2.6 cm up and 3 cm forward (the end pose =
        // the start pose shifted, an in-place climb's rung): no step, lift-off or contact artefact
        // at the seam, the same hand events.
        std::vector<BoneTrack> dr = t;
        Drift(dr, Vec3d{0.0, 0.026, 0.03}, o.dur);
        const PhysicsAnalysis shifted = AnalyseMotion(dr, loop);
        CHECK(shifted.ok);
        CHECK(FootEvents(shifted, loop).empty());
        const std::vector<PhysicsEvent> esh = HandEvents(shifted, loop);
        CHECK(esh.size() == eon.size());
        for (size_t i = 0; i < esh.size() && i < eon.size(); ++i)
            CHECK(esh[i].kind == eon[i].kind && std::fabs(esh[i].time_s - eon[i].time_s) < 0.005);
        for (int s = 0; s < 2; ++s)
            for (int q = 0; q < kFootPartCount; ++q) CHECK(shifted.foot[s].part[q].contacts.size() == 1);  // whole track
    }
    {
        // The walk read as a loop (4 cycles of 1 s, the end pose 2.4 m ahead): the same steps; the
        // left foot's lift-off at the seam (it leaves at t = 0) is found once; every event in
        // [0, T).
        PhysicsParams loop = SpeedTiming();
        loop.looping = true;
        const std::vector<BoneTrack>    t = Rig(walk_l, walk_r, Options{});
        const PhysicsAnalysis           a = AnalyseMotion(t, loop);
        const std::vector<PhysicsEvent> ev = FootEvents(a, loop);
        CHECK(a.ok);
        for (int q = 0; q < kFootPartCount; ++q) {
            CHECK(StepsNear(Of(ev, PhysicsKind::Step, 'L', q), kLeft, 0.025));
            CHECK(StepsNear(Of(ev, PhysicsKind::Step, 'R', q), kRight, 0.025));
            std::vector<double> pt;
            for (const PhysicsEvent& e : Of(ev, PhysicsKind::Step, 'L', q)) pt.push_back(e.time_s);
            CHECK(pt == PartStepTimes(a, 'L', q, StepTiming::Speed, 0.0));
        }
        // The seam lift-off (its speed crosses just before the seam here): once, at the seam.
        const std::vector<PhysicsEvent> lifts = Of(ev, PhysicsKind::LiftOff, 'L');
        CHECK(lifts.size() == 4);
        if (lifts.size() == 4) {
            CHECK(StepsNear({lifts[0], lifts[1], lifts[2]}, {1.0, 2.0, 3.0}, 0.025));
            CHECK(lifts[3].time_s > 3.975);
        }
        CHECK(StepsNear(Of(ev, PhysicsKind::LiftOff, 'R'), {0.5, 1.5, 2.5, 3.5}, 0.025));
        for (const PhysicsEvent& e : ev) CHECK(e.time_s >= 0.0 && e.time_s < 4.0);
        // Off: as before (no lift-off at the clip start).
        CHECK(StepsNear(Of(FootEvents(AnalyseMotion(t), loop), PhysicsKind::LiftOff, 'L'), {1.0, 2.0, 3.0}, 0.025));
    }
    {
        // Rotations unrolled with the positions: a hand pivot in the middle of the clip (the hand's
        // rotation track) is the same with and without looping, and a toe rig without a toe end
        // keeps its rotation heading source.
        PhysicsParams loop;
        loop.looping = true;
        Options o;
        o.dur = 2.5;
        std::vector<BoneTrack> t = Rig(walk_l, walk_r, o);
        AddHands(t, nullptr, HandMove(0.3, 1.5 * kLeg, {{1.0, 1.6}}), o, nullptr, TurnYaw(1.175, 0.25, 90.0));
        const PhysicsAnalysis off = AnalyseMotion(t), on = AnalyseMotion(t, loop);
        CHECK(off.hand[1].has_heading && on.hand[1].has_heading && on.hand[1].heading_valid.size() == on.samples);
        const std::vector<PhysicsEvent> poff = Of(HandEvents(off), PhysicsKind::HandPivot, 'R');
        const std::vector<PhysicsEvent> pon = Of(HandEvents(on, loop), PhysicsKind::HandPivot, 'R');
        CHECK(poff.size() == 1 && pon.size() == 1);
        if (poff.size() == 1 && pon.size() == 1)
            CHECK(std::fabs(pon[0].time_s - poff[0].time_s) < 0.005 && std::fabs(pon[0].strength - poff[0].strength) < 1.0);
        Options r;
        r.rot = true;
        r.tip = false;
        const std::vector<BoneTrack> toes = Rig(walk_l, walk_r, r);
        const PhysicsAnalysis        roff = AnalyseMotion(toes), ron = AnalyseMotion(toes, loop);
        for (int s = 0; s < 2; ++s) {
            CHECK(roff.foot[s].heading_source == HeadingSource::Rotation);
            CHECK(ron.foot[s].heading_source == HeadingSource::Rotation && ron.foot[s].heading_valid.size() == ron.samples);
        }
    }
    {
        // Seam event: a grab exactly on the seam (its copies at the middle cycle's start and the
        // third cycle's start) is one event, at 0.
        PhysicsAnalysis a;
        a.ok = true;
        a.looping = true;
        a.rate_hz = 240.0;
        a.window_first = 240;
        a.window_samples = 241;
        a.samples = 721;
        a.leg_length = 0.87;
        PartTrack& pt = a.hand[1].part;
        a.hand[1].side = 'R';
        pt.present = true;
        for (int k = 1; k <= 2; ++k) {
            PartContact c;
            c.start = 240 * k;
            c.end = 240 * k + 60;
            c.approach = c.departure = 2.0;
            for (double& s : c.step_s) s = c.start / 240.0;
            c.lift_s = c.end / 240.0;
            pt.contacts.push_back(c);
        }
        const std::vector<PhysicsEvent> ev = HandEvents(a);
        CHECK(Of(ev, PhysicsKind::Grab).size() == 1 && Of(ev, PhysicsKind::Release).size() == 1);
        if (!Of(ev, PhysicsKind::Grab).empty()) CHECK(Of(ev, PhysicsKind::Grab)[0].time_s == 0.0);
        CHECK(StepsNear(Of(ev, PhysicsKind::Release), {0.25}, 1e-9));
    }

    // Story 10-9a: jumps. Both feet leave together at 1.0 s and fly a 0.5 s ballistic arc (31 cm):
    // one Jump at the take-off, one Land at the landing, the hips' vertical speed as strength (2.8
    // leg/s at take-off before the smoothing). Story 10-9a-3: the feet tie, so the first foot (L)
    // names both, with the part that leaves last / touches first.
    {
        Options o;
        o.dur = 2.5;
        const std::vector<BoneTrack> rig = Rig(JumpFoot(-0.1, 1.0, 0.5), JumpFoot(0.1, 1.0, 0.5), o);
        const PhysicsAnalysis        a = AnalyseMotion(rig);
        CHECK(a.ok && a.hips_vy.size() == a.samples && a.hips_missing.empty());
        const std::vector<PhysicsEvent> ev = JumpEvents(a);
        const std::vector<PhysicsEvent> jump = Of(ev, PhysicsKind::Jump), land = Of(ev, PhysicsKind::Land);
        CHECK(ev.size() == 2 && jump.size() == 1 && land.size() == 1);
        if (jump.size() == 1 && land.size() == 1) {
            CHECK(std::fabs(jump[0].time_s - 1.0) < 0.03 && std::fabs(land[0].time_s - 1.5) < 0.04);
            CHECK(jump[0].side == 'L' && jump[0].part >= 0 && land[0].side == 'L' && land[0].part >= 0);
            CHECK(jump[0].strength > 1.0 && land[0].strength > 1.0);
            CHECK(PhysicsMarkerName(jump[0]).compare(0, 11, "PHY L jump ") == 0);
            CHECK(PhysicsMarkerName(land[0]).compare(0, 11, "PHY L land ") == 0);
            // The take-off is the L foot's lift-off without its offset (story 10-8p), the landing
            // its foot step.
            const double                    lift_off = PhysicsParams{}.lift_offset_s;
            const std::vector<PhysicsEvent> lifts = Of(FootEvents(a), PhysicsKind::LiftOff, 'L');
            const std::vector<PhysicsEvent> steps = Of(FootEvents(a), PhysicsKind::FootStep, 'L');
            CHECK(lifts.size() == 1 && steps.size() == 1);
            if (lifts.size() == 1 && steps.size() == 1) {
                CHECK(std::fabs(lifts[0].time_s - lift_off - jump[0].time_s) < 1e-9 && lifts[0].part == jump[0].part);
                CHECK(steps[0].time_s == land[0].time_s && steps[0].part == land[0].part);
            }
        }
        CHECK(JumpSummary(a, ev) == "jumps: take-offs 1, landings 1");
        // The foot events still see two lift-offs and two foot steps (Land / Jump replace them
        // only in the auto layer).
        const std::vector<PhysicsEvent> fev = FootEvents(a);
        CHECK(Of(fev, PhysicsKind::LiftOff).size() == 2 && Of(fev, PhysicsKind::FootStep).size() == 2);

        // The feet 30 ms apart (the right leaves last, the left lands first): one Jump on the last
        // foot leaving (R, its lift-off), one Land on the first touching (L, its foot step).
        const std::vector<BoneTrack> rig2 = Rig(JumpFoot(-0.1, 1.0, 0.5), JumpFoot(0.1, 1.03, 0.5), o);
        const PhysicsAnalysis        a2 = AnalyseMotion(rig2, SpeedTiming());
        const std::vector<PhysicsEvent> ev2 = JumpEvents(a2, SpeedTiming());
        const std::vector<PhysicsEvent> fev2 = FootEvents(a2, SpeedTiming());
        CHECK(Of(ev2, PhysicsKind::Jump).size() == 1 && Of(ev2, PhysicsKind::Land).size() == 1);
        if (Of(ev2, PhysicsKind::Jump).size() == 1 && Of(fev2, PhysicsKind::LiftOff, 'R').size() == 1 &&
            Of(fev2, PhysicsKind::LiftOff, 'L').size() == 1) {
            CHECK(Of(ev2, PhysicsKind::Jump)[0].side == 'R');
            CHECK(std::fabs(Of(ev2, PhysicsKind::Jump)[0].time_s - (Of(fev2, PhysicsKind::LiftOff, 'R')[0].time_s - SpeedTiming().lift_offset_s)) < 1e-9);
            CHECK(Of(ev2, PhysicsKind::Jump)[0].part == Of(fev2, PhysicsKind::LiftOff, 'R')[0].part);
            CHECK(Of(fev2, PhysicsKind::LiftOff, 'L')[0].time_s < Of(fev2, PhysicsKind::LiftOff, 'R')[0].time_s);
        }
        if (Of(ev2, PhysicsKind::Land).size() == 1 && Of(fev2, PhysicsKind::FootStep, 'L').size() == 1) {
            CHECK(Of(ev2, PhysicsKind::Land)[0].side == 'L');
            CHECK(Of(ev2, PhysicsKind::Land)[0].time_s == Of(fev2, PhysicsKind::FootStep, 'L')[0].time_s);
        }
        // The right foot 5 ms behind (a two-footed take-off and landing, within jump_tie_s): the
        // first foot (L) names both, at its own lift-off / step.
        const std::vector<BoneTrack> rig3 = Rig(JumpFoot(-0.1, 1.0, 0.5), JumpFoot(0.1, 1.005, 0.5), o);
        const PhysicsAnalysis        a3 = AnalyseMotion(rig3, SpeedTiming());
        const std::vector<PhysicsEvent> ev3 = JumpEvents(a3, SpeedTiming());
        const std::vector<PhysicsEvent> fev3 = FootEvents(a3, SpeedTiming());
        CHECK(Of(ev3, PhysicsKind::Jump, 'L').size() == 1 && Of(ev3, PhysicsKind::Land, 'L').size() == 1 && ev3.size() == 2);
        if (Of(ev3, PhysicsKind::Jump, 'L').size() == 1 && Of(fev3, PhysicsKind::LiftOff, 'L').size() == 1 &&
            Of(fev3, PhysicsKind::LiftOff, 'R').size() == 1) {
            CHECK(std::fabs(Of(ev3, PhysicsKind::Jump, 'L')[0].time_s - (Of(fev3, PhysicsKind::LiftOff, 'L')[0].time_s - SpeedTiming().lift_offset_s)) < 1e-9);
            CHECK(Of(fev3, PhysicsKind::LiftOff, 'L')[0].time_s < Of(fev3, PhysicsKind::LiftOff, 'R')[0].time_s);
        }
        PhysicsParams no_tie = SpeedTiming();
        no_tie.jump_tie_s = 0.0;
        CHECK(Of(JumpEvents(a3, no_tie), PhysicsKind::Jump, 'R').size() == 1);
        // The landing tie: the right foot lands 5 ms before the left (it leaves 5 ms before too):
        // Land L by default, Land R with no tie; 30 ms before: Land R.
        const PhysicsAnalysis a4 = AnalyseMotion(Rig(JumpFoot(-0.1, 1.0, 0.5), JumpFoot(0.1, 0.995, 0.5), o), SpeedTiming());
        CHECK(Of(JumpEvents(a4, SpeedTiming()), PhysicsKind::Land, 'L').size() == 1 &&
              Of(JumpEvents(a4, SpeedTiming()), PhysicsKind::Land).size() == 1);
        CHECK(Of(JumpEvents(a4, no_tie), PhysicsKind::Land, 'R').size() == 1 &&
              Of(JumpEvents(a4, no_tie), PhysicsKind::Land).size() == 1);
        const PhysicsAnalysis a5 = AnalyseMotion(Rig(JumpFoot(-0.1, 1.0, 0.5), JumpFoot(0.1, 0.97, 0.5), o), SpeedTiming());
        CHECK(Of(JumpEvents(a5, SpeedTiming()), PhysicsKind::Land, 'R').size() == 1 &&
              Of(JumpEvents(a5, SpeedTiming()), PhysicsKind::Land).size() == 1);

        // The same feet off while the hips are held up (hanging: the hands hold the body): no jump.
        std::vector<BoneTrack> held = rig;
        for (Vec3d& h : held[static_cast<size_t>(Role::Hips)].pos) h.y = 0.98;
        CHECK(JumpEvents(AnalyseMotion(held)).empty());
        CHECK(JumpFreeFallEvents(AnalyseMotion(held)).empty());  // story 10-9a-3: nor a free fall
        // Story 10-9a-3: the free fall finds the same take-off (and no landing).
        const std::vector<PhysicsEvent> ff = JumpFreeFallEvents(a);
        CHECK(ff.size() == 1 && jump.size() == 1);
        if (ff.size() == 1 && jump.size() == 1)
            CHECK(ff[0].kind == PhysicsKind::Jump && ff[0].time_s == jump[0].time_s && ff[0].side == jump[0].side);

        // No hips bone: no jump, said in the report; the foot events stay.
        std::vector<BoneTrack> nohips = rig;
        nohips[static_cast<size_t>(Role::Hips)] = BoneTrack{};
        const PhysicsAnalysis an = AnalyseMotion(nohips);
        CHECK(an.ok && an.hips_vy.empty() && !an.hips_missing.empty() && JumpEvents(an).empty());
        CHECK(JumpSummary(an, {}).find("; missing: hips") != std::string::npos);
        CHECK(Of(FootEvents(an), PhysicsKind::LiftOff).size() == 2);
    }
    // A run-like 100 ms flight (3 m/s forward): no jump at the default minimum flight (300 ms),
    // one with 80 ms.
    {
        Options o;
        o.dur = 2.0;
        const PhysicsAnalysis a =
            AnalyseMotion(Rig(JumpFoot(-0.1, 1.0, 0.1, 0.0, 3.0), JumpFoot(0.1, 1.0, 0.1, 0.0, 3.0), o));
        CHECK(a.ok && JumpEvents(a).empty());
        PhysicsParams shorter;
        shorter.jump_min_flight_s = 0.08;
        CHECK(Of(JumpEvents(a, shorter), PhysicsKind::Jump).size() == 1);
    }
    // Looping: a jump across the seam (take-off at 1.8 s of a 2 s cycle, landing at 0.3 s) is read
    // whole: one Jump and one Land, in clip time.
    {
        Options o;
        o.dur = 2.0;
        PhysicsParams loop;
        loop.looping = true;
        const PhysicsAnalysis a = AnalyseMotion(Rig(JumpFoot(-0.1, 1.8, 0.5, 2.0), JumpFoot(0.1, 1.8, 0.5, 2.0), o), loop);
        const std::vector<PhysicsEvent> ev = JumpEvents(a, loop);
        CHECK(Of(ev, PhysicsKind::Jump).size() == 1 && Of(ev, PhysicsKind::Land).size() == 1);
        if (Of(ev, PhysicsKind::Jump).size() == 1 && Of(ev, PhysicsKind::Land).size() == 1) {
            CHECK(std::fabs(Of(ev, PhysicsKind::Jump)[0].time_s - 1.8) < 0.03);
            CHECK(std::fabs(Of(ev, PhysicsKind::Land)[0].time_s - 0.3) < 0.04);
        }
    }

    // Story 10-9a-3: a take-off at the clip start. The right foot slides in at the floor's height
    // (1 m/s, braking), rests 20 ms from 0.03 s and leaves at 0.05 s on a 0.5 s arc; the left one
    // is in the air from the start and lands at 0.45 s. Not looping, the right foot's parts are in
    // contact from sample 0: Jump R at their last lift-off, Land L. Looping (the cycle wraps), no
    // contact from the clip start.
    {
        Options o;
        o.dur = 1.5;
        const FootFn start_r = [](double t) {
            FootPose f;
            f.ball = Vec3d{0.1, 0.02, 0.0};
            if (t < 0.03) f.ball.z = -16.7 * (0.03 - t) * (0.03 - t);
            const double u = t - 0.05;
            if (u > 0.0 && u < 0.5) f.ball.y += 9.81 * 0.25 * u - 0.5 * 9.81 * u * u;
            return f;
        };
        const std::vector<BoneTrack> rig = Rig(JumpFoot(-0.1, -0.1, 0.55), start_r, o);
        const PhysicsAnalysis        a = AnalyseMotion(rig);
        CHECK(a.ok);
        bool from_start = false;
        for (const PartTrack& pt : a.foot[1].part)
            if (pt.present && !pt.contacts.empty() && pt.contacts[0].start == 0 && pt.contacts[0].end > 1) from_start = true;
        CHECK(from_start);
        const std::vector<PhysicsEvent> ev = JumpEvents(a);
        CHECK(Of(ev, PhysicsKind::Jump, 'R').size() == 1 && Of(ev, PhysicsKind::Land, 'L').size() == 1 && ev.size() == 2);
        if (Of(ev, PhysicsKind::Jump, 'R').size() == 1) CHECK(std::fabs(Of(ev, PhysicsKind::Jump, 'R')[0].time_s - 0.05) < 0.03);
        // No step at the clip start (the contact has no arrival).
        CHECK(Of(FootEvents(a), PhysicsKind::FootStep, 'R').empty() || Of(FootEvents(a), PhysicsKind::FootStep, 'R')[0].time_s > 0.4);
        PhysicsParams loop;
        loop.looping = true;
        const PhysicsAnalysis al = AnalyseMotion(rig, loop);
        CHECK(al.ok);
        for (const PartTrack& pt : al.foot[1].part)
            if (pt.present && !pt.contacts.empty()) CHECK(pt.contacts[0].start > 0);
        // A foot skimming the floor at speed from the clip start never comes to rest: no contact.
        const FootFn skim = [](double t) {
            FootPose f;
            f.ball = Vec3d{0.1, 0.02, 1.0 * t};
            const double u = t - 0.05;
            if (u > 0.0 && u < 0.5) f.ball.y += 9.81 * 0.25 * u - 0.5 * 9.81 * u * u;
            if (u >= 0.5) f.ball.z = 0.55;
            return f;
        };
        const PhysicsAnalysis as = AnalyseMotion(Rig(JumpFoot(-0.1, -0.1, 0.55), skim, o));
        CHECK(as.ok);
        for (const PartTrack& pt : as.foot[1].part)
            if (pt.present && !pt.contacts.empty()) CHECK(pt.contacts[0].start > 0);
        CHECK(Of(JumpEvents(as), PhysicsKind::Jump).empty());
    }
    // Story 10-9a-3: free fall. Both feet leave at 1.0 s on an arc (apex 0.25 s later), fall for
    // 0.3 s, then glide on at that height (1 m/s, never landing: a body lying). The flight's mean
    // fall over the whole run is too small for a flight Jump; the free fall gives the take-off
    // (Jump L, the feet tie), and no Land.
    {
        Options o;
        o.dur = 3.0;
        auto fall = [](double x) {
            return FootFn([x](double t) {
                FootPose     f;
                const double u = std::min(t - 1.0, 0.55);
                f.ball = Vec3d{x, 0.02, 0.0};
                if (u > 0.0) {
                    f.ball.y += 9.81 * 0.25 * u - 0.5 * 9.81 * u * u;
                    f.ball.z = 1.0 * (t - 1.0);
                }
                return f;
            });
        };
        const PhysicsAnalysis a = AnalyseMotion(Rig(fall(-0.1), fall(0.1), o));
        CHECK(a.ok);
        CHECK(Of(JumpEvents(a), PhysicsKind::Jump).empty() && Of(JumpEvents(a), PhysicsKind::Land).empty());
        const std::vector<PhysicsEvent> ff = JumpFreeFallEvents(a);
        CHECK(ff.size() == 1);
        if (ff.size() == 1) CHECK(ff[0].kind == PhysicsKind::Jump && ff[0].side == 'L' && std::fabs(ff[0].time_s - 1.0) < 0.03);
        // A head shorter than the smoothing reach still reads (right after it); a fall softer than
        // jump_fall_g gives none.
        PhysicsParams head;
        head.jump_freefall_window_s = 0.02;
        CHECK(Of(JumpFreeFallEvents(a, head), PhysicsKind::Jump).size() == 1);
        PhysicsParams soft;
        soft.jump_fall_g = 1.2;
        CHECK(JumpFreeFallEvents(a, soft).empty());
    }

    // Story 10-9a-2: push-off jumps, on an analysis set by hand (standing feet, then the hips'
    // acceleration and the feet's contacts written in): a push of `push_g` peaking at 0.9 s, both
    // feet off over [1.0 s, 1.0 s + off_s).
    {
        Options o;
        o.dur = 2.0;
        const PhysicsAnalysis stand = AnalyseMotion(Rig(Planted(-0.1, 0.0), Planted(0.1, 0.0), o));
        CHECK(stand.ok && stand.hips_vy.size() == stand.samples);
        auto make = [&](double push_g, double off_s) {
            PhysicsAnalysis a = stand;
            const int       n = static_cast<int>(a.samples);
            const double    rate = a.rate_hz;
            double          v = 0.0;  // m/s
            for (int i = 0; i < n; ++i) {
                const double t = i / rate, u = (t - 0.9) / 0.05;
                v += push_g * 9.81 * std::exp(-0.5 * u * u) / rate;
                a.hips_vy[static_cast<size_t>(i)] = v / a.leg_length;
            }
            const int fs = static_cast<int>(std::lround(1.0 * rate)), fe = fs + static_cast<int>(std::lround(off_s * rate));
            for (FootTrack& f : a.foot)
                for (PartTrack& pt : f.part) {
                    if (!pt.present) continue;
                    pt.contact.assign(a.samples, 1);
                    pt.contacts.clear();
                    if (fe <= fs) {
                        PartContact c;
                        c.end = n;
                        pt.contacts.push_back(c);
                        continue;
                    }
                    for (int i = fs; i < fe; ++i) pt.contact[static_cast<size_t>(i)] = 0;
                    PartContact c0, c1;
                    c0.end = fs;
                    c0.lift_s = fs / rate;
                    c1.start = fe;
                    c1.end = n;
                    pt.contacts = {c0, c1};
                }
            return a;
        };
        // A 1.5 g push, then both feet off 50 ms: one Jump at the take-off.
        const PhysicsAnalysis a = make(1.5, 0.05);
        const std::vector<double> acc = HipsUpAccelerationG(a);
        CHECK(acc.size() == a.samples);
        if (acc.size() == a.samples) {
            const size_t peak = static_cast<size_t>(std::max_element(acc.begin(), acc.end()) - acc.begin());
            CHECK(acc[peak] > 1.0 && acc[peak] < 1.5 && std::fabs(peak / a.rate_hz - 0.9) < 0.01);
        }
        const std::vector<PhysicsEvent> ev = JumpPushOffEvents(a);
        CHECK(ev.size() == 1);
        if (ev.size() == 1) {
            // Story 10-9a-3: both feet leave together: the first foot (L), its last part.
            CHECK(ev[0].kind == PhysicsKind::Jump && ev[0].side == 'L' && ev[0].part >= 0);
            CHECK(std::fabs(ev[0].time_s - 1.0) < 1e-9 && ev[0].strength > 0.0);
        }
        CHECK(JumpPushOffSummary(a, ev) == "jumps (push-off): 1");
        CHECK(JumpEvents(a).empty());  // 50 ms: no flight jump
        // The same push with a hand in contact: none.
        PhysicsAnalysis held = a;
        held.hand[0].part.present = true;
        held.hand[0].part.contact.assign(held.samples, 1);
        CHECK(JumpPushOffEvents(held).empty());
        // The hand in contact before the push only: still a jump.
        for (size_t i = 0; i < held.samples; ++i) held.hand[0].part.contact[i] = i / held.rate_hz < 0.6 ? 1 : 0;
        CHECK(JumpPushOffEvents(held).size() == 1);
        // The push with no feet off after it: none; a single free sample: none.
        CHECK(JumpPushOffEvents(make(1.5, 0.0)).empty());
        CHECK(JumpPushOffEvents(make(1.5, 1.0 / a.rate_hz)).empty());
        // A soft push (0.5 g): none at the default, one with a lower threshold.
        CHECK(JumpPushOffEvents(make(0.5, 0.05)).empty());
        PhysicsParams soft;
        soft.jump_impulse_g = 0.3;
        CHECK(JumpPushOffEvents(make(0.5, 0.05), soft).size() == 1);
        // The same push while running (the body at 3 leg/s, a stride): none; at 2 leg/s: a jump.
        PhysicsAnalysis run = a;
        run.hips_vh.assign(run.samples, 3.0);
        CHECK(JumpPushOffEvents(run).empty());
        run.hips_vh.assign(run.samples, 2.0);
        CHECK(JumpPushOffEvents(run).size() == 1);
        // The push too long before the take-off (window 0.05 s < 0.1 s): none.
        PhysicsParams tight;
        tight.jump_impulse_window_s = 0.05;
        CHECK(JumpPushOffEvents(a, tight).empty());
        // No hips: none, said in the report.
        PhysicsAnalysis nh = a;
        nh.hips_vy.clear();
        nh.hips_missing = "no bone for hips";
        CHECK(JumpPushOffEvents(nh).empty() && HipsUpAccelerationG(nh).empty());
        CHECK(JumpPushOffSummary(nh, {}) == "jumps (push-off): 0; missing: hips: no bone for hips");
    }

    // Story 10-9a-2: looping push-off. A 250 ms hop (no flight jump) taking off at 1.8 s of a
    // 2 s cycle, the hips dipping 3 cm just before it (a hard push): one Jump, in clip time.
    {
        Options o;
        o.dur = 2.0;
        PhysicsParams loop;
        loop.looping = true;
        std::vector<BoneTrack> rig = Rig(JumpFoot(-0.1, 1.8, 0.25, 2.0), JumpFoot(0.1, 1.8, 0.25, 2.0), o);
        for (size_t i = 0; i < rig[static_cast<size_t>(Role::Hips)].pos.size(); ++i) {
            const double u = (i / o.rate - 1.77) / 0.04;
            rig[static_cast<size_t>(Role::Hips)].pos[i].y -= 0.03 * std::exp(-0.5 * u * u);
        }
        const PhysicsAnalysis a = AnalyseMotion(rig, loop);
        CHECK(a.ok && a.looping);
        const std::vector<PhysicsEvent> ev = JumpPushOffEvents(a, loop);
        CHECK(ev.size() == 1);
        if (ev.size() == 1) CHECK(ev[0].time_s >= 0.0 && ev[0].time_s < 2.0 && std::fabs(ev[0].time_s - 1.8) < 0.03);
        CHECK(JumpEvents(a, loop).empty());
    }

    {
        // Story 10-8p: a foot that rolls off its toes. The heel rises about the ball (0.85-0.95
        // s), then the ball rises about the planted tip (1.0-1.08 s), then the foot swings. The
        // lift-off is the ball's (the last of the heel and ball to leave) plus lift_offset_s;
        // lift_from_tip (the old rule) gives the tip's, 50 ms or more later.
        const FootFn roll = [](double t) {
            const double kHeel = 0.4;
            FootPose     f;
            f.ball = Vec3d{-0.1, 0.02, 0.0};
            f.pitch = kHeel * Ease((t - 0.85) / 0.1);
            const Vec3d  tip0{-0.1, 0.0, 0.07};
            const double th = -0.6 * Ease((t - 1.0) / 0.08);
            f.toe_pitch = th;
            const double c = std::cos(th), s = std::sin(th);
            f.ball = Vec3d{tip0.x, tip0.y - (-0.02 * c + 0.07 * s), tip0.z - (0.02 * s + 0.07 * c)};
            if (t > 1.08) {
                f.ball.y += 0.5 * (t - 1.08);
                f.ball.z += 1.0 * (t - 1.08);
            }
            return f;
        };
        Options o;
        o.dur = 1.6;
        const PhysicsAnalysis a = AnalyseMotion(Rig(roll, Planted(0.1, 0.0), o));
        CHECK(a.ok);
        const PhysicsParams             dp;
        const std::vector<PhysicsEvent> lift = Of(FootEvents(a, dp), PhysicsKind::LiftOff, 'L');
        PhysicsParams                   old = dp;
        old.lift_from_tip = true;
        old.lift_offset_s = 0.0;
        const std::vector<PhysicsEvent> tip = Of(FootEvents(a, old), PhysicsKind::LiftOff, 'L');
        const PartTrack&                ball = a.foot[0].part[static_cast<int>(FootPart::Ball)];
        CHECK(lift.size() == 1 && tip.size() == 1 && ball.contacts.size() == 1);
        if (lift.size() == 1 && tip.size() == 1 && ball.contacts.size() == 1) {
            CHECK(lift[0].part == static_cast<int>(FootPart::Ball));
            CHECK(std::fabs(lift[0].time_s - (ball.contacts[0].lift_s + dp.lift_offset_s)) < 1e-9);
            CHECK(lift[0].time_s > 0.98 && lift[0].time_s < 1.05);
            CHECK(tip[0].part == static_cast<int>(FootPart::Tip));
            CHECK(tip[0].time_s > lift[0].time_s + 0.05);
        }
        // A rig without toe bones: the old rule, plus the offset.
        Options nb = o;
        nb.toes = false;
        const PhysicsAnalysis           h = AnalyseMotion(Rig(roll, Planted(0.1, 0.0), nb));
        const std::vector<PhysicsEvent> hl = Of(FootEvents(h, dp), PhysicsKind::LiftOff, 'L');
        const std::vector<PhysicsEvent> ho = Of(FootEvents(h, old), PhysicsKind::LiftOff, 'L');
        CHECK(hl.size() == 1 && ho.size() == 1);
        if (hl.size() == 1 && ho.size() == 1)
            CHECK(hl[0].part == ho[0].part && std::fabs(hl[0].time_s - ho[0].time_s - dp.lift_offset_s) < 1e-9);
    }

    if (g_fails == 0) std::printf("motion_physics: all tests passed\n");
    return g_fails == 0 ? 0 : 1;
}
