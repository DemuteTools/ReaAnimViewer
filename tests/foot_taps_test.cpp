// SPDX-License-Identifier: MIT
//
// Host test of story 10-9e: foot taps and drags (src/foot_taps.h) on synthetic feet, and their
// Tap / Drag rows through Detect (src/auto_detect.h). No REAPER, no Windows: any C++17 compiler.
//   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests

#include "auto_detect.h"
#include "auto_types.h"
#include "bone_roles.h"
#include "foot_taps.h"
#include "motion_physics.h"

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

// One foot at one instant: the ball (toe base), and the heel raised about the ball by `pitch`
// (radians). Flat, the heel is 14 cm behind the ball and 6 cm above it; the tip 7 cm ahead and
// 2 cm below the ball (the toes stay level).
struct Foot {
    Vec3d  ball;
    double pitch = 0.0;
};
using FootFn = std::function<Foot(double)>;

double Ease(double u)
{
    u = std::clamp(u, 0.0, 1.0);
    return 0.5 - 0.5 * std::cos(kPi * u);
}

// From a to b between t0 and t1 (eased), a before, b after.
double Ramp(double t, double t0, double t1, double a, double b)
{
    return a + (b - a) * Ease((t - t0) / (t1 - t0));
}

// One track per Role: both feet, the knee 42 cm and the up leg 87 cm above the heel (a 0.87 m
// leg), the hips between the up legs. 240 Hz.
std::vector<BoneTrack> Rig(const FootFn& left, const FootFn& right, double dur)
{
    std::vector<BoneTrack> t(static_cast<size_t>(Role::Count));
    const double           rate = 240.0;
    const int              n = static_cast<int>(std::floor(dur * rate + 1e-9)) + 1;
    auto put = [&](Role r, const Vec3d& p) {
        BoneTrack& b = t[static_cast<size_t>(r)];
        b.rate_hz = rate;
        b.pos.push_back(p);
    };
    for (int i = 0; i < n; ++i) {
        const double time = i / rate;
        Vec3d        up[2];
        for (int s = 0; s < 2; ++s) {
            const Foot   f = s ? right(time) : left(time);
            const Vec3d  heel{f.ball.x, f.ball.y + 0.06 * std::cos(f.pitch) + 0.14 * std::sin(f.pitch),
                             f.ball.z + 0.06 * std::sin(f.pitch) - 0.14 * std::cos(f.pitch)};
            const Vec3d  tip{f.ball.x, f.ball.y - 0.02, f.ball.z + 0.07};
            const char   side = s ? 'R' : 'L';
            put(FootPartRole(side, 0), heel);
            put(FootPartRole(side, 1), f.ball);
            put(FootPartRole(side, 2), tip);
            put(s ? Role::RightKnee : Role::LeftKnee, Vec3d{heel.x, heel.y + 0.42, heel.z});
            up[s] = Vec3d{heel.x, heel.y + 0.87, heel.z};
            put(s ? Role::RightUpLeg : Role::LeftUpLeg, up[s]);
        }
        put(Role::Hips, Vec3d{0.5 * (up[0].x + up[1].x), 0.5 * (up[0].y + up[1].y) + 0.05, 0.5 * (up[0].z + up[1].z)});
    }
    return t;
}

// A foot planted flat all along.
FootFn Planted(double x)
{
    return [x](double) {
        Foot f;
        f.ball = Vec3d{x, 0.02, 0.0};
        return f;
    };
}

// The right foot: flat until 0.5 s, then up (heel high, ball 8 cm up) by 0.8 s; the ball comes
// down to the floor at `touch` (over 100 ms), holds `hold` s, goes back up over 100 ms; then the
// foot lands flat at 2.4 s (over 300 ms) and stays.
FootFn Tapping(double touch, double hold)
{
    return [touch, hold](double t) {
        Foot f;
        f.ball.x = 0.2;
        double y = 0.02, pitch = 0.0;
        if (t >= 0.5) {
            y = Ramp(t, 0.5, 0.8, 0.02, 0.10);
            pitch = Ramp(t, 0.5, 0.8, 0.0, 0.6);
        }
        if (t >= touch - 0.1) y = Ramp(t, touch - 0.1, touch, 0.10, 0.02);
        if (t >= touch + hold) y = Ramp(t, touch + hold, touch + hold + 0.1, 0.02, 0.10);
        if (t >= 2.4) {
            y = Ramp(t, 2.4, 2.7, 0.10, 0.02);
            pitch = Ramp(t, 2.4, 2.7, 0.6, 0.0);
        }
        f.ball.y = y;
        f.pitch = pitch;
        // Up in the air the foot keeps moving (a still foot reads as resting).
        if (y > 0.021) f.ball.z = 0.03 * std::sin(2.0 * kPi * 1.5 * (t - 0.5)) * (y - 0.02) / 0.08;
        return f;
    };
}

