// SPDX-License-Identifier: MIT
//
// Story 10-9l: the Slap type (Arms, L / R): a flat hand hitting something, the fingers staying
// extended. Its physics (hand_slap.h: the finger curl, the slap test, the event sources) and the
// rows' functions of auto_types.cpp (event sources, matches, required parts, lanes, roles).
//
// Two signatures: "contact" (Grab's contacts: the hand landing on the floor, a wall, an object)
// and "on body" (Hand on body's touches: a slap on the thigh, the chest). With Grab / Hand on body
// ticked, a slap replaces that hand's grab / hand on body at the same contact (its type row's
// `replaces`, `replaces_in_span`, `replaces_own_side`): one motion, one sound, the more specific
// one. A grab or a hand on body whose fingers close after the contact, or that come with a closed
// hand, stays what it was.

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "auto_types.h"
#include "hand_slap.h"
#include "hand_touch.h"

namespace rav {
namespace {

Role SlapHandRole(char side)
{
    return side == 'R' ? Role::RightHand : Role::LeftHand;
}
Role SlapKnuckleRole(char side)
{
    return side == 'R' ? Role::RightKnuckle : Role::LeftKnuckle;
}
Role SlapTipRole(char side)
{
    return side == 'R' ? Role::RightFingertip : Role::LeftFingertip;
}

const std::vector<Vec3d>* SlapPos(const PhysicsAnalysis& a, Role r)
{
    const size_t i = static_cast<size_t>(r);
    if (i >= a.role_pos.size() || a.role_pos[i].size() != a.samples || a.samples == 0) return nullptr;
    return &a.role_pos[i];
}

// A clip time's analysed sample (looping: the middle cycle), clamped to the track.
long SlapSampleOf(const PhysicsAnalysis& a, double clip_t)
{
    const double base = a.looping ? static_cast<double>(a.window_first) : 0.0;
    const long   i = std::lround(base + clip_t * a.rate_hz);
    return std::clamp(i, 0L, static_cast<long>(a.samples) - 1);
}

// The events of `src` that are slaps, their span reduced to their time.
std::vector<PhysicsEvent> KeepSlaps(const PhysicsAnalysis& a, std::vector<PhysicsEvent> src, PhysicsKind kind)
{
    std::vector<PhysicsEvent> out;
    for (PhysicsEvent& e : src) {
        if (e.kind != kind || !SlapAt(a, e.side, e.time_s)) continue;
        e.start_s = e.end_s = e.time_s;
        out.push_back(e);
    }
    return out;
}

}  // namespace

// ---- Physics -----------------------------------------------------------------------------------

std::vector<double> FingerCurlDeg(const PhysicsAnalysis& a, char side)
{
    if (!a.ok) return {};
    const std::vector<Vec3d>* h = SlapPos(a, SlapHandRole(side));
    const std::vector<Vec3d>* k = SlapPos(a, SlapKnuckleRole(side));
    const std::vector<Vec3d>* t = SlapPos(a, SlapTipRole(side));
    if (!h || !k || !t) return {};
    std::vector<double> out(a.samples, 0.0);
    double              prev = 0.0;
    for (size_t i = 0; i < a.samples; ++i) {
        const Vec3d  u{(*k)[i].x - (*h)[i].x, (*k)[i].y - (*h)[i].y, (*k)[i].z - (*h)[i].z};
        const Vec3d  v{(*t)[i].x - (*k)[i].x, (*t)[i].y - (*k)[i].y, (*t)[i].z - (*k)[i].z};
        const double nu = std::sqrt(u.x * u.x + u.y * u.y + u.z * u.z);
        const double nv = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (nu > 1e-9 && nv > 1e-9) {
            const double c = std::clamp((u.x * v.x + u.y * v.y + u.z * v.z) / (nu * nv), -1.0, 1.0);
            prev = std::acos(c) * 180.0 / 3.14159265358979323846;
        }
        out[i] = prev;  // a degenerate sample (two bones on one point) holds the previous curl
    }
    return out;
}

bool SlapAt(const PhysicsAnalysis& a, char side, double t, std::string* why)
{
    auto no = [&](const char* w) {
        if (why) *why = w;
        return false;
    };
    const std::vector<double> curl = FingerCurlDeg(a, side);
    if (curl.empty()) return no("no finger bones");
    const auto mm = std::minmax_element(curl.begin(), curl.end());
    if (*mm.second - *mm.first < kSlapMovingDeg) return no("the fingers do not move in this clip");
    const long i = SlapSampleOf(a, t);
    const long after = std::lround(kSlapAfterS * a.rate_hz);
    if (i + after >= static_cast<long>(a.samples)) return no("the clip ends before the fingers can be seen after the contact");
    const double at = curl[static_cast<size_t>(i)];
    if (at > kSlapOpenMaxDeg + 1e-9) return no("the fingers are bent at the contact");
    double hi = at;
    for (long j = i; j <= i + after; ++j) hi = std::max(hi, curl[static_cast<size_t>(j)]);
    if (hi - at > kSlapCloseRiseDeg + 1e-9) return no("the fingers close after the contact");
    if (why) why->clear();
    return true;
}

std::vector<PhysicsEvent> SlapContactEvents(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return KeepSlaps(a, HandEvents(a, p), PhysicsKind::Grab);
}

std::vector<PhysicsEvent> SlapBodyEvents(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return KeepSlaps(a, HandOnBodyEvents(a, p), PhysicsKind::HandOnBody);
}

// ---- The rows' functions -----------------------------------------------------------------------

std::vector<PhysicsEvent> AutoSourceSlapContact(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return SlapContactEvents(a, p);
}

std::vector<PhysicsEvent> AutoSourceSlapBody(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return SlapBodyEvents(a, p);
}

// The hands and their fingers (knuckle and fingertip).
void AutoPartsSlap(bool, const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    for (char side : {'L', 'R'})
        for (Role r : {SlapHandRole(side), SlapKnuckleRole(side), SlapTipRole(side)})
            if (!AutoHasRole(role_to_bone, r)) AutoPushOnce(out, RoleName(r));
}

bool AutoMatchSlapContact(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Grab;
}

bool AutoMatchSlapBody(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::HandOnBody;
}

// The hand and its finger. (The body signature also needs a body segment; without one it finds
// nothing, the contact signature still runs.)
bool AutoMissingSlap(char side, const std::vector<int>& role_to_bone)
{
    for (Role r : {SlapHandRole(side), SlapKnuckleRole(side), SlapTipRole(side)})
        if (!AutoHasRole(role_to_bone, r)) return true;
    return false;
}

// The finger curl (deg) vs kSlapOpenMaxDeg, shaded where the hand rests (Grab's contacts).
void AutoLanesSlap(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const HandTrack&          ht = args.Hand();
    const std::vector<double> curl = FingerCurlDeg(*args.a, args.side);
    if (!ht.part.present) {
        reason = ht.part.missing;
        return;
    }
    if (curl.empty()) {
        reason = "no finger bones (knuckle and fingertip): a slap cannot be told from a grab";
        return;
    }
    const auto mm = std::minmax_element(curl.begin(), curl.end());
    if (*mm.second - *mm.first < kSlapMovingDeg) {
        reason = "the fingers do not move in this clip: a slap cannot be told from a grab";
        return;
    }
    AutoLane l;
    l.label = "Fingers " + std::string(1, args.side) + "  curl";
    l.unit = "deg";
    l.curve = AutoLaneWindow(curl, args.first, args.n, false);
    l.contact.assign(args.n, 0);
    for (size_t i = 0; i < args.n && args.first + i < ht.part.contact.size(); ++i) l.contact[i] = ht.part.contact[args.first + i];
    l.threshold = kSlapOpenMaxDeg;
    l.above = false;
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "Shaded: the hand rests (a contact). A slap: the curl under the line at the contact, rising "
                  "less than %.0f deg over the next %.2f s.",
                  kSlapCloseRiseDeg, kSlapAfterS);
    l.shade_note = buf;
    out.push_back(std::move(l));
}

// The hand, then its finger.
std::vector<Role> AutoRolesSlap(char side)
{
    return {SlapHandRole(side), SlapKnuckleRole(side), SlapTipRole(side)};
}

}  // namespace rav
