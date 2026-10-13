// SPDX-License-Identifier: MIT
//
// See ui_flow_math.h.

#include "ui_flow_math.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace rav {
namespace flow {

size_t EllipsisKeep(const std::string& text, float avail, float dots_w, const MeasureFn& measure)
{
    const char* b = text.c_str();
    if (measure(b, b + text.size()) <= avail) return text.size();
    // Glyph boundaries (whole UTF-8 sequences); the prefix width only grows, so binary search
    // for the longest prefix that fits with the dots: O(log n) measures, not O(n).
    std::vector<size_t> cuts;
    cuts.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) cuts.push_back(i);  // cuts[0] == 0
    size_t lo = 0, hi = cuts.size();  // cuts[lo] fits (0 always "fits"), cuts[hi] (== size) does not
    while (hi - lo > 1) {
        const size_t mid = lo + (hi - lo) / 2;
        if (measure(b, b + cuts[mid]) + dots_w <= avail) lo = mid;
        else hi = mid;
    }
    size_t n = cuts.empty() ? 0 : cuts[lo];
    while (n > 0 && text[n - 1] == ' ') --n;
    return n;
}

std::string Ellipsize(const std::string& text, float avail, const char* dots, const MeasureFn& measure, bool* cut)
{
    const std::string d = dots ? dots : "";
    const float       dw = d.empty() ? 0.0f : measure(d.c_str(), d.c_str() + d.size());
    const size_t      keep = EllipsisKeep(text, avail, dw, measure);
    if (cut) *cut = keep < text.size();
    if (keep >= text.size()) return text;
    return text.substr(0, keep) + d;
}

bool FitsAfter(float last_right, float spacing, float next_w, float line_right)
{
    return last_right + spacing + next_w <= line_right + 0.5f;  // half a pixel of rounding slack
}

LabelSplit SplitLabelRow(float avail, float label_natural, float label_col, float field_min, float spacing)
{
    LabelSplit s;
    avail = std::max(1.0f, avail);
    if (avail - label_col - spacing >= field_min) {
        s.beside = true;
        s.label_w = std::max(1.0f, label_col - spacing);
        s.field_dx = label_col;
        s.field_w = std::max(1.0f, avail - label_col);
        (void)label_natural;
    } else {
        s.beside = false;
        s.label_w = std::min(avail, std::max(1.0f, label_natural));
        s.field_dx = 0.0f;
        s.field_w = avail;
    }
    return s;
}

float FieldBeforeUnit(float avail, float want, float unit_w, float gap, float field_min)
{
    const float room = avail - (unit_w > 0.0f ? unit_w + gap : 0.0f);
    return std::max(field_min, std::min(want, room));
}

float StretchBeside(float avail, float a_min, float b_w, float spacing, bool* beside)
{
    const float a = avail - b_w - spacing;
    const bool  fits = a >= a_min;
    if (beside) *beside = fits;
    return fits ? a : std::max(1.0f, avail);
}

int LayoutSegments(const float* text_w, int n, float avail, float pad_nat, float pad_min, float gap, float* out_w,
                   int* out_row)
{
    if (n <= 0) return 0;
    float nat = gap, mn = gap;
    for (int i = 0; i < n; ++i) {
        nat += text_w[i] + pad_nat + gap;
        mn += text_w[i] + pad_min + gap;
    }
    if (nat <= avail) {
        for (int i = 0; i < n; ++i) {
            out_w[i] = text_w[i] + pad_nat;
            out_row[i] = 0;
        }
        return 1;
    }
    if (mn <= avail) {
        // One row: every option gets the same share of the room above its minimum.
        const float extra = std::min(pad_nat - pad_min, (avail - mn) / static_cast<float>(n));
        for (int i = 0; i < n; ++i) {
            out_w[i] = text_w[i] + pad_min + extra;
            out_row[i] = 0;
        }
        return 1;
    }
    // Several rows: greedy at the minimum widths, then each row stretched to the whole width.
    int   row = 0, first = 0;
    float x = gap;
    auto  close_row = [&](int end) {
        const int   k = end - first;
        float       sum = 0.0f;
        for (int j = first; j < end; ++j) sum += text_w[j] + pad_min;
        const float room = std::max(1.0f, avail - gap * static_cast<float>(k + 1));
        if (sum >= room) {  // a lone option wider than the row (or rounding): share the row
            for (int j = first; j < end; ++j) out_w[j] = std::max(1.0f, room / static_cast<float>(k));
        } else {
            const float add = (room - sum) / static_cast<float>(k);
            for (int j = first; j < end; ++j) out_w[j] = text_w[j] + pad_min + add;
        }
    };
    for (int i = 0; i < n; ++i) {
        const float w = text_w[i] + pad_min;
        if (i > first && x + w + gap > avail) {
            close_row(i);
            ++row;
            first = i;
            x = gap;
        }
        out_row[i] = row;
        x += w + gap;
    }
    close_row(n);
    return row + 1;
}

