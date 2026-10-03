// SPDX-License-Identifier: MIT
//
// Shots of the RAV video FX (Epic 11, Story 11-3). Main thread only.
//
// The video camera is a sequence of shots, stored where REAPER plays them back:
//  - each shot is a time + one envelope point on each of the FX's six camera
//    parameters (video_camera_params.h). The point shape says how the camera leaves
//    the shot: square = Cut to next, slow start/end = Move to next;
//  - shot names, the output size override and the saved angles are in the FX state
//    (video_fx_state.h), read and written through REAPER's "clap_chunk".
// Anything else may drive the parameters too (envelopes edited by hand, modulation,
// DM-XYZ-Pad); the shots are read back from whatever the envelopes' own points hold
// (points inside automation items are not read yet: deferred-work.md).
//
// Reading back: a shot is a group of envelope points within kVideoShotTimeTolerance
// of each other over the six envelopes. A value comes from that envelope's point at
// the shot, else from the envelope at that time, else from the current parameter
// value. A shot is Cut when all its points are square, else Move. Without any point,
// the camera is one implicit shot at 0 s holding the current parameter values.
//
// Writers do not open undo blocks: the caller wraps one gesture in one undo block
// (Undo_BeginBlock2 / Undo_EndBlock2 with UNDO_STATE_TRACKCFG | UNDO_STATE_FX).

#pragma once

#include <string>
#include <vector>

#include "reaper_api.h"
#include "video_camera_params.h"
#include "video_fx_state.h"

namespace rav {

constexpr double kVideoShotTimeTolerance = 0.0005;  // seconds: points closer than this are one shot

// REAPER envelope point shapes RAV writes.
constexpr int kVideoShapeCut  = 1;  // square
constexpr int kVideoShapeMove = 2;  // slow start/end

struct VideoShot {
    double time = 0.0;
    std::string name;            // stored name, else "Shot N" (1-based, positional, never stored)
    bool move_to_next = false;   // false = Cut to next
    bool implicit = false;       // no envelope point: the FX's current values at 0 s
    double values[vcam::kParamCount];  // defaults to the Recenter framing

    VideoShot()
    {
        for (int p = 0; p < vcam::kParamCount; ++p) values[p] = vcam::DefaultNorm(p);
    }
};

// ---- FX state (clap_chunk) -------------------------------------------------------------
// False when REAPER gives no chunk or it holds no RAV state.
bool ReadVideoFxState(MediaTrack* track, int fx, VideoFxState* out);

// Sends the names, override and angles of `meta` to the FX (camera values untouched).
bool WriteVideoFxMeta(MediaTrack* track, int fx, const VideoFxState& meta);

// ---- Shots -------------------------------------------------------------------------------
// The shots in time order (at least one).
std::vector<VideoShot> ReadVideoShots(MediaTrack* track, int fx);

// The current shot at the playhead (spec 11-fb-10): the last one whose first frame is at
// or before the playhead's frame; fps unknown (<= 0 or NaN): the last one starting within
// kVideoShotTimeTolerance of the playhead or before. Else 0; -1 when `shots` is empty.
// video_shot_timing.h VideoCurrentShotIndexOf has the rule.
int VideoCurrentShotIndex(const std::vector<VideoShot>& shots, double playhead, double fps);

// The six camera values the envelopes give at time t (what REAPER renders at t).
void ReadVideoCameraAt(MediaTrack* track, int fx, double t, double out[vcam::kParamCount]);

// Adds the shot, or replaces the one at shot.time: a point on each of the six
// envelopes (created when missing). An envelope that had no point first gets the
// parameter's current value pinned at 0 s, so the start keeps its framing. Stores the
// name (an empty name, or the automatic "Shot N", stores none).
bool WriteVideoShot(MediaTrack* track, int fx, const VideoShot& shot);

// Removes the shot's points and name. Removing the last point leaves the implicit shot.
bool DeleteVideoShot(MediaTrack* track, int fx, double time);

// Moves the shot at old_time to new_time: every existing point within the tolerance of
// old_time on the six envelopes changes time (values and shapes kept, no point added),
// and its name follows it. The caller keeps new_time between the neighbours. False when
// no point sat at old_time (nothing written).
bool MoveVideoShot(MediaTrack* track, int fx, double old_time, double new_time);

// Changes how the camera leaves the shot (point shapes).
bool SetVideoShotTransition(MediaTrack* track, int fx, double time, bool move_to_next);

// Empty name = back to "Shot N".
bool RenameVideoShot(MediaTrack* track, int fx, double time, const std::string& name);

// ---- Saved angles and output ---------------------------------------------------------------
// Replaces an angle of the same name, else adds it.
bool SaveVideoAngle(MediaTrack* track, int fx, const std::string& name, const double values[vcam::kParamCount]);
bool DeleteVideoAngle(MediaTrack* track, int fx, const std::string& name);

// 0 x 0 = follow the project's video size.
bool SetVideoOutputOverride(MediaTrack* track, int fx, int width, int height);

// ---- Click-based test hooks (actions) -------------------------------------------------------
// "RAV: Video FX: show shots of selected track": message box with the shot list.
void ShowVideoShotsOfSelectedTrack();

// "RAV: Video FX: add shot at edit cursor": a Cut shot holding the camera at the edit
// cursor, name asked in a dialog, one undo point.
void AddVideoShotAtEditCursor();

// "RAV: Video FX: set output size / save angle of selected track": a dialog that sets
// the output size override (empty = project size) and saves the camera at the edit
// cursor as a named angle (empty = none), one undo point.
void SetVideoOutputAndAngleOfSelectedTrack();

}  // namespace rav
