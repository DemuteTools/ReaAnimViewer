// SPDX-License-Identifier: MIT
//
// Host test of src/sound_place_plan.h (13-6): position, gain, slice, folder rule, NVK chunk,
// copy chunk, pile member offset.

#include "sound_place_plan.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace rav;

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)
static bool Near(double a, double b, double e = 1e-9) { return std::fabs(a - b) <= e; }

int main()
{
    // Position (Blend offset + nudge) and clamp.
    CHECK(Near(PlacementPosition(10.0, 0.25, 12.0, 0.1), 9.862));
    CHECK(Near(PlacementPosition(0.1, 0.5, 0.0, 0.0), 0.0));
    CHECK(Near(PlacementPosition(5.0, 0.0, -20.0, 0.0), 4.98));
    // Folder child keeps its offset from the folder item.
    CHECK(Near(PileMemberPosition(9.862, 3.0, 3.25), 10.112));
    CHECK(Near(PileMemberPosition(2.0, 4.0, 4.0), 2.0));

    // Gain.
    CHECK(Near(ItemGainFor(-6.0), 0.501187, 1e-5));
    CHECK(Near(ItemGainFor(0.0), 1.0));
    CHECK(Near(ItemGainFor(std::nan("")), 1.0));

    // Slice.
    SoundFrame f;
    f.start = 2.0;
    f.length = 1.0;
    SliceApply s = SliceApplyFor(f);
    CHECK(s.set && Near(s.start_offs, 2.0) && Near(s.length, 1.0) && Near(s.snap_offset, 0.0));
    f.length = 0.0;
    CHECK(!SliceApplyFor(f).set);

    // Folder rule.
    FolderItemFacts ff;
    ff.on_folder_track = true;
    ff.empty_source = true;
    ff.children_inside = true;
    CHECK(IsFolderItemRule(ff));
    ff.children_inside = false;
    ff.nvk_item_type = "folder";
    CHECK(IsFolderItemRule(ff));
    ff.empty_source = false;  // folder track + audio
    CHECK(!IsFolderItemRule(ff));
    ff.empty_source = true;
    ff.on_folder_track = false;  // plain track + empty
    ff.children_inside = true;
    CHECK(!IsFolderItemRule(ff));
    // Span.
    CHECK(SpanInside(1.0, 2.0, 1.0005, 1.9995));
    CHECK(!SpanInside(1.0, 2.0, 0.5, 1.0));

    // NVK chunk.
    const std::string nvk =
        "<ITEM\n  POSITION 1\n  <EXTI\n    nvk_item_type folder\n  >\n  IGUID {A}\n>\n";
    CHECK(NvkItemTypeFromChunk(nvk) == "folder");
    const std::string take_only =
        "<ITEM\n  POSITION 1\n  NAME x\n  <SOURCE WAVE\n    FILE \"a.wav\"\n  >\n  <EXT\n    nvk_item_type folder\n  >\n>\n";
    CHECK(NvkItemTypeFromChunk(take_only).empty());
    CHECK(NvkItemTypeFromChunk("garbage\n>>>\n<<").empty());
    CHECK(NvkItemTypeFromChunk("").empty());

    // Copy chunk (CRLF lines kept as CRLF).
    const std::string src =
        "<ITEM\r\n  POSITION 2\r\n  SEL 1\r\n  IGUID {OLD}\r\n  NAME a\r\n  GUID {T1}\r\n  <SOURCE WAVE\r\n"
        "    FILE \"a.wav\"\r\n  >\r\n  TAKE\r\n  GUID {T2}\r\n  <TAKEFX\r\n    FXID {F1}\r\n  >\r\n>";
    int         n = 0;
    std::string out = PrepareCopyChunk(src, "{NEW}", [&n] { return "{G" + std::to_string(++n) + "}"; });
    const std::string expect =
        "<ITEM\r\n  POSITION 2\r\n  SEL 0\r\n  IGUID {NEW}\r\n  NAME a\r\n  GUID {G1}\r\n  <SOURCE WAVE\r\n"
        "    FILE \"a.wav\"\r\n  >\r\n  TAKE\r\n  GUID {G2}\r\n  <TAKEFX\r\n    FXID {G3}\r\n  >\r\n>";
    CHECK(out == expect);
    CHECK(n == 3);

    if (g_fail) {
        std::printf("sound_place_plan_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("sound_place_plan_test: all passed\n");
    return 0;
}
