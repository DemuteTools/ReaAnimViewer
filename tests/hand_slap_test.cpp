// SPDX-License-Identifier: MIT
//
// Host test of story 10-9l: slaps (src/hand_slap.h) on a synthetic hand with one finger, and the
// Slap row through Detect (src/auto_detect.h): a slap replaces the grab of the same contact, a
// grab whose fingers close stays a grab, a fist is no slap, a rig without fingers (or whose
// fingers never move) keeps its grabs. No REAPER, no Windows: any C++17 compiler.
//   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests

#include "auto_detect.h"
#include "auto_types.h"
#include "bone_roles.h"
#include "hand_slap.h"
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
constexpr double kLeg = 0.87;  // the rig's leg (m)

double Ease(double u)
{
    u = std::clamp(u, 0.0, 1.0);
    return 0.5 - 0.5 * std::cos(kPi * u);
}

double Ramp(double t, double t0, double t1, double a, double b)
{
    return a + (b - a) * Ease((t - t0) / (t1 - t0));
}

using CurlFn = std::function<double(double)>;  // degrees at time t

// The right hand moves forward and down at 1.5 leg/s and rests over [rest_a, rest_b) (a Grab's
// contact at rest_a); the left hand hangs still. Each hand has one finger: the knuckle 9 cm ahead
// of the wrist along the hand's motion, the 8 cm finger bent down by the curl. Feet planted, a
// 0.87 m leg, the hips and the spine (a body for Hand on body). 240 Hz. `fingers` false: no
// knuckle and fingertip bones.
// `on_thigh`: the right hand instead moves in from the side at 2 leg/s and stops on the right
// thigh at rest_a (a slap on the thigh, Hand on body's touch), resting there until rest_b.
std::vector<BoneTrack> Rig(const CurlFn& curl_r, double rest_a, double rest_b, double dur, bool fingers = true,
                           const CurlFn& curl_l = nullptr, bool on_thigh = false)
{
    std::vector<BoneTrack> t(static_cast<size_t>(Role::Count));
    const double           rate = 240.0;
    const int              n = static_cast<int>(std::floor(dur * rate + 1e-9)) + 1;
    auto put = [&](Role r, const Vec3d& p) {
        BoneTrack& b = t[static_cast<size_t>(r)];
        b.rate_hz = rate;
        b.pos.push_back(p);
    };
    const double speed = 1.5 * kLeg;
    for (int i = 0; i < n; ++i) {
        const double time = i / rate;
        for (int s = 0; s < 2; ++s) {
            const char  side = s ? 'R' : 'L';
            const double x = s ? 0.2 : -0.2;
            const Vec3d heel{x, 0.08, -0.14}, ball{x, 0.02, 0.0}, tip{x, 0.0, 0.07};
            put(FootPartRole(side, 0), heel);
            put(FootPartRole(side, 1), ball);
            put(FootPartRole(side, 2), tip);
            put(s ? Role::RightKnee : Role::LeftKnee, Vec3d{x, 0.08 + 0.42, -0.14});
            put(s ? Role::RightUpLeg : Role::LeftUpLeg, Vec3d{x, 0.08 + 0.87, -0.14});
        }
        put(Role::Hips, Vec3d{0.0, 1.0, -0.14});
        put(Role::Spine, Vec3d{0.0, 1.15, -0.14});
        put(Role::Chest, Vec3d{0.0, 1.35, -0.14});
        // The hands: the right one moving (unit direction (0, -0.6, 0.8)) except at rest.
        const double moving = time - (std::clamp(time, rest_a, rest_b) - rest_a);
        const double d = speed * moving;
        Vec3d        hand[2] = {Vec3d{-0.45, 0.8, 0.0}, Vec3d{0.45, 1.4 - 0.6 * d, 0.8 * d}};
        if (on_thigh) {
            const double run = 2.0 * kLeg * (std::min(time, rest_a) - rest_a);  // <= 0: before the stop
            hand[1] = Vec3d{0.28 - run, 0.72, -0.14};
        }
        for (int s = 0; s < 2; ++s) {
            put(s ? Role::RightHand : Role::LeftHand, hand[s]);
            if (!fingers) continue;
            const CurlFn& c = s ? curl_r : curl_l;
            const double  th = (c ? c(time) : 20.0 + 10.0 * std::sin(time)) * kPi / 180.0;
            const Vec3d   k{hand[s].x, hand[s].y, hand[s].z + 0.09};
            put(s ? Role::RightKnuckle : Role::LeftKnuckle, k);
            put(s ? Role::RightFingertip : Role::LeftFingertip, Vec3d{k.x, k.y - 0.08 * std::sin(th), k.z + 0.08 * std::cos(th)});
        }
    }
    return t;
}

