// SPDX-License-Identifier: MIT
//
// Host test of the body moves (story 10-12e, src/auto_types_body_move.cpp): Crouch, Stand Up,
// Bend and Rise on a synthetic rig (both feet planted, a two-link leg under the hips, the trunk
// leaning forward by a curve), through Detect's path (DetectAutoEvents): a squat (one Crouch,
// its back bending a little, no Bend), a fold (Bend then Rise), idle, a push on one foot while the
// other steps up (no Stand Up), the sensitivity, the missing parts, Peak / Start / Start & Stop
// (the stop kinds, their words, markers and sound keys), the settings' round trip and an item
// saved before. No REAPER.
//   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests

#include "auto_detect.h"
#include "auto_types.h"
#include "bone_roles.h"
#include "motion_physics.h"
#include "sound_auto_assign.h"

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
constexpr double kShin = 0.45, kThigh = 0.45;  // m: the leg length 0.9
constexpr double kStand = 0.97;                // the up legs' height standing (knees a little bent)

using Curve = std::function<double(double)>;  // a value at time t

double Ease(double u)
{
    u = std::clamp(u, 0.0, 1.0);
    return 0.5 - 0.5 * std::cos(kPi * u);
}

// `amount` eased in over [t0, t0 + d].
Curve Ramp(double t0, double d, double amount)
{
    return [=](double t) { return amount * Ease((t - t0) / d); };
}

// `amount` eased in over [t0, t0 + d], held, eased out over [t1, t1 + d].
Curve Hill(double t0, double t1, double d, double amount)
{
    return [=](double t) { return amount * (Ease((t - t0) / d) - Ease((t - t1) / d)); };
}

Curve Zero()
{
    return [](double) { return 0.0; };
}

struct RigOpts {
    bool  trunk = true;  // spine, chest, neck, head
    bool  up_legs = true;     // the up leg bones (the hip line)
    bool  shoulders = false;  // both shoulders on the trunk (the facing's fallback)
    // Both feet lifted by this (m) at time t (0 = planted): a hop.
    Curve both_lift = Zero();
    // The left foot lifted by this (m) at time t (0 = planted): a step up while the right pushes.
    Curve left_lift = Zero();
};

// Both feet planted (the left at x = +0.1, so the body faces +z: up x (left -> right)), the up
// legs at kStand - drop(t) over them, the knee bent forward to keep the shin and thigh lengths, the
// hips between the up legs; the trunk (spine 0.1, chest 0.3, neck 0.5, head 0.62 m from the hips)
// leaning forward by fold(t) degrees.
std::vector<BoneTrack> Rig(const Curve& drop, const Curve& fold, double dur, const RigOpts& o = {})
{
    std::vector<BoneTrack> t(static_cast<size_t>(Role::Count));
    const int              n = static_cast<int>(std::floor(dur * kRate + 1e-9)) + 1;
    auto put = [&](Role r, const Vec3d& p) {
        BoneTrack& b = t[static_cast<size_t>(r)];
        b.rate_hz = kRate;
        b.pos.push_back(p);
    };
    for (int i = 0; i < n; ++i) {
        const double ti = i / kRate;
        const double H = kStand - drop(ti);
        for (int s = 0; s < 2; ++s) {
            const double x = s ? -0.1 : 0.1;
            const char   side = s ? 'R' : 'L';
            const double lift = (s ? 0.0 : o.left_lift(ti)) + o.both_lift(ti);
            const Vec3d  heel{x, 0.08 + lift, -0.05}, ball{x, 0.02 + lift, 0.09}, tip{x, 0.0 + lift, 0.16};
            put(FootPartRole(side, 0), heel);
            put(FootPartRole(side, 1), ball);
            put(FootPartRole(side, 2), tip);
            // The knee: the two-link solution in the sagittal plane, forward (+z).
            const Vec3d  up{x, H, -0.05};
            const double dy = up.y - heel.y, d = std::max(1e-6, std::fabs(dy));
            const double half = std::min(d / 2.0, kShin - 1e-6);
            const double h = std::sqrt(kShin * kShin - half * half);
            put(s ? Role::RightKnee : Role::LeftKnee, Vec3d{x, heel.y + dy / 2.0, heel.z + h});
            if (o.up_legs) put(s ? Role::RightUpLeg : Role::LeftUpLeg, up);
        }
        const Vec3d  hips{0.0, H, -0.05};
        const double a = fold(ti) * kPi / 180.0;
        put(Role::Hips, hips);
        if (o.trunk) {
            auto along = [&](double l) { return Vec3d{hips.x, hips.y + l * std::cos(a), hips.z + l * std::sin(a)}; };
            put(Role::Spine, along(0.10));
            put(Role::Chest, along(0.30));
            put(Role::Neck, along(0.50));
            put(Role::Head, along(0.62));
            if (o.shoulders) {
                const Vec3d c = along(0.45);
                put(Role::LeftShoulder, Vec3d{c.x + 0.18, c.y, c.z});
                put(Role::RightShoulder, Vec3d{c.x - 0.18, c.y, c.z});
            }
        }
    }
    return t;
}

