// SPDX-License-Identifier: MIT
//
// Host test of story 10-9c: swings, kicks and punches (src/motion_whoosh.cpp, WhooshEvents) and
// their auto types (Kick in Legs; Swing, Punch in Arms; Peak / Start) on a synthetic standing body.
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
constexpr double kDur = 2.5;
constexpr double kLeg = 0.87;  // up leg -> knee 0.45, knee -> heel 0.42

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

// A point rotated about the X axis (forward = +Z, up = +Y) around `pivot` by `ang` radians
// (positive lifts a point below the pivot forward).
Vec3d AboutX(const Vec3d& p, const Vec3d& pivot, double ang)
{
    const double y = p.y - pivot.y, z = p.z - pivot.z;
    return Vec3d{p.x, pivot.y + y * std::cos(ang) - z * std::sin(ang), pivot.z + y * std::sin(ang) + z * std::cos(ang)};
}

struct Body {
    bool   swing = false;   // left hand: an arc about the shoulder at 0.5 s
    bool   punch = false;   // right hand: a straight thrust at 1.2 s
    bool   punch_down = false;  // 10-9h: the thrust driven down (to the floor) instead of forward
    bool   punch_up = false;    // review 10-9h: the thrust driven up (a climb's reach for a hold)
    bool   kick = false;    // left leg: a straight-leg front kick at 1.6 s
    bool   fold = false;    // right shank: a fast heel kick-up behind (a running stride's fold) at 0.4 s
    bool   pump = false;    // right hand: a running arm pump, the elbow straightening as it swings forward
    double swing_t0 = 0.5;  // the swing's start (s)
    bool   torso = true;    // false: no chest, spine nor hips
};

// The kick's leg angle (rad, about the left up leg) at t: up to 95 deg over 0.25 s, the speed
// peaking late (near the top, as a kick's snap), then down over 0.5 s.
double KickAngle(double t)
{
    const double a = 95.0 * kPi / 180.0, t0 = 1.6;
    if (t < t0) return 0.0;
    if (t < t0 + 0.25) return a * Smoothstep(((t - t0) / 0.25) * ((t - t0) / 0.25));
    return a * (1.0 - Smoothstep((t - t0 - 0.25) / 0.5));
}

