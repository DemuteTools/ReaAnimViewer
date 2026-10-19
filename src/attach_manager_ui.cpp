// SPDX-License-Identifier: MIT
//
// See attach_manager_ui.h (13-68e v5).

#include "attach_manager_ui.h"

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

#include "attach_manager_plan.h"
#include "float_ui_window.h"
#include "library_icons.h"  // ToolIcon
#include "sound_project.h"  // GetSoundStore
#include "sound_record.h"
#include "sound_spread_plan.h"
#include "sound_spread_selection.h"
#include "ui_flow.h"
#include "ui_theme.h"

namespace rav {

namespace {

const char kDot[] = " \xC2\xB7 ";
const char kDash[] = "\xE2\x80\x94";

// The mockup's 360 px window, a little taller than its content; its size is kept, it opens at the mouse.
constexpr int kDefaultW = 360, kDefaultH = 400;
constexpr int kMinW = 300, kMinH = 260;
FloatUiWindow g_host("attachmgr.float", L"Attach manager", kDefaultW, kDefaultH, kMinW, kMinH);

double ClockSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- The title row's icon (the mockup's 16 px link) ------------------------------------------------------------

// A chain link: a capsule centred on `c` along the diagonal (up-right), in the icon's 16-unit square.
void Capsule(ImDrawList* dl, ImVec2 c, float u, ImU32 col)
{
    const float r = 2.0f * u, hl = 1.7f * u, k = 0.70710678f;
    const ImVec2 a(c.x - hl * k, c.y + hl * k), b(c.x + hl * k, c.y - hl * k);
    const float  base = -3.14159265f * 0.25f;  // the direction a -> b: up-right
    dl->PathClear();
    dl->PathArcTo(b, r, base - 3.14159265f * 0.5f, base + 3.14159265f * 0.5f, 8);
    dl->PathArcTo(a, r, base + 3.14159265f * 0.5f, base + 3.14159265f * 1.5f, 8);
    dl->PathStroke(col, ImDrawFlags_Closed, std::max(1.0f, 1.4f * u));
}

void DrawLink(ImDrawList* dl, ImVec2 p, float s, ImU32 col)
{
    const float u = s / 16.0f;
    Capsule(dl, ImVec2(p.x + 5.6f * u, p.y + 10.4f * u), u, col);
    Capsule(dl, ImVec2(p.x + 10.4f * u, p.y + 5.6f * u), u, col);
}

// ---- State ----------------------------------------------------------------------------------------------------

struct State {
    bool               open = false;
    bool               focus = false;
    bool               place = false;
    AttachManagerState st;
    bool               dirty = true;
    std::string        sig, clicked;
    double             next_poll = 0.0;
    std::string        pick_key;  // the user's Marker pick
    am::GestureWant    want = am::GestureWant::Auto;
    bool               force = false;    // read now (a gesture ran, the window opened): no throttle
    double             next_read = 0.0;  // a project change is read at most every 0.25 s
    // A gesture clicked, run from the viewer's window procedure.
    std::function<void()> pending;
};
State g;

// The snapshot again when the project, the time selection, the item selection, the store or a clicked label moved.
void Refresh()
{
    const std::string sig = AttachManagerSignature();
    if (sig != g.sig) {
        g.sig = sig;
        g.dirty = true;
    }
    const double now = ClockSeconds();
    if (now >= g.next_poll) {
        g.next_poll = now + 0.4;
        const std::string ck = ClickedMarkersKey();
        if (ck != g.clicked) {
            g.clicked = ck;
            g.dirty = true;
        }
    }
    if (!g.dirty) return;
    if (!g.force && now < g.next_read) return;  // a drag changes the project every frame: read it now and then
    g.next_read = now + 0.25;
    g.force = false;
    g.dirty = false;
    ReadAttachManager(&g.st);
}

// One gesture at a time: a new one while the last still waits for the window procedure is ignored (its buttons are
// greyed meanwhile).
bool Queue(std::function<void()> fn)
{
    if (g.pending) return false;
    g.pending = std::move(fn);
    g.dirty = true;
    return true;
}

// A muted label in a fixed column, the cursor left after it for its control (which takes the room left).
float FieldLabel(const char* label, float col_w)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  fh = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x, std::floor(p.y + (fh - ImGui::GetTextLineHeight()) * 0.5f)), ui::kMuted, label);
    ImGui::Dummy(ImVec2(col_w, fh));
    ImGui::SameLine(0.0f, ui::Dp(8.0f));
    return std::max(ui::Dp(60.0f), ImGui::GetContentRegionAvail().x);
}