const AutoType kMoves[4] = {AutoType::Crouch, AutoType::StandUp, AutoType::Bend, AutoType::Rise};

// Every body move ticked, at `sens`, in mode 0 = Peak, 1 = Start, 2 = Start & Stop.
AutoSettings MoveSettings(int mode = 1, double sens = kAutoDefaultSens)
{
    AutoSettings s;
    for (AutoType t : kMoves) {
        const int ti = static_cast<int>(t);
        s.type[ti].on = true;
        s.type[ti].sens = sens;
        s.at_start[ti] = mode >= 1;
        s.with_stop[ti] = mode == 2;
    }
    return s;
}

struct Hit {
    AutoKind kind;
    double   t;
    double   strength;
};

std::vector<Hit> Run(const std::vector<BoneTrack>& tracks, const AutoSettings& s, bool looping = false)
{
    const std::vector<Block> blocks = AutoBlocks(s);
    std::vector<Hit>         out;
    for (const Event& e : DetectAutoEvents(blocks, tracks, nullptr, looping)) {
        AutoKind k;
        if (AutoBlockKind(blocks[static_cast<size_t>(e.block)], &k, nullptr)) out.push_back({k, e.time_s, e.strength});
    }
    return out;
}

int Count(const std::vector<Hit>& h, AutoKind k)
{
    return static_cast<int>(std::count_if(h.begin(), h.end(), [&](const Hit& x) { return x.kind == k; }));
}

const Hit* First(const std::vector<Hit>& h, AutoKind k)
{
    for (const Hit& x : h)
        if (x.kind == k) return &x;
    return nullptr;
}

// ---- Words, rows, settings ----------------------------------------------------------------------

