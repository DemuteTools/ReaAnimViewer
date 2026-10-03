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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
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

// The shot strip's ruler (spec 11-fb-6). With a known fps it counts whole project frames
// from the item's first frame F0 = VideoShotFirstFrame(origin, fps): tick k is frame
// F0 + k * step_frames, drawn at F / fps. A "second" is the rounded fps in frames (nominal
// seconds, like non-drop timecode). fps unknown: whole seconds from the item start.

// The fps the ruler counts frames with: the rounded fps, 0 when unknown (NaN, inf, under
// 0.5 or absurd, 1e6 and up).
inline long long VideoRulerFps(double fps)
{
    if (!(fps >= 0.5) || !(fps < 1.0e6)) return 0;
    return static_cast<long long>(std::llround(fps));
}

// The tick step, in seconds, for an item `span_s` long drawn over `lane_px` pixels: the
// smallest allowed step whose ticks sit at least `min_px` apart (min_px <= 0: 1 px).
// Allowed: frame steps of 1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 24, 25, 30, 40, 50 or 60
// frames that divide the rounded fps and are under it (so every whole second is a tick),
// then 1, 2, 5, 10, 30 s, 1, 2, 5, 10, 30 min, 1 h, then whole hours. A frame step is
// n / fps seconds. fps unknown (VideoRulerFps 0): seconds only. 0 when there is nothing to draw.
inline double VideoRulerStep(double span_s, double lane_px, double fps, double min_px)
{
    if (!(span_s > 0.0) || !(lane_px > 0.0) || !std::isfinite(span_s) || !std::isfinite(lane_px)) return 0.0;
    if (!(min_px > 0.0) || !std::isfinite(min_px)) min_px = 1.0;
    const double px_per_s = lane_px / span_s;
    const long long ifps = VideoRulerFps(fps);
    if (ifps > 0) {
        static const int kFrameSteps[] = {1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 24, 25, 30, 40, 50, 60};
        for (int n : kFrameSteps) {
            if (n >= ifps) break;
            if (ifps % n != 0) continue;
            const double step = static_cast<double>(n) / fps;
            if (step * px_per_s >= min_px) return step;
        }
    }
    static const double kSecondSteps[] = {1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0, 600.0, 1800.0, 3600.0};
    for (double step : kSecondSteps) {
        if (step * px_per_s >= min_px) return step;
    }
    // Longer still: whole hours, as many as it takes.
    const double hours = std::ceil(min_px / (3600.0 * px_per_s));
    return 3600.0 * std::max(1.0, hours);
}

// A VideoRulerStep step in frames (seconds steps: whole seconds times the rounded fps).
// 0 when the fps is unknown or there is no step.
inline long long VideoRulerStepFrames(double step_s, double fps)
{
    const long long ifps = VideoRulerFps(fps);
    if (ifps <= 0 || !(step_s > 0.0) || !std::isfinite(step_s)) return 0;
    const long long n = step_s < 1.0 - 1e-9 ? std::llround(step_s * fps) : std::llround(step_s) * ifps;
    return std::max(1LL, n);
}

// The time of ruler tick k (k >= 0) for an item starting at `origin`: frame
// F0 + k * step_frames at F / fps, or origin + k * step when the fps is unknown.
inline double VideoRulerTickTime(long long k, double origin, double step_s, double fps)
{
    const long long sf = VideoRulerStepFrames(step_s, fps);
    if (sf > 0) return static_cast<double>(VideoShotFirstFrame(origin, fps) + k * sf) / fps;
    return origin + static_cast<double>(k) * step_s;
}

// The ruler tick nearest t (k >= 0: never before the item's first tick). 0 without a step.
inline long long VideoRulerNearestTick(double t, double origin, double step_s, double fps)
{
    if (!(step_s > 0.0) || !std::isfinite(step_s) || !std::isfinite(t)) return 0;
    const long long sf = VideoRulerStepFrames(step_s, fps);
    double k;
    if (sf > 0) {
        const double f0 = static_cast<double>(VideoShotFirstFrame(origin, fps));
        k = std::round((t * fps - f0) / static_cast<double>(sf));
    } else {
        k = std::round((t - origin) / step_s);
    }
    return k > 0.0 ? static_cast<long long>(k) : 0;
}

// Snap (spec 11-fb-6): the time of the ruler tick nearest t. A scrub seeks to it; a
// junction drag passes it to VideoJunctionDragTime, whose first frame is then that tick's
// frame. step <= 0: t itself.
inline double VideoRulerSnap(double t, double origin, double step_s, double fps)
{
    if (!(step_s > 0.0) || !std::isfinite(step_s) || !std::isfinite(t)) return t;
    return VideoRulerTickTime(VideoRulerNearestTick(t, origin, step_s, fps), origin, step_s, fps);
}

// The ruler tick nearest t when ticks sit at origin + k * step (seconds). step <= 0: t itself.
inline double VideoSnapToTick(double t, double step, double origin)
{
    if (!(step > 0.0) || !std::isfinite(t) || !std::isfinite(step)) return t;
    return origin + std::round((t - origin) / step) * step;
}

// True when tick k falls on a whole (nominal) second from the item start: a major tick.
inline bool VideoRulerTickIsMajor(long long k, double step_s, double fps)
{
    const long long sf = VideoRulerStepFrames(step_s, fps);
    if (sf > 0) return (k * sf) % VideoRulerFps(fps) == 0;
    return true;  // seconds only: every tick is whole seconds
}

// The label of tick k, from the item start: whole seconds "m:ss" ("h:mm:ss" from one hour
// on), frame ticks between seconds "+Nf" (frames past the last whole second).
inline void VideoRulerTickLabel(long long k, double step_s, double fps, char* out, std::size_t out_size)
{
    if (!out || out_size == 0) return;
    const long long sf = VideoRulerStepFrames(step_s, fps);
    long long sec;
    if (sf > 0) {
        const long long ifps = VideoRulerFps(fps);
        const long long frames = k * sf;
        const long long rem = frames % ifps;
        if (rem != 0) {
            std::snprintf(out, out_size, "+%lldf", rem);
            return;
        }
        sec = frames / ifps;
    } else {
        sec = std::llround(static_cast<double>(k) * step_s);
    }
    if (sec >= 3600) {
        std::snprintf(out, out_size, "%lld:%02lld:%02lld", sec / 3600, (sec / 60) % 60, sec % 60);
    } else {
        std::snprintf(out, out_size, "%lld:%02lld", sec / 60, sec % 60);
    }
}

}  // namespace rav
