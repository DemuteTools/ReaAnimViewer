// SPDX-License-Identifier: MIT
//
// Host test of the Spin type (story 10-9f, src/auto_types_spin.cpp): a synthetic rig standing
// with both feet planted and turning about the vertical by a yaw curve; the spins through
// Detect's path (DetectAutoEvents), their start and end, the 45 deg sweep, the sensitivity, a
// hip sway, a turn back, a loop's seam, the missing parts and the block / marker words. No REAPER.
//   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests

#include "auto_detect.h"
#include "auto_types.h"
#include "bone_roles.h"
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
constexpr double kRate = 240.0;

using YawFn = std::function<double(double)>;  // degrees at time t

Vec3d Yawed(double yaw_rad, double x, double y, double z)
{
    return Vec3d{x * std::cos(yaw_rad) + z * std::sin(yaw_rad), y, -x * std::sin(yaw_rad) + z * std::cos(yaw_rad)};
}

// Both feet planted 20 cm apart (the heel 14 cm behind the ball), the knee 42 cm and the up leg
// 87 cm above the heel, the hips between the up legs; the whole body turned by yaw(t) about the
// vertical through the origin. `up_legs` false: no bone for the right up leg.
std::vector<BoneTrack> TurningRig(const YawFn& yaw, double dur, bool up_legs = true)
{
    std::vector<BoneTrack> t(static_cast<size_t>(Role::Count));
    const int              n = static_cast<int>(std::floor(dur * kRate + 1e-9)) + 1;
    auto put = [&](Role r, const Vec3d& p) {
        BoneTrack& b = t[static_cast<size_t>(r)];
        b.rate_hz = kRate;
        b.pos.push_back(p);
    };
    for (int i = 0; i < n; ++i) {
        const double y = yaw(i / kRate) * kPi / 180.0;
        Vec3d        ul[2];
        for (int s = 0; s < 2; ++s) {
            const double x = s ? 0.1 : -0.1;
            const char   side = s ? 'R' : 'L';
            put(FootPartRole(side, 0), Yawed(y, x, 0.08, -0.14));
            put(FootPartRole(side, 1), Yawed(y, x, 0.02, 0.0));
            put(FootPartRole(side, 2), Yawed(y, x, 0.0, 0.07));
            put(s ? Role::RightKnee : Role::LeftKnee, Yawed(y, x, 0.50, -0.14));
            ul[s] = Yawed(y, x, 0.95, -0.14);
            if (up_legs || s == 0) put(s ? Role::RightUpLeg : Role::LeftUpLeg, ul[s]);
        }
        put(Role::Hips, Vec3d{0.5 * (ul[0].x + ul[1].x), 1.0, 0.5 * (ul[0].z + ul[1].z)});
    }
    return t;
}

double Ease(double u)
{
    u = std::clamp(u, 0.0, 1.0);
    return 0.5 - 0.5 * std::cos(kPi * u);
}

// A turn of `deg` degrees eased over [t0, t0 + d].
YawFn Turn(double t0, double d, double deg)
{
    return [=](double t) { return deg * Ease((t - t0) / d); };
}

std::vector<Block> SpinBlocks(double sens = kAutoDefaultSens)
{
    AutoSettings s;
    s.type[static_cast<int>(AutoType::Spin)].on = true;
    s.type[static_cast<int>(AutoType::Spin)].sens = sens;
    return AutoBlocks(s);
}

struct Found {
    std::vector<double> start, end;
    std::vector<double> sweep;
};

Found Run(const std::vector<BoneTrack>& tracks, bool looping = false, double sens = kAutoDefaultSens)
{
    const std::vector<Block> blocks = SpinBlocks(sens);
    Found                    f;
    for (const Event& e : DetectAutoEvents(blocks, tracks, nullptr, looping)) {
        AutoKind k;
        if (!AutoBlockKind(blocks[static_cast<size_t>(e.block)], &k, nullptr)) continue;
        if (k == AutoKind::Spin) {
            f.start.push_back(e.time_s);
            f.sweep.push_back(e.strength);
        } else if (k == AutoKind::SpinEnd) {
            f.end.push_back(e.time_s);
        }
    }
    return f;
}