void TestWords()
{
    const char* words[12] = {"crouch", "crouch_peak", "crouch_stop", "stand_up", "stand_up_peak", "stand_up_stop",
                             "bend",   "bend_peak",   "bend_stop",   "rise",     "rise_peak",     "rise_stop"};
    const AutoKind kinds[12] = {AutoKind::Crouch,  AutoKind::CrouchPeak,  AutoKind::CrouchStop, AutoKind::StandUp,
                                AutoKind::StandUpPeak, AutoKind::StandUpStop, AutoKind::Bend,   AutoKind::BendPeak,
                                AutoKind::BendStop, AutoKind::Rise, AutoKind::RisePeak, AutoKind::RiseStop};
    for (int i = 0; i < 12; ++i) {
        AutoKind k;
        CHECK(AutoKindFromWord(words[i], &k) && k == kinds[i] && std::string(AutoKindWord(kinds[i])) == words[i]);
        CHECK(AutoKindSideless(kinds[i]) && std::string(AutoKindSides(kinds[i])) == "B");
        CHECK(AutoCategoryOf(AutoTypeOfKind(kinds[i])) == AutoCategory::Body);
        CHECK(AutoKindRow(kinds[i]).at_stop == (i % 3 == 2) && AutoKindRow(kinds[i]).at_start == (i % 3 == 0));
    }
    CHECK(AutoMarkerName(AutoKind::Crouch, 'L') == "Crouch" && AutoMarkerName(AutoKind::CrouchPeak, 'B') == "Crouch" &&
          AutoMarkerName(AutoKind::CrouchStop, 'B') == "Crouch Stop");
    CHECK(AutoMarkerName(AutoKind::StandUpStop, 'B') == "Stand Up Stop" && AutoMarkerName(AutoKind::BendStop, 'B') == "Bend Stop" &&
          AutoMarkerName(AutoKind::RiseStop, 'B') == "Rise Stop");
    CHECK(std::string(AutoTypeLabel(AutoType::StandUp)) == "Stand Up");
    // The stop has its own sound key (Sound assign lists it apart); the peak shares the start's.
    CHECK(AutoSoundKey(AutoKind::Crouch, 'B') == "crouch:B" && AutoSoundKey(AutoKind::CrouchPeak, 'B') == "crouch:B" &&
          AutoSoundKey(AutoKind::CrouchStop, 'B') == "crouch_stop:B" && AutoSoundKey(AutoKind::RiseStop, 'L') == "rise_stop:B");
    CHECK(AutoDefaultColor(AutoKind::Crouch, 'B') == AutoDefaultColor(AutoKind::CrouchPeak, 'B') &&
          AutoDefaultColor(AutoKind::Crouch, 'B') != AutoDefaultColor(AutoKind::CrouchStop, 'B'));
    for (AutoType t : kMoves) {
        const AutoTypeInfo& r = AutoTypeRow(t);
        CHECK(r.sideless && r.start_option && r.stop_option && !r.part_split && !r.shared_sides && r.replaces == -1);
        CHECK(AutoStartDefault(t) && AutoSettings().at_start[static_cast<int>(t)] &&
              !AutoSettings().with_stop[static_cast<int>(t)]);
    }
    // The sensitivity divides each type's minimum, and tells the analyses apart.
    const PhysicsParams d;
    CHECK(AutoPhysicsParams(AutoType::Crouch, 100.0, false).crouch_min_drop == d.crouch_min_drop / 2.0);
    CHECK(AutoPhysicsParams(AutoType::StandUp, 0.0, false).stand_min_rise == d.stand_min_rise * 2.0);
    CHECK(AutoPhysicsParams(AutoType::Bend, 100.0, false).bend_min_deg == d.bend_min_deg / 2.0);
    CHECK(AutoPhysicsParams(AutoType::Rise, 100.0, false).rise_min_deg == d.rise_min_deg / 2.0);
    CHECK(AutoPhysicsParams(AutoType::Bend, 100.0, false).crouch_min_drop == d.crouch_min_drop);
    CHECK(!SameAutoAnalysis(AutoPhysicsParams(AutoType::Crouch, 80.0, false), d));
    // No other type's constant moves: at the default every type shares one analysis.
    for (AutoType t : kMoves) CHECK(SameAutoAnalysis(AutoPhysicsParams(t, kAutoDefaultSens, false), d));
}

std::vector<std::string> Words(const std::vector<Block>& blocks)
{
    std::vector<std::string> w;
    for (const Block& b : blocks) w.push_back(b.auto_type + ":" + std::string(1, b.auto_side));
    return w;
}

