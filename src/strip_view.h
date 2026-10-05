// SPDX-License-Identifier: MIT
//
// The shared timeline strip (Story 10-3, generalised from Epic 11's shot strip): the parts
// Video view's shot strip and Tagging view's curve strip have in common. Each strip keeps
// its own state struct (no statics here):
//   - the height grip on the strip window's top edge (kept in ExtState on release);
//   - the view window [v0, v0 + span] of project time around the current item: framed by
//     default with a margin, following the playhead, re-framed when REAPER starts playing;
//     Alt + wheel zooms about the mouse, Shift + wheel scrolls (spec 11-fb-12 / 11-fb-14);
//   - time <-> x over a lane;
//   - the seconds / frames ruler above a lane (spec 11-fb-6 / 11-fb-9);
//   - click / drag seek.
// The pure maths lives in video_shot_timing.h (host-tested). Dear ImGui, main thread.

#pragma once

#ifdef _WIN32

#include <imgui.h>

namespace rav {

// The ruler's layout (spec 11-fb-6 / 11-fb-9).
constexpr float kStripRulerTickRoom = 6.0f;    // band height under the labels (minor ticks)
constexpr float kStripRulerMinorTickH = 4.0f;  // minor tick height: inside the tick room
constexpr float kStripRulerMinorMinPx = 4.0f;  // minor ticks at least this far apart
constexpr float kStripRulerLabelPad = 3.0f;    // label text starts this far right of its tick
constexpr float kStripRulerLabelGap = 12.0f;   // free space after the widest label
static_assert(kStripRulerMinorTickH <= kStripRulerTickRoom, "minor ticks stay under the labels");

// ---- Height grip ---------------------------------------------------------------------------
struct StripGripState {
    float start_h = 0.0f;
    float press_y = 0.0f;
    bool  moved = false;  // a click without a move changes nothing
};

// Call first inside the strip window: an invisible ~4 px button over its top edge. While it
// is dragged, *stored_h follows (min_h .. max_h, floored); on release after a move it is
// saved under pref_key. `h` = the strip's height this frame. The cursor is put back.
void StripHeightGrip(StripGripState& g, const char* id, float* stored_h, float h, float min_h, float max_h,
                     const char* pref_key);

// ---- View window ---------------------------------------------------------------------------
struct StripViewState {
    bool   init = false;
    double v0 = 0.0;
    double span = 1.0;
    bool   zoomed = false;       // Alt + wheel changed the span from the default
    bool   scrolled = false;     // Shift + wheel moved the view
    bool   was_playing = false;  // play start re-frames the clip
    double item_start = 0.0;     // the current item the view last followed
    double item_end = 0.0;
    double last_playhead = 0.0;
    float  last_seek_mx = -1.0e9f;
};

struct StripWindow {
    double v0 = 0.0;
    double span = 1.0;
    double v1 = 1.0;
    double item_span = 0.0;
};

// No current item: the next one is framed afresh.
inline void StripViewReset(StripViewState& s)
{
    s.init = false;
}

// Follows the current item [item_start, item_end) and the playhead (never while `hold`, a
// drag in the lane), then applies Alt / Shift + wheel when `window_hovered`. `mouse_x` and
// the lane [lane_x, lane_x + lane_w] place the zoom's fixed time. Returns the window to draw.
StripWindow StripViewUpdate(StripViewState& s, double item_start, double item_end, double playhead, bool playing,
                            double fps, bool hold, bool window_hovered, float mouse_x, float lane_x, float lane_w);

// Time <-> x over a lane.
inline float StripX(const StripWindow& w, float lane_x, float lane_w, double t)
{
    return lane_x + static_cast<float>((t - w.v0) / w.span) * lane_w;
}
inline double StripTimeAt(const StripWindow& w, float lane_x, float lane_w, float x)
{
    double f = static_cast<double>((x - lane_x) / lane_w);
    f = f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
    return w.v0 + f * w.span;
}

// ---- Ruler ---------------------------------------------------------------------------------
struct StripRuler {
    double step = 0.0;   // labelled ticks (0 = none)
    double minor = 0.0;  // minor ticks
    double snap = 0.0;   // the finest graduation drawn
};

// The ruler's steps for this window over a lane of lane_w pixels (project time, origin 0).
StripRuler StripRulerFor(const StripWindow& w, float lane_w, double fps);

// The ruler band [lane_x, lane_x + lane_w] x [lane_top - ruler_h, lane_top]: minor ticks,
// labelled ticks, labels at font size ruler_h - kStripRulerTickRoom (none under 6 px).
void DrawStripRuler(ImDrawList* dl, const StripWindow& w, const StripRuler& r, float lane_x, float lane_w,
                    float lane_top, float ruler_h, double fps);

// ---- Seek ----------------------------------------------------------------------------------
// While `scrubbing` (the lane held, not grabbing anything else): seeks to t_seek on the press
// and whenever the mouse moved (seek = the strip's way of moving REAPER's playhead).
void StripScrub(StripViewState& s, bool scrubbing, bool pressed, float mouse_x, double t_seek, void (*seek)(double));

}  // namespace rav

#endif  // _WIN32
