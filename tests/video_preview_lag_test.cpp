// SPDX-License-Identifier: MIT
//
// Host test of the REAPER preview lag rule and probe (src/video_preview_lag.h, Epic 11,
// REAPER feedback 2026-10-03). No REAPER, no Windows: any C++17 compiler.
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
        CHECK(m.Update(100.0, true, 99.9, 12.4, 10.0, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.4));
        // Rounded to 0.1 s.
        CHECK(m.Update(100.1, true, 100.05, 12.47, 10.1, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.4));  // max(2.4, 2.37) = 2.4
        // The maximum of the last second holds: a smaller lead does not lower it at once.
        CHECK(m.Update(100.5, true, 100.5, 11.0, 10.5, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.4));
        // A second later, the old samples are gone.
        CHECK(m.Update(101.2, true, 101.2, 11.7, 11.2, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 0.5));
    }

    // Stopped: requests recent -> live.
    {
        PreviewLagMeter m;
        CHECK(m.Update(50.0, true, 49.8, 3.0, 3.0, false, &lag) == PreviewLagState::Live);
        CHECK(Near(lag, 0.0));
    }

    // Window closed: no request for over 1 s -> hidden, playing or not. No request at all -> hidden.
    {
        PreviewLagMeter m;
        CHECK(m.Update(50.0, true, 48.5, 13.0, 10.0, true, &lag) == PreviewLagState::Hidden);
        CHECK(m.Update(50.0, true, 48.5, 13.0, 10.0, false, &lag) == PreviewLagState::Hidden);
        CHECK(m.Update(50.0, false, 0.0, 0.0, 10.0, true, &lag) == PreviewLagState::Hidden);
    }

    // Seek / loop wrap: the latest request behind the play position -> 0.0 (clamped),
    // back to the real lag within ~1 s.
    {
        PreviewLagMeter m;
        CHECK(m.Update(10.0, true, 10.0, 2.0, 5.0, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 0.0));
        CHECK(m.Update(10.2, true, 10.2, 7.2, 5.2, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.0));
        // Loop wrap after a 2 s lag: the wrapped request is behind, the max holds ~1 s ...
        CHECK(m.Update(10.5, true, 10.5, 0.1, 5.5, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.0));
        // ... then the clamped samples rule.
        CHECK(m.Update(11.3, true, 11.3, 0.2, 6.3, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 0.0));
    }

    // The probe: requests for another track do not show for this one.
    {
        const MediaTrack* a = reinterpret_cast<const MediaTrack*>(0x1000);
        const MediaTrack* b = reinterpret_cast<const MediaTrack*>(0x2000);
        bool live = true;
        CHECK(!VideoPreviewLag(a, Play(0.0), &lag, &live));
        NoteVideoFrameRequest(b, 5.0);
        CHECK(!VideoPreviewLag(a, Play(3.0), &lag, &live));
        CHECK(VideoPreviewLag(b, Play(3.0), &lag, &live));
        CHECK(!live);
        CHECK(Near(lag, 2.0));
        CHECK(VideoPreviewLag(b, Play(3.0, false), &lag, &live));
        CHECK(live);
        CHECK(!VideoPreviewLag(nullptr, Play(3.0), &lag, &live));

        // Track switch while playing: b in Lag, then c with its own recent request.
        const MediaTrack* c = reinterpret_cast<const MediaTrack*>(0x3000);
        CHECK(VideoPreviewLag(b, Play(3.0), &lag, &live));
        CHECK(!live);
        CHECK(Near(lag, 2.0));
        NoteVideoFrameRequest(c, 3.5);
        CHECK(VideoPreviewLag(c, Play(3.0), &lag, &live));
        CHECK(!live);
        CHECK(Near(lag, 0.5));  // b's 2.0 does not carry over

        // A non-finite project time is ignored.
        const MediaTrack* d = reinterpret_cast<const MediaTrack*>(0x4000);
        NoteVideoFrameRequest(d, std::nan(""));
        NoteVideoFrameRequest(d, HUGE_VAL);
        CHECK(!VideoPreviewLag(d, Play(3.0), &lag, &live));
    }

    // Rounding of a single sample to 0.1 s.
    {
        PreviewLagMeter m1, m2, m3;
        CHECK(m1.Update(1.0, true, 1.0, 12.34, 10.0, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.3));
        CHECK(m2.Update(1.0, true, 1.0, 12.35, 10.0, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.4));
        CHECK(m3.Update(1.0, true, 1.0, 12.37, 10.0, true, &lag) == PreviewLagState::Lag);
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
        CHECK(m.Update(20.0, true, 20.0, 1.4, t, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.4));  // (10 - 9) + (1.4 - 0)
        // The play position wraps to the loop start: not a seek, the lead holds.
        t.play_pos = 9.9;
        CHECK(m.Update(20.9, true, 20.9, 2.3, t, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.4));
        t.play_pos = 0.1;
        CHECK(m.Update(21.1, true, 21.1, 2.5, t, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.4));
        // Repeat off: the same wrapped request is behind -> 0.0.
        PreviewLagMeter n;
        PreviewLagTransport u = Play(9.0);
        CHECK(n.Update(20.0, true, 20.0, 1.4, u, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 0.0));
    }

    // Backward seek while playing: the request still far ahead was asked for before the
    // seek -> no bogus lag; the first fresh request gives the real one.
    {
        PreviewLagMeter m;
        CHECK(m.Update(30.0, true, 29.95, 52.0, 50.0, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.0));
        CHECK(m.Update(30.1, true, 30.05, 52.1, 0.0, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 0.0));  // not +52.1
        CHECK(m.Update(30.2, true, 30.15, 2.1, 0.1, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.0));
        // Forward seek: same.
        CHECK(m.Update(30.3, true, 30.25, 2.2, 40.0, true, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 0.0));
    }

    // Playrate 2: 4 s of project time ahead = 2 s of delay.
    {
        PreviewLagMeter m;
        PreviewLagTransport t = Play(10.0);
        t.playrate = 2.0;
        CHECK(m.Update(5.0, true, 5.0, 14.0, t, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 2.0));
        // A playrate <= 0 reads as 1.
        PreviewLagMeter n;
        t.playrate = 0.0;
        CHECK(n.Update(5.0, true, 5.0, 14.0, t, &lag) == PreviewLagState::Lag);
        CHECK(Near(lag, 4.0));
    }

    if (g_fails == 0) std::printf("video_preview_lag: all passed\n");
    return g_fails == 0 ? 0 : 1;
}