void TestSettings()
{
    using V = std::vector<std::string>;
    // Peak / Start / Start & Stop: the blocks, side B, in type order (the stop after its start).
    CHECK(Words(AutoBlocks(MoveSettings(0))) == V({"crouch_peak:B", "stand_up_peak:B", "bend_peak:B", "rise_peak:B"}));
    CHECK(Words(AutoBlocks(MoveSettings(1))) == V({"crouch:B", "stand_up:B", "bend:B", "rise:B"}));
    CHECK(Words(AutoBlocks(MoveSettings(2))) == V({"crouch:B", "crouch_stop:B", "stand_up:B", "stand_up_stop:B", "bend:B",
                                                   "bend_stop:B", "rise:B", "rise_stop:B"}));
    // with_stop is read only at the Start: Peak with it set makes no stop.
    AutoSettings peak_stop = MoveSettings(0);
    for (AutoType t : kMoves) peak_stop.with_stop[static_cast<int>(t)] = true;
    CHECK(Words(AutoBlocks(peak_stop)) == Words(AutoBlocks(MoveSettings(0))));
    // with_stop on a type with no stop_option does nothing.
    AutoSettings punch;
    punch.type[static_cast<int>(AutoType::Punch)].on = true;
    const std::vector<Block> pb = AutoBlocks(punch);
    punch.with_stop[static_cast<int>(AutoType::Punch)] = true;
    punch.at_start[static_cast<int>(AutoType::Punch)] = false;
    CHECK(Words(AutoBlocks(punch)) == Words(pb));
    // Round trip: the blocks say the mode.
    for (int mode = 0; mode < 3; ++mode) {
        const AutoSettings s = MoveSettings(mode);
        const AutoSettings r = ReadAutoSettings(AutoBlocks(s));
        for (AutoType t : kMoves) {
            const int ti = static_cast<int>(t);
            CHECK(r.type[ti].on && r.at_start[ti] == (mode >= 1) && r.with_stop[ti] == (mode == 2));
        }
        CHECK(!AutoPending(r, s));
        CHECK(r == s);
    }
    // A stop block alone reads as Start & Stop.
    {
        const AutoSettings r = ReadAutoSettings({MakeAutoBlock(AutoKind::BendStop, 'B', 50.0, 0.0)});
        const int          ti = static_cast<int>(AutoType::Bend);
        CHECK(r.type[ti].on && r.at_start[ti] && r.with_stop[ti]);
    }
    // Pending: Start <-> Start & Stop on a ticked type waits for Detect; on an unticked one not.
    {
        AutoSettings a = MoveSettings(1), b = MoveSettings(2);
        CHECK(AutoPending(a, b) && AutoPending(b, a) && a != b);
        for (AutoType t : kMoves) a.type[static_cast<int>(t)].on = b.type[static_cast<int>(t)].on = false;
        CHECK(!AutoPending(a, b));
    }
    // Apply: Start -> Start & Stop keeps the start blocks (place, marker, edits) and adds the
    // stops; back to Start drops them; Peak keeps the block (its kind switched).
    {
        std::vector<Block> blocks = AutoBlocks(MoveSettings(1));
        blocks[0].marker = "My crouch";
        bool                   changed = false;
        const std::vector<int> map = ApplyAutoSettings(blocks, MoveSettings(2), &changed);
        CHECK(changed && blocks.size() == 8 && map.size() == 4 && map[0] == 0 && blocks[0].marker == "My crouch");
        CHECK(Words(blocks) == V({"crouch:B", "stand_up:B", "bend:B", "rise:B", "crouch_stop:B", "stand_up_stop:B",
                                  "bend_stop:B", "rise_stop:B"}));
        CHECK(blocks[4].marker == "Crouch Stop");
        std::vector<int> back = ApplyAutoSettings(blocks, MoveSettings(1), &changed);
        CHECK(changed && blocks.size() == 4 && back[4] == -1 && back[0] == 0 && blocks[0].marker == "My crouch");
        ApplyAutoSettings(blocks, MoveSettings(0), &changed);
        CHECK(changed && blocks.size() == 4 && blocks[0].auto_type == "crouch_peak" && blocks[0].marker == "My crouch");
    }
}

