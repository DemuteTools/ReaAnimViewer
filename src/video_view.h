// SPDX-License-Identifier: MIT
//
// Video view and the Video panel, the model behind them (Epic 11, Story 11-4). Main
// thread only; no ImGui, no GL here (video_view_ui.h draws, viewer_window.cpp renders).
//
// Video view shows what the RAV video FX renders: the FX track's item at the playhead,
// seen through the FX's camera, which is the six camera envelopes evaluated at the
// playhead's frame time (video_shot_timing.h VideoPlayheadFrameTime, video_shots.h ReadVideoCameraAt), decoded on the displayed model's posed
// bounds (video_camera.h), exactly what the FX decodes from REAPER's parmlist. The
// viewer's free camera is never touched: Video view has its own camera.
//
// Gestures (orbit / pan / zoom / ViewCube / an inspector slider) edit the shot under
// the playhead: the view follows the gesture at once, and the shot is written at the
// end of the gesture through 11-3's helpers, inside ONE named undo block.
//
// Writes are never made inside the ImGui frame: a widget queues a command and the
// viewer runs the queue from its window procedure (VideoViewRunPending), where a
// message box or a plug-in load may safely run a modal loop.

#pragma once

#ifdef _WIN32

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "camera.h"
#include "video_camera_params.h"
#include "video_shots.h"

