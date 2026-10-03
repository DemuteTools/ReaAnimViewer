// SPDX-License-Identifier: MIT
//
// Host test of the pure cut / first-frame / duplicate rules (src/video_shot_timing.h,
// Epic 11, Story 11-5). No REAPER, no Windows: any C++17 compiler.
//   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
// or: c++ -std=c++17 -I src tests/video_shot_timing_test.cpp -o t && ./t

#include "video_shot_timing.h"

#include <cmath>
#include <cstdio>
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

    std::printf(g_fails ? "FAILED %d\n" : "ALL PASS\n", g_fails);
    return g_fails != 0 ? 1 : 0;
}
