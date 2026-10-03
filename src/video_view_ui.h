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

constexpr float kVideoTopBand = 44.0f;  // the band above the frame: toggle, captions, FPS, panel button
constexpr float kVideoStripHeight = 56.0f;  // Story 11-5: the shot strip under the viewport (lane + spec 11-fb-6 ruler band)
constexpr float kVideoStripGap = 8.0f;      // its margin to the window's edges and the frame

// Width the panel takes from the viewport (column + gaps), 0 when it is hidden.
float VideoPanelFootprint(int client_w);

// The RAV view / Video view segmented toggle, centred on center_x at the top.
void DrawVideoViewToggle(float center_x);

// The panel button (shows / hides the Video panel), its right edge at right_x. Video view only.
void DrawVideoPanelButton(float right_x);

// Around and over the frame: its edge, the captions above it, and the state message
// inside it when Video view cannot show the camera (UX decision 7). copy_log copies the
// error log (the "out of date" state offers it). show_preview_lag: the caption line also
// shows how far REAPER's Video window runs ahead while playing (video_preview_lag.h).
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

}  // namespace rav

#endif  // _WIN32