// Open at 40 deg, opening to 10 by 0.8 s and staying there (a slap at 1.0 s).
double CurlSlap(double t)
{
    return Ramp(t, 0.5, 0.8, 40.0, 10.0);
}
// Open at 10 at the contact (1.0 s), closing to 80 over 1.03-1.2 s (a grab).
double CurlGrab(double t)
{
    return Ramp(t, 1.03, 1.2, 10.0, 80.0) + Ramp(t, 0.2, 0.5, 20.0, 0.0);
}
// A fist, clenched a little harder at the contact.
double CurlFist(double t)
{
    return Ramp(t, 0.9, 1.0, 150.0, 165.0);
}

int Count(const std::vector<Event>& ev, const std::string& marker)
{
    int n = 0;
    for (const Event& e : ev)
        if (e.marker == marker) ++n;
    return n;
}

std::vector<Event> Detect(const std::vector<BoneTrack>& rt, bool grab, bool slap, bool body = false)
{
    AutoSettings s;
    s.type[static_cast<int>(AutoType::Grab)].on = grab;
    s.type[static_cast<int>(AutoType::Slap)].on = slap;
    s.type[static_cast<int>(AutoType::HandOnBody)].on = body;
    return DetectAutoEvents(AutoBlocks(s), rt, nullptr, false);
}

// The finger curl: the angle the rig bends the finger by; empty without the finger bones.
void TestCurl()
{
    const PhysicsAnalysis a = AnalyseMotion(Rig(&CurlGrab, 1.0, 1.6, 2.5));
    CHECK(a.ok);
    const std::vector<double> c = FingerCurlDeg(a, 'R');
    CHECK(c.size() == a.samples);
    if (c.size() == a.samples) {
        CHECK(std::fabs(c[static_cast<size_t>(0.8 * 240)] - 10.0) < 1.0);
        CHECK(std::fabs(c[static_cast<size_t>(1.5 * 240)] - 80.0) < 1.0);
    }
    CHECK(FingerCurlDeg(AnalyseMotion(Rig(&CurlGrab, 1.0, 1.6, 2.5, false)), 'R').empty());
    CHECK(FingerCurlDeg(PhysicsAnalysis{}, 'L').empty());
}

// SlapAt on each hand shape, with its reason.
void TestSlapAt()
{
    std::string why;
    const PhysicsAnalysis slap = AnalyseMotion(Rig(&CurlSlap, 1.0, 1.6, 2.5));
    const std::vector<PhysicsEvent> hands = HandEvents(slap);
    double grab_t = -1.0;
    for (const PhysicsEvent& e : hands)
        if (e.kind == PhysicsKind::Grab && e.side == 'R') grab_t = e.time_s;
    CHECK(std::fabs(grab_t - 1.0) < 0.03);
    CHECK(SlapAt(slap, 'R', grab_t, &why) && why.empty());
    const std::vector<PhysicsEvent> sc = SlapContactEvents(slap);
    CHECK(sc.size() == 1);
    if (sc.size() == 1)
        CHECK(sc[0].kind == PhysicsKind::Grab && sc[0].side == 'R' && sc[0].time_s == grab_t &&
              sc[0].start_s == grab_t && sc[0].end_s == grab_t && sc[0].strength > 1.3);

    const PhysicsAnalysis grab = AnalyseMotion(Rig(&CurlGrab, 1.0, 1.6, 2.5));
    CHECK(!SlapAt(grab, 'R', 1.0, &why) && why == "the fingers close after the contact");
    CHECK(SlapContactEvents(grab).empty());

    const PhysicsAnalysis fist = AnalyseMotion(Rig(&CurlFist, 1.0, 1.6, 2.5));
    CHECK(!SlapAt(fist, 'R', 1.0, &why) && why == "the fingers are bent at the contact");

    // Fingers that never move (frozen open): no slap.
    const PhysicsAnalysis still = AnalyseMotion(Rig([](double) { return 10.0; }, 1.0, 1.6, 2.5));
    CHECK(!SlapAt(still, 'R', 1.0, &why) && why == "the fingers do not move in this clip");

    CHECK(!SlapAt(AnalyseMotion(Rig(&CurlSlap, 1.0, 1.6, 2.5, false)), 'R', 1.0, &why) && why == "no finger bones");

    // A contact closer to the end of the clip than kSlapAfterS: not seen after it, no slap.
    const PhysicsAnalysis late = AnalyseMotion(Rig(&CurlSlap, 1.0, 1.6, 1.15));
    CHECK(!SlapAt(late, 'R', 1.0, &why) && why == "the clip ends before the fingers can be seen after the contact");
}