// Faint small text wrapped to the width.
void FaintLine(const std::string& text, ImU32 col = ui::kFaint)
{
    ImGui::PushFont(ui::FontSmall());
    ui::ToneText(text, col, true, ImGui::GetContentRegionAvail().x);
    ImGui::PopFont();
}

// ---- What the window shows ------------------------------------------------------------------------------------

struct View {
    std::vector<int>  choices;
    int               marker = -1;
    am::GestureChoice gc;
    std::string       kind;    // the kind's side-free name
    char              side = 0;
    std::string       target;  // "FS L · 1.70 s"
    std::string       anim;    // the marker's animation
    am::Scope         scope = am::Scope::Anim;
    bool              has_group = false;
    am::Group         group;   // the marker's animation's Group (its Attached library)
    bool              any_together = false, any_split = false;  // over the library's Groups with content
    std::string       legacy;  // the library was laid out by an earlier version (LegacyLayout)
    SpreadMarker      lr;      // the marker the L and R card acts from
};

View MakeView()
{
    View                v;
    const am::Snapshot& s = g.st.snap;
    const SoundStore&   store = GetSoundStore(nullptr);
    v.scope = am::ScopeOf(store);
    v.choices = am::MarkerChoices(s);
    v.marker = am::PreselectMarker(s, v.choices, g.pick_key);
    v.gc = am::ChooseGesture(s, v.choices, v.marker, g.want);
    const SoundLibrary* lib = nullptr;
    int                 anim = -1;
    if (v.gc.gesture == am::Gesture::Pile) {
        if (v.gc.stack_event >= 0 && static_cast<size_t>(v.gc.stack_event) < s.events.size()) {
            const am::AttachedEvent& e = s.events[static_cast<size_t>(v.gc.stack_event)];
            lib = FindLibrary(store, e.lib_uid);
            v.kind = e.kind_name;  // the kind's name from its rule (as the gesture names it), not the library's
            v.side = e.side;
            char b[32];
            std::snprintf(b, sizeof(b), "%.2f s", e.pos);
            v.target = e.label + kDot + b;
            anim = e.anim;
            v.lr.name = e.label;
            v.lr.pos = e.pos;
        }
    } else if (v.marker >= 0) {
        const am::Marker& m = s.markers[static_cast<size_t>(v.marker)];
        lib = FindAttachedLibrary(store, m.kind_key);
        v.kind = m.kind_name;
        v.side = m.side;
        v.target = am::MarkerText(m);
        anim = m.anim;
        v.lr.name = m.name;
        v.lr.pos = m.pos;
    }
    if (anim >= 0 && static_cast<size_t>(anim) < s.anims.size()) {
        v.anim = s.anims[static_cast<size_t>(anim)].name;
        v.lr.item = static_cast<size_t>(anim) < g.st.anims.size() ? g.st.anims[static_cast<size_t>(anim)] : nullptr;
        if (lib) {
            const am::Layout lay = am::ReadLayout(store, *lib);
            const int        gi = am::GroupOf(lay, s.anims[static_cast<size_t>(anim)].values);
            if (gi >= 0) {
                v.group = lay.groups[static_cast<size_t>(gi)];
                v.has_group = true;
            }
            for (const am::Group& gr : lay.groups) {
                if (!am::HasContent(gr)) continue;
                v.any_split = v.any_split || gr.split;
                v.any_together = v.any_together || !gr.split;
            }
            am::LegacyLayout(store, *lib, &v.legacy);
        }
    }
    if (v.kind.empty()) v.kind = kDash;
    return v;
}

// ---- The title row ----------------------------------------------------------------------------------------------