// An item saved before the body moves: its blocks read and apply unchanged (no move ticked, no
// stop), and a saved block of a word this version does not know stays.
void TestOldItem()
{
    AutoSettings old;
    old.type[static_cast<int>(AutoType::Step)].on = true;
    old.type[static_cast<int>(AutoType::Spin)].on = true;
    old.type[static_cast<int>(AutoType::Punch)].on = true;
    std::vector<Block> blocks = AutoBlocks(old);
    Block              future;
    future.auto_type = "crouch_future";
    future.auto_side = 'B';
    future.marker = "Future";
    blocks.push_back(future);
    const std::vector<Block> saved = blocks;
    const AutoSettings       r = ReadAutoSettings(blocks);
    for (AutoType t : kMoves) CHECK(!r.type[static_cast<int>(t)].on && !r.with_stop[static_cast<int>(t)]);
    bool changed = true;
    ApplyAutoSettings(blocks, r, &changed);
    CHECK(!changed && blocks.size() == saved.size() && blocks.back().auto_type == "crouch_future");
}

// ---- Detection ---------------------------------------------------------------------------------

// A squat: the hips down 0.4 leg (0.36 m) over 0.6 s from 1.0 s, the back bending 15 deg with it,
// held, then up at 2.5 s. Crouch at its start, Stand Up at its; no Bend / Rise (one motion).
void TestSquat()
{
    const std::vector<BoneTrack> rig = Rig(Hill(1.0, 2.5, 0.6, 0.36), Hill(1.0, 2.5, 0.6, 15.0), 4.0);
    const std::vector<Hit>       h = Run(rig, MoveSettings(1));
    CHECK(Count(h, AutoKind::Crouch) == 1 && Count(h, AutoKind::StandUp) == 1);
    CHECK(Count(h, AutoKind::Bend) == 0 && Count(h, AutoKind::Rise) == 0);
    const Hit* c = First(h, AutoKind::Crouch);
    const Hit* u = First(h, AutoKind::StandUp);
    if (c) CHECK(c->t >= 0.95 && c->t <= 1.15 && c->strength > 0.8 && c->strength < 1.3);  // ~1.05 leg/s peak
    if (u) CHECK(u->t >= 2.45 && u->t <= 2.65);
    // Start & Stop: Crouch Stop near rest (1.6 s), Stand Up Stop near 3.1 s.
    const std::vector<Hit> s = Run(rig, MoveSettings(2));
    const Hit*             cs = First(s, AutoKind::CrouchStop);
    const Hit*             us = First(s, AutoKind::StandUpStop);
    CHECK(cs && us && Count(s, AutoKind::Crouch) == 1);
    if (cs) CHECK(cs->t >= 1.45 && cs->t <= 1.65);
    if (us) CHECK(us->t >= 2.95 && us->t <= 3.15);
    // Peak: at the fastest (mid-way, 1.3 s).
    const std::vector<Hit> p = Run(rig, MoveSettings(0));
    const Hit*             cp = First(p, AutoKind::CrouchPeak);
    CHECK(cp && cp->t >= 1.25 && cp->t <= 1.35);
    // A deeper fold during the squat (35 deg) is still the crouch: one motion, one marker.
    const std::vector<Hit> f = Run(Rig(Hill(1.0, 2.5, 0.6, 0.36), Hill(1.0, 2.5, 0.6, 35.0), 4.0), MoveSettings(1));
    CHECK(Count(f, AutoKind::Crouch) == 1 && Count(f, AutoKind::Bend) == 0 && Count(f, AutoKind::Rise) == 0);
}

