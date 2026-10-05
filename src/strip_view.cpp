// SPDX-License-Identifier: MIT
//
// See strip_view.h. Moved out of video_view_ui.cpp's DrawVideoShotStrip (Story 10-3) with
// its behaviour unchanged.

#include "strip_view.h"

#ifdef _WIN32

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "shortcuts.h"  // SavePrefFloat
#include "ui_theme.h"
#include "video_shot_timing.h"

namespace rav {

void StripHeightGrip(StripGripState& g, const char* id, float* stored_h, float h, float min_h, float max_h,
                     const char* pref_key)
{
    constexpr float kGripH = 4.0f;
    const ImVec2 content = ImGui::GetCursorScreenPos();
    const ImVec2 wpos = ImGui::GetWindowPos();
    ImGui::SetCursorScreenPos(wpos);
    ImGui::InvisibleButton(id, ImVec2(ImGui::GetWindowSize().x, kGripH));
    const float my = ImGui::GetIO().MousePos.y;
    if (ImGui::IsItemActivated()) {
        g.start_h = h;
        g.press_y = my;
        g.moved = false;
    }
    if (ImGui::IsItemActive() && my != g.press_y) g.moved = true;
    if (ImGui::IsItemActive() && g.moved) {
        const float nh = g.start_h - (my - g.press_y);
        *stored_h = std::floor(std::min(std::max(nh, min_h), max_h));
    }
    if (ImGui::IsItemDeactivated() && g.moved && pref_key) SavePrefFloat(pref_key, *stored_h);
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    ImGui::SetCursorScreenPos(content);
}

StripWindow StripViewUpdate(StripViewState& s, double item_start, double item_end, double playhead, bool playing,
                            double fps, bool hold, bool window_hovered, float mouse_x, float lane_x, float lane_w)
{
    // Spec 11-fb-12: the strip shows a view window [v0, v0 + span] of project time around
    // the current item: by default the item plus a margin, zoomed with Alt + wheel.
    const double item_span = item_end - item_start;
    const double min_span = VideoStripMinSpan(fps);
    const double max_span = VideoStripMaxSpan(item_span, fps);
    const double def_span =
        VideoStripClampSpan(item_span + 2.0 * VideoStripDefaultMargin(item_span, fps), min_span, max_span);
    const double def_v0 = (item_start + item_end) * 0.5 - def_span * 0.5;
    const bool item_changed = item_start != s.item_start || item_end != s.item_end;
    // Spec 11-fb-14: when REAPER starts playing, and on each new clip while it plays, the
    // view drops any zoom or scroll and frames the clip under the play position.
    const bool play_started = playing && !s.was_playing;
    if (!s.init) {
        s.v0 = def_v0;
        s.span = def_span;
        s.zoomed = false;
        s.scrolled = false;
        s.item_start = item_start;
        s.item_end = item_end;
        s.last_playhead = playhead;
        s.init = true;
        s.was_playing = playing;
    } else if (!hold) {  // the view never jumps during a lane drag
        if (play_started || (playing && item_changed)) {
            s.zoomed = false;
            s.scrolled = false;
        }
        s.was_playing = playing;
        if (item_changed || play_started) {
            // Unzoomed and unscrolled: frame the new item. A user view (zoomed or
            // scrolled): keep the span (within the new limits).
            if (!s.zoomed && !s.scrolled) {
                s.v0 = def_v0;
                s.span = def_span;
            } else {
                s.span = VideoStripClampSpan(s.span, min_span, max_span);
            }
            s.item_start = item_start;
            s.item_end = item_end;
        }
        // The playhead stays visible: the smallest shift that brings it back in.
        if (item_changed || play_started || playhead != s.last_playhead) s.v0 = VideoStripFollow(s.v0, s.span, playhead);
        s.last_playhead = playhead;
    }
    const double f = std::min(1.0, std::max(0.0, static_cast<double>((mouse_x - lane_x) / lane_w)));
    // Alt + wheel anywhere over the strip window (lane, ruler, empty band): zoom about the
    // time under the mouse (the lane's nearest edge when the mouse is beside it). The
    // viewer window keeps it from the 3D view and REAPER (viewer_window.cpp).
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyAlt && io.MouseWheel != 0.0f && !hold && window_hovered) {
            const double t_fixed = s.v0 + f * s.span;
            VideoStripZoom(s.v0, s.span, t_fixed, static_cast<double>(io.MouseWheel), min_span, max_span, &s.v0, &s.span);
            s.zoomed = std::fabs(s.span - def_span) > 1.0e-6 * def_span;
        } else if (io.KeyShift && io.MouseWheel != 0.0f && !hold && !playing && window_hovered) {
            // (Ignored while playing: the follow rule would pull the view back every frame.)
            // Spec 11-fb-14: Shift + wheel scrolls (up: earlier, down: later), 10 % of the
            // span per notch, never before the default margin before 0. A user view.
            s.v0 = VideoStripScroll(s.v0, s.span, static_cast<double>(io.MouseWheel),
                                    -VideoStripDefaultMargin(item_span, fps));
            s.scrolled = true;
        }
    }
    StripWindow w;
    w.v0 = s.v0;
    w.span = s.span;
    w.v1 = s.v0 + s.span;
    w.item_span = item_span;
    return w;
}