void TestWords()
{
    // Body's sideless type: two blocks, side B, "Spin" and "Spin End".
    CHECK(AutoCategoryOf(AutoType::Spin) == AutoCategory::Body);
    CHECK(std::string(AutoTypeLabel(AutoType::Spin)) == "Spin" && std::string(AutoTypeRow(AutoType::Spin).sound_word) == "spin");
    CHECK(AutoKindSideless(AutoKind::Spin) && AutoKindSideless(AutoKind::SpinEnd));
    CHECK(std::string(AutoKindSides(AutoKind::Spin)) == "B");
    CHECK(std::string(AutoKindWord(AutoKind::Spin)) == "spin" && std::string(AutoKindWord(AutoKind::SpinEnd)) == "spin_end");
    CHECK(AutoMarkerName(AutoKind::Spin, 'L') == "Spin" && AutoMarkerName(AutoKind::SpinEnd, 'R') == "Spin End");
    const std::vector<Block> b = SpinBlocks();
    CHECK(b.size() == 2);
    if (b.size() == 2) {
        CHECK(b[0].auto_type == "spin" && b[0].auto_side == 'B' && b[0].marker == "Spin");
        CHECK(b[1].auto_type == "spin_end" && b[1].auto_side == 'B' && b[1].marker == "Spin End");
    }
    CHECK(AutoKindRow(AutoKind::Spin).degrees && AutoKindRow(AutoKind::SpinEnd).degrees);
    // The sensitivity divides the sweep only.
    const PhysicsParams d, hi = AutoPhysicsParams(AutoType::Spin, 100.0, false);
    CHECK(hi.spin_min_deg == d.spin_min_deg / 2.0 && hi.spin_start_dps == d.spin_start_dps);
    CHECK(!SameAutoAnalysis(hi, d));
}

void TestTurns()
{
    // Standing still: nothing.
    {
        const Found f = Run(TurningRig([](double) { return 0.0; }, 3.0));
        CHECK(f.start.empty() && f.end.empty());
    }
    // A 180 deg turn over [1, 2] s: one spin, its start near 1 s and its end near 2 s (the
    // eased rate passes 5 deg/s 8 ms in and out of the turn), sweeping about 180 deg.
    {
        const Found f = Run(TurningRig(Turn(1.0, 1.0, -180.0), 3.0));
        CHECK(f.start.size() == 1 && f.end.size() == 1);
        if (f.start.size() == 1 && f.end.size() == 1) {
            CHECK(std::fabs(f.start[0] - 1.0) < 0.06 && std::fabs(f.end[0] - 2.0) < 0.06);
            CHECK(f.sweep[0] > 175.0 && f.sweep[0] < 181.0);
        }
    }
    // A 30 deg turn: under 45 deg, none; at 100 % sensitivity (22.5 deg) it counts.
    {
        const std::vector<BoneTrack> tr = TurningRig(Turn(1.0, 0.5, 30.0), 2.5);
        CHECK(Run(tr).start.empty());
        const Found f = Run(tr, false, 100.0);
        CHECK(f.start.size() == 1 && f.end.size() == 1);
        // Story 10-9q: Detect's search raises a silent Spin block as any type's (the 30 deg turn
        // at 80 %).
        const std::vector<Block> blocks = SpinBlocks();
        std::vector<int>         role_to_bone(static_cast<size_t>(Role::Count), 0);
        const std::vector<AutoSensResult> r =
            SearchAutoSensitivity(blocks, tr, std::vector<int>(blocks.size(), 0), role_to_bone, false);
        CHECK(r.size() == blocks.size());
        for (const AutoSensResult& x : r) CHECK(x.searched && x.raised && !x.none && x.sens == 80.0);
    }
    // A sudden turn (90 deg/s from 1 s to 2 s, no easing): the markers sit on the turn's start and
    // end, not 80 ms out where the smoothed rate passes 5 deg/s.
    {
        const Found f = Run(TurningRig([](double t) { return 90.0 * std::clamp(t - 1.0, 0.0, 1.0); }, 3.0));
        CHECK(f.start.size() == 1 && f.end.size() == 1);
        if (f.start.size() == 1 && f.end.size() == 1)
            CHECK(std::fabs(f.start[0] - 1.0) < 0.01 && std::fabs(f.end[0] - 2.0) < 0.01);
    }
    // A hip sway of +-15 deg at 1 Hz (a walk's at most): sweeps 30 deg each way, none.
    {
        const Found f = Run(TurningRig([](double t) { return 15.0 * std::sin(2.0 * kPi * t); }, 4.0));
        CHECK(f.start.empty());
    }
    // Story 10-9h: a 90 deg turn one way then straight back is a twist (out and back, a punch's
    // hip drive): no spin.
    {
        const YawFn y = [](double t) { return 90.0 * Ease((t - 0.5) / 0.6) - 90.0 * Ease((t - 1.1) / 0.6); };
        const Found f = Run(TurningRig(y, 2.5));
        CHECK(f.start.empty() && f.end.empty());
    }
    // The same turn back after a 0.5 s pause: two spins (a turn back ends the first).
    {
        const YawFn y = [](double t) { return 90.0 * Ease((t - 0.5) / 0.6) - 90.0 * Ease((t - 1.6) / 0.6); };
        const Found f = Run(TurningRig(y, 3.0));
        CHECK(f.start.size() == 2 && f.end.size() == 2);
        if (f.start.size() == 2 && f.end.size() == 2) {
            CHECK(f.end[0] <= f.start[1] + 1e-9);
            CHECK(f.sweep[0] > 85.0 && f.sweep[1] > 85.0);
        }
    }
    // A 180 deg turn and a 45 deg turn straight back (unlike sweeps): both spins.
    {
        const YawFn y = [](double t) { return 180.0 * Ease((t - 0.5) / 0.8) - 50.0 * Ease((t - 1.3) / 0.4); };
        const Found f = Run(TurningRig(y, 2.5));
        CHECK(f.start.size() == 2 && f.end.size() == 2);
    }
    // A turn running at the clip's start and end is kept, cut there.
    {
        const Found f = Run(TurningRig([](double t) { return 90.0 * t; }, 1.0));
        CHECK(f.start.size() == 1 && f.end.size() == 1);
        if (f.start.size() == 1 && f.end.size() == 1) CHECK(f.start[0] == 0.0 && std::fabs(f.end[0] - 1.0) < 1e-9);
    }
}