int PlaceTrail(const float* w, int n, float left, float right, float lead_min, float gap, TrailPlace* out)
{
    if (n <= 0) return 1;
    const float row_w = std::max(1.0f, right - left);
    float total = 0.0f;
    for (int i = 0; i < n; ++i) total += w[i] + (i ? gap : 0.0f);
    if (left + lead_min + gap + total <= right) {
        float x = right - total;
        for (int i = 0; i < n; ++i) {
            out[i].x = x;
            out[i].w = w[i];
            out[i].line = 0;
            x += w[i] + gap;
        }
        return 1;
    }
    int line = 1, first = 0;
    float used = 0.0f;
    auto close_line = [&](int end) {
        float x = std::max(left, right - used);
        for (int j = first; j < end; ++j) {
            out[j].w = std::min(w[j], row_w);
            out[j].x = x;
            out[j].line = line;
            x += out[j].w + gap;
        }
    };
    for (int i = 0; i < n; ++i) {
        const float wi = std::min(w[i], row_w);
        if (i > first && used + gap + wi > row_w) {
            close_line(i);
            ++line;
            first = i;
            used = 0.0f;
        }
        used += (i > first ? gap : 0.0f) + wi;
    }
    close_line(n);
    return line + 1;
}

namespace {

bool LogRange(float v_min, float v_max) { return v_min > 0.0f && v_max > v_min; }

}  // namespace

float FaderValueAt(float mouse_x, float x0, float x1, float thumb_r, float v_min, float v_max, bool log)
{
    const float a = x0 + thumb_r, b = x1 - thumb_r;
    if (!(b > a)) return v_min;
    float t = (mouse_x - a) / (b - a);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    if (log && LogRange(v_min, v_max)) {
        if (t <= 0.0f) return v_min;
        if (t >= 1.0f) return v_max;
        return v_min * std::pow(v_max / v_min, t);
    }
    return v_min + t * (v_max - v_min);
}

float FaderThumbX(float v, float x0, float x1, float thumb_r, float v_min, float v_max, bool log)
{
    const float a = x0 + thumb_r, b = x1 - thumb_r;
    if (!(b > a) || !(v_max != v_min)) return a;
    float t = 0.0f;
    if (log && LogRange(v_min, v_max)) t = v <= v_min ? 0.0f : std::log(v / v_min) / std::log(v_max / v_min);
    else t = (v - v_min) / (v_max - v_min);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return a + t * (b - a);
}

size_t TwoLineBreak(const std::string& text, float width, const MeasureFn& measure)
{
    const char* b = text.c_str();
    if (text.empty() || measure(b, b + text.size()) <= width) return 0;
    // The last space whose first line (its trailing spaces dropped) fits.
    for (size_t i = text.size(); i-- > 0;) {
        if (text[i] != ' ') continue;
        size_t end = i;
        while (end > 0 && text[end - 1] == ' ') --end;
        if (end == 0) return 0;  // only spaces before it: no first line
        if (measure(b, b + end) > width) continue;
        size_t next = i + 1;
        while (next < text.size() && text[next] == ' ') ++next;
        return next < text.size() ? next : 0;  // nothing after the space: one line
    }
    return 0;
}

std::string TrimmedNumber(double v, int decimals)
{
    const int d = decimals < 0 ? 0 : (decimals > 9 ? 9 : decimals);
    char      buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", d, v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

bool SingleGlyph(const std::string& text)
{
    if (text.empty() || text[0] == ' ') return false;
    size_t leads = 0;
    for (const char c : text)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++leads;
    return leads == 1;
}

LabelFit ButtonLabelFit(const std::string& label, float label_w, float frame_w, float pad)
{
    if (label_w <= frame_w - 2.0f * pad) return LabelFit::Padded;
    if (label_w + 2.0f <= frame_w || SingleGlyph(label)) return LabelFit::Bare;
    return LabelFit::Cut;
}

float GlyphButtonMinWidth(const std::string& label, float label_w)
{
    return SingleGlyph(label) ? label_w + 2.0f : 0.0f;
}

void ShadowLayerAlphas(float peak, int bands, float* out)
{
    if (!out || bands <= 0) return;
    peak = std::min(std::max(peak, 0.0f), 0.95f);
    // The opacity wanted in band j; the outermost layer alone covers the last band, and each layer
    // further in adds what band j lacks over band j + 1: 1 - (1 - f(j)) / (1 - f(j + 1)).
    auto f = [&](int j) {
        if (j >= bands) return 0.0f;
        const float t = 1.0f - static_cast<float>(j) / static_cast<float>(bands);
        return peak * t * t;
    };
    for (int j = 0; j < bands; ++j) out[j] = 1.0f - (1.0f - f(j)) / (1.0f - f(j + 1));
}

}  // namespace flow
}  // namespace rav
