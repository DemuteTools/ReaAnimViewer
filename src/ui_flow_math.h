// SPDX-License-Identifier: MIT
//
// The width arithmetic behind ui_flow.h (13-7a / 13-7b / 13-7c: the Library view adapts to any
// column width by construction). Pure, ImGui-free: text widths come from a measure function, so
// the host tests run it with a fixed-pitch measure. Every function answers one question:
// does it fit, and if not, where does it go and how wide is it -- never a width that overflows.

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace rav {
namespace flow {

// The width of the text [begin, end).
using MeasureFn = std::function<float(const char* begin, const char* end)>;

// How many bytes of `text` to keep so that they plus the ellipsis (`dots_w` wide) end within
// `avail`: text.size() when the whole text fits, else whole UTF-8 glyphs only (0 when even the
// ellipsis alone does not fit). Trailing spaces of the kept part are dropped.
size_t EllipsisKeep(const std::string& text, float avail, float dots_w, const MeasureFn& measure);

// The text shortened to `avail` with `dots` ("..."), or the text itself; *cut says which.
std::string Ellipsize(const std::string& text, float avail, const char* dots, const MeasureFn& measure,
                      bool* cut = nullptr);

// True when an item `next_w` wide, put `spacing` after the last item (ending at `last_right`),
// still ends within `line_right`.
bool FitsAfter(float last_right, float spacing, float next_w, float line_right);

// A labelled row: the label in a column `label_col` wide, its field beside it -- or, when the
// field would get less than `field_min`, the label on its own line and the field under it, the
// whole width. The label is never wider than what it is given (it is ellipsized to label_w).
struct LabelSplit {
    bool  beside = true;
    float label_w = 0.0f;  // the room the label is drawn in
    float field_dx = 0.0f; // the field's x from the row's left
    float field_w = 0.0f;  // the field's width
};
LabelSplit SplitLabelRow(float avail, float label_natural, float label_col, float field_min, float spacing);

// A value field followed by its unit ("dB", "ms"): the field's width so that both end within
// `avail` -- `want` when it fits, else shrunk, never under `field_min` (the unit is never cut:
// the field gives way first). unit_w 0 = no unit; `gap` sits between field and unit.
float FieldBeforeUnit(float avail, float want, float unit_w, float gap, float field_min);

// Two things on a row, `a` stretching and `b` fixed (`b_w`): `a` beside `b` when it keeps at least
// `a_min`, else `a` alone the whole width and `b` on the next line. Returns a's width; *beside.
float StretchBeside(float avail, float a_min, float b_w, float spacing, bool* beside);

// A segmented control's options: `text_w[i]` is each label's width. Each option is its text plus
// `pad_nat` (natural) and may shrink to its text plus `pad_min`; `gap` sits around and between
// options. Lays them out within `avail`:
//   - one row at their natural widths when it fits;
//   - else one row, each option between its minimum and natural width (the room shared out);
//   - else several rows (greedy, minimum widths), each row stretched to the full width;
//   - an option wider than the whole width alone gets the whole width (its label is ellipsized).
// Writes out_w[i] and out_row[i]; returns the number of rows.
int LayoutSegments(const float* text_w, int n, float avail, float pad_nat, float pad_min, float gap, float* out_w,
                   int* out_row);

// Trailing badges / buttons on a row (counts, "!", "Locate..."), left to right, `w[i]` wide:
// beside the row's leading part when that keeps `lead_min` (from `left`), right-aligned on line 0;
// else on the lines under it, packed greedily and right-aligned, each never wider than the row.
struct TrailPlace {
    float x = 0.0f;
    float w = 0.0f;
    int   line = 0;
};
// Returns the row's number of lines (1 when the trail stays beside the lead or is empty).
int PlaceTrail(const float* w, int n, float left, float right, float lead_min, float gap, TrailPlace* out);

// 13-7 (B5): a fader (ui::Fader). Its thumb (radius `thumb_r`) has its centre from x0 + r to x1 - r.
// The value under the mouse at `mouse_x`, clamped to [v_min, v_max]; v_min when the track has no room.
// 13-14 (G-13): `log` spreads the range logarithmically along the track (a distance, a zoom); it
// needs 0 < v_min < v_max, else the track stays linear.
float FaderValueAt(float mouse_x, float x0, float x1, float thumb_r, float v_min, float v_max, bool log = false);
// The thumb centre's x for value `v` (clamped to the range).
float FaderThumbX(float v, float x0, float x1, float thumb_r, float v_min, float v_max, bool log = false);

// 13-14 (C4.2, the mockup's .row > .k): a label too wide for its column wraps onto a second line
// (never more). The byte where the second line starts: after the last space whose first line still
// fits `width`; 0 when the text fits on one line, or when no space allows a first line that fits
// (the label is then ellipsized on one line). The second line may itself be ellipsized.
size_t TwoLineBreak(const std::string& text, float width, const MeasureFn& measure);

// 13-7 (B6): a number as the mockup shows it ("0", "-14.5", "1.25"): rounded to `decimals` (0..9),
// trailing zeros and a bare point dropped, never "-0".
std::string TrimmedNumber(double v, int decimals);

// 13-30: true when `text` is one visible glyph (exactly one UTF-8 code point, not a space): the
// minus, "+" or cross of a glyph-only button.
bool SingleGlyph(const std::string& text);

// 13-30: how a button draws its label (`label_w` wide) in a frame `frame_w` wide with `pad` on each
// side. Padded: it fits with the padding (drawn as usual). Bare: it fits once the padding is given up,
// or it is a single glyph: drawn whole and centred, never cut to "..". Cut: ellipsized within the pad.
enum class LabelFit { Padded, Bare, Cut };
LabelFit ButtonLabelFit(const std::string& label, float label_w, float frame_w, float pad);

// 13-30: the narrowest a button may shrink to: a single glyph keeps its width plus a pixel each side;
// any other label 0 (it may be ellipsized).
float GlyphButtonMinWidth(const std::string& label, float label_w);

// 13-47: a soft drop shadow drawn as `bands` concentric layers, layer i spreading (i + 1) / bands of
// the spread past the window. out[i] gets each layer's own alpha so that the stacked layers darken
// band j (between layers j - 1 and j) to peak x (1 - j / bands)^2: `peak` at the window's edge,
// fading smoothly to nothing at the spread. `peak` is clamped to [0, 0.95].
void ShadowLayerAlphas(float peak, int bands, float* out);

}  // namespace flow
}  // namespace rav
