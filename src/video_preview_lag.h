// SPDX-License-Identifier: MIT
//
// How far ahead REAPER's Video window renders while playing (Epic 11, REAPER feedback
// 2026-10-03). REAPER asks the RAV video FX for frames a few seconds before they show,
// so a camera, shot or display change reaches its Video window that much later. Video
// view is exact, and renders have no offset (every frame is drawn for its own time).
//
// The video thread notes each frame request (track + project time); the main thread
// compares the latest one with the play position. The lag is a lead measurement:
// REAPER does not say which frame its window displays, but the frames it asks for are
// the ones a change has to wait behind.
//
// Visibility (spec 11-fb-5) comes from the caller: whether REAPER's Video window is open
// (its show/hide toggle action). REAPER stops asking for frames when nothing changes, so
// the request flow cannot say it. Only when that action cannot be found does a request in
// the last kPreviewLagFallbackWindow seconds stand in for it.
//
// No REAPER call and no Windows here: host-tested (tests/video_preview_lag_test.cpp).

#pragma once

#include <vector>

class MediaTrack;

namespace rav {

// ---- Pure rule (no clock, no REAPER) ---------------------------------------------------------
constexpr double kPreviewLagWindow = 1.0;  // seconds: lag samples (and requests feeding them) older than this are dropped
constexpr double kPreviewLagFallbackWindow = 10.0;  // seconds: window state unknown -> open while a request is this recent

enum class PreviewLagState {
    Hidden,  // REAPER's Video window is closed, or playing with nothing measured yet
    Live,    // stopped: its window shows the playhead
    Lag,     // playing: REAPER's catch-up delay = the largest lead of the last second, rounded
             // to 0.1 s; with no request in the last second, the last value measured is held
};

// Whether the readout's window counts as open. known: the Video window's toggle state was
// read (open = that state). Not known: open while a request was noted in the last
// kPreviewLagFallbackWindow seconds (now / request_at on one monotonic clock).
bool PreviewWindowOpen(bool known, bool open, double now, bool has_request, double request_at);

constexpr double kPreviewLagJump = 0.25;  // seconds: a play position this far off its course = a seek

// The main thread's view of the transport: the play position, and what turns project time
// into a delay (loop range when the project repeats, the playrate).
struct PreviewLagTransport {
    double play_pos = 0.0;
    bool   playing = false;
    bool   looping = false;     // repeat on and loop_end > loop_start
    double loop_start = 0.0;
    double loop_end = 0.0;
    double playrate = 1.0;      // project playrate (<= 0 or non-finite reads as 1)
};

// The main thread's view of one FX track's requests: the lag samples of the last second.
class PreviewLagMeter {
public:
    // now / request_at: seconds on one monotonic clock. window_open: REAPER's Video window
    // is open (PreviewWindowOpen); closed = Hidden. has_request: a request was noted for the
    // track (request_at = when, requested_time = its project time). Playing with no request
    // in the last kPreviewLagWindow: the last lag measured is held (REAPER pauses its
    // requests). The held value is dropped on stop, seek, window close and Reset(); until a
    // new value is measured, playing reads as Hidden (never a made-up 0.0).
    // Looping: a request behind the play position but inside the loop is ahead by
    // (loop_end - play_pos) + (requested - loop_start). The lead is divided by the playrate.
    // A seek while playing (the play position leaves its course by more than
    // kPreviewLagJump, a loop wrap excepted) clears the samples and ignores the requests
    // noted before it until a fresh one arrives.
    PreviewLagState Update(double now, bool window_open, bool has_request, double request_at,
                           double requested_time, const PreviewLagTransport& t, double* out_lag);
    PreviewLagState Update(double now, bool window_open, bool has_request, double request_at,
                           double requested_time, double play_pos, bool playing, double* out_lag)
    {
        PreviewLagTransport t;
        t.play_pos = play_pos;
        t.playing = playing;
        return Update(now, window_open, has_request, request_at, requested_time, t, out_lag);
    }
    void Reset();

private:
    struct Sample {
        double at;
        double lag;
    };
    std::vector<Sample> samples_;
    bool   has_last_ = false;  // playing: the previous call's clock and play position
    double last_now_ = 0.0;
    double last_pos_ = 0.0;
    double stale_before_ = 0.0;  // requests noted at or before this are pre-seek (valid if has_stale_)
    bool   has_stale_ = false;
    double held_lag_ = 0.0;    // the last lag measured (held while REAPER pauses its requests)
    bool   has_held_ = false;  // held_lag_ was measured since the last stop / seek / close / Reset
};

// ---- Probe -------------------------------------------------------------------------------------
// Video thread: REAPER asked the FX on `track` for the frame at project_time. Cheap (a
// short lock), no REAPER call.
void NoteVideoFrameRequest(const MediaTrack* track, double project_time);

// Main thread, once per frame: the readout for `track` (the Video view's FX track), with
// the transport and REAPER's Video window state read on the main thread (window_known =
// the state was read; else the request fallback applies, see PreviewWindowOpen).
// false = hidden. true: *out_live = stopped ("live"), else *out_lag = the lag in seconds
// (>= 0, rounded to 0.1 s).
bool VideoPreviewLag(const MediaTrack* track, const PreviewLagTransport& t, bool window_known, bool window_open,
                     double* out_lag, bool* out_live);

}  // namespace rav