void DrawTitle(const std::string& gesture, const std::string& what)
{
    const float  h = ui::Dp(30.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float  close_w = ui::ControlH();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    g_host.SetDragArea(p.x - ui::Dp(8.0f), p.y - ui::Dp(8.0f), p.x + w - close_w - ui::Dp(4.0f), p.y + h,
                       ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
    const float ip = ui::IconPx();
    DrawLink(dl, ImVec2(p.x + ui::Dp(2.0f), std::floor(p.y + (h - ip) * 0.5f)), ip, ui::kMuted);
    const float ty = std::floor(p.y + (h - ImGui::GetTextLineHeight()) * 0.5f);
    float       x = p.x + ui::Dp(2.0f) + ip + ui::Dp(8.0f);
    ui::AddTextIn(dl, ui::FontSemibold(), ImVec2(x, ty), ui::kText, gesture.c_str());
    x += ui::TextSizeIn(ui::FontSemibold(), gesture.c_str()).x;
    if (!what.empty()) ui::EllipsisTextTip(dl, ImVec2(x, ty), p.x + w - close_w - ui::Dp(8.0f), ui::kMuted, std::string(kDot) + what);
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - close_w, p.y + (h - close_w) * 0.5f));
    if (ui::IconOnlyButton("##amclose", ToolIcon(kToolCross), "Close (Esc)")) g.open = false;
    dl->AddLine(ImVec2(p.x - ui::Dp(8.0f), p.y + h), ImVec2(p.x + w + ui::Dp(8.0f), p.y + h), ui::kStroke);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + ui::Dp(8.0f)));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

// ---- The body ---------------------------------------------------------------------------------------------------

void DrawApplyTo(const View& v, float col)
{
    const float              fw = FieldLabel("Apply to", col);
    static const char* const kScope[3] = {"Item", "Anim", "Project"};
    int                      si = v.scope == am::Scope::Item ? 0 : v.scope == am::Scope::Project ? 2 : 1;
    const bool               busy = static_cast<bool>(g.pending);
    if (busy) ImGui::BeginDisabled();
    if (ui::Segmented("##amscope", kScope, 3, &si, fw / 3.0f) && !busy) {
        const am::Scope s = si == 0 ? am::Scope::Item : si == 2 ? am::Scope::Project : am::Scope::Anim;
        if (s != v.scope) Queue([s] { SetApplyTo(s); });
    }
    if (busy) ImGui::EndDisabled();
    const int hov = ui::SegmentedHovered();
    if (hov >= 0) ui::ShowTip(am::ScopeTip(hov == 0 ? am::Scope::Item : hov == 2 ? am::Scope::Project : am::Scope::Anim));
}

void DrawMarker(const View& v, float col)
{
    const am::Snapshot& s = g.st.snap;
    const float         fw = FieldLabel("Marker", col);
    const bool          pile = v.gc.gesture == am::Gesture::Pile;
    const bool          one = pile || v.choices.size() < 2;
    const std::string   preview = pile ? (v.target.empty() ? std::string(kDash) : v.target)
                                       : v.marker >= 0 ? am::MarkerText(s.markers[static_cast<size_t>(v.marker)]) : std::string(kDash);
    if (one) ImGui::BeginDisabled();
    if (ui::BeginComboStyled("##ammarker", preview.c_str(), fw)) {
        for (int i : v.choices) {
            const am::Marker& m = s.markers[static_cast<size_t>(i)];
            ImGui::PushID(i);
            if (ImGui::Selectable(am::MarkerText(m).c_str(), i == v.marker)) g.pick_key = am::MarkerKey(s, m);
            ImGui::PopID();
        }
        ui::EndComboStyled();
    }
    if (one) ImGui::EndDisabled();
    ui::ItemTipText("Every marker kind and side in the time selection, and the clicked marker label");
    FieldLabel("On", col);
    ui::FitText(v.anim.empty() ? std::string(kDash) : v.anim, v.anim.empty() ? ui::kMuted : ui::kText);
    if (!v.gc.from.empty()) FaintLine(v.gc.from);
}

void RunGesture(const View& v)
{
    const am::Scope scope = v.scope;
    if (v.gc.gesture == am::Gesture::Pile) {
        Queue([scope] { AddToPile(scope); });
    } else {
        const am::Marker& m = g.st.snap.markers[static_cast<size_t>(v.marker)];
        SpreadMarker      sm;
        sm.item = m.anim >= 0 && static_cast<size_t>(m.anim) < g.st.anims.size() ? g.st.anims[static_cast<size_t>(m.anim)] : nullptr;
        sm.name = m.name;
        sm.pos = m.pos;
        Queue([scope, sm] { AttachToMarker(scope, &sm); });
    }
    g.want = am::GestureWant::Auto;
}

