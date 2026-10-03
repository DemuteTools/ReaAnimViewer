// SPDX-License-Identifier: MIT
//
// See video_camera.h.

#include "video_camera.h"

#ifdef _WIN32

#include <cmath>

namespace rav {

// The FX defaults must stay the viewer's Recenter (OrbitCamera::Reset, camera.h); the
// distance default (3 radii) mirrors Reset's `diag * 1.5`, kept in step by hand.
static_assert(static_cast<float>(vcam::kDefaultYawRad) == kInitYaw, "video FX default yaw != Recenter (camera.h)");
static_assert(static_cast<float>(vcam::kDefaultPitchRad) == kInitPitch,
              "video FX default pitch != Recenter (camera.h)");

VideoFraming VideoFramingFromBounds(const glm::vec3& aabb_min, const glm::vec3& aabb_max)
{
    // Same rules as OrbitCamera::Reset (camera.h), so the defaults equal Recenter.
    VideoFraming f;
    glm::vec3 centre = 0.5f * (aabb_min + aabb_max);
    float diag = glm::length(aabb_max - aabb_min);
    if (!(std::isfinite(diag) && diag > 1e-4f)) diag = 2.0f;
    if (!(std::isfinite(centre.x) && std::isfinite(centre.y) && std::isfinite(centre.z))) centre = glm::vec3(0.0f);
    f.centre = centre;
    f.radius = 0.5f * diag;
    return f;
}

void DefaultVideoCameraValues(double out[vcam::kParamCount])
{
    for (int p = 0; p < vcam::kParamCount; ++p) out[p] = vcam::DefaultNorm(p);
}

OrbitCamera VideoCameraFromValues(const double values[vcam::kParamCount], const glm::vec3& aabb_min,
                                  const glm::vec3& aabb_max)
{
    double v[vcam::kParamCount];
    for (int p = 0; p < vcam::kParamCount; ++p) v[p] = vcam::Sanitize(p, values ? values[p] : vcam::DefaultNorm(p));

    const VideoFraming f = VideoFramingFromBounds(aabb_min, aabb_max);
    OrbitCamera cam;
    cam.frameRadius = f.radius;
    cam.yaw = static_cast<float>(vcam::YawFromNorm(v[vcam::kYaw]));
    cam.pitch = static_cast<float>(vcam::PitchFromNorm(v[vcam::kPitch]));
    cam.distance = static_cast<float>(vcam::DistanceFromNorm(v[vcam::kDistance]) * f.radius);
    cam.target = f.centre + f.radius * glm::vec3(static_cast<float>(vcam::OffsetFromNorm(v[vcam::kTargetX])),
                                                 static_cast<float>(vcam::OffsetFromNorm(v[vcam::kTargetY])),
                                                 static_cast<float>(vcam::OffsetFromNorm(v[vcam::kTargetZ])));
    cam.animating = false;
    return cam;
}

OrbitCamera VideoCameraFromParms(const double* parms, int nparms, const glm::vec3& aabb_min,
                                 const glm::vec3& aabb_max)
{
    double v[vcam::kParamCount];
    for (int p = 0; p < vcam::kParamCount; ++p) {
        const int slot = p + 1;  // parms[0] is wet
        v[p] = (parms && slot < nparms) ? parms[slot] : vcam::DefaultNorm(p);
    }
    return VideoCameraFromValues(v, aabb_min, aabb_max);
}

void VideoCameraValuesFromCamera(const OrbitCamera& cam, const glm::vec3& aabb_min, const glm::vec3& aabb_max,
                                 double out[vcam::kParamCount])
{
    const VideoFraming f = VideoFramingFromBounds(aabb_min, aabb_max);
    const glm::vec3 offset = (cam.target - f.centre) / f.radius;
    out[vcam::kYaw] = vcam::NormFromYaw(cam.yaw);
    out[vcam::kPitch] = vcam::NormFromPitch(cam.pitch);
    out[vcam::kDistance] = vcam::NormFromDistance(static_cast<double>(cam.distance) / f.radius);
    out[vcam::kTargetX] = vcam::NormFromOffset(offset.x);
    out[vcam::kTargetY] = vcam::NormFromOffset(offset.y);
    out[vcam::kTargetZ] = vcam::NormFromOffset(offset.z);
}

}  // namespace rav

#endif  // _WIN32