// The right foot: flat until 0.5 s, the heel raised on the planted ball by 0.7 s; from 0.8 s the
// ball slides back 0.6 m along the floor (`lift` m above it) over 0.6 s; then the foot lifts
// (by 1.6 s), and lands flat at 2.2 s (over 300 ms) and stays.
FootFn Dragging(double lift)
{
    return [lift](double t) {
        Foot f;
        f.ball.x = 0.2;
        f.ball.y = 0.02;
        if (t >= 0.5) f.pitch = Ramp(t, 0.5, 0.7, 0.0, 0.6);
        if (t >= 0.7) f.ball.y = Ramp(t, 0.7, 0.8, 0.02, 0.02 + lift);
        if (t >= 0.8) f.ball.z = Ramp(t, 0.8, 1.4, 0.0, -0.6);
        if (t >= 1.4) f.ball.y = Ramp(t, 1.4, 1.6, 0.02 + lift, 0.10);
        if (t >= 2.2) {
            f.ball.y = Ramp(t, 2.2, 2.5, 0.10, 0.02);
            f.pitch = Ramp(t, 2.2, 2.5, 0.6, 0.0);
            f.ball.z = Ramp(t, 2.2, 2.5, -0.6, -0.3);
        }
        return f;
    };
}

// A walk: each foot planted 0.6 s from each contact start, then a 0.4 s swing 0.6 m forward with
// a 10 cm lift; the feet half a cycle apart.
FootFn Walk(double first, double x)
{
    return [first, x](double t) {
        const double rel = t - first;
        const int    k = static_cast<int>(std::floor(rel));
        const double in = rel - k;
        Foot         f;
        f.ball.x = x;
        if (in < 0.6) {
            f.ball.z = 0.6 * k;
            f.ball.y = 0.02;
        } else {
            const double u = (in - 0.6) / 0.4;
            f.ball.z = 0.6 * (k + Ease(u));
            f.ball.y = 0.02 + 0.10 * std::sin(kPi * u);
        }
        return f;
    };
}

// A sprint: each foot on its ball only (the heel up) for `stance` s of each 0.8 s cycle, then a
// swing 1.6 m forward with a 15 cm lift; the feet half a cycle apart, so both are in the air
// between the stances (a flight): the toe-only stances of Treadmill Running / Two Cycle Sprint.
FootFn Sprint(double first, double x, double stance)
{
    return [first, x, stance](double t) {
        const double rel = t - first;
        const int    k = static_cast<int>(std::floor(rel / 0.8));
        const double in = rel - 0.8 * k;
        Foot         f;
        f.ball.x = x;
        f.pitch = 0.6;
        if (in < stance) {
            f.ball.z = 1.6 * k;
            f.ball.y = 0.02;
        } else {
            const double u = (in - stance) / (0.8 - stance);
            f.ball.z = 1.6 * (k + Ease(u));
            f.ball.y = 0.02 + 0.15 * std::sin(kPi * u);
        }
        return f;
    };
}

std::vector<PhysicsEvent> Side(const std::vector<PhysicsEvent>& ev, char side)
{
    std::vector<PhysicsEvent> out;
    for (const PhysicsEvent& e : ev)
        if (e.side == side) out.push_back(e);
    return out;
}

