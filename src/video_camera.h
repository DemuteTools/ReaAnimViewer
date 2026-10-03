// SPDX-License-Identifier: MIT
//
// The video camera of the RAV video FX (Epic 11, Story 11-3): the six normalized FX
// parameters (video_camera_params.h) <-> an orbit camera in world units, framed on
// the posed bounds of the model the way the viewer's Recenter frames it.
//
// Pure functions of their arguments, no REAPER call, no shared state: safe on REAPER's
// video thread. The frame for project time t uses only the parmlist REAPER passes for
// t, so automation renders frame-accurately (story 11-2's camera seam calls these).

#pragma once

#ifdef _WIN32

#include <glm/glm.hpp>

#include "camera.h"
#include "video_camera_params.h"

namespace rav {

// The framing reference of the posed bounds: Recenter's centre and radius (half the
// diagonal), with OrbitCamera::Reset's guards for empty / NaN / infinite bounds.
struct VideoFraming {
    glm::vec3 centre{0.0f};
    float radius = 1.0f;
};
VideoFraming VideoFramingFromBounds(const glm::vec3& aabb_min, const glm::vec3& aabb_max);

// The six default values (= Recenter framing).
void DefaultVideoCameraValues(double out[vcam::kParamCount]);

// From REAPER's parmlist as process_frame receives it: parms[0] = wet (ignored),
// parms[1..6] = yaw, pitch, distance, target X/Y/Z. Missing or NaN values fall back
// to the defaults. parms may be nullptr (nparms 0).
OrbitCamera VideoCameraFromParms(const double* parms, int nparms, const glm::vec3& aabb_min,
                                 const glm::vec3& aabb_max);

// From the six camera values alone (no wet slot).
OrbitCamera VideoCameraFromValues(const double values[vcam::kParamCount], const glm::vec3& aabb_min,
                                  const glm::vec3& aabb_max);

// Back to the six values ("Copy RAV view", camera gestures in Video view). Values out
// of range are clamped (pitch to +/-89 deg, distance to 0.05..20 radii, target to
// +/-3 radii per axis); yaw is wrapped to one turn.
void VideoCameraValuesFromCamera(const OrbitCamera& cam, const glm::vec3& aabb_min, const glm::vec3& aabb_max,
                                 double out[vcam::kParamCount]);

}  // namespace rav

#endif  // _WIN32
