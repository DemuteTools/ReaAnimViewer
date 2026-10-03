// SPDX-License-Identifier: MIT
//
// Shots belong to clips (spec 11-fb-15). Pure, header-only, no REAPER.
//
// The envelopes hold "raw" shots (video_shots.h): a time and a point on each camera
// parameter. Video view shows them per clip (the RAV animation items of the FX track):
//  - each clip starts its own shot on its first frame (the first frame at or after the
//    item start). Until edited it is IMPLICIT: no envelope point, it holds the camera the
//    envelopes give there (the point is written when an edit needs one);
//  - a raw shot belongs to the clip holding its first frame; raw shots in gaps, before the
//    first clip or after the last are not shown (their points stay where they are);
//  - a shot ends at the next shot's start, else at its clip's end. A shot with the SPAN
//    flag (stored in the FX state, video_fx_state.h) runs on through the start of the next
//    clip when it is back-to-back (the first frame after the clip is that clip's first
//    frame): that clip then gets no implicit first shot.
// fps unknown (<= 0 or NaN): times themselves instead of frames.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#include "video_shot_timing.h"

namespace rav {

// A clip as the timeline gives it: [start, end) in project time and its animation name.
struct VideoClipIn {
    double      start = 0.0;
    double      end = 0.0;
    std::string anim_name;
};

// A raw shot (envelope points), in time order.
struct VideoRawShotIn {
    double time = 0.0;
    bool   stored_name = false;  // the user named it (else "<anim name> N")
    bool   span = false;         // runs on through the next back-to-back clip start
};

// A clip as Video view uses it: overlapping items start where the previous one ends
// (effective start), empty ones are dropped. `first` = its first frame's time, `stop` = the
// time of the first frame after it (fps unknown: start and end).
struct VideoClip {
    double      start = 0.0;  // effective start
    double      end = 0.0;
    double      first = 0.0;
    double      stop = 0.0;
    std::string anim_name;
};

struct VideoClipShot {
    int    clip = -1;         // index into VideoClipShots::clips
    int    raw = -1;          // index into the raw shots, -1 = the clip's implicit first shot
    double start = 0.0;       // first frame (display start)
    double end = 0.0;         // display end (exclusive): the next shot's start or the clip's stop
    int    number = 1;        // 1-based within its clip
    bool   clip_first = false;  // opens its clip (on its first frame)
};

struct VideoClipShots {
    std::vector<VideoClip>     clips;  // in time order
    std::vector<VideoClipShot> shots;  // in time order
};

namespace detail {

// Times compared as frames: with a known fps every time here is k / fps (the same k gives the
// same double), so == and < are exact; fps unknown: within half a millisecond.
inline bool ClipSameTime(double a, double b, double fps)
{
    if (fps > 0.0) return a == b;
    return std::fabs(a - b) <= 0.0005;
}

// The first frame at or after t, as a time (fps unknown: t itself; t <= 0: 0).
inline double ClipFrameTime(double t, double fps)
{
    if (!std::isfinite(t)) return t;
    return VideoShotFirstFrameTime(t, fps);
}

}  // namespace detail

// The animation name of a clip: its file name without folders and extension ("Clip" when empty).
inline std::string VideoAnimNameFromPath(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
    if (name.empty()) name = "Clip";
    return name;
}

// The automatic name of shot `number` of a clip: "<anim name> N".
inline std::string VideoAutoShotName(const std::string& anim_name, int number)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), " %d", number);
    return anim_name + buf;
}

// The envelope time the implicit first shot of a clip starting at `clip_start` gets when it is
// written: half a frame before the clip's first frame, like a cut there (fps unknown: the start).
inline double VideoClipImplicitTime(double clip_start, double fps)
{
    return VideoCutTimeInItem(clip_start, fps, clip_start);
}

