// SPDX-License-Identifier: MIT
//
// See video_camera_seam.h.

#include "video_camera_seam.h"

#ifdef _WIN32

#include "camera.h"
#include "video_camera.h"

namespace rav {

VideoCameraPose ResolveVideoCamera(const double* parms, int nparms, const PosedBounds& b)
{
    OrbitCamera cam;
    if (parms && nparms > 1) {
        // Merge of 11-2 + 11-3: the camera of THIS frame, decoded only from the parmlist
        // REAPER passed for its time (parms[0] = wet, ignored; [1..6] = yaw, pitch,
        // distance, target X/Y/Z), framed on the posed bounds. A missing or NaN value
        // falls back to its default (= Recenter), so automation renders frame-accurately.
        cam = VideoCameraFromParms(parms, nparms, b.min, b.max);
    } else {
        // No camera parameter passed: exactly what the viewer's Recenter shows for this
        // model (OrbitCamera::Reset, with its degenerate / non-finite bounds guards).
        cam.Reset(b.min, b.max);
    }
    VideoCameraPose pose;
    pose.target   = cam.target;
    pose.distance = cam.distance;
    pose.yaw      = cam.yaw;
    pose.pitch    = cam.pitch;
    return pose;
}

}  // namespace rav

#endif  // _WIN32
