// SPDX-License-Identifier: MIT
//
// Host test of story 10-9i: heel taps (src/heel_taps.h) on synthetic feet, and their Heel Tap row
// through Detect (src/auto_detect.h). No REAPER, no Windows: any C++17 compiler.
//   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests

#include "auto_detect.h"
#include "auto_types.h"
#include "bone_roles.h"
#include "heel_taps.h"
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
// 2 cm below the ball (as tests/foot_taps_test.cpp).
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
            const Foot  f = s ? right(time) : left(time);
            const Vec3d heel{f.ball.x, f.ball.y + 0.06 * std::cos(f.pitch) + 0.14 * std::sin(f.pitch),
                             f.ball.z + 0.06 * std::sin(f.pitch) - 0.14 * std::cos(f.pitch)};
            const Vec3d tip{f.ball.x, f.ball.y - 0.02, f.ball.z + 0.07};
            const char  side = s ? 'R' : 'L';
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

// A foot planted flat all along, its ball `y` m up (0.02: on the floor).
FootFn Planted(double x, double y = 0.02)
{
    return [x, y](double) {
        Foot f;
        f.ball = Vec3d{x, y, 0.0};
        return f;
    };
}

// The right foot, its ball planted `y` m up: the heel rises on the ball (to `pitch` rad) from 1.0
// s over `rise` s, holds until 1.6 s, and comes back down in `fall` s.
FootFn HeelDrop(double pitch, double fall, double y = 0.02, double rise = 0.4)
{
    return [pitch, fall, y, rise](double t) {
        Foot f;
        f.ball = Vec3d{0.2, y, 0.0};
        if (t >= 1.0) f.pitch = Ramp(t, 1.0, 1.0 + rise, 0.0, pitch);
        if (t >= 1.6) f.pitch = Ramp(t, 1.6, 1.6 + fall, pitch, 0.0);
        return f;
    };
}

// The right foot lifted flat (heel and ball together) 3 cm from 1.0 to 1.4 s, set down from 1.6 s
// over 0.25 s.
FootFn FlatLift()
{
    return [](double t) {
        Foot f;
        f.ball = Vec3d{0.2, 0.02, 0.0};
        if (t >= 1.0) f.ball.y = Ramp(t, 1.0, 1.4, 0.02, 0.05);
        if (t >= 1.6) f.ball.y = Ramp(t, 1.6, 1.85, 0.05, 0.02);
        return f;
    };
}

int Count(const std::vector<Event>& v, const std::string& m)
{
    int n = 0;
    for (const Event& e : v) n += e.marker == m;
    return n;
}