// A fold: the trunk forward 45 deg over 0.5 s from 1.0 s, the hips down 0.1 leg, held, back up at
// 2.5 s. Bend, then Rise; no Crouch (0.1 leg is under the minimum).
void TestFold()
{
    const std::vector<BoneTrack> rig = Rig(Hill(1.0, 2.5, 0.5, 0.09), Hill(1.0, 2.5, 0.5, 45.0), 4.0);
    const std::vector<Hit>       h = Run(rig, MoveSettings(2));
    CHECK(Count(h, AutoKind::Bend) == 1 && Count(h, AutoKind::Rise) == 1 && Count(h, AutoKind::BendStop) == 1 &&
          Count(h, AutoKind::RiseStop) == 1);
    CHECK(Count(h, AutoKind::Crouch) == 0 && Count(h, AutoKind::StandUp) == 0);
    const Hit* b = First(h, AutoKind::Bend);
    const Hit* r = First(h, AutoKind::Rise);
    if (b) CHECK(b->t >= 0.95 && b->t <= 1.15 && b->strength > 100.0 && b->strength < 160.0);  // ~141 deg/s peak
    if (r) CHECK(r->t >= 2.45 && r->t <= 2.65);
    // A lean back (the trunk behind the vertical) is no fold.
    const std::vector<Hit> back = Run(Rig(Zero(), Hill(1.0, 2.5, 0.5, -30.0), 4.0), MoveSettings(1));
    CHECK(back.empty());
    // The sensitivity: a 14 deg fold is no Bend at 50 %, one at 100 % (minimum 10 deg).
    const std::vector<BoneTrack> small = Rig(Zero(), Hill(1.0, 2.5, 0.3, 14.0), 4.0);
    CHECK(Count(Run(small, MoveSettings(1)), AutoKind::Bend) == 0);
    CHECK(Count(Run(small, MoveSettings(1, 100.0)), AutoKind::Bend) == 1);
    // A fold already under way at the clip's first sample began before the clip: none; one
    // starting from rest just after it is kept
    CHECK(Count(Run(Rig(Zero(), Ramp(-0.25, 0.5, 40.0), 2.0), MoveSettings(1)), AutoKind::Bend) == 0);
    // (still at the first frame: the rate there under the end rate; the smoothing reads a move
    // starting on frame 0 itself as already moving).
    const std::vector<Hit> first = Run(Rig(Ramp(0.1, 0.6, 0.36), Zero(), 2.0), MoveSettings(1));
    CHECK(Count(first, AutoKind::Crouch) == 1 && First(first, AutoKind::Crouch) && First(first, AutoKind::Crouch)->t < 0.2);
}

// Idle (a slow sway of the trunk, 20 deg over 3 s, and the hips breathing 1 cm): nothing.
void TestIdle()
{
    const Curve breathe = [](double t) { return 0.01 * std::sin(2.0 * kPi * t / 3.0); };
    const Curve sway = [](double t) { return 10.0 + 10.0 * std::sin(2.0 * kPi * t / 6.0); };
    CHECK(Run(Rig(breathe, sway, 8.0), MoveSettings(2)).empty());
    CHECK(Run(Rig(Zero(), Zero(), 3.0), MoveSettings(2)).empty());
}

// A push on one foot while the other steps up (a climb): the hips up 0.3 leg over the right foot
// while the left is in the air the whole time: no Stand Up (both feet down over a quarter of the
// move at least). The same rise with both feet down is one.
void TestOneFootPush()
{
    // The left foot up 30 cm from 0.8 s to 2.9 s, feeling for the next hold (moving: a foot held
    // still in the air would read as down).
    RigOpts     o;
    const Curve up = Hill(0.8, 2.6, 0.3, 0.30);
    o.left_lift = [up](double t) { return up(t) + (t > 1.1 && t < 2.6 ? 0.1 * std::sin(2.0 * kPi * (t - 1.1) / 0.6) : 0.0); };
    // The hips start low (a bent leg on the right foot) and rise 0.27 m over 0.6 s from 1.4 s.
    const Curve drop = [](double t) { return 0.30 - 0.27 * Ease((t - 1.4) / 0.6); };
    CHECK(Count(Run(Rig(drop, Zero(), 3.5, o), MoveSettings(1)), AutoKind::StandUp) == 0);
    CHECK(Count(Run(Rig(drop, Zero(), 3.5), MoveSettings(1)), AutoKind::StandUp) == 1);
}