StripRuler StripRulerFor(const StripWindow& w, float lane_w, double fps)
{
    // The ruler's step: labelled ticks at least the widest label for this view apart (the
    // longest seconds label, or a "+Nf" frame label), so full-size labels never collide.
    // Spec 11-fb-12: project time, like REAPER's ruler (origin 0); fps unknown: seconds.
    float label_w;
    {
        const long long ifps = VideoRulerFps(fps);
        char frame_label[32];
        std::snprintf(frame_label, sizeof(frame_label), "+%lldf", ifps > 1 ? ifps - 1 : 0LL);
        const double t_far = std::max(std::fabs(w.v0), std::fabs(w.v1));
        const char* sec_label = t_far >= 36000.0 ? "00:00:00" : t_far >= 3600.0 ? "0:00:00"
                              : t_far >= 600.0  ? "00:00"    : "0:00";
        label_w = std::max(ImGui::CalcTextSize(sec_label).x, ifps > 1 ? ImGui::CalcTextSize(frame_label).x : 0.0f);
    }
    StripRuler r;
    r.step = VideoRulerStep(w.span, static_cast<double>(lane_w), fps,
                            static_cast<double>(kStripRulerLabelPad + label_w + kStripRulerLabelGap));
    // Spec 11-fb-9: minor ticks under it, down to one per frame.
    r.minor = VideoRulerMinorStep(r.step, w.span, static_cast<double>(lane_w), fps, kStripRulerMinorMinPx);
    // Snap lands on the finest graduation drawn (project frames).
    r.snap = VideoRulerSnapStep(r.step, r.minor);
    return r;
}

void DrawStripRuler(ImDrawList* dl, const StripWindow& w, const StripRuler& r, float lane_x, float lane_w,
                    float lane_top, float ruler_h, double fps)
{
    if (!(r.step > 0.0)) return;
    ImFont* const ruler_font = ImGui::GetFont();
    const float ruler_fs = ruler_h - kStripRulerTickRoom;
    const double t_lo = std::max(w.v0, 0.0);  // nothing before project time 0
    auto x_of = [&](double t) { return StripX(w, lane_x, lane_w, t); };
    // The ruler (spec 11-fb-6 / 11-fb-9): above the lane, in project time like REAPER's
    // ruler (spec 11-fb-12), over the view window. Whole seconds read "0:02", frame ticks
    // between them "+12f", at the full font size.
    const ImVec2 r0(lane_x, lane_top - ruler_h);
    dl->PushClipRect(ImVec2(r0.x - 1.0f, r0.y), ImVec2(lane_x + lane_w + 1.0f, lane_top), true);
    // Minor ticks (spec 11-fb-9): short, in the tick room under the labels, and the
    // faintest stroke; those on a labelled tick are skipped (same frame grid).
    const long long per_label = VideoRulerMinorPerLabel(r.step, r.minor, fps);
    if (per_label > 1) {
        const float minor_h = std::min(kStripRulerMinorTickH, ruler_h);
        const long long k0 = VideoRulerFirstTickFrom(t_lo, 0.0, r.minor, fps);
        for (long long k = k0; k <= k0 + 100000; ++k) {
            const double tick_t = VideoRulerTickTime(k, 0.0, r.minor, fps);
            if (tick_t > w.v1 + 1e-9) break;
            if (k % per_label == 0) continue;  // a labelled tick
            const float tick_x = x_of(tick_t);
            dl->AddLine(ImVec2(tick_x + 0.5f, lane_top - minor_h), ImVec2(tick_x + 0.5f, lane_top), ui::kStrokeStrong,
                        1.0f);
        }
    }
    const long long k0 = VideoRulerFirstTickFrom(t_lo, 0.0, r.step, fps);
    for (long long k = k0; k <= k0 + 100000; ++k) {
        const double tick_t = VideoRulerTickTime(k, 0.0, r.step, fps);
        if (tick_t > w.v1 + 1e-9) break;
        const float tick_x = x_of(tick_t);
        char label[32];
        VideoRulerTickLabel(k, r.step, fps, label, sizeof(label));
        const bool major = VideoRulerTickIsMajor(k, r.step, fps);
        // Labelled ticks rise past the minor ones and read brighter (minor kStrokeStrong <
        // frame kFaint < second kMuted on the dark surface): whole seconds the full band.
        const float tick_h = major ? ruler_h : std::max(3.0f, std::floor(ruler_h * 0.6f));
        dl->AddLine(ImVec2(tick_x + 0.5f, lane_top - tick_h), ImVec2(tick_x + 0.5f, lane_top),
                    major ? ui::kMuted : ui::kFaint, 1.0f);
        if (ruler_fs >= 6.0f)
            dl->AddText(ruler_font, ruler_fs, ImVec2(tick_x + kStripRulerLabelPad, r0.y), major ? ui::kText : ui::kMuted,
                        label);
    }
    dl->PopClipRect();
}

void StripScrub(StripViewState& s, bool scrubbing, bool pressed, float mouse_x, double t_seek, void (*seek)(double))
{
    if (!scrubbing) return;
    // Spec 11-fb-12: anywhere in the view (>= 0), no stop inside the item: a drag runs on
    // into the next clip.
    if ((pressed || mouse_x != s.last_seek_mx) && seek) seek(std::max(0.0, t_seek));
    s.last_seek_mx = mouse_x;
}

}  // namespace rav

#endif  // _WIN32
