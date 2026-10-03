// SPDX-License-Identifier: MIT
//
// The video FX camera seam (Epic 11 cross-story contract). Story 11-2 renders the frame;
// Story 11-3 turns the FX's automatable parameters (yaw, pitch, distance, target) into a
// camera. This is the ONE function between them: the video renderer asks it for the
// camera of each frame, from the parameter values REAPER passed for that frame and the
// model's posed bounds. Since the merge of 11-2 and 11-3 its body calls 11-3's decoder
// (video_camera.h VideoCameraFromParms).
//
// No parameter passed (parms null or wet only): the framing of the viewer's "Recenter
// camera" (OrbitCamera::Reset on the posed bounds). Pure function, any thread, never throws.

#pragma once

#ifdef _WIN32

#include <glm/glm.hpp>

namespace rav {

// The model's bounds as drawn over its clip (Epic 9 ComputePosedBounds), world units.
struct PosedBounds {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

// An orbit camera in the viewer's convention (camera.h OrbitCamera): the eye sits at
// `distance` from `target`, at azimuth `yaw` and elevation `pitch` (radians, Y up).
struct VideoCameraPose {
    glm::vec3 target{0.0f};
    float     distance = 3.0f;
    float     yaw      = 0.0f;
    float     pitch    = 0.0f;
};

// parms = REAPER's parmlist for this frame ([0] = wet, then the FX's parameters, 0..1),
// nparms its length (0 when none).
VideoCameraPose ResolveVideoCamera(const double* parms, int nparms, const PosedBounds& b);

}  // namespace rav

#endif  // _WIN32
