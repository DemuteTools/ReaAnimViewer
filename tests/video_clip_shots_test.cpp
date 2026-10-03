// SPDX-License-Identifier: MIT
//
// Host test of the clip-aware shot list (src/video_clip_shots.h, spec 11-fb-15). No REAPER,
// no Windows: any C++17 compiler.
//   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests

#include "video_clip_shots.h"
#include "video_fx_state.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
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

VideoClipIn Clip(double s, double e, const char* name = "Anim")
{
    VideoClipIn c;
    c.start = s;
    c.end = e;
    c.anim_name = name;
    return c;
}

VideoRawShotIn Raw(double t, bool span = false, bool named = false)
{
    VideoRawShotIn r;
    r.time = t;
    r.span = span;
    r.stored_name = named;
    return r;
}

// A raw shot made by a cut with the playhead on second s (half a frame early).
double CutAt(double s, double fps)
{
    return VideoCutTime(s, fps);
}

}  // namespace

int main()
{
    const double fps = 24.0;

    // No point at all: each clip one implicit shot "<anim> 1".
    {
        const VideoClipShots r = BuildVideoClipShots({Clip(0, 2, "Walk"), Clip(5, 7, "Run")}, {}, fps);
        CHECK(r.clips.size() == 2);
        CHECK(r.shots.size() == 2);
        CHECK(r.shots[0].raw == -1 && r.shots[0].clip == 0 && r.shots[0].clip_first && r.shots[0].number == 1);
        CHECK(Near(r.shots[0].start, 0.0) && Near(r.shots[0].end, 2.0));
        CHECK(r.shots[1].raw == -1 && r.shots[1].clip == 1 && Near(r.shots[1].start, 5.0) && Near(r.shots[1].end, 7.0));
        CHECK(VideoAutoShotName(r.clips[0].anim_name, r.shots[0].number) == "Walk 1");
        CHECK(VideoAutoShotName(r.clips[1].anim_name, r.shots[1].number) == "Run 1");
    }

    // Shot at 0 s (pinned), first clip at 2 s: hidden; clip 1 implicit first shot.
    {
        const VideoClipShots r = BuildVideoClipShots({Clip(2, 4)}, {Raw(0.0)}, fps);
        CHECK(r.shots.size() == 1);
        CHECK(r.shots[0].raw == -1 && Near(r.shots[0].start, 2.0) && Near(r.shots[0].end, 4.0));
    }

    // Gap: A 0-2, B 240-242, raw shots at 0 and 1: A two shots ending at 2 s, B implicit.
    {
        const VideoClipShots r =
            BuildVideoClipShots({Clip(0, 2, "A"), Clip(240, 242, "B")}, {Raw(0.0), Raw(CutAt(1.0, fps))}, fps);
        CHECK(r.shots.size() == 3);
        CHECK(r.shots[0].raw == 0 && r.shots[0].clip_first && Near(r.shots[0].start, 0.0) && Near(r.shots[0].end, 1.0));
        CHECK(r.shots[1].raw == 1 && !r.shots[1].clip_first && Near(r.shots[1].start, 1.0) &&
              Near(r.shots[1].end, 2.0) && r.shots[1].number == 2);
        CHECK(r.shots[2].raw == -1 && r.shots[2].clip == 1 && Near(r.shots[2].start, 240.0) &&
              Near(r.shots[2].end, 242.0) && r.shots[2].number == 1);
        // The playhead in the gap: no shot.
        CHECK(VideoClipShotAt(r.shots, 100.0, fps) == -1);
        CHECK(VideoClipShotAt(r.shots, 2.0, fps) == -1);
        CHECK(VideoClipShotAt(r.shots, 2.0 - 1.0 / fps, fps) == 1);
        CHECK(VideoClipShotAt(r.shots, 240.0, fps) == 2);
        CHECK(VideoClipShotAt(r.shots, 0.5, fps) == 0);
        CHECK(VideoClipShotAt(r.shots, 1.0, fps) == 1);
        CHECK(VideoClipShotAt(r.shots, 1.0 - 1e-12, fps) == 1);  // float error on the frame start
        CHECK(VideoClipShotAt(r.shots, 300.0, fps) == -1);       // after the last clip
        CHECK(VideoClipShotAt(r.shots, std::nan(""), fps) == -1); // no playhead: no shot
        CHECK(VideoClipShotAt(r.shots, std::numeric_limits<double>::infinity(), fps) == -1);
    }

    // Back-to-back, no span: the raw shot ends at 2 s, B gets its implicit first shot.
    {
        const VideoClipShots r = BuildVideoClipShots({Clip(0, 2), Clip(2, 4)}, {Raw(CutAt(1.0, fps))}, fps);
        CHECK(r.shots.size() == 3);
        CHECK(r.shots[0].raw == -1 && Near(r.shots[0].end, 1.0));
        CHECK(r.shots[1].raw == 0 && Near(r.shots[1].start, 1.0) && Near(r.shots[1].end, 2.0));
        CHECK(r.shots[2].raw == -1 && r.shots[2].clip == 1 && Near(r.shots[2].start, 2.0) && Near(r.shots[2].end, 4.0));
        CHECK(VideoClipsBackToBack(r.clips, 0, fps));
    }

    // Back-to-back, span: one shot 1-4 s, no implicit shot in B.
    {
        const VideoClipShots r = BuildVideoClipShots({Clip(0, 2), Clip(2, 4)}, {Raw(CutAt(1.0, fps), true)}, fps);
        CHECK(r.shots.size() == 2);
        CHECK(r.shots[1].raw == 0 && Near(r.shots[1].start, 1.0) && Near(r.shots[1].end, 4.0));
        CHECK(VideoClipShotAt(r.shots, 3.0, fps) == 1);
        // A cut in B after the span: the span shot ends there, the cut is B's shot 1.
        const VideoClipShots r2 =
            BuildVideoClipShots({Clip(0, 2), Clip(2, 4)}, {Raw(CutAt(1.0, fps), true), Raw(CutAt(3.0, fps))}, fps);
        CHECK(r2.shots.size() == 3);
        CHECK(Near(r2.shots[1].end, 3.0));
        CHECK(r2.shots[2].clip == 1 && r2.shots[2].number == 1 && !r2.shots[2].clip_first && Near(r2.shots[2].end, 4.0));
        // A raw shot on B's first frame wins over the span: it ends at 2 s.
        const VideoClipShots r3 =
            BuildVideoClipShots({Clip(0, 2), Clip(2, 4)}, {Raw(CutAt(1.0, fps), true), Raw(CutAt(2.0, fps))}, fps);
        CHECK(r3.shots.size() == 3);
        CHECK(Near(r3.shots[1].end, 2.0) && r3.shots[2].clip_first && r3.shots[2].raw == 1);
    }

    // Span then gap: A, B back-to-back, C after a gap, span on A's last: ends at B's end, C implicit.
    {
        const VideoClipShots r = BuildVideoClipShots({Clip(0, 2), Clip(2, 4), Clip(10, 12)},
                                                     {Raw(0.0), Raw(CutAt(1.0, fps), true)}, fps);
        CHECK(r.shots.size() == 3);
        CHECK(Near(r.shots[1].end, 4.0));
        CHECK(r.shots[2].raw == -1 && r.shots[2].clip == 2 && Near(r.shots[2].start, 10.0));
        CHECK(VideoClipShotAt(r.shots, 5.0, fps) == -1);
    }

    // Span over a clip that is not back-to-back: ignored (the shot ends at its clip's end).
    {
        const VideoClipShots r = BuildVideoClipShots({Clip(0, 2), Clip(3, 4)}, {Raw(CutAt(1.0, fps), true)}, fps);
        CHECK(r.shots.size() == 3);
        CHECK(Near(r.shots[1].end, 2.0));
        CHECK(r.shots[2].raw == -1 && Near(r.shots[2].start, 3.0));
    }

    // A span carries through one clip start only: A, B, C back-to-back, B with no shot of its own.
    {
        const VideoClipShots r = BuildVideoClipShots({Clip(0, 2), Clip(2, 4), Clip(4, 6)},
                                                     {Raw(0.0), Raw(CutAt(1.0, fps), true)}, fps);
        CHECK(r.shots.size() == 3);
        CHECK(Near(r.shots[1].end, 4.0));
        CHECK(r.shots[2].raw == -1 && r.shots[2].clip == 2 && Near(r.shots[2].start, 4.0));
    }

    // Off-grid clip start: B at 1.400 s, 24 fps: B's first shot starts on frame 34.
    {
        const VideoClipShots r = BuildVideoClipShots({Clip(0, 1.4), Clip(1.4, 3.0)}, {}, fps);
        CHECK(r.shots.size() == 2);
        CHECK(Near(r.shots[1].start, 34.0 / 24.0));
        CHECK(Near(r.shots[0].end, 34.0 / 24.0));
        CHECK(VideoClipsBackToBack(r.clips, 0, fps));
        CHECK(VideoClipShotAt(r.shots, 1.4, fps) == 0);  // frame 33 (1.375 .. 1.4167) shows A
        CHECK(VideoClipShotAt(r.shots, 34.0 / 24.0, fps) == 1);
        // A cut with the playhead on B's start goes in B (on its first frame), just before it in A.
        CHECK(VideoClipIndexAtTime(r.clips, 1.4, fps) == 1);
        CHECK(VideoClipIndexAtTime(r.clips, 1.39, fps) == 0);
        CHECK(VideoClipIndexAtTime(r.clips, 5.0, fps) == -1);
        CHECK(VideoClipIndexAtFrame(r.clips, 33.0 / 24.0, fps) == 0);
        CHECK(VideoClipIndexAtFrame(r.clips, 34.0 / 24.0, fps) == 1);
        // Its implicit point: half a frame before frame 34.
        CHECK(Near(VideoClipImplicitTime(1.4, fps), 33.5 / 24.0));
        CHECK(Near(VideoShotFirstFrameTime(VideoClipImplicitTime(1.4, fps), fps), 34.0 / 24.0));
        // A raw point written there is B's first shot (no implicit one any more).
        const VideoClipShots r2 = BuildVideoClipShots({Clip(0, 1.4), Clip(1.4, 3.0)}, {Raw(VideoClipImplicitTime(1.4, fps))}, fps);
        CHECK(r2.shots.size() == 2);
        CHECK(r2.shots[1].raw == 0 && r2.shots[1].clip_first && r2.shots[1].clip == 1);
    }
    CHECK(Near(VideoClipImplicitTime(0.0, fps), 0.0));
    CHECK(Near(VideoClipImplicitTime(2.0, fps), 47.5 / 24.0));
    CHECK(Near(VideoClipImplicitTime(2.0, 0.0), 2.0));

    // Overlapping items: the later one starts where the earlier ends; one inside another is dropped.
    {
        const VideoClipShots r = BuildVideoClipShots({Clip(2, 6, "B"), Clip(0, 4, "A"), Clip(1, 3, "C")}, {}, fps);
        CHECK(r.clips.size() == 2);
        CHECK(r.clips[0].anim_name == "A" && Near(r.clips[0].start, 0.0) && Near(r.clips[0].end, 4.0));
        CHECK(r.clips[1].anim_name == "B" && Near(r.clips[1].start, 4.0) && Near(r.clips[1].first, 4.0));
        CHECK(r.shots.size() == 2 && Near(r.shots[1].start, 4.0));
        CHECK(VideoClipsBackToBack(r.clips, 0, fps));
        // An empty clip, and one under a frame long between frames: dropped.
        const VideoClipShots r2 = BuildVideoClipShots({Clip(1, 1), Clip(1.01, 1.02), Clip(3, 2)}, {}, fps);
        CHECK(r2.clips.empty() && r2.shots.empty());
    }

    // Unknown fps: the times themselves.
    {
        const VideoClipShots r =
            BuildVideoClipShots({Clip(0, 2), Clip(2, 4), Clip(9, 10)}, {Raw(0.0), Raw(1.3), Raw(5.0)}, 0.0);
        CHECK(r.shots.size() == 4);
        CHECK(Near(r.shots[1].start, 1.3) && Near(r.shots[1].end, 2.0));
        CHECK(r.shots[2].raw == -1 && Near(r.shots[2].start, 2.0) && Near(r.shots[2].end, 4.0));
        CHECK(r.shots[3].raw == -1 && Near(r.shots[3].start, 9.0));
        CHECK(VideoClipsBackToBack(r.clips, 0, 0.0));
        CHECK(!VideoClipsBackToBack(r.clips, 1, 0.0));
        CHECK(VideoClipShotAt(r.shots, 1.5, 0.0) == 1);
        CHECK(VideoClipShotAt(r.shots, 6.0, 0.0) == -1);
        CHECK(VideoClipShotAt(r.shots, std::nan(""), 0.0) == -1);
        const VideoClipShots r2 = BuildVideoClipShots({Clip(0, 2)}, {Raw(1.0)}, std::nan(""));
        CHECK(r2.shots.size() == 2 && Near(r2.shots[1].start, 1.0));
    }

    // Numbering per clip, two raw shots on one frame (the later wins), non-finite times ignored.
    {
        const VideoClipShots r = BuildVideoClipShots(
            {Clip(0, 4, "A"), Clip(4, 8, "B")},
            {Raw(0.0), Raw(CutAt(1.0, fps)), Raw(CutAt(1.0, fps) + 0.001), Raw(CutAt(2.0, fps)),
             Raw(std::nan("")), Raw(CutAt(5.0, fps)), Raw(CutAt(6.0, fps))},
            fps);
        CHECK(r.shots.size() == 6);
        CHECK(r.shots[0].number == 1 && r.shots[1].number == 2 && r.shots[2].number == 3);
        CHECK(r.shots[1].raw == 2);  // the later of the two on frame 24
        CHECK(r.shots[3].raw == -1 && r.shots[3].clip == 1 && r.shots[3].number == 1);
        CHECK(r.shots[4].number == 2 && r.shots[5].number == 3 && r.shots[5].clip == 1);
        CHECK(Near(r.shots[5].end, 8.0));
    }

    // Names.
    {
        CHECK(VideoAnimNameFromPath("C:\\Anims\\Pistol Equip.fbx") == "Pistol Equip");
        CHECK(VideoAnimNameFromPath("/home/x/run.cycle.glb") == "run.cycle");
        CHECK(VideoAnimNameFromPath("walk") == "walk");
        CHECK(VideoAnimNameFromPath("dir.v2/walk") == "walk");
        CHECK(VideoAnimNameFromPath(".hidden") == ".hidden");
        CHECK(VideoAnimNameFromPath("") == "Clip");
        CHECK(VideoAutoShotName("Pistol Equip", 2) == "Pistol Equip 2");
    }

    // VideoShotCoversNextClip: after a junction move.
    {
        const VideoClipShots r = BuildVideoClipShots({Clip(0, 2), Clip(2, 4), Clip(10, 12)}, {}, fps);
        const double inf = std::numeric_limits<double>::infinity();
        // A's last shot, the next shot now starts inside B: it covers B's start.
        CHECK(VideoShotCoversNextClip(1.0, CutAt(3.0, fps), r.clips, fps));
        // The next shot starts on B's first frame: no.
        CHECK(!VideoShotCoversNextClip(1.0, CutAt(2.0, fps), r.clips, fps));
        CHECK(!VideoShotCoversNextClip(1.0, 2.0, r.clips, fps));
        // No next shot: covers the back-to-back clip.
        CHECK(VideoShotCoversNextClip(1.0, inf, r.clips, fps));
        CHECK(VideoShotCoversNextClip(1.0, std::nan(""), r.clips, fps));
        // B's shot: C is after a gap.
        CHECK(!VideoShotCoversNextClip(3.0, inf, r.clips, fps));
        // In a gap: no clip.
        CHECK(!VideoShotCoversNextClip(5.0, inf, r.clips, fps));
        // The next shot starts before B (in A): no.
        CHECK(!VideoShotCoversNextClip(0.5, CutAt(1.0, fps), r.clips, fps));
        // Unknown fps.
        const VideoClipShots u = BuildVideoClipShots({Clip(0, 2), Clip(2, 4)}, {}, 0.0);
        CHECK(VideoShotCoversNextClip(1.0, 3.0, u.clips, 0.0));
        CHECK(!VideoShotCoversNextClip(1.0, 2.0, u.clips, 0.0));
    }

    // The whole cycle: a clip far after the last shot, cut in its middle -> two shots in it.
    {
        std::vector<VideoRawShotIn> raws = {Raw(0.0)};
        std::vector<VideoClipIn> clips = {Clip(0, 2, "Idle"), Clip(240, 244, "Pistol Equip")};
        VideoClipShots r = BuildVideoClipShots(clips, raws, fps);
        CHECK(r.shots.size() == 2 && r.shots[1].raw == -1);
        // The cut writes the clip's first point and the cut.
        raws.push_back(Raw(VideoClipImplicitTime(240.0, fps)));
        raws.push_back(Raw(CutAt(242.0, fps)));
        r = BuildVideoClipShots(clips, raws, fps);
        CHECK(r.shots.size() == 3);
        CHECK(r.shots[1].clip == 1 && r.shots[1].number == 1 && r.shots[1].clip_first && r.shots[1].raw == 1);
        CHECK(r.shots[2].clip == 1 && r.shots[2].number == 2 && Near(r.shots[2].start, 242.0));
        CHECK(VideoAutoShotName(r.clips[1].anim_name, r.shots[2].number) == "Pistol Equip 2");
        CHECK(Near(r.shots[0].end, 2.0));
    }

    // The span flags travel in the FX state ("span <time>"), merged with the meta fields.
    {
        VideoFxState st;
        st.kind = VideoFxStateKind::Meta;
        st.shot_names.push_back({1.25, "Close"});
        st.shot_spans = {1.25, 3.5};
        const std::string text = SerializeVideoFxState(st);
        VideoFxState back;
        CHECK(ParseVideoFxState(text.data(), text.size(), &back));
        CHECK(back.shot_spans.size() == 2 && back.shot_spans[0] == 1.25 && back.shot_spans[1] == 3.5);
        CHECK(back.shot_names.size() == 1 && back.shot_names[0].name == "Close");
        VideoFxState cur;
        MergeVideoFxState(back, false, &cur);
        CHECK(cur.shot_spans == back.shot_spans);
        VideoFxState preset_cur;
        preset_cur.shot_spans = {9.0};
        MergeVideoFxState(back, true, &preset_cur);  // a preset load keeps the shots' meta
        CHECK(preset_cur.shot_spans.size() == 1 && preset_cur.shot_spans[0] == 9.0);
        // An old state without span records: none.
        const char old[] = "RAVVFX 1\nkind meta\noverride 0 0\nshot 2 Wide\n";
        VideoFxState o;
        CHECK(ParseVideoFxState(old, sizeof(old) - 1, &o) && o.shot_spans.empty() && o.shot_names.size() == 1);
    }

    std::printf(g_fails ? "FAILED %d\n" : "ALL PASS\n", g_fails);
    return g_fails != 0 ? 1 : 0;
}