void TestHeelTap()
{
    // A 3.3 cm heel rise (0.038 leg) coming down over 250 ms: one heel tap, on the right heel,
    // where the heel reaches the floor.
    const PhysicsAnalysis a = AnalyseMotion(Rig(Planted(-0.2), HeelDrop(0.25, 0.25), 3.0));
    CHECK(a.ok);
    const std::vector<PhysicsEvent> ev = HeelTapEvents(a);
    CHECK(ev.size() == 1);
    if (ev.size() == 1) {
        CHECK(ev[0].kind == PhysicsKind::Tap && ev[0].side == 'R' && ev[0].part == static_cast<int>(FootPart::Heel));
        CHECK(ev[0].time_s > 1.75 && ev[0].time_s < 1.86);
        CHECK(ev[0].strength > 0.1 && ev[0].strength < 0.4);  // the fastest descent (leg/s)
        CHECK(PhysicsMarkerName(ev[0]).rfind("PHY R tap heel", 0) == 0);
    }
    // The rise curve: the heel's height over its floor, the ball down throughout.
    std::vector<char>         down;
    const std::vector<double> r = HeelTapRise(a, PhysicsParams{}, 'R', &down);
    CHECK(r.size() == a.samples && down.size() == a.samples);
    if (r.size() == a.samples) {
        CHECK(*std::max_element(r.begin(), r.end()) > 0.03);
        CHECK(std::count(down.begin(), down.end(), 1) > static_cast<long>(a.samples / 2));
    }
    // A 0.9 cm rise (0.010 leg) is a weight shift: none at the default, one at 100 %.
    const PhysicsAnalysis small = AnalyseMotion(Rig(Planted(-0.2), HeelDrop(0.065, 0.2), 3.0));
    CHECK(HeelTapEvents(small).empty());
    CHECK(HeelTapEvents(small, AutoPhysicsParams(AutoType::HeelTap, 100.0, false)).size() == 1);
    // A foot lifted flat and set down is no heel tap (the heel never rises over the ball).
    CHECK(HeelTapEvents(AnalyseMotion(Rig(Planted(-0.2), FlatLift(), 3.0))).empty());
    // A foot on a step 30 cm over the floor the other one stands on (a climbing hold): none.
    CHECK(HeelTapEvents(AnalyseMotion(Rig(Planted(-0.2), HeelDrop(0.25, 0.25, 0.32), 3.0))).empty());
    // Slammed down in 60 ms, the heel stays a contact (too short to break it): a stronger tap.
    const std::vector<PhysicsEvent> fast = HeelTapEvents(AnalyseMotion(Rig(Planted(-0.2), HeelDrop(0.25, 0.06), 3.0)));
    CHECK(fast.size() == 1 && !ev.empty() && fast[0].strength > ev[0].strength);
    // Kicked up 8 cm in 100 ms and slammed down, the heel leaves the ground (its contact breaks):
    // a heel step, no tap.
    const PhysicsAnalysis slam = AnalyseMotion(Rig(Planted(-0.2), HeelDrop(0.6, 0.06, 0.02, 0.1), 3.0));
    CHECK(HeelTapEvents(slam).empty());
    // Cut by the clip end (the heel still up): none.
    CHECK(HeelTapEvents(AnalyseMotion(Rig(Planted(-0.2), HeelDrop(0.25, 0.25), 1.7))).empty());
    // No heel bone: none, and an empty rise curve.
    std::vector<BoneTrack> no_heel = Rig(Planted(-0.2), HeelDrop(0.25, 0.25), 3.0);
    no_heel[static_cast<size_t>(Role::RightHeel)].pos.clear();
    const PhysicsAnalysis nh = AnalyseMotion(no_heel);
    for (const PhysicsEvent& e : HeelTapEvents(nh)) CHECK(e.side != 'R');
    CHECK(HeelTapRise(nh, PhysicsParams{}, 'R').empty());
}

void TestRows()
{
    CHECK(std::string(AutoTypeLabel(AutoType::HeelTap)) == "Heel Tap");
    CHECK(AutoCategoryOf(AutoType::HeelTap) == AutoCategory::Legs);
    CHECK(std::string(AutoKindWord(AutoKind::HeelTap)) == "heel_tap");
    CHECK(AutoMarkerName(AutoKind::HeelTap, 'L') == "Heel Tap L" && AutoMarkerName(AutoKind::HeelTap, 'R') == "Heel Tap R");
    CHECK(std::string(AutoTypeRow(AutoType::HeelTap).sound_word) == "heel_tap");
    CHECK(std::string(AutoKindSides(AutoKind::HeelTap)) == "LR");
    AutoKind k;
    CHECK(AutoKindFromWord("heel_tap", &k) && k == AutoKind::HeelTap);
    const AutoTypeInfo& row = AutoTypeRow(AutoType::HeelTap);
    CHECK(row.no_sens_raise && row.replaces == static_cast<int>(AutoType::Step) && row.replaces_own_side);
    CHECK(row.replaces_in_span == -1 && row.conf_scale == 0.5);
    // Sensitivity: divides the heel rise.
    const PhysicsParams hi = AutoPhysicsParams(AutoType::HeelTap, 100.0, false);
    CHECK(std::fabs(hi.heel_tap_min_rise - 0.0065) < 1e-9 && std::fabs(hi.heel_tap_min_speed - 0.06) < 1e-9);
    // Strength levels: Soft below 0.26 leg/s, Hard from 0.31.
    CHECK(AutoStrengthLevel(AutoType::HeelTap, 0.12) == kStrengthSoft);
    CHECK(AutoStrengthLevel(AutoType::HeelTap, 0.28) == kStrengthMedium);
    CHECK(AutoStrengthLevel(AutoType::HeelTap, 0.33) == kStrengthHard);
    // Required parts: the heel and the toe of each foot.
    std::vector<int> bones(static_cast<size_t>(Role::Count), 0);
    CHECK(AutoMissingParts(AutoType::HeelTap, false, bones).empty());
    bones[static_cast<size_t>(Role::RightHeel)] = -1;
    const std::vector<std::string> miss = AutoMissingParts(AutoType::HeelTap, false, bones);
    CHECK(std::find(miss.begin(), miss.end(), std::string(RoleName(Role::RightHeel))) != miss.end());
}