// True when clip c + 1 follows clip c without a gap (its first frame is the frame after c).
inline bool VideoClipsBackToBack(const std::vector<VideoClip>& clips, std::size_t c, double fps)
{
    return c + 1 < clips.size() && detail::ClipSameTime(clips[c + 1].first, clips[c].stop, fps);
}

// The index of the clip holding the frame that starts at `frame_time` (a first-frame time),
// -1 in a gap.
inline int VideoClipIndexAtFrame(const std::vector<VideoClip>& clips, double frame_time, double fps)
{
    for (std::size_t c = 0; c < clips.size(); ++c) {
        const bool after_first = clips[c].first < frame_time || detail::ClipSameTime(clips[c].first, frame_time, fps);
        const bool before_stop = clips[c].stop > frame_time && !detail::ClipSameTime(clips[c].stop, frame_time, fps);
        if (after_first && before_stop) return static_cast<int>(c);
    }
    return -1;
}

// The clip a cut with the playhead at t goes in: the clip whose [start, end) holds t (spec
// 11-fb-12: a playhead on an off-grid clip start cuts on that clip's first frame), else the
// clip holding the playhead's frame; -1 between clips.
inline int VideoClipIndexAtTime(const std::vector<VideoClip>& clips, double t, double fps)
{
    if (!std::isfinite(t)) return -1;
    for (std::size_t c = 0; c < clips.size(); ++c) {
        if (clips[c].start <= t && t < clips[c].end) return static_cast<int>(c);
    }
    return VideoClipIndexAtFrame(clips, VideoPlayheadFrameTime(t, fps), fps);
}

inline VideoClipShots BuildVideoClipShots(const std::vector<VideoClipIn>& clips_in,
                                          const std::vector<VideoRawShotIn>& raws, double fps)
{
    if (!(fps > 0.0) || !std::isfinite(fps)) fps = 0.0;
    VideoClipShots out;

    // ---- Clips: sorted by start, overlaps trimmed, empty ones dropped ----
    std::vector<std::size_t> order(clips_in.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return clips_in[a].start < clips_in[b].start; });
    double running_end = -std::numeric_limits<double>::infinity();
    for (std::size_t i : order) {
        const VideoClipIn& in = clips_in[i];
        if (!std::isfinite(in.start) || !std::isfinite(in.end)) continue;
        VideoClip c;
        c.start = std::max(in.start, running_end);
        c.end = in.end;
        running_end = std::max(running_end, in.end);
        if (!(c.end > c.start)) continue;
        c.first = detail::ClipFrameTime(c.start, fps);
        c.stop = detail::ClipFrameTime(c.end, fps);
        if (!(c.stop > c.first) || detail::ClipSameTime(c.stop, c.first, fps)) continue;  // no frame of its own
        c.anim_name = in.anim_name;
        out.clips.push_back(c);
    }
    if (out.clips.empty()) return out;

    // ---- Raw shots: each in the clip holding its first frame (later wins on one frame) ----
    std::vector<std::size_t> raw_order(raws.size());
    std::iota(raw_order.begin(), raw_order.end(), std::size_t{0});
    std::stable_sort(raw_order.begin(), raw_order.end(),
                     [&](std::size_t a, std::size_t b) { return raws[a].time < raws[b].time; });
    std::vector<std::vector<std::pair<int, double>>> per_clip(out.clips.size());  // (raw index, first frame)
    for (std::size_t r : raw_order) {
        if (!std::isfinite(raws[r].time)) continue;
        const double ft = detail::ClipFrameTime(raws[r].time, fps);
        const int c = VideoClipIndexAtFrame(out.clips, ft, fps);
        if (c < 0) continue;  // in a gap, before the first clip or after the last
        std::vector<std::pair<int, double>>& list = per_clip[static_cast<std::size_t>(c)];
        if (!list.empty() && detail::ClipSameTime(list.back().second, ft, fps)) list.pop_back();
        list.push_back({static_cast<int>(r), ft});
    }

    // ---- Shots, clip by clip ----
    for (std::size_t c = 0; c < out.clips.size(); ++c) {
        const VideoClip& clip = out.clips[c];
        const std::vector<std::pair<int, double>>& list = per_clip[c];
        const bool raw_on_first = !list.empty() && detail::ClipSameTime(list.front().second, clip.first, fps);
        int number = 0;
        if (!raw_on_first) {
            // The shot running into this clip spans it: only from the previous clip, back-to-back.
            bool spanned = false;
            if (c > 0 && !out.shots.empty() && VideoClipsBackToBack(out.clips, c - 1, fps)) {
                const VideoClipShot& prev = out.shots.back();
                spanned = prev.clip == static_cast<int>(c) - 1 && prev.raw >= 0 &&
                          raws[static_cast<std::size_t>(prev.raw)].span;
            }
            if (!spanned) {
                VideoClipShot s;
                s.clip = static_cast<int>(c);
                s.raw = -1;
                s.start = clip.first;
                s.number = ++number;
                s.clip_first = true;
                out.shots.push_back(s);
            }
        }
        for (std::size_t k = 0; k < list.size(); ++k) {
            VideoClipShot s;
            s.clip = static_cast<int>(c);
            s.raw = list[k].first;
            s.start = list[k].second;
            s.number = ++number;
            s.clip_first = (k == 0 && raw_on_first);
            out.shots.push_back(s);
        }
    }

    // ---- Ends ----
    for (std::size_t i = 0; i < out.shots.size(); ++i) {
        VideoClipShot& s = out.shots[i];
        const std::size_t c = static_cast<std::size_t>(s.clip);
        double limit = out.clips[c].stop;
        if (s.raw >= 0 && raws[static_cast<std::size_t>(s.raw)].span && VideoClipsBackToBack(out.clips, c, fps))
            limit = out.clips[c + 1].stop;
        s.end = (i + 1 < out.shots.size()) ? std::min(out.shots[i + 1].start, limit) : limit;
    }
    return out;
}