void TestTap()
{
    // A 100 ms ball touch, the left foot planted: one tap, at the foot step of that contact.
    const PhysicsAnalysis a = AnalyseMotion(Rig(Planted(-0.2), Tapping(1.2, 0.1), 3.0));
    CHECK(a.ok);
    const std::vector<PhysicsEvent> taps = FootTapEvents(a);
    CHECK(taps.size() == 1);
    if (taps.size() == 1) {
        CHECK(taps[0].kind == PhysicsKind::Tap && taps[0].side == 'R');
        CHECK(taps[0].part != static_cast<int>(FootPart::Heel));
        CHECK(std::fabs(taps[0].time_s - 1.2) < 0.06);
        CHECK(taps[0].strength > 0.0);
        bool on_step = false;
        for (const PhysicsEvent& e : FootEvents(a))
            if (e.kind == PhysicsKind::FootStep && e.side == 'R' && std::fabs(e.time_s - taps[0].time_s) < 1e-9) on_step = true;
        CHECK(on_step);
        CHECK(PhysicsMarkerName(taps[0]).rfind("PHY R tap ", 0) == 0);
    }
    // Held 400 ms: a step, no tap; at 100 % sensitivity (300 ms) still none.
    const PhysicsAnalysis held = AnalyseMotion(Rig(Planted(-0.2), Tapping(1.2, 0.4), 3.0));
    CHECK(FootTapEvents(held).empty());
    CHECK(FootTapEvents(held, AutoPhysicsParams(AutoType::Tap, 100.0, false)).empty());
    // The other foot in the air during the touch: no tap (it carries no body).
    FootFn lifted = [](double t) {
        Foot f;
        f.ball = Vec3d{-0.2, Ramp(t, 0.9, 1.0, 0.02, 0.12), 0.0};
        if (t >= 1.5) f.ball.y = Ramp(t, 1.5, 1.6, 0.12, 0.02);
        return f;
    };
    CHECK(FootTapEvents(AnalyseMotion(Rig(lifted, Tapping(1.2, 0.1), 3.0))).empty());
    // A sprint's toe-only stances (130 ms, the heel up, both feet in the air between them): single
    // supports, no tap, even at 100 % (300 ms).
    const PhysicsAnalysis sprint = AnalyseMotion(Rig(Sprint(0.0, -0.1, 0.13), Sprint(0.4, 0.1, 0.13), 4.0));
    CHECK(sprint.ok);
    {
        int toe_steps = 0;
        for (const PhysicsEvent& e : FootEvents(sprint)) toe_steps += e.kind == PhysicsKind::FootStep;
        CHECK(toe_steps >= 6);  // the stances are contacts (the test sees them)
    }
    CHECK(FootTapEvents(sprint).empty());
    CHECK(FootTapEvents(sprint, AutoPhysicsParams(AutoType::Tap, 100.0, false)).empty());
    // The same touch while the other foot stands, the other foot's toe also on its ball (both on
    // tiptoe): still a tap (the other foot carries the body, on the same floor).
    FootFn tiptoe = [](double) {
        Foot f;
        f.ball = Vec3d{-0.2, 0.02, 0.0};
        f.pitch = 0.3;
        return f;
    };
    CHECK(FootTapEvents(AnalyseMotion(Rig(tiptoe, Tapping(1.2, 0.1), 3.0))).size() == 1);
    // The other foot on a step 20 cm higher: not the same floor, no tap.
    FootFn stair = [](double) {
        Foot f;
        f.ball = Vec3d{-0.2, 0.22, 0.0};
        return f;
    };
    CHECK(FootTapEvents(AnalyseMotion(Rig(stair, Tapping(1.2, 0.1), 3.0))).empty());
    // A walk: no tap, no drag.
    const PhysicsAnalysis walk = AnalyseMotion(Rig(Walk(0.4, -0.1), Walk(-0.1, 0.1), 4.0));
    CHECK(walk.ok);
    CHECK(FootTapEvents(walk).empty());
    CHECK(FootDragEvents(walk).empty());
}