std::vector<BoneTrack> MakeBody(const Body& b)
{
    const size_t           n = static_cast<size_t>(kDur * kRate) + 1;
    std::vector<BoneTrack> rt(static_cast<size_t>(Role::Count));
    auto                   put = [&](Role r, const Vec3d& p) { rt[static_cast<size_t>(r)].pos.push_back(p); };
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRate;
        if (b.torso) {
            put(Role::Hips, V(0.0, 0.95, 0.0));
            put(Role::Spine, V(0.0, 1.15, 0.0));
            put(Role::Chest, V(0.0, 1.35, 0.0));
        }
        const Vec3d shl = V(0.2, 1.45, 0.0), shr = V(-0.2, 1.45, 0.0);
        put(Role::LeftShoulder, shl);
        put(Role::RightShoulder, shr);
        // Left hand: hangs 0.6 m under the shoulder; the swing lifts it forward 150 deg in 0.3 s.
        double sw = 0.0;
        if (b.swing) sw = 150.0 * kPi / 180.0 * Smoothstep((t - b.swing_t0) / 0.3);
        put(Role::LeftHand, AboutX(Plus(shl, V(0.0, -0.6, 0.0)), shl, sw));
        // Right hand: guard at the chest, then a straight thrust 0.55 m forward in 0.08 s.
        const Vec3d guard = Plus(shr, V(0.0, -0.1, 0.15));
        if (b.pump) {
            // Review 10-9c: +-60 deg about the shoulder at 2 Hz (8 leg/s at the bottom, a sprint's
            // arms swing hard), the hand 0.35 m from the shoulder behind, 0.6 m in front (the
            // elbow opening as it drives forward).
            const double ph = 2.0 * kPi * 2.0 * t, ang = 60.0 * kPi / 180.0 * std::sin(ph);
            const double reach = 0.475 + 0.125 * std::sin(ph);
            put(Role::RightHand, AboutX(Plus(shr, V(0.0, -reach, 0.0)), shr, ang));
        } else {
            put(Role::RightHand,
                b.punch ? Plus(guard, b.punch_down ? V(0.0, -0.5 * Smoothstep((t - 1.2) / 0.08), 0.2 * Smoothstep((t - 1.2) / 0.08))
                                      : b.punch_up ? V(0.0, 0.6 * Smoothstep((t - 1.2) / 0.08), 0.1 * Smoothstep((t - 1.2) / 0.08))
                                                   : V(0.0, 0.1 * Smoothstep((t - 1.2) / 0.08), 0.55 * Smoothstep((t - 1.2) / 0.08)))
                        : guard);
        }
        for (int s = 0; s < 2; ++s) {
            const double x = s ? -0.1 : 0.1;
            const Vec3d  up = V(x, 0.95, 0.0);
            Vec3d        knee = V(x, 0.5, 0.0), heel = V(x, 0.08, -0.05), ball = V(x, 0.02, 0.09), tip = V(x, 0.0, 0.16);
            if (s == 0 && b.kick) {
                const double a = -KickAngle(t);  // forward
                knee = AboutX(knee, up, a);
                heel = AboutX(heel, up, a);
                ball = AboutX(ball, up, a);
                tip = AboutX(tip, up, a);
            }
            if (s == 1 && b.fold) {
                // The shank folds back 120 deg about the knee in 0.15 s and returns in 0.15 s.
                const double u = t < 0.55 ? Smoothstep((t - 0.4) / 0.15) : 1.0 - Smoothstep((t - 0.55) / 0.15);
                const double a = 120.0 * kPi / 180.0 * u;
                heel = AboutX(heel, knee, a);
                ball = AboutX(ball, knee, a);
                tip = AboutX(tip, knee, a);
            }
            put(s ? Role::RightUpLeg : Role::LeftUpLeg, up);
            put(s ? Role::RightKnee : Role::LeftKnee, knee);
            put(s ? Role::RightHeel : Role::LeftHeel, heel);
            put(s ? Role::RightToe : Role::LeftToe, ball);
            put(s ? Role::RightToeEnd : Role::LeftToeEnd, tip);
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

const PhysicsEvent* Find(const std::vector<PhysicsEvent>& ev, PhysicsKind k, char side)
{
    for (const PhysicsEvent& e : ev)
        if (e.kind == k && e.side == side) return &e;
    return nullptr;
}

int Count(const std::vector<PhysicsEvent>& ev, PhysicsKind k)
{
    return static_cast<int>(std::count_if(ev.begin(), ev.end(), [&](const PhysicsEvent& e) { return e.kind == k; }));
}

void TestPhysics()
{
    Body all;
    all.swing = all.punch = all.kick = all.fold = true;
    const PhysicsParams   p;
    const PhysicsAnalysis a = AnalyseMotion(MakeBody(all), p);
    CHECK(a.ok);
    CHECK(std::fabs(a.leg_length - kLeg) < 0.01);
    CHECK(a.torso_missing.empty());
    for (int s = 0; s < 2; ++s) CHECK(a.whoosh_hand[s].present && a.whoosh_foot[s].present);
    const std::vector<PhysicsEvent> ev = WhooshEvents(a, p, false);
    // One of each: the swing (L hand), the punch (R hand), the kick (L foot); the folded shank
    // whips as fast but is no kick (up leg -> foot under 1 leg).
    CHECK(Count(ev, PhysicsKind::Swing) == 1 && Count(ev, PhysicsKind::Punch) == 1 && Count(ev, PhysicsKind::Kick) == 1);
    const PhysicsEvent* sw = Find(ev, PhysicsKind::Swing, 'L');
    const PhysicsEvent* pu = Find(ev, PhysicsKind::Punch, 'R');
    const PhysicsEvent* ki = Find(ev, PhysicsKind::Kick, 'L');
    CHECK(sw && pu && ki);
    if (sw) {
        // The arc's speed peaks at its middle (0.65 s): 0.6 m x 150 deg x 1.5 / 0.3 s = 7.85 m/s.
        CHECK(std::fabs(sw->time_s - 0.65) < 0.01);
        CHECK(sw->start_s < sw->time_s && sw->end_s > sw->time_s && sw->start_s >= 0.5 && sw->end_s <= 0.8);
        CHECK(std::fabs(sw->strength - 7.85 / kLeg) < 0.6);
        CHECK(sw->part == -1);
    }
    if (pu) {
        CHECK(std::fabs(pu->time_s - 1.24) < 0.01);
        CHECK(pu->start_s >= 1.17 && pu->start_s < pu->time_s && pu->end_s <= 1.31);
    }
    if (ki) {
        CHECK(ki->time_s > 1.75 && ki->time_s < 1.85);  // late in the 0.25 s lift (the snap)
        CHECK(ki->part == static_cast<int>(FootPart::Tip));  // the toe end, furthest from the hip
        CHECK(ki->strength > p.kick_speed);
    }
    // Start: the same events, timed at their start.
    const std::vector<PhysicsEvent> st = WhooshEvents(a, p, true);
    CHECK(st.size() == ev.size());
    if (const PhysicsEvent* s2 = Find(st, PhysicsKind::Swing, 'L'))
        if (sw) CHECK(s2->time_s == sw->start_s && s2->start_s == sw->start_s && s2->strength == sw->strength);
    // The sensitivity's speeds: twice the speed, no event.
    PhysicsParams hard = p;
    hard.swing_speed = hard.punch_speed = hard.kick_speed = 30.0;
    CHECK(WhooshEvents(a, hard, false).empty());
    // A punch needs a thrust: with the radial share at 1.01 every hand peak is a swing.
    PhysicsParams arcs = p;
    arcs.punch_radial_frac = 1.01;
    const std::vector<PhysicsEvent> ar = WhooshEvents(a, arcs, false);
    CHECK(Count(ar, PhysicsKind::Punch) == 0 && Count(ar, PhysicsKind::Swing) == 2);
    // Story 10-9h: a punch is an extension (bent -> straight, along the shoulder -> hand line);
    // story 10-9o: strength = the hand's peak speed relative to the torso (0.55 m in 0.08 s,
    // smoothstep: 1.5 x 0.55 / 0.08 = 10.3 m/s at most).
    if (pu) CHECK(pu->strength > p.punch_speed && pu->strength < 10.4 / kLeg);
    // The arm must end straight: with the end reach above 1, none (the peak is a swing).
    PhysicsParams bent = p;
    bent.punch_end_reach = 1.01;
    CHECK(Count(WhooshEvents(a, bent, false), PhysicsKind::Punch) == 0 && Count(WhooshEvents(a, bent, false), PhysicsKind::Swing) == 2);
    // It must start bent: with the start reach at 0.1 of the arm, none.
    PhysicsParams straight = p;
    straight.punch_start_reach = 0.1;
    CHECK(Count(WhooshEvents(a, straight, false), PhysicsKind::Punch) == 0);
    // The same thrust driven down (an arm thrown to the floor, for support or balance): no punch.
    {
        Body down;
        down.punch = down.punch_down = true;
        const PhysicsAnalysis da = AnalyseMotion(MakeBody(down), p);
        CHECK(da.ok && Count(WhooshEvents(da, p, false), PhysicsKind::Punch) == 0);
        CHECK(da.whoosh_hand[1].rise.back() < p.punch_min_rise);
    }
    // Review 10-9h: the same thrust driven up (a climb's arm reaching for the next hold): no punch,
    // and a punch again with the cap lifted (the rise is the only test it fails; story 10-9o: and
    // the forward share, the hand moving up).
    {
        Body up;
        up.punch = up.punch_up = true;
        const PhysicsAnalysis ua = AnalyseMotion(MakeBody(up), p);
        CHECK(ua.ok && Count(WhooshEvents(ua, p, false), PhysicsKind::Punch) == 0);
        CHECK(ua.whoosh_hand[1].rise.back() > p.punch_max_rise);
        PhysicsParams any_rise = p;
        any_rise.punch_max_rise = 1.0;
        any_rise.punch_min_fwd = -2.0;
        CHECK(Count(WhooshEvents(ua, any_rise, false), PhysicsKind::Punch) == 1);
    }
    // A kick must be raised: with the minimum height at 2 leg, none.
    PhysicsParams low = p;
    low.kick_min_height = 2.0;
    CHECK(Count(WhooshEvents(a, low, false), PhysicsKind::Kick) == 0);
    CHECK(WhooshSummary(a, ev) == "whooshes: kicks 1, swings 1, punches 1");

    // Review 10-9c: a running arm pump driving forward while the elbow opens is fast (a swing),
    // never a punch: its speed along the shoulder -> hand line stays far under the thrust's share
    // (0.7; the fixtures' runs, sprints and strafes: 0.54 at most).
    {
        Body pump;
        pump.pump = true;
        const PhysicsAnalysis pa = AnalyseMotion(MakeBody(pump), p);
        const std::vector<PhysicsEvent> pe = WhooshEvents(pa, p, false);
        CHECK(pa.ok && Count(pe, PhysicsKind::Swing) >= 3 && Count(pe, PhysicsKind::Punch) == 0);
        double share = 0.0;
        for (const PhysicsEvent& e : pe) {
            const size_t i = static_cast<size_t>(std::lround(e.time_s * kRate));
            share = std::max(share, pa.whoosh_hand[1].radial[i] / pa.whoosh_hand[1].speed[i]);
        }
        CHECK(share < 0.5);
        // The pump's arcs at the punch's own sensitivity 100 (half its speed): still no punch.
        PhysicsParams soft = p;
        soft.punch_speed /= 2.0;
        CHECK(Count(WhooshEvents(pa, soft, false), PhysicsKind::Punch) == 0);
    }
    // Review 10-9c: a swing already under way on the clip's first sample (it started before the
    // clip: peak at 0.05 s) is none, Peak or Start.
    {
        Body cut;
        cut.swing = true;
        cut.swing_t0 = -0.1;
        const PhysicsAnalysis ca = AnalyseMotion(MakeBody(cut), p);
        CHECK(ca.ok && ca.whoosh_hand[0].speed[1] > p.whoosh_edge_frac * p.swing_speed);
        CHECK(WhooshEvents(ca, p, false).empty() && WhooshEvents(ca, p, true).empty());
        Body late = cut;
        late.swing_t0 = 0.05;  // its start in the clip
        CHECK(Count(WhooshEvents(AnalyseMotion(MakeBody(late), p), p, false), PhysicsKind::Swing) == 1);
    }

    // Still body: nothing.
    const PhysicsAnalysis still = AnalyseMotion(MakeBody(Body{}), p);
    CHECK(still.ok && WhooshEvents(still, p, false).empty());

    // No torso (chest, spine, hips): no whoosh, said why.
    Body bare = all;
    bare.torso = false;
    const PhysicsAnalysis nt = AnalyseMotion(MakeBody(bare), p);
    CHECK(nt.ok && !nt.torso_missing.empty() && WhooshEvents(nt, p, false).empty());
    CHECK(WhooshSummary(nt, {}).find("missing: torso") != std::string::npos);

    // Looping: the clip read as a cycle finds the same events at the same clip times.
    PhysicsParams lp = p;
    lp.looping = true;
    const PhysicsAnalysis la = AnalyseMotion(MakeBody(all), lp);
    const std::vector<PhysicsEvent> lev = WhooshEvents(la, lp, false);
    CHECK(la.ok && lev.size() == ev.size());
    for (size_t i = 0; i < lev.size() && i < ev.size(); ++i)
        CHECK(lev[i].kind == ev[i].kind && std::fabs(lev[i].time_s - ev[i].time_s) < 1e-6);

    // Dev marker names.
    if (sw) CHECK(PhysicsMarkerName(*sw).rfind("PHY L swing ", 0) == 0);
    if (ki) CHECK(PhysicsMarkerName(*ki).rfind("PHY L kick tip ", 0) == 0);
}

AutoSettings WhooshOn(bool at_start, bool separate_kick)
{
    AutoSettings s;
    for (AutoType t : {AutoType::Kick, AutoType::Swing, AutoType::Punch}) {
        s.type[static_cast<int>(t)].on = true;
        s.at_start[static_cast<int>(t)] = at_start;
    }
    s.separate_parts[static_cast<int>(AutoType::Kick)] = separate_kick;
    return s;
}

std::vector<std::string> Words(const std::vector<Block>& blocks)
{
    std::vector<std::string> out;
    for (const Block& b : blocks) out.push_back(b.auto_type + ":" + std::string(1, b.auto_side));
    return out;
}

void TestTypes()
{
    // Rows: categories, words, markers, the Start twins' sound keys.
    CHECK(AutoCategoryOf(AutoType::Kick) == AutoCategory::Legs && AutoCategoryOf(AutoType::Swing) == AutoCategory::Arms &&
          AutoCategoryOf(AutoType::Punch) == AutoCategory::Arms);
    CHECK(std::string(AutoTypeLabel(AutoType::Kick)) == "Kick" && std::string(AutoTypeLabel(AutoType::Swing)) == "Swing" &&
          std::string(AutoTypeLabel(AutoType::Punch)) == "Punch");
    CHECK(AutoMarkerName(AutoKind::Kick, 'L') == "Kick L" && AutoMarkerName(AutoKind::KickToeStart, 'R') == "Kick Toe R" &&
          AutoMarkerName(AutoKind::SwingStart, 'L') == "Swing L" && AutoMarkerName(AutoKind::Punch, 'R') == "Punch R");
    CHECK(AutoKindRow(AutoKind::SwingStart).sound_key == AutoKind::Swing &&
          AutoKindRow(AutoKind::KickHeelStart).sound_key == AutoKind::KickHeel);
    CHECK(AutoDefaultColor(AutoKind::PunchStart, 'L') == AutoDefaultColor(AutoKind::Punch, 'L'));
    AutoKind k;
    CHECK(AutoKindFromWord("kick_toe_start", &k) && k == AutoKind::KickToeStart);
    CHECK(AutoKindFromWord("punch_start", &k) && k == AutoKind::PunchStart);
    // The sensitivity divides the type's speed.
    CHECK(AutoPhysicsParams(AutoType::Swing, 100.0, false).swing_speed == PhysicsParams{}.swing_speed / 2.0);
    CHECK(AutoPhysicsParams(AutoType::Kick, 0.0, false).kick_speed == PhysicsParams{}.kick_speed * 2.0);

    // Blocks: Peak, Start, Separate kick.
    CHECK((Words(AutoBlocks(WhooshOn(false, false))) ==
           std::vector<std::string>{"kick:L", "kick:R", "swing:L", "swing:R", "punch:L", "punch:R"}));
    CHECK((Words(AutoBlocks(WhooshOn(true, false))) ==
           std::vector<std::string>{"kick_start:L", "kick_start:R", "swing_start:L", "swing_start:R", "punch_start:L",
                                    "punch_start:R"}));
    CHECK((Words(AutoBlocks(WhooshOn(true, true))) ==
           std::vector<std::string>{"kick_heel_start:L", "kick_heel_start:R", "kick_toe_start:L", "kick_toe_start:R",
                                    "swing_start:L", "swing_start:R", "punch_start:L", "punch_start:R"}));
    // Read back, pending.
    for (bool st : {false, true})
        for (bool sep : {false, true}) {
            const AutoSettings s = WhooshOn(st, sep);
            CHECK(ReadAutoSettings(AutoBlocks(s)) == s);
        }
    // Review 10-9c: a project saved before the whooshes (steps, separate jumps, grabs) reads back
    // with no Start anywhere, and re-applying what it reads changes nothing.
    {
        AutoSettings old;
        for (AutoType t : {AutoType::Step, AutoType::Jump, AutoType::Land, AutoType::Grab}) old.type[static_cast<int>(t)].on = true;
        old.separate_parts[static_cast<int>(AutoType::Jump)] = true;
        std::vector<Block>       saved = AutoBlocks(old);
        const std::vector<Block> before = saved;
        const AutoSettings       read = ReadAutoSettings(saved);
        CHECK(read == old);
        // Story 10-9j: each type's default (Peak for the whooshes, Start for the scuffs).
        for (int t = 0; t < kAutoTypeCount; ++t) CHECK(read.at_start[t] == AutoStartDefault(static_cast<AutoType>(t)));
        bool ch = true;
        ApplyAutoSettings(saved, read, &ch);
        CHECK(!ch && Words(saved) == Words(before) && !AutoPending(read, read));
    }
    CHECK(AutoPending(WhooshOn(true, false), WhooshOn(false, false)));
    CHECK(!AutoPending(WhooshOn(false, false), WhooshOn(false, false)));

    // Peak -> Start keeps each block (its marker, colour, rule id, edits): only the kind changes.
    std::vector<Block> blocks = AutoBlocks(WhooshOn(false, false));
    blocks[2].marker = "My Swing";
    blocks[2].rid = "0123456789abcdef";
    bool                   changed = false;
    const std::vector<int> map = ApplyAutoSettings(blocks, WhooshOn(true, false), &changed);
    CHECK(changed && blocks.size() == 6);
    for (size_t i = 0; i < map.size(); ++i) CHECK(map[i] == static_cast<int>(i));
    CHECK(blocks[2].auto_type == "swing_start" && blocks[2].marker == "My Swing" && blocks[2].rid == "0123456789abcdef");
    // Combined -> Separate at the start: the kick's edits go to the heel block of that foot.
    const std::vector<int> map2 = ApplyAutoSettings(blocks, WhooshOn(true, true), &changed);
    CHECK(changed && map2.size() == 6 && map2[0] >= 0);
    if (map2[0] >= 0) CHECK(blocks[static_cast<size_t>(map2[0])].auto_type == "kick_heel_start");

    // Parts: the torso, then the feet or the hands.
    std::vector<int> none(static_cast<size_t>(Role::Count), -1);
    const std::vector<std::string> kp = AutoMissingParts(AutoType::Kick, false, none);
    CHECK(std::find(kp.begin(), kp.end(), RoleName(Role::Chest)) != kp.end());
    const std::vector<std::string> hp = AutoMissingParts(AutoType::Punch, false, none);
    CHECK(std::find(hp.begin(), hp.end(), RoleName(Role::LeftHand)) != hp.end() &&
          std::find(hp.begin(), hp.end(), RoleName(Role::Chest)) != hp.end());
}

void TestDetect()
{
    Body all;
    all.swing = all.punch = all.kick = all.fold = true;
    const std::vector<BoneTrack> rt = MakeBody(all);
    const std::vector<int>       map = RoleMap(rt);
    std::vector<Block>           blocks = AutoBlocks(WhooshOn(false, true));
    std::vector<AutoAnalysis>    cache;
    const std::vector<Event>     ev = DetectAutoEvents(blocks, rt, &cache, false);
    std::vector<std::string>     names;
    for (const Event& e : ev) names.push_back(e.marker);
    CHECK((names == std::vector<std::string>{"Swing L", "Punch R", "Kick Toe L"}));
    for (const Event& e : ev)
        if (e.marker == "Swing L") CHECK(std::fabs(e.time_s - 0.65) < 0.01 && e.strength > 7.0);

    // Start: the swing's marker at its start.
    std::vector<Block>       sb = AutoBlocks(WhooshOn(true, false));
    std::vector<AutoAnalysis> c2;
    for (const Event& e : DetectAutoEvents(sb, rt, &c2, false))
        if (e.marker == "Swing L") CHECK(e.time_s >= 0.5 && e.time_s < 0.6);

    // Detect never raises a silent whoosh block (the right kick, the left punch...).
    std::vector<int> counts(blocks.size(), 0);
    for (const Event& e : ev)
        if (e.block >= 0 && static_cast<size_t>(e.block) < counts.size()) ++counts[static_cast<size_t>(e.block)];
    // Review 10-9c: such a block counts as searched (Detect is not left lit for it).
    const std::vector<AutoSensResult> sr = SearchAutoSensitivity(blocks, rt, counts, map, false);
    CHECK(sr.size() == blocks.size());
    for (size_t i = 0; i < sr.size(); ++i) {
        CHECK(!sr[i].raised && !sr[i].none && sr[i].sens == blocks[i].sens);
        // (a silent kick part whose sibling part holds the kick is left alone, as any part type's)
        if (counts[i] == 0 && blocks[i].auto_type.rfind("kick", 0) != 0) CHECK(sr[i].searched);
    }

    // Lanes: the speed vs torso against the type's speed.
    AutoAnalysis aa;
    aa.params = AutoPhysicsParams(AutoType::Swing, 50.0, false);
    aa.analysis = AnalyseMotion(rt, aa.params);
    const std::vector<AutoLane> lanes = AutoBlockLanes(aa, MakeAutoBlock(AutoKind::Swing, 'L', 50.0, 0.0));
    CHECK(lanes.size() == 1);
    if (!lanes.empty()) CHECK(lanes[0].above && lanes[0].threshold == aa.params.swing_speed && lanes[0].unit == "leg/s");
}

}  // namespace

int main()
{
    TestPhysics();
    TestTypes();
    TestDetect();
    if (g_fails) {
        std::printf("%d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("motion_whoosh: all checks passed\n");
    return 0;
}
