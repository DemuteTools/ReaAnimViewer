// SPDX-License-Identifier: MIT
//
// Host test of the adaptive layout arithmetic (src/ui_flow_math.h, 13-7a / 13-7b / 13-7c): at any
// width, nothing laid out by it runs past the room it is given. A fixed-pitch measure (7 px per
// glyph, the default ImGui font's advance) stands in for ImGui.
//   c++ -std=c++17 -I src tests/ui_flow_test.cpp src/ui_flow_math.cpp -o t && ./t

#include "ui_flow_math.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace rav::flow;

namespace {

int g_fails = 0;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);        \
            ++g_fails;                                              \
        }                                                           \
    } while (0)

// 7 px per UTF-8 glyph (continuation bytes take no width).
float Measure(const char* b, const char* e)
{
    float w = 0.0f;
    for (const char* p = b; p < e; ++p)
        if ((static_cast<unsigned char>(*p) & 0xC0) != 0x80) w += 7.0f;
    return w;
}

bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

}  // namespace

int main()
{
    const MeasureFn m = Measure;

    // ---- Ellipsize ----
    {
        bool cut = true;
        CHECK(Ellipsize("Footsteps", 63.0f, "...", m, &cut) == "Footsteps" && !cut);  // exactly fits
        CHECK(Ellipsize("Footsteps", 62.0f, "...", m, &cut) == "Foots..." && cut);    // 5*7 + 21 = 56 <= 62
        CHECK(Ellipsize("Footsteps", 55.0f, "...", m) == "Foot...");                  // 56 > 55
        CHECK(Ellipsize("Footsteps", 20.0f, "...", m, &cut).empty() == false);         // "..." alone
        CHECK(Ellipsize("Footsteps", 20.0f, "...", m) == "...");
        // Trailing spaces of the kept part are dropped.
        CHECK(Ellipsize("Wood Hard Big", 56.0f, "...", m) == "Wood...");
        // A multi-byte glyph is never split (e acute = 2 bytes, one glyph).
        const std::string fr = "R\xC3\xA9verb\xC3\xA9ration";
        const std::string e = Ellipsize(fr, 7.0f * 2.0f + 21.0f, "...", m);
        CHECK(e == "R\xC3\xA9...");
        CHECK(EllipsisKeep(fr, 1000.0f, 21.0f, m) == fr.size());
        // A long string: same result as the glyph-by-glyph walk, in a logarithmic number of measures.
        std::string lng;
        for (int i = 0; i < 2000; ++i) lng += (i % 7 == 3) ? "\xC3\xA9" : "a";
        int calls = 0;
        const MeasureFn counted = [&](const char* a, const char* b) { ++calls; return Measure(a, b); };
        const size_t keep = EllipsisKeep(lng, 7.0f * 1000.0f + 21.0f, 21.0f, counted);
        size_t glyphs = 0, at = 0;
        for (; at < lng.size() && glyphs < 1000; ++at)
            if ((static_cast<unsigned char>(lng[at]) & 0xC0) != 0x80) ++glyphs;
        while (at < lng.size() && (static_cast<unsigned char>(lng[at]) & 0xC0) == 0x80) ++at;
        CHECK(keep == at);
        CHECK(calls <= 16);
    }

    // ---- FitsAfter ----
    CHECK(FitsAfter(100.0f, 8.0f, 50.0f, 158.0f));
    CHECK(!FitsAfter(100.0f, 8.0f, 51.0f, 158.0f));

    // ---- A labelled row ----
    {
        // Column 3 at its minimum: a section's inner width 218 -- Volume (field min 120) goes beside.
        LabelSplit s = SplitLabelRow(218.0f, 42.0f, 90.0f, 120.0f, 8.0f);
        CHECK(s.beside && Near(s.field_dx, 90.0f) && Near(s.field_w, 128.0f));
        CHECK(s.field_dx + s.field_w <= 218.0f);
        // Split by (field min 160): under its label, the whole width.
        s = SplitLabelRow(218.0f, 56.0f, 90.0f, 160.0f, 8.0f);
        CHECK(!s.beside && Near(s.field_w, 218.0f) && Near(s.label_w, 56.0f));
        // A label longer than a tiny row: never wider than the row.
        s = SplitLabelRow(30.0f, 200.0f, 90.0f, 60.0f, 8.0f);
        CHECK(!s.beside && s.label_w <= 30.0f && s.field_w <= 30.0f);
    }

    // ---- A field and its unit: the unit is never cut ----
    {
        // The old volume row: 60 px left for a 48 px field + 4 + "dB" (14) = 66 -> the field gives way.
        const float f = FieldBeforeUnit(60.0f, 48.0f, 4.0f + 14.0f, 0.0f, 24.0f);
        CHECK(Near(f, 42.0f) && f + 18.0f <= 60.0f);
        CHECK(Near(FieldBeforeUnit(200.0f, 48.0f, 18.0f, 0.0f, 24.0f), 48.0f));   // room: its own width
        CHECK(Near(FieldBeforeUnit(10.0f, 48.0f, 18.0f, 0.0f, 24.0f), 24.0f));    // its minimum, no less
        CHECK(Near(FieldBeforeUnit(100.0f, 48.0f, 0.0f, 4.0f, 24.0f), 48.0f));    // no unit
    }

    // ---- A fader beside its dB field, or above it ----
    {
        bool beside = false;
        float a = StretchBeside(128.0f, 40.0f, 66.0f, 8.0f, &beside);
        CHECK(beside && Near(a, 54.0f) && a + 8.0f + 66.0f <= 128.0f);
        a = StretchBeside(100.0f, 40.0f, 66.0f, 8.0f, &beside);
        CHECK(!beside && Near(a, 100.0f));
    }

    // ---- Segmented ----
    {
        float w[8];
        int   row[8];
        // Random / Blend / Switch at 218: natural (text + 24), one row.
        const float t3[3] = {42.0f, 35.0f, 42.0f};
        CHECK(LayoutSegments(t3, 3, 218.0f, 24.0f, 8.0f, 2.0f, w, row) == 1);
        CHECK(Near(w[0], 66.0f) && Near(w[1], 59.0f) && row[2] == 0);
        // "Per variation" / "Whole sound" at 224 (natural 228): shared on one row, each >= its text + 8.
        const float t2[2] = {91.0f, 77.0f};
        CHECK(LayoutSegments(t2, 2, 224.0f, 24.0f, 8.0f, 2.0f, w, row) == 1);
        CHECK(2.0f + w[0] + 2.0f + w[1] + 2.0f <= 224.01f && w[0] >= 99.0f && w[1] >= 85.0f);
        // Split by: Whole file / Takes / Take markers / Silence at 218: two rows, each filling the width.
        const float t4[4] = {70.0f, 35.0f, 84.0f, 49.0f};
        const int rows = LayoutSegments(t4, 4, 218.0f, 24.0f, 8.0f, 2.0f, w, row);
        CHECK(rows == 2);
        for (int r = 0; r < rows; ++r) {
            float x = 2.0f;
            for (int i = 0; i < 4; ++i)
                if (row[i] == r) {
                    x += w[i] + 2.0f;
                }
            CHECK(Near(x, 218.0f));  // stretched to the full width, never past it
        }
        for (int i = 0; i < 4; ++i) CHECK(w[i] >= t4[i] + 8.0f);  // every label whole
        // An option wider than the whole width: it alone gets the width.
        const float big[2] = {300.0f, 20.0f};
        CHECK(LayoutSegments(big, 2, 100.0f, 24.0f, 8.0f, 2.0f, w, row) == 2);
        CHECK(Near(w[0], 96.0f) && row[0] == 0 && row[1] == 1);
    }

    // ---- Trailing badges ----
    {
        TrailPlace tp[4];
        // A library row 108 wide: "12" (14) and "!" (12) beside a name that keeps 52 px.
        const float b2[2] = {14.0f, 12.0f};
        CHECK(PlaceTrail(b2, 2, 16.0f, 108.0f, 52.0f, 6.0f, tp) == 1);
        CHECK(Near(tp[1].x + tp[1].w, 108.0f) && tp[0].line == 0);
        // "User" + "12" + "!" + "..." (28, 14, 12, 22 + gaps = 94): the name would keep < 52 -> the
        // badges go under it, on one line when the row (94 wide) takes them all, else on two.
        const float b4[4] = {28.0f, 14.0f, 12.0f, 22.0f};
        CHECK(PlaceTrail(b4, 4, 16.0f, 110.0f, 52.0f, 6.0f, tp) == 2);
        for (int i = 0; i < 4; ++i) CHECK(tp[i].line == 1 && tp[i].x >= 16.0f && tp[i].x + tp[i].w <= 110.01f);
        CHECK(PlaceTrail(b4, 4, 16.0f, 108.0f, 52.0f, 6.0f, tp) == 3);
        for (int i = 0; i < 4; ++i) CHECK(tp[i].line >= 1 && tp[i].x >= 16.0f && tp[i].x + tp[i].w <= 108.01f);
        // A user file: "in project" (70), "3" (7), "Locate..." button (64) in 92 px: wraps over two lines.
        const float b3[3] = {70.0f, 7.0f, 64.0f};
        const int l3 = PlaceTrail(b3, 3, 16.0f, 108.0f, 52.0f, 6.0f, tp);
        CHECK(l3 == 3 && tp[0].line == 1 && tp[1].line == 1 && tp[2].line == 2);
        for (int i = 0; i < 3; ++i) CHECK(tp[i].x >= 16.0f && tp[i].x + tp[i].w <= 108.01f);
        // Nothing trailing: one line.
        CHECK(PlaceTrail(nullptr, 0, 0.0f, 50.0f, 52.0f, 6.0f, tp) == 1);
        // A badge wider than the row is narrowed to it.
        const float wide[1] = {200.0f};
        CHECK(PlaceTrail(wide, 1, 16.0f, 108.0f, 52.0f, 6.0f, tp) == 2 && Near(tp[0].w, 92.0f) && Near(tp[0].x, 16.0f));
    }

    // ---- 13-7 B5: the fader's value <-> thumb position ----
    {
        // A 200 px track (x 10..210), thumb radius 8: the centre runs 18..202 (184 px) for -60..12 dB.
        CHECK(Near(FaderValueAt(18.0f, 10.0f, 210.0f, 8.0f, -60.0f, 12.0f), -60.0f));
        CHECK(Near(FaderValueAt(202.0f, 10.0f, 210.0f, 8.0f, -60.0f, 12.0f), 12.0f));
        CHECK(Near(FaderValueAt(0.0f, 10.0f, 210.0f, 8.0f, -60.0f, 12.0f), -60.0f));    // left of the track: clamped
        CHECK(Near(FaderValueAt(999.0f, 10.0f, 210.0f, 8.0f, -60.0f, 12.0f), 12.0f));   // right of it: clamped
        CHECK(Near(FaderValueAt(110.0f, 10.0f, 210.0f, 8.0f, -60.0f, 12.0f), -24.0f));  // the middle
        CHECK(Near(FaderThumbX(-24.0f, 10.0f, 210.0f, 8.0f, -60.0f, 12.0f), 110.0f));
        CHECK(Near(FaderThumbX(0.0f, 10.0f, 210.0f, 8.0f, -60.0f, 12.0f), 18.0f + 184.0f * 60.0f / 72.0f));
        CHECK(Near(FaderThumbX(50.0f, 10.0f, 210.0f, 8.0f, -60.0f, 12.0f), 202.0f));    // past the range: the end
        // Round trip at every pixel.
        for (float x = 18.0f; x <= 202.0f; x += 1.0f)
            CHECK(Near(FaderThumbX(FaderValueAt(x, 10.0f, 210.0f, 8.0f, -60.0f, 12.0f), 10.0f, 210.0f, 8.0f, -60.0f, 12.0f), x));
        // No room for the thumb's travel (a track narrower than the thumb): the minimum, the left end.
        CHECK(Near(FaderValueAt(15.0f, 10.0f, 20.0f, 8.0f, -60.0f, 12.0f), -60.0f));
        CHECK(Near(FaderThumbX(0.0f, 10.0f, 20.0f, 8.0f, -60.0f, 12.0f), 18.0f));
        CHECK(Near(FaderThumbX(5.0f, 10.0f, 210.0f, 8.0f, 5.0f, 5.0f), 18.0f));          // an empty range
    }

    // ---- 13-7 B6: numbers shown as the mockup shows them ----
    {
        CHECK(TrimmedNumber(0.0, 1) == "0");
        CHECK(TrimmedNumber(-0.04, 1) == "0");  // rounds to -0.0: never "-0"
        CHECK(TrimmedNumber(-14.5, 1) == "-14.5");
        CHECK(TrimmedNumber(-14.0, 1) == "-14");
        CHECK(TrimmedNumber(1.25, 2) == "1.25");
        CHECK(TrimmedNumber(1.20, 2) == "1.2");
        CHECK(TrimmedNumber(144.0, 0) == "144");
        CHECK(TrimmedNumber(100.0, 0) == "100");  // zeros before the point stay
        CHECK(TrimmedNumber(12.0, -3) == "12");
    }

    if (g_fails) {
        std::printf("%d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("ui_flow: all passed\n");
    return 0;
}
