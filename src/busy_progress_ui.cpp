// SPDX-License-Identifier: MIT
//
// See busy_progress_ui.h.

#include "busy_progress_ui.h"
#include "watchdog.h"  // freeze audit: the heartbeat

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

#include <imgui.h>

#include "ui_theme.h"

namespace rav {
namespace {

BusyPaintFn g_paint = nullptr;
BusyEndedFn g_ended = nullptr;

int        g_depth = 0;       // the scopes open
int        g_item_level = 0;  // the level of the scope that counts the items (0 = none)
BusyFrame  g_frame;      // the outermost scope's card
BusyGate   g_gate;
BusyMemory g_memory;
double     g_start = 0.0;
bool       g_painted = false;  // a gesture showed its card since the last BusyTakePainted
bool       g_shown = false;    // 13-59 review: the running gesture's card was really painted (once at least)
bool       g_painting = false;  // re-entry guard: a paint never paints again

double NowSeconds()
{
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point t0 = Clock::now();
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

// Paints a frame of the card when one is due (and a painter is set). `force`: at once, the card being
// shown already (a new phase is not left behind its first frame's "Working...").
void MaybePaint(bool force = false)
{
    if (g_depth <= 0 || !g_paint || g_painting) return;
    if (!(force && g_shown) && !g_gate.Due(NowSeconds())) return;
    g_painting = true;
    try {
        if (g_paint(g_frame)) g_shown = true;
    } catch (...) {
    }
    g_painting = false;
}

}  // namespace

void SetBusyPainter(BusyPaintFn paint, BusyEndedFn ended)
{
    g_paint = paint;
    g_ended = ended;
}

BusyScope::BusyScope(const char* title, bool eager)
{
    level_ = ++g_depth;
    if (level_ > 1) return;  // nested: the outer gesture's card
    g_item_level = 0;
    g_frame = BusyFrame();
    g_frame.title = title ? title : "";
    g_shown = false;
    g_start = NowSeconds();
    // An eager gesture that took long last time shows its card at once (its one long step says nothing).
    g_gate.Start(g_start, eager && g_memory.WasLong(g_frame.title));
    MaybePaint();
}

BusyScope::~BusyScope()
{
    if (g_depth > 0) --g_depth;
    if (g_item_level >= level_) g_item_level = 0;  // its items are no longer counted
    if (level_ != 1) return;
    g_memory.Record(g_frame.title, NowSeconds() - g_start);
    if (g_shown) {  // 13-59 review: only a card really painted (the gate may have been due on a hidden window)
        g_shown = false;
        g_painted = true;
        if (g_ended) {
            try {
                g_ended();
            } catch (...) {
            }
        }
    }
    g_depth = 0;
}

void BusyScope::Item(int i, int n, const char* unit)
{
    if (g_item_level != 0 && g_item_level < level_) return;  // an outer scope counts the items
    g_item_level = level_;
    g_frame.item = i;
    g_frame.items = n;
    g_frame.unit = unit ? unit : "Item";
    g_frame.step = 0;
    g_frame.steps = 0;
    MaybePaint();
}

void BusyScope::Phase(const char* phase)
{
    if (level_ != 1) return;
    g_frame.phase = phase ? phase : "";
    MaybePaint(true);  // 13-59 review: a card already shown (an eager one) shows the phase now
}

void BusyStep(int done, int total, const char* unit)
{
    if (g_depth <= 0) return;
    RAV_MAIN_BEAT();  // freeze audit: a long gesture that moves on (its progress) is not a freeze
    g_frame.step = done;
    g_frame.steps = total;
    g_frame.step_unit = unit ? unit : "event";
    MaybePaint();
}

bool BusyActive() { return g_depth > 0; }

bool BusyTakePainted()
{
    const bool was = g_painted;
    g_painted = false;
    return was;
}

void DrawBusyBar(float fraction, float height_dp)
{
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float  h = ui::Dp(height_dp);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const float  r = h * 0.5f;
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), ui::kBg, r);
    dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), ui::kStroke, r);
    float a = 0.0f, b = 0.0f;
    if (fraction < 0.0f) {  // indeterminate: a third of the bar sweeping across, once a second and a half
        const float t = static_cast<float>(std::fmod(ImGui::GetTime() / 1.5, 1.0));
        a = std::clamp((t * 1.33f - 0.33f), 0.0f, 1.0f);
        b = std::clamp(t * 1.33f, 0.0f, 1.0f);
    } else {
        b = std::clamp(fraction, 0.0f, 1.0f);
    }
    if (b - a > 0.0f) dl->AddRectFilled(ImVec2(p0.x + w * a, p0.y), ImVec2(p0.x + w * b, p0.y + h), ui::kAccent, r);
}

}  // namespace rav

#endif  // _WIN32