// Through Detect: Heel Tap R once, with its lane; with Step ticked too nothing changes for the
// steps (no step at a slow heel drop); the slam is a step, kept, and no heel tap.
void TestDetect()
{
    const std::vector<BoneTrack> rt = Rig(Planted(-0.2), HeelDrop(0.25, 0.25), 3.0);
    AutoSettings                 s;
    s.type[static_cast<int>(AutoType::HeelTap)].on = true;
    const std::vector<Event> ev = DetectAutoEvents(AutoBlocks(s), rt, nullptr, false);
    CHECK(Count(ev, "Heel Tap R") == 1 && Count(ev, "Heel Tap L") == 0);
    AutoSettings st;
    st.type[static_cast<int>(AutoType::Step)].on = true;
    const std::vector<Event> steps = DetectAutoEvents(AutoBlocks(st), rt, nullptr, false);
    st.type[static_cast<int>(AutoType::HeelTap)].on = true;
    const std::vector<Event> both = DetectAutoEvents(AutoBlocks(st), rt, nullptr, false);
    CHECK(Count(both, "Heel Tap R") == 1);
    CHECK(Count(both, "FS R") == Count(steps, "FS R") && Count(both, "FS L") == Count(steps, "FS L"));
    double tap = -1.0;
    for (const Event& e : both)
        if (e.marker == "Heel Tap R") tap = e.time_s;
    for (const Event& e : both)
        if (e.marker == "FS R") CHECK(std::fabs(e.time_s - tap) > kAutoReplaceS);

    const std::vector<BoneTrack> slam = Rig(Planted(-0.2), HeelDrop(0.6, 0.06, 0.02, 0.1), 3.0);
    AutoSettings                 sl;
    sl.type[static_cast<int>(AutoType::Step)].on = true;
    sl.separate_steps = true;  // the heel's own steps (combined: the ball never left, no landing)
    const std::vector<Event> slam_steps = DetectAutoEvents(AutoBlocks(sl), slam, nullptr, false);
    sl.type[static_cast<int>(AutoType::HeelTap)].on = true;
    const std::vector<Event> slam_both = DetectAutoEvents(AutoBlocks(sl), slam, nullptr, false);
    CHECK(Count(slam_steps, "Heel R") >= 1);
    CHECK(Count(slam_both, "Heel R") == Count(slam_steps, "Heel R") && Count(slam_both, "Heel Tap R") == 0);

    // The lane: the right heel's height against heel_tap_min_rise.
    AutoAnalysis aa;
    aa.params = AutoPhysicsParams(AutoType::HeelTap, 50.0, false);
    aa.analysis = AnalyseMotion(rt, aa.params);
    std::string                 why;
    const std::vector<AutoLane> lanes = AutoBlockLanes(aa, MakeAutoBlock(AutoKind::HeelTap, 'R', 50.0, 0.0), &why);
    CHECK(lanes.size() == 1 && why.empty());
    if (lanes.size() == 1) {
        CHECK(lanes[0].above && std::fabs(lanes[0].threshold - 0.013) < 1e-9 && !lanes[0].curve.empty());
        CHECK(lanes[0].curve.size() == lanes[0].contact.size());
        CHECK(*std::max_element(lanes[0].curve.begin(), lanes[0].curve.end()) > 0.03);
    }
}

// Detect's sensitivity search never raises a silent Heel Tap block (a weight shift's heel lift
// reads as one from about 75 %): searched, not raised, its value unchanged.
void TestNoRaise()
{
    const std::vector<BoneTrack> rt = Rig(Planted(-0.2), HeelDrop(0.065, 0.2), 3.0);
    AutoSettings                 s;
    s.type[static_cast<int>(AutoType::HeelTap)].on = true;
    std::vector<Block>     blocks = AutoBlocks(s);
    const std::vector<int> counts(blocks.size(), 0);
    const std::vector<int> bones(static_cast<size_t>(Role::Count), 0);
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
    TestHeelTap();
    TestRows();
    TestDetect();
    TestNoRaise();
    if (g_fails) {
        std::printf("%d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("heel_taps_test: all checks passed\n");
    return 0;
}
