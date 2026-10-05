// SPDX-License-Identifier: MIT
//
// Video view and the Video panel, the Dear ImGui side (Epic 11, Story 11-4), in the
// DM-XYZ-Pad theme (ui_theme.h). Drawn by the viewer inside its one ImGui frame; reads
// and edits the model of video_view.h (state only: REAPER writes are queued there).
// Layout follows the validated mock-up (ux-designs/epic-11-video-mode): the RAV view /
// Video view toggle top centre, the panel button top right of Video view, the size /
// frame rate / shot name just above the frame (nothing inside it), the Video panel as a
// column on the right.

#pragma once

#ifdef _WIN32

#include "camera.h"
#include "video_view.h"

namespace rav {

// The band above the Video view frame: the overlay row (menu button, view toggle, keyboard
// icon, FPS, panel button; its bottom is kVideoOverlayRowBottom) and, under it, the caption
// line (size / fps / catch-up readout, shot name), so captions never sit under an overlay
// (spec 11-fb-8). The row's lowest overlay is the ##tools menu card: y 10 + padding 8 +
// 22 px icon + 2 x 3 frame padding + padding 8 = 54. Change it with those values.
constexpr float kVideoOverlayRowBottom = 54.0f;
constexpr float kVideoCaptionGap = 5.0f;  // captions sit this far above the frame
// Called before ImGui's frame starts (RenderVideoView): uses the font size of the last
// frame, or 13 px when there is no context / font yet.
float VideoTopBand();
// Story 11-5: the shot strip under the viewport. 2 x 6 px padding + the 24 px lane + the
// spec 11-fb-9 ruler band (full-size labels, 13 px at the default font, + 6 px tick room)
// = 55 px, with margin for a slightly larger font. A larger one shrinks the band (clamped in
// DrawVideoShotStrip), never the lane. Spec 11-fb-11: the strip's minimum height; its top
// edge drags it taller (VideoStripHeight).
constexpr float kVideoStripHeight = 64.0f;
constexpr float kVideoStripGap = 8.0f;      // its margin to the window's edges and the frame

// Spec 11-fb-11 -- the shot strip's current height: what the user dragged its top edge to
// (kept in ExtState across sessions), between kVideoStripHeight and half the client
// height. The extra height goes to the shot lane; the ruler keeps its own.
float VideoStripHeight(float client_h);

// Width the panel takes from the viewport (column + gaps), 0 when it is hidden.
float VideoPanelFootprint(int client_w);

// The RAV view / Tagging view / Video view segmented toggle, centred on center_x at the top.
void DrawVideoViewToggle(float center_x);

// The panel button (shows / hides the Video panel), its right edge at right_x. Video view only.
void DrawVideoPanelButton(float right_x);

// Around and over the frame: its edge, the captions above it, and the state message
// inside it when Video view cannot show the camera (UX decision 7). copy_log copies the
// error log (the "out of date" state offers it). show_preview_lag: the caption line also
// shows how long REAPER's Video window takes to catch up with an output size or display
// change while playing, shown while that window is open (video_preview_lag.h, spec 11-fb-5).
void DrawVideoFrameDecor(const VideoFrameRect& fr, void (*copy_log)(), bool show_preview_lag);

// Story 11-5 -- the shot strip shows in Video view while the FX on its track is active.
bool VideoShotStripVisible();

// The shot strip at (x, y), size (w, h) (UX decision 4): the playhead's timecode, a lane
// with the shots over the item and REAPER's playhead (click / drag = move the playhead),
// and the Cut button (C).
void DrawVideoShotStrip(float x, float y, float w, float h);

// The Video panel at (x, y), size (w, h). free_camera = the RAV view camera (Copy RAV
// view); copy_log copies the error log (the "out of date" state offers it).
void DrawVideoPanel(float x, float y, float w, float h, const OrbitCamera& free_camera, void (*copy_log)());

// Spec 11-fb-11 -- the confirmation the Delete key opens (video_view.h
// RequestVideoDeleteCurrentShot): Delete shot "<name>"?, Delete / Cancel, "Don't ask
// again". Call once per frame, at the top level of the ImGui frame.
void DrawVideoDeleteConfirm();
// Enter pressed while it is open (window procedure): the next frame confirms it with the
// dialog's current "Don't ask again" choice.
void RequestVideoDeleteConfirmByKey();

}  // namespace rav

#endif  // _WIN32