// Through Detect: a slap replaces the grab of its contact, only with both ticked; a grab stays.
void TestDetect()
{
    const std::vector<BoneTrack> slap = Rig(&CurlSlap, 1.0, 1.6, 2.5);
    CHECK(Count(Detect(slap, true, false), "Grab R") == 1);
    const std::vector<Event> both = Detect(slap, true, true);
    CHECK(Count(both, "Slap R") == 1 && Count(both, "Grab R") == 0 && Count(both, "Slap L") == 0);
    CHECK(Count(Detect(slap, false, true), "Slap R") == 1);
    for (const Event& e : both)
        if (e.marker == "Slap R") CHECK(std::fabs(e.time_s - 1.0) < 0.03 && e.strength > 1.3 && e.speed > 0.0);

    const std::vector<BoneTrack> grab = Rig(&CurlGrab, 1.0, 1.6, 2.5);
    const std::vector<Event>     g = Detect(grab, true, true);
    CHECK(Count(g, "Grab R") == 1 && Count(g, "Slap R") == 0);

    // No finger bones: Slap finds nothing (its parts are missing), Grab is unchanged.
    const std::vector<BoneTrack> bare = Rig(&CurlSlap, 1.0, 1.6, 2.5, false);
    const std::vector<Event>     nb = Detect(bare, true, true);
    CHECK(Count(nb, "Grab R") == 1 && Count(nb, "Slap R") == 0);
    std::vector<int> map(static_cast<size_t>(Role::Count), -1);
    for (int r = 0; r < static_cast<int>(Role::Count); ++r) map[static_cast<size_t>(r)] = r;
    CHECK(!AutoBlockPartMissing(MakeAutoBlock(AutoKind::Slap, 'R', kAutoDefaultSens, 0.0), map));
    CHECK(AutoMissingParts(AutoType::Slap, false, map).empty());
    map[static_cast<size_t>(Role::RightFingertip)] = -1;
    CHECK(AutoBlockPartMissing(MakeAutoBlock(AutoKind::Slap, 'R', kAutoDefaultSens, 0.0), map));
    CHECK(!AutoBlockPartMissing(MakeAutoBlock(AutoKind::Slap, 'L', kAutoDefaultSens, 0.0), map));
    CHECK(!AutoBlockPartMissing(MakeAutoBlock(AutoKind::Grab, 'R', kAutoDefaultSens, 0.0), map));
    const std::vector<std::string> miss = AutoMissingParts(AutoType::Slap, false, map);
    CHECK(miss.size() == 1 && miss[0] == RoleName(Role::RightFingertip));
}

// A slap on the thigh: Hand on body's touch with the fingers open is a Slap (its "on body"
// signature), and replaces that Hand on body; a touch whose fingers close stays Hand on body.
void TestBody()
{
    const std::vector<BoneTrack> slap = Rig(&CurlSlap, 1.0, 2.0, 2.5, true, nullptr, true);
    const std::vector<Event>     body = Detect(slap, false, false, true);
    CHECK(Count(body, "Hand on body R") == 1);
    const std::vector<Event> both = Detect(slap, false, true, true);
    CHECK(Count(both, "Slap R") == 1 && Count(both, "Hand on body R") == 0);
    CHECK(SlapBodyEvents(AnalyseMotion(slap)).size() == 1);
    for (const PhysicsEvent& e : SlapBodyEvents(AnalyseMotion(slap))) CHECK(e.kind == PhysicsKind::HandOnBody);
    const std::vector<BoneTrack> hold = Rig(&CurlGrab, 1.0, 2.0, 2.5, true, nullptr, true);
    const std::vector<Event>     h = Detect(hold, false, true, true);
    CHECK(Count(h, "Hand on body R") == 1 && Count(h, "Slap R") == 0);
}