void TestDrag()
{
    // The ball slid 0.6 m along the floor, the heel up: one drag of the right foot, with its span.
    const PhysicsAnalysis a = AnalyseMotion(Rig(Planted(-0.2), Dragging(0.0), 3.0));
    CHECK(a.ok);
    const std::vector<PhysicsEvent> drags = FootDragEvents(a);
    CHECK(drags.size() == 1);
    if (drags.size() == 1) {
        const PhysicsEvent& d = drags[0];
        CHECK(d.kind == PhysicsKind::Drag && d.side == 'R' && d.part != static_cast<int>(FootPart::Heel));
        CHECK(d.start_s > 0.75 && d.start_s < 1.0);
        CHECK(d.end_s > 1.2 && d.end_s < 1.45);
        CHECK(d.strength > 1.0 && d.strength < 2.0);  // peak 1.57 m/s = 1.8 leg/s
        CHECK(PhysicsMarkerName(d).rfind("PHY R drag ", 0) == 0);
    }
    CHECK(Side(FootDragEvents(a), 'L').empty());
    CHECK(FootTapEvents(a).empty());
    // The same slide 4 cm above the floor: a swing, no drag.
    CHECK(FootDragEvents(AnalyseMotion(Rig(Planted(-0.2), Dragging(0.04), 3.0))).empty());
    // A shorter minimum (the sensitivity divides it) keeps the drag; a far longer one drops it.
    PhysicsParams p;
    p.drag_min_s = 1.0;
    CHECK(FootDragEvents(a, p).empty());
    // The mask: the right foot drags somewhere, the left never.
    const std::vector<char> mr = FootDragMask(a, PhysicsParams{}, 'R');
    const std::vector<char> ml = FootDragMask(a, PhysicsParams{}, 'L');
    CHECK(mr.size() == a.samples && ml.size() == a.samples);
    CHECK(std::count(mr.begin(), mr.end(), 1) > 0 && std::count(ml.begin(), ml.end(), 1) == 0);
}

void TestRows()
{
    CHECK(std::string(AutoTypeLabel(AutoType::Tap)) == "Tap" && std::string(AutoTypeLabel(AutoType::Drag)) == "Drag");
    CHECK(AutoCategoryOf(AutoType::Tap) == AutoCategory::Legs && AutoCategoryOf(AutoType::Drag) == AutoCategory::Legs);
    CHECK(std::string(AutoKindWord(AutoKind::Tap)) == "tap" && std::string(AutoKindWord(AutoKind::Drag)) == "drag");
    CHECK(AutoMarkerName(AutoKind::Tap, 'L') == "Tap L" && AutoMarkerName(AutoKind::Drag, 'R') == "Drag R");
    CHECK(std::string(AutoTypeRow(AutoType::Tap).sound_word) == "tap" && std::string(AutoTypeRow(AutoType::Drag).sound_word) == "drag");
    CHECK(std::string(AutoKindSides(AutoKind::Tap)) == "LR" && std::string(AutoKindSides(AutoKind::Drag)) == "LR");
    AutoKind k;
    CHECK(AutoKindFromWord("tap", &k) && k == AutoKind::Tap);
    CHECK(AutoKindFromWord("drag", &k) && k == AutoKind::Drag);
    // Sensitivity: Tap multiplies the longest tap, Drag divides its minimum length (not its speed).
    const PhysicsParams hi = AutoPhysicsParams(AutoType::Tap, 100.0, false);
    CHECK(std::fabs(hi.tap_max_s - 0.300) < 1e-9);
    const PhysicsParams dh = AutoPhysicsParams(AutoType::Drag, 100.0, false);
    CHECK(std::fabs(dh.drag_min_s - 0.050) < 1e-9 && std::fabs(dh.drag_speed - 0.5) < 1e-9);
    // Required parts: the heel and the toe of each foot.
    std::vector<int> bones(static_cast<size_t>(Role::Count), 0);
    CHECK(AutoMissingParts(AutoType::Tap, false, bones).empty());
    bones[static_cast<size_t>(Role::RightHeel)] = -1;
    const std::vector<std::string> miss = AutoMissingParts(AutoType::Drag, false, bones);
    CHECK(std::find(miss.begin(), miss.end(), std::string(RoleName(Role::RightHeel))) != miss.end());
}

