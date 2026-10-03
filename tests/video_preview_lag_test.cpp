// SPDX-License-Identifier: MIT
//
// Host test of the REAPER catch-up readout rule and probe (src/video_preview_lag.h,
// Epic 11, REAPER feedback 2026-10-03). No REAPER, no Windows: any C++17 compiler.
//   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests

#include "video_preview_lag.h"

#include <cmath>
#include <cstdio>

using namespace rav;

namespace {

int g_fails = 0;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);        \
            ++g_fails;                                              \
        }                                                           \
    } while (0)

bool Near(double a, double b)
{
    return std::fabs(a - b) < 1e-9;
}

PreviewLagTransport Play(double pos, bool playing = true)
{
    PreviewLagTransport t;
    t.play_pos = pos;
    t.playing = playing;
    return t;
}

}  // namespace

int main()
{
    double lag = -1.0;

    // Playing, window open: requests reach 12.4 s, play position 10.0 s.
    {
        PreviewLagMeter m;
        CHECK(m.Update(100.0, true, true, 99.9, 12.4, 10.0, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.4));
        // Rounded to 0.1 s.
        CHECK(m.Update(100.1, true, true, 100.05, 12.47, 10.1, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.4));  // max(2.4, 2.37) = 2.4
        // The maximum of the last second holds: a smaller lead does not lower it at once.
        CHECK(m.Update(100.5, true, true, 100.5, 11.0, 10.5, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.4));
        // A second later, the old samples are gone.
        CHECK(m.Update(101.2, true, true, 101.2, 11.7, 11.2, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 0.5));
    }

    // Stopped: requests recent -> live.
    {
        PreviewLagMeter m;
        CHECK(m.Update(50.0, true, true, 49.8, 3.0, 3.0, false, &lag) == PreviewLagState::Live);
        CHECK(Near(lag, 0.0));
    }

    // Window closed (window_open = false) -> hidden, playing or not, requests or not.
    {
        PreviewLagMeter m;
        CHECK(m.Update(50.0, false, true, 49.9, 13.0, 10.0, true, &lag) == PreviewLagState::Hidden);
        CHECK(m.Update(50.0, false, true, 49.9, 13.0, 10.0, false, &lag) == PreviewLagState::Hidden);
        CHECK(m.Update(50.0, false, false, 0.0, 0.0, 10.0, true, &lag) == PreviewLagState::Hidden);
    }

    // Spec 11-fb-5: window open, stopped, no request for minutes -> still live.
    {
        PreviewLagMeter m;
        CHECK(m.Update(500.0, true, true, 380.0, 3.0, 3.0, false, &lag) == PreviewLagState::Live);
        CHECK(m.Update(500.0, true, false, 0.0, 0.0, 3.0, false, &lag) == PreviewLagState::Live);
    }

    // Spec 11-fb-5: playing, REAPER pauses its requests for 2 s -> the last lag holds.
    {
        PreviewLagMeter m;
        CHECK(m.Update(100.0, true, true, 100.0, 12.4, 10.0, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.4));
        CHECK(m.Update(101.0, true, true, 100.0, 12.4, 11.0, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.4));  // the request is exactly 1 s old: still a sample
        CHECK(m.Update(102.0, true, true, 100.0, 12.4, 12.0, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.4));  // no request for 2 s: held, not hidden, not recomputed (0.4)
        // Requests resume: measured again.
        CHECK(m.Update(102.1, true, true, 102.1, 13.6, 12.1, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 1.5));
        // Closing the window drops the held value: reopened with no fresh request, nothing
        // is measured yet -> hidden (not a made-up 0.0).
        CHECK(m.Update(102.2, false, true, 102.2, 13.7, 12.2, true, &lag) == PreviewLagState::Hidden);
        CHECK(m.Update(104.0, true, true, 102.2, 13.7, 14.0, true, &lag) == PreviewLagState::Hidden);
        // The first fresh request measures again.
        CHECK(m.Update(104.1, true, true, 104.1, 15.1, 14.1, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 1.0));
    }

    // Held value, then a seek with no fresh request -> no stale value.
    {
        PreviewLagMeter m;
        CHECK(m.Update(100.0, true, true, 100.0, 12.4, 10.0, true, &lag) == PreviewLagState::Lag);
        CHECK(m.Update(102.0, true, true, 100.0, 12.4, 12.0, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.4));  // held
        CHECK(m.Update(102.1, true, true, 100.0, 12.4, 50.0, true, &lag) == PreviewLagState::Hidden);
        CHECK(m.Update(102.2, true, true, 102.15, 52.0, 50.1, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 1.9));
    }

    // Held value, then stop, then play with no fresh request -> no stale value.
    {
        PreviewLagMeter m;
        CHECK(m.Update(100.0, true, true, 100.0, 12.4, 10.0, true, &lag) == PreviewLagState::Lag);
        CHECK(m.Update(102.0, true, true, 100.0, 12.4, 12.0, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.4));  // held
        CHECK(m.Update(102.1, true, true, 100.0, 12.4, 12.0, false, &lag) == PreviewLagState::Live);
        CHECK(m.Update(102.2, true, true, 100.0, 12.4, 12.0, true, &lag) == PreviewLagState::Hidden);
    }

    // Window state unknown (action not found): open while a request is under 10 s old.
    {
        CHECK(PreviewWindowOpen(true, true, 100.0, false, 0.0));
        CHECK(!PreviewWindowOpen(true, false, 100.0, true, 100.0));
        CHECK(PreviewWindowOpen(false, false, 100.0, true, 91.0));
        CHECK(!PreviewWindowOpen(false, false, 100.0, true, 89.0));
        CHECK(!PreviewWindowOpen(false, true, 100.0, false, 0.0));
    }

    // Seek / loop wrap: the latest request behind the play position -> 0.0 (clamped),
    // back to the real lag within ~1 s.
    {
        PreviewLagMeter m;
        CHECK(m.Update(10.0, true, true, 10.0, 2.0, 5.0, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 0.0));
        CHECK(m.Update(10.2, true, true, 10.2, 7.2, 5.2, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.0));
        // Loop wrap after a 2 s lag: the wrapped request is behind, the max holds ~1 s ...
        CHECK(m.Update(10.5, true, true, 10.5, 0.1, 5.5, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.0));
        // ... then the clamped samples rule.
        CHECK(m.Update(11.3, true, true, 11.3, 0.2, 6.3, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 0.0));
    }

    // The probe: requests for another track do not show for this one.
    {
        const MediaTrack* a = reinterpret_cast<const MediaTrack*>(0x1000);
        const MediaTrack* b = reinterpret_cast<const MediaTrack*>(0x2000);
        bool live = true;
        CHECK(!VideoPreviewLag(a, Play(0.0), false, false, &lag, &live));
        NoteVideoFrameRequest(b, 5.0);
        CHECK(!VideoPreviewLag(a, Play(3.0), false, false, &lag, &live));
        CHECK(VideoPreviewLag(b, Play(3.0), true, true, &lag, &live));
        CHECK(!live);
        CHECK(Near(lag, 2.0));
        CHECK(VideoPreviewLag(b, Play(3.0, false), true, true, &lag, &live));
        CHECK(live);
        CHECK(!VideoPreviewLag(nullptr, Play(3.0), true, true, &lag, &live));

        // Track switch while playing: b in Lag, then c with its own recent request.
        const MediaTrack* c = reinterpret_cast<const MediaTrack*>(0x3000);
        CHECK(VideoPreviewLag(b, Play(3.0), true, true, &lag, &live));
        CHECK(!live);
        CHECK(Near(lag, 2.0));
        NoteVideoFrameRequest(c, 3.5);
        CHECK(VideoPreviewLag(c, Play(3.0), true, true, &lag, &live));
        CHECK(!live);
        CHECK(Near(lag, 0.5));  // b's 2.0 does not carry over

        // Track switch while playing: b in Lag (2.0), then e with no recent request ->
        // nothing measured for e: hidden, not b's 2.0.
        CHECK(VideoPreviewLag(b, Play(3.0), true, true, &lag, &live));
        CHECK(Near(lag, 2.0));
        const MediaTrack* e = reinterpret_cast<const MediaTrack*>(0x5000);
        lag = -1.0;
        CHECK(!VideoPreviewLag(e, Play(3.0), true, true, &lag, &live));
        CHECK(!Near(lag, 2.0));

        // A non-finite project time is ignored.
        const MediaTrack* d = reinterpret_cast<const MediaTrack*>(0x4000);
        NoteVideoFrameRequest(d, std::nan(""));
        NoteVideoFrameRequest(d, HUGE_VAL);
        CHECK(!VideoPreviewLag(d, Play(3.0), false, false, &lag, &live));

        // Window known open, d never asked for: stopped -> live; known closed -> hidden.
        CHECK(VideoPreviewLag(d, Play(3.0, false), true, true, &lag, &live));
        CHECK(live);
        CHECK(!VideoPreviewLag(d, Play(3.0, false), true, false, &lag, &live));
        // Unknown window state: b's recent request stands in for it.
        CHECK(VideoPreviewLag(b, Play(3.0, false), false, false, &lag, &live));
        CHECK(live);
    }

    // Rounding of a single sample to 0.1 s.
    {
        PreviewLagMeter m1, m2, m3;
        CHECK(m1.Update(1.0, true, true, 1.0, 12.34, 10.0, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.3));
        CHECK(m2.Update(1.0, true, true, 1.0, 12.35, 10.0, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.4));
        CHECK(m3.Update(1.0, true, true, 1.0, 12.37, 10.0, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.4));
    }

    // Loop playback (loop 0..10 s, repeat on): near the loop end REAPER asks for frames
    // from the loop start; the lead runs through the wrap.
    {
        PreviewLagMeter m;
        PreviewLagTransport t = Play(9.0);
        t.looping = true;
        t.loop_start = 0.0;
        t.loop_end = 10.0;
        CHECK(m.Update(20.0, true, true, 20.0, 1.4, t, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.4));  // (10 - 9) + (1.4 - 0)
        // The play position wraps to the loop start: not a seek, the lead holds.
        t.play_pos = 9.9;
        CHECK(m.Update(20.9, true, true, 20.9, 2.3, t, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.4));
        t.play_pos = 0.1;
        CHECK(m.Update(21.1, true, true, 21.1, 2.5, t, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.4));
        // Repeat off: the same wrapped request is behind -> 0.0.
        PreviewLagMeter n;
        PreviewLagTransport u = Play(9.0);
        CHECK(n.Update(20.0, true, true, 20.0, 1.4, u, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 0.0));
    }

    // Backward seek while playing: the request still far ahead was asked for before the
    // seek -> no bogus lag; the first fresh request gives the real one.
    {
        PreviewLagMeter m;
        CHECK(m.Update(30.0, true, true, 29.95, 52.0, 50.0, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.0));
        // Only the pre-seek request: nothing measured since the seek -> hidden (not +52.1).
        CHECK(m.Update(30.1, true, true, 30.05, 52.1, 0.0, true, &lag) == PreviewLagState::Hidden);
        CHECK(m.Update(30.2, true, true, 30.15, 2.1, 0.1, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.0));
        // Forward seek: same.
        CHECK(m.Update(30.3, true, true, 30.25, 2.2, 40.0, true, &lag) == PreviewLagState::Hidden);
    }

    // Playrate 2: 4 s of project time ahead = 2 s of delay.
    {
        PreviewLagMeter m;
        PreviewLagTransport t = Play(10.0);
        t.playrate = 2.0;
        CHECK(m.Update(5.0, true, true, 5.0, 14.0, t, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.0));
        // A playrate <= 0 reads as 1.
        PreviewLagMeter n;
        t.playrate = 0.0;
        CHECK(n.Update(5.0, true, true, 5.0, 14.0, t, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 4.0));
    }

    if (g_fails == 0) std::printf("video_preview_lag: all passed\n");
    return g_fails == 0 ? 0 : 1;
}