// The index of the shot whose [start, end) holds the frame the playhead t is in, -1 in a gap
// (or before / after every clip). `list`: shots with `start` and `end` (VideoClipShot or a
// type carrying them), in time order.
template <class Shot>
inline int VideoClipShotAt(const std::vector<Shot>& list, double t, double fps)
{
    if (!std::isfinite(t)) return -1;
    if (fps > 0.0) {
        const long long k = VideoFrameIndexAt(t, fps);
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (VideoShotFirstFrame(list[i].start, fps) <= k && k < VideoShotFirstFrame(list[i].end, fps))
                return static_cast<int>(i);
        }
        return -1;
    }
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].start <= t + 0.0005 && t + 0.0005 < list[i].end) return static_cast<int>(i);
    }
    return -1;
}

// After a junction move (spec 11-fb-15): true when the shot starting at `shot_start` (its
// first frame) covers the start of the next clip, i.e. the first clip after the shot's clip
// is back-to-back with it and starts before `next_start`'s first frame (the next shot's
// start; NaN or +inf: no next shot). That shot then needs the span flag.
inline bool VideoShotCoversNextClip(double shot_start, double next_start, const std::vector<VideoClip>& clips,
                                    double fps)
{
    if (!(fps > 0.0) || !std::isfinite(fps)) fps = 0.0;
    const int c = VideoClipIndexAtFrame(clips, detail::ClipFrameTime(shot_start, fps), fps);
    if (c < 0 || !VideoClipsBackToBack(clips, static_cast<std::size_t>(c), fps)) return false;
    if (!std::isfinite(next_start)) return true;
    const double next_first = detail::ClipFrameTime(next_start, fps);
    const double b = clips[static_cast<std::size_t>(c) + 1].first;
    return b < next_first && !detail::ClipSameTime(b, next_first, fps);
}

}  // namespace rav