// The lane: the curl against kSlapOpenMaxDeg, shaded over the hand's rest; a reason without
// fingers, or with fingers that never move.
void TestLanes()
{
    AutoAnalysis aa;
    aa.params = AutoPhysicsParams(AutoType::Slap, kAutoDefaultSens, false);
    aa.analysis = AnalyseMotion(Rig(&CurlSlap, 1.0, 1.6, 2.5), aa.params);
    std::string                 why;
    const std::vector<AutoLane> lanes = AutoBlockLanes(aa, MakeAutoBlock(AutoKind::Slap, 'R', kAutoDefaultSens, 0.0), &why);
    CHECK(lanes.size() == 1 && why.empty());
    if (lanes.size() == 1) {
        CHECK(!lanes[0].above && lanes[0].threshold == kSlapOpenMaxDeg && lanes[0].unit == "deg");
        CHECK(lanes[0].curve.size() == lanes[0].contact.size() && !lanes[0].curve.empty());
        CHECK(std::count(lanes[0].contact.begin(), lanes[0].contact.end(), 1) > 0);
    }
    aa.analysis = AnalyseMotion(Rig([](double) { return 10.0; }, 1.0, 1.6, 2.5), aa.params);
    CHECK(AutoBlockLanes(aa, MakeAutoBlock(AutoKind::Slap, 'R', kAutoDefaultSens, 0.0), &why).empty() &&
          why.find("do not move") != std::string::npos);
}

// The rows: Arms, L / R, after Heel Tap; replaces its hand's Grab and Hand on body; never raised.
void TestRows()
{
    const AutoTypeInfo& t = AutoTypeRow(AutoType::Slap);
    CHECK(t.type == AutoType::Slap && std::string(t.label) == "Slap" && t.category == AutoCategory::Arms);
    CHECK(std::string(t.sound_word) == "slap" && !t.sideless && t.no_sens_raise && !t.start_option);
    CHECK(t.replaces == static_cast<int>(AutoType::Grab) && t.replaces_in_span == static_cast<int>(AutoType::HandOnBody) &&
          t.replaces_own_side);
    CHECK(std::string(t.hint).find("finger") != std::string::npos);
    const AutoKindInfo& k = AutoKindRow(AutoKind::Slap);
    CHECK(std::string(k.word) == "slap" && std::string(k.marker) == "Slap" && std::string(k.role_key) == "hand");
    CHECK(AutoSignatureCount(k) == 2 && AutoMarkerName(AutoKind::Slap, 'L') == "Slap L");
    CHECK(AutoKindsOf(AutoType::Slap) == std::vector<AutoKind>{AutoKind::Slap});
    CHECK((k.roles('R') == std::vector<Role>{Role::RightHand, Role::RightKnuckle, Role::RightFingertip}));
    CHECK(AutoStrengthLevel(AutoType::Slap, 1.0) == kStrengthSoft);
    CHECK(AutoStrengthLevel(AutoType::Slap, 2.0) == kStrengthMedium);
    CHECK(AutoStrengthLevel(AutoType::Slap, 8.0) == kStrengthHard);
}

// The finger roles: guessed on Mixamo (the middle finger, else the index) and Unreal names.
void TestRoles()
{
    const std::vector<int> mx = GuessRoleMapping({"mixamorig:LeftHand", "mixamorig:LeftHandMiddle1", "mixamorig:LeftHandMiddle4",
                                                  "mixamorig:RightHandIndex1", "mixamorig:RightHandIndex4"});
    CHECK(mx[static_cast<size_t>(Role::LeftKnuckle)] == 1 && mx[static_cast<size_t>(Role::LeftFingertip)] == 2);
    CHECK(mx[static_cast<size_t>(Role::RightKnuckle)] == 3 && mx[static_cast<size_t>(Role::RightFingertip)] == 4);
    const std::vector<int> ue = GuessRoleMapping({"hand_r", "middle_01_r", "middle_02_r", "middle_03_r"});
    CHECK(ue[static_cast<size_t>(Role::RightKnuckle)] == 1 && ue[static_cast<size_t>(Role::RightFingertip)] == 3);
    CHECK(ue[static_cast<size_t>(Role::LeftKnuckle)] == -1);
    CHECK(RoleGroupOf(Role::LeftFingertip) == RoleGroup::Arms && RoleSide(Role::RightKnuckle) == 'R');
    CHECK(std::string(RoleKey(Role::LeftKnuckle)) == "left_knuckle" && RoleShortLabel(Role::RightFingertip) == "R fingertip");
    const std::vector<Role>& phys = PhysicsRoles();
    for (Role r : {Role::LeftKnuckle, Role::RightKnuckle, Role::LeftFingertip, Role::RightFingertip})
        CHECK(std::find(phys.begin(), phys.end(), r) != phys.end());
}

}  // namespace

int main()
{
    TestCurl();
    TestSlapAt();
    TestDetect();
    TestBody();
    TestLanes();
    TestRows();
    TestRoles();
    if (g_fails) {
        std::printf("hand_slap_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("hand_slap_test: all passed\n");
    return 0;
}
