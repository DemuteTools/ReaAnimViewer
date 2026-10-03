// SPDX-License-Identifier: MIT
//
// The video FX content path (Story 11-2): RAV's picture of the FX track's animation at
// the requested project time, drawn on REAPER's video thread.
//
// For each frame, as a pure function of the request (+ the latest timeline snapshot and
// display settings): find the RAV item on the FX's track at project_time
// (video_timeline.h), get its shared parse (asset_cache.h), draw it with a renderer
// instance that lives on this thread's own GL context (video_gl_context.h) and its own
// GPU copies, posed at the item time, framed by the camera seam (video_camera_seam.h),
// with the viewer's display settings (display_settings.h); read it back into REAPER's
// frame. No item at the time, no loadable file or no GL = no picture.
//
// Called only from the frame API (video_fx_host.cpp) on REAPER's video thread. Never
// throws (the API wraps it too).

#pragma once

#ifdef _WIN32

struct RavVideoFrameRequest;

namespace rav {

struct VideoTrackTimeline;

// The one output-size rule, shared by the content and the test pattern: the request's
// override when set, else the track's project video size (`tl` may be null), else
// 1920x1080, clamped to 16..8192.
void VideoOutputSize(const RavVideoFrameRequest* req, const VideoTrackTimeline* tl, int* w, int* h);

// The same rule on plain numbers (Story 11-4: Video view frames at the FX's exact output
// size): the override when both are > 0, else the project size when both are > 0, else
// 1920x1080, clamped to 16..8192. Pure, any thread.
void VideoOutputSizeFor(int override_w, int override_h, int project_w, int project_h, int* w, int* h);

// Output size of the content at req->project_time: 0 x 0 when there is no picture.
// Else the request's override when set, else the project's video size, else 1920x1080,
// clamped to 16..8192.
void VideoContentFrameSize(const RavVideoFrameRequest* req, int* out_width, int* out_height);

// Draws the content into req->pixels. 1 = picture produced, 0 = none.
int VideoContentRenderFrame(const RavVideoFrameRequest* req);

}  // namespace rav

#endif  // _WIN32
