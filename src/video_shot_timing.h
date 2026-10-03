// SPDX-License-Identifier: MIT
//
// Where a shot cut goes in time (Epic 11, Story 11-5). Pure, header-only, no REAPER.
//
// REAPER asks the video FX for frame k at project time ~ k / fps, and the camera at that
// time comes from the envelopes. A square envelope point placed EXACTLY on k / fps could
// fall one frame early or late depending on how REAPER rounds that time. So a cut made
// at the playhead goes half a frame before the frame the playhead is in: frame k is the
// first frame of the new shot whatever the rounding, and Video view (which evaluates the
// envelopes at the playhead) shows the new shot at once.

#pragma once

#include <cmath>
#include <vector>

namespace rav {

// The frame the playhead is in (floor), with a small tolerance so a playhead sitting on
// k / fps (minus floating error) counts as frame k.
inline long long VideoFrameIndexAt(double t, double fps)
{
    if (!(fps > 0.0) || !std::isfinite(t) || t <= 0.0) return 0;
    return static_cast<long long>(std::floor(t * fps + 1e-6));
}

// Time of the envelope points of a cut made with the playhead at t.
// fps <= 0 (unknown): t itself. Frame 0: 0.
inline double VideoCutTime(double t, double fps)
{
    if (!std::isfinite(t) || t < 0.0) t = 0.0;
    if (!(fps > 0.0)) return t;
    const long long k = VideoFrameIndexAt(t, fps);
    if (k <= 0) return 0.0;
    return (static_cast<double>(k) - 0.5) / fps;
}

// The first frame a shot starting at shot_time shows: where a click on the shot puts
// the playhead. fps <= 0: shot_time itself.
inline double VideoShotFirstFrameTime(double shot_time, double fps)
{
    if (!std::isfinite(shot_time) || shot_time <= 0.0) return 0.0;
    if (!(fps > 0.0)) return shot_time;
    return std::ceil(shot_time * fps - 1e-6) / fps;
}

// The frame a shot starting at shot_time starts on (its first frame).
inline long long VideoShotFirstFrame(double shot_time, double fps)
{
    if (!(fps > 0.0) || !std::isfinite(shot_time) || shot_time <= 0.0) return 0;
    return static_cast<long long>(std::ceil(shot_time * fps - 1e-6));
}

// Where a junction dragged in the shot strip puts the later shot (Epic 11, feedback 2).
// Snapped like a cut (its first frame is the frame under the mouse), then clamped so both
// neighbours keep at least one frame: the moved shot's first frame stays strictly after
// the previous shot's first frame and strictly before the first frame of `next_limit`
// (the next shot's start, or the lane end for the last shot). fps <= 0 (unknown): the
// mouse time itself, kept 1 ms inside (prev_start, next_limit). NaN when there is no
// frame between the neighbours (they are less than two frames apart): no move is possible.
inline double VideoJunctionDragTime(double t_mouse, double prev_start, double next_limit, double fps)
{
    if (!std::isfinite(t_mouse) || t_mouse < 0.0) t_mouse = 0.0;
    if (!(fps > 0.0)) {
        constexpr double kMargin = 1.0e-3;
        const double lo = prev_start + kMargin;
        const double hi = next_limit - kMargin;
        if (lo > hi) return std::nan("");  // no room
        if (t_mouse > hi) t_mouse = hi;
        if (t_mouse < lo) t_mouse = lo;
        return t_mouse;
    }
    long long k = VideoFrameIndexAt(t_mouse, fps);
    const long long k_min = VideoShotFirstFrame(prev_start, fps) + 1;
    const long long k_max = VideoShotFirstFrame(next_limit, fps) - 1;
    if (k_min > k_max) return std::nan("");  // no room
    if (k > k_max) k = k_max;
    if (k < k_min) k = k_min;
    return (static_cast<double>(k) - 0.5) / fps;
}

// True when one of `shot_times` already starts on the frame a cut at playhead t would
// start, or within `tolerance` seconds of t when fps is unknown.
inline bool VideoCutWouldDuplicate(const std::vector<double>& shot_times, double t, double fps, double tolerance)
{
    if (!(fps > 0.0)) {
        for (double s : shot_times) {
            if (std::fabs(s - t) <= tolerance) return true;
        }
        return false;
    }
    const long long k = VideoFrameIndexAt(t, fps);
    for (double s : shot_times) {
        if (VideoShotFirstFrame(s, fps) == k) return true;
    }
    return false;
}

}  // namespace rav
