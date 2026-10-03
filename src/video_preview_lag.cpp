// SPDX-License-Identifier: MIT
//
// See video_preview_lag.h.

#include "video_preview_lag.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

namespace rav {

bool PreviewWindowOpen(bool known, bool open, double now, bool has_request, double request_at)
{
    if (known) return open;
    return has_request && now - request_at <= kPreviewLagFallbackWindow && request_at <= now;
}

void PreviewLagMeter::Reset()
{
    samples_.clear();
    has_last_ = false;
    has_stale_ = false;
    held_lag_ = 0.0;
    has_held_ = false;
}

PreviewLagState PreviewLagMeter::Update(double now, bool window_open, bool has_request, double request_at,
                                        double requested_time, const PreviewLagTransport& t, double* out_lag)
{
    if (out_lag) *out_lag = 0.0;
    const double rate = (std::isfinite(t.playrate) && t.playrate > 0.0) ? t.playrate : 1.0;
    const bool looping = t.looping && t.loop_end > t.loop_start;

    // A seek while playing: the play position leaves its course (a loop wrap excepted).
    if (t.playing) {
        if (has_last_) {
            const double expected = last_pos_ + (now - last_now_) * rate;
            bool on_course = std::fabs(t.play_pos - expected) <= kPreviewLagJump;
            if (!on_course && looping && expected >= t.loop_end - kPreviewLagJump) {
                const double wrapped = t.loop_start + (expected - t.loop_end);
                on_course = std::fabs(t.play_pos - wrapped) <= kPreviewLagJump;
            }
            if (!on_course) {
                samples_.clear();
                held_lag_ = 0.0;
                has_held_ = false;
                stale_before_ = now;
                has_stale_ = true;
            }
        }
        has_last_ = true;
        last_now_ = now;
        last_pos_ = t.play_pos;
    } else {
        has_last_ = false;
        has_stale_ = false;
        held_lag_ = 0.0;  // play resumes: nothing measured yet
        has_held_ = false;
    }

    if (!window_open) {
        samples_.clear();
        held_lag_ = 0.0;
        has_held_ = false;
        return PreviewLagState::Hidden;
    }
    if (!t.playing) {
        samples_.clear();
        return PreviewLagState::Live;
    }
    samples_.erase(std::remove_if(samples_.begin(), samples_.end(),
                                  [now](const Sample& s) { return now - s.at > kPreviewLagWindow || s.at > now; }),
                   samples_.end());
    if (!has_request || now - request_at > kPreviewLagWindow) {
        // REAPER paused its requests (nothing changed): the last value shown holds.
        // Nothing measured yet (window opened, play started, seek): nothing to show.
        if (!has_held_) return PreviewLagState::Hidden;
        if (out_lag) *out_lag = held_lag_;
        return PreviewLagState::Lag;
    }
    if (has_stale_ && request_at <= stale_before_) {
        // Asked for before the seek: its time says nothing about the new position.
    } else {
        has_stale_ = false;
        double ahead = requested_time - t.play_pos;
        // Looping: near the loop end REAPER already asks for frames from the loop start.
        if (ahead < 0.0 && looping && requested_time >= t.loop_start && requested_time <= t.loop_end &&
            t.play_pos <= t.loop_end)
            ahead = (t.loop_end - t.play_pos) + (requested_time - t.loop_start);
        // Still behind the play position (seek, loop wrap): no lag.
        const double lead = std::max(0.0, ahead) / rate;
        samples_.push_back({now, std::isfinite(lead) ? lead : 0.0});
    }
    if (samples_.empty()) {
        // Only pre-seek requests so far: no value measured since the seek.
        if (!has_held_) return PreviewLagState::Hidden;
        if (out_lag) *out_lag = held_lag_;
        return PreviewLagState::Lag;
    }
    double peak = 0.0;
    for (const Sample& s : samples_) peak = std::max(peak, s.lag);
    // The epsilon keeps a lead like 12.35 - 10.0 (2.3499999...) rounding to 2.4.
    held_lag_ = std::round(peak * 10.0 + 1e-6) / 10.0;
    has_held_ = true;
    if (out_lag) *out_lag = held_lag_;
    return PreviewLagState::Lag;
}

namespace {

using Clock = std::chrono::steady_clock;

struct TrackRequest {
    const MediaTrack* track;
    double project_time;
    Clock::time_point at;
};

std::mutex g_mutex;                  // guards g_requests (video thread writes, main thread reads)
std::vector<TrackRequest> g_requests;  // the latest request per FX track: a handful at most
constexpr size_t kMaxTracks = 64;      // bound: an entry per track ever seen is dropped past this

// Main thread only.
PreviewLagMeter g_meter;
const MediaTrack* g_meter_track = nullptr;

double Seconds(Clock::time_point t)
{
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}

}  // namespace

void NoteVideoFrameRequest(const MediaTrack* track, double project_time)
{
    if (!std::isfinite(project_time)) return;
    const Clock::time_point now = Clock::now();
    std::lock_guard<std::mutex> lock(g_mutex);
    // A track not asked for within the fallback window (deleted, window closed) is dropped:
    // a new track at a reused address must not inherit its request.
    g_requests.erase(std::remove_if(g_requests.begin(), g_requests.end(),
                                    [now](const TrackRequest& r) {
                                        return std::chrono::duration<double>(now - r.at).count() >
                                               kPreviewLagFallbackWindow;
                                    }),
                     g_requests.end());
    for (TrackRequest& r : g_requests) {
        if (r.track == track) {
            r.project_time = project_time;
            r.at = now;
            return;
        }
    }
    if (g_requests.size() >= kMaxTracks) {
        // Drop the oldest entry (a track not asked for in the longest time).
        auto oldest = std::min_element(g_requests.begin(), g_requests.end(),
                                       [](const TrackRequest& a, const TrackRequest& b) { return a.at < b.at; });
        g_requests.erase(oldest);
    }
    g_requests.push_back({track, project_time, now});
}

bool VideoPreviewLag(const MediaTrack* track, const PreviewLagTransport& t, bool window_known, bool window_open,
                     double* out_lag, bool* out_live)
{
    if (out_lag) *out_lag = 0.0;
    if (out_live) *out_live = false;
    if (track != g_meter_track) {
        g_meter.Reset();
        g_meter_track = track;
    }
    bool has = false;
    double requested = 0.0;
    double at = 0.0;
    if (track) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const TrackRequest& r : g_requests) {
            if (r.track == track) {
                has = true;
                requested = r.project_time;
                at = Seconds(r.at);
                break;
            }
        }
    }
    if (!track) return false;
    const double now = Seconds(Clock::now());
    const bool open = PreviewWindowOpen(window_known, window_open, now, has, at);
    double lag = 0.0;
    const PreviewLagState st = g_meter.Update(now, open, has, at, requested, t, &lag);
    if (st == PreviewLagState::Hidden) return false;
    if (st == PreviewLagState::Live) {
        if (out_live) *out_live = true;
        return true;
    }
    if (out_lag) *out_lag = lag;
    return true;
}

}  // namespace rav
