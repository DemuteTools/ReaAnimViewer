// SPDX-License-Identifier: MIT
//
// Where a shot cut goes in time (Epic 11, Story 11-5). Pure, header-only, no REAPER.
//
// REAPER asks the video FX for frame k at project time ~ k / fps, and the camera at that
// time comes from the envelopes. A square envelope point placed EXACTLY on k / fps could
// fall one frame early or late depending on how REAPER rounds that time. So a cut made
// at the playhead goes half a frame before the frame the playhead is in: frame k is the
// first frame of the new shot whatever the rounding, and Video view (which evaluates the
// envelopes at the playhead's frame time, VideoPlayheadFrameTime) shows the new shot at once.

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

// Spec 11-fb-10: a shot starts at its first frame, everywhere in Video view. The current
// shot is the one whose first frame is at or before the frame the playhead is in, so it
// changes exactly where the strip draws the line (the first frame), not half a frame early
// at the envelope points. When two shots share one first frame the later one is current
// (the earlier one cannot be reached; this only happens when the project fps changed after
// the cuts were made). fps unknown (<= 0 or NaN): the shot times themselves, within
// `tolerance` seconds (the envelope rule); `tolerance` is only used then. `count` shots,
// `time_of(i)` the start time of shot i, ascending. -1 when there is none; a playhead
// before every shot is in the first one.
template <class TimeOf>
inline int VideoCurrentShotIndexOf(std::size_t count, TimeOf time_of, double playhead, double fps,
                                   double tolerance)
{
    if (count == 0) return -1;
    int idx = 0;
    if (!(fps > 0.0)) {
        for (std::size_t i = 0; i < count; ++i) {
            if (time_of(i) <= playhead + tolerance) idx = static_cast<int>(i);
        }
        return idx;
    }
    const long long k = VideoFrameIndexAt(playhead, fps);
    for (std::size_t i = 0; i < count; ++i) {
        if (VideoShotFirstFrame(time_of(i), fps) <= k) idx = static_cast<int>(i);
    }
    return idx;
}

// VideoCurrentShotIndexOf over a list of shot start times (video_shots.h has the overload
// taking the shots themselves).
inline int VideoCurrentShotIndex(const std::vector<double>& shot_times, double playhead, double fps,
                                 double tolerance)
{
    return VideoCurrentShotIndexOf(
        shot_times.size(), [&](std::size_t i) { return shot_times[i]; }, playhead, fps, tolerance);
}

