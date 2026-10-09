// SPDX-License-Identifier: MIT
//
// Host test of the motion physics (src/motion_physics.h, Epic 10, spike 10-8a; hands: story
// 10-8c): the specs' matrix rows on synthetic feet and hands. No REAPER, no Windows: any C++17
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
        const Vec3d  back{0.0, 0.0, -o.ground * time};
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
// function: no bone for that hand). Same length and rate as Rig's.
using HandFn = std::function<Vec3d(double)>;

void AddHands(std::vector<BoneTrack>& t, const HandFn& left, const HandFn& right, const Options& o)
{
    const int n = static_cast<int>(std::floor(o.dur * o.rate + 1e-9)) + 1;
    for (int s = 0; s < 2; ++s) {
        const HandFn& fn = s ? right : left;
        if (!fn) continue;
        BoneTrack& b = t[static_cast<size_t>(s ? Role::RightHand : Role::LeftHand)];
        b.rate_hz = o.rate;
        for (int i = 0; i < n; ++i) b.pos.push_back(fn(i / o.rate));
    }
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
        // 0.6 m/s). The ground frame reads it, and the steps are the moving walk's.
        Options o;
        o.ground = 0.6;
        const PhysicsAnalysis a = AnalyseMotion(Rig(walk_l, walk_r, o));
        CHECK(a.ok && std::fabs(a.ground_velocity.z + 0.6) < 1e-3 && std::fabs(a.ground_velocity.x) < 1e-3);
        const std::vector<PhysicsEvent> ev = FootEvents(a, SpeedTiming());
        const std::vector<PhysicsEvent> moving = FootEvents(AnalyseMotion(Rig(walk_l, walk_r, Options{})), SpeedTiming());
        CHECK(ev.size() == moving.size());
        for (PhysicsKind kind : {PhysicsKind::Step, PhysicsKind::FootStep, PhysicsKind::LiftOff})
            for (char side : {'L', 'R'})
                for (int q = -2; q < kFootPartCount; ++q) {  // -2: every part
                    const std::vector<PhysicsEvent> x = Of(ev, kind, side, q), y = Of(moving, kind, side, q);
                    CHECK(x.size() == y.size());
                    for (size_t i = 0; i < x.size() && i < y.size(); ++i) CHECK(std::fabs(x[i].time_s - y[i].time_s) < 0.002);
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
        // A heel whip at toe-off is no pivot: the foot rolls onto its toes (pitched 80 deg by
        // 1.25 s) and turns 40 deg on the ball as it rolls, then the ball leaves 80 ms after
        // the turn (from 1.28 s, up and forward at 1 m/s).
        Options o;
        o.dur = 2.0;
        auto roll = [](double leave) {
            return [leave](double t) {
                FootPose     f;
                const double up = std::max(0.0, t - leave);
                f.ball = Vec3d{-0.1, 0.02 + up, up};
                f.pitch = 80.0 * kDeg * std::clamp((t - 1.0) / 0.25, 0.0, 1.0);
                f.yaw = 40.0 * kDeg * Ease((t - 1.0) / 0.2);
                return f;
            };
        };
        for (bool rot : {false, true}) {
            Options v = o;
            v.tip = !rot;
            v.rot = rot;
            const PhysicsAnalysis a = AnalyseMotion(Rig(roll(1.28), Planted(0.1, 0.2), v));
            CHECK(a.ok && Of(FootEvents(a), PhysicsKind::Pivot).empty());
            // Swivel: the same roll with the ball staying (on its toes to the clip end) is a
            // pivot on the ball, about 40 deg.
            const std::vector<PhysicsEvent> sw = Of(FootEvents(AnalyseMotion(Rig(roll(10.0), Planted(0.1, 0.2), v))), PhysicsKind::Pivot);
            CHECK(sw.size() == 1);
            if (sw.size() == 1) CHECK(sw[0].part == static_cast<int>(FootPart::Ball) && sw[0].strength > 33.0 && sw[0].strength < 45.0);
        }
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
    constexpr double kLeg = 0.87;  // the rig's leg (m): hand speeds below in leg/s x kLeg
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
        CHECK(HandSummary(a, ev) == "hands: grabs 1, releases 1");
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
        CHECK(HandSummary(a, ev) == "hands: grabs 0, releases 1; missing: R hand: no bone for right hand");
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
                                           "hands: grabs 0, releases 0; missing: L hand: no bone for left hand, R hand: "
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

    if (g_fails == 0) std::printf("motion_physics: all tests passed\n");
    return g_fails == 0 ? 0 : 1;
}