void TestLoop()
{
    // A loop turning 90 deg per cycle (one turn over [1, 2] of 3 s): its heading jumps back at
    // the seam, which is no spin; the cycle's turn is found once, in [0, T).
    const std::vector<BoneTrack> tr = TurningRig(Turn(1.0, 1.0, 90.0), 3.0);
    const Found                  f = Run(tr, true);
    CHECK(f.start.size() == 1 && f.end.size() == 1);
    if (f.start.size() == 1 && f.end.size() == 1) {
        CHECK(std::fabs(f.start[0] - 1.0) < 0.06 && std::fabs(f.end[0] - 2.0) < 0.06);
        CHECK(f.sweep[0] > 85.0 && f.sweep[0] < 95.0);
    }
    // A turn across the seam: the cycle's yaw goes 0 -> 45 deg over its last 0.5 s and -45 -> 0
    // over its first 0.5 s, at 90 deg/s, so the seam joins one 90 deg spin: start near 2.5 s,
    // end near 0.5 s.
    const YawFn y2 = [](double t) {
        return t < 0.5 ? -45.0 + 90.0 * t : t < 2.5 ? 0.0 : 90.0 * (t - 2.5);
    };
    const Found g = Run(TurningRig(y2, 3.0), true);
    CHECK(g.start.size() == 1 && g.end.size() == 1);
    if (g.start.size() == 1 && g.end.size() == 1) {
        CHECK(std::fabs(g.start[0] - 2.5) < 0.01 && std::fabs(g.end[0] - 0.5) < 0.01);  // a sudden 90 deg/s: the markers on the unsmoothed yaw
        CHECK(g.sweep[0] > 85.0 && g.sweep[0] < 95.0);
    }
}

void TestMissing()
{
    // No right up leg: no hip line, no spin; the parts and the block say so.
    const std::vector<BoneTrack> tr = TurningRig(Turn(1.0, 1.0, 180.0), 3.0, false);
    CHECK(Run(tr).start.empty());
    std::vector<int> map(static_cast<size_t>(Role::Count), -1);
    for (size_t r = 0; r < map.size(); ++r)
        if (!tr[r].pos.empty()) map[r] = static_cast<int>(r);
    const std::vector<std::string> missing = AutoMissingParts(AutoType::Spin, false, map);
    CHECK(std::find(missing.begin(), missing.end(), RoleName(Role::RightUpLeg)) != missing.end());
    for (const Block& b : SpinBlocks()) CHECK(AutoBlockPartMissing(b, map));
    map[static_cast<size_t>(Role::RightUpLeg)] = static_cast<int>(Role::RightUpLeg);
    CHECK(AutoMissingParts(AutoType::Spin, false, map).empty());
    for (const Block& b : SpinBlocks()) CHECK(!AutoBlockPartMissing(b, map));
}

void TestLanes()
{
    // The block's lane: the hip line's |yaw rate| against the start rate, shaded over the spin.
    const std::vector<BoneTrack> tr = TurningRig(Turn(1.0, 1.0, 180.0), 3.0);
    AutoAnalysis                 aa;
    aa.params = AutoPhysicsParams(AutoType::Spin, kAutoDefaultSens, false);
    aa.analysis = AnalyseMotion(tr, aa.params);
    CHECK(aa.analysis.ok);
    const std::vector<Block>    b = SpinBlocks();
    std::string                 why;
    const std::vector<AutoLane> lanes = AutoBlockLanes(aa, b[0], &why);
    CHECK(lanes.size() == 1);
    if (lanes.size() == 1) {
        CHECK(lanes[0].unit == "deg/s" && lanes[0].threshold == 15.0 && lanes[0].above);
        CHECK(lanes[0].curve.size() == aa.analysis.samples && lanes[0].contact.size() == aa.analysis.samples);
        const size_t mid = static_cast<size_t>(1.5 * kRate);
        if (mid < lanes[0].curve.size()) CHECK(lanes[0].curve[mid] > 200.0 && lanes[0].contact[mid] == 1);
        CHECK(lanes[0].contact[10] == 0);
    }
}

}  // namespace

int main()
{
    TestWords();
    TestTurns();
    TestLoop();
    TestMissing();
    TestLanes();
    if (g_fails) {
        std::printf("%d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("auto_spin: all checks passed\n");
    return 0;
}