namespace rav {

// The FX on the Video view's track, as the panel shows it (UX decision 7).
enum class VideoFxStatus {
    NoTrack,    // no animation item was displayed yet: nothing to follow
    Missing,    // no RAV video FX on the track
    Bypassed,   // the FX is there but bypassed
    Offline,    // the FX is there but offline
    OutOfDate,  // an FX asked this extension for another API version: it draws nothing
    Active,
};

struct VideoPinChoice {
    MediaTrack* track = nullptr;
    std::string label;  // "3 \xC2\xB7 Name"
};

// Spec 11-fb-15 -- a shot as Video view shows it: shots belong to clips (video_clip_shots.h).
// `time` is its envelope time (implicit: the time its point gets when an edit writes it),
// `values` its camera (implicit: what the envelopes give on the clip's first frame), `name`
// the stored name, else `auto_name` ("<anim name> N").
struct VideoViewShot : VideoShot {
    int         clip = -1;          // index into VideoViewModel::clips
    int         number = 1;         // 1-based within its clip
    double      start = 0.0;        // its first frame (display and seek start)
    double      end = 0.0;          // display end (exclusive)
    bool        clip_first = false; // opens its clip: never deleted
    std::string auto_name;
};

// A clip of the Video view's track (the RAV items, overlaps trimmed, in time order).
struct VideoViewClip {
    double      start = 0.0;  // effective start
    double      end = 0.0;
    double      first = 0.0;  // its first frame's time
    std::string name;         // the animation name: file name without folders and extension
};

// What the panel and the frame read. Refreshed by VideoViewFrame.
struct VideoViewModel {
    MediaTrack* track = nullptr;   // the FX track: pinned, else the displayed item's
    bool        pinned = false;
    int         fx = -1;           // index of the RAV video FX on `track`, -1 if none
    VideoFxStatus status = VideoFxStatus::NoTrack;
    std::string track_label;
    std::vector<VideoViewShot> shots;  // empty unless the FX is on the track (spec 11-fb-15: per clip)
    std::vector<VideoViewClip> clips;  // the track's clips, in time order
    std::vector<VideoPinChoice> pin_choices;  // tracks carrying the FX (+ the followed one)
    int    out_w = 1920;           // the FX's output size (override, else project, else 1920x1080)
    int    out_h = 1080;
    double fps = 0.0;              // project frame rate, 0 = unknown
    // Story 11-5 -- the Video panel's Output section and saved angles.
    int    override_w = 0;         // the FX's output override, 0 x 0 = the project's size
    int    override_h = 0;
    int    project_w = 0;          // the project's video size, 0 x 0 = not set
    int    project_h = 0;
    bool   transparent = false;    // the track's background option (video_timeline.h)
    std::vector<VideoFxAngle> angles;  // saved angles in the FX state
    // Spec 11-fb-11 -- the Envelopes button: the FX has an envelope, one of them shows.
    bool   has_envelopes = false;
    bool   envelopes_visible = false;
};

const VideoViewModel& GetVideoViewModel();

// Spec 11-fb-15 -- true when shot `index` + 1 starts where shot `index` ends (same frame):
// the shot has a next shot to move to and a junction with it. False across a gap between
// clips and for the last shot (Move to next is then refused, like for the last shot).
bool VideoViewShotTouchesNext(int index);

// ---- Session state (not saved) -----------------------------------------------------------
bool VideoViewActive();
// The first entry of the session also opens the panel. A switch ends a running drag or
// wheel gesture as shown (one undo point).
void SetVideoViewActive(bool on);
bool VideoPanelVisible();
void SetVideoPanelVisible(bool visible);  // hiding it ends a running slider gesture as shown

// Pin a track (nullptr = follow the displayed item).
void VideoViewPin(MediaTrack* track);
// The pinned track, nullptr when following. Checked against the current project.
MediaTrack* VideoViewPinnedTrack();

// ---- Per frame (viewer RenderTick, before the ImGui frame) --------------------------------
// What the viewer displays this frame: the item's track (nullptr = no item at the
// playhead; the followed track then stays the last one), whether an item spans the
// playhead on the Video view's track, and the displayed model's posed bounds
// (has_model false = nothing loaded). Also refreshes the model when the project changed
// (at most twice a second otherwise), queues the write of a wheel gesture that went
// quiet, advances the ViewCube tween and reads the FX camera at the playhead. Does
// almost nothing while Video view and the panel are both off.
void VideoViewFrame(MediaTrack* item_track, bool has_item, bool has_model, const glm::vec3& aabb_min,
                    const glm::vec3& aabb_max);

// True when Video view can show and edit the camera: the FX is active, an item spans the
// playhead on its track, and a model is loaded.
bool VideoViewCanEdit();

// The camera Video view draws with this frame (the gesture's while one runs).
bool VideoViewCamera(OrbitCamera* out);

// The shot under the playhead (index into the model's shots), -1 when none (spec 11-fb-15:
// also between clips).
int VideoViewShotIndex();

// ---- Gestures in the view (window procedure) ----------------------------------------------
// Starts an orbit (pan = false) or pan drag; false when Video view cannot edit.
bool VideoViewBeginDrag(bool pan);
void VideoViewDrag(int dx, int dy);
// Ends the drag: writes the shot (one undo point). No-op without a drag.
void VideoViewEndDrag();
bool VideoViewDragging();
// One wheel step; steps less than 0.4 s apart are one gesture (written when it goes quiet).
void VideoViewWheel(float delta);

// ---- Inspector (inside the ImGui frame: state only, writes are queued) --------------------
// The six values the inspector shows: the shot under the playhead, live while a slider moves.
bool VideoViewInspectorValues(double out[vcam::kParamCount]);
// A slider moves (normalized value); the view follows at once.
void VideoViewSliderEdit(int param, double norm);
// The slider was released: queue the write (one undo point).
void VideoViewSliderCommit(int param);

// Renames the shot at shot_time on that track's FX (captured when the edit began). `auto_name`
// is its automatic name (typing it back stores none); an implicit shot is written first
// (spec 11-fb-15, same undo point) unless the name stays automatic.
void QueueVideoRename(MediaTrack* track, int fx, double shot_time, const std::string& name,
                      const std::string& auto_name, bool implicit);
void QueueVideoTransition(bool move_to_next);
// Spec 11-fb-11 -- switches shot `index` (not the current one) between Cut and Move to
// next, one undo point (the inspector's write). The last shot cannot become Move: a notice
// says so and nothing is written.
void QueueVideoTransitionAt(int index);
// Spec 11-fb-11 -- the Envelopes button: hides every existing envelope of the FX when one
// shows, else shows them all (none is created). One undo point.
void QueueVideoToggleEnvelopes();
void QueueVideoCopyRavView(const OrbitCamera& free_camera);
void QueueVideoFrameModel();
void QueueVideoSnap(const glm::vec3& dir);  // the ViewCube in Video view
void QueueVideoAddFx();
void QueueVideoEnableFx();
void QueueVideoSetFxOnline();

bool VideoViewHasPending();
// Runs the queued commands (main thread, outside the render tick).
void VideoViewRunPending();

// The viewer closes: drop a running gesture without writing, forget the queue.
void VideoViewOnViewerClosed();

// ---- Shot strip, shot list, saved angles, output, render (Story 11-5) -----------------------
// The shot strip's current clip: the RAV item under the playhead on the Video view's track,
// else the nearest item on it. valid false = no item on the track. Spec 11-fb-12: every item
// of the track too (drawn greyed around the current one), `current` the index of start / end
// in `items`.
struct VideoStripSpan {
    double start = 0.0;
    double end = 0.0;
};
struct VideoStripRange {
    bool   valid = false;
    bool   under_playhead = false;      // spec 11-fb-16: the current item spans the playhead (not just the nearest)
    double start = 0.0;
    double end = 0.0;
    std::vector<VideoStripSpan> items;  // the track's items (end > start), in track order
    int    current = -1;                // index of the current item in `items`
};
const VideoStripRange& VideoViewStripRange();
// Spec 11-fb-15 -- the shot list's group headers: clip `clip`'s animation name ("" when out of
// range) and its start time.
const char* VideoViewClipName(int clip);
double VideoViewClipStart(int clip);

// REAPER's playhead as Video view reads it (play position while playing, else the edit cursor).
double VideoViewPlayhead();
// True while REAPER plays (or records): VideoViewPlayhead is then the play position.
bool VideoViewPlaying();
// Read with the playhead: REAPER repeats over a loop range (start < end), and the project playrate.
void VideoViewLoop(bool* out_looping, double* out_start, double* out_end);
double VideoViewPlayRate();

// A short message after an action that did nothing (e.g. a cut on a frame that already
// starts a shot), nullptr when none is showing.
const char* VideoViewNotice();

// True when a cut can be made: the FX on the Video view's track is active and no gesture runs.
bool VideoViewCanCut();

// Moves REAPER's playhead (seeks playback). Consecutive seeks of one frame keep the last.
void QueueVideoSeek(double t);
// Moves the playhead to the first frame of shot `index`.
void QueueVideoSeekToShot(int index);
// A new Cut shot at the playhead holding the camera shown (video_shot_timing.h places it).
void QueueVideoCut();
void QueueVideoDeleteShot(int index);

// Spec 11-fb-11 -- the Delete key (Video view): deletes the shot under the playhead after a
// confirmation, at once when the user ticked "Don't ask again". A clip's first shot is never
// deleted (a notice says so; spec 11-fb-15).
void RequestVideoDeleteCurrentShot();
// The confirmation: open, the name of the shot it asks about, confirm (with the "Don't ask
// again" choice) or cancel (Esc, Cancel, Video view left).
bool VideoDeleteConfirmOpen();
const char* VideoDeleteConfirmName();
void ConfirmVideoDelete(bool dont_ask_again);
void CancelVideoDelete();
// The global "Ask before deleting a shot" setting (ExtState, kept across sessions).
bool VideoAskBeforeDeleteShot();
void SetVideoAskBeforeDeleteShot(bool ask);
// Moves the start of shot `index` (>= 1) to new_time (VideoJunctionDragTime places it):
// one undo point "RAV: Move video shot". Nothing when it stays on its frame, while a
// camera gesture is unwritten, or when the shot does not touch the previous one (spec
// 11-fb-15: a junction only exists between contiguous shots). An implicit shot is written at
// new_time; a shot that now covers the next clip's start gets the span flag, the previous
// shot loses it when it no longer does.
void QueueVideoMoveShot(int index, double new_time);
// Saved angle `index` into the shot under the playhead.
void QueueVideoApplyAngle(int index);
// Saves the six values of the shot under the playhead under `name` (same name replaces).
void QueueVideoSaveAngle(const std::string& name);
void QueueVideoDeleteAngle(const std::string& name);
// The FX's output override (0 x 0 = the project's size).
void QueueVideoOutputSize(int width, int height);
// Asks a size in REAPER's input dialog, then sets it as the override.
void QueueVideoCustomOutputSize();
void QueueVideoBackground(bool transparent);
void QueueVideoOpenRenderMatrix();
void QueueVideoOpenRenderDialog();
// Spec 11-fb-16 -- REAPER's Render dialog prefilled with the clip under the playhead (custom
// time range = its start to end, source = master mix), at the Output size; the project's
// render bounds and source are put back once the dialog closes. Nothing without a clip
// under the playhead.
void QueueVideoRenderCurrent();

// ---- Layout ---------------------------------------------------------------------------------
struct VideoFrameRect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;  // the frame, in client pixels (top-left origin)
    bool cube_right = true;                         // ViewCube in a right column (else a bottom band)
};
// The frame at the output aspect inside the area [0, area_w] x [0, area_h], below a
// top band, leaving the ViewCube's corner free (right column or bottom band, whichever
// gives the larger frame). Pure.
VideoFrameRect ComputeVideoFrameRect(float area_w, float area_h, float top_band, float cube_room, int out_w,
                                     int out_h);

}  // namespace rav

#endif  // _WIN32
