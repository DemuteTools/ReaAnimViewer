// SPDX-License-Identifier: MIT
//
// Story 10-9g: the Jolt type (Body, sideless): an impact that jolts the body, for gear and cloth
// layers. Its physics (JoltAccelerationG, JoltEvents: declared in motion_physics.h) and the rows'
// functions of auto_types.cpp (event source, match, required parts, lanes, roles).
//
// The impacts it reads are the other types' events (their own detection, at the default
// constants): the foot steps (FootEvents' combined FootStep, either foot) and the jumps' landings
// (JumpEvents' Land), and the body's contacts with the ground (story 10-9b BodyContactEvents: a
// knee coming down, the body sitting down, a fall landing). Each impact's jolt is the centre of
// mass's acceleration peak around it (the hips standing for the centre of mass, as the jumps'
// push-off does).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#include "auto_types.h"

namespace rav {
namespace {

constexpr char kSides[2] = {'L', 'R'};

// ---- Impact sources ----------------------------------------------------------------------------
// One impact = a clip time. Each source lists the impacts of one other type's events, at that
// type's default constants (only the Jolt constants of `p` differ from them).
// The merge, the window and the threshold apply to every source alike.

using ImpactSourceFn = void (*)(const PhysicsAnalysis& a, const PhysicsParams& p, std::vector<double>& out);

// The foot steps (combined, the first part to touch), either foot.
void FootStepImpacts(const PhysicsAnalysis& a, const PhysicsParams& p, std::vector<double>& out)
{
    for (const PhysicsEvent& e : FootEvents(a, p))
        if (e.kind == PhysicsKind::FootStep) out.push_back(e.time_s);
}

// The jumps' landings (a landing is timed as a step: usually the same impact, merged).
void LandImpacts(const PhysicsAnalysis& a, const PhysicsParams& p, std::vector<double>& out)
{
    for (const PhysicsEvent& e : JumpEvents(a, p))
        if (e.kind == PhysicsKind::Land) out.push_back(e.time_s);
}

// Story 10-9b: the body's contacts with the ground: a knee coming down, the body sitting down, a
// fall landing on the hips or the back (BodyContactEvents' Knee, Sit, Fall; an elbow resting is
// no impact). They find the floor from the feet, which Jolt needs already (AutoMissingJolt).
void BodyContactImpacts(const PhysicsAnalysis& a, const PhysicsParams& p, std::vector<double>& out)
{
    for (const PhysicsEvent& e : BodyContactEvents(a, p))
        if (e.kind == PhysicsKind::Knee || e.kind == PhysicsKind::Sit || e.kind == PhysicsKind::Fall)
            out.push_back(e.time_s);
}

constexpr ImpactSourceFn kImpactSources[] = {&FootStepImpacts, &LandImpacts, &BodyContactImpacts};

// A clip time's analysed sample (looping: the middle cycle), clamped to the track.
long SampleOf(const PhysicsAnalysis& a, double clip_t)
{
    const double base = a.looping ? static_cast<double>(a.window_first) : 0.0;
    const long   i = std::lround(base + clip_t * a.rate_hz);
    return std::clamp(i, 0L, static_cast<long>(a.samples) - 1);
}

}  // namespace

// ---- Physics -----------------------------------------------------------------------------------

std::vector<double> JoltAccelerationG(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    // The push-off's upward acceleration (story 10-9a-2), on the jolt's own smoothing: a contact's
    // jolt lasts 50-150 ms, a push 150-300 ms. Downward (a fall) is no jolt: 0.
    PhysicsParams q = p;
    q.jump_impulse_smooth_s = p.jolt_smooth_s;
    std::vector<double> acc = HipsUpAccelerationG(a, q);
    for (double& v : acc) v = std::max(0.0, v);
    return acc;
}

std::vector<PhysicsEvent> JoltEvents(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    std::vector<PhysicsEvent> ev;
    const std::vector<double> acc = JoltAccelerationG(a, p);
    if (acc.empty()) return ev;
    std::vector<double> impacts;
    for (ImpactSourceFn src : kImpactSources) src(a, p, impacts);
    std::sort(impacts.begin(), impacts.end());
    const long before = std::lround(p.jolt_before_s * a.rate_hz), after = std::lround(p.jolt_after_s * a.rate_hz);
    // One impact's peak (g) over its window.
    auto peak_at = [&](double t) {
        const long i = SampleOf(a, t);
        const long lo = std::max(0L, i - before), hi = std::min(static_cast<long>(a.samples) - 1, i + after);
        double     pk = 0.0;
        for (long k = lo; k <= hi; ++k) pk = std::max(pk, acc[static_cast<size_t>(k)]);
        return pk;
    };
    // Impacts within jolt_merge_s of the group's first make one jolt (the first's time, the highest
    // peak): a landing and its step, both feet landing together.
    std::vector<std::pair<double, double>> groups;  // (time, peak)
    for (size_t i = 0; i < impacts.size();) {
        const double t0 = impacts[i];
        double       pk = 0.0;
        for (; i < impacts.size() && impacts[i] - t0 <= p.jolt_merge_s + 1e-9; ++i) pk = std::max(pk, peak_at(impacts[i]));
        groups.push_back({t0, pk});
    }
    // A loop: the last group and the first merge through the seam (period T) when the first comes
    // within jolt_merge_s of the last; the last one is the earlier in the motion and gives the time.
    if (a.looping && a.window_samples > 1 && groups.size() > 1) {
        const double T = static_cast<double>(a.window_samples - 1) / a.rate_hz;
        if (groups.front().first + T - groups.back().first <= p.jolt_merge_s + 1e-9) {
            groups.back().second = std::max(groups.back().second, groups.front().second);
            groups.erase(groups.begin());
        }
    }
    for (const auto& g : groups) {
        if (!(g.second >= p.jolt_min_g)) continue;
        PhysicsEvent e;
        e.kind = PhysicsKind::Jolt;
        e.side = 'B';
        e.part = -1;
        e.time_s = e.start_s = e.end_s = g.first;
        e.strength = g.second;
        ev.push_back(e);
    }
    return ev;
}

// ---- The rows' functions -----------------------------------------------------------------------

std::vector<PhysicsEvent> AutoSourceJolt(const PhysicsAnalysis& a, const PhysicsParams& p)
{
    return JoltEvents(a, p);
}

// The hips (the centre of mass), and a heel, toe or toe end on each foot (all listed for a foot
// with none): the steps' parts.
void AutoPartsJolt(bool, const std::vector<int>& role_to_bone, std::vector<std::string>& out)
{
    if (!AutoHasRole(role_to_bone, Role::Hips)) AutoPushOnce(out, RoleName(Role::Hips));
    for (char side : kSides) {
        const Role heel = FootPartRole(side, 0), ball = FootPartRole(side, 1), tip = FootPartRole(side, 2);
        if (AutoHasRole(role_to_bone, heel) || AutoHasRole(role_to_bone, ball) || AutoHasRole(role_to_bone, tip)) continue;
        AutoPushOnce(out, RoleName(heel));
        AutoPushOnce(out, RoleName(ball));
        AutoPushOnce(out, RoleName(tip));
    }
}

bool AutoMatchJolt(const PhysicsEvent& e, const PhysicsAnalysis&, char)
{
    return e.kind == PhysicsKind::Jolt;
}

// Missing, as AutoPartsJolt lists: no hips, or a foot with none of its parts.
bool AutoMissingJolt(char, const std::vector<int>& role_to_bone)
{
    if (!AutoHasRole(role_to_bone, Role::Hips)) return true;
    for (char side : kSides) {
        bool any = false;
        for (int q = 0; q < kFootPartCount; ++q) any = any || AutoHasRole(role_to_bone, FootPartRole(side, q));
        if (!any) return true;
    }
    return false;
}

// The centre of mass's acceleration (g) against the threshold, shaded where a foot is on the
// ground.
void AutoLanesJolt(const AutoLaneArgs& args, std::vector<AutoLane>& out, std::string& reason)
{
    const PhysicsAnalysis&    a = *args.a;
    const std::vector<double> acc = JoltAccelerationG(a, *args.p);
    if (acc.empty()) {
        reason = a.hips_missing.empty() ? std::string("jolt needs the hips") : a.hips_missing;
        return;
    }
    AutoLane l;
    l.label = "Hips  acceleration";
    l.unit = "g";
    l.curve = AutoLaneWindow(acc, args.first, args.n, false);
    l.contact.assign(args.n, 0);
    for (size_t i = 0; i < args.n; ++i) {
        const size_t j = args.first + i;
        bool         on = false;
        for (const FootTrack& f : a.foot)
            for (const PartTrack& pt : f.part) on = on || (pt.present && j < pt.contact.size() && pt.contact[j]);
        l.contact[i] = on ? 1 : 0;
    }
    l.threshold = args.p->jolt_min_g;
    l.above = true;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "Shaded: a foot is on the ground. A jolt is a step, a landing, a knee, a sit or a\n"
                  "fall where this curve peaks above the line within %.0f ms before to %.0f ms after\n"
                  "the contact.",
                  args.p->jolt_before_s * 1000.0, args.p->jolt_after_s * 1000.0);
    l.shade_note = buf;
    out.push_back(std::move(l));
}

// The hips and both feet's parts.
std::vector<Role> AutoRolesJolt(char)
{
    std::vector<Role> out = {Role::Hips};
    for (char side : kSides)
        for (int q = 0; q < kFootPartCount; ++q) out.push_back(FootPartRole(side, q));
    return out;
}

}  // namespace rav
