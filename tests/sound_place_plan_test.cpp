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

    // 13-10 (Antho 2026-10-12): an attached sound is never replaced or removed; it may move.
    CHECK(PlacedItemRefusal(false, false, false, Ownership::Populated, false) == "item missing");
    CHECK(PlacedItemRefusal(true, false, false, Ownership::Populated, true) == "not a placed item (no RAV_SND tag)");
    CHECK(PlacedItemRefusal(true, true, true, Ownership::Populated, false) == "folder item");
    CHECK(PlacedItemRefusal(true, true, false, Ownership::Attached, true) == "attached by the user");
    CHECK(PlacedItemRefusal(true, true, false, Ownership::Attached, false).empty());
    CHECK(PlacedItemRefusal(true, true, false, Ownership::Spread, true).empty());
    CHECK(PlacedItemRefusal(true, true, false, Ownership::Populated, true).empty());
    CHECK(PlacedItemRefusal(true, true, true, Ownership::Spread, true) == "folder item");
    CHECK(PlacedItemRefusal(true, false, false, Ownership::Populated, false) == "not a placed item (no RAV_SND tag)");

    // 13-11 (B9): a library source is muted; its copy is placed unmuted, any other source's mute copied.
    CHECK(!PlacedCopyMuted(true, true));
    CHECK(!PlacedCopyMuted(false, true));
    CHECK(PlacedCopyMuted(true, false));
    CHECK(!PlacedCopyMuted(false, false));

    // Antho 2026-10-12 (13-11): a File Sound is placed straight from its file.
    {
        SoundData d;
        d.mode = DetectMode::Whole;
        SoundFrame f;
        f.start = 0.25;
        f.length = 0.5;
        d.frames = {f};
        SliceApply sl = FileSliceFor(d, 0);  // the frame in every mode (Whole too)
        CHECK(sl.set && Near(sl.start_offs, 0.25) && Near(sl.length, 0.5));
        d.mode = DetectMode::Takes;
        CHECK(FileSliceFor(d, 0).set);
        CHECK(!FileSliceFor(d, 1).set && !FileSliceFor(d, -1).set);  // out of range: the whole file
        FadeApply fa = FileFadeApplyFor(d, 0);  // both sides, the Sound's default, with shapes
        CHECK(fa.set_in && fa.set_out && fa.shape_in && fa.shape_out && Near(fa.in, 0.001) && Near(fa.out, 0.005));
        d.frames[0].fade_in = 0.05;
        d.frames[0].fade_in_shape = 3;
        fa = FileFadeApplyFor(d, 0);
        CHECK(Near(fa.in, 0.05) && fa.in_shape == 3 && Near(fa.out, 0.005));
        d.fade_out = 0.02;
        fa = FileFadeApplyFor(d, 4);  // no frame: the Sound's default
        CHECK(fa.set_in && fa.set_out && Near(fa.in, 0.001) && Near(fa.out, 0.02) && fa.in_shape == 0);

        // The item: the slice, the event minus the sync plus Blend and nudge, the gain, the fades.
        FileItemFields it = FileItemFieldsFor(2.0, sl, FileFadeApplyFor(d, 0), -6.0, 10.0, 20.0, 0.5);
        CHECK(it.ok && Near(it.start_offs, 0.25) && Near(it.length, 0.5) && Near(it.snap_offset, 0.0));
        CHECK(Near(it.position, 10.52));
        CHECK(Near(it.volume, std::pow(10.0, -6.0 / 20.0)));
        CHECK(Near(it.fade_in, 0.05) && Near(it.fade_out, 0.02));  // the Sound default set above
        it = FileItemFieldsFor(3.0, SliceApply{}, FadeApply{}, 0.0, 1.0, 0.0, 0.0);  // whole file
        CHECK(it.ok && Near(it.start_offs, 0.0) && Near(it.length, 3.0) && Near(it.volume, 1.0) && Near(it.position, 1.0));
        CHECK(Near(it.fade_in, 0.0) && Near(it.fade_out, 0.0));
        SliceApply past;
        past.set = true;
        past.start_offs = 1.5;
        past.length = 1.0;
        it = FileItemFieldsFor(2.0, past, FadeApply{}, 0.0, 0.0, 0.0, 0.0);  // clamped into the file
        CHECK(it.ok && Near(it.length, 0.5));
        past.start_offs = 2.5;
        CHECK(!FileItemFieldsFor(2.0, past, FadeApply{}, 0.0, 0.0, 0.0, 0.0).ok);  // nothing left
        CHECK(FileItemFieldsFor(0.0, past, FadeApply{}, 0.0, 0.0, 0.0, 0.0).ok);   // length unknown: as is
        CHECK(!FileItemFieldsFor(0.0, SliceApply{}, FadeApply{}, 0.0, 0.0, 0.0, 0.0).ok);
        it = FileItemFieldsFor(1.0, SliceApply{}, FileFadeApplyFor(d, 0), 0.0, 0.0, -50.0, 0.0);  // clamped at 0
        CHECK(it.ok && Near(it.position, 0.0));
        FadeApply big;
        big.set_in = big.set_out = true;
        big.in = 0.8;
        big.out = 0.8;
        it = FileItemFieldsFor(1.0, SliceApply{}, big, 0.0, 0.0, 0.0, 0.0);
        CHECK(it.fade_in + it.fade_out <= 1.0 + 1e-9);  // fades clamped into the item

        CHECK(MediaCopyName("steps.wav", 0) == "steps.wav");
        CHECK(MediaCopyName("steps.wav", 1) == "steps-001.wav");
        CHECK(MediaCopyName("my.steps.flac", 12) == "my.steps-012.flac");
        CHECK(MediaCopyName("noext", 2) == "noext-002");
        CHECK(MediaCopyName(".hidden", 1) == ".hidden-001");
    }

    // Antho 2026-10-12: the fades a placed variation sets.
    {
        SoundData d;
        d.mode = DetectMode::Silence;
        SoundFrame f;
        f.start = 1.0;
        f.length = 0.5;
        d.frames = {f};
        FadeApply fa = FadeApplyFor(d, 0);  // a slice: the Sound's default on both sides
        CHECK(fa.set_in && fa.set_out && Near(fa.in, 0.001) && Near(fa.out, 0.005) && fa.in_shape == 0);
        CHECK(fa.shape_in && fa.shape_out);  // a slice writes its shapes
        fa = FadeApplyFor(d, 0, true);       // a folder-pile source: no fades placed
        CHECK(!fa.set_in && !fa.set_out);
        CHECK(!FadeApplyFor(d, 1).set_in && !FadeApplyFor(d, -1).set_out);  // out of range: nothing
        d.frames[0].fade_out = 0.1;
        d.frames[0].fade_out_shape = 2;
        fa = FadeApplyFor(d, 0);
        CHECK(Near(fa.out, 0.1) && fa.out_shape == 2 && Near(fa.in, 0.001));
        d.mode = DetectMode::Takes;  // a take too
        CHECK(FadeApplyFor(d, 0).set_in && FadeApplyFor(d, 0).set_out);
        // A Whole-item Sound keeps its item's own fades, except a side set by hand.
        d.mode = DetectMode::Whole;
        fa = FadeApplyFor(d, 0);
        CHECK(!fa.set_in && fa.set_out && Near(fa.out, 0.1) && fa.shape_out);
        d.frames[0].fade_out_shape = 0;  // a hand-set length with the default shape: the item's shape kept
        fa = FadeApplyFor(d, 0);
        CHECK(fa.set_out && !fa.shape_out);
        d.frames[0].fade_out = -1.0;
        fa = FadeApplyFor(d, 0);
        CHECK(!fa.set_in && !fa.set_out);
        // On the item: playrate 2 halves the fades like the item; the copy's own for an unset side.
        FadeApply x;
        x.set_in = true;
        x.in = 0.01;
        double in = -1.0, out = -1.0;
        FadesOnItem(x, 2.0, 0.25, 0.3, 0.02, &in, &out);
        CHECK(Near(in, 0.005) && Near(out, 0.02));
        // Clamped to the placed item's length.
        x.set_out = true;
        x.out = 1.0;
        x.in = 1.0;
        FadesOnItem(x, 1.0, 0.5, 0.0, 0.0, &in, &out);
        CHECK(Near(in, 0.25) && Near(out, 0.25));
        FadesOnItem(x, 0.0, 0.5, 0.0, 0.0, &in, &out);  // a bad rate reads as 1
        CHECK(Near(in + out, 0.5));
    }

    if (g_fail) {
        std::printf("sound_place_plan_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("sound_place_plan_test: all passed\n");
    return 0;
}