void DrawSelection(const View& v)
{
    const am::Snapshot& s = g.st.snap;
    const std::string   count = s.selected_items.empty()
                                    ? std::string()
                                    : std::to_string(s.selected_items.size()) + (s.selected_items.size() == 1 ? " item" : " items");
    ui::Section     sec;
    ui::SectionOpts so;
    so.foldable = false;
    so.suffix = count.empty() ? nullptr : count.c_str();
    if (ui::BeginSection(sec, "Selection##amsel", so)) {
        static const char* const kGest[2] = {"Attach", "Add to pile"};
        int                      gi = v.gc.gesture == am::Gesture::Pile ? 1 : 0;
        if (ui::Segmented("##amgest", kGest, 2, &gi, 0.0f, v.gc.pile_ok || gi == 1 ? -1 : 1))
            g.want = gi == 1 ? am::GestureWant::Pile : am::GestureWant::Attach;
        const int hov = ui::SegmentedHovered();
        if (hov == 0) ui::ShowTip("Put the selected sound on the marker and its linked markers");
        else if (hov == 1)
            ui::ShowTip(v.gc.pile_ok ? "Add the selected items as a layer to every linked instance"
                                     : "Add to pile needs an attached instance selected with the new items");
        if (v.gc.reason.empty() && (v.gc.gesture == am::Gesture::Pile || v.marker >= 0)) {
            std::string bold, rest;
            am::GestureInfo(v.gc.gesture, v.scope, s, v.kind, v.side, v.has_group && v.group.split, v.target, &bold, &rest);
            ImGui::PushFont(ui::FontSmall());
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
            ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::kMuted));
            ImGui::TextWrapped("%s%s", bold.c_str(), rest.c_str());
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
            ImGui::PopFont();
        }
        if (!v.gc.reason.empty()) ui::ToneText(v.gc.reason, ui::kWarn, true, ImGui::GetContentRegionAvail().x);
        const std::string label = am::RunLabel(v.gc.gesture, v.scope) + "##amrun";
        const bool        can = v.gc.reason.empty() && !g.pending;
        if (!can) ImGui::BeginDisabled();
        if (ui::PrimaryButton(label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)) && can) RunGesture(v);
        if (!can) ImGui::EndDisabled();
        // Reshuffle (Antho 2026-10-20): the selected copies RAV placed drawn again, their own row (nothing moves).
        int                    sounds = 0;
        const std::vector<int> evs = am::ReshuffleTargets(s, &sounds);
        const bool             rs = !evs.empty() && !g.pending;
        if (!rs) ImGui::BeginDisabled();
        if (ui::SolidButton("Reshuffle##amreshuffle", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)) && rs) {
            std::vector<ReshuffleTarget> targets;
            for (int i : evs) {
                const am::AttachedEvent& e = s.events[static_cast<size_t>(i)];
                if (e.anim >= 0 && static_cast<size_t>(e.anim) < s.anims.size()) targets.push_back({s.anims[static_cast<size_t>(e.anim)].guid, e.rid, e.t});
            }
            Queue([targets, sounds] { ReshuffleSelected(targets, sounds); });
        }
        if (!rs) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ui::ShowTip(evs.empty() ? "Select copies RAV placed (not the original) to draw their variations again"
                                    : ("Draw the variation of the " + std::to_string(sounds) + (sounds == 1 ? " selected copy" : " selected copies") +
                                       " again, the other sounds left as they are")
                                          .c_str());
    }
    ui::EndSection(sec);
}

// A small rounded pill at the right of the line (not clickable).
float Pill(ImDrawList* dl, float right, float y, float h, const std::string& text)
{
    ImFont*      f = ui::FontSmall();
    const ImVec2 ts = ui::TextSizeIn(f, text.c_str());
    const float  w = ts.x + ui::Dp(16.0f);
    const ImVec2 a(right - w, y + (h - ts.y - ui::Dp(4.0f)) * 0.5f), b(right, y + (h + ts.y + ui::Dp(4.0f)) * 0.5f);
    dl->AddRect(a, b, ui::kStrokeStrong, (b.y - a.y) * 0.5f);
    ui::AddTextIn(dl, f, ImVec2(std::floor(a.x + ui::Dp(8.0f)), std::floor(a.y + ui::Dp(2.0f))), ui::kText, text.c_str());
    return w;
}