// The parts: no spine, chest nor neck: Bend / Rise list them and find nothing; Crouch still runs.
void TestMissing()
{
    RigOpts o;
    o.trunk = false;
    const std::vector<BoneTrack> rig = Rig(Hill(1.0, 2.5, 0.6, 0.36), Zero(), 4.0, o);
    std::vector<int>             map(static_cast<size_t>(Role::Count), -1);
    for (size_t r = 0; r < rig.size(); ++r)
        if (!rig[r].pos.empty()) map[r] = static_cast<int>(r);
    const std::vector<std::string> miss = AutoMissingParts(AutoType::Bend, false, map);
    CHECK(std::find(miss.begin(), miss.end(), RoleName(Role::Neck)) != miss.end() &&
          std::find(miss.begin(), miss.end(), RoleName(Role::Spine)) != miss.end());
    CHECK(AutoMissingParts(AutoType::Crouch, false, map).empty());
    CHECK(AutoBlockPartMissing(MakeAutoBlock(AutoKind::Bend, 'B', 50.0, 0.0), map));
    CHECK(AutoBlockPartMissing(MakeAutoBlock(AutoKind::RiseStop, 'B', 50.0, 0.0), map));
    CHECK(!AutoBlockPartMissing(MakeAutoBlock(AutoKind::Crouch, 'B', 50.0, 0.0), map));
    const std::vector<Hit> h = Run(rig, MoveSettings(1));
    CHECK(Count(h, AutoKind::Bend) == 0 && Count(h, AutoKind::Rise) == 0 && Count(h, AutoKind::Crouch) == 1);
    // A neck alone (no spine or chest) is enough; without the hips Crouch lists them.
    map[static_cast<size_t>(Role::Neck)] = static_cast<int>(Role::Neck);
    CHECK(AutoMissingParts(AutoType::Bend, false, map).empty());
    map[static_cast<size_t>(Role::Hips)] = -1;
    const std::vector<std::string> mh = AutoMissingParts(AutoType::Crouch, false, map);
    CHECK(std::find(mh.begin(), mh.end(), RoleName(Role::Hips)) != mh.end());
    // No foot bone (the scale from the knee and the up leg): Bend / Rise list the feet as Crouch
    // does (the flights are read from them) and cannot run.
    std::vector<int> nofeet(static_cast<size_t>(Role::Count), -1);
    for (Role r : {Role::Hips, Role::LeftKnee, Role::RightKnee, Role::LeftUpLeg, Role::RightUpLeg, Role::Spine, Role::Chest,
                   Role::Neck})
        nofeet[static_cast<size_t>(r)] = static_cast<int>(r);
    for (AutoType t : {AutoType::Bend, AutoType::Rise, AutoType::Crouch}) {
        const std::vector<std::string> mf = AutoMissingParts(t, false, nofeet);
        CHECK(std::find(mf.begin(), mf.end(), RoleName(Role::LeftHeel)) != mf.end() &&
              std::find(mf.begin(), mf.end(), RoleName(Role::RightHeel)) != mf.end());
    }
    CHECK(AutoBlockPartMissing(MakeAutoBlock(AutoKind::Bend, 'B', 50.0, 0.0), nofeet));
    CHECK(AutoBlockPartMissing(MakeAutoBlock(AutoKind::RiseStop, 'B', 50.0, 0.0), nofeet));
}

// The facing from both shoulders when the rig has no up legs: Bend needs nothing more, and a fold
// still gives one Bend and one Rise.
void TestShoulderFacing()
{
    RigOpts o;
    o.up_legs = false;
    o.shoulders = true;
    const std::vector<BoneTrack> rig = Rig(Zero(), Hill(1.0, 2.5, 0.5, 45.0), 4.0, o);
    std::vector<int>             map(static_cast<size_t>(Role::Count), -1);
    for (size_t r = 0; r < rig.size(); ++r)
        if (!rig[r].pos.empty()) map[r] = static_cast<int>(r);
    CHECK(AutoMissingParts(AutoType::Bend, false, map).empty());
    const std::vector<Hit> h = Run(rig, MoveSettings(1));
    CHECK(Count(h, AutoKind::Bend) == 1 && Count(h, AutoKind::Rise) == 1);
}