// The time of the frame the playhead is in (frame / fps): where Video view evaluates the
// envelopes, so it shows the camera REAPER renders for that position (spec 11-fb-10).
// fps unknown (<= 0 or NaN): the playhead itself. A negative or non-finite playhead: 0.
inline double VideoPlayheadFrameTime(double playhead, double fps)
{
    if (!std::isfinite(playhead) || playhead <= 0.0) return 0.0;
    if (!(fps > 0.0)) return playhead;
    return static_cast<double>(VideoFrameIndexAt(playhead, fps)) / fps;
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

// Spec 11-fb-12 -- a cut never starts on a frame that begins before the start of the item
// under the playhead. An item whose start is off the frame grid (1.400 s at 24 fps = frame
// 33.6) has its first frame at the next whole frame (34); a cut with the playhead in frame 33
// (at the item start) would start on frame 33, which REAPER renders with the previous item.
// The cut's first frame: max(the playhead's frame, the item's first frame). `item_start`
// non-finite (NaN): no item under the playhead, the playhead's frame. fps unknown: unused.
inline long long VideoCutFrameInItem(double t, double fps, double item_start)
{
    long long k = VideoFrameIndexAt(t, fps);
    if (fps > 0.0 && std::isfinite(item_start)) k = std::max(k, VideoShotFirstFrame(item_start, fps));
    return k;
}

// Time of the envelope points of a cut made with the playhead at t in the item starting at
// `item_start` (NaN: no item): half a frame before VideoCutFrameInItem, like VideoCutTime.
// No item or fps unknown: VideoCutTime itself.
inline double VideoCutTimeInItem(double t, double fps, double item_start)
{
    if (!(fps > 0.0) || !std::isfinite(item_start)) return VideoCutTime(t, fps);
    const long long k = VideoCutFrameInItem(t, fps, item_start);
    if (k <= 0) return 0.0;
    return (static_cast<double>(k) - 0.5) / fps;
}

// VideoCutWouldDuplicate for a cut placed by VideoCutTimeInItem: true when a shot already
// starts on its frame. No item or fps unknown: VideoCutWouldDuplicate itself.
inline bool VideoCutWouldDuplicateInItem(const std::vector<double>& shot_times, double t, double fps,
                                         double item_start, double tolerance)
{
    if (!(fps > 0.0) || !std::isfinite(item_start)) return VideoCutWouldDuplicate(shot_times, t, fps, tolerance);
    const long long k = VideoCutFrameInItem(t, fps, item_start);
    for (double s : shot_times) {
        if (VideoShotFirstFrame(s, fps) == k) return true;
    }
    return false;
}

// ---- The shot strip's view window (spec 11-fb-12) ------------------------------------------
// The strip shows [v0, v0 + span] of project time: by default the current item plus a margin.

// The default margin on each side of an item `item_span` long: 15 % of it, at least 4 frames
// (fps unknown: 15 % only).
inline double VideoStripDefaultMargin(double item_span, double fps)
{
    if (!(item_span > 0.0) || !std::isfinite(item_span)) item_span = 0.0;
    const double m = 0.15 * item_span;
    return (fps > 0.0 && std::isfinite(fps)) ? std::max(m, 4.0 / fps) : m;
}

// The smallest visible span: 10 frames, or 0.4 s when the fps is unknown.
inline double VideoStripMinSpan(double fps)
{
    return (fps > 0.0 && std::isfinite(fps)) ? 10.0 / fps : 0.4;
}

// The largest visible span: 20 times the current item, never under the smallest one.
inline double VideoStripMaxSpan(double item_span, double fps)
{
    const double lo = VideoStripMinSpan(fps);
    if (!(item_span > 0.0) || !std::isfinite(item_span)) return lo;
    return std::max(lo, 20.0 * item_span);
}

// `span` kept within [min_span, max_span] (a bad span: min_span).
inline double VideoStripClampSpan(double span, double min_span, double max_span)
{
    if (!(span > 0.0) || !std::isfinite(span)) return min_span;
    if (max_span < min_span) max_span = min_span;
    return std::min(std::max(span, min_span), max_span);
}

// Alt + wheel (`notches` > 0 zooms in): the visible span changes by 1.25 per notch, within
// [min_span, max_span], and the time t_fixed (under the mouse) keeps its place on screen.
// Writes the new view start and span.
inline void VideoStripZoom(double v0, double span, double t_fixed, double notches, double min_span,
                           double max_span, double* out_v0, double* out_span)
{
    double ns = span;
    if (std::isfinite(notches) && span > 0.0 && std::isfinite(span)) ns = span * std::pow(1.25, -notches);
    ns = VideoStripClampSpan(ns, min_span, max_span);
    double nv0 = v0;
    if (span > 0.0 && std::isfinite(span) && std::isfinite(t_fixed) && std::isfinite(v0)) {
        nv0 = t_fixed - (t_fixed - v0) * (ns / span);
    }
    if (out_v0) *out_v0 = nv0;
    if (out_span) *out_span = ns;
}

// Spec 11-fb-14 -- Shift + wheel scrolls the view: `notches` > 0 (wheel up) moves it left
// (earlier), < 0 right, 10 % of the span per notch, the span kept. The view never starts
// before `min_v0` (the default margin before 0); a view already further left only moves right.
inline double VideoStripScroll(double v0, double span, double notches, double min_v0)
{
    if (!std::isfinite(v0) || !(span > 0.0) || !std::isfinite(span) || !std::isfinite(notches)) return v0;
    const double nv0 = v0 - notches * 0.1 * span;
    if (!std::isfinite(min_v0)) return nv0;
    return std::max(nv0, std::min(v0, min_v0));
}

// The view start after the smallest shift that brings t into [v0, v0 + span] (v0 itself
// when t is already visible).
inline double VideoStripFollow(double v0, double span, double t)
{
    if (!std::isfinite(t) || !std::isfinite(v0) || !(span > 0.0)) return v0;
    if (t < v0) return t;
    if (t > v0 + span) return t - span;
    return v0;
}

// The shot strip's ruler (spec 11-fb-6). With a known fps it counts whole project frames
// from the item's first frame F0 = VideoShotFirstFrame(origin, fps): tick k is frame
// F0 + k * step_frames, drawn at F / fps. A "second" is the rounded fps in frames (nominal
// seconds, like non-drop timecode). fps unknown: whole seconds from the item start.
// Spec 11-fb-12: the strip's top ruler passes origin 0 (project time, like REAPER's ruler);
// the clip-time labels pass the current item's start.

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

// The ruler's minor (unlabelled) tick step, in seconds, under the labelled step `step_s`
// (spec 11-fb-9): the finest "nice" step that divides the labelled step, is finer than it and
// keeps ticks at least `min_px` apart (min_px <= 0 or NaN: 1 px). Known fps, under a second:
// a frame count that divides the rounded fps (30 fps: 1, 2, 3, 5, 6, 10, 15 frames, never 4),
// so every whole second stays a tick and the ticks share the labelled ticks' frame grid; one
// tick per frame when zoomed in. A second and up: 1, 2, 5, 10, 15, 30 s, 1, 2, 5, 10, 15,
// 30 min, 1, 2, 5, 10 h (whole nominal seconds). fps unknown: 0.2 s, then the same seconds.
// 0: no minor ticks (nothing fits, no step, or an absurd span).
inline double VideoRulerMinorStep(double step_s, double span_s, double lane_px, double fps, double min_px)
{
    if (!(step_s > 0.0) || !std::isfinite(step_s)) return 0.0;
    if (!(span_s > 0.0) || !(lane_px > 0.0) || !std::isfinite(span_s) || !std::isfinite(lane_px)) return 0.0;
    if (!(min_px > 0.0) || !std::isfinite(min_px)) min_px = 1.0;
    const double px_per_s = lane_px / span_s;
    if (!(px_per_s > 0.0) || !std::isfinite(px_per_s)) return 0.0;
    static const double kNiceSeconds[] = {1.0,   2.0,    5.0,    10.0,   15.0,   30.0,    60.0,   120.0,
                                          300.0, 600.0,  900.0,  1800.0, 3600.0, 7200.0,  18000.0, 36000.0};
    const long long ifps = VideoRulerFps(fps);
    if (ifps > 0) {
        // Absurd spans (frame counts near overflow): no minor ticks.
        if (step_s * static_cast<double>(ifps) > 1.0e15) return 0.0;
        const long long sf = VideoRulerStepFrames(step_s, fps);
        if (sf <= 1) return 0.0;
        const double px_per_frame = px_per_s / fps;
        // Under a second: the smallest divisor d of ifps (d < ifps) that divides sf, is finer
        // than it and fits (divisors in pairs up to sqrt(ifps)).
        long long best = 0;
        auto consider = [&](long long d) {
            if (d >= ifps || d >= sf || sf % d != 0) return;
            if (static_cast<double>(d) * px_per_frame < min_px - 1e-9) return;
            if (best == 0 || d < best) best = d;
        };
        for (long long i = 1; i * i <= ifps; ++i) {
            if (ifps % i != 0) continue;
            consider(i);
            consider(ifps / i);
        }
        if (best > 0) return static_cast<double>(best) / fps;
        for (double c : kNiceSeconds) {
            const long long cf = static_cast<long long>(c) * ifps;
            if (cf >= sf) break;
            if (sf % cf == 0 && c * px_per_s >= min_px - 1e-9) return c;
        }
        return 0.0;
    }
    if (step_s > 1.0e12) return 0.0;  // absurd span
    auto divides = [&](double c) {
        const double q = step_s / c;
        return std::fabs(q - std::round(q)) < 1e-6;
    };
    if (0.2 < step_s - 1e-9 && divides(0.2) && 0.2 * px_per_s >= min_px - 1e-9) return 0.2;
    for (double c : kNiceSeconds) {
        if (c >= step_s - 1e-9) break;
        if (divides(c) && c * px_per_s >= min_px - 1e-9) return c;
    }
    return 0.0;
}

// The step Snap lands on (spec 11-fb-9): the finest graduation drawn, the minor step when
// there are minor ticks, else the labelled step.
inline double VideoRulerSnapStep(double step_s, double minor_s)
{
    return (minor_s > 0.0 && std::isfinite(minor_s)) ? minor_s : step_s;
}

// How many minor steps make one labelled step: minor tick k sits on a labelled tick when
// k % count == 0 (those are not drawn twice). Known fps: whole frames; fps unknown: the
// rounded ratio. 0 when there are no minor ticks, the minor step does not divide the
// labelled step, or the count is absurd.
inline long long VideoRulerMinorPerLabel(double step_s, double minor_s, double fps)
{
    if (!(step_s > 0.0) || !(minor_s > 0.0) || !std::isfinite(step_s) || !std::isfinite(minor_s)) return 0;
    if (!(minor_s < step_s)) return 0;
    const double ratio = step_s / minor_s;
    if (!(ratio < 1.0e9)) return 0;
    const long long ifps = VideoRulerFps(fps);
    if (ifps > 0) {
        if (step_s * static_cast<double>(ifps) > 1.0e15) return 0;
        const long long sf = VideoRulerStepFrames(step_s, fps);
        const long long mf = VideoRulerStepFrames(minor_s, fps);
        if (mf <= 0 || sf <= mf || sf % mf != 0) return 0;
        return sf / mf;
    }
    const long long n = std::llround(ratio);
    if (n <= 1 || std::fabs(ratio - static_cast<double>(n)) > 1e-6) return 0;
    return n;
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

// Spec 11-fb-12 -- the first ruler tick at or after t (k >= 0), so a ruler drawn over a
// visible window starts at its left edge instead of at tick 0. 0 without a step.
inline long long VideoRulerFirstTickFrom(double t, double origin, double step_s, double fps)
{
    if (!(step_s > 0.0) || !std::isfinite(step_s) || !std::isfinite(t)) return 0;
    const long long sf = VideoRulerStepFrames(step_s, fps);
    double k;
    if (sf > 0) {
        const double f0 = static_cast<double>(VideoShotFirstFrame(origin, fps));
        k = std::ceil((t * fps - f0) / static_cast<double>(sf) - 1e-6);
    } else {
        k = std::ceil((t - origin) / step_s - 1e-9);
    }
    if (!(k > 0.0)) return 0;
    if (k > 9.0e15) return 9000000000000000LL;
    return static_cast<long long>(k);
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
