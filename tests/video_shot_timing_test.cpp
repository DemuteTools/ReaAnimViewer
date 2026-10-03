// SPDX-License-Identifier: MIT
//
// Host test of the pure cut / first-frame / duplicate rules (src/video_shot_timing.h,
// Epic 11, Story 11-5). No REAPER, no Windows: any C++17 compiler.
//   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
// or: c++ -std=c++17 -I src tests/video_shot_timing_test.cpp -o t && ./t

#include "video_shot_timing.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

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

}  // namespace

int main()
{
    // 30 fps: the playhead in frame 75 (2.5 s .. 2.5333 s).
    CHECK(VideoFrameIndexAt(2.51, 30.0) == 75);
    CHECK(VideoFrameIndexAt(2.5 - 1e-12, 30.0) == 75);  // float error under a frame start counts as that frame
    CHECK(Near(VideoCutTime(2.51, 30.0), 74.5 / 30.0));
    CHECK(Near(VideoCutTime(0.01, 30.0), 0.0));    // frame 0 -> 0
    CHECK(Near(VideoCutTime(1.234, 0.0), 1.234));  // fps unknown: at the playhead
    CHECK(Near(VideoCutTime(-3.0, 30.0), 0.0));

    // The first frame of a cut shot = the playhead's frame.
    CHECK(Near(VideoShotFirstFrameTime(74.5 / 30.0, 30.0), 2.5));
    CHECK(Near(VideoShotFirstFrameTime(2.5, 30.0), 2.5));
    CHECK(Near(VideoShotFirstFrameTime(2.5 + 1e-9, 30.0), 2.5));
    CHECK(Near(VideoShotFirstFrameTime(0.0, 30.0), 0.0));

    // Frame k sampled at k/fps -/+ epsilon falls in the new shot, frame k-1 before it; the
    // round trip first-frame(cut-time(t)) gives the playhead's frame.
    for (double fps : {23.976, 24.0, 25.0, 29.97, 30.0, 59.94, 60.0}) {
        for (long long k = 1; k < 20000; k += 7) {
            const double playhead = k / fps + 0.3 / fps;
            const double cut = VideoCutTime(playhead, fps);
            for (double eps : {-1e-9, 0.0, 1e-9}) {
                const double tk = k / fps + eps;
                const double tk1 = (k - 1) / fps + eps;
                if (!(tk >= cut)) {
                    std::printf("FAIL fps %g k %lld: frame not in the new shot\n", fps, k);
                    ++g_fails;
                    break;
                }
                if (!(tk1 < cut)) {
                    std::printf("FAIL fps %g k %lld: previous frame in the new shot\n", fps, k);
                    ++g_fails;
                    break;
                }
            }
            if (VideoShotFirstFrame(cut, fps) != k || VideoFrameIndexAt(playhead, fps) != k) {
                std::printf("FAIL fps %g k %lld: first frame round trip\n", fps, k);
                ++g_fails;
            }
        }
    }

    // Duplicates: per frame with a known rate, within the tolerance without one.
    const std::vector<double> shots = {0.0, 74.5 / 30.0, 3.0};
    CHECK(VideoCutWouldDuplicate(shots, 2.52, 30.0, 0.0005));    // same frame 75
    CHECK(!VideoCutWouldDuplicate(shots, 2.54, 30.0, 0.0005));   // frame 76
    CHECK(VideoCutWouldDuplicate(shots, 3.01, 30.0, 0.0005));    // a shot at exactly 3.0 = frame 90
    CHECK(!VideoCutWouldDuplicate({89.0 / 30.0}, 3.01, 30.0, 0.0005));  // frame 89, not 90
    CHECK(VideoCutWouldDuplicate(shots, 0.01, 30.0, 0.0005));    // frame 0 against the shot at 0
    CHECK(VideoCutWouldDuplicate({1.0}, 1.0002, 0.0, 0.0005));   // fps unknown: within the tolerance
    CHECK(!VideoCutWouldDuplicate({1.0}, 1.01, 0.0, 0.0005));

    // Junction drag (feedback 11-fb-2): shots at 0 / 2.0 / 5.0 s, 30 fps, the 2.0 junction.
    CHECK(Near(VideoJunctionDragTime(3.01, 0.0, 5.0, 30.0), 89.5 / 30.0));   // retime right: first frame 90
    CHECK(VideoShotFirstFrame(VideoJunctionDragTime(3.01, 0.0, 5.0, 30.0), 30.0) == 90);
    CHECK(Near(VideoJunctionDragTime(1.0, 0.0, 5.0, 30.0), 29.5 / 30.0));    // retime left
    CHECK(Near(VideoJunctionDragTime(6.0, 0.0, 5.0, 30.0), 148.5 / 30.0));   // clamp at next: frame 149
    CHECK(Near(VideoJunctionDragTime(5.0, 0.0, 5.0, 30.0), 148.5 / 30.0));   // on the next shot's frame
    CHECK(Near(VideoJunctionDragTime(-1.0, 0.0, 5.0, 30.0), 0.5 / 30.0));    // clamp at previous: frame 1
    CHECK(Near(VideoJunctionDragTime(0.01, 0.0, 5.0, 30.0), 0.5 / 30.0));
    CHECK(Near(VideoJunctionDragTime(0.5, 1.0, 5.0, 30.0), 30.5 / 30.0));    // previous at frame 30: frame 31
    CHECK(Near(VideoJunctionDragTime(4.0, 0.0, 74.5 / 30.0, 30.0), 73.5 / 30.0));  // next on a cut time: frame 74
    CHECK(VideoShotFirstFrame(VideoJunctionDragTime(2.0, 0.0, 5.0, 30.0), 30.0) == 60);  // back on its own frame
    // fps unknown: unsnapped, 1 ms margin.
    CHECK(Near(VideoJunctionDragTime(3.0123, 0.0, 5.0, 0.0), 3.0123));
    CHECK(Near(VideoJunctionDragTime(9.0, 0.0, 5.0, 0.0), 4.999));
    CHECK(Near(VideoJunctionDragTime(-2.0, 0.0, 5.0, 0.0), 0.001));
    CHECK(Near(VideoJunctionDragTime(0.5, 1.0, 5.0, 0.0), 1.001));
    // No frame between the neighbours: NaN (no move), never onto a neighbour's frame.
    CHECK(std::isnan(VideoJunctionDragTime(2.0, 30.5 / 30.0, 31.5 / 30.0, 30.0)));
    CHECK(std::isnan(VideoJunctionDragTime(2.0, 1.0, 1.0015, 0.0)));
    // The UI feeds the junction's own time + the drag delta + half a frame: no drag = same frame.
    CHECK(VideoShotFirstFrame(VideoJunctionDragTime(59.5 / 30.0 + 0.5 / 30.0, 0.0, 5.0, 30.0), 30.0) == 60);

    // Ruler step (spec 11-fb-6): labelled ticks at least 60 px apart.
    CHECK(Near(VideoRulerStep(3.0, 600.0, 30.0, 60.0), 10.0 / 30.0));    // zoomed in: 10 frames (66.7 px)
    CHECK(VideoRulerStep(3.0, 600.0, 30.0, 60.0) < 1.0);                  // frame-based
    CHECK(Near(VideoRulerStep(1.0, 1200.0, 30.0, 60.0), 2.0 / 30.0));     // 1200 px/s: 2 frames (80 px)
    CHECK(Near(VideoRulerStep(0.1, 1200.0, 30.0, 60.0), 1.0 / 30.0));     // one frame is wide enough
    CHECK(Near(VideoRulerStep(3.0, 600.0, 24.0, 60.0), 8.0 / 24.0));      // 24 fps: 5 and 10 skip (no divisor)
    CHECK(Near(VideoRulerStep(120.0, 600.0, 30.0, 60.0), 30.0));          // zoomed out: 30 s
    CHECK(Near(VideoRulerStep(60.0, 600.0, 30.0, 60.0), 10.0));           // 10 s
    CHECK(Near(VideoRulerStep(10.0, 600.0, 30.0, 60.0), 1.0));            // 15 frames = 30 px: 1 s
    CHECK(Near(VideoRulerStep(3.0, 600.0, 0.0, 60.0), 1.0));              // fps unknown: seconds only
    CHECK(Near(VideoRulerStep(0.5, 600.0, 0.0, 60.0), 1.0));
    CHECK(Near(VideoRulerStep(300000.0, 600.0, 30.0, 60.0), 3600.0 * 9.0));  // huge: whole hours
    CHECK(VideoRulerStep(0.0, 600.0, 30.0, 60.0) == 0.0);                 // nothing to draw
    CHECK(VideoRulerStep(3.0, 0.0, 30.0, 60.0) == 0.0);
    for (double span : {0.2, 1.0, 3.0, 17.0, 120.0, 900.0}) {
        for (double fps : {0.0, 24.0, 25.0, 29.97, 30.0, 60.0}) {
            const double step = VideoRulerStep(span, 600.0, fps, 60.0);
            CHECK(step / span * 600.0 >= 60.0 - 1e-9);
        }
    }

    // The step is the SMALLEST allowed one: the next smaller allowed step is under min_px.
    CHECK(Near(VideoRulerStep(3.0, 600.0, 30.0, 60.0), 10.0 / 30.0) && 6.0 / 30.0 * 200.0 < 60.0);  // 6f = 40 px
    CHECK(Near(VideoRulerStep(1.0, 1200.0, 30.0, 60.0), 2.0 / 30.0) && 1.0 / 30.0 * 1200.0 < 60.0);  // 1f = 40 px
    CHECK(Near(VideoRulerStep(10.0, 600.0, 30.0, 60.0), 1.0) && 15.0 / 30.0 * 60.0 < 60.0);  // 15f = 30 px
    CHECK(Near(VideoRulerStep(120.0, 600.0, 30.0, 60.0), 30.0) && 10.0 * 5.0 < 60.0);        // 10 s = 50 px
    CHECK(Near(VideoRulerStep(60.0, 600.0, 30.0, 60.0), 10.0) && 5.0 * 10.0 < 60.0);         // 5 s = 50 px
    CHECK(Near(VideoRulerStep(3.0, 600.0, 24.0, 60.0), 8.0 / 24.0) && 6.0 / 24.0 * 200.0 < 60.0);
    // min_px <= 0 (or NaN): 1 px, so the smallest step that is at least 1 px wide.
    CHECK(Near(VideoRulerStep(3.0, 600.0, 30.0, 0.0), 1.0 / 30.0));
    CHECK(Near(VideoRulerStep(3.0, 600.0, 30.0, -5.0), 1.0 / 30.0));
    CHECK(Near(VideoRulerStep(3.0, 600.0, 30.0, std::nan("")), 1.0 / 30.0));
    CHECK(Near(VideoRulerStep(600.0, 600.0, 0.0, 0.0), 1.0));
    // fps NaN, inf, under 0.5 or absurd: unknown, seconds only (no llround overflow).
    CHECK(Near(VideoRulerStep(3.0, 600.0, std::nan(""), 60.0), 1.0));
    CHECK(Near(VideoRulerStep(3.0, 600.0, INFINITY, 60.0), 1.0));
    CHECK(Near(VideoRulerStep(3.0, 600.0, 0.4, 60.0), 1.0));
    CHECK(Near(VideoRulerStep(3.0, 600.0, -30.0, 60.0), 1.0));
    CHECK(Near(VideoRulerStep(3.0, 600.0, 1.0e6, 60.0), 1.0));
    CHECK(Near(VideoRulerStep(3.0, 600.0, 1.0e300, 60.0), 1.0));
    CHECK(VideoRulerFps(1.0e300) == 0 && VideoRulerFps(0.4) == 0 && VideoRulerFps(29.97) == 30);

    // Ticks count whole frames from the item's first frame, drawn at F / fps.
    CHECK(VideoRulerStepFrames(10.0 / 30.0, 30.0) == 10);
    CHECK(VideoRulerStepFrames(1.0, 29.97) == 30);                        // nominal second: 30 frames
    CHECK(VideoRulerStepFrames(30.0, 24.0) == 720);
    CHECK(VideoRulerStepFrames(1.0, 0.0) == 0);                           // fps unknown
    CHECK(Near(VideoRulerTickTime(0, 0.51, 1.0, 30.0), 16.0 / 30.0));     // off-grid start: frame 16
    CHECK(Near(VideoRulerTickTime(2, 0.51, 1.0, 30.0), 76.0 / 30.0));
    CHECK(Near(VideoRulerTickTime(1, 0.0, 1.0, 29.97), 30.0 / 29.97));    // frame 30 at its exact time
    CHECK(Near(VideoRulerTickTime(3, 0.25, 1.0, 0.0), 3.25));             // fps unknown: seconds from start

    // Labels and majors.
    {
        char l[32];
        const double s10 = 10.0 / 30.0;  // 30 fps, 10 frames
        const char* want30[] = {"0:00", "+10f", "+20f", "0:01"};
        for (int k = 0; k < 4; ++k) {
            VideoRulerTickLabel(k, s10, 30.0, l, sizeof(l));
            CHECK(std::strcmp(l, want30[k]) == 0);
            CHECK(VideoRulerTickIsMajor(k, s10, 30.0) == (k == 0 || k == 3));
        }
        const double s8 = 8.0 / 24.0;    // 24 fps, 8 frames
        const char* want24[] = {"0:00", "+8f", "+16f", "0:01"};
        for (int k = 0; k < 4; ++k) {
            VideoRulerTickLabel(k, s8, 24.0, l, sizeof(l));
            CHECK(std::strcmp(l, want24[k]) == 0);
            CHECK(VideoRulerTickIsMajor(k, s8, 24.0) == (k == 0 || k == 3));
        }
        VideoRulerTickLabel(1, 30.0, 30.0, l, sizeof(l));
        CHECK(std::strcmp(l, "0:30") == 0);
        VideoRulerTickLabel(2, 30.0, 30.0, l, sizeof(l));
        CHECK(std::strcmp(l, "1:00") == 0);
        CHECK(VideoRulerTickIsMajor(1, 30.0, 30.0));
        // A 1 h item: h:mm:ss from one hour on.
        const double hstep = VideoRulerStep(3600.0, 600.0, 30.0, 60.0);
        CHECK(Near(hstep, 600.0));
        VideoRulerTickLabel(5, hstep, 30.0, l, sizeof(l));
        CHECK(std::strcmp(l, "50:00") == 0);
        VideoRulerTickLabel(6, hstep, 30.0, l, sizeof(l));
        CHECK(std::strcmp(l, "1:00:00") == 0);
        VideoRulerTickLabel(1, 3600.0 + 0.0, 0.0, l, sizeof(l));
        CHECK(std::strcmp(l, "1:00:00") == 0);
        // 29.97 fps: frames / rounded fps (30 frames = "0:01").
        const double s2997 = VideoRulerStep(3.0, 600.0, 29.97, 60.0);
        CHECK(VideoRulerStepFrames(s2997, 29.97) == 10);
        VideoRulerTickLabel(1, s2997, 29.97, l, sizeof(l));
        CHECK(std::strcmp(l, "+10f") == 0);
        VideoRulerTickLabel(3, s2997, 29.97, l, sizeof(l));
        CHECK(std::strcmp(l, "0:01") == 0);
        CHECK(VideoRulerTickIsMajor(3, s2997, 29.97) && !VideoRulerTickIsMajor(2, s2997, 29.97));
        VideoRulerTickLabel(2, 1.0, 0.0, l, sizeof(l));                   // fps unknown: seconds
        CHECK(std::strcmp(l, "0:02") == 0);
    }

    // Snap to the nearest tick (seconds grid).
    CHECK(Near(VideoSnapToTick(2.04, 1.0, 0.0), 2.0));
    CHECK(Near(VideoSnapToTick(2.6, 1.0, 0.0), 3.0));
    CHECK(Near(VideoSnapToTick(2.6, 1.0, 0.25), 2.25));                   // ticks from the item start
    CHECK(Near(VideoSnapToTick(1.234, 0.0, 0.0), 1.234));                 // no step: unchanged
    CHECK(Near(VideoRulerSnap(2.6, 0.25, 1.0, 0.0), 2.25));               // fps unknown: same grid
    CHECK(Near(VideoRulerSnap(0.0, 0.51, 1.0, 30.0), 16.0 / 30.0));       // never before the first tick
    CHECK(Near(VideoRulerSnap(1.234, 0.0, 0.0, 30.0), 1.234));            // no step: unchanged

    // Snap scrub (copies the UI: seek = VideoRulerSnap(t_mouse)): tick 1 s, 2.04 s, 30 fps.
    {
        const double seek = VideoRulerSnap(2.04, 0.0, 1.0, 30.0);
        CHECK(VideoFrameIndexAt(seek, 30.0) == 60);
        CHECK(Near(seek, 2.0));
    }
    // Snap junction (copies the UI: t = orig + delta + half, snapped, then VideoJunctionDragTime).
    // Matrix: tick 1 s, junction dragged near 2.04 s, 30 fps: the shot starts on 2.0 s's frame.
    {
        const double half = 0.5 / 30.0;
        const double orig = 59.5 / 30.0;                                  // a cut: half a frame early
        const double t = orig + (2.04 - orig) + half;                     // dragged to 2.04 s
        const double moved = VideoJunctionDragTime(VideoRulerSnap(t, 0.0, 1.0, 30.0), 0.0, 5.0, 30.0);
        CHECK(VideoShotFirstFrame(moved, 30.0) == 60);
        CHECK(Near(VideoShotFirstFrameTime(moved, 30.0), 2.0));
        // No drag from a junction already on a tick: stays on its frame.
        const double still = VideoJunctionDragTime(VideoRulerSnap(orig + half, 0.0, 1.0, 30.0), 0.0, 5.0, 30.0);
        CHECK(VideoShotFirstFrame(still, 30.0) == 60);
        // Dragged a little past the tick: back onto frame 60.
        const double near = VideoJunctionDragTime(VideoRulerSnap(2.0 + 0.4 / 30.0, 0.0, 1.0, 30.0), 0.0, 5.0, 30.0);
        CHECK(VideoShotFirstFrame(near, 30.0) == 60);
    }
    // An item starting off the frame grid (0.51 s, 30 fps: first frame 16, ticks 16, 46, 76):
    // scrub and junction near the same tick land on the same frame.
    for (double x : {1.50, 1.53, 1.55, 1.60}) {
        const double start = 0.51;
        const double seek = VideoRulerSnap(x, start, 1.0, 30.0);
        const double orig = 40.5 / 30.0;                                  // shot on frame 41
        const double t = orig + (x - orig) + 0.5 / 30.0;
        const double moved = VideoJunctionDragTime(VideoRulerSnap(t, start, 1.0, 30.0), 0.0, 5.0, 30.0);
        CHECK(VideoFrameIndexAt(seek, 30.0) == 46);
        CHECK(VideoShotFirstFrame(moved, 30.0) == 46);
    }

    // Spec 11-fb-10: the current shot by first frame, and the playhead's frame time.
    {
        const double fps = 60.0;
        const double cut2 = VideoCutTime(2.0, fps);                       // shot 2: first frame 120, point at 119.5 / 60
        CHECK(Near(cut2, 119.5 / 60.0));
        const std::vector<double> times = {0.0, cut2};
        // Shot list click: the seek lands on the first frame, which is shot 2.
        const double seek = VideoShotFirstFrameTime(cut2, fps);
        CHECK(Near(seek, 2.0));
        CHECK(VideoCurrentShotIndex(times, seek, fps, 0.0005) == 1);
        // Scrub across: changes exactly at 2.0 s (frame 120).
        CHECK(VideoCurrentShotIndex(times, 1.99, fps, 0.0005) == 0);
        CHECK(VideoCurrentShotIndex(times, 2.0, fps, 0.0005) == 1);
        CHECK(VideoCurrentShotIndex(times, 2.0 - 1e-12, fps, 0.0005) == 1);  // float error on the frame start
        // Half-frame window: past the point (1.9917), still shot 1, camera of frame 119.
        CHECK(VideoCurrentShotIndex(times, 1.995, fps, 0.0005) == 0);
        CHECK(VideoCurrentShotIndex(times, cut2 + 1e-6, fps, 0.0005) == 0);
        CHECK(Near(VideoPlayheadFrameTime(1.995, fps), 119.0 / 60.0));
        CHECK(VideoPlayheadFrameTime(1.995, fps) < cut2);                  // envelopes read before the point
        CHECK(Near(VideoPlayheadFrameTime(2.0, fps), 2.0));
        CHECK(Near(VideoPlayheadFrameTime(2.0 - 1e-12, fps), 2.0));
        CHECK(Near(VideoPlayheadFrameTime(2.012, fps), 2.0));
        CHECK(Near(VideoPlayheadFrameTime(-1.0, fps), 0.0));
        // A negative or non-finite playhead reads frame 0, fps known or not.
        CHECK(VideoPlayheadFrameTime(-1.0, 0.0) == 0.0);
        CHECK(VideoPlayheadFrameTime(std::nan(""), fps) == 0.0);
        CHECK(VideoPlayheadFrameTime(std::nan(""), 0.0) == 0.0);
        CHECK(VideoPlayheadFrameTime(INFINITY, fps) == 0.0);
        CHECK(VideoPlayheadFrameTime(-INFINITY, 0.0) == 0.0);
        CHECK(Near(VideoPlayheadFrameTime(1.995, std::nan("")), 1.995));   // NaN fps: unknown
        // fps unknown: envelope times within the tolerance, raw playhead.
        CHECK(VideoCurrentShotIndex(times, 1.995, 0.0, 0.0005) == 1);
        CHECK(VideoCurrentShotIndex(times, cut2 - 0.0004, 0.0, 0.0005) == 1);
        CHECK(VideoCurrentShotIndex(times, cut2 - 0.001, 0.0, 0.0005) == 0);
        CHECK(Near(VideoPlayheadFrameTime(1.995, 0.0), 1.995));
        // Before every shot: the first one. None: -1.
        CHECK(VideoCurrentShotIndex({1.0, 2.0}, 0.5, fps, 0.0005) == 0);
        CHECK(VideoCurrentShotIndex({}, 0.5, fps, 0.0005) == -1);
        // A shot on frame 0 is current from 0.
        CHECK(VideoCurrentShotIndex({0.0, VideoCutTime(1.0 / fps, fps)}, 0.0, fps, 0.0005) == 0);
        CHECK(VideoCurrentShotIndex({0.0, VideoCutTime(1.0 / fps, fps)}, 1.0 / fps, fps, 0.0005) == 1);
    }

    // Spec 11-fb-10: NTSC rates over long projects. A cut at frame k: the shot list seeks to
    // frame k, and the shot is current there (and not one frame before).
    {
        const double rates[] = {30000.0 / 1001.0, 24000.0 / 1001.0};
        for (double fps : rates) {
            int bad = 0;
            for (long long k = 1; k <= 1000000; k += 997) {
                const double playhead = static_cast<double>(k) / fps;
                const double cut = VideoCutTime(playhead, fps);
                const double seek = VideoShotFirstFrameTime(cut, fps);
                const std::vector<double> times = {0.0, cut};
                if (VideoFrameIndexAt(seek, fps) != k) ++bad;
                if (VideoCurrentShotIndex(times, seek, fps, 0.0005) != 1) ++bad;
                if (VideoCurrentShotIndex(times, static_cast<double>(k - 1) / fps, fps, 0.0005) != 0) ++bad;
            }
            CHECK(bad == 0);
        }
    }

    // Spec 11-fb-10: a junction dragged to frame k starts the moved shot on frame k, where
    // the shot list seeks and where it becomes current.
    {
        const double rates[] = {60.0, 30000.0 / 1001.0, 24000.0 / 1001.0};
        for (double fps : rates) {
            int bad = 0;
            for (long long k = 2; k <= 1000000; k += 1009) {
                const double prev = 0.0;
                const double next = static_cast<double>(k + 10) / fps;
                const double moved = VideoJunctionDragTime(static_cast<double>(k) / fps, prev, next, fps);
                if (!Near(VideoShotFirstFrameTime(moved, fps), static_cast<double>(k) / fps)) ++bad;
                const std::vector<double> times = {prev, moved, next};
                if (VideoCurrentShotIndex(times, static_cast<double>(k) / fps, fps, 0.0005) != 1) ++bad;
            }
            CHECK(bad == 0);
        }
    }

    // Minor ticks (spec 11-fb-9): the finest frame step dividing the labelled step, >= 4 px.
    {
        // 60 fps, 1 s in 600 px (10 px a frame): labelled every 6 frames, a tick per frame.
        const double step = VideoRulerStep(1.0, 600.0, 60.0, 60.0);
        CHECK(VideoRulerStepFrames(step, 60.0) == 6);
        CHECK(VideoRulerStepFrames(VideoRulerMinorStep(step, 1.0, 600.0, 60.0, 4.0), 60.0) == 1);
    }
    {
        // 60 fps, 3 s in 600 px: a frame is 3.33 px (under 4 px), so 2 frames (6.67 px) under a
        // 20-frame label step. With a 3 px floor it is one frame.
        const double step = VideoRulerStep(3.0, 600.0, 60.0, 60.0);
        CHECK(VideoRulerStepFrames(step, 60.0) == 20);
        CHECK(VideoRulerStepFrames(VideoRulerMinorStep(step, 3.0, 600.0, 60.0, 4.0), 60.0) == 2);
        CHECK(VideoRulerStepFrames(VideoRulerMinorStep(step, 3.0, 600.0, 60.0, 3.0), 60.0) == 1);
    }
    {
        // 60 fps, 30 s in 600 px: labelled every 5 s (300 frames); 1 frame is 0.33 px, so the
        // finest divisor of 60 that divides 300 and is >= 4 px: 12 frames (0.2 s, 4 px).
        const double step = VideoRulerStep(30.0, 600.0, 60.0, 60.0);
        CHECK(Near(step, 5.0));
        const double minor = VideoRulerMinorStep(step, 30.0, 600.0, 60.0, 4.0);
        CHECK(VideoRulerStepFrames(minor, 60.0) == 12);
        CHECK(Near(minor, 12.0 / 60.0));
    }
    // 30 fps under a second: only divisors of 30 (1, 2, 3, 5, 6, 10, 15), never 4 frames.
    // 1 s labelled in 600 px over 7.5 s: a frame is 2.67 px, 2 frames 5.3 px.
    CHECK(VideoRulerStepFrames(VideoRulerMinorStep(1.0, 7.5, 600.0, 30.0, 4.0), 30.0) == 2);
    // 27 s in 1000 px: a frame is 1.23 px, 3 frames 3.7 px (too close), 4 is not a divisor: 5.
    CHECK(VideoRulerStepFrames(VideoRulerMinorStep(1.0, 27.0, 1000.0, 30.0, 4.0), 30.0) == 5);
    // A minor step of a second or more is a nice whole-seconds step on the same frame grid:
    // 60 fps, 1 h in 600 px is labelled every 10 min (36000 frames); 4 px = 24 s, so 30 s.
    {
        const double step = VideoRulerStep(3600.0, 600.0, 60.0, 60.0);
        CHECK(Near(step, 600.0));
        const double minor = VideoRulerMinorStep(step, 3600.0, 600.0, 60.0, 4.0);
        CHECK(Near(minor, 30.0) && VideoRulerStepFrames(minor, 60.0) == 30 * 60);
    }
    // The nice divisors a minor step may take (seconds), and a brute-force reference: the
    // smallest nice divisor of the labelled step (frames) that is finer and >= min_px.
    auto nice_minor_ref = [](double step, double span, double lane, double fps, double min_px) -> long long {
        const long long ifps = VideoRulerFps(fps);
        const long long sf = VideoRulerStepFrames(step, fps);
        const double ppf = lane / span / fps;
        for (long long d = 1; d < ifps; ++d) {
            if (ifps % d == 0 && d < sf && sf % d == 0 && d * ppf >= min_px - 1e-9) return d;
        }
        for (long long c : {1LL, 2LL, 5LL, 10LL, 15LL, 30LL, 60LL, 120LL, 300LL, 600LL, 900LL, 1800LL, 3600LL,
                            7200LL, 18000LL, 36000LL}) {
            const long long cf = c * ifps;
            if (cf < sf && sf % cf == 0 && cf * ppf >= min_px - 1e-9) return cf;
        }
        return 0;
    };
    // Every minor step divides the labelled step, is finer, keeps 4 px, is nice (under a second:
    // divides the rounded fps; else whole nice seconds), and exists wherever a nice divisor does.
    // Minor ticks land on labelled ticks; none doubles a labelled tick between them.
    {
        int checked = 0;
        int with_minor = 0;
        for (double span : {0.2, 1.0, 3.0, 7.5, 17.0, 120.0, 900.0, 3600.0, 7200.0, 86400.0}) {
            for (double lane : {300.0, 600.0, 1700.0}) {
                for (double fps : {23.976, 24.0, 25.0, 29.97, 30.0, 50.0, 59.94, 60.0}) {
                    const double step = VideoRulerStep(span, lane, fps, 60.0);
                    const double minor = VideoRulerMinorStep(step, span, lane, fps, 4.0);
                    const long long sf = VideoRulerStepFrames(step, fps);
                    const long long ref = nice_minor_ref(step, span, lane, fps, 4.0);
                    ++checked;
                    CHECK((minor > 0.0) == (ref > 0));
                    if (minor <= 0.0) continue;
                    ++with_minor;
                    const long long ifps = VideoRulerFps(fps);
                    const long long mf = VideoRulerStepFrames(minor, fps);
                    CHECK(mf == ref);
                    CHECK(mf > 0 && mf < sf && sf % mf == 0);
                    CHECK(mf < ifps ? ifps % mf == 0 : mf % ifps == 0);
                    CHECK(static_cast<double>(mf) / fps * lane / span >= 4.0 - 1e-6);
                    const long long per = VideoRulerMinorPerLabel(step, minor, fps);
                    CHECK(per == sf / mf && per > 1);
                    CHECK(Near(VideoRulerTickTime(per, 0.51, minor, fps), VideoRulerTickTime(1, 0.51, step, fps)));
                    // Minor tick k is a labelled tick exactly when k % per == 0.
                    for (long long k = 0; k <= 2 * per; ++k) {
                        const double tk = VideoRulerTickTime(k, 0.51, minor, fps);
                        const long long kl = VideoRulerNearestTick(tk, 0.51, step, fps);
                        const bool on_label = Near(VideoRulerTickTime(kl, 0.51, step, fps), tk);
                        CHECK(on_label == (k % per == 0));
                    }
                }
            }
        }
        CHECK(checked == 10 * 3 * 8);
        CHECK(with_minor >= 150);  // the loop is not vacuous
    }
    // fps unknown: 0.2 s, then the nice seconds, finer than and dividing the labelled step.
    CHECK(Near(VideoRulerMinorStep(1.0, 3.0, 600.0, 0.0, 4.0), 0.2));      // 40 px
    CHECK(Near(VideoRulerMinorStep(5.0, 30.0, 300.0, 0.0, 4.0), 1.0));      // 0.2 s = 2 px: 1 s = 10 px
    CHECK(Near(VideoRulerMinorStep(600.0, 3600.0, 600.0, 0.0, 4.0), 30.0)); // zoomed out: still minor ticks
    CHECK(VideoRulerMinorStep(1.0, 600.0, 600.0, 0.0, 4.0) == 0.0);         // 0.2 s = 0.2 px: none
    CHECK(VideoRulerMinorPerLabel(1.0, 0.2, 0.0) == 5);
    CHECK(VideoRulerMinorPerLabel(600.0, 30.0, 0.0) == 20);
    CHECK(Near(VideoRulerSnap(1.13, 0.0, 0.2, 0.0), 1.2));
    // Labelled step of one frame: nothing finer.
    CHECK(VideoRulerMinorStep(1.0 / 60.0, 0.1, 1200.0, 60.0, 4.0) == 0.0);
    CHECK(VideoRulerMinorStep(0.0, 3.0, 600.0, 60.0, 4.0) == 0.0);          // no labelled step
    // Bad inputs: NaN / inf span or lane, no minor ticks; min_px <= 0 or NaN counts as 1 px.
    CHECK(VideoRulerMinorStep(1.0, std::nan(""), 600.0, 60.0, 4.0) == 0.0);
    CHECK(VideoRulerMinorStep(1.0, INFINITY, 600.0, 60.0, 4.0) == 0.0);
    CHECK(VideoRulerMinorStep(1.0, 3.0, std::nan(""), 60.0, 4.0) == 0.0);
    CHECK(VideoRulerMinorStep(1.0, 3.0, INFINITY, 60.0, 4.0) == 0.0);
    CHECK(VideoRulerMinorStep(std::nan(""), 3.0, 600.0, 60.0, 4.0) == 0.0);
    CHECK(VideoRulerMinorStep(INFINITY, 3.0, 600.0, 60.0, 4.0) == 0.0);
    CHECK(Near(VideoRulerMinorStep(1.0, 3.0, 600.0, 60.0, 0.0), 1.0 / 60.0));
    CHECK(Near(VideoRulerMinorStep(1.0, 3.0, 600.0, 60.0, -5.0), 1.0 / 60.0));
    CHECK(Near(VideoRulerMinorStep(1.0, 3.0, 600.0, 60.0, std::nan("")), 1.0 / 60.0));
    CHECK(VideoRulerMinorStep(1.0, 600.0, 600.0, 0.0, 0.0) == 0.0);           // 1 px floor: 0.2 s = 0.2 px
    // Absurd spans: no minor ticks, no overflow.
    CHECK(VideoRulerMinorStep(1.0e300, 1.0e300, 600.0, 60.0, 4.0) == 0.0);
    CHECK(VideoRulerMinorStep(1.0e300, 1.0e300, 600.0, 0.0, 4.0) == 0.0);
    CHECK(VideoRulerMinorStep(1.0e14, 1.0e15, 600.0, 60.0, 4.0) == 0.0);
    CHECK(VideoRulerMinorPerLabel(1.0e300, 1.0, 60.0) == 0);
    CHECK(VideoRulerMinorPerLabel(1.0, 1.0e-300, 0.0) == 0);
    CHECK(VideoRulerMinorPerLabel(1.0, 0.0, 60.0) == 0);
    CHECK(VideoRulerMinorPerLabel(1.0, 1.0, 60.0) == 0);                      // not finer
    CHECK(VideoRulerMinorPerLabel(1.0, std::nan(""), 0.0) == 0);
    CHECK(VideoRulerMinorPerLabel(1.0, 0.3, 0.0) == 0);                       // does not divide
    // 29.97 / 23.976 at long spans: nominal-second minor steps on the labelled frame grid.
    {
        const double step = VideoRulerStep(7200.0, 600.0, 29.97, 60.0);
        const double minor = VideoRulerMinorStep(step, 7200.0, 600.0, 29.97, 4.0);
        CHECK(Near(step, 1800.0) && Near(minor, 60.0));                       // 30 min / 1 min (5 px)
        CHECK(VideoRulerMinorPerLabel(step, minor, 29.97) == 30);
        const double step2 = VideoRulerStep(86400.0, 600.0, 23.976, 60.0);
        const double minor2 = VideoRulerMinorStep(step2, 86400.0, 600.0, 23.976, 4.0);
        CHECK(VideoRulerStepFrames(step2, 23.976) % VideoRulerStepFrames(minor2, 23.976) == 0);
        CHECK(minor2 >= 1.0 && minor2 < step2);
    }
    // Snap step: the minor step when there are minor ticks, else the labelled step.
    CHECK(Near(VideoRulerSnapStep(1.0, 1.0 / 60.0), 1.0 / 60.0));
    CHECK(Near(VideoRulerSnapStep(1.0, 0.0), 1.0));
    CHECK(Near(VideoRulerSnapStep(1.0, std::nan("")), 1.0));
    // Snap to the minor step lands on frames: 60 fps, item at 0.51 s (first frame 31).
    CHECK(VideoFrameIndexAt(VideoRulerSnap(1.0, 0.51, VideoRulerSnapStep(1.0, 1.0 / 60.0), 60.0), 60.0) == 60);
    CHECK(VideoFrameIndexAt(VideoRulerSnap(1.2, 0.51, VideoRulerSnapStep(1.0, 0.0), 60.0), 60.0) == 91);

    std::printf(g_fails ? "FAILED %d\n" : "ALL PASS\n", g_fails);
    return g_fails != 0 ? 1 : 0;
}