// A crouch in two steps (0.2 m each): a pause of 0.1 s between them is one move (Crouch Stop at
// the second step's end), a pause over body_move_merge_s two, and a short hop between them (a
// flight: the samples between are not read) two as well.
void TestTwoStep()
{
    auto steps = [](double pause) {
        const Curve a = Ramp(1.0, 0.35, 0.20), b = Ramp(1.35 + pause, 0.35, 0.20);
        return Curve([a, b](double t) { return a(t) + b(t); });
    };
    const std::vector<Hit> one = Run(Rig(steps(0.1), Zero(), 3.0), MoveSettings(2));
    CHECK(Count(one, AutoKind::Crouch) == 1 && Count(one, AutoKind::CrouchStop) == 1);
    const Hit* stop = First(one, AutoKind::CrouchStop);
    if (stop) CHECK(stop->t >= 1.7 && stop->t <= 1.9);  // the second step ends at 1.8 s
    CHECK(Count(Run(Rig(steps(0.4), Zero(), 3.0), MoveSettings(1)), AutoKind::Crouch) == 2);
    RigOpts hop;
    hop.both_lift = Hill(1.36, 1.41, 0.04, 0.06);  // both feet up 6 cm in the 0.1 s pause
    CHECK(Count(Run(Rig(steps(0.1), Zero(), 3.0, hop), MoveSettings(1)), AutoKind::Crouch) == 2);
}

// The lanes: Crouch shows the hips' descent over the feet vs the start speed, shaded over the
// crouch; Bend the trunk's fold rate.
void TestLanes()
{
    const std::vector<BoneTrack> rig = Rig(Hill(1.0, 2.5, 0.6, 0.36), Zero(), 4.0);
    std::vector<AutoAnalysis>    cache;
    const std::vector<Block>     blocks = AutoBlocks(MoveSettings(1));
    DetectAutoEvents(blocks, rig, &cache, false);
    CHECK(!cache.empty());
    if (cache.empty()) return;
    std::string                 why;
    const std::vector<AutoLane> l = AutoBlockLanes(cache[0], blocks[0], &why);
    CHECK(l.size() == 1);
    if (l.size() == 1) {
        CHECK(l[0].unit == "leg/s" && l[0].above && l[0].threshold == PhysicsParams().body_move_start_speed);
        int shaded = 0;
        for (char c : l[0].contact) shaded += c ? 1 : 0;
        CHECK(shaded > 100 && shaded < 200);  // the 0.6 s crouch, 144 samples
    }
    const std::vector<AutoLane> b = AutoBlockLanes(cache[0], blocks[2], &why);
    CHECK(b.size() == 1 && b[0].unit == "deg/s");
}

// A loop: the squat's events stay in the clip, once.
void TestLoop()
{
    const std::vector<BoneTrack> rig = Rig(Hill(1.0, 2.5, 0.6, 0.36), Zero(), 4.0);
    const std::vector<Hit>       h = Run(rig, MoveSettings(2), true);
    CHECK(Count(h, AutoKind::Crouch) == 1 && Count(h, AutoKind::StandUp) == 1 && Count(h, AutoKind::CrouchStop) == 1);
    for (const Hit& x : h) CHECK(x.t >= 0.0 && x.t < 4.0);
}

}  // namespace

int main()
{
    TestWords();
    TestSettings();
    TestOldItem();
    TestSquat();
    TestFold();
    TestIdle();
    TestOneFootPush();
    TestMissing();
    TestShoulderFacing();
    TestTwoStep();
    TestLanes();
    TestLoop();
    if (g_fails) {
        std::printf("body_move_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("body_move_test: all passed\n");
    return 0;
}