void DrawLr(const View& v)
{
    am::LrInput in;
    in.has_marker = v.marker >= 0 || (v.gc.gesture == am::Gesture::Pile && v.gc.stack_events == 1);
    in.marker_read = v.lr.item != nullptr;
    in.kind = v.kind;
    in.side = v.side;
    in.group = v.has_group ? &v.group : nullptr;
    in.any_together = v.any_together;
    in.any_split = v.any_split;
    in.legacy = v.legacy;
    const am::LrCard c = am::LrCardOf(in, v.scope);
    ui::Section     sec;
    ui::SectionOpts so;
    so.foldable = false;
    if (ui::BeginSection(sec, "L and R##amlr", so)) {
        // The state line: its text ellipsized, the pill at its right edge.
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  w = std::max(ui::Dp(60.0f), ImGui::GetContentRegionAvail().x);
        const float  h = ImGui::GetTextLineHeight() + ui::Dp(6.0f);
        ImDrawList*  dl = ImGui::GetWindowDrawList();
        const float  pw = Pill(dl, p.x + w, p.y, h, c.pill);
        ImGui::PushFont(ui::FontSmall());
        ui::EllipsisTextTip(dl, ImVec2(p.x, std::floor(p.y + (h - ImGui::GetTextLineHeight()) * 0.5f)), p.x + w - pw - ui::Dp(8.0f), ui::kMuted, c.text);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(w, h));
        const bool can = c.enabled && !g.pending;
        if (!can) ImGui::BeginDisabled();
        const std::string label = c.button + "##amlrbtn";
        if (ui::SolidButton(label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)) && can) {
            const am::Scope    scope = v.scope;
            const SpreadMarker m = v.lr;
            if (c.merge) Queue([scope, m] { MergeLR(scope, m); });
            else Queue([scope, m] { SeparateLR(scope, m); });
        }
        if (!can) ImGui::EndDisabled();
        if (!c.tip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ui::ShowTip(c.tip.c_str());
    }
    ui::EndSection(sec);
}

void DrawContent()
{
    // Esc closes the window, unless a dropdown was open to take it.
    const bool popup_before = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    Refresh();
    const View v = MakeView();
    DrawTitle(v.gc.gesture == am::Gesture::Pile ? "Add to pile" : "Attach", v.kind);
    const float h = std::max(ui::Dp(60.0f), ImGui::GetContentRegionAvail().y);
    if (ImGui::BeginChild("##ambody", ImVec2(0.0f, h), ImGuiChildFlags_None)) {
        const float col = std::max({ImGui::CalcTextSize("Apply to").x, ImGui::CalcTextSize("Marker").x, ImGui::CalcTextSize("On").x});
        DrawApplyTo(v, col);
        ImGui::Dummy(ImVec2(0.0f, ui::Dp(2.0f)));
        DrawMarker(v, col);
        ImGui::Dummy(ImVec2(0.0f, ui::Dp(4.0f)));
        DrawSelection(v);
        DrawLr(v);
    }
    ImGui::EndChild();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !popup_before) g.open = false;
}

}  // namespace

void OpenAttachManager()
{
    g.open = true;
    g.focus = true;
    g.place = true;
    g.dirty = true;
    g.force = true;
    g.want = am::GestureWant::Auto;
}

bool AttachManagerIsOpen()
{
    return g.open;
}

void DrawAttachManagerFloat(HWND viewer, HDC viewer_dc, HGLRC rc)
{
    if (!ImGui::GetCurrentContext()) return;
    if (!g.open && !g_host.IsShown()) return;
    g_host.Bind(viewer, viewer_dc, rc);
    // The pointer at the middle of the title row: the window's 8 px top padding + half the 30 px row (DrawTitle).
    if (g.place && g.open) g_host.PlaceAtMouse(static_cast<int>(std::lround(ui::Dp(8.0f + 15.0f))));
    g.place = false;
    const bool activate = g.open && g.focus;
    g.focus = false;
    g_host.Frame(g.open, activate, [] { DrawContent(); });
    if (g_host.TakeCloseRequest()) g.open = false;
}

void DestroyAttachManagerFloat()
{
    g_host.Destroy();
    g.open = false;  // a viewer made again later does not bring it back unasked
    g.pending = nullptr;
}

bool AttachManagerFloatOwns(HWND hwnd)
{
    const HWND h = g_host.Hwnd();
    return h && hwnd && (hwnd == h || IsChild(h, hwnd));
}

HWND AttachManagerFloatHwnd()
{
    return g_host.Hwnd();
}

bool AttachManagerFloatWantsText()
{
    return g_host.WantsText();
}

bool AttachManagerHasPending()
{
    return static_cast<bool>(g.pending);
}

void AttachManagerRunPending()
{
    std::function<void()> fn = std::move(g.pending);
    g.pending = nullptr;
    g.dirty = true;
    g.force = true;  // its result shows at once
    if (fn) fn();
}

void AttachManagerDropPending()
{
    g.pending = nullptr;
}

}  // namespace rav

#endif  // _WIN32
