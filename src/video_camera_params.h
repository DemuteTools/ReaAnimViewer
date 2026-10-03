// SPDX-License-Identifier: MIT
//
// The six camera parameters of the RAV video FX (Epic 11, Story 11-3), shared by
// both binaries: rav_video_fx.clap declares them as CLAP parameters (names, defaults,
// text), reaper_animviewer.dll decodes them into the video camera (video_camera.h)
// and writes them as envelope points (video_shots.h).
//
// Every parameter is declared 0..1 to REAPER, so a parameter value, an envelope value
// and a parmlist value are the same number. The meaning of that number:
//   Yaw       (v - 0.5) x 360 deg      0.5 = front (yaw 0), the wrap sits at the back
//   Pitch     (v - 0.5) x 178 deg      +/-89 deg, 0.5 = level
//   Distance  0.05 x 400^v radii      0.05 .. 20 radii (the viewer's zoom band), log scale
//   Target X/Y/Z  (v - 0.5) x 6 radii  offset from the posed bounds centre, +/-3 radii
// "radius" and "centre" are the viewer Recenter's: half the diagonal and the centre of
// the posed bounds (OrbitCamera::Reset, camera.h). The defaults reproduce Recenter.
// Known limit: REAPER interpolates the 0..1 yaw value in a straight line, so a Move
// between yaws on either side of the back (e.g. 170 to -170 deg) turns the long way
// round (340 deg), never across the wrap.
//
// Header-only and dependency-free (no glm, REAPER or CLAP): the CLAP target compiles
// one source file. Changing a mapping changes the picture of every saved project.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace rav {
namespace vcam {

enum Param : int { kYaw = 0, kPitch, kDistance, kTargetX, kTargetY, kTargetZ, kParamCount };

// One more FX parameter after the camera, NOT a camera value (never a shot, never an
// envelope RAV writes): RAV bumps it when something REAPER cannot see changes the picture
// (light, floor, output size, background), because REAPER only re-asks for frames it has
// cached when the time or a parameter changes (spike finding 6). The picture ignores it.
constexpr int kRefreshParam  = kParamCount;
constexpr int kFxParamCount  = kParamCount + 1;

constexpr double kPi          = 3.14159265358979323846;
constexpr double kPitchLimit  = 89.0 * kPi / 180.0;  // radians
constexpr double kDistMin     = 0.05;                // radii
constexpr double kDistMax     = 20.0;                // radii
constexpr double kTargetRange = 3.0;                 // +/- radii

// Recenter framing (camera.h kInitYaw / kInitPitch, distance = diag x 1.5 = 3 radii).
constexpr double kDefaultYawRad   = 0.6;
constexpr double kDefaultPitchRad = 0.3;
constexpr double kDefaultDistance = 3.0;  // radii

inline const char* ParamName(int p)
{
    switch (p) {
        case kYaw:      return "Yaw";
        case kPitch:    return "Pitch";
        case kDistance: return "Distance";
        case kTargetX:  return "Target X";
        case kTargetY:  return "Target Y";
        case kTargetZ:  return "Target Z";
        default:        return "";
    }
}

inline double Clamp01(double v)
{
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

// ---- normalized <-> physical (radians, radii) ---------------------------------------
inline double YawFromNorm(double v) { return (v - 0.5) * 2.0 * kPi; }

inline double NormFromYaw(double rad)
{
    if (!std::isfinite(rad)) rad = kDefaultYawRad;
    const double wrapped = std::remainder(rad, 2.0 * kPi);  // [-pi, pi]
    return Clamp01(wrapped / (2.0 * kPi) + 0.5);
}

inline double PitchFromNorm(double v) { return (v - 0.5) * 2.0 * kPitchLimit; }

inline double NormFromPitch(double rad)
{
    if (!std::isfinite(rad)) rad = kDefaultPitchRad;
    return Clamp01(rad / (2.0 * kPitchLimit) + 0.5);
}

inline double DistanceFromNorm(double v) { return kDistMin * std::pow(kDistMax / kDistMin, v); }

inline double NormFromDistance(double radii)
{
    if (!std::isfinite(radii)) radii = kDefaultDistance;
    if (radii <= kDistMin) return 0.0;  // 0 or negative (typed): the closest framing, not the default
    return Clamp01(std::log(radii / kDistMin) / std::log(kDistMax / kDistMin));
}

inline double OffsetFromNorm(double v) { return (v - 0.5) * 2.0 * kTargetRange; }

inline double NormFromOffset(double radii)
{
    if (!std::isfinite(radii)) radii = 0.0;
    return Clamp01(radii / (2.0 * kTargetRange) + 0.5);
}

inline double DefaultNorm(int p)
{
    switch (p) {
        case kYaw:      return NormFromYaw(kDefaultYawRad);
        case kPitch:    return NormFromPitch(kDefaultPitchRad);
        case kDistance: return NormFromDistance(kDefaultDistance);
        default:        return 0.5;  // targets: no offset
    }
}

// A value from REAPER, a state blob or an envelope: NaN -> default, then 0..1.
inline double Sanitize(int p, double v)
{
    return std::isfinite(v) ? Clamp01(v) : DefaultNorm(p);
}

// ---- text (the FX window, envelope tooltips) -----------------------------------------
// Display units: degrees for yaw/pitch, radii of the model for distance and target.
inline double DisplayFromNorm(int p, double v)
{
    v = Sanitize(p, v);
    switch (p) {
        case kYaw:      return YawFromNorm(v) * 180.0 / kPi;
        case kPitch:    return PitchFromNorm(v) * 180.0 / kPi;
        case kDistance: return DistanceFromNorm(v);
        default:        return OffsetFromNorm(v);
    }
}

inline double NormFromDisplay(int p, double x)
{
    switch (p) {
        case kYaw:      return NormFromYaw(x * kPi / 180.0);
        case kPitch:    return NormFromPitch(x * kPi / 180.0);
        case kDistance: return NormFromDistance(x);
        default:        return NormFromOffset(x);
    }
}

inline void FormatValue(int p, double v, char* out, size_t cap)
{
    if (!out || cap == 0) return;
    const double x = DisplayFromNorm(p, v);
    switch (p) {
        case kYaw:
        case kPitch:    std::snprintf(out, cap, "%.1f\xC2\xB0", x); break;  // UTF-8 degree sign
        case kDistance: std::snprintf(out, cap, "%.2f radii", x); break;
        default:        std::snprintf(out, cap, "%+.2f radii", x); break;
    }
}

// Reads the leading number of `text` (units and spaces after it are ignored).
inline bool ParseValue(int p, const char* text, double* out_norm)
{
    if (!text || !out_norm || p < 0 || p >= kParamCount) return false;
    char* end = nullptr;
    const double x = std::strtod(text, &end);
    if (end == text || !std::isfinite(x)) return false;
    *out_norm = NormFromDisplay(p, x);
    return true;
}

}  // namespace vcam
}  // namespace rav