// Through Detect: Step and Tap ticked, the Tap replaces the right foot's step at the touch; its
// other steps stay.
void TestDetect()
{
    const std::vector<BoneTrack> rt = Rig(Planted(-0.2), Tapping(1.2, 0.1), 3.0);
    AutoSettings                 s;
    s.type[static_cast<int>(AutoType::Step)].on = true;
    std::vector<Block> steps_only = AutoBlocks(s);
    s.type[static_cast<int>(AutoType::Tap)].on = true;
    s.type[static_cast<int>(AutoType::Drag)].on = true;
    std::vector<Block> blocks = AutoBlocks(s);
    const std::vector<Event> ev = DetectAutoEvents(blocks, rt, nullptr, false);
    const std::vector<Event> ev_steps = DetectAutoEvents(steps_only, rt, nullptr, false);
    auto count = [](const std::vector<Event>& v, const std::string& m) {
        int n = 0;
        for (const Event& e : v) n += e.marker == m;
        return n;
    };
    CHECK(count(ev, "Tap R") == 1);
    CHECK(count(ev, "Tap L") == 0 && count(ev, "Drag L") == 0 && count(ev, "Drag R") == 0);
    CHECK(count(ev_steps, "FS R") == 2);  // the touch, and the foot held still up in the air before it
    CHECK(count(ev, "FS R") == 1);        // the touch's is the Tap's
    double tap = -1.0;
    for (const Event& e : ev)
        if (e.marker == "Tap R") tap = e.time_s;
    for (const Event& e : ev)
        if (e.marker == "FS R") CHECK(std::fabs(e.time_s - tap) > kAutoReplaceS);
    // The left foot's steps are never the Tap's (none here: it is planted from the start).
    CHECK(count(ev, "FS L") == count(ev_steps, "FS L"));
    // With Lift-off ticked too, the Tap also replaces the lift that ends its touch (a tap is one
    // sound); the right foot's other lifts stay.
    AutoSettings l;
    l.type[static_cast<int>(AutoType::Step)].on = true;
    l.type[static_cast<int>(AutoType::LiftOff)].on = true;
    const std::vector<Event> ev_lifts = DetectAutoEvents(AutoBlocks(l), rt, nullptr, false);
    l.type[static_cast<int>(AutoType::Tap)].on = true;
    const std::vector<Event> ev_tap = DetectAutoEvents(AutoBlocks(l), rt, nullptr, false);
    CHECK(count(ev_tap, "Tap R") == 1);
    CHECK(count(ev_lifts, "Lift R") >= 1);
    CHECK(count(ev_tap, "Lift R") == count(ev_lifts, "Lift R") - 1);
    for (const Event& e : ev_tap)
        if (e.marker == "Lift R") CHECK(e.time_s < tap - kAutoReplaceS || e.time_s > tap + 0.2);
    CHECK(count(ev_tap, "Lift L") == count(ev_lifts, "Lift L"));
}

// Detect's sensitivity search never raises a silent Tap or Drag block (a walk's toe skims drag
// from 67 %): searched, not raised, its value unchanged.
void TestNoRaise()
{
    const std::vector<BoneTrack> rt = Rig(Walk(0.4, -0.1), Walk(-0.1, 0.1), 4.0);
    AutoSettings                 s;
    s.type[static_cast<int>(AutoType::Tap)].on = true;
    s.type[static_cast<int>(AutoType::Drag)].on = true;
    std::vector<Block>     blocks = AutoBlocks(s);
    const std::vector<int> counts(blocks.size(), 0);
    const std::vector<int> bones(static_cast<size_t>(Role::Count), 0);
    CHECK(AutoTypeRow(AutoType::Tap).no_sens_raise && AutoTypeRow(AutoType::Drag).no_sens_raise);
    CHECK(!AutoTypeRow(AutoType::Step).no_sens_raise && !AutoTypeRow(AutoType::Land).no_sens_raise);
    const std::vector<AutoSensResult> r = SearchAutoSensitivity(blocks, rt, counts, bones, false);
    CHECK(r.size() == blocks.size() && !blocks.empty());
    for (size_t i = 0; i < r.size() && i < blocks.size(); ++i) {
        CHECK(r[i].searched && !r[i].raised && !r[i].none);
        CHECK(r[i].sens == blocks[i].sens);
    }
}

}  // namespace

int main()
{
    TestTap();
    TestDrag();
    TestRows();
    TestDetect();
    TestNoRaise();
    if (g_fails) {
        std::printf("%d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("foot_taps_test: all checks passed\n");
    return 0;
}
