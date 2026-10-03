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
#include "video_clip_shots.h"
#include "video_fx_state.h"

namespace rav {

constexpr double kVideoShotTimeTolerance = 0.0005;  // seconds: points closer than this are one shot

// REAPER envelope point shapes RAV writes.
constexpr int kVideoShapeCut  = 1;  // square
constexpr int kVideoShapeMove = 2;  // slow start/end

struct VideoShot {
    double time = 0.0;
    std::string name;            // stored name, else "Shot N" (1-based, positional, never stored)
    bool stored_name = false;    // spec 11-fb-15: `name` is a stored (user) name
    bool span = false;           // spec 11-fb-15: runs on through the next back-to-back clip start
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

// The six camera values the envelopes give at time t (what REAPER renders at t).
void ReadVideoCameraAt(MediaTrack* track, int fx, double t, double out[vcam::kParamCount]);

// Adds the shot, or replaces the one at shot.time: a point on each of the six
// envelopes (created when missing). An envelope that had no point first gets the
// parameter's current value pinned at 0 s, so the start keeps its framing. Stores the
// name (an empty name, or `auto_name`, the shot's automatic name, stores none).
bool WriteVideoShot(MediaTrack* track, int fx, const VideoShot& shot, const std::string& auto_name = std::string());

// Removes the shot's points, name and span flag. Removing the last point leaves the implicit shot.
bool DeleteVideoShot(MediaTrack* track, int fx, double time);

// Moves the shot at old_time to new_time: every existing point within the tolerance of
// old_time on the six envelopes changes time (values and shapes kept, no point added),
// and its name and span flag follow it. The caller keeps new_time between the neighbours.
// False when no point sat at old_time (nothing written).
bool MoveVideoShot(MediaTrack* track, int fx, double old_time, double new_time);

// Changes how the camera leaves the shot (point shapes).
bool SetVideoShotTransition(MediaTrack* track, int fx, double time, bool move_to_next);

// Empty name, or `auto_name` (the shot's automatic name) = back to the automatic name.
bool RenameVideoShot(MediaTrack* track, int fx, double time, const std::string& name,
                     const std::string& auto_name = std::string());

// Spec 11-fb-15 -- sets or clears the span flag of the shot at `time` (FX state). Sends the
// state only when it changes. False when the state is not readable.
bool SetVideoShotSpan(MediaTrack* track, int fx, double time, bool on);

// ---- Shots per clip (spec 11-fb-15) ---------------------------------------------------------
// The FX's raw shots (ReadVideoShots, `raw`) laid over the clips of its track (the RAV items
// in the latest timeline snapshot, video_timeline.h): video_clip_shots.h has the rules.
struct VideoTrackClipShots {
    std::vector<VideoShot> raw;
    VideoClipShots         built;
};
VideoTrackClipShots ReadVideoClipShots(MediaTrack* track, int fx, double fps);

// The automatic name of built shot `i` ("<anim name> N").
std::string VideoClipShotAutoName(const VideoClipShots& built, size_t i);

// Writes a Cut point at `time` holding the camera of its first frame (what the envelopes
// render there) when no point is there yet: an implicit shot becomes a real one. `values`:
// that camera read beforehand (when several points are written in one gesture, every camera
// is read before the first write); nullptr = read it now. No undo block (the caller's gesture
// has one). True when a point is there afterwards.
bool EnsureVideoShotPoint(MediaTrack* track, int fx, double time, double fps,
                          const double* values = nullptr);

// Before a cut at `cut_time` (its envelope time) in clip `clip` of `built`: writes the clip's
// implicit first shot when it has one and the cut is not on it (EnsureVideoShotPoint).
bool MaterializeClipFirstShotForCut(MediaTrack* track, int fx, const VideoClipShots& built, int clip,
                                    double cut_time, double fps);

// After a cut at `cut_time` (written) in clip `clip`: when the cut lands in a shot of that
// clip with the span flag (before the clip it spans into), the flag moves to the cut (the
// shot that now runs into the next clip). `cs`: read before the cut. No undo block.
void MoveSpanToCut(MediaTrack* track, int fx, const VideoTrackClipShots& cs, int clip, double cut_time,
                   double fps);

// ---- Envelope visibility (spec 11-fb-11) --------------------------------------------------
// Over every existing envelope of the FX (never created here): *any = it has one,
// *visible = at least one shows in the arrange.
void VideoFxEnvelopesState(MediaTrack* track, int fx, bool* any, bool* visible);

// Shows or hides every existing envelope of the FX in the arrange (none is created).
// False when the FX has no envelope. No undo block (the caller opens one).
bool SetVideoFxEnvelopesVisible(MediaTrack* track, int fx, bool visible);

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
// cursor, name asked in a dialog, one undo point. Spec 11-fb-15: like a cut in Video view, it
// also writes the clip's implicit first shot (same undo point); refused between clips.
void AddVideoShotAtEditCursor();

// "RAV: Video FX: set output size / save angle of selected track": a dialog that sets
// the output size override (empty = project size) and saves the camera at the edit
// cursor as a named angle (empty = none), one undo point.
void SetVideoOutputAndAngleOfSelectedTrack();

}  // namespace rav
